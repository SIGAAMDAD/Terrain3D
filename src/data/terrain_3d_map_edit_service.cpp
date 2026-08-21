// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_map_edit_service.h"

#include <godot_cpp/classes/os.hpp>

#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampler.h"
#include "data/terrain_3d_sampling_storage.h"
#include "core/terrain_3d_util.h"
#include "rendering/terrain_3d_compute.h"
#include "simd/terrain_3d_parallel.h"
#include "simd/terrain_3d_simd.h"

#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/bitset.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

typedef struct t3dDerivedMapBuildContext_s {
	t3dSamplingSource_t m_samplingSource;
	eastl::vector<t3dDerivedMapTask_t> *m_pTasks = nullptr;
	float *m_pWorkerHaloScratch = nullptr;
	uint8_t *m_pOutputScratch = nullptr;
	size_t m_nWorkerHaloElements = 0u;
	int m_nRegionSize = 0;
	float m_fVertexSpacing = 1.0f;
} t3dDerivedMapBuildContext_t;

static void t3dMapEditServiceBuildDerivedMapRange( void *pContext, const size_t nBegin, const size_t nEnd, const int32_t nWorkerIndex ) {
	t3dDerivedMapBuildContext_t *const pBuildContext = ( t3dDerivedMapBuildContext_t * ) ( pContext );
	T3DSampler sampler( pBuildContext->m_samplingSource );
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

		const int haloStride = width + 2;
		const size_t halo_elements = ( size_t ) haloStride * ( size_t ) ( height + 2 );
		if ( pBuildContext->m_pWorkerHaloScratch == nullptr || pBuildContext->m_pOutputScratch == nullptr || halo_elements > pBuildContext->m_nWorkerHaloElements ) continue;
		float *const pHalo = pBuildContext->m_pWorkerHaloScratch + ( size_t ) MAX( 0, nWorkerIndex ) * pBuildContext->m_nWorkerHaloElements;
		const int64_t regionBaseX = ( int64_t ) ( task.m_location.x ) * pBuildContext->m_nRegionSize;
		const int64_t regionBaseZ = ( int64_t ) ( task.m_location.y ) * pBuildContext->m_nRegionSize;
		const int64_t haloGridX = regionBaseX + task.m_dirtyRect.position.x - 1;
		const int64_t haloGridZ = regionBaseZ + task.m_dirtyRect.position.y - 1;
		sampler.copyHeightRect( haloGridX, haloGridZ, width + 2, height + 2, pHalo, haloStride, false );
		for ( int y = -1; y <= height; ++y ) {
			for ( int x = -1; x <= width; ++x ) {
				float &sample = pHalo[( size_t ) ( y + 1 ) * ( size_t ) haloStride + ( size_t ) ( x + 1 )];
				if ( std::isfinite( sample ) ) {
					continue;
				}
				const int localX = task.m_dirtyRect.position.x + x;
				const int localY = task.m_dirtyRect.position.y + y;
				const int clampedX = CLAMP( localX, 0, pBuildContext->m_nRegionSize - 1 );
				const int clampedY = CLAMP( localY, 0, pBuildContext->m_nRegionSize - 1 );
				sample = sampler.sampleHeightGrid( regionBaseX + clampedX, regionBaseZ + clampedY, false );
				sample = std::isfinite( sample ) ? sample : 0.0f;
			}
		}

		if ( task.m_nOutputBytes != ( size_t ) width * ( size_t ) height * 2u ) continue;
		t3dSimdDerivedNormalsRg8( pHalo, width, height, haloStride,
			pBuildContext->m_fVertexSpacing, pBuildContext->m_pOutputScratch + task.m_iOutputOffset );
		task.m_bSuccess = true;
	}
}

T3DMapEditService::T3DMapEditService(
	T3DRegionRepository *const pRepository, T3DSamplingStorage *const pSamplingStorage
) :
		m_pRepository( pRepository ), m_pSamplingStorage( pSamplingStorage ) {
}

void T3DMapEditService::reset( void ) {
	m_editedArea = AABB();
	m_masterHeightRange = V2_ZERO;
	m_derivedTaskScratch.clear();
	m_fDerivedHaloScratch.clear();
	m_fDerivedWorkerHaloScratch.clear();
	m_derivedOutputScratch.clear();
	m_activeLocationScratch.clear();
	m_derivedLocationScratch.clear();
	m_derivedUpdateScratch.clear();
}

