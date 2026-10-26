// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypeSupport.h"
#include "Terrain3DConstants.h"
#include "data/editing/terrain_3d_map_edit_service.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"

#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampler.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "eastl_config.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "simd/core/terrain_3d_parallel.h"
#include "simd/core/terrain_3d_simd.h"

#include <EASTL/numeric_limits.h>
#include <EASTL/array.h>
#include <EASTL/bitset.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/os.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

typedef struct t3dDerivedMapBuildContext_s
{
	t3dSamplingSource_t											   m_samplingSource;
	eastl::vector< t3dDerivedMapTask_t, Terrain3DEastlAllocator > *m_pTasks = NULL;
	float														  *m_pWorkerHaloScratch = NULL;
	uint8_t														  *m_pOutputScratch = NULL;
	size_t														   m_nWorkerHaloElements = 0u;
	float														   m_fVertexSpacing = 1.0f;
} t3dDerivedMapBuildContext_t;

static bool t3dMapEditServicePrepareDerivedHeightHalo(
	Terrain3DSampler		  &rSampler,
	const t3dDerivedMapTask_t &rTask,
	float *const			   pHalo,
	const size_t			   nHaloCapacity,
	int						  &rHaloStride,
	size_t					  &rHaloElements ) {
	const int width = rTask.m_dirtyRect.size.x;
	const int height = rTask.m_dirtyRect.size.y;
	if ( !rTask.m_bAvailable || width <= 0 || height <= 0 || pHalo == NULL ) {
		return false;
	}
	rHaloStride = width + 2;
	rHaloElements = ( size_t ) rHaloStride * ( size_t ) ( height + 2 );
	if ( rHaloElements > nHaloCapacity ) {
		return false;
	}
	const int64_t regionBaseX = ( int64_t ) ( rTask.m_location.x ) * T3D_REGION_SIZE;
	const int64_t regionBaseZ = ( int64_t ) ( rTask.m_location.y ) * T3D_REGION_SIZE;
	const int64_t haloGridX = regionBaseX + rTask.m_dirtyRect.position.x - 1;
	const int64_t haloGridZ = regionBaseZ + rTask.m_dirtyRect.position.y - 1;
	rSampler.copyHeightRect( haloGridX, haloGridZ, width + 2, height + 2, pHalo, rHaloStride, false );
	for ( int y = -1; y <= height; ++y ) {
		for ( int x = -1; x <= width; ++x ) {
			float &sample = pHalo[( size_t ) ( y + 1 ) * ( size_t ) rHaloStride + ( size_t ) ( x + 1 )];
			if ( isfinite( sample ) ) {
				continue;
			}
			const int localX = rTask.m_dirtyRect.position.x + x;
			const int localY = rTask.m_dirtyRect.position.y + y;
			const int clampedX = CLAMP( localX, 0, T3D_REGION_SIZE - 1 );
			const int clampedY = CLAMP( localY, 0, T3D_REGION_SIZE - 1 );
			sample = rSampler.sampleHeightGrid( regionBaseX + clampedX, regionBaseZ + clampedY, false );
			sample = isfinite( sample ) ? sample : 0.0f;
		}
	}
	return true;
}

static void t3dMapEditServiceBuildDerivedMapRange(
	void		 *pContext,
	const size_t  nBegin,
	const size_t  nEnd,
	const int32_t nWorkerIndex ) {
	t3dDerivedMapBuildContext_t *const pBuildContext = ( t3dDerivedMapBuildContext_t * ) ( pContext );
	Terrain3DSampler				   sampler( pBuildContext->m_samplingSource );
	for ( size_t taskIndex = nBegin; taskIndex < nEnd; ++taskIndex ) {
		t3dDerivedMapTask_t &task = ( *pBuildContext->m_pTasks )[taskIndex];
		if ( task.m_bSuccess ) {
			continue;
		}
		const int width = task.m_dirtyRect.size.x;
		const int height = task.m_dirtyRect.size.y;
		if ( !task.m_bAvailable || width <= 0 || height <= 0 ) {
			continue;
		}

		if ( pBuildContext->m_pWorkerHaloScratch == NULL || pBuildContext->m_pOutputScratch == NULL ) {
			continue;
		}
		float *const pHalo = pBuildContext->m_pWorkerHaloScratch +
							 ( size_t ) MAX( 0, nWorkerIndex ) * pBuildContext->m_nWorkerHaloElements;
		int	   haloStride = 0;
		size_t halo_elements = 0u;
		if ( !t3dMapEditServicePrepareDerivedHeightHalo( sampler,
														 task,
														 pHalo,
														 pBuildContext->m_nWorkerHaloElements,
														 haloStride,
														 halo_elements ) ) {
			continue;
		}
		( void ) halo_elements;

		if ( task.m_nOutputBytes != ( size_t ) width * ( size_t ) height * 2u ) {
			continue;
		}
		t3dSimdDerivedNormalsRg8( pHalo,
								  width,
								  height,
								  haloStride,
								  pBuildContext->m_fVertexSpacing,
								  pBuildContext->m_pOutputScratch + task.m_iOutputOffset );
		task.m_bSuccess = true;
	}
}

Terrain3DMapEditService::Terrain3DMapEditService(
	Terrain3DRegionRepository *const pRepository,
	Terrain3DSamplingStorage *const	 pSamplingStorage ) :
	m_pRepository( pRepository ),
	m_pSamplingStorage( pSamplingStorage ) {
}

const t3dMaterialTileRegionClasses_t *Terrain3DMapEditService::getControlTileClassesCold(
    const Vector2i &rRegionLocation ) const {
    const int index = Terrain3DRegionRepository::getRegionMapIndex( rRegionLocation );
    return index >= 0 ? &m_controlTileClasses[index] : NULL;
}

void Terrain3DMapEditService::invalidateControlTileClassesCold( const Vector2i &rLocation ) {
    const int index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
    if ( index >= 0 ) t3d_material_tiles_invalidate( m_controlTileClasses[index] );
}

void Terrain3DMapEditService::refreshControlTileClassesCold(
    const Vector2i &rLocation, const Terrain3DRegion *pRegion ) {
    rebuildControlTileClassesCold( rLocation, pRegion );
}