Rect2i T3DMapEditService::getRegionDirtyRect(
	const Vector2i &rLocation, const int nRegionSize, const real_t fVertexSpacing,
	const AABB &rEditedArea, const int nMargin
) {
	const Rect2i bounds( V2I_ZERO, V2I( nRegionSize ) );
	if ( nRegionSize <= 0 || fVertexSpacing <= 0.0f || rEditedArea.size.x <= 0.0f ||
		rEditedArea.size.z <= 0.0f ) {
		return bounds;
	}

	const Vector3 worldEnd = rEditedArea.get_end();
	const int64_t globalBeginX = ( int64_t ) ( Math::floor( ( double ) ( rEditedArea.position.x / fVertexSpacing ) ) ) - nMargin;
	const int64_t globalBeginZ = ( int64_t ) ( Math::floor( ( double ) ( rEditedArea.position.z / fVertexSpacing ) ) ) - nMargin;
	const int64_t globalEndX = ( int64_t ) ( Math::ceil( ( double ) ( worldEnd.x / fVertexSpacing ) ) ) + nMargin + 1;
	const int64_t globalEndZ = ( int64_t ) ( Math::ceil( ( double ) ( worldEnd.z / fVertexSpacing ) ) ) + nMargin + 1;
	const int64_t regionOriginX = ( int64_t ) ( rLocation.x ) * nRegionSize;
	const int64_t regionOriginZ = ( int64_t ) ( rLocation.y ) * nRegionSize;
	const int beginX = ( int ) ( CLAMP<int64_t>( globalBeginX - regionOriginX, 0, nRegionSize ) );
	const int beginZ = ( int ) ( CLAMP<int64_t>( globalBeginZ - regionOriginZ, 0, nRegionSize ) );
	const int endX = ( int ) ( CLAMP<int64_t>( globalEndX - regionOriginX, 0, nRegionSize ) );
	const int endZ = ( int ) ( CLAMP<int64_t>( globalEndZ - regionOriginZ, 0, nRegionSize ) );
	const Rect2i rect( Vector2i( beginX, beginZ ), Vector2i( endX - beginX, endZ - beginZ ) );
	return rect.size.x > 0 && rect.size.y > 0 ? rect : bounds;
}

Error T3DMapEditService::setPixel(
	const t3dMapType_t eMapType, const Vector3 &rGlobalPosition, const Color &rPixel,
	Vector2i &rRegionLocation, Rect2i &rDirtyRect
) {
	rRegionLocation = V2I_MAX;
	rDirtyRect = Rect2i();
	if ( !m_pRepository || eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ) {
		return ERR_INVALID_PARAMETER;
	}

	rRegionLocation = m_pRepository->getRegionLocation( rGlobalPosition );
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( rRegionLocation );
	if ( !pRegion || pRegion->isDeleted() || m_pRepository->getRegionId( rRegionLocation ) < 0 ) {
		return ERR_DOES_NOT_EXIST;
	}

	const Vector2i globalOffset = rRegionLocation * m_pRepository->getRegionSize();
	const Vector3 descaledPosition = rGlobalPosition / m_pRepository->getVertexSpacing();
	Vector2i imagePosition = Vector2i(
		descaledPosition.x - globalOffset.x,
		descaledPosition.z - globalOffset.y
	);
	imagePosition = imagePosition.clamp( V2I_ZERO, V2I( m_pRepository->getRegionSize() - 1 ) );

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
	return OK;
}