void Terrain3DMapEditService::rebuildControlTileClassesCold(
    const Vector2i &rLocation, const Terrain3DRegion *const pRegion ) {
    const int index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
    if ( index < 0 ) return;
    t3dMaterialTileRegionClasses_t &result = m_controlTileClasses[index];
    t3d_material_tiles_invalidate( result );
    if ( !pRegion || pRegion->isDeleted() ) return;
    Image *const pMap = pRegion->getMapPtr( TYPE_CONTROL );
    if ( !pMap || pMap->get_format() != Image::FORMAT_RF ||
         pMap->get_width() != T3D_REGION_SIZE || pMap->get_height() != T3D_REGION_SIZE ) return;
    // RF image data contains packed control words, not numerically converted floats.
    // Copy only during an edit/cold build; no new per-fragment or per-frame work.
    const PackedByteArray bytes = pMap->get_data();
    if ( bytes.size() != T3D_REGION_SIZE * T3D_REGION_SIZE * ( int ) sizeof( uint32_t ) ) return;
    uint32_t controls[T3D_REGION_SIZE * T3D_REGION_SIZE];
    memcpy( controls, bytes.ptr(), sizeof( controls ) );
    t3d_material_tiles_rebuild( result, controls, T3D_REGION_SIZE, T3D_REGION_SIZE );
}

void Terrain3DMapEditService::reset(
	void ) {
	m_editedArea = AABB();
	m_masterHeightRange = s_v2Zero;
    for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) t3d_material_tiles_invalidate( m_controlTileClasses[i] );
	m_derivedTaskScratch.clear();
	m_fDerivedHaloScratch.clear();
	m_fDerivedWorkerHaloScratch.clear();
	m_derivedComputeScratch.clear();
	m_derivedOutputScratch.clear();
	m_activeLocationScratch.clear();
	m_derivedLocationScratch.clear();
	m_derivedUpdateScratch.clear();
}

void Terrain3DMapEditService::releaseRuntimeAllocatorStorage(
	void ) {
	reset();
	t3dVectorRelease( m_derivedTaskScratch );
	t3dVectorRelease( m_fDerivedHaloScratch );
	t3dVectorRelease( m_fDerivedWorkerHaloScratch );
	t3dVectorRelease( m_derivedComputeScratch );
	t3dVectorRelease( m_derivedOutputScratch );
	t3dVectorRelease( m_activeLocationScratch );
	t3dVectorRelease( m_derivedLocationScratch );
	t3dVectorRelease( m_derivedUpdateScratch );
}

Rect2i Terrain3DMapEditService::getRegionDirtyRect(
	const Vector2i &rLocation,
	const AABB	   &rEditedArea,
	const int		nMargin ) {
	const Rect2i bounds( s_v2iZero, V2I( T3D_REGION_SIZE ) );
	if ( rEditedArea.size.x <= 0.0f || rEditedArea.size.z <= 0.0f ) {
		return bounds;
	}

	const Vector3 worldEnd = rEditedArea.get_end();
	const int64_t globalBeginX =
		( int64_t ) ( Math::floor( ( double ) ( rEditedArea.position.x / s_fRegionVertexSpacing ) ) ) - nMargin;
	const int64_t globalBeginZ =
		( int64_t ) ( Math::floor( ( double ) ( rEditedArea.position.z / s_fRegionVertexSpacing ) ) ) - nMargin;
	const int64_t globalEndX =
		( int64_t ) ( Math::ceil( ( double ) ( worldEnd.x / s_fRegionVertexSpacing ) ) ) + nMargin + 1;
	const int64_t globalEndZ =
		( int64_t ) ( Math::ceil( ( double ) ( worldEnd.z / s_fRegionVertexSpacing ) ) ) + nMargin + 1;
	const int64_t regionOriginX = ( int64_t ) ( rLocation.x ) * T3D_REGION_SIZE;
	const int64_t regionOriginZ = ( int64_t ) ( rLocation.y ) * T3D_REGION_SIZE;
	const int	  beginX = ( int ) ( CLAMP< int64_t >( globalBeginX - regionOriginX, 0, T3D_REGION_SIZE ) );
	const int	  beginZ = ( int ) ( CLAMP< int64_t >( globalBeginZ - regionOriginZ, 0, T3D_REGION_SIZE ) );
	const int	  endX = ( int ) ( CLAMP< int64_t >( globalEndX - regionOriginX, 0, T3D_REGION_SIZE ) );
	const int	  endZ = ( int ) ( CLAMP< int64_t >( globalEndZ - regionOriginZ, 0, T3D_REGION_SIZE ) );
	const Rect2i  rect( Vector2i( beginX, beginZ ), Vector2i( endX - beginX, endZ - beginZ ) );
	return rect.size.x > 0 && rect.size.y > 0 ? rect : bounds;
}

Error Terrain3DMapEditService::setPixel(
	const t3dMapType_t eMapType,
	const Vector3	  &rGlobalPosition,
	const Color		  &rPixel,
	Vector2i		  &rRegionLocation,
	Rect2i			  &rDirtyRect ) {
	rRegionLocation = s_v2iMax;
	rDirtyRect = Rect2i();
	if ( !m_pRepository || eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ) {
		return ERR_INVALID_PARAMETER;
	}

	rRegionLocation = m_pRepository->getRegionLocation( rGlobalPosition );
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rRegionLocation );
	if ( !pRegion || pRegion->isDeleted() || m_pRepository->getRegionId( rRegionLocation ) < 0 ) {
		return ERR_DOES_NOT_EXIST;
	}

	const Vector2i globalOffset = rRegionLocation * T3D_REGION_SIZE;
	const Vector3  descaledPosition = rGlobalPosition / s_fRegionVertexSpacing;
	Vector2i	   imagePosition = Vector2i( descaledPosition.x - globalOffset.x, descaledPosition.z - globalOffset.y );
	imagePosition = imagePosition.clamp( s_v2iZero, V2I( T3D_REGION_SIZE - 1 ) );

	Image *const pMap = pRegion->getMapPtrForWrite( eMapType );
	if ( !pMap ) {
		return ERR_UNAVAILABLE;
	}
	if ( eMapType == TYPE_HEIGHT ) {
		t3dRegionCodecWriteHeightPixel( pMap, imagePosition, rPixel.r );
	} else {
		pMap->set_pixelv( imagePosition, rPixel );
	}
	rDirtyRect = Rect2i( imagePosition, Vector2i( 1, 1 ) );
    if ( eMapType == TYPE_CONTROL ) {
        const int index = Terrain3DRegionRepository::getRegionMapIndex( rRegionLocation );
        if ( index >= 0 ) t3d_material_tiles_invalidate( m_controlTileClasses[index] );
    }
	return OK;
}

const eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > &Terrain3DMapEditService::regenerateDerivedMaps(
	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
	const AABB												 &rEditedArea,
	Terrain3DCompute *const									  pCompute ) {
	Terrain3DColdPhaseScope cold_phase_scope;
	m_derivedUpdateScratch.clear();
	if ( !m_pRepository || !m_pSamplingStorage || rRegionLocations.empty() || T3D_REGION_SIZE <= 0 ||
		 s_fRegionVertexSpacing <= 0.0f ) {
		return m_derivedUpdateScratch;
	}

	m_derivedTaskScratch.clear();
	m_derivedTaskScratch.reserve( rRegionLocations.size() );
	eastl::vector< t3dDerivedMapTask_t, Terrain3DEastlAllocator > &tasks = m_derivedTaskScratch;
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( rRegionLocations ).size() ); iRegion++ ) {
		const Vector2i	   &location = ( rRegionLocations )[iRegion];
		t3dDerivedMapTask_t task;
		task.m_location = location;
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			tasks.push_back( eastl::move( task ) );
			continue;
		}

		task.m_bAvailable = true;
		task.m_dirtyRect = Rect2i( s_v2iZero, V2I( T3D_REGION_SIZE ) );
		if ( pRegion->getDerivedNormalSlopeMap().is_valid() && rEditedArea.size.x > 0.0f &&
			 rEditedArea.size.z > 0.0f ) {
			const Vector2 regionWorldOrigin =
				Vector2( location ) * ( real_t ) ( T3D_REGION_SIZE ) *s_fRegionVertexSpacing;
			const Vector2  editBegin( rEditedArea.position.x, rEditedArea.position.z );
			const Vector3  editEnd3d = rEditedArea.get_end();
			const Vector2  editEnd( editEnd3d.x, editEnd3d.z );
			const Vector2  localBegin = ( editBegin - regionWorldOrigin ) / s_fRegionVertexSpacing;
			const Vector2  localEnd = ( editEnd - regionWorldOrigin ) / s_fRegionVertexSpacing;
			const Vector2i pixelBegin = Vector2i( Math::floor( localBegin.x ), Math::floor( localBegin.y ) ) - V2I( 1 );
			const Vector2i pixelEnd = Vector2i( Math::ceil( localEnd.x ), Math::ceil( localEnd.y ) ) + V2I( 1 );
			task.m_dirtyRect =
				Rect2i( pixelBegin, pixelEnd - pixelBegin ).intersection( Rect2i( s_v2iZero, V2I( T3D_REGION_SIZE ) ) );
		}
		tasks.push_back( eastl::move( task ) );
	}

	size_t eligibleTaskCount = 0u;
	for ( size_t iTask = 0u; iTask < ( size_t ) ( ( tasks ).size() ); iTask++ ) {
		const t3dDerivedMapTask_t &task = ( tasks )[iTask];
		if ( task.m_bAvailable && task.m_dirtyRect.size.x > 0 && task.m_dirtyRect.size.y > 0 ) {
			++eligibleTaskCount;
		}
	}

	const uint32_t hardwareThreads = MAX( 1u, ( uint32_t ) MAX( 1, OS::get_singleton()->get_processor_count() ) );
	const int32_t  workerCount =
		( int32_t ) MIN< size_t >( ( size_t ) hardwareThreads, MAX< size_t >( 1u, tasks.size() ) );
	size_t totalOutputBytes = 0u;
	size_t maxHaloElements = 0u;
	size_t maxComputeOutputBytes = 0u;
	for ( size_t iTask = 0u; iTask < ( size_t ) ( ( tasks ).size() ); iTask++ ) {
		t3dDerivedMapTask_t &task = ( tasks )[iTask];
		if ( !task.m_bAvailable || task.m_dirtyRect.size.x <= 0 || task.m_dirtyRect.size.y <= 0 ) {
			continue;
		}
		task.m_iOutputOffset = totalOutputBytes;
		task.m_nOutputBytes = ( size_t ) task.m_dirtyRect.size.x * ( size_t ) task.m_dirtyRect.size.y * 2u;
		totalOutputBytes += task.m_nOutputBytes;
		maxHaloElements =
			MAX( maxHaloElements,
				 ( size_t ) ( task.m_dirtyRect.size.x + 2 ) * ( size_t ) ( task.m_dirtyRect.size.y + 2 ) );
		maxComputeOutputBytes =
			MAX( maxComputeOutputBytes,
				 ( size_t ) task.m_dirtyRect.size.x * ( size_t ) task.m_dirtyRect.size.y * sizeof( uint32_t ) );
	}
	m_derivedOutputScratch.resize( totalOutputBytes );
	m_fDerivedWorkerHaloScratch.resize( maxHaloElements * ( size_t ) workerCount );

	const t3dSamplingSource_t samplingSource{ m_pRepository, m_pSamplingStorage };
	if ( pCompute && eligibleTaskCount > 0u && maxHaloElements > 0u && maxComputeOutputBytes > 0u &&
		 maxHaloElements <= UINT32_MAX && maxComputeOutputBytes <= UINT32_MAX ) {
		m_fDerivedHaloScratch.resize( maxHaloElements * T3D_COMPUTE_BATCH_SIZE );
		m_derivedComputeScratch.resize( maxComputeOutputBytes * T3D_COMPUTE_BATCH_SIZE );
		Terrain3DSampler sampler( samplingSource );
		size_t			 taskCursor = 0u;
		while ( taskCursor < tasks.size() ) {
			eastl::fixed_vector< t3dDerivedMapComputeJob_t, T3D_COMPUTE_BATCH_SIZE, false > jobs;
			eastl::fixed_vector< size_t, T3D_COMPUTE_BATCH_SIZE, false >					task_indices;
			while ( taskCursor < tasks.size() && jobs.size() < T3D_COMPUTE_BATCH_SIZE ) {
				t3dDerivedMapTask_t &task = tasks[taskCursor];
				const size_t		 currentTask = taskCursor++;
				const int			 width = task.m_dirtyRect.size.x;
				const int			 height = task.m_dirtyRect.size.y;
				if ( !task.m_bAvailable || width <= 0 || height <= 0 ) {
					continue;
				}
				const size_t job_index = jobs.size();
				const size_t rgbaBytes = ( size_t ) width * ( size_t ) height * sizeof( uint32_t );
				float *const pHalo = m_fDerivedHaloScratch.data() + job_index * maxHaloElements;
				int			 haloStride = 0;
				size_t		 haloElements = 0u;
				if ( !t3dMapEditServicePrepareDerivedHeightHalo( sampler,
																 task,
																 pHalo,
																 maxHaloElements,
																 haloStride,
																 haloElements ) ) {
					continue;
				}
				jobs.push_back( t3dDerivedMapComputeJob_t{} );
				t3dDerivedMapComputeJob_t &job = jobs.back();
				job.m_pHeightHalo = pHalo;
				job.m_pRgbaOutput = m_derivedComputeScratch.data() + job_index * maxComputeOutputBytes;
				job.m_nHaloElements = ( uint32_t ) haloElements;
				job.m_nOutputBytes = ( uint32_t ) rgbaBytes;
				job.m_nWidth = ( uint32_t ) width;
				job.m_nHeight = ( uint32_t ) height;
				job.m_fVertexSpacing = ( float ) ( s_fRegionVertexSpacing );
				task_indices.push_back( currentTask );
			}
			if ( jobs.empty() ) {
				continue;
			}
			pCompute->generateDerivedMaps( jobs.data(), ( uint32_t ) ( jobs.size() ) );
			for ( size_t slot = 0u; slot < jobs.size(); ++slot ) {
				if ( !jobs[slot].m_bSuccess ) {
					continue;
				}
				t3dDerivedMapTask_t &task = tasks[task_indices[slot]];
				const size_t texelCount = ( size_t ) task.m_dirtyRect.size.x * ( size_t ) task.m_dirtyRect.size.y;
				const uint8_t *const pRgba = jobs[slot].m_pRgbaOutput;
				uint8_t *const		 pOutput = m_derivedOutputScratch.data() + task.m_iOutputOffset;
				for ( size_t texel = 0u; texel < texelCount; ++texel ) {
					pOutput[texel * 2u] = pRgba[texel * 4u];
					pOutput[texel * 2u + 1u] = pRgba[texel * 4u + 2u];
				}
				task.m_bSuccess = true;
			}
		}
	}

	const Terrain3DParallelExecutor executor( workerCount );
	t3dDerivedMapBuildContext_t		buildContext;
	buildContext.m_samplingSource = samplingSource;
	buildContext.m_pTasks = &tasks;
	buildContext.m_pWorkerHaloScratch = m_fDerivedWorkerHaloScratch.data();
	buildContext.m_pOutputScratch = m_derivedOutputScratch.data();
	buildContext.m_nWorkerHaloElements = maxHaloElements;
	buildContext.m_fVertexSpacing = ( float ) ( s_fRegionVertexSpacing );
	executor.runStatic( tasks.size(), 1u, t3dMapEditServiceBuildDerivedMapRange, &buildContext );

	m_derivedUpdateScratch.reserve( tasks.size() );
	for ( size_t iTask = 0u; iTask < ( size_t ) ( ( tasks ).size() ); iTask++ ) {
		t3dDerivedMapTask_t &task = ( tasks )[iTask];
		if ( !task.m_bAvailable || task.m_dirtyRect.size.x <= 0 || task.m_dirtyRect.size.y <= 0 ) {
			continue;
		}
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( task.m_location );
		t3dDerivedMapUpdate_t  update;
		update.m_location = task.m_location;
		update.m_dirtyRect = task.m_dirtyRect;
		if ( !pRegion || pRegion->isDeleted() || !task.m_bSuccess ) {
			if ( pRegion ) {
				pRegion->invalidateDerivedNormalSlopeMap();
			}
			m_derivedUpdateScratch.push_back( update );
			continue;
		}

		PackedByteArray output;
		output.resize( ( int64_t ) task.m_nOutputBytes );
		if ( task.m_nOutputBytes > 0u ) {
			memcpy( output.ptrw(), m_derivedOutputScratch.data() + task.m_iOutputOffset, task.m_nOutputBytes );
		}
		const int		   width = task.m_dirtyRect.size.x;
		const int		   height = task.m_dirtyRect.size.y;
		const Ref< Image > pTile = Image::create_from_data( width, height, false, Image::FORMAT_RG8, output );
		if ( pTile.is_null() ) {
			pRegion->invalidateDerivedNormalSlopeMap();
			m_derivedUpdateScratch.push_back( update );
			continue;
		}

		if ( task.m_dirtyRect.position == s_v2iZero && task.m_dirtyRect.size == V2I( T3D_REGION_SIZE ) ) {
			pRegion->setDerivedNormalSlopeMapInternal( pTile );
		} else {
			Ref< Image > pDerived = pRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() || pDerived->get_width() != T3D_REGION_SIZE ||
				 pDerived->get_height() != T3D_REGION_SIZE ) {
				pRegion->invalidateDerivedNormalSlopeMap();
				m_derivedUpdateScratch.push_back( update );
				continue;
			}
			pDerived->blit_rect( pTile, Rect2i( s_v2iZero, task.m_dirtyRect.size ), task.m_dirtyRect.position );
			pRegion->setDerivedNormalSlopeMapInternal( pDerived );
		}

		if ( m_pSamplingStorage->isActiveOwner() ) {
			m_pSamplingStorage->syncActiveDerived( task.m_location, pRegion, task.m_dirtyRect );
		}
		m_pSamplingStorage->syncResidentDerived( task.m_location, pRegion, task.m_dirtyRect );
		update.m_bSuccess = true;
		m_derivedUpdateScratch.push_back( update );
	}
	return m_derivedUpdateScratch;
}