const eastl::vector<t3dDerivedMapUpdate_t> &T3DMapEditService::regenerateDerivedMaps(
	const eastl::vector<Vector2i> &rRegionLocations, const AABB &rEditedArea,
	T3DCompute *const pCompute
) {
	m_derivedUpdateScratch.clear();
	if ( !m_pRepository || !m_pSamplingStorage || rRegionLocations.empty() ||
		m_pRepository->getRegionSize() <= 0 || m_pRepository->getVertexSpacing() <= 0.0f ) {
		return m_derivedUpdateScratch;
	}

	m_derivedTaskScratch.clear();
	m_derivedTaskScratch.reserve( rRegionLocations.size() );
	eastl::vector<t3dDerivedMapTask_t> &tasks = m_derivedTaskScratch;
	for ( const Vector2i &location : rRegionLocations ) {
		t3dDerivedMapTask_t task;
		task.m_location = location;
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			tasks.push_back( std::move( task ) );
			continue;
		}

		task.m_bAvailable = true;
		task.m_dirtyRect = Rect2i( V2I_ZERO, V2I( m_pRepository->getRegionSize() ) );
		if ( pRegion->getDerivedNormalSlopeMap().is_valid() && rEditedArea.size.x > 0.0f && rEditedArea.size.z > 0.0f ) {
			const Vector2 regionWorldOrigin = Vector2( location ) *
				( real_t ) ( m_pRepository->getRegionSize() ) * m_pRepository->getVertexSpacing();
			const Vector2 editBegin( rEditedArea.position.x, rEditedArea.position.z );
			const Vector3 editEnd3d = rEditedArea.get_end();
			const Vector2 editEnd( editEnd3d.x, editEnd3d.z );
			const Vector2 localBegin = ( editBegin - regionWorldOrigin ) / m_pRepository->getVertexSpacing();
			const Vector2 localEnd = ( editEnd - regionWorldOrigin ) / m_pRepository->getVertexSpacing();
			const Vector2i pixelBegin = Vector2i( Math::floor( localBegin.x ), Math::floor( localBegin.y ) ) - V2I( 1 );
			const Vector2i pixelEnd = Vector2i( Math::ceil( localEnd.x ), Math::ceil( localEnd.y ) ) + V2I( 1 );
			task.m_dirtyRect = Rect2i( pixelBegin, pixelEnd - pixelBegin ).intersection(
				Rect2i( V2I_ZERO, V2I( m_pRepository->getRegionSize() ) )
			);
		}
		tasks.push_back( std::move( task ) );
	}

	size_t eligibleTaskCount = 0u;
	size_t eligibleTaskIndex = 0u;
	for ( size_t taskIndex = 0u; taskIndex < tasks.size(); ++taskIndex ) {
		const t3dDerivedMapTask_t &task = tasks[taskIndex];
		if ( task.m_bAvailable && task.m_dirtyRect.size.x > 0 && task.m_dirtyRect.size.y > 0 ) {
			eligibleTaskIndex = taskIndex;
			++eligibleTaskCount;
		}
	}

	const uint32_t hardwareThreads = MAX( 1u, ( uint32_t ) MAX( 1, OS::get_singleton()->get_processor_count() ) );
	const int32_t workerCount = ( int32_t ) MIN<size_t>( ( size_t ) hardwareThreads, MAX<size_t>( 1u, tasks.size() ) );
	size_t totalOutputBytes = 0u;
	size_t maxHaloElements = 0u;
	for ( t3dDerivedMapTask_t &task : tasks ) {
		if ( !task.m_bAvailable || task.m_dirtyRect.size.x <= 0 || task.m_dirtyRect.size.y <= 0 ) continue;
		task.m_iOutputOffset = totalOutputBytes;
		task.m_nOutputBytes = ( size_t ) task.m_dirtyRect.size.x * ( size_t ) task.m_dirtyRect.size.y * 2u;
		totalOutputBytes += task.m_nOutputBytes;
		maxHaloElements = MAX( maxHaloElements, ( size_t ) ( task.m_dirtyRect.size.x + 2 ) * ( size_t ) ( task.m_dirtyRect.size.y + 2 ) );
	}
	m_derivedOutputScratch.resize( totalOutputBytes );
	m_fDerivedWorkerHaloScratch.resize( maxHaloElements * ( size_t ) workerCount );

	const t3dSamplingSource_t samplingSource{m_pRepository, m_pSamplingStorage};
	if ( pCompute && eligibleTaskCount == 1u ) {
		t3dDerivedMapTask_t &task = tasks[eligibleTaskIndex];
		const int width = task.m_dirtyRect.size.x;
		const int height = task.m_dirtyRect.size.y;
		const int haloStride = width + 2;
		m_fDerivedHaloScratch.resize( ( size_t ) ( haloStride ) * ( size_t ) ( height + 2 ) );
		T3DSampler sampler( samplingSource );
		const int64_t regionBaseX = ( int64_t ) ( task.m_location.x ) * m_pRepository->getRegionSize();
		const int64_t regionBaseZ = ( int64_t ) ( task.m_location.y ) * m_pRepository->getRegionSize();
		const int64_t haloGridX = regionBaseX + task.m_dirtyRect.position.x - 1;
		const int64_t haloGridZ = regionBaseZ + task.m_dirtyRect.position.y - 1;
		sampler.copyHeightRect( haloGridX, haloGridZ, width + 2, height + 2,
			m_fDerivedHaloScratch.data(), haloStride, false );
		for ( int y = -1; y <= height; ++y ) {
			for ( int x = -1; x <= width; ++x ) {
				float &sample = m_fDerivedHaloScratch[
					( size_t ) ( y + 1 ) * ( size_t ) ( haloStride ) + ( size_t ) ( x + 1 )
				];
				if ( std::isfinite( sample ) ) {
					continue;
				}
				const int localX = task.m_dirtyRect.position.x + x;
				const int localY = task.m_dirtyRect.position.y + y;
				const int clampedX = CLAMP( localX, 0, m_pRepository->getRegionSize() - 1 );
				const int clampedY = CLAMP( localY, 0, m_pRepository->getRegionSize() - 1 );
				sample = sampler.sampleHeightGrid( regionBaseX + clampedX, regionBaseZ + clampedY, false );
				sample = std::isfinite( sample ) ? sample : 0.0f;
			}
		}

		PackedByteArray rgbaOutput;
		if ( pCompute->generateDerivedMap(
			m_fDerivedHaloScratch, width, height,
			( float ) ( m_pRepository->getVertexSpacing() ), rgbaOutput
		) && rgbaOutput.size() >= ( int64_t ) ( width ) * ( int64_t ) ( height ) * 4 ) {
			const uint8_t *const pRgba = rgbaOutput.ptr();
			const size_t texelCount = ( size_t ) width * ( size_t ) height;
			uint8_t *const pOutput = m_derivedOutputScratch.data() + task.m_iOutputOffset;
			for ( size_t texel = 0u; texel < texelCount; ++texel ) {
				pOutput[texel * 2u] = pRgba[texel * 4u];
				pOutput[texel * 2u + 1u] = pRgba[texel * 4u + 2u];
			}
			task.m_bSuccess = true;
		}
	}

	const T3DParallelExecutor executor( workerCount );
	t3dDerivedMapBuildContext_t buildContext;
	buildContext.m_samplingSource = samplingSource;
	buildContext.m_pTasks = &tasks;
	buildContext.m_pWorkerHaloScratch = m_fDerivedWorkerHaloScratch.data();
	buildContext.m_pOutputScratch = m_derivedOutputScratch.data();
	buildContext.m_nWorkerHaloElements = maxHaloElements;
	buildContext.m_nRegionSize = m_pRepository->getRegionSize();
	buildContext.m_fVertexSpacing = ( float ) ( m_pRepository->getVertexSpacing() );
	executor.runStatic( tasks.size(), 1u, t3dMapEditServiceBuildDerivedMapRange, &buildContext );

	m_derivedUpdateScratch.reserve( tasks.size() );
	for ( t3dDerivedMapTask_t &task : tasks ) {
		if ( !task.m_bAvailable || task.m_dirtyRect.size.x <= 0 || task.m_dirtyRect.size.y <= 0 ) {
			continue;
		}
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( task.m_location );
		t3dDerivedMapUpdate_t update;
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
		if ( task.m_nOutputBytes > 0u ) std::memcpy( output.ptrw(), m_derivedOutputScratch.data() + task.m_iOutputOffset, task.m_nOutputBytes );
		const int width = task.m_dirtyRect.size.x;
		const int height = task.m_dirtyRect.size.y;
		const Ref<Image> pTile = Image::create_from_data( width, height, false, Image::FORMAT_RG8, output );
		if ( pTile.is_null() ) {
			pRegion->invalidateDerivedNormalSlopeMap();
			m_derivedUpdateScratch.push_back( update );
			continue;
		}

		if ( task.m_dirtyRect.position == V2I_ZERO &&
			task.m_dirtyRect.size == V2I( m_pRepository->getRegionSize() ) ) {
			pRegion->setDerivedNormalSlopeMapInternal( pTile );
		} else {
			Ref<Image> pDerived = pRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() || pDerived->get_width() != m_pRepository->getRegionSize() ||
				pDerived->get_height() != m_pRepository->getRegionSize() ) {
				pRegion->invalidateDerivedNormalSlopeMap();
				m_derivedUpdateScratch.push_back( update );
				continue;
			}
			pDerived->blit_rect( pTile, Rect2i( V2I_ZERO, task.m_dirtyRect.size ), task.m_dirtyRect.position );
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

t3dMapMutation_t T3DMapEditService::commitMapEdits(
	const t3dMapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations,
	const AABB &rEditedArea, T3DCompute *const pCompute
) {
	m_activeLocationScratch.clear();
	m_derivedLocationScratch.clear();
	m_derivedUpdateScratch.clear();
	if ( !m_pRepository || !m_pSamplingStorage || eMapType < TYPE_HEIGHT ||
		eMapType >= REGION_TYPE_MAX || rRegionLocations.empty() ) {
		return {};
	}

	m_activeLocationScratch.reserve( MIN<size_t>(
		rRegionLocations.size(),
		( size_t ) ( m_pRepository->getRegionMapSize() ) * ( size_t ) ( m_pRepository->getRegionMapSize() )
	) );
	AABB editedArea = rEditedArea;
	real_t editedMinHeight = std::numeric_limits<real_t>::infinity();
	real_t editedMaxHeight = -std::numeric_limits<real_t>::infinity();

	for ( const Vector2i &location : rRegionLocations ) {
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || m_pRepository->getRegionId( location ) < 0 ) {
			continue;
		}
		pRegion->markMapEdited( eMapType, true );
		if ( eMapType == TYPE_CONTROL || eMapType == TYPE_COLOR ) {
			pRegion->compactMapIfDefault( eMapType );
		}
		const Rect2i dirtyRect = getRegionDirtyRect(
			location, m_pRepository->getRegionSize(), m_pRepository->getVertexSpacing(), editedArea
		);
		if ( m_pSamplingStorage->isActiveOwner() &&
			!m_pSamplingStorage->syncActiveMap( location, pRegion, eMapType, dirtyRect ) ) {
			continue;
		}
		m_pSamplingStorage->syncResidentMap( location, pRegion, eMapType, dirtyRect );
		m_activeLocationScratch.push_back( location );
	}
	if ( m_activeLocationScratch.empty() ) {
		return {};
	}

	if ( eMapType == TYPE_HEIGHT ) {
		m_derivedLocationScratch.reserve( MIN<size_t>(
			m_activeLocationScratch.size() * 3u,
			( size_t ) ( m_pRepository->getRegionMapSize() ) * ( size_t ) ( m_pRepository->getRegionMapSize() )
		) );
		eastl::bitset<s_nMaxRegionSlots> derivedLocationSeen;
		eastl::bitset<s_nMaxRegionSlots> activeLocationSeen;
		for ( const Vector2i &location : m_activeLocationScratch ) {
			const int mapIndex = T3DRegionRepository::getRegionMapIndex( location );
			if ( mapIndex >= 0 ) {
				activeLocationSeen.set( ( size_t ) ( mapIndex ) );
			}

			eastl::array<Vector2i, 3> candidates;
			int candidateCount = 1;
			candidates[0] = location;
			const Rect2i dirtyRect = getRegionDirtyRect(
				location, m_pRepository->getRegionSize(), m_pRepository->getVertexSpacing(), editedArea
			);
			if ( dirtyRect.position.x == 0 ) {
				candidates[( size_t ) ( candidateCount++ )] = location + Vector2i( -1, 0 );
			}
			if ( dirtyRect.position.y == 0 ) {
				candidates[( size_t ) ( candidateCount++ )] = location + Vector2i( 0, -1 );
			}

			for ( int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex ) {
				const Vector2i &candidate = candidates[( size_t ) ( candidateIndex )];
				const int candidateMapIndex = T3DRegionRepository::getRegionMapIndex( candidate );
				if ( candidateMapIndex < 0 || derivedLocationSeen.test( ( size_t ) ( candidateMapIndex ) ) ) {
					continue;
				}
				T3DRegion *const pCandidateRegion = m_pRepository->getRegionPtr( candidate );
				if ( !pCandidateRegion || pCandidateRegion->isDeleted() ) {
					continue;
				}
				derivedLocationSeen.set( ( size_t ) ( candidateMapIndex ) );
				m_derivedLocationScratch.push_back( candidate );
			}
		}

		regenerateDerivedMaps( m_derivedLocationScratch, editedArea, pCompute );
		for ( const Vector2i &location : m_derivedLocationScratch ) {
			const int mapIndex = T3DRegionRepository::getRegionMapIndex( location );
			if ( mapIndex >= 0 && !activeLocationSeen.test( ( size_t ) ( mapIndex ) ) ) {
				m_pSamplingStorage->publishRegionGeneration( location );
			}
		}

		for ( const Vector2i &location : m_activeLocationScratch ) {
			if ( const T3DRegion *const pRegion = m_pRepository->getRegionPtr( location ) ) {
				const Vector2 range = pRegion->getHeightRange();
				editedMinHeight = MIN( editedMinHeight, range.x );
				editedMaxHeight = MAX( editedMaxHeight, range.y );
			}
		}
	}

	if ( editedArea.size.x <= 0.0f || editedArea.size.z <= 0.0f ) {
		bool bFirstRegion = true;
		for ( const Vector2i &location : m_activeLocationScratch ) {
			const T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( !pRegion ) {
				continue;
			}
			const Vector2 range = pRegion->getHeightRange();
			const real_t worldSize = pRegion->getRegionSize() * m_pRepository->getVertexSpacing();
			const AABB regionArea(
				Vector3( location.m_nX * worldSize, range.x, location.m_nY * worldSize ),
				Vector3( worldSize, MAX( range.y - range.x, m_pRepository->getVertexSpacing() ), worldSize )
			);
			if ( bFirstRegion ) {
				editedArea = regionArea;
				bFirstRegion = false;
			} else {
				editedArea = editedArea.merge( regionArea );
			}
		}
	}

	Vector2 editedHeightRange = V2_ZERO;
	if ( eMapType == TYPE_HEIGHT ) {
		calculateHeightRange( false );
		if ( std::isfinite( editedMinHeight ) && std::isfinite( editedMaxHeight ) ) {
			editedHeightRange = Vector2( editedMinHeight, editedMaxHeight );
			editedArea.position.y = editedMinHeight;
			editedArea.size.y = MAX( editedMaxHeight - editedMinHeight, m_pRepository->getVertexSpacing() );
		}
	}

	for ( const Vector2i &location : m_activeLocationScratch ) {
		m_pSamplingStorage->publishRegionGeneration( location );
	}

	return makeMutationResult( eMapType, editedArea, editedHeightRange );
}