t3dMapMutation_t Terrain3DMapEditService::commitMapEdits(
	const t3dMapType_t										  eMapType,
	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
	const AABB												 &rEditedArea,
	Terrain3DCompute *const									  pCompute ) {
	m_activeLocationScratch.clear();
	m_derivedLocationScratch.clear();
	m_derivedUpdateScratch.clear();
	if ( !m_pRepository || !m_pSamplingStorage || eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ||
		 rRegionLocations.empty() ) {
		return {};
	}

	m_activeLocationScratch.reserve(
		MIN< size_t >( rRegionLocations.size(),
					   ( size_t ) ( T3D_REGION_MAP_SIZE ) * ( size_t ) ( T3D_REGION_MAP_SIZE ) ) );
	AABB   editedArea = rEditedArea;
	real_t editedMinHeight = eastl::numeric_limits< real_t >::infinity();
	real_t editedMaxHeight = -eastl::numeric_limits< real_t >::infinity();

	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( rRegionLocations ).size() ); iRegion++ ) {
		const Vector2i		  &location = ( rRegionLocations )[iRegion];
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || m_pRepository->getRegionId( location ) < 0 ) {
			continue;
		}
		pRegion->markMapEdited( eMapType, true );
		if ( eMapType == TYPE_CONTROL || eMapType == TYPE_COLOR ) {
			pRegion->compactMapIfDefault( eMapType );
		}
		const Rect2i dirtyRect = getRegionDirtyRect( location, editedArea );
		if ( m_pSamplingStorage->isActiveOwner() &&
			 !m_pSamplingStorage->syncActiveMap( location, pRegion, eMapType, dirtyRect ) ) {
			continue;
		}
		m_pSamplingStorage->syncResidentMap( location, pRegion, eMapType, dirtyRect );
        if ( eMapType == TYPE_CONTROL ) rebuildControlTileClassesCold( location, pRegion );
		m_activeLocationScratch.push_back( location );
	}
	if ( m_activeLocationScratch.empty() ) {
		return {};
	}

	if ( eMapType == TYPE_HEIGHT ) {
		m_derivedLocationScratch.reserve(
			MIN< size_t >( m_activeLocationScratch.size() * 3u,
						   ( size_t ) ( T3D_REGION_MAP_SIZE ) * ( size_t ) ( T3D_REGION_MAP_SIZE ) ) );
		eastl::bitset< T3D_REGION_SLOTS > derivedLocationSeen;
		eastl::bitset< T3D_REGION_SLOTS > activeLocationSeen;
		for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( m_activeLocationScratch ).size() ); iLocation++ ) {
			const Vector2i &location = ( m_activeLocationScratch )[iLocation];
			const int		mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
			if ( mapIndex >= 0 ) {
				activeLocationSeen.set( ( size_t ) ( mapIndex ) );
			}

			eastl::fixed_vector< Vector2i, 3, false > candidates;
			candidates.push_back( location );
			const Rect2i dirtyRect = getRegionDirtyRect( location, editedArea );
			if ( dirtyRect.position.x == 0 ) {
				candidates.push_back( location + Vector2i( -1, 0 ) );
			}
			if ( dirtyRect.position.y == 0 ) {
				candidates.push_back( location + Vector2i( 0, -1 ) );
			}

			for ( size_t iCandidate = 0u; iCandidate < ( size_t ) ( ( candidates ).size() ); iCandidate++ ) {
				const Vector2i &candidate = ( candidates )[iCandidate];
				const int		candidateMapIndex = Terrain3DRegionRepository::getRegionMapIndex( candidate );
				if ( candidateMapIndex < 0 || derivedLocationSeen.test( ( size_t ) ( candidateMapIndex ) ) ) {
					continue;
				}
				Terrain3DRegion *const pCandidateRegion = m_pRepository->getRegionPtr( candidate );
				if ( !pCandidateRegion || pCandidateRegion->isDeleted() ) {
					continue;
				}
				derivedLocationSeen.set( ( size_t ) ( candidateMapIndex ) );
				m_derivedLocationScratch.push_back( candidate );
			}
		}

		regenerateDerivedMaps( m_derivedLocationScratch, editedArea, pCompute );
		for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( m_derivedLocationScratch ).size() ); iLocation++ ) {
			const Vector2i &location = ( m_derivedLocationScratch )[iLocation];
			const int		mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
			if ( mapIndex >= 0 && !activeLocationSeen.test( ( size_t ) ( mapIndex ) ) ) {
				m_pSamplingStorage->publishRegionGeneration( location );
			}
		}

		for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( m_activeLocationScratch ).size() ); iLocation++ ) {
			const Vector2i &location = ( m_activeLocationScratch )[iLocation];
			if ( const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location ) ) {
				const Vector2 range = pRegion->getHeightRange();
				editedMinHeight = MIN( editedMinHeight, range.x );
				editedMaxHeight = MAX( editedMaxHeight, range.y );
			}
		}
	}

	if ( editedArea.size.x <= 0.0f || editedArea.size.z <= 0.0f ) {
		bool bFirstRegion = true;
		for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( m_activeLocationScratch ).size() ); iLocation++ ) {
			const Vector2i				&location = ( m_activeLocationScratch )[iLocation];
			const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( !pRegion ) {
				continue;
			}
			const Vector2 range = pRegion->getHeightRange();
			const real_t  worldSize = T3D_REGION_SIZE * s_fRegionVertexSpacing;
			const AABB	  regionArea( Vector3( location.x * worldSize, range.x, location.y * worldSize ),
									  Vector3( worldSize, MAX( range.y - range.x, s_fRegionVertexSpacing ), worldSize ) );
			if ( bFirstRegion ) {
				editedArea = regionArea;
				bFirstRegion = false;
			} else {
				editedArea = editedArea.merge( regionArea );
			}
		}
	}

	Vector2 editedHeightRange = s_v2Zero;
	if ( eMapType == TYPE_HEIGHT ) {
		calculateHeightRange( false );
		if ( isfinite( editedMinHeight ) && isfinite( editedMaxHeight ) ) {
			editedHeightRange = Vector2( editedMinHeight, editedMaxHeight );
			editedArea.position.y = editedMinHeight;
			editedArea.size.y = MAX( editedMaxHeight - editedMinHeight, s_fRegionVertexSpacing );
		}
	}

	for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( m_activeLocationScratch ).size() ); iLocation++ ) {
		const Vector2i &location = ( m_activeLocationScratch )[iLocation];
		m_pSamplingStorage->publishRegionGeneration( location );
	}

	return makeMutationResult( eMapType, editedArea, editedHeightRange );
}

void Terrain3DMapEditService::finalizeMapEdits(
	const t3dMapMutation_t &rMutation,
	const bool				bKeepEdited ) {
	if ( !bKeepEdited && m_pRepository ) {
		for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( rMutation.m_changedLocations ).size() ); iLocation++ ) {
			const Vector2i &location = ( rMutation.m_changedLocations )[iLocation];
			if ( Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location ) ) {
				pRegion->setEdited( false );
			}
		}
	}
	if ( rMutation.m_editedArea.size.x > 0.0f && rMutation.m_editedArea.size.z > 0.0f ) {
		addEditedArea( rMutation.m_editedArea );
	}
}

void Terrain3DMapEditService::recycleMutation(
	t3dMapMutation_t &&rMutation ) {
	if ( rMutation.m_changedLocations.capacity() > m_activeLocationScratch.capacity() ) {
		m_activeLocationScratch.swap( rMutation.m_changedLocations );
	}
	if ( rMutation.m_derivedLocations.capacity() > m_derivedLocationScratch.capacity() ) {
		m_derivedLocationScratch.swap( rMutation.m_derivedLocations );
	}
	if ( rMutation.m_derivedUpdates.capacity() > m_derivedUpdateScratch.capacity() ) {
		m_derivedUpdateScratch.swap( rMutation.m_derivedUpdates );
	}
	m_activeLocationScratch.clear();
	m_derivedLocationScratch.clear();
	m_derivedUpdateScratch.clear();
}