void T3DMapEditService::finalizeMapEdits( const t3dMapMutation_t &rMutation, const bool bKeepEdited ) {
	if ( !bKeepEdited && m_pRepository ) {
		for ( const Vector2i &location : rMutation.m_changedLocations ) {
			if ( T3DRegion *const pRegion = m_pRepository->getRegionPtr( location ) ) {
				pRegion->setEdited( false );
			}
		}
	}
	if ( rMutation.m_editedArea.size.x > 0.0f && rMutation.m_editedArea.size.z > 0.0f ) {
		addEditedArea( rMutation.m_editedArea );
	}
}

void T3DMapEditService::recycleMutation( t3dMapMutation_t &&rMutation ) {
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

bool T3DMapEditService::buildRegionEditArea(
	const Vector2i &rRegionLocation, const Rect2i &rDirtyRect, AABB &rEditedArea
) const {
	rEditedArea = AABB();
	if ( !m_pRepository ) {
		return false;
	}
	const T3DRegion *const pRegion = m_pRepository->getRegionPtr( rRegionLocation );
	if ( !pRegion || pRegion->isDeleted() || m_pRepository->getRegionId( rRegionLocation ) < 0 ) {
		return false;
	}

	const int regionSize = pRegion->getRegionSize();
	Rect2i dirtyRect = rDirtyRect;
	if ( dirtyRect.size.x <= 0 || dirtyRect.size.y <= 0 ) {
		dirtyRect = Rect2i( V2I_ZERO, V2I( regionSize ) );
	} else {
		dirtyRect = dirtyRect.intersection( Rect2i( V2I_ZERO, V2I( regionSize ) ) );
		if ( dirtyRect.size.x <= 0 || dirtyRect.size.y <= 0 ) {
			dirtyRect = Rect2i( V2I_ZERO, V2I( regionSize ) );
		}
	}

	const real_t spacing = m_pRepository->getVertexSpacing();
	const Vector2i pixelOrigin = rRegionLocation * regionSize + dirtyRect.position;
	const Vector2 heightRange = pRegion->getHeightRange();
	rEditedArea = AABB(
		Vector3( pixelOrigin.x * spacing, heightRange.x, pixelOrigin.y * spacing ),
		Vector3(
			MAX( dirtyRect.size.x * spacing, spacing ),
			MAX( heightRange.y - heightRange.x, spacing ),
			MAX( dirtyRect.size.y * spacing, spacing )
		)
	);
	return true;
}

void T3DMapEditService::addEditedArea( const AABB &rArea ) {
	if ( m_editedArea.has_surface() ) {
		m_editedArea = m_editedArea.merge( rArea );
	} else {
		m_editedArea = rArea;
	}
}

void T3DMapEditService::updateMasterHeight( const real_t fHeight ) {
	if ( fHeight < m_masterHeightRange.x ) {
		m_masterHeightRange.x = fHeight;
	} else if ( fHeight > m_masterHeightRange.y ) {
		m_masterHeightRange.y = fHeight;
	}
}

void T3DMapEditService::updateMasterHeights( const Vector2 &rLowHigh ) {
	if ( rLowHigh.x < m_masterHeightRange.x ) {
		m_masterHeightRange.x = rLowHigh.x;
	}
	if ( rLowHigh.y > m_masterHeightRange.y ) {
		m_masterHeightRange.y = rLowHigh.y;
	}
}

void T3DMapEditService::calculateHeightRange(
	const bool bRecursive, const EnsureRegionResidentFn pEnsureResident,
	void *const pResidentContext
) {
	m_masterHeightRange = V2_ZERO;
	if ( !m_pRepository ) {
		return;
	}
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i regionLocation = m_pRepository->getSlotsData()[slot];
		T3DRegion *pRegion = m_pRepository->getRegionPtr( regionLocation );
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

void T3DMapEditService::copyRegionMaps(
	const T3DRegion *const pSourceRegion, const Rect2i &rSourceRect,
	const Rect2i &rDestinationRect, T3DRegion *const pDestinationRegion
) const {
	if ( !pSourceRegion || !pDestinationRegion ) {
		return;
	}
	for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
		const t3dMapType_t mapType = ( t3dMapType_t ) ( mapIndex );
		Image *const pSourceImage = pSourceRegion->getMapPtr( mapType );
		Image *pDestinationImage = pDestinationRegion->getMapPtr( mapType );
		if ( pSourceImage ) {
			pDestinationImage = pDestinationRegion->getMapPtrForWrite( mapType );
			if ( pDestinationImage ) {
				pDestinationImage->blit_rect( pSourceImage, rSourceRect, rDestinationRect.position );
			}
		} else if ( pDestinationImage && mapType != TYPE_HEIGHT ) {
			pDestinationImage->fill_rect( rDestinationRect, TERRAIN3D_REGION_MAP_DEFAULT_COLORS[mapIndex] );
		}
	}
}

Error T3DMapEditService::importImages(
	const TypedArray<Image> &rImages, const Vector3 &rGlobalPosition,
	const real_t fOffset, const real_t fScale, const AcquireEditableRegionFn pAcquireRegion,
	void *const pAcquireContext, bool &rGenerateMipmaps
) {
	rGenerateMipmaps = false;
	if ( !m_pRepository || !pAcquireRegion || rImages.size() != REGION_TYPE_MAX ) {
		return ERR_INVALID_PARAMETER;
	}

	Vector2i imageSize = V2I_ZERO;
	for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
		const Ref<Image> pImage = rImages[mapIndex];
		if ( pImage.is_null() || pImage->is_empty() ) {
			continue;
		}
		if ( imageSize == V2I_ZERO ) {
			imageSize = pImage->get_size();
		} else if ( imageSize != pImage->get_size() ) {
			return ERR_INVALID_DATA;
		}
	}
	if ( imageSize == V2I_ZERO ) {
		return ERR_INVALID_DATA;
	}

	const Vector3 descaledPosition = rGlobalPosition / m_pRepository->getVertexSpacing();
	const int maxDimension = m_pRepository->getRegionSize() * m_pRepository->getRegionMapSize() / 2;
	if ( std::abs( descaledPosition.x ) > maxDimension || std::abs( descaledPosition.z ) > maxDimension ||
		descaledPosition.x + imageSize.x > maxDimension || descaledPosition.z + imageSize.y > maxDimension ) {
		return ERR_INVALID_PARAMETER;
	}

	TypedArray<Image> sourceImages;
	sourceImages.resize( REGION_TYPE_MAX );
	for ( int mapIndex = 0; mapIndex < REGION_TYPE_MAX; ++mapIndex ) {
		const Ref<Image> pImage = rImages[mapIndex];
		if ( pImage.is_null() ) {
			continue;
		}

		Ref<Image> pReadable = pImage;
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
		Ref<Image> pAdjusted = Image::create_empty(
			pReadable->get_width(), pReadable->get_height(), false, Image::FORMAT_RF
		);
		for ( int y = 0; y < pReadable->get_height(); ++y ) {
			for ( int x = 0; x < pReadable->get_width(); ++x ) {
				const float sourceHeight = pReadable->get_pixel( x, y ).r;
				const float height = std::isfinite( sourceHeight ) ? sourceHeight * fScale + fOffset : fOffset;
				pAdjusted->set_pixel( x, y, Color( height, 0.0f, 0.0f, 1.0f ) );
			}
		}
		sourceImages[mapIndex] = pAdjusted;
	}

	const int imageStartX = ( int ) ( Math::floor( descaledPosition.x ) );
	const int imageStartZ = ( int ) ( Math::floor( descaledPosition.z ) );
	const int imageEndX = imageStartX + imageSize.x - 1;
	const int imageEndZ = imageStartZ + imageSize.y - 1;
	int startRegionX = ( int ) ( Math::floor(
		( real_t ) ( imageStartX ) / ( real_t ) ( m_pRepository->getRegionSize() )
	) );
	int startRegionZ = ( int ) ( Math::floor(
		( real_t ) ( imageStartZ ) / ( real_t ) ( m_pRepository->getRegionSize() )
	) );
	int endRegionX = ( int ) ( Math::floor(
		( real_t ) ( imageEndX ) / ( real_t ) ( m_pRepository->getRegionSize() )
	) );
	int endRegionZ = ( int ) ( Math::floor(
		( real_t ) ( imageEndZ ) / ( real_t ) ( m_pRepository->getRegionSize() )
	) );

	const int halfRegionMap = m_pRepository->getRegionMapSize() / 2;
	startRegionX = CLAMP( startRegionX, -halfRegionMap, halfRegionMap - 1 );
	startRegionZ = CLAMP( startRegionZ, -halfRegionMap, halfRegionMap - 1 );
	endRegionX = CLAMP( endRegionX, -halfRegionMap, halfRegionMap - 1 );
	endRegionZ = CLAMP( endRegionZ, -halfRegionMap, halfRegionMap - 1 );

	for ( int regionZ = startRegionZ; regionZ <= endRegionZ; ++regionZ ) {
		for ( int regionX = startRegionX; regionX <= endRegionX; ++regionX ) {
			const Vector2i regionLocation( regionX, regionZ );
			const int regionStartX = regionX * m_pRepository->getRegionSize();
			const int regionStartZ = regionZ * m_pRepository->getRegionSize();
			const int regionEndX = regionStartX + m_pRepository->getRegionSize() - 1;
			const int regionEndZ = regionStartZ + m_pRepository->getRegionSize() - 1;
			const int overlapStartX = MAX( regionStartX, imageStartX );
			const int overlapStartZ = MAX( regionStartZ, imageStartZ );
			const int overlapEndX = MIN( regionEndX, imageEndX );
			const int overlapEndZ = MIN( regionEndZ, imageEndZ );
			if ( overlapEndX < overlapStartX || overlapEndZ < overlapStartZ ) {
				continue;
			}

			Ref<T3DRegion> pRegion;
			const Error acquireError = pAcquireRegion( pAcquireContext, regionLocation, pRegion );
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
				const Ref<Image> pImage = sourceImages[mapIndex];
				if ( pImage.is_null() || pImage->is_empty() ) {
					continue;
				}

				Ref<Image> pRegionMap;
				const t3dMapType_t mapType = ( t3dMapType_t ) ( mapIndex );
				const Ref<Image> pExistingMap = pRegion->getMap( mapType );
				if ( mapType == TYPE_HEIGHT ) {
					pRegionMap = Image::create_empty(
						m_pRepository->getRegionSize(), m_pRepository->getRegionSize(), false, Image::FORMAT_RF
					);
					if ( pExistingMap.is_valid() && !pExistingMap->is_empty() ) {
						for ( int y = 0; y < m_pRepository->getRegionSize(); ++y ) {
							for ( int x = 0; x < m_pRepository->getRegionSize(); ++x ) {
								const float height = t3dRegionCodecReadHeightPixel(
									*pExistingMap, Vector2i( x, y )
								);
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
					pRegionMap = T3DUtil::getFilledImage(
						m_pRepository->getRegionSizeVector(), TERRAIN3D_REGION_MAP_DEFAULT_COLORS[mapIndex], false,
						pImage->get_format()
					);
				}

				pRegionMap->blit_rect(
					pImage, Rect2i( sourceX, sourceZ, copyWidth, copyHeight ),
					Vector2i( destinationX, destinationZ )
				);
				pRegion->setMap( mapType, pRegionMap );
				if ( mapType == TYPE_COLOR ) {
					rGenerateMipmaps = true;
				}
			}
			pRegion->setModified( true );
			pRegion->sanitizeMaps();
		}
	}
	return OK;
}

Ref<Image> T3DMapEditService::layeredToImage(
	const t3dMapType_t eMapType, const EnsureRegionResidentFn pEnsureResident,
	void *const pResidentContext
) const {
	if ( !m_pRepository || m_pRepository->getRegionSize() <= 0 ) {
		return Ref<Image>();
	}
	const t3dMapType_t mapType = eMapType >= TYPE_HEIGHT && eMapType < REGION_TYPE_MAX
		? eMapType
		: TYPE_HEIGHT;

	bool bFoundRegion = false;
	Vector2i topLeft = V2I_ZERO;
	Vector2i bottomRight = V2I_ZERO;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		const T3DRegion *const pRegion = location == V2I_MAX ? nullptr : m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( !bFoundRegion ) {
			topLeft = location;
			bottomRight = location;
			bFoundRegion = true;
		} else {
			topLeft.x = MIN( topLeft.x, location.m_nX );
			topLeft.y = MIN( topLeft.y, location.m_nY );
			bottomRight.x = MAX( bottomRight.x, location.m_nX );
			bottomRight.y = MAX( bottomRight.y, location.m_nY );
		}
	}

	if ( !bFoundRegion ) {
		return T3DUtil::getFilledImage(
			m_pRepository->getRegionSizeVector(), TERRAIN3D_REGION_MAP_DEFAULT_COLORS[mapType], false,
			mapType == TYPE_HEIGHT ? Image::FORMAT_RF : TERRAIN3D_REGION_MAP_FORMATS[mapType]
		);
	}

	const Vector2i imageSize = Vector2i( 1 + bottomRight.x - topLeft.x, 1 + bottomRight.y - topLeft.y ) *
		m_pRepository->getRegionSize();
	Ref<Image> pImage = T3DUtil::getFilledImage(
		imageSize, TERRAIN3D_REGION_MAP_DEFAULT_COLORS[mapType], false,
		mapType == TYPE_HEIGHT ? Image::FORMAT_RF : TERRAIN3D_REGION_MAP_FORMATS[mapType]
	);

	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const T3DRegion *pRegion = m_pRepository->getRegionPtr( location );
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

		const Vector2i imageLocation = ( location - topLeft ) * m_pRepository->getRegionSize();
		const Ref<Image> pSource = pRegion->getMap( mapType );
		if ( mapType == TYPE_HEIGHT && pSource.is_valid() ) {
			for ( int y = 0; y < m_pRepository->getRegionSize(); ++y ) {
				for ( int x = 0; x < m_pRepository->getRegionSize(); ++x ) {
					pImage->set_pixel( imageLocation.x + x, imageLocation.y + y,
						Color( t3dRegionCodecReadHeightPixel( *pSource, Vector2i( x, y ) ), 0.0f, 0.0f, 1.0f ) );
				}
			}
		} else if ( pSource.is_valid() ) {
			pImage->blit_rect( pSource, Rect2i( V2I_ZERO, m_pRepository->getRegionSizeVector() ), imageLocation );
		}
	}
	return pImage;
}

size_t T3DMapEditService::getTransientScratchBytes( void ) const {
	return m_derivedTaskScratch.capacity() * sizeof( t3dDerivedMapTask_t ) +
		m_fDerivedHaloScratch.capacity() * sizeof( float ) +
		m_fDerivedWorkerHaloScratch.capacity() * sizeof( float ) +
		m_derivedOutputScratch.capacity() * sizeof( uint8_t ) +
		m_activeLocationScratch.capacity() * sizeof( Vector2i ) +
		m_derivedLocationScratch.capacity() * sizeof( Vector2i ) +
		m_derivedUpdateScratch.capacity() * sizeof( t3dDerivedMapUpdate_t );
}

t3dMapMutation_t T3DMapEditService::makeMutationResult(
	const t3dMapType_t eMapType, const AABB &rEditedArea,
	const Vector2 &rEditedHeightRange
) {
	t3dMapMutation_t result;
	result.m_mapType = eMapType;
	result.m_editedArea = rEditedArea;
	result.m_editedHeightRange = rEditedHeightRange;
	result.m_changedLocations.swap( m_activeLocationScratch );
	result.m_derivedLocations.swap( m_derivedLocationScratch );
	result.m_derivedUpdates.swap( m_derivedUpdateScratch );
	return result;
}