bool Terrain3DMapEditService::buildRegionEditArea(
	const Vector2i &rRegionLocation,
	const Rect2i   &rDirtyRect,
	AABB		   &rEditedArea ) const {
	rEditedArea = AABB();
	if ( !m_pRepository ) {
		return false;
	}
	const Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rRegionLocation );
	if ( !pRegion || pRegion->isDeleted() || m_pRepository->getRegionId( rRegionLocation ) < 0 ) {
		return false;
	}

	const int regionSize = T3D_REGION_SIZE;
	Rect2i	  dirtyRect = rDirtyRect;
	if ( dirtyRect.size.x <= 0 || dirtyRect.size.y <= 0 ) {
		dirtyRect = Rect2i( s_v2iZero, V2I( regionSize ) );
	} else {
		dirtyRect = dirtyRect.intersection( Rect2i( s_v2iZero, V2I( regionSize ) ) );
		if ( dirtyRect.size.x <= 0 || dirtyRect.size.y <= 0 ) {
			dirtyRect = Rect2i( s_v2iZero, V2I( regionSize ) );
		}
	}

	const real_t   spacing = s_fRegionVertexSpacing;
	const Vector2i pixelOrigin = rRegionLocation * regionSize + dirtyRect.position;
	const Vector2  heightRange = pRegion->getHeightRange();
	rEditedArea = AABB( Vector3( pixelOrigin.x * spacing, heightRange.x, pixelOrigin.y * spacing ),
						Vector3( MAX( dirtyRect.size.x * spacing, spacing ),
								 MAX( heightRange.y - heightRange.x, spacing ),
								 MAX( dirtyRect.size.y * spacing, spacing ) ) );
	return true;
}

void Terrain3DMapEditService::addEditedArea(
	const AABB &rArea ) {
	if ( m_editedArea.has_surface() ) {
		m_editedArea = m_editedArea.merge( rArea );
	} else {
		m_editedArea = rArea;
	}
}

void Terrain3DMapEditService::updateMasterHeight(
	const real_t fHeight ) {
	if ( fHeight < m_masterHeightRange.x ) {
		m_masterHeightRange.x = fHeight;
	} else if ( fHeight > m_masterHeightRange.y ) {
		m_masterHeightRange.y = fHeight;
	}
}

void Terrain3DMapEditService::updateMasterHeights(
	const Vector2 &rLowHigh ) {
	if ( rLowHigh.x < m_masterHeightRange.x ) {
		m_masterHeightRange.x = rLowHigh.x;
	}
	if ( rLowHigh.y > m_masterHeightRange.y ) {
		m_masterHeightRange.y = rLowHigh.y;
	}
}

void Terrain3DMapEditService::calculateHeightRange(
	const bool								 bRecursive,
	const t3dMapEditEnsureRegionResidentFn_t pEnsureResident,
	void *const								 pResidentContext ) {
	m_masterHeightRange = s_v2Zero;
	if ( !m_pRepository ) {
		return;
	}
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i	 regionLocation = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		Terrain3DRegion *pRegion = m_pRepository->getRegionPtr( regionLocation );
		if ( !pRegion ) {
			continue;
		}
		if ( bRecursive ) {
			if ( pEnsureResident && !pEnsureResident( pResidentContext, regionLocation ) ) {
				continue;
			}
			pRegion = m_pRepository->getRegionPtr( regionLocation );
			if ( !pRegion ) {
				continue;
			}
			pRegion->calcHeightRange();
		}
		updateMasterHeights( pRegion->getHeightRange() );
	}
}

void Terrain3DMapEditService::copyRegionMaps(
	const Terrain3DRegion *const pSourceRegion,
	const Rect2i				&rSourceRect,
	const Rect2i				&rDestinationRect,
	Terrain3DRegion *const		 pDestinationRegion ) const {
	if ( !pSourceRegion || !pDestinationRegion ) {
		return;
	}
	for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
		const t3dMapType_t mapType = ( t3dMapType_t ) ( mapIndex );
		Image *const	   pSourceImage = pSourceRegion->getMapPtr( mapType );
		Image			  *pDestinationImage = pDestinationRegion->getMapPtr( mapType );
		if ( pSourceImage ) {
			pDestinationImage = pDestinationRegion->getMapPtrForWrite( mapType );
			if ( pDestinationImage ) {
				pDestinationImage->blit_rect( pSourceImage, rSourceRect, rDestinationRect.position );
			}
		} else if ( pDestinationImage && mapType != TYPE_HEIGHT ) {
			pDestinationImage->fill_rect( rDestinationRect, s_szRegionMapDefaultColors( mapIndex ) );
		}
	}
}

Error Terrain3DMapEditService::importImages(
	const TypedArray< Image >				 &rImages,
	const Vector3							 &rGlobalPosition,
	const real_t							  fOffset,
	const real_t							  fScale,
	const t3dMapEditAcquireEditableRegionFn_t pAcquireRegion,
	void *const								  pAcquireContext,
	bool									 &rGenerateMipmaps ) {
	rGenerateMipmaps = false;
	if ( !m_pRepository || !pAcquireRegion || rImages.size() != REGION_TYPE_MAX ) {
		return ERR_INVALID_PARAMETER;
	}

	Vector2i imageSize = s_v2iZero;
	for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
		const Ref< Image > pImage = rImages[mapIndex];
		if ( pImage.is_null() || pImage->is_empty() ) {
			continue;
		}
		if ( imageSize == s_v2iZero ) {
			imageSize = pImage->get_size();
		} else if ( imageSize != pImage->get_size() ) {
			return ERR_INVALID_DATA;
		}
	}
	if ( imageSize == s_v2iZero ) {
		return ERR_INVALID_DATA;
	}

	const Vector3 descaledPosition = rGlobalPosition / s_fRegionVertexSpacing;
	const int	  maxDimension = T3D_REGION_SIZE * T3D_REGION_MAP_SIZE / 2;
	if ( abs( descaledPosition.x ) > maxDimension || abs( descaledPosition.z ) > maxDimension ||
		 descaledPosition.x + imageSize.x > maxDimension || descaledPosition.z + imageSize.y > maxDimension ) {
		return ERR_INVALID_PARAMETER;
	}

	TypedArray< Image > sourceImages;
	sourceImages.resize( REGION_TYPE_MAX );
	for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
		const Ref< Image > pImage = rImages[mapIndex];
		if ( pImage.is_null() ) {
			continue;
		}

		Ref< Image > pReadable = pImage;
		if ( pReadable->is_compressed() ) {
			pReadable = pReadable->duplicate();
			const Error decompressError = pReadable->decompress();
			if ( decompressError != OK ) {
				return decompressError;
			}
		}
		sourceImages[mapIndex] = pReadable;

		if ( mapIndex != TYPE_HEIGHT ) {
			continue;
		}
		Ref< Image > pAdjusted =
			Image::create_empty( pReadable->get_width(), pReadable->get_height(), false, Image::FORMAT_RF );
		for ( int y = 0; y < pReadable->get_height(); ++y ) {
			for ( int x = 0; x < pReadable->get_width(); ++x ) {
				const float sourceHeight = pReadable->get_pixel( x, y ).r;
				const float height = isfinite( sourceHeight ) ? sourceHeight * fScale + fOffset : fOffset;
				pAdjusted->set_pixel( x, y, Color( height, 0.0f, 0.0f, 1.0f ) );
			}
		}
		sourceImages[mapIndex] = pAdjusted;
	}

	const int imageStartX = ( int ) ( Math::floor( descaledPosition.x ) );
	const int imageStartZ = ( int ) ( Math::floor( descaledPosition.z ) );
	const int imageEndX = imageStartX + imageSize.x - 1;
	const int imageEndZ = imageStartZ + imageSize.y - 1;
	int		  startRegionX = ( int ) ( Math::floor( ( real_t ) ( imageStartX ) / ( real_t ) ( T3D_REGION_SIZE ) ) );
	int		  startRegionZ = ( int ) ( Math::floor( ( real_t ) ( imageStartZ ) / ( real_t ) ( T3D_REGION_SIZE ) ) );
	int		  endRegionX = ( int ) ( Math::floor( ( real_t ) ( imageEndX ) / ( real_t ) ( T3D_REGION_SIZE ) ) );
	int		  endRegionZ = ( int ) ( Math::floor( ( real_t ) ( imageEndZ ) / ( real_t ) ( T3D_REGION_SIZE ) ) );

	const int halfRegionMap = T3D_REGION_MAP_SIZE / 2;
	startRegionX = CLAMP( startRegionX, -halfRegionMap, halfRegionMap - 1 );
	startRegionZ = CLAMP( startRegionZ, -halfRegionMap, halfRegionMap - 1 );
	endRegionX = CLAMP( endRegionX, -halfRegionMap, halfRegionMap - 1 );
	endRegionZ = CLAMP( endRegionZ, -halfRegionMap, halfRegionMap - 1 );

	for ( int regionZ = startRegionZ; regionZ <= endRegionZ; ++regionZ ) {
		for ( int regionX = startRegionX; regionX <= endRegionX; ++regionX ) {
			const Vector2i regionLocation( regionX, regionZ );
			const int	   regionStartX = regionX * T3D_REGION_SIZE;
			const int	   regionStartZ = regionZ * T3D_REGION_SIZE;
			const int	   regionEndX = regionStartX + T3D_REGION_SIZE - 1;
			const int	   regionEndZ = regionStartZ + T3D_REGION_SIZE - 1;
			const int	   overlapStartX = MAX( regionStartX, imageStartX );
			const int	   overlapStartZ = MAX( regionStartZ, imageStartZ );
			const int	   overlapEndX = MIN( regionEndX, imageEndX );
			const int	   overlapEndZ = MIN( regionEndZ, imageEndZ );
			if ( overlapEndX < overlapStartX || overlapEndZ < overlapStartZ ) {
				continue;
			}

			Ref< Terrain3DRegion > pRegion;
			const Error			   acquireError = pAcquireRegion( pAcquireContext, regionLocation, pRegion );
			if ( acquireError != OK || pRegion.is_null() ) {
				return acquireError != OK ? acquireError : ERR_DOES_NOT_EXIST;
			}

			const int copyWidth = overlapEndX - overlapStartX + 1;
			const int copyHeight = overlapEndZ - overlapStartZ + 1;
			const int sourceX = overlapStartX - imageStartX;
			const int sourceZ = overlapStartZ - imageStartZ;
			const int destinationX = overlapStartX - regionStartX;
			const int destinationZ = overlapStartZ - regionStartZ;

			for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
				const Ref< Image > pImage = sourceImages[mapIndex];
				if ( pImage.is_null() || pImage->is_empty() ) {
					continue;
				}

				Ref< Image >	   pRegionMap;
				const t3dMapType_t mapType = ( t3dMapType_t ) ( mapIndex );
				const Ref< Image > pExistingMap = pRegion->getMap( mapType );
				if ( mapType == TYPE_HEIGHT ) {
					pRegionMap = Image::create_empty( T3D_REGION_SIZE, T3D_REGION_SIZE, false, Image::FORMAT_RF );
					if ( pExistingMap.is_valid() && !pExistingMap->is_empty() ) {
						for ( int y = 0; y < T3D_REGION_SIZE; ++y ) {
							for ( int x = 0; x < T3D_REGION_SIZE; ++x ) {
								const float height = t3dRegionCodecReadHeightPixel( *pExistingMap, Vector2i( x, y ) );
								pRegionMap->set_pixel( x, y, Color( height, 0.0f, 0.0f, 1.0f ) );
							}
						}
					} else {
						pRegionMap->fill( Color( 0.0f, 0.0f, 0.0f, 1.0f ) );
					}
				} else if ( pExistingMap.is_valid() && !pExistingMap->is_empty() ) {
					pRegionMap.instantiate();
					pRegionMap->copy_from( pExistingMap );
					if ( pRegionMap->get_format() != pImage->get_format() ) {
						pRegionMap->convert( pImage->get_format() );
					}
				} else {
					pRegionMap = Terrain3DUtil::getFilledImage( m_pRepository->getRegionSizeVector(),
																s_szRegionMapDefaultColors( mapIndex ),
																false,
																pImage->get_format() );
				}

				pRegionMap->blit_rect( pImage,
									   Rect2i( sourceX, sourceZ, copyWidth, copyHeight ),
									   Vector2i( destinationX, destinationZ ) );
				pRegion->setMap( mapType, pRegionMap );
				if ( mapType == TYPE_COLOR ) {
					rGenerateMipmaps = true;
				}
			}
            // An image import can replace controls without setPixel/commitMapEdits.
            rebuildControlTileClassesCold( regionLocation, pRegion.ptr() );
			pRegion->setModified( true );
			pRegion->sanitizeMaps();
		}
	}
	return OK;
}

Ref< Image > Terrain3DMapEditService::layeredToImage(
	const t3dMapType_t						 eMapType,
	const t3dMapEditEnsureRegionResidentFn_t pEnsureResident,
	void *const								 pResidentContext ) const {
	if ( !m_pRepository || T3D_REGION_SIZE <= 0 ) {
		return Ref< Image >();
	}
	const t3dMapType_t mapType = eMapType >= TYPE_HEIGHT && eMapType < REGION_TYPE_MAX ? eMapType : TYPE_HEIGHT;

	bool	 bFoundRegion = false;
	Vector2i topLeft = s_v2iZero;
	Vector2i bottomRight = s_v2iZero;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i				 location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		const Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( !bFoundRegion ) {
			topLeft = location;
			bottomRight = location;
			bFoundRegion = true;
		} else {
			topLeft.x = MIN( topLeft.x, location.x );
			topLeft.y = MIN( topLeft.y, location.y );
			bottomRight.x = MAX( bottomRight.x, location.x );
			bottomRight.y = MAX( bottomRight.y, location.y );
		}
	}

	if ( !bFoundRegion ) {
		return Terrain3DUtil::getFilledImage( m_pRepository->getRegionSizeVector(),
											  s_szRegionMapDefaultColors( mapType ),
											  false,
											  mapType == TYPE_HEIGHT ? Image::FORMAT_RF
																	 : s_szRegionMapFormats[mapType] );
	}

	const Vector2i imageSize =
		Vector2i( 1 + bottomRight.x - topLeft.x, 1 + bottomRight.y - topLeft.y ) * T3D_REGION_SIZE;
	Ref< Image > pImage =
		Terrain3DUtil::getFilledImage( imageSize,
									   s_szRegionMapDefaultColors( mapType ),
									   false,
									   mapType == TYPE_HEIGHT ? Image::FORMAT_RF : s_szRegionMapFormats[mapType] );

	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
		if ( location == s_v2iMax ) {
			continue;
		}
		const Terrain3DRegion *pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( pEnsureResident && !pEnsureResident( pResidentContext, location ) ) {
			continue;
		}
		pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion ) {
			continue;
		}

		const Vector2i	   imageLocation = ( location - topLeft ) * T3D_REGION_SIZE;
		const Ref< Image > pSource = pRegion->getMap( mapType );
		if ( mapType == TYPE_HEIGHT && pSource.is_valid() ) {
			for ( int y = 0; y < T3D_REGION_SIZE; ++y ) {
				for ( int x = 0; x < T3D_REGION_SIZE; ++x ) {
					pImage->set_pixel(
						imageLocation.x + x,
						imageLocation.y + y,
						Color( t3dRegionCodecReadHeightPixel( *pSource, Vector2i( x, y ) ), 0.0f, 0.0f, 1.0f ) );
				}
			}
		} else if ( pSource.is_valid() ) {
			pImage->blit_rect( pSource, Rect2i( s_v2iZero, m_pRepository->getRegionSizeVector() ), imageLocation );
		}
	}
	return pImage;
}

size_t Terrain3DMapEditService::getTransientScratchBytes(
	void ) const {
	return m_derivedTaskScratch.capacity() * sizeof( t3dDerivedMapTask_t ) +
		   m_fDerivedHaloScratch.capacity() * sizeof( float ) +
		   m_fDerivedWorkerHaloScratch.capacity() * sizeof( float ) +
		   m_derivedOutputScratch.capacity() * sizeof( uint8_t ) +
		   m_activeLocationScratch.capacity() * sizeof( Vector2i ) +
		   m_derivedLocationScratch.capacity() * sizeof( Vector2i ) +
		   m_derivedUpdateScratch.capacity() * sizeof( t3dDerivedMapUpdate_t );
}

t3dMapMutation_t Terrain3DMapEditService::makeMutationResult(
	const t3dMapType_t eMapType,
	const AABB		  &rEditedArea,
	const Vector2	  &rEditedHeightRange ) {
	t3dMapMutation_t result;
	result.m_mapType = eMapType;
	result.m_editedArea = rEditedArea;
	result.m_editedHeightRange = rEditedHeightRange;
	result.m_changedLocations.swap( m_activeLocationScratch );
	result.m_derivedLocations.swap( m_derivedLocationScratch );
	result.m_derivedUpdates.swap( m_derivedUpdateScratch );
	return result;
}
