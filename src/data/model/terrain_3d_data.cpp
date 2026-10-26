#include "memory/policy/terrain_3d_memory_account.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "Terrain3DTypeSupport.h"
#include "data/model/terrain_3d_data.h"
#include "memory/policy/terrain_3d_memory_profile.h"
#if defined( T3D_EDITOR_NATIVE )
#include "authoring/erosion/terrain_3d_erosion_job.h"
#include "authoring/erosion/terrain_3d_erosion_settings.h"
#endif
#include "streaming/utility/terrain_3d_scope_timer.h"

#include "Terrain3DConstants.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "core/utility/logger.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/persistence/terrain_3d_raster_bundle.h"
#include "data/cooking/terrain_3d_control_map_cooker.h"
#include "data/cooking/terrain_3d_region_image_cooker.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "data/runtime/terrain_3d_runtime_page_format.h"
#include "streaming/io/terrain_3d_shared_stream_pack.h"
#include "data/sampling/terrain_3d_sampler.h"
#include "eastl_config.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "instances/storage/terrain_3d_instance_page_codec.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/core/terrain_3d_memory.h"
#include "physics/build/terrain_3d_collision_surface_builder.h"
#include "physics/runtime/terrain_3d_collision.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "rendering/material/terrain_3d_material.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>

// Returns the half-open region-coordinate bounds touched by a positive-sized
// area. Computing the exclusive end independently is required for offset and
// negative-coordinate rectangles; ceil(area.size / region_size) is only valid
// when the area starts on a region boundary.
static Rect2i t3dDataGetRegionBoundsForArea(
	const Rect2i &rArea ) {
	if ( rArea.size.x <= 0 || rArea.size.y <= 0 ) {
		return Rect2i();
	}

	const Vector2i begin =
		Vector2i( t3dRegionCoordinateFromGrid( rArea.position.x ), t3dRegionCoordinateFromGrid( rArea.position.y ) );
	const Vector2i last = rArea.get_end() - Vector2i( 1, 1 );
	const Vector2i end =
		Vector2i( t3dRegionCoordinateFromGrid( last.x ) + 1, t3dRegionCoordinateFromGrid( last.y ) + 1 );
	return Rect2i( begin, end - begin );
}

// -----------------------------------------------------------------------------
// Private functions

bool Terrain3DData::setRegionSizeInternal(
	void ) {
	if ( !m_samplingStorage.claimOrReconfigureActive() ) {
		LOG( ERROR, "Unable to initialize instance-local active terrain storage." );
		return false;
	}
	m_residency.activeHeightResidency().markDirty();
	return true;
}

Error Terrain3DData::rehydrateActiveStorage(
	void ) {
	if ( !m_samplingStorage.isActiveOwner() ) {
		return ERR_UNCONFIGURED;
	}

	// The physical bank is intentionally smaller than the logical world. Clear its
	// descriptors here and let the normal residency pass republish only the current
	// working set. The 128 m specialization makes page geometry implicit.
	if ( !m_samplingStorage.replaceActiveHeightPages( NULL, NULL, 0u ) ) {
		LOG( ERROR, "Failed to reset active terrain page descriptors during scene activation" );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	m_samplingStorage.refreshActiveGeneration();
	m_residency.activeHeightResidency().markDirty();
	return OK;
}

bool Terrain3DData::syncActiveRegion(
	const Vector2i				&vLocation,
	const Terrain3DRegion *const pRegion ) {
	return m_samplingStorage.syncActiveRegion( vLocation, pRegion );
}

void Terrain3DData::clear(
	void ) {
	m_residency.resetRegionResidency();
	m_runtimeArtifacts.reset();
	m_samplingStorage.resetActive();
	m_samplingStorage.resetResidentPool();
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	m_residency.resetFarSurface();
	LOG( INFO, "Clearing data" );
	m_regionRepository.clear();
	m_sparseRasterTiles.clear();
	m_sparseRasterHierarchy.clear();
	m_sparseHierarchyIndex.clear();
	m_sparseRasterIndex.clear();
	m_sparseTileResidency.reset();
	m_sparseRegionInitialized.reset();
	m_sparseDirtySavedRegions.reset();
	m_bSparseHierarchyStale = false;
	m_bSparseBundleLoaded = false;
	m_mapEditService.reset();
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_defaultHeightMap.unref();
	m_defaultControlMap.unref();
	m_defaultColorMap.unref();
	m_defaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_generatedHeightMaps.clear();
	m_generatedTerrainShadowMasks.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();
	m_generatedMiddleCompositeMaps.clear();
	invalidateSamplingCache();
}

void Terrain3DData::ensureDefaultMaps(
	void ) {
	if ( T3D_REGION_SIZE <= 0 ) {
		return;
	}
	if ( m_defaultHeightMap.is_null() || m_defaultHeightMap->get_width() != T3D_REGION_SIZE ) {
		m_defaultHeightMap = Terrain3DUtil::getFilledImage( Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ),
															t3dRegionCodecDefaultHeightColor(),
															false,
															Image::FORMAT_R16 );
	}
	if ( m_defaultControlMap.is_null() || m_defaultControlMap->get_width() != T3D_REGION_SIZE ) {
		m_defaultControlMap = Terrain3DUtil::getFilledImage( Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ),
															 COLOR_CONTROL,
															 false,
															 Image::FORMAT_RF );
	}
	if ( m_defaultColorMap.is_null() || m_defaultColorMap->get_width() != T3D_REGION_SIZE ) {
		m_defaultColorMap = Terrain3DUtil::getFilledImage( Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ),
														   COLOR_ROUGHNESS,
														   false,
														   Image::FORMAT_RGBA8 );
	}
	if ( m_defaultDerivedMap.is_null() || m_defaultDerivedMap->get_width() != T3D_REGION_SIZE ) {
		m_defaultDerivedMap = Terrain3DUtil::getFilledImage( Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ),
															 Color( 0.5f, 0.5f, 0.0f, 1.0f ),
															 false,
															 Image::FORMAT_RG8 );
	}
	syncCacheDefaultMaps();
}

Ref< Image > Terrain3DData::getCookedSparseRegionChannel(
	const Terrain3DRegion *pRegion,
	const t3dMapType_t	   eMapType ) const {
	if ( !pRegion || pRegion->isDeleted() || IS_EDITOR || !m_bSparseBundleLoaded ||
		 pRegion->getMap( eMapType ).is_valid() ) {
		return Ref< Image >();
	}
	if ( eMapType != TYPE_HEIGHT &&
		 ( eMapType != TYPE_CONTROL && eMapType != TYPE_COLOR || !pRegion->hasChannel( eMapType ) ) ) {
		return Ref< Image >();
	}
	const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( pRegion->getLocation() );
	if ( mapIndex < 0 || m_sparseDirtySavedRegions.test( ( size_t ) mapIndex ) ) {
		return Ref< Image >();
	}
	return m_sparseTileResidency.reconstructRegion( pRegion->getLocation(), eMapType, m_sparseRasterIndex );
}

Ref< Image > Terrain3DData::getCookedSparseVisualFallback(
	const Terrain3DRegion *pRegion,
	const t3dMapType_t	   eChannel ) const {
	// A coarser raster may temporarily represent an absent visual channel,
	// but physics and editor source data must always use exact authored height.
	if ( IS_EDITOR || !pRegion || pRegion->isDeleted() || ( eChannel != TYPE_CONTROL && eChannel != TYPE_COLOR ) ||
		 !pRegion->hasChannel( eChannel ) || pRegion->getMap( eChannel ).is_valid() || m_bSparseHierarchyStale ||
		 m_sparseHierarchyIndex.getEntryCount() == 0u ) {
		return Ref< Image >();
	}
	const Vector2i location = pRegion->getLocation();
	if ( location.x < -16 || location.x >= 16 || location.y < -16 || location.y >= 16 ) {
		return Ref< Image >();
	}
	t3dRasterHierarchyPayload_t parent;
	uint8_t						foundLevel = 0u;
	if ( !m_sparseHierarchyIndex.readNearestAncestor( 5u,
													  ( uint8_t ) ( location.x + 16 ),
													  ( uint8_t ) ( location.y + 16 ),
													  parent,
													  foundLevel ) ) {
		return Ref< Image >();
	}
	// Each 32x32 LOD node represents 2^(5-level) logical regions per axis.
	// Select only this region's window before expanding to 128x128 staging.
	const uint32_t	regionsPerNode = 1u << ( 5u - foundLevel );
	const uint32_t	samplesPerRegion = 32u / regionsPerNode;
	const uint32_t	regionX = ( uint32_t ) ( location.x + 16 ) % regionsPerNode;
	const uint32_t	regionY = ( uint32_t ) ( location.y + 16 ) % regionsPerNode;
	PackedByteArray decoded;
	decoded.resize( T3D_REGION_SIZE * T3D_REGION_SIZE * 4 );
	uint8_t *const pOut = decoded.ptrw();
	for ( uint32_t y = 0u; y < T3D_REGION_SIZE; ++y ) {
		for ( uint32_t x = 0u; x < T3D_REGION_SIZE; ++x ) {
			const uint32_t sx = regionX * samplesPerRegion + x * samplesPerRegion / T3D_REGION_SIZE;
			const uint32_t sy = regionY * samplesPerRegion + y * samplesPerRegion / T3D_REGION_SIZE;
			const uint32_t parentPixel = sy * 32u + sx;
			uint8_t *const pPixel = pOut + ( y * T3D_REGION_SIZE + x ) * 4u;
			if ( eChannel == TYPE_CONTROL ) {
				const uint32_t packedControl = parent.m_szControl[parentPixel];
				memcpy( pPixel, &packedControl, 4u );
			} else {
				memcpy( pPixel, parent.m_szColor + parentPixel * 4u, 4u );
			}
		}
	}
	return Image::create_from_data( T3D_REGION_SIZE,
									T3D_REGION_SIZE,
									false,
									eChannel == TYPE_CONTROL ? Image::FORMAT_RF : Image::FORMAT_RGBA8,
									decoded );
}

Ref< Image > Terrain3DData::getRegionMapOrDefault(
	const Terrain3DRegion *pRegion,
	const t3dMapType_t	   eMapType ) const {
	const Ref< Image > cold = getCookedSparseRegionChannel( pRegion, eMapType );
	if ( cold.is_valid() ) {
		return cold;
	}
	return t3dRegionImageResolveMap( pRegion, eMapType, m_defaultHeightMap, m_defaultControlMap, m_defaultColorMap );
}

bool Terrain3DData::hasMaterializedMap(
	const t3dMapType_t eMapType ) const {
	if ( eMapType != TYPE_CONTROL && eMapType != TYPE_COLOR ) {
		return true;
	}
	Terrain3DRegion *const *const ppRegions = m_regionRepository.getRegionPtrMapData();
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		const Terrain3DRegion *const pRegion = ppRegions[i];
		if ( pRegion && !pRegion->isDeleted() && pRegion->hasChannel( eMapType ) ) {
			return true;
		}
	}
	return false;
}

bool Terrain3DData::refreshImplicitMapState(
	void ) {
	const bool bControlImplicit = !hasMaterializedMap( TYPE_CONTROL );
	const bool bColorImplicit = !hasMaterializedMap( TYPE_COLOR );
	const bool bChanged = bControlImplicit != m_bControlMapsImplicit || bColorImplicit != m_bColorMapsImplicit;
	if ( bControlImplicit != m_bControlMapsImplicit ) {
		m_bControlMapsImplicit = bControlImplicit;
		m_generatedControlMaps.clear();
	}
	if ( bColorImplicit != m_bColorMapsImplicit ) {
		m_bColorMapsImplicit = bColorImplicit;
		m_generatedColorMaps.clear();
	}
	if ( bChanged ) {
		syncCacheDefaultMaps();
	}
	return bChanged;
}

void Terrain3DData::syncCacheDefaultMaps(
	void ) {
	m_residency.gpuRegionCache().setTerrain( m_pTerrain );
	m_residency.gpuRegionCache().setDefaultMaps( m_defaultHeightMap,
												 m_defaultControlMap,
												 m_defaultColorMap,
												 m_defaultDerivedMap,
												 m_bControlMapsImplicit,
												 m_bColorMapsImplicit );
	m_residency.farSurfaceAtlasCache().setConfig( getFarSurfaceAtlasConfig() );
}

t3dFarSurfaceAtlasConfig_t Terrain3DData::getFarSurfaceAtlasConfig(
	void ) const {
	t3dFarSurfaceAtlasConfig_t config;
	config.m_bRequired = isFarAtlasRequired();
	config.m_bStrictCooked =
		m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode();
	config.m_nHeightAtlasSide = m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : T3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	config.m_nSurfaceAtlasSide = m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : T3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	// A fixed grant is computed from configured GPU capacity, never from live
	// occupancy or brush edits; avoids the historic reallocation feedback loop.
	config.m_nFinePageLimit = m_pTerrain ? CLAMP( m_pTerrain->getGpuRegionCachePageLimit() / 2, 16, 64 ) : 64;
	config.m_nFineUploadBudget = m_pTerrain ? CLAMP( m_pTerrain->getGpuRegionCachePageLimit() / 16, 1, 8 ) : 4;
	config.m_dataDirectory = m_pTerrain ? m_pTerrain->getDataDirectory() : String();
	config.m_pData = this;
	config.m_defaultHeightMap = m_defaultHeightMap;
	config.m_defaultControlMap = m_defaultControlMap;
	config.m_defaultColorMap = m_defaultColorMap;
	config.m_material = m_pTerrain ? m_pTerrain->materialResource() : Ref< Terrain3DMaterial >();
	config.m_assets = m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
	return config;
}

t3dMiddleHeightRegionCacheConfig_t Terrain3DData::getMiddleHeightCacheConfig(
	void ) const {
	t3dMiddleHeightRegionCacheConfig_t config;
	config.m_bAuthoring = m_pTerrain && m_pTerrain->isEditorAuthoringMode();
	config.m_nHotRadius = m_pTerrain ? m_pTerrain->getStreamingHotRadius() : 0;
	config.m_nResolutionDivisor =
		config.m_bAuthoring || !m_pTerrain ? 1 : m_pTerrain->runtimePageNumericConfig().m_nMiddleHeightDivisor;
	config.m_nRetireFrames = m_pTerrain ? m_pTerrain->getGpuRegionRetireFrames() : T3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
	return config;
}

void Terrain3DData::applyInstanceRegionCacheChanges(
	const t3dInstanceRegionCacheChanges_s &rChanges ) {
	if ( !m_pTerrain || !m_pTerrain->instancerObject() ) {
		return;
	}
	for ( int index = 0; index < rChanges.m_nEvicted; ++index ) {
		m_pTerrain->instancerObject()->destroyRegionMmis( t3dUnpackRegionCoord( rChanges.m_szEvicted[index] ) );
	}
	for ( int index = 0; index < rChanges.m_nLoaded; ++index ) {
		m_pTerrain->instancerObject()->updateMmis( -1, t3dUnpackRegionCoord( rChanges.m_szLoaded[index] ), false );
	}
}

void Terrain3DData::residencyPrepareGpuFarSurface(
	void *const pContext ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	if ( pData && pData->isFarAtlasRequired() ) {
		pData->ensureFarAtlases();
	}
}

void Terrain3DData::residencyApplyInstanceChanges(
	void *const							   pContext,
	const t3dInstanceRegionCacheChanges_s &rChanges ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	if ( pData ) {
		pData->applyInstanceRegionCacheChanges( rChanges );
	}
}

void Terrain3DData::residencyGpuPageTableChanged(
	void *const pContext ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	if ( pData ) {
		pData->emitGpuRegionMapChanged();
	}
}

Ref< Image > Terrain3DData::makeGpuColorImage(
	const Ref< Image > &rImage ) const {
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	return t3dRegionImageCookGpuColor( rImage, m_defaultColorMap, divisor );
}

Ref< Image > Terrain3DData::makeGpuDerivedImage(
	const Ref< Image > &rImage ) const {
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	return t3dRegionImageCookGpuDerived( rImage, m_defaultDerivedMap, divisor );
}

Ref< Image > Terrain3DData::makeMiddleCompositeImage(
	const Vector2i	   &rLocation,
	const Ref< Image > &rControl,
	const Ref< Image > &rColor ) const {
	if ( rControl.is_null() || rColor.is_null() || rControl->get_format() != Image::FORMAT_RF ||
		 rColor->get_format() != Image::FORMAT_RGBA8 || rColor->get_width() <= 0 || rColor->get_height() <= 0 ) {
		return Ref< Image >();
	}
	PackedColorArray									 material_colors;
	PackedFloat32Array									 material_scales;
	eastl::array< Ref< Image >, T3D_MAX_TEXTURE_ASSETS > material_images;
	const Ref< Terrain3DAssets > pAssets = m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
	t3dRegionImagePrepareMiddleCompositeMaterials( pAssets,
												   material_colors,
												   material_scales,
												   material_images.data(),
												   material_images.size() );
	PackedByteArray output;
	const int64_t	output_bytes = ( int64_t ) ( rColor->get_width() ) * rColor->get_height() * 4;
	output.resize( output_bytes );
	if ( output.size() != output_bytes ) {
		return Ref< Image >();
	}
	return t3dRegionImageCookMiddleComposite( rLocation,
											  rControl,
											  rColor,
											  material_colors,
											  material_scales,
											  material_images.data(),
											  material_images.size(),
											  output );
}

Ref< Image > Terrain3DData::makeTerrainShadowMaskImage(
	const Terrain3DRegion *const pRegion ) const {
	return t3dRegionImageCookTerrainShadowMask( pRegion, T3D_REGION_SIZE );
}

Ref< Image > Terrain3DData::makeGpuRenderControlImage(
	const Vector2i &vLocation ) const {
	return t3dControlCookNeighborhoodImage(
		vLocation,
		[&]( const Vector2i &location, t3dControlCookSource_t &source ) -> bool {
			const Terrain3DRegion *pRegion = getRegionPtr( location );
			Ref< Terrain3DRegion > pLoadedSource;
			const bool			   bNeedsSourceLoad = !pRegion || pRegion->isDeleted() ||
										  ( pRegion->hasChannel( TYPE_CONTROL ) && pRegion->getControlMap().is_null() );
			if ( bNeedsSourceLoad && m_residency.cpuRegionCache().loadSource( location, pLoadedSource ) ) {
				pRegion = pLoadedSource.ptr();
			}
			if ( !pRegion || pRegion->isDeleted() ) {
				return true;
			}
			source.m_bPresent = true;
			if ( !pRegion->hasChannel( TYPE_CONTROL ) || pRegion->getControlMap().is_null() ) {
				return true;
			}
			if ( !t3dControlCookSetExplicitSource( source, pRegion->getControlMap() ) ) {
				return false;
			}
			return true;
		} );
}

t3dRuntimePageConfig_s Terrain3DData::getRuntimePageConfig(
	void ) const {
	t3dRuntimePageConfig_s config;
	if ( !m_pTerrain ) {
		return config;
	}
	const t3dRuntimePageNumericConfig_t &runtime_config = m_pTerrain->runtimePageNumericConfig();
	config.m_dataDirectory = m_pTerrain->getDataDirectory();
	config.m_nColorDivisor = runtime_config.m_nColorDivisor;
	config.m_nDerivedDivisor = runtime_config.m_nDerivedDivisor;
	config.m_nMiddleHeightDivisor = runtime_config.m_nMiddleHeightDivisor;
	config.m_nPrepareThreads = runtime_config.m_nPrepareThreads;
	config.m_bCookedPagesEnabled = m_pTerrain->isGpuRegionCookedPagesEnabled();
	config.m_bEditorAuthoring = m_pTerrain->isEditorAuthoringMode();
	const Ref< Terrain3DAssets > pAssets = m_pTerrain->assetsResource();
	config.m_nMaterialSourceSignature = pAssets.is_valid() ? pAssets->getRuntimeMaterialSourceSignature() : 0u;
	const Ref< Terrain3DMaterial > material = m_pTerrain->materialResource();
	if ( material.is_valid() ) {
		uint64_t collision_signature = 1469598103934665603ull;
		t3dHashFnv1a64AppendNativeU32( collision_signature, ( uint32_t ) material->getWorldBackground() );
		t3dHashFnv1a64AppendNativeF32( collision_signature, ( float ) ( real_t ) material->get( "ground_level" ) );
		t3dHashFnv1a64AppendNativeF32( collision_signature, ( float ) ( real_t ) material->get( "region_blend" ) );
		config.m_nCollisionSurfaceSignature = collision_signature;
	}
	return config;
}

t3dResidencyFrameState_s Terrain3DData::makeResidencyFrameState(
	const Vector3 &rWorldPosition ) const {
	t3dResidencyFrameState_s frame;
	frame.m_worldPosition = rWorldPosition;
	const bool bGeometryReady = s_fRegionVertexSpacing > 0.0f;
	if ( bGeometryReady ) {
		const Vector2i			 rawCenter = m_regionRepository.getRegionLocation( rWorldPosition );
		const t3dGpuCacheCoord_t clampedCenter = t3dGpuCacheClampCenter( { rawCenter.x, rawCenter.y } );
		frame.m_residencyCenter = Vector2i( clampedCenter.m_nX, clampedCenter.m_nY );
	}
	frame.m_bCpuRegionCacheEnabled = m_residency.cpuRegionCache().isEnabled();
	if ( !m_pTerrain ) {
		return frame;
	}
	frame.m_streamingCenter = m_pTerrain->getRegionStreamingCenterInternal();
	frame.m_predictedCenter = m_pTerrain->getRegionStreamingPredictedCenterInternal();
	if ( bGeometryReady ) {
		frame.m_activeHeightCenter = m_regionRepository.getRegionLocation( m_pTerrain->getClipmapTargetPosition() );
	}
	frame.m_pStreamingStates = m_pTerrain->getRegionStreamingStatesInternal();
	frame.m_pStreamingPrefetch = m_pTerrain->getRegionStreamingPrefetchInternal();
	frame.m_nStreamingGeneration = m_pTerrain->getRegionStreamingGeneration();
	frame.m_nStreamingActiveRadius = MAX( 0, m_pTerrain->getStreamingActiveRadius() );
	frame.m_nMiddleLods = m_pTerrain->getRuntimeMiddleShaderLods();
	frame.m_nGpuHysteresisTicks =
		t3dEditorGpuHysteresisTicks( m_pTerrain->isEditorAuthoringMode(), m_pTerrain->getGpuRegionHysteresisTicks() );
	frame.m_bAuthoring = m_pTerrain->isEditorAuthoringMode();
	frame.m_bRuntimeTiering = m_pTerrain->isRuntimeShaderTieringActive();
	frame.m_bCookedPagesEnabled = m_pTerrain->isGpuRegionCookedPagesEnabled();
	frame.m_bProfilingEnabled = m_pTerrain->isRuntimeProfilingEnabled();
	frame.m_runtimePageConfig = getRuntimePageConfig();
	return frame;
}

uint64_t Terrain3DData::computeGpuRuntimePageSourceSignature(
	const Terrain3DRegion *const pRegion ) const {
	return m_runtimeArtifacts.computeSourceSignature( pRegion, getRuntimePageConfig() );
}

bool Terrain3DData::readGpuRuntimePageSourceSignature(
	const String &rPagePath,
	uint64_t	 &nSignature ) const {
	return m_runtimeArtifacts.readSourceSignature( rPagePath, nSignature );
}

String Terrain3DData::getGpuRuntimePagePath(
	const Terrain3DRegion *const pRegion ) const {
	return m_runtimeArtifacts.getPagePath( pRegion, m_pTerrain ? m_pTerrain->getDataDirectory() : String() );
}

void Terrain3DData::pollGpuRuntimePagePrepare(
	const bool bWait ) {
	m_runtimeArtifacts.poll( bWait );
}

bool Terrain3DData::isGpuRuntimePagePreparePending(
	const Vector2i &vLocation ) const {
	return m_runtimeArtifacts.isPending( vLocation );
}

void Terrain3DData::requestGpuRuntimePage(
	const Vector2i &vLocation,
	const uint16_t	nConsumers,
	const uint8_t	nPriority ) {
	m_runtimeArtifacts.request( vLocation,
								nConsumers,
								nPriority,
								m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() );
}

void Terrain3DData::serviceGpuRuntimePagePrepare(
	const t3dResidencyFrameState_s &rFrame ) {
	t3dRuntimePageServiceState_t state;
	state.m_config = rFrame.m_runtimePageConfig;
	state.m_predictedCenter = rFrame.m_predictedCenter;
	state.m_nStreamingGeneration = rFrame.m_nStreamingGeneration;
	state.m_isWanted = &t3dResidencyFrameWantsRuntimePage;
	state.m_pWantedContext = const_cast< t3dResidencyFrameState_s * >( &rFrame );
	m_runtimeArtifacts.service( state );
}

bool Terrain3DData::takePreparedRuntimeHeight(
	const Vector2i		  &vLocation,
	const uint16_t		   nConsumer,
	Ref< Image >		  &rHeight,
	const bool			   bMiddle,
	PackedByteArray *const pNativeHeightBytes ) {
	return m_runtimeArtifacts.takeHeight( vLocation, nConsumer, rHeight, bMiddle, pNativeHeightBytes );
}

bool Terrain3DData::takePreparedRuntimeInstance(
	const Vector2i		   &vLocation,
	Terrain3DInstanceStore &rStore ) {
	return m_runtimeArtifacts.takeInstance( vLocation, rStore );
}

bool Terrain3DData::takePreparedGpuRuntimePage(
	const Vector2i			  &rLocation,
	t3dRuntimeGpuPageResult_t &rResult ) {
	return m_runtimeArtifacts.takeGpuPage( rLocation, rResult );
}

bool Terrain3DData::ensureCookedNodeDescriptors(
	const Vector2i &vLocation ) {
	Terrain3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->getStreamedNodeCount() == 0 || !pRegion->getStreamedNodesRuntimeInternal().empty() ) {
		return true;
	}
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS, 0u );
	PackedByteArray descriptors;
	if ( !m_runtimeArtifacts.takeNodeDescriptors( vLocation, descriptors ) ) {
		return false;
	}
	if ( descriptors.is_empty() ) {
		return pRegion->getStreamedNodeCount() == 0;
	}
	return Terrain3DRuntimeBakeCodec::decodeNodeDescriptors( descriptors,
															 m_runtimeArtifacts.worldManifest(),
															 *pRegion );
}

int Terrain3DData::getStreamedNodeCapacityHint(
	const Vector2i &vLocation ) const {
	const t3dRuntimeWorldRegionRecord_t *const pRecord = m_runtimeArtifacts.worldManifest().findRegion( vLocation );
	if ( pRecord ) {
		return ( int ) ( pRecord->m_nStreamedNodeCount );
	}
	Terrain3DRegion *const pRegion = getRegionPtr( vLocation );
	return pRegion && !pRegion->isDeleted() ? MAX( pRegion->getStreamedNodeCount(), 0 ) : 0;
}

bool Terrain3DData::getCookedSceneDependencySpan(
	const Vector2i	&vLocation,
	const uint16_t *&pDependencies,
	uint16_t		&nDependencyCount ) const {
	pDependencies = NULL;
	nDependencyCount = 0u;
	const t3dRuntimeWorldRegionRecord_t *const pRecord = m_runtimeArtifacts.worldManifest().findRegion( vLocation );
	if ( !pRecord || pRecord->m_nSceneDependencyCount == 0u ) {
		return false;
	}
	const eastl::vector< uint16_t, Terrain3DEastlAllocator > &ids =
		m_runtimeArtifacts.worldManifest().getRegionSceneDependencies();
	const uint32_t first = pRecord->m_nSceneDependencyFirst;
	const uint32_t count = pRecord->m_nSceneDependencyCount;
	if ( first > ids.size() || count > ids.size() - first ) {
		return false;
	}
	pDependencies = ids.data() + first;
	nDependencyCount = ( uint16_t ) count;
	return true;
}

const String &Terrain3DData::getCookedSceneDependencyPath(
	const uint16_t nDependency ) const {
	return m_runtimeArtifacts.worldManifest().getSceneDependency( nDependency );
}

PackedStringArray Terrain3DData::getCookedSceneDependencies(
	const Vector2i &vLocation ) const {
	PackedStringArray						   dependencies;
	const t3dRuntimeWorldRegionRecord_t *const pRecord = m_runtimeArtifacts.worldManifest().findRegion( vLocation );
	if ( !pRecord ) {
		return dependencies;
	}
	const eastl::vector< uint16_t, Terrain3DEastlAllocator > &ids =
		m_runtimeArtifacts.worldManifest().getRegionSceneDependencies();
	const uint32_t end = pRecord->m_nSceneDependencyFirst + pRecord->m_nSceneDependencyCount;
	for ( uint32_t index = pRecord->m_nSceneDependencyFirst; index < end && index < ids.size(); ++index ) {
		const String &path = m_runtimeArtifacts.worldManifest().getSceneDependency( ids[index] );
		if ( !path.is_empty() ) {
			dependencies.push_back( path );
		}
	}
	return dependencies;
}

void Terrain3DData::requestCookedCollisionTile(
	const Vector2i &vLocation,
	const uint8_t	nPriority ) {
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_COLLISION, nPriority );
}

bool Terrain3DData::takeCookedCollisionTile(
	const Vector2i	&vLocation,
	PackedByteArray &rHeight,
	PackedByteArray &rBounds,
	PackedByteArray &rHoleMask ) {
	return m_runtimeArtifacts.takeCollision( vLocation, rHeight, rBounds, rHoleMask );
}

void Terrain3DData::requestCookedNavigationTile(
	const Vector2i &vLocation,
	const uint8_t	nPriority ) {
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_NAVIGATION, nPriority );
}

bool Terrain3DData::takeCookedNavigationTile(
	const Vector2i	&vLocation,
	PackedByteArray &rNavigation ) {
	return m_runtimeArtifacts.takeNavigation( vLocation, rNavigation );
}

void Terrain3DData::requestCookedTerrainShadowMask(
	const Vector2i &vLocation,
	const uint8_t	nPriority ) {
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW, nPriority );
}

bool Terrain3DData::takeCookedTerrainShadowMask(
	const Vector2i &vLocation,
	Ref< Image >   &rShadowMask ) {
	return m_runtimeArtifacts.takeTerrainShadowMask( vLocation, rShadowMask );
}

bool Terrain3DData::loadCookedCollisionTileNow(
	const Vector2i	&vLocation,
	PackedByteArray &rHeight,
	PackedByteArray &rBounds,
	PackedByteArray &rHoleMask ) const {
	return m_runtimeArtifacts.loadCollisionNow( vLocation, getRuntimePageConfig(), rHeight, rBounds, rHoleMask );
}

bool Terrain3DData::loadCookedNavigationTileNow(
	const Vector2i		  &vLocation,
	PackedByteArray		  &rNavigation,
	PackedByteArray *const pCollisionHeight,
	PackedByteArray *const pMinMaxHeight ) const {
	return m_runtimeArtifacts.loadNavigationNow( vLocation,
												 getRuntimePageConfig(),
												 rNavigation,
												 pCollisionHeight,
												 pMinMaxHeight );
}

bool Terrain3DData::loadCookedLightingReceiversNow(
	const Vector2i	&vLocation,
	PackedByteArray &rReceivers ) const {
	return m_runtimeArtifacts.loadLightingReceiversNow( vLocation, getRuntimePageConfig(), rReceivers );
}

int Terrain3DData::getRegionSlotReserve(
	void ) const {
	return IS_EDITOR ? 16 : 0;
}

bool Terrain3DData::canUpdateRegionSlotsIncrementally(
	void ) const {
	if ( m_residency.gpuRegionCache().isEnabled() ) {
		return false;
	}
	const int slotCount = m_regionRepository.getSlotCount();
	return !m_regionRepository.isRegionMapDirty() && slotCount > 0 && !m_generatedHeightMaps.isDirty() &&
		   !m_generatedControlMaps.isDirty() && !m_generatedColorMaps.isDirty() && !m_generatedDerivedMaps.isDirty() &&
		   m_generatedHeightMaps.size() == slotCount && m_generatedControlMaps.size() == slotCount &&
		   m_generatedColorMaps.size() == slotCount && m_generatedDerivedMaps.size() == slotCount &&
		   m_heightMaps.size() == slotCount && m_controlMaps.size() == slotCount && m_colorMaps.size() == slotCount &&
		   m_derivedMaps.size() == slotCount;
}

int Terrain3DData::calculateGpuRegionRequiredCorePagesForLods(
	const int nDetailLods ) const {
	return m_residency.gpuRegionCache().calculateRequiredCorePagesForLods( nDetailLods );
}

int Terrain3DData::calculateGpuRegionCapacityForLods(
	const int nDetailLods ) const {
	return m_residency.gpuRegionCache().calculateCapacityForLods( nDetailLods );
}

bool Terrain3DData::isGpuRegionColorChannelEnabled(
	void ) const {
	return m_residency.gpuRegionCache().isColorChannelEnabled();
}

bool Terrain3DData::isGpuRegionDerivedChannelEnabled(
	void ) const {
	return m_residency.gpuRegionCache().isDerivedChannelEnabled();
}

void Terrain3DData::resetGpuRegionCache(
	const bool bClearTextures ) {
	m_residency.resetGpu( bClearTextures );
}

bool Terrain3DData::isFarAtlasRequired(
	void ) const {
	return m_residency.gpuRegionCache().isEnabled() || ( m_pTerrain && m_pTerrain->isRuntimeShaderTieringActive() );
}

bool Terrain3DData::writeCookedGpuFarAtlases(
	const String &rDirectory ) {
	const t3dFarSurfaceAtlasConfig_t config = getFarSurfaceAtlasConfig();
	m_residency.farSurfaceAtlasCache().setConfig( config );
	const int mapSize = T3D_REGION_MAP_SIZE;
	LOG( INFO,
		 "Cooking far atlases: height ",
		 config.m_nHeightAtlasSide,
		 " (",
		 mapSize > 0 ? config.m_nHeightAtlasSide / mapSize : 0,
		 " samples/region), surface ",
		 config.m_nSurfaceAtlasSide,
		 " (",
		 mapSize > 0 ? config.m_nSurfaceAtlasSide / mapSize : 0,
		 " samples/region), region map ",
		 mapSize );
	return m_residency.farSurfaceAtlasCache().writeCooked( rDirectory );
}

void Terrain3DData::markAllGpuFarRegionsDirty(
	const t3dMapType_t eMapType ) {
	m_residency.farSurfaceAtlasCache().markAllDirty( eMapType );
}

void Terrain3DData::markGpuFarRegionDirty(
	const Vector2i	  &vLocation,
	const t3dMapType_t eMapType ) {
	m_residency.farSurfaceAtlasCache().markRegionDirty( vLocation, eMapType );
}

void Terrain3DData::armPendingGpuRegionRetirements(
	void ) {
	m_residency.gpuRegionCache().armPendingRetirements();
}

void Terrain3DData::invalidateGpuRegionLocation(
	const Vector2i &vLocation ) {
	m_residency.gpuRegionCache().invalidate( vLocation );
}

void Terrain3DData::queueGpuRegionRepublish(
	const Vector2i &vLocation ) {
	m_residency.gpuRegionCache().queueRepublish( vLocation );
}

bool Terrain3DData::republishGpuRegionLocation(
	const Vector2i &vLocation ) {
	const bool bChanged = m_residency.gpuRegionCache().republish( vLocation, getComputeBackendIfCreated() );
	if ( bChanged ) {
		emitGpuRegionMapChanged();
	}
	return bChanged;
}

void Terrain3DData::resetMiddleHeightCache(
	void ) {
	m_residency.resetMiddleHeight();
}

bool Terrain3DData::configureMiddleHeightCache(
	void ) {
	return m_residency.middleHeightRegionCache().configure( getMiddleHeightCacheConfig() );
}

void Terrain3DData::updateMiddleHeightResidencyInternal(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce ) {
	m_residency.middleHeightRegionCache().update( rFrame,
												  bForce,
												  getEffectiveGpuRegionUploadBudgetLayers(),
												  getMiddleHeightCacheConfig() );
}

Dictionary Terrain3DData::getMiddleHeightCacheStats(
	void ) const {
	return m_residency.middleHeightRegionCache().getStatsDictionary();
}

int Terrain3DData::fillGpuMaterialPriorityIds(
	uint8_t *const pIds,
	const int	   nCapacity ) const {
	return m_residency.gpuRegionCache().fillMaterialPriorityIds( pIds, nCapacity );
}

size_t Terrain3DData::getMiddleHeightCacheReservedBytes(
	void ) const {
	return m_residency.middleHeightRegionCache().getReservedBytes();
}

void Terrain3DData::resetCpuRegionCache(
	const bool bReleaseCleanMaps ) {
	m_residency.resetCpu( bReleaseCleanMaps );
}

void Terrain3DData::invalidateCpuRegionLocation(
	const Vector2i &vLocation ) {
	m_residency.cpuRegionCache().invalidate( vLocation );
}

bool Terrain3DData::requestCpuRegionSourceAsync(
	const Vector2i &vLocation ) {
	return m_residency.cpuRegionCache().requestSourceAsync( vLocation );
}

bool Terrain3DData::ensureCpuRegionResidentInternal(
	const Vector2i &vLocation,
	const bool		bDemandLoad ) {
	return m_residency.cpuRegionCache().ensureResident( vLocation, bDemandLoad );
}

void Terrain3DData::setRuntimePageBudgetOverrides(
	const int nGpuUploadLayers,
	const int nCpuLoadLayers,
	const int nInstanceLoadPages ) {
	m_residency.setBudgetOverrides( nGpuUploadLayers, nCpuLoadLayers, nInstanceLoadPages );
}

void Terrain3DData::clearRuntimePageBudgetOverrides(
	void ) {
	m_residency.clearBudgetOverrides();
}

int Terrain3DData::getEffectiveGpuRegionUploadBudgetLayers(
	void ) const {
	const int effective_layers = m_residency.effectiveGpuUploadLayers();
	return m_pTerrain ? t3dEditorGpuUploadLayers( m_pTerrain->isEditorAuthoringMode(), effective_layers )
					  : effective_layers;
}

int Terrain3DData::getEffectiveCpuRegionLoadBudgetLayers(
	void ) const {
	return m_residency.effectiveCpuLoadLayers();
}

int Terrain3DData::getEffectiveInstanceRegionLoadBudgetPages(
	void ) const {
	return m_residency.effectiveInstanceLoadPages();
}

void Terrain3DData::configureCpuRegionCache(
	const bool bEnabled,
	const int  nBudgetMb,
	const int  nLoadLayers,
	const int  nLoadBudgetMb ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	m_residency.cpuRegionCache().configure( bEnabled, nBudgetMb, nLoadLayers, nLoadBudgetMb );
}

bool Terrain3DData::ensureCpuRegionResident(
	const Vector2i &vLocation ) {
	return m_residency.cpuRegionCache().ensureResident( vLocation, true );
}

bool Terrain3DData::prepareSamplingArea(
	const Vector3 &rWorldPosition,
	const int	   nRadiusTexels ) {
	if ( !rWorldPosition.is_finite() || nRadiusTexels < 0 ) {
		return false;
	}
	return m_residency.cpuRegionCache().ensureSampleArea( rWorldPosition, nRadiusTexels );
}

bool Terrain3DData::tryPrepareSamplingArea(
	const Vector3 &position,
	const int	   radius ) {
	return m_residency.cpuRegionCache().ensureSampleArea( position, radius, false );
}

void Terrain3DData::updateCpuRegionResidencyInternal(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce ) {
	m_residency.cpuRegionCache().update( rFrame, bForce, getEffectiveCpuRegionLoadBudgetLayers() );
}

Dictionary Terrain3DData::getCpuRegionCacheStatsFast(
	void ) const {
	return m_residency.cpuRegionCache().getStatsDictionary( false, getEffectiveCpuRegionLoadBudgetLayers() );
}

Dictionary Terrain3DData::getCpuRegionCacheStats(
	void ) const {
	return m_residency.cpuRegionCache().getStatsDictionary( true, getEffectiveCpuRegionLoadBudgetLayers() );
}

void Terrain3DData::emitGpuRegionMapChanged(
	void ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	m_residency.gpuRegionCache().recordPageTableUpdate();
	emit_signal( "gpu_region_page_table_changed" );
	armPendingGpuRegionRetirements();
}

void Terrain3DData::resetInstanceRegionCache(
	const bool bRestoreAll ) {
	t3dInstanceRegionCacheChanges_s changes;
	m_residency.instanceRegionCache().reset( bRestoreAll, &changes );
	applyInstanceRegionCacheChanges( changes );
}

void Terrain3DData::invalidateInstanceRegionLocation(
	const Vector2i &vLocation ) {
	t3dInstanceRegionCacheChanges_s changes;
	m_residency.instanceRegionCache().invalidate( vLocation, &changes );
	applyInstanceRegionCacheChanges( changes );
}

bool Terrain3DData::ensureInstanceRegionResidentInternal(
	const Vector2i &vLocation,
	const bool		bDemandLoad ) {
	t3dInstanceRegionCacheChanges_s changes;
	const bool bReady = m_residency.instanceRegionCache().ensureResident( vLocation, bDemandLoad, &changes );
	applyInstanceRegionCacheChanges( changes );
	return bReady;
}

bool Terrain3DData::evictInstanceRegion(
	const Vector2i &vLocation ) {
	t3dInstanceRegionCacheChanges_s changes;
	const bool						bEvicted = m_residency.instanceRegionCache().evict( vLocation, &changes );
	applyInstanceRegionCacheChanges( changes );
	return bEvicted;
}

void Terrain3DData::configureInstanceRegionCache(
	const bool bEnabled,
	const int  nBudgetMb,
	const int  nLoadPages,
	const int  nLoadBudgetMb ) {
	Terrain3DMemoryAccountScope		memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	t3dInstanceRegionCacheChanges_s changes;
	m_residency.instanceRegionCache().configure( bEnabled, nBudgetMb, nLoadPages, nLoadBudgetMb, &changes );
	applyInstanceRegionCacheChanges( changes );
}

void Terrain3DData::updateInstanceRegionResidencyInternal(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce ) {
	t3dInstanceRegionCacheChanges_s changes;
	m_residency.instanceRegionCache().update( rFrame, bForce, getEffectiveInstanceRegionLoadBudgetPages(), changes );
	applyInstanceRegionCacheChanges( changes );
}

void Terrain3DData::serviceColdInstanceResidencyInternal(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce ) {
	// Poll prepared runtime pages first, materialize instance ownership in the
	// owner-controlled cold service, then service again so emitted requests can
	// launch the next fixed-capacity prepare batch.
	serviceGpuRuntimePagePrepare( rFrame );
	m_residency.instanceRegionCache().serviceColdMaterialization( rFrame,
																  bForce,
																  getEffectiveInstanceRegionLoadBudgetPages() );
	serviceGpuRuntimePagePrepare( rFrame );
}

void Terrain3DData::updateCpuRegionResidency(
	const Vector3 &rWorldPosition,
	const bool	   bForce ) {
	updateCpuRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::serviceColdInstanceResidency(
	const Vector3 &rWorldPosition,
	const bool	   bForce ) {
	serviceColdInstanceResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::updateInstanceRegionResidency(
	const Vector3 &rWorldPosition,
	const bool	   bForce ) {
	updateInstanceRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::updateGpuRegionResidency(
	const Vector3 &rWorldPosition,
	const bool	   bForce ) {
	updateGpuRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::updateRegionResidency(
	const Vector3 &rWorldPosition,
	const bool	   bForce ) {
	t3dRegionResidencyUpdateContext_t context;
	context.m_pCompute = getComputeBackendIfCreated();
	context.m_middleHeightConfig = getMiddleHeightCacheConfig();
	context.m_pOwnerContext = this;
	context.m_prepareGpuFarSurface = &Terrain3DData::residencyPrepareGpuFarSurface;
	context.m_applyInstanceChanges = &Terrain3DData::residencyApplyInstanceChanges;
	context.m_gpuPageTableChanged = &Terrain3DData::residencyGpuPageTableChanged;
	const t3dResidencyFrameState_s frame = makeResidencyFrameState( rWorldPosition );
	m_sparseTileResidency.setFrameDemand( frame );
	m_residency.updateRegionResidency( frame, bForce, context );
}

void Terrain3DData::resetRuntimeStreamingCaches(
	void ) {
	t3dRegionResidencyUpdateContext_t context;
	context.m_pOwnerContext = this;
	context.m_applyInstanceChanges = &Terrain3DData::residencyApplyInstanceChanges;
	m_residency.resetRuntimeCaches( context );
}

void Terrain3DData::appendRuntimeProfileSample(
	t3dRuntimeProfileSample_t &rSample ) const {
	m_residency.appendRuntimeProfileSample( rSample );
}

Dictionary Terrain3DData::getInstanceRegionCacheStatsFast(
	void ) const {
	return m_residency.instanceRegionCache().getStatsDictionary( false, getEffectiveInstanceRegionLoadBudgetPages() );
}

Dictionary Terrain3DData::getInstanceRegionCacheStats(
	void ) const {
	return m_residency.instanceRegionCache().getStatsDictionary( true, getEffectiveInstanceRegionLoadBudgetPages() );
}

void Terrain3DData::updateMapsBounded(
	const t3dMapType_t eMapType,
	const bool		   bAllRegions ) {
	const bool bImplicitStateChanged = refreshImplicitMapState();
	bool	   bTopologyChanged = false;
	if ( m_regionRepository.isRegionMapDirty() ) {
		m_regionRepository.rebuildRegionMapFromSlots();
		bTopologyChanged = true;
		m_residency.gpuRegionCache().markSelectionDirty();
		if ( !m_bStartupBulkLoad ) {
			emit_signal( "region_map_changed" );
		}
	}

	ensureDefaultMaps();
	bool bPhysicalMappingChanged = false;
	if ( bAllRegions && m_residency.gpuRegionCache().isTexturesReady() ) {
		bPhysicalMappingChanged = m_residency.gpuRegionCache().republishAllResidents( getComputeBackendIfCreated() );
	}

	if ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX ) {
		calcHeightRange();
	}

	if ( bImplicitStateChanged || bTopologyChanged || !m_residency.gpuRegionCache().isTexturesReady() ) {
		m_residency.gpuRegionCache().markSelectionDirty();
		const Vector3 center = m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : Vector3();
		updateGpuRegionResidency( center, false );
	}
	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}

	if ( !m_bStartupBulkLoad ) {
		const bool bHeightChanged = bAllRegions && ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX );
		const bool bControlChanged = bAllRegions && ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX );
		const bool bColorChanged = bAllRegions && ( eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX );
		if ( bHeightChanged ) {
			emit_signal( "height_maps_changed" );
		}
		if ( bControlChanged || bImplicitStateChanged ) {
			emit_signal( "control_maps_changed" );
		}
		if ( bColorChanged || bImplicitStateChanged ) {
			emit_signal( "color_maps_changed" );
		}
		if ( bHeightChanged || bControlChanged || bColorChanged || bImplicitStateChanged || bTopologyChanged ) {
			emit_signal( "maps_changed" );
		}
	}
}

void Terrain3DData::emitRegionLayoutChanged(
	const bool bTextureLayersChanged ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	LOG( DEBUG, "Emitting region_map_changed" );
	emit_signal( "region_map_changed" );
	if ( bTextureLayersChanged ) {
		LOG( DEBUG, "Emitting height_maps_changed" );
		emit_signal( "height_maps_changed" );
		LOG( DEBUG, "Emitting control_maps_changed" );
		emit_signal( "control_maps_changed" );
		LOG( DEBUG, "Emitting color_maps_changed" );
		emit_signal( "color_maps_changed" );
	}
	LOG( DEBUG, "Emitting maps_changed" );
	emit_signal( "maps_changed" );
	m_pTerrain->snap();
}

TypedArray< Vector2i > Terrain3DData::getRegionLocations(
	void ) const {
	return m_regionRepository.getActiveLocations();
}

TypedArray< Image > Terrain3DData::getActiveMaps(
	const t3dMapType_t eMapType ) const {
	TypedArray< Image > maps;
	if ( m_residency.cpuRegionCache().isEnabled() ) {
		LOG( ERROR,
			 "Whole-world map arrays are unavailable while bounded CPU residency is enabled. "
			 "Page one region with ensure_cpu_region_resident() and read that Terrain3DRegion, or disable the CPU "
			 "cache." );
		return maps;
	}
	maps.resize( m_regionRepository.getActiveRegionCount() );
	int write_index = 0;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[slot] );
		if ( location == s_v2iMax ) {
			continue;
		}
		const Terrain3DRegion *pRegion = getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() ) {
			maps[write_index++] = getRegionMapOrDefault( pRegion, eMapType );
		}
	}
	if ( write_index != maps.size() ) {
		maps.resize( write_index );
	}
	return maps;
}

TypedArray< Image > Terrain3DData::getHeightMaps(
	void ) const {
	return getActiveMaps( TYPE_HEIGHT );
}

TypedArray< Image > Terrain3DData::getControlMaps(
	void ) const {
	return getActiveMaps( TYPE_CONTROL );
}

TypedArray< Image > Terrain3DData::getColorMaps(
	void ) const {
	return getActiveMaps( TYPE_COLOR );
}

// Specialized copy/paste callback used by doForRegions().
void Terrain3DData::copyPasteDfr(
	const Terrain3DRegion *pSrcRegion,
	const Rect2i		  &rSrcRect,
	const Rect2i		  &rDstRect,
	Terrain3DRegion		  *pDstRegion ) {
	if ( !pSrcRegion || !pDstRegion ) {
		return;
	}
	m_mapEditService.copyRegionMaps( pSrcRegion, rSrcRect, rDstRect, pDstRegion );
	m_pTerrain->instancerObject()->copyPasteDfr( pSrcRegion, rSrcRect, pDstRegion );
}

// -----------------------------------------------------------------------------
// Public functions

Terrain3DData::Terrain3DData(
	void ) :
	m_mapEditService( &m_regionRepository, &m_samplingStorage ),
	m_regionPersistence( &m_regionRepository ),
	m_runtimeArtifacts( &m_regionRepository, &m_regionPersistence ),
	m_residency( m_regionRepository, m_samplingStorage, m_mapEditService, m_regionPersistence, m_runtimeArtifacts ) {
}

bool Terrain3DData::bindRuntimeAllocatorInternal(
	Terrain3DMemoryZone *const		   pZone,
	const t3dAllocatorOverflowPolicy_t ePolicy ) {
	if ( !pZone ) {
		return false;
	}
	if ( m_pRuntimeAllocatorZone && m_pRuntimeAllocatorZone != pZone ) {
		return false;
	}
	m_pRuntimeAllocatorZone = pZone;
	m_eRuntimeAllocatorOverflowPolicy = ePolicy;
	// Resource-owned region state, authoring scratch and persistence staging use
	// bootstrap allocation deliberately because those objects can outlive the
	// Terrain3D runtime memory image. Runtime-only owners bind explicitly in their
	// own initialization paths.
	if ( !m_samplingStorage.bindRuntimeAllocator( pZone ) ||
		 !m_runtimeArtifacts.bindRuntimeAllocator( pZone, ePolicy ) ) {
		return false;
	}
	return true;
}

Terrain3DData::~Terrain3DData(
	void ) {
	if ( !m_regionRepository.releaseRuntimeAllocatorStorage() ) {
		LOG( ERROR, "Failed releasing region allocator storage during Terrain3DData teardown" );
	}
	clear();
	if ( m_pCompute ) {
		memdelete( m_pCompute );
		m_pCompute = NULL;
	}
}

Terrain3DCompute *Terrain3DData::getComputeBackend(
	void ) {
	if ( !m_pCompute ) {
		m_pCompute = memnew( Terrain3DCompute );
	}
	return m_pCompute;
}

void Terrain3DData::configureResidentPool(
	const int nCapacity ) {
	if ( !m_samplingStorage.configureResidentPool( m_bControlMapsImplicit, nCapacity ) ) {
		LOG( WARN, "Unable to configure experimental resident region map pool" );
	}
}

void Terrain3DData::configureGpuRegionCache(
	const bool	   bEnabled,
	const uint64_t nBudgetBytes,
	const int	   nUploadLayers,
	const int	   nUploadBudgetMb,
	const int	   nSparePages,
	const int	   nRetireFrames ) {
	ensureDefaultMaps();
	m_residency.gpuRegionCache().setTerrain( m_pTerrain );
	syncCacheDefaultMaps();
	const bool bStorageChanged = m_residency.gpuRegionCache().configure( bEnabled,
																		 nBudgetBytes,
																		 nUploadLayers,
																		 nUploadBudgetMb,
																		 nSparePages,
																		 nRetireFrames );
	m_residency.farSurfaceAtlasCache().setConfig( getFarSurfaceAtlasConfig() );
	if ( bStorageChanged && m_pTerrain && T3D_REGION_SIZE > 0 ) {
		updateMaps( REGION_TYPE_MAX, true, false );
	}
}

void Terrain3DData::updateGpuRegionResidencyInternal(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce ) {
	m_residency.farSurfaceAtlasCache().resetFrameStats();
	m_sparseTileResidency.setFrameDemand( rFrame );
	m_residency.farSurfaceAtlasCache().setFrameDemand( rFrame );
	if ( isFarAtlasRequired() ) {
		ensureFarAtlases();
	}
	const t3dGpuRegionCacheUpdateResult_t result =
		m_residency.gpuRegionCache().update( rFrame,
											 bForce,
											 getEffectiveGpuRegionUploadBudgetLayers(),
											 getComputeBackendIfCreated() );
	if ( result.m_bPageTableChanged ) {
		emitGpuRegionMapChanged();
	}
}

void Terrain3DData::invalidateGpuFarSurface(
	void ) {
	m_residency.farSurfaceAtlasCache().setConfig( getFarSurfaceAtlasConfig() );
	m_residency.farSurfaceAtlasCache().invalidateSurface();
	if ( isFarAtlasRequired() && !m_residency.gpuRegionCache().isEnabled() ) {
		ensureFarAtlases();
	}
}

void Terrain3DData::invalidateGpuFarAtlasLayout(
	void ) {
	m_residency.farSurfaceAtlasCache().setConfig( getFarSurfaceAtlasConfig() );
	m_residency.farSurfaceAtlasCache().invalidateLayout();
	if ( isFarAtlasRequired() && m_samplingStorage.isActiveOwner() && T3D_REGION_SIZE > 0 && T3D_REGION_MAP_SIZE > 0 ) {
		ensureFarAtlases();
	}
}

bool Terrain3DData::ensureFarAtlases(
	void ) {
	if ( !isFarAtlasRequired() || !m_samplingStorage.isActiveOwner() || T3D_REGION_SIZE <= 0 ||
		 T3D_REGION_MAP_SIZE <= 0 ) {
		return false;
	}
	m_residency.farSurfaceAtlasCache().setConfig( getFarSurfaceAtlasConfig() );
	const bool bReady = m_residency.farSurfaceAtlasCache().ensure();
	if ( bReady && m_pTerrain ) {
		const Ref< Terrain3DMaterial > pMaterial = m_pTerrain->materialResource();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateFarAtlasTextures();
		}
	}
	return bReady;
}

Dictionary Terrain3DData::getRuntimeResidencyGrid(
	const int nRadius ) const {
	Dictionary grid;
	const int  radius = CLAMP( nRadius, 1, 15 );
	Vector2i   center;
	Vector2i   predicted_center;
	Dictionary streaming_stats;
	if ( m_pTerrain ) {
		streaming_stats = m_pTerrain->getRegionStreamingStats();
		center = streaming_stats["center"];
		predicted_center = streaming_stats["predicted_center"];
	}
	grid["center"] = center;
	grid["predicted_center"] = predicted_center;
	grid["radius"] = radius;
	grid["motion_direction"] = predicted_center - center;

	Array	  cells;
	const int diameter = radius * 2 + 1;
	cells.resize( diameter * diameter );
	int write_index = 0;
	for ( int z = center.y - radius; z <= center.y + radius; ++z ) {
		for ( int x = center.x - radius; x <= center.x + radius; ++x ) {
			const Vector2i location( x, z );
			Dictionary	   cell;
			cell["location"] = location;
			cell["offset"] = location - center;
			const bool						bValid = isRegionLocationValidInternal( location );
			const bool						bAuthored = bValid && hasRegion( location );
			const t3dRegionStreamingState_t state = m_pTerrain && bValid
														? m_pTerrain->getRegionStreamingStateInternal( location )
														: TERRAIN3D_REGION_STREAMING_UNLOADED;
			const bool predicted = m_pTerrain && bValid && m_pTerrain->isRegionStreamingPrefetchInternal( location );
			cell["valid_region_coordinate"] = bValid;
			cell["active"] = bAuthored;
			cell["streaming_state"] = ( int ) ( state );
			cell["streaming_state_name"] = String( t3dStreamingStateName( state ) );
			cell["predicted"] = predicted;
			cell["desired_class"] = state == TERRAIN3D_REGION_STREAMING_ACTIVE
										? 1
										: ( state == TERRAIN3D_REGION_STREAMING_HOT ? 2 : ( predicted ? 3 : 0 ) );
			const int gpu_slot = m_residency.gpuRegionCache().getLayerForLocation( location );
			cell["gpu_resident"] = gpu_slot >= 0;
			cell["gpu_layer"] = gpu_slot;
			cell["middle_height_resident"] = m_residency.middleHeightRegionCache().isResident( location );
			cell["cpu_resident"] = m_residency.cpuRegionCache().isResident( location );
			cell["instance_resident"] = m_residency.instanceRegionCache().isResident( location );
			cell["prepared"] = m_runtimeArtifacts.isPrepared( location );
			cell["prepare_pending"] = isGpuRuntimePagePreparePending( location );
			cell["gpu_slot_state"] = m_residency.gpuRegionCache().getSlotStateForLocation( location );
			cell["far_height_dirty"] = m_residency.farSurfaceAtlasCache().isRegionHeightDirty( location );
			cell["far_color_dirty"] = m_residency.farSurfaceAtlasCache().isRegionColorDirty( location );
			cells[write_index++] = cell;
		}
	}
	grid["cells"] = cells;
	grid["active_count"] = streaming_stats.get( "active", 0 );
	grid["hot_count"] = streaming_stats.get( "hot", 0 );
	grid["cold_count"] = streaming_stats.get( "cold", 0 );
	grid["unloaded_count"] = streaming_stats.get( "unloaded", 0 );
	grid["predicted_count"] = streaming_stats.get( "predicted_promotions", 0 );
	grid["required_count"] = streaming_stats.get( "active", 0 );
	grid["guard_count"] = streaming_stats.get( "hot", 0 );
	grid["prefetch_count"] = streaming_stats.get( "predicted_promotions", 0 );
	return grid;
}

Dictionary Terrain3DData::getGpuRegionCacheStatsFast(
	void ) const {
	Dictionary		 stats = m_residency.gpuRegionCache().getStatsDictionary( false );
	const Dictionary farStats = m_residency.farSurfaceAtlasCache().getStatsDictionary();
	const Array		 keys = farStats.keys();
	for ( int index = 0; index < keys.size(); ++index ) {
		stats[keys[index]] = farStats[keys[index]];
	}
	return stats;
}

bool Terrain3DData::isAuthoringGpuFullResidencyPossible(
	void ) const {
	return m_residency.gpuRegionCache().isAuthoringFullResidencyPossible();
}

bool Terrain3DData::isAuthoringGpuReadbackSafe(
	void ) const {
	return m_residency.gpuRegionCache().isAuthoringReadbackSafe();
}

Dictionary Terrain3DData::getStartupLoadStats(
	void ) const {
	Dictionary						 stats = m_regionPersistence.getStartupLoadStatsDictionary();
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	stats["startup_generation"] = ( int64_t ) ( m_nStartupGenerationLast );
	const t3dStartupStage_t eStartupStage = startup.m_eStage;
	stats["startup_stage"] = ( int64_t ) ( eStartupStage );
	stats["startup_stage_name"] = String( t3dStartupStageName( eStartupStage ) );
	stats["startup_state_generation"] = ( int64_t ) ( startup.m_nGeneration );
	stats["startup_completed_units"] = ( int64_t ) ( startup.m_nCompletedUnits );
	stats["startup_total_units"] = ( int64_t ) ( startup.m_nTotalUnits );
	stats["startup_start_usec"] = ( int64_t ) ( startup.m_nStartUsec );
	stats["startup_end_usec"] = ( int64_t ) ( startup.m_nEndUsec );
	stats["initialize_count"] = ( int64_t ) ( m_nStartupInitializeCount );
	stats["initialize_usec_last"] = ( int64_t ) ( m_nStartupInitializeUsecLast );
	stats["directory_load_count"] = ( int64_t ) ( m_nStartupDirectoryLoadCount );
	stats["directory_load_usec_last"] = ( int64_t ) ( m_nStartupDirectoryLoadUsecLast );
	stats["rehydrate_count"] = ( int64_t ) ( m_nStartupRehydrateCount );
	stats["rehydrate_usec_last"] = ( int64_t ) ( m_nStartupRehydrateUsecLast );
	stats["startup_region_count"] = ( int64_t ) ( m_nStartupRegionCountLast );
	stats["startup_bytes_read"] = ( int64_t ) ( m_nStartupBytesReadLast );
	const uint64_t nNowUsec = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0u;
	stats["startup_elapsed_ms"] = ( double ) ( t3dStartupDomainElapsedUsec( startup, nNowUsec ) ) / 1000.0;
	stats["startup_ready"] = eStartupStage == T3D_STARTUP_COMMITTED;
	return stats;
}

Dictionary Terrain3DData::getGpuRegionCacheStats(
	void ) const {
	Dictionary		 stats = m_residency.gpuRegionCache().getStatsDictionary( true );
	const Dictionary farStats = m_residency.farSurfaceAtlasCache().getStatsDictionary();
	const Array		 keys = farStats.keys();
	for ( int index = 0; index < keys.size(); ++index ) {
		stats[keys[index]] = farStats[keys[index]];
	}
	if ( m_pTerrain ) {
		stats["far_height_atlas_side"] = m_pTerrain->getGpuFarHeightAtlasSide();
		stats["far_surface_atlas_side"] = m_pTerrain->getGpuFarSurfaceAtlasSide();
		stats["far_quality_divisor"] = m_pTerrain->getGpuFarQualityDivisor();
		stats["cooked_pages_enabled"] = m_pTerrain->isGpuRegionCookedPagesEnabled();
		stats["cook_on_save"] = m_pTerrain->isGpuRegionCookOnSave();
	}
	return stats;
}

size_t Terrain3DData::getGpuRegionCacheReservedBytes(
	void ) const {
	return m_residency.gpuRegionCache().getReservedBytes();
}

size_t Terrain3DData::getGpuRegionCacheMinimumBytes(
	void ) const {
	return m_residency.gpuRegionCache().getMinimumBytes();
}

size_t Terrain3DData::getGpuRegionCacheHardCapBytes(
	void ) const {
	return m_residency.gpuRegionCache().getHardCapBytes();
}

size_t Terrain3DData::getCpuRegionCacheReservedBytes(
	void ) const {
	return m_residency.cpuRegionCache().getReservedBytes();
}

size_t Terrain3DData::getCpuRegionCacheMinimumBytes(
	void ) const {
	return m_residency.cpuRegionCache().getMinimumBytes();
}

size_t Terrain3DData::getCpuRegionCacheHardCapBytes(
	void ) const {
	return m_residency.cpuRegionCache().getHardCapBytes();
}

void Terrain3DData::setRuntimePagePreparedByteBudget(
	const uint64_t nBytes ) {
	m_runtimeArtifacts.setPreparedByteBudget( nBytes );
}

size_t Terrain3DData::getMemoryRuntimePagePreparedBytes(
	void ) const {
	const uint64_t bytes = m_runtimeArtifacts.getPreparedReservedBytes();
	return bytes > SIZE_MAX ? SIZE_MAX : ( size_t ) bytes;
}

size_t Terrain3DData::getMemoryNonPageTransientStagingBytes(
	void ) const {
	// Far-atlas CPU images are retained whole-world residency, not transient
	// staging. Terrain3D::calculateFixedMemoryBytes() already reserves their
	// canonical height/surface footprint through getGpuFarAtlasBytes(). Charging
	// the same retained images against the transient reserve a second time can
	// drive the runtime-page prepared-byte grant to zero after ensureFarAtlases().
	return m_mapEditService.getTransientScratchBytes();
}

size_t Terrain3DData::getMemoryTransientStagingBytes(
	void ) const {
	const size_t runtime_page_bytes = getMemoryRuntimePagePreparedBytes();
	const size_t non_page_bytes = getMemoryNonPageTransientStagingBytes();
	return runtime_page_bytes > SIZE_MAX - non_page_bytes ? SIZE_MAX : runtime_page_bytes + non_page_bytes;
}

size_t Terrain3DData::getMemoryFixedRepositoryBytes(
	void ) const {
	return sizeof( *this ) + m_samplingStorage.getActiveReservedBytes() + m_samplingStorage.getResidentReservedBytes();
}

size_t Terrain3DData::getMemoryAllocatorContainedRepositoryBytes(
	void ) const {
	return m_samplingStorage.getResidentReservedBytes();
}

uint64_t Terrain3DData::getMemoryComputeLocalGpuBytes(
	void ) const {
	return m_pCompute ? m_pCompute->getRetainedLocalGpuBufferBytes() : 0u;
}

Dictionary Terrain3DData::getComputeLocalGpuTelemetry(
	void ) const {
	Dictionary result;
	if ( !m_pCompute ) {
		return result;
	}
	const t3dLocalComputeBufferStats_t stats = m_pCompute->getLocalBufferStats();
	result["buffer_creates"] = ( int64_t ) stats.bufferCreates;
	result["uniform_set_creates"] = ( int64_t ) stats.uniformSetCreates;
	result["pipeline_creates"] = ( int64_t ) stats.pipelineCreates;
	result["resource_failures"] = ( int64_t ) stats.resourceFailures;
	result["upload_calls"] = ( int64_t ) stats.uploadCalls;
	result["bytes_uploaded"] = ( int64_t ) stats.bytesUploaded;
	result["readback_calls"] = ( int64_t ) stats.readbackCalls;
	result["bytes_read_back"] = ( int64_t ) stats.bytesReadBack;
	return result;
}

void Terrain3DData::resetStartupPreparationCold(
	void ) {
	m_startupPreparedRepository.reset();
	m_startupPreparedWorldManifest.reset();
	m_startupPreparedDirectory = String();
	m_bStartupPreparationValid = false;
	m_bStartupInitialDirectoryLoad = false;
	m_bStartupPreparedDirectory = false;
	m_bStartupPreparedBoundedCpuLoad = false;
	m_bStartupPreparedCookedManifest = false;
	m_bStartupNeedsRehydrate = false;
	m_bStartupDerivedPending = false;
}

Error Terrain3DData::beginStartupCold(
	Terrain3DOwner *const pTerrain,
	const uint32_t		  nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( !pTerrain || nStartupGeneration == 0u ) {
		LOG( ERROR, "Initialization failed, invalid Terrain3D owner/startup generation" );
		return ERR_INVALID_PARAMETER;
	}

	const uint64_t initialize_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][Data][Begin] BEGIN generation=", nStartupGeneration );
	m_nStartupGenerationLast = nStartupGeneration;
	t3dStartupDomainRequest( m_startupState, nStartupGeneration, initialize_started_usec, 3u );
	t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_PREPARING );
	resetStartupPreparationCold();
	++m_nStartupInitializeCount;
	m_nStartupInitializeUsecLast = 0u;
	m_nStartupDirectoryLoadUsecLast = 0u;
	m_nStartupRehydrateUsecLast = 0u;
	m_nStartupBytesReadLast = 0u;
	m_nStartupRegionCountLast = 0u;
	LOG( INFO, "[Terrain3D][Startup][Data] REQUESTED generation=", nStartupGeneration );

	if ( m_bRuntimeActive && m_pTerrain == pTerrain && m_samplingStorage.isActiveOwner() ) {
		const uint32_t previous_generation = m_samplingStorage.getActiveGeneration();
		const uint32_t current_generation = m_samplingStorage.refreshActiveGeneration();
		if ( current_generation != 0u && current_generation != previous_generation ) {
			resetMiddleHeightCache();
			resetGpuRegionCache( true );
		}
		const uint64_t initialize_finished_usec = Time::get_singleton()->get_ticks_usec();
		m_nStartupInitializeUsecLast = initialize_finished_usec - initialize_started_usec;
		t3dStartupDomainSetCompletedUnits( m_startupState, 3u );
		t3dStartupDomainFinish( m_startupState, T3D_STARTUP_COMMITTED, initialize_finished_usec );
		LOG( INFO,
			 "[Terrain3D][Startup][Data][Begin] END generation=",
			 nStartupGeneration,
			 " reused_active_storage=true elapsed=",
			 m_nStartupInitializeUsecLast,
			 " us" );
		return OK;
	}

	LOG( INFO, "Initializing data runtime" );
	m_pTerrain = pTerrain;
	const uint32_t previous_generation = m_samplingStorage.getActiveGeneration();
	if ( !setRegionSizeInternal() ) {
		m_bRuntimeActive = false;
		t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, Time::get_singleton()->get_ticks_usec() );
		return ERR_ALREADY_IN_USE;
	}
	const uint32_t active_generation = m_samplingStorage.refreshActiveGeneration();
	if ( active_generation == 0u ) {
		deactivateRuntimeStorage( pTerrain );
		t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, Time::get_singleton()->get_ticks_usec() );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if ( previous_generation != active_generation ) {
		resetMiddleHeightCache();
		resetGpuRegionCache( true );
	}

	ensureDefaultMaps();
	m_startupPreparedDirectory = m_pTerrain->getDataDirectory();
	m_bStartupInitialDirectoryLoad = !m_regionRepository.isLoaded() && !m_startupPreparedDirectory.is_empty();
	if ( m_bStartupInitialDirectoryLoad ) {
		m_bStartupBulkLoad = true;
		m_bSkipSlotReserveOnce = true;
	}

	configureCpuRegionCache( m_pTerrain->isCpuRegionCacheEffective(),
							 m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(),
							 m_pTerrain->getCpuRegionLoadBudgetLayers(),
							 m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
								  m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(),
								  m_pTerrain->getInstanceRegionLoadBudgetPages(),
								  m_pTerrain->getInstanceRegionLoadBudgetMb() );
	m_pTerrain->reconfigureGpuRegionCache();

	t3dStartupDomainSetCompletedUnits( m_startupState, 1u );
	LOG( INFO,
		 "[Terrain3D][Startup][Data][Begin] END generation=",
		 nStartupGeneration,
		 " initial_directory_load=",
		 m_bStartupInitialDirectoryLoad,
		 " elapsed=",
		 Time::get_singleton()->get_ticks_usec() - initialize_started_usec,
		 " us" );
	return OK;
}

Error Terrain3DData::prepareStartupCold(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	const uint64_t				prepare_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][Data][Prepare] BEGIN generation=", nStartupGeneration );
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( startup.m_nGeneration != nStartupGeneration ) {
		LOG( ERROR, "[Terrain3D][Startup][Data][Prepare] INVALID GENERATION expected=", nStartupGeneration,
			 " actual=", startup.m_nGeneration, " stage=", t3dStartupStageName( startup.m_eStage ) );
		return ERR_INVALID_DATA;
	}
	if ( startup.m_eStage == T3D_STARTUP_COMMITTED ) {
		return OK;
	}
	if ( startup.m_eStage != T3D_STARTUP_PREPARING ) {
		LOG( ERROR, "[Terrain3D][Startup][Data][Prepare] INVALID STAGE generation=", nStartupGeneration,
			 " actual=", t3dStartupStageName( startup.m_eStage ) );
		return ERR_BUSY;
	}

	if ( m_bStartupInitialDirectoryLoad ) {
		const uint64_t directory_load_started_usec = Time::get_singleton()->get_ticks_usec();
		LOG( INFO,
			 "[Terrain3D][Startup][Data][Prepare][DirectoryLoad] BEGIN generation=",
			 nStartupGeneration,
			 " path=",
			 m_startupPreparedDirectory );
		++m_nStartupDirectoryLoadCount;
		bool		bBoundedCpuLoad = false;
		bool		bCookedRuntimeManifest = false;
		const Error prepare_error = prepareDirectoryCandidateCold( m_startupPreparedDirectory,
																   m_startupPreparedRepository,
																   bBoundedCpuLoad,
																   bCookedRuntimeManifest );
		m_nStartupDirectoryLoadUsecLast = Time::get_singleton()->get_ticks_usec() - directory_load_started_usec;
		if ( prepare_error != OK && prepare_error != ERR_FILE_NOT_FOUND ) {
			LOG( ERROR,
				 "[Terrain3D][Startup][Data][Prepare][DirectoryLoad] FAIL generation=",
				 nStartupGeneration,
				 " error=",
				 prepare_error,
				 " elapsed=",
				 m_nStartupDirectoryLoadUsecLast,
				 " us" );
			m_bStartupBulkLoad = false;
			m_bSkipSlotReserveOnce = false;
			t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, Time::get_singleton()->get_ticks_usec() );
			return prepare_error;
		}
		m_bStartupPreparedDirectory = prepare_error == OK;
		m_bStartupPreparedBoundedCpuLoad = bBoundedCpuLoad;
		m_bStartupPreparedCookedManifest = bCookedRuntimeManifest;
		if ( prepare_error == OK ) {
			m_nStartupRegionCountLast = ( uint32_t ) ( m_startupPreparedRepository.getRegionCount() );
			if ( bCookedRuntimeManifest ) {
				const Ref< FileAccess > pManifestFile =
					FileAccess::open( m_startupPreparedWorldManifest.getPath( m_startupPreparedDirectory ),
									  FileAccess::READ );
				m_nStartupBytesReadLast = pManifestFile.is_valid() ? pManifestFile->get_length() : 0u;
			} else {
				m_nStartupBytesReadLast = m_regionPersistence.getStartupLoadStats().m_nBytesReadLast;
			}
		}
		LOG( INFO,
			 "[Terrain3D][Startup][Data][Prepare][DirectoryLoad] END generation=",
			 nStartupGeneration,
			 " result=",
			 prepare_error,
			 " regions=",
			 m_nStartupRegionCountLast,
			 " bytes=",
			 m_nStartupBytesReadLast,
			 " elapsed=",
			 m_nStartupDirectoryLoadUsecLast,
			 " us" );
	}

	m_bStartupNeedsRehydrate = !m_bStartupPreparedDirectory && ( m_regionRepository.getActiveRegionCount() > 0 ||
																 m_regionRepository.getSlotCount() > 0 );
	m_bStartupPreparationValid = true;
	t3dStartupDomainSetCompletedUnits( m_startupState, 2u );
	t3dStartupDomainSetStage( m_startupState, T3D_STARTUP_CPU_READY );
	LOG( INFO,
		 "[Terrain3D][Startup][Data][Prepare] END generation=",
		 nStartupGeneration,
		 " prepared_directory=",
		 m_bStartupPreparedDirectory,
		 " needs_rehydrate=",
		 m_bStartupNeedsRehydrate,
		 " elapsed=",
		 Time::get_singleton()->get_ticks_usec() - prepare_started_usec,
		 " us" );
	return OK;
}

Error Terrain3DData::commitStartupCold(
	const uint32_t nStartupGeneration ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	const uint64_t				commit_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][Data][Commit] BEGIN generation=", nStartupGeneration );
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( startup.m_nGeneration != nStartupGeneration || !m_bStartupPreparationValid ) {
		LOG( ERROR, "[Terrain3D][Startup][Data][Commit] INVALID PREPARATION expected=", nStartupGeneration,
			 " actual=", startup.m_nGeneration,
			 " prepared=", m_bStartupPreparationValid,
			 " stage=", t3dStartupStageName( startup.m_eStage ) );
		return ERR_INVALID_DATA;
	}
	if ( startup.m_eStage == T3D_STARTUP_COMMITTED ) {
		return OK;
	}
	if ( startup.m_eStage != T3D_STARTUP_CPU_READY ) {
		LOG( ERROR, "[Terrain3D][Startup][Data][Commit] INVALID STAGE generation=", nStartupGeneration,
			 " actual=", t3dStartupStageName( startup.m_eStage ) );
		return ERR_BUSY;
	}

	if ( m_bStartupPreparedDirectory ) {
		LOG( INFO, "[Terrain3D][Startup][Data][Commit][Directory] BEGIN generation=", nStartupGeneration );
		const Error commit_error = commitDirectoryCandidateCold( m_startupPreparedDirectory,
																 m_startupPreparedRepository,
																 m_bStartupPreparedBoundedCpuLoad,
																 m_bStartupPreparedCookedManifest,
																 false );
		if ( commit_error != OK ) {
			LOG( ERROR,
				 "[Terrain3D][Startup][Data][Commit][Directory] FAIL generation=",
				 nStartupGeneration,
				 " error=",
				 commit_error );
			m_bStartupBulkLoad = false;
			m_bSkipSlotReserveOnce = false;
			deactivateRuntimeStorage( m_pTerrain );
			t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, Time::get_singleton()->get_ticks_usec() );
			return commit_error;
		}
		LOG( INFO, "[Terrain3D][Startup][Data][Commit][Directory] END generation=", nStartupGeneration );
		m_startupPreparedRepository.reset();
		m_startupPreparedWorldManifest.reset();
		m_bStartupDerivedPending = true;
	} else if ( m_bStartupNeedsRehydrate ) {
		const uint64_t rehydrate_started_usec = Time::get_singleton()->get_ticks_usec();
		LOG( INFO, "[Terrain3D][Startup][Data][Commit][Rehydrate] BEGIN generation=", nStartupGeneration );
		++m_nStartupRehydrateCount;
		const Error rehydrate_error = rehydrateActiveStorage();
		m_nStartupRehydrateUsecLast = Time::get_singleton()->get_ticks_usec() - rehydrate_started_usec;
		if ( rehydrate_error != OK ) {
			LOG( ERROR,
				 "[Terrain3D][Startup][Data][Commit][Rehydrate] FAIL generation=",
				 nStartupGeneration,
				 " error=",
				 rehydrate_error,
				 " elapsed=",
				 m_nStartupRehydrateUsecLast,
				 " us" );
			deactivateRuntimeStorage( m_pTerrain );
			t3dStartupDomainFinish( m_startupState, T3D_STARTUP_FAILED, Time::get_singleton()->get_ticks_usec() );
			return rehydrate_error;
		}
		LOG( INFO,
			 "[Terrain3D][Startup][Data][Commit][Rehydrate] END generation=",
			 nStartupGeneration,
			 " elapsed=",
			 m_nStartupRehydrateUsecLast,
			 " us" );
	} else {
		m_regionRepository.rebuildRegionMapFromSlots();
		m_samplingStorage.refreshActiveGeneration();
	}

	m_regionRepository.setLoaded( true );
	configureResidentPool( 64 );
	m_bRuntimeActive = true;
	t3dStartupDomainSetCompletedUnits( m_startupState, 3u );
	const uint64_t commit_finished_usec = Time::get_singleton()->get_ticks_usec();
	if ( startup.m_nStartUsec != 0u ) {
		m_nStartupInitializeUsecLast = commit_finished_usec - startup.m_nStartUsec;
	}
	t3dStartupDomainFinish( m_startupState, T3D_STARTUP_COMMITTED, commit_finished_usec );
	LOG( INFO,
		 "[Terrain3D][Startup][Data][Commit] END generation=",
		 nStartupGeneration,
		 " elapsed=",
		 commit_finished_usec - commit_started_usec,
		 " us total=",
		 m_nStartupInitializeUsecLast,
		 " us" );
	return OK;
}

void Terrain3DData::finalizeStartupDerivedCold(
	const uint32_t nStartupGeneration ) {
	const t3dStartupDomainSnapshot_t startup = t3dStartupDomainSnapshot( m_startupState );
	if ( startup.m_nGeneration != nStartupGeneration || startup.m_eStage != T3D_STARTUP_COMMITTED ) {
		return;
	}
	const uint64_t finalize_started_usec = Time::get_singleton()->get_ticks_usec();
	LOG( INFO, "[Terrain3D][Startup][Data][Derived] BEGIN generation=", nStartupGeneration );
	if ( m_bStartupDerivedPending ) {
		finalizeDirectoryCandidateCold( m_bStartupPreparedCookedManifest );
		m_bStartupDerivedPending = false;
	}
	m_bStartupBulkLoad = false;
	m_bSkipSlotReserveOnce = false;
	m_bStartupPreparationValid = false;
	m_bStartupInitialDirectoryLoad = false;
	m_bStartupPreparedDirectory = false;
	m_bStartupPreparedBoundedCpuLoad = false;
	m_bStartupNeedsRehydrate = false;
	m_startupPreparedDirectory = String();
	LOG( INFO,
		 "[Terrain3D][Startup][Data][Derived] END generation=",
		 nStartupGeneration,
		 " elapsed=",
		 Time::get_singleton()->get_ticks_usec() - finalize_started_usec,
		 " us" );
}

void Terrain3DData::cancelStartupCold(
	const uint32_t nStartupGeneration ) {
	if ( !t3dStartupDomainMatches( m_startupState, nStartupGeneration ) ) {
		return;
	}
	const t3dStartupStage_t eStage = t3dStartupDomainGetStage( m_startupState );
	if ( eStage == T3D_STARTUP_FAILED || eStage == T3D_STARTUP_CANCELLED ) {
		return;
	}
	m_bStartupBulkLoad = false;
	m_bSkipSlotReserveOnce = false;
	resetStartupPreparationCold();
	( void ) t3dStartupDomainCancel( m_startupState, nStartupGeneration, Time::get_singleton()->get_ticks_usec() );
}

void Terrain3DData::deactivateRuntimeStorage(
	Terrain3DOwner *const pTerrain ) {
	if ( pTerrain && m_pTerrain && pTerrain != m_pTerrain ) {
		return;
	}
	if ( !m_bRuntimeActive && !m_samplingStorage.isActiveOwner() ) {
		return;
	}
	m_bRuntimeActive = false;
	m_residency.resetRegionResidency();
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	m_samplingStorage.resetResidentPool();
	m_samplingStorage.releaseActive();
}

bool Terrain3DData::releaseRuntimeAllocatorStorageInternal(
	void ) {
	m_runtimeArtifacts.releaseRuntimeAllocatorStorage();
	m_mapEditService.releaseRuntimeAllocatorStorage();
	return m_regionRepository.releaseRuntimeAllocatorStorage();
}

Dictionary Terrain3DData::getRuntimeState(
	void ) const {
	Dictionary state;
	state["terrain_initialized"] = m_pTerrain && m_pTerrain->isInitialized();
	state["repository_loaded"] = m_regionRepository.isLoaded();
	state["repository_topology_generation"] = ( int64_t ) ( m_regionRepository.getTopologyGeneration() );
	state["runtime_active"] = m_bRuntimeActive;
	state["active_storage_attached"] = m_samplingStorage.isActiveOwner();
	state["active_storage_generation"] = ( int64_t ) ( getActiveStorageGeneration() );
	state["region_map_size"] = T3D_REGION_MAP_SIZE;
	state["logical_region_count"] = m_regionRepository.getActiveRegionCount();
	state["gpu_resident_page_count"] = m_residency.gpuRegionCache().getResidentCount();
	state["middle_height_resident_page_count"] = m_residency.middleHeightRegionCache().getResidentCount();
	state["middle_height_page_table_valid"] = m_residency.middleHeightRegionCache().getRegionMapRid().is_valid();
	return state;
}

void Terrain3DData::setRegionLocations(
	const TypedArray< Vector2i > &vLocations ) {
	LOG( INFO, "Reconciling active region locations with array sized: ", vLocations.size() );

	// Public locations are compact while render texture slots deliberately retain
	// holes. The repository owns that stable slot topology; Terrain3DData only performs
	// the cache/render side effects caused by the topology change.
	const bool								 bArraysReady = canUpdateRegionSlotsIncrementally();
	const t3dRegionLocationReconcileResult_t reconcile = m_regionRepository.reconcileLocations( vLocations );
	if ( reconcile.m_bCapacityExceeded ) {
		LOG( ERROR, "Region slot capacity exceeded while reconciling active locations" );
	}

	for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( reconcile.m_removedLocations ).size() ); iLocation++ ) {
		const Vector2i location = ( reconcile.m_removedLocations )[iLocation];
		m_samplingStorage.invalidateResident( location );
		if ( m_samplingStorage.isActiveOwner() ) {
			m_samplingStorage.invalidateActive( location );
		}
		invalidateGpuRegionLocation( location );
		invalidateCpuRegionLocation( location );
		invalidateInstanceRegionLocation( location );
	}

	if ( !reconcile.m_bSlotsChanged ) {
		if ( m_regionRepository.isRegionMapDirty() ) {
			updateMaps( REGION_TYPE_MAX, false, false );
		}
		return;
	}

	const bool bCanUploadAddedLayers = bArraysReady && m_regionRepository.getSlotCount() == reconcile.m_nOldSlotCount;
	if ( !bCanUploadAddedLayers ) {
		updateMaps( REGION_TYPE_MAX, true, false );
		if ( m_pTerrain && m_pTerrain->instancerObject() ) {
			m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, false );
		}
		invalidateSamplingCache();
		return;
	}

	m_regionRepository.rebuildRegionMapFromSlots();
	bool						  bTextureLayersChanged = false;
	const t3dRegionCoord_t *const pSlots = m_regionRepository.getSlotsData();
	for ( size_t iSlot = 0u; iSlot < ( size_t ) ( ( reconcile.m_addedSlots ).size() ); iSlot++ ) {
		const int slot = ( reconcile.m_addedSlots )[iSlot];
		if ( slot < 0 || slot >= m_regionRepository.getSlotCount() ) {
			continue;
		}
		const Vector2i location = t3dUnpackRegionCoord( pSlots[slot] );
		if ( location == s_v2iMax ) {
			continue;
		}
		Terrain3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			updateMaps( REGION_TYPE_MAX, true, false );
			if ( m_pTerrain && m_pTerrain->instancerObject() ) {
				m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, false );
			}
			invalidateSamplingCache();
			return;
		}

		if ( !syncActiveRegion( location, pRegion ) ) {
			LOG( ERROR, "Unable to restore active terrain storage for region ", location );
			continue;
		}
		ensureDefaultMaps();
		m_heightMaps[slot] = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
		m_controlMaps[slot] = getRegionMapOrDefault( pRegion, TYPE_CONTROL );
		m_colorMaps[slot] = getRegionMapOrDefault( pRegion, TYPE_COLOR );
		Ref< Image > pDerived = pRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( { location } );
			pDerived = pRegion->getDerivedNormalSlopeMap();
		}
		pDerived = pDerived.is_valid() ? pDerived : m_defaultDerivedMap;
		m_derivedMaps[slot] = pDerived;
		m_generatedHeightMaps.update( m_heightMaps[slot], slot );
		m_generatedControlMaps.update( makeGpuRenderControlImage( location ), slot );
		m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[slot] ), slot );
		m_generatedDerivedMaps.update( pDerived, slot );
		if ( !IS_EDITOR ) {
			m_samplingStorage.syncResidentDerived( location, pRegion );
			pRegion->setDerivedNormalSlopeMapInternal( Ref< Image >() );
			m_derivedMaps[slot] = Ref< Image >();
		}
		bTextureLayersChanged = true;
	}

	calcHeightRange();
	emitRegionLayoutChanged( bTextureLayersChanged );
	if ( m_pTerrain && m_pTerrain->instancerObject() ) {
		m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, false );
	}
	invalidateSamplingCache();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after setRegionLocations" );
	}
#endif
}

// Returns an array of active regions, optionally a shallow or deep copy
TypedArray< Vector2i > Terrain3DData::getRegionSlots(
	void ) const {
	return m_regionRepository.getSlots();
}

TypedArray< Vector2i > Terrain3DData::getRenderRegionSlots(
	void ) const {
	return m_residency.gpuRegionCache().isEnabled() ? m_residency.gpuRegionCache().getRenderRegionSlots()
													: getRegionSlots();
}

PackedInt32Array Terrain3DData::getRegionMap(
	void ) const {
	PackedInt32Array region_map;
	if ( T3D_REGION_MAP_SIZE <= 0 ) {
		return region_map;
	}
	region_map.resize( T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE );
	int32_t		   *pWrite = region_map.ptrw();
	const int		half = T3D_REGION_MAP_SIZE / 2;
	const uint16_t *pSourceMap = m_residency.gpuRegionCache().isEnabled()
									 ? m_residency.gpuRegionCache().getRegionMapData()
									 : m_regionRepository.getLogicalRegionMapData();
	for ( int y = 0; y < T3D_REGION_MAP_SIZE; ++y ) {
		for ( int x = 0; x < T3D_REGION_MAP_SIZE; ++x ) {
			const Vector2i location( x - half, y - half );
			const int	   source_index = getRegionMapIndex( location );
			pWrite[y * T3D_REGION_MAP_SIZE + x] = source_index >= 0 ? pSourceMap[source_index] : 0;
		}
	}
	return region_map;
}

PackedInt32Array Terrain3DData::getLogicalRegionMap(
	void ) const {
	return m_regionRepository.getLogicalRegionMapPacked();
}

TypedArray< Terrain3DRegion > Terrain3DData::getRegionsActive(
	const bool bCopy,
	const bool bDeep ) const {
	TypedArray< Terrain3DRegion > region_arr;
	if ( bCopy && m_residency.cpuRegionCache().isEnabled() ) {
		LOG( ERROR,
			 "Whole-repository region copies are unavailable while bounded CPU residency is enabled. "
			 "Disable the CPU cache before requesting copied Terrain3DRegion resources." );
		return region_arr;
	}
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i		   region_loc = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[slot] );
		Ref< Terrain3DRegion > pRegion = getRegion( region_loc );
		if ( pRegion.is_valid() ) {
			region_arr.push_back( bCopy ? pRegion->duplicate( bDeep ) : pRegion );
		}
	}
	return region_arr;
}

// Calls the callback for every region intersecting the descaled area.
void Terrain3DData::doForRegions(
	const Rect2i   &rArea,
	const Callable &rCallback ) {
	const Rect2i location_bounds = t3dDataGetRegionBoundsForArea( rArea );
	if ( location_bounds.size.x <= 0 || location_bounds.size.y <= 0 ) {
		return;
	}
	LOG( DEBUG, "Processing global area: ", rArea, " -> ", location_bounds );
	Point2i current_region_loc;
	for ( int y = location_bounds.position.y; y < location_bounds.get_end().y; y++ ) {
		current_region_loc.y = y;
		for ( int x = location_bounds.position.x; x < location_bounds.get_end().x; x++ ) {
			current_region_loc.x = x;
			const Terrain3DRegion *pRegion = getRegionPtr( current_region_loc );
			if ( pRegion && !pRegion->isDeleted() ) {
				if ( m_residency.cpuRegionCache().isEnabled() && !ensureCpuRegionResident( current_region_loc ) ) {
					LOG( ERROR, "Unable to page CPU terrain maps for region callback: ", current_region_loc );
					continue;
				}
				pRegion = getRegionPtr( current_region_loc );
				LOG( DEBUG, "Current region: ", current_region_loc );
				const Rect2i regionArea = rArea.intersection(
					Rect2i( current_region_loc * T3D_REGION_SIZE, Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ) ) );
				LOG( DEBUG,
					 "Region bounds: ",
					 Rect2i( current_region_loc * T3D_REGION_SIZE, Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ) ) );
				LOG( DEBUG, "Region area: ", regionArea );
				const Rect2i dstCoords( regionArea.position - rArea.position, regionArea.size );
				const Rect2i srcCoords( regionArea.position -
											( pRegion->getLocation() * Vector2i( T3D_REGION_SIZE, T3D_REGION_SIZE ) ),
										dstCoords.size );
				LOG( DEBUG, "src map coords: ", srcCoords );
				LOG( DEBUG, "dst map coords: ", dstCoords );
				rCallback.call( pRegion, srcCoords, dstCoords );
			}
		}
	}
}

void Terrain3DData::setRegionModified(
	const Vector2i &rRegionLoc,
	const bool		bModified ) {
	Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setModified( bModified );
}

bool Terrain3DData::isRegionModified(
	const Vector2i &rRegionLoc ) const {
	Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return false;
	}
	return pRegion->isModified();
}

void Terrain3DData::setRegionDeleted(
	const Vector2i &rRegionLoc,
	const bool		bDeleted ) {
	Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setDeleted( bDeleted );
}

bool Terrain3DData::isRegionDeleted(
	const Vector2i &rRegionLoc ) const {
	const Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return true;
	}
	return pRegion->isDeleted();
}

Ref< Terrain3DRegion > Terrain3DData::addRegionBlankp(
	const Vector3 &rGlobalPosition,
	const bool	   bUpdate ) {
	return addRegionBlank( getRegionLocation( rGlobalPosition ), bUpdate );
}

Ref< Terrain3DRegion > Terrain3DData::addRegionBlank(
	const Vector2i &rRegionLoc,
	const bool		bUpdate ) {
	Ref< Terrain3DRegion > pRegion;
	pRegion.instantiate();
	pRegion->setLocation( rRegionLoc );
	if ( addRegion( pRegion, bUpdate ) == OK ) {
		pRegion->setModified( true );
		return pRegion;
	}
	return Ref< Terrain3DRegion >();
}

/** Adds a Terrain3DRegion to the terrain
 * Marks region as modified
 *	p_update - rebuild the maps if true. Set to false if bulk adding many regions.
 */
Error Terrain3DData::addRegion(
	const Ref< Terrain3DRegion > &rRegion,
	const bool					  bUpdate ) {
	if ( rRegion.is_null() ) {
		return ERR_INVALID_PARAMETER;
	}
	return addRegionInternal( rRegion, bUpdate, true );
}

Error Terrain3DData::replaceRegionData(
	const Vector2i	 &rRegionLoc,
	const Dictionary &dictData ) {
	if ( !ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( ERROR, "Cannot load CPU map payload for replacement region: ", rRegionLoc );
		return ERR_CANT_OPEN;
	}
	Ref< Terrain3DRegion > pCurrent = getRegion( rRegionLoc );
	if ( pCurrent.is_null() || pCurrent->isDeleted() || getRegionId( rRegionLoc ) < 0 ) {
		LOG( ERROR, "Cannot replace data for inactive region: ", rRegionLoc );
		return ERR_DOES_NOT_EXIST;
	}

	// Mutate a detached deep copy first. If validation fails, the currently active
	// resource, render state, collision, and GPU layers remain untouched.
	Ref< Terrain3DRegion > pReplacement = pCurrent->duplicate( true );
	if ( pReplacement.is_null() ) {
		return ERR_CANT_CREATE;
	}
	pReplacement->setData( dictData );
	pReplacement->setLocation( rRegionLoc ); // Repository location is authoritative.
	pReplacement->setDeleted( false );
	pReplacement->setEdited( false );
	if ( !pReplacement->hasValidGeometryContractInternal() ) {
		LOG( ERROR, "Replacement region violates the fixed 128 samples x 1.0 metre geometry contract" );
		return ERR_INVALID_DATA;
	}
	pReplacement->sanitizeMaps();
	if ( !pReplacement->validateMapSize( pReplacement->getHeightMap() ) ||
		 ( pReplacement->getControlMap().is_valid() &&
		   !pReplacement->validateMapSize( pReplacement->getControlMap() ) ) ||
		 ( pReplacement->getColorMap().is_valid() && !pReplacement->validateMapSize( pReplacement->getColorMap() ) ) ) {
		LOG( ERROR, "Replacement snapshot contains invalid map dimensions" );
		return ERR_INVALID_DATA;
	}
	pReplacement->setModified( true );
	return addRegionInternal( pReplacement, true, false );
}

Error Terrain3DData::addRegionInternal(
	const Ref< Terrain3DRegion > &rRegion,
	const bool					  bUpdate,
	const bool					  bSanitize ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	if ( rRegion.is_null() || !rRegion->hasValidGeometryContractInternal() ) {
		LOG( ERROR, "Rejected region that violates the fixed 128 samples x 1.0 metre geometry contract" );
		return ERR_INVALID_DATA;
	}
	if ( rRegion.is_null() ) {
		LOG( ERROR, "Provided region is null. Returning" );
		return FAILED;
	}
	const Vector2i regionLoc = rRegion->getLocation();
	LOG( DEBUG, "Adding region at location ", regionLoc, ", update maps: ", bUpdate ? "yes" : "no" );

	if ( !isRegionLocationValidInternal( regionLoc ) ) {
		LOG( ERROR,
			 "Location ",
			 regionLoc,
			 " out of active bounds. Max: ",
			 -T3D_REGION_MAP_SIZE / 2,
			 " to ",
			 T3D_REGION_MAP_SIZE / 2 - 1 );
		return FAILED;
	}

	if ( bSanitize ) {
		rRegion->sanitizeMaps();
	}
	rRegion->setDeleted( false );

	int							 existingSlot = getRegionId( regionLoc );
	const Ref< Terrain3DRegion > pPriorRegion = m_regionRepository.getRegion( regionLoc );
	Terrain3DRegion *const		 pStoredRegion = pPriorRegion.ptr();
	const bool					 bHadPriorRegionResource = pStoredRegion != NULL;
	if ( existingSlot < 0 && pStoredRegion && !pStoredRegion->isDeleted() ) {
		existingSlot = m_regionRepository.findSlot( regionLoc );
	}
	const bool bReplacingExistingRegion = existingSlot >= 0;
	if ( !bReplacingExistingRegion ) {
		const int maximumRegions = T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE;
		if ( m_regionRepository.getActiveRegionCount() >= maximumRegions ) {
			LOG( ERROR, "Region slot capacity exceeded while adding region ", regionLoc );
			return ERR_OUT_OF_MEMORY;
		}
	}

	if ( !syncActiveRegion( regionLoc, rRegion.ptr() ) ) {
		if ( pStoredRegion && !pStoredRegion->isDeleted() ) {
			syncActiveRegion( regionLoc, pStoredRegion );
		}
		LOG( ERROR, "Unable to publish region ", regionLoc, " into instance-local active storage" );
		return ERR_INVALID_DATA;
	}

	const bool bRequiresInstanceReconciliation = bHadPriorRegionResource || rRegion->hasInstances();
	m_samplingStorage.invalidateResident( regionLoc );
	if ( !m_regionRepository.storeRegion( regionLoc, rRegion ) ) {
		if ( pPriorRegion.is_valid() && !pPriorRegion->isDeleted() ) {
			syncActiveRegion( regionLoc, pPriorRegion.ptr() );
		} else if ( m_samplingStorage.isActiveOwner() ) {
			m_samplingStorage.invalidateActive( regionLoc );
		}
		LOG( ERROR, "Region repository rejected region publication at ", regionLoc );
		return ERR_INVALID_DATA;
	}

    m_mapEditService.refreshControlTileClassesCold( regionLoc, rRegion.ptr() );

	// Replacing an active resource does not alter topology or texture-array layout.
	if ( bReplacingExistingRegion && bUpdate && canUpdateRegionSlotsIncrementally() ) {
		ensureDefaultMaps();
		m_heightMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_HEIGHT );
		m_controlMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_CONTROL );
		m_colorMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_COLOR );
		Ref< Image > pDerived = rRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( { regionLoc } );
			pDerived = rRegion->getDerivedNormalSlopeMap();
		}
		if ( pDerived.is_null() ) {
			pDerived = m_defaultDerivedMap;
		}
		m_derivedMaps[existingSlot] = pDerived;
		m_generatedHeightMaps.update( m_heightMaps[existingSlot], existingSlot );
		m_generatedControlMaps.update( makeGpuRenderControlImage( regionLoc ), existingSlot );
		m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[existingSlot] ), existingSlot );
		m_generatedDerivedMaps.update( pDerived, existingSlot );
		calcHeightRange();
		emitRegionLayoutChanged( true );
		m_pTerrain->instancerObject()->updateMmis( -1, regionLoc, false );
		invalidateSamplingCache();
		return OK;
	}

	if ( !bReplacingExistingRegion ) {
		const int newSlot = m_regionRepository.activateLocation( regionLoc, bUpdate );
		if ( newSlot < 0 ) {
			if ( pPriorRegion.is_valid() ) {
				( void ) m_regionRepository.storeRegion( regionLoc, pPriorRegion );
				if ( !pPriorRegion->isDeleted() ) {
					syncActiveRegion( regionLoc, pPriorRegion.ptr() );
				}
			} else {
				m_regionRepository.eraseRegionResource( regionLoc );
				if ( m_samplingStorage.isActiveOwner() ) {
					m_samplingStorage.invalidateActive( regionLoc );
				}
			}
			LOG( ERROR, "Region slot capacity exceeded while adding region ", regionLoc );
			return ERR_OUT_OF_MEMORY;
		}

		const bool bCanUploadSingleLayer = bUpdate && canUpdateRegionSlotsIncrementally();
		if ( bCanUploadSingleLayer ) {
			ensureDefaultMaps();
			m_heightMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_HEIGHT );
			m_controlMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_CONTROL );
			m_colorMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_COLOR );
			regenerateDerivedMaps( { regionLoc } );
			Ref< Image > pDerived = rRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() ) {
				pDerived = m_defaultDerivedMap;
			}
			m_derivedMaps[newSlot] = pDerived;
			m_generatedHeightMaps.update( m_heightMaps[newSlot], newSlot );
			m_generatedControlMaps.update( makeGpuRenderControlImage( regionLoc ), newSlot );
			m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[newSlot] ), newSlot );
			m_generatedDerivedMaps.update( pDerived, newSlot );
			updateMasterHeights( rRegion->getHeightRange() );
			emitRegionLayoutChanged( true );
			if ( bRequiresInstanceReconciliation ) {
				m_pTerrain->instancerObject()->updateMmis( -1, regionLoc, false );
			}
			invalidateSamplingCache();
#ifdef DEBUG_ENABLED
			if ( !m_regionRepository.validateInvariants() ) {
				LOG( ERROR, "Region repository invariant failure after incremental add" );
			}
#endif
			return OK;
		}
		m_regionRepository.markRegionMapDirty();
	}

	LOG( DEBUG,
		 "Storing region ",
		 regionLoc,
		 " version ",
		 vformat( "%.3f", rRegion->getVersion() ),
		 " active count: ",
		 m_regionRepository.getActiveRegionCount(),
		 ", slots: ",
		 m_regionRepository.getSlotCount() );
	if ( bUpdate ) {
		updateMaps( REGION_TYPE_MAX, true, false );
		if ( bRequiresInstanceReconciliation ) {
			m_pTerrain->instancerObject()->updateMmis( -1, regionLoc, false );
		}
	}
	invalidateSamplingCache();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after addRegionInternal" );
	}
#endif
	return OK;
}

void Terrain3DData::removeRegionp(
	const Vector3 &rGlobalPosition,
	const bool	   bUpdate ) {
	const Ref< Terrain3DRegion > pRegion = getRegion( getRegionLocation( rGlobalPosition ) );
	removeRegion( pRegion, bUpdate );
}

void Terrain3DData::removeRegionl(
	const Vector2i &rRegionLoc,
	const bool		bUpdate ) {
	const Ref< Terrain3DRegion > pRegion = getRegion( rRegionLoc );
	removeRegion( pRegion, bUpdate );
}

// Remove region marks the region for deletion, and removes it from the active arrays indexed by ID
// It remains stored in _regions and the file remains on disk until saved, when both are removed
void Terrain3DData::removeRegion(
	const Ref< Terrain3DRegion > &rRegion,
	const bool					  bUpdate ) {
	if ( rRegion.is_null() ) {
		LOG( ERROR, "Region not found or is null. Returning" );
		return;
	}

	const Vector2i regionLoc = rRegion->getLocation();
	const int	   regionSlot = m_regionRepository.findSlot( regionLoc );
	LOG( INFO, "Marking region ", regionLoc, " for deletion. update maps: ", bUpdate ? "yes" : "no" );
	if ( regionSlot < 0 ) {
		LOG( ERROR, "Region ", regionLoc, " not found in active region slots. Returning" );
		return;
	}
    m_mapEditService.invalidateControlTileClassesCold( regionLoc );
	m_samplingStorage.invalidateResident( regionLoc );
	if ( m_samplingStorage.isActiveOwner() ) {
		m_samplingStorage.invalidateActive( regionLoc );
	}
	invalidateGpuRegionLocation( regionLoc );
	invalidateCpuRegionLocation( regionLoc );
	invalidateInstanceRegionLocation( regionLoc );

	rRegion->setDeleted( true );
	m_sparseRasterTiles.removeRegion( regionLoc );
	m_sparseRasterHierarchy.removeRegion( regionLoc );
	m_bSparseHierarchyStale = true;
	const int sparseIndex = Terrain3DRegionRepository::getRegionMapIndex( regionLoc );
	if ( sparseIndex >= 0 ) {
		m_sparseDirtySavedRegions.set( ( size_t ) sparseIndex );
		m_sparseRegionInitialized.reset( ( size_t ) sparseIndex );
	}
	if ( m_regionRepository.deactivateLocation( regionLoc ) < 0 ) {
		LOG( ERROR, "Region repository failed to deactivate ", regionLoc );
		return;
	}
	markGpuFarRegionDirty( regionLoc, REGION_TYPE_MAX );
	calcHeightRange();

	invalidateSamplingCache();
	if ( bUpdate ) {
		emitRegionLayoutChanged( false );
		m_pTerrain->instancerObject()->destroyRegionMmis( regionLoc );
	}
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after removeRegion" );
	}
#endif
}

bool Terrain3DData::ensurePersistenceRegionResident(
	void *const		pContext,
	const Vector2i &rLocation ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	return pData && pData->ensureCpuRegionResidentInternal( rLocation, true );
}

// A partial or source-only save cannot publish a coherent multi-region bake.
// Invalidate the runtime manifest on disk so the previously baked pack cannot
// be mistaken for a snapshot of the newly saved .res sources.
void Terrain3DData::invalidateBakedWorldManifest(
	const String &rDataDirectory ) {
	if ( rDataDirectory.is_empty() ) {
		return;
	}
	const String manifestPath = rDataDirectory.trim_suffix( "/" ).path_join( "terrain3d_world.t3dmanifest" );
	if ( FileAccess::file_exists( manifestPath ) && DirAccess::remove_absolute( manifestPath ) != OK ) {
		LOG( WARN, "Unable to invalidate stale baked-world manifest: ", manifestPath );
	}
}

void Terrain3DData::publishSparseRasterEdit(
	const t3dMapType_t eType,
	const Vector2i	  &rLocation,
	const Rect2i	  &rDirty ) {
	Terrain3DRegion *const pRegion = getRegionPtr( rLocation );
	const int			   mapIndex = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( !pRegion || pRegion->isDeleted() || mapIndex < 0 ) {
		return;
	}
	// Preserve the immutable index and pinned decoded tiles for *other* regions.
	// The edited region is excluded from cold reads until a coherent save.
	m_sparseDirtySavedRegions.set( ( size_t ) mapIndex );
	m_bSparseHierarchyStale = true;
	if ( !m_sparseRegionInitialized.test( ( size_t ) mapIndex ) ) {
		// Untracked legacy mutable Image aliases can exist. On the first commit,
		// ingest all channels; subsequent commits touch only intersecting 32x32 tiles.
		for ( int channel = TYPE_HEIGHT; channel < REGION_TYPE_MAX; ++channel ) {
			m_sparseRasterTiles.updateRegion( rLocation,
											  ( t3dMapType_t ) channel,
											  pRegion->getMap( ( t3dMapType_t ) channel ),
											  Rect2i() );
		}
		m_sparseRegionInitialized.set( ( size_t ) mapIndex );
	} else {
		m_sparseRasterTiles.updateRegion( rLocation, eType, pRegion->getMap( eType ), rDirty );
	}
	// Coarse hierarchy is generated on explicit save/cook, never during brush strokes.
}

bool Terrain3DData::rebuildSparseRasterSnapshotCold(
	void ) {
	Terrain3DRasterTileStore		snapshot;
	Terrain3DRasterHierarchyBuilder hierarchy;
	const t3dRegionCoord_t *const	pSlots = m_regionRepository.getSlotsData();
	for ( int i = 0; i < m_regionRepository.getSlotCount(); ++i ) {
		const Vector2i location = t3dUnpackRegionCoord( pSlots[i] );
		if ( location == s_v2iMax ) {
			continue;
		}
		Terrain3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( !ensurePersistenceRegionResident( this, location ) ) {
			return false;
		}
		for ( int channel = TYPE_HEIGHT; channel < REGION_TYPE_MAX; ++channel ) {
			const t3dMapType_t type = ( t3dMapType_t ) channel;
			const Ref< Image > source = pRegion->getMap( type );
			if ( source.is_null() && ( type == TYPE_HEIGHT || pRegion->hasChannel( type ) ) ) {
				return false; // do not publish a silently incomplete source bundle
			}
			if ( !snapshot.updateRegion( location, type, source, Rect2i() ) ) {
				return false;
			}
		}
		hierarchy.updateRegion( location, snapshot );
	}
	m_sparseRasterTiles = eastl::move( snapshot );
	m_sparseRasterHierarchy = eastl::move( hierarchy );
	m_sparseRegionInitialized.reset();
	for ( int i = 0; i < m_regionRepository.getSlotCount(); ++i ) {
		const Vector2i location = t3dUnpackRegionCoord( pSlots[i] );
		const int	   index = location == s_v2iMax ? -1 : Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( index >= 0 ) {
			m_sparseRegionInitialized.set( ( size_t ) index );
		}
	}
	return true;
}

Dictionary Terrain3DData::getSparseRasterStats(
	void ) const {
	Dictionary		 stats = m_sparseRasterTiles.getStats();
	const Dictionary hierarchyStats = m_sparseRasterHierarchy.getStats();
	const Array		 keys = hierarchyStats.keys();
	for ( int i = 0; i < keys.size(); ++i ) {
		stats[keys[i]] = hierarchyStats[keys[i]];
	}
	stats["raster_bundle_loaded"] = m_bSparseBundleLoaded;
	stats["raster_saved_regions_dirty"] = ( int64_t ) m_sparseDirtySavedRegions.count();
	stats["raster_hierarchy_stale"] = m_bSparseHierarchyStale;
	stats["raster_bundle_index_entries"] = m_sparseRasterIndex.getEntryCount();
	const Dictionary rasterIo = m_sparseRasterIndex.getSourceIoStats();
	const Array		 rasterIoKeys = rasterIo.keys();
	for ( int i = 0; i < rasterIoKeys.size(); ++i ) {
		const Variant key = rasterIoKeys[i];
		stats[key] = rasterIo[key];
	}
	stats["raster_hierarchy_index_entries"] = m_sparseHierarchyIndex.getEntryCount();
	const Dictionary tileStats = m_sparseTileResidency.getStats();
	const Array		 tileKeys = tileStats.keys();
	for ( int i = 0; i < tileKeys.size(); ++i ) {
		stats[tileKeys[i]] = tileStats[tileKeys[i]];
	}
	return stats;
}

void Terrain3DData::saveDirectory(
	const String &rDir ) {
	if ( !m_pTerrain ) {
		LOG( ERROR, "Cannot save terrain data without an attached Terrain3D owner" );
		return;
	}
	LOG( INFO, "Saving data files to ", rDir );
	const bool					   cook_on_save = m_pTerrain && m_pTerrain->isGpuRegionCookOnSave();
	const t3dDirectorySaveResult_t saveResult =
		m_regionPersistence.saveDirectory( rDir,
										   m_pTerrain->getSave16Bit(),
										   &Terrain3DData::ensurePersistenceRegionResident,
										   this );
	bool sourceChanged = false;
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( saveResult.m_regions ).size() ); iRegion++ ) {
		const t3dRegionSaveResult_t &regionResult = ( saveResult.m_regions )[iRegion];
		sourceChanged |= regionResult.m_bSourceChanged || regionResult.m_bDeleted;
	}
	if ( sourceChanged ) {
		( void ) m_runtimeArtifacts.pageStore().setSharedSourcePackage( String() );
		invalidateBakedWorldManifest( rDir );
	}
	if ( sourceChanged && saveResult.m_nFailed == 0 && rebuildSparseRasterSnapshotCold() ) {
		const Error sparseError = Terrain3DRasterBundle::save( rDir, m_sparseRasterTiles );
		if ( sparseError == OK ) {
			const Error hierarchyError = Terrain3DRasterHierarchyBundle::save( rDir, m_sparseRasterHierarchy );
			if ( hierarchyError == OK ) {
				if ( m_sparseHierarchyIndex.open( rDir ) == OK ) {
					// Parent rasters are immutable cooked data. Release the entire
					// temporary quadtree instead of retaining up to 13 MiB of
					// duplicated parent payloads between explicit saves.
					m_sparseRasterHierarchy.clear();
				}
			} else {
				m_sparseHierarchyIndex.clear();
				LOG( WARN, "Could not publish derived terrain LOD hierarchy bundle: ", hierarchyError );
			}
			m_sparseRasterTiles.acknowledgeSaved();
			m_bSparseBundleLoaded = m_sparseRasterIndex.open( rDir ) == OK;
			m_sparseTileResidency.reset();
			if ( m_bSparseBundleLoaded ) {
				m_sparseDirtySavedRegions.reset();
				m_bSparseHierarchyStale = m_sparseHierarchyIndex.getEntryCount() == 0u;
			}
			if ( m_bSparseBundleLoaded ) {
				// Authoring images remain canonical; free the redundant derived leaf
				// bytes once the immutable bundle index is published successfully.
				m_sparseRasterTiles.clear();
				m_sparseRegionInitialized.reset();
			}
		} else {
			LOG( WARN, "Could not publish derived sparse terrain raster bundle: ", sparseError );
		}
	}
	if ( cook_on_save ) {
#if defined( T3D_EDITOR_NATIVE )
		const Dictionary baked = cookGpuRegionPages();
		if ( !( bool ) baked.get( "complete", false ) ) {
			LOG( ERROR, "Authored regions saved, but packed baked terrain generation failed" );
		}
#endif
	}
	// Persistence updates only the exact resource paths Terrain3D changed. Do not
	// force a project-wide EditorFileSystem scan from a scene-save callback.
}

void Terrain3DData::saveRegion(
	const Vector2i &rRegionLoc,
	const String   &rDir,
	const bool		b16Bit ) {
	const t3dRegionSaveResult_t result =
		m_regionPersistence.saveRegion( rRegionLoc,
										rDir,
										b16Bit,
										&Terrain3DData::ensurePersistenceRegionResident,
										this );
	if ( result.m_bSourceChanged || result.m_bDeleted ) {
		( void ) m_runtimeArtifacts.pageStore().setSharedSourcePackage( String() );
		invalidateBakedWorldManifest( rDir );
	}
	if ( result.succeeded() && (result.m_bSourceChanged || result.m_bDeleted) ) {
		// Saving one region cannot publish a coherent world snapshot when other
		// regions have unsaved authoring edits. Invalidate the derived bundle;
		// full directory save recreates it transactionally from saved sources.
		m_sparseRasterIndex.clear();
		m_sparseHierarchyIndex.clear();
		m_sparseTileResidency.reset();
		m_bSparseBundleLoaded = false;
		m_bSparseHierarchyStale = true;
		m_sparseDirtySavedRegions.reset();
		const String hierarchyPath = Terrain3DRasterHierarchyBundle::getPath( rDir );
		if ( FileAccess::file_exists( hierarchyPath ) ) {
			DirAccess::remove_absolute( hierarchyPath );
		}
		const String rasterPath = Terrain3DRasterBundle::getPath( rDir );
		if ( FileAccess::file_exists( rasterPath ) ) {
			const Error removeError = DirAccess::remove_absolute( rasterPath );
			if ( removeError != OK ) {
				LOG( WARN, "Could not invalidate outdated sparse raster bundle: ", rasterPath );
			}
		}
	}
	// Region-only saves leave the previous baked world invalidated. A full
	// save or explicit cook_gpu_region_pages is required to republish it.
}

bool Terrain3DData::collectGpuRegionPageSources(
	String											   &rDataDirectory,
	PackedStringArray								   &rSourceFiles,
	eastl::vector< Vector2i, Terrain3DEastlAllocator > &rLocations,
	PackedStringArray								   &rErrors ) const {
	rDataDirectory = m_pTerrain ? m_pTerrain->getDataDirectory().trim_suffix( "/" ) : String();
	if ( !m_pTerrain ) {
		rSourceFiles.clear();
		rLocations.clear();
		rErrors.clear();
		rErrors.push_back( "Terrain3D owner is unavailable" );
		return false;
	}
	return m_runtimeArtifacts.collectPageSources( rDataDirectory, rSourceFiles, rLocations, rErrors );
}

Dictionary Terrain3DData::inspectGpuRegionPages(
	void ) {
	Dictionary					 result;
	const String				 dataDir = m_pTerrain ? m_pTerrain->getDataDirectory().trim_suffix( "/" ) : String();
	const String				 packPath = dataDir.path_join( "terrain_stream.t3sp" );
	Terrain3DSharedStreamPack	 pack;
	const bool					 ready = !dataDir.is_empty() && pack.open( packPath );
	int							 valid = 0;
	int							 missing = 0;
	PackedStringArray			 needRebake;
	const TypedArray< Vector2i > locations = m_regionRepository.getActiveLocations();
	for ( int i = 0; i < locations.size(); ++i ) {
		const Vector2i		   location = locations[i];
		Terrain3DRegion *const region = getRegionPtr( location );
		if ( !region || region->isDeleted() ) {
			continue;
		}
		PackedByteArray metadata;
		uint64_t		length = 0u;
		if ( ready && pack.lookupPage( getGpuRuntimePagePath( region ), metadata, length ) &&
			 length >= T3D_RUNTIME_PAGE_HEADER_BYTES ) {
			++valid;
		} else {
			++missing;
			needRebake.push_back( String( location ) );
		}
	}
	result["data_directory"] = dataDir;
	result["package_path"] = packPath;
	result["package_ready"] = ready;
	result["discovered"] = valid + missing;
	result["current"] = valid;
	result["missing"] = missing;
	result["needs_cook"] = missing;
	result["needs_cook_regions"] = needRebake;
	result["complete"] = ready && missing == 0;
	return result;
}

Error Terrain3DData::prepareDirectoryCandidateCold(
	const String		  &rDir,
	t3dStagedRepository_s &rStaged,
	bool				  &rBoundedCpuLoad,
	bool				  &rCookedRuntimeManifest ) {
	Terrain3DColdPhaseScope cold_scope;
	rStaged.reset();
	rBoundedCpuLoad = m_residency.cpuRegionCache().isEnabled();
	LOG( INFO,
		 "[terrain-data] Candidate repository prepare; CPU cache ",
		 rBoundedCpuLoad ? "bounded" : "authoring/unbounded",
		 "; GPU cache ",
		 m_residency.gpuRegionCache().isEnabled() ? "enabled" : "disabled" );
	const t3dRuntimePageConfig_s runtime_page_config = getRuntimePageConfig();
	m_residency.cpuRegionCache().setRuntimePageConfig( runtime_page_config );
	rCookedRuntimeManifest =
		rBoundedCpuLoad && runtime_page_config.m_bCookedPagesEnabled && !runtime_page_config.m_bEditorAuthoring;
	if ( rCookedRuntimeManifest ) {
		PackedStringArray manifest_errors;
		m_startupPreparedWorldManifest.reset();
		if ( !m_startupPreparedWorldManifest.load( rDir, &manifest_errors ) ||
			 !m_startupPreparedWorldManifest.stageRepository( rDir, rStaged ) ) {
			for ( size_t iError = 0u; iError < ( size_t ) ( ( manifest_errors ).size() ); iError++ ) {
				LOG( ERROR, ( manifest_errors )[iError] );
			}
			LOG( ERROR, "Cooked runtime profile requires a valid terrain3d_world.t3dmanifest." );
			return ERR_FILE_CORRUPT;
		}
		LOG( INFO,
			 "Prepared cooked world manifest with ",
			 rStaged.getRegionCount(),
			 " regions; live repository remains untouched." );
		return OK;
	}
	return m_regionPersistence.stageDirectory( rDir, rBoundedCpuLoad, rStaged );
}

Error Terrain3DData::commitDirectoryCandidateCold(
	const String		  &rDir,
	t3dStagedRepository_s &rStaged,
	const bool			   bBoundedCpuLoad,
	const bool			   bCookedRuntimeManifest,
	const bool			   bFinalizeDerived ) {
	Terrain3DColdPhaseScope cold_scope;
	LOG( DEBUG, "[terrain-data] Phase 3/4: committing active storage and region ownership" );
	m_regionPersistence.beginPublication();
	m_samplingStorage.resetResidentPool();
	bool activeStorageReady = false;
	if ( bBoundedCpuLoad ) {
		eastl::fixed_vector< const uint8_t *, T3D_REGION_SLOTS, false > staged_height_pages;
		eastl::fixed_vector< int, T3D_REGION_SLOTS, false >				staged_height_slots;
		if ( !bCookedRuntimeManifest ) {
			for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( rStaged.m_regions ).size() ); iRegion++ ) {
				const t3dStagedRegion_t &entry = ( rStaged.m_regions )[iRegion];
				if ( entry.m_nActiveSlot < 0 || entry.m_stagedHeight.is_empty() ) {
					LOG( ERROR, "Missing staged height payload for region ", entry.m_location );
					m_regionPersistence.finishPublication( 0 );
					m_regionPersistence.finishDirectoryLoad( false );
					return ERR_FILE_CORRUPT;
				}
				staged_height_pages.push_back( entry.m_stagedHeight.ptr() );
				staged_height_slots.push_back( entry.m_nActiveSlot );
			}
		}
		activeStorageReady = m_samplingStorage.replaceActiveHeightPages( staged_height_pages.data(),
																		 staged_height_slots.data(),
																		 staged_height_pages.size() );
		if ( activeStorageReady ) {
			activeStorageReady = setRegionSizeInternal();
		}
	} else {
		if ( !setRegionSizeInternal() ) {
			m_regionPersistence.finishPublication( 0 );
			m_regionPersistence.finishDirectoryLoad( false );
			return ERR_ALREADY_IN_USE;
		}
		m_samplingStorage.resetActive();
		activeStorageReady = true;
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( rStaged.m_regions ).size() ); iRegion++ ) {
			const t3dStagedRegion_t &entry = ( rStaged.m_regions )[iRegion];
			if ( !syncActiveRegion( entry.m_location, entry.m_region.ptr() ) ) {
				activeStorageReady = false;
				break;
			}
		}
	}
	if ( !activeStorageReady ) {
		if ( !bBoundedCpuLoad ) {
			Terrain3DRegion *const *const ppRegions = m_regionRepository.getRegionPtrMapData();
			for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
				Terrain3DRegion *const pRegion = ppRegions[i];
				if ( pRegion && !pRegion->isDeleted() ) {
					syncActiveRegion( pRegion->getLocation(), pRegion );
				}
			}
		}
		m_regionPersistence.finishPublication( 0 );
		m_regionPersistence.finishDirectoryLoad( false );
		LOG( ERROR, "Failed to populate instance-local active terrain storage; previous repository restored" );
		return ERR_INVALID_DATA;
	}

	if ( bCookedRuntimeManifest ) {
		m_runtimeArtifacts.worldManifest() = m_startupPreparedWorldManifest;
	} else {
		m_runtimeArtifacts.worldManifest().reset();
	}
	m_regionRepository.replaceAll( rStaged.m_resources,
								   rStaged.m_pRegionPtrs.data(),
								   rStaged.m_regionLocations.data(),
								   rStaged.getRegionCount(),
								   rStaged.m_logicalRegionMap.data() );
	m_regionRepository.rebuildRegionMapFromSlots();
	m_regionRepository.setLoaded( true );
	// Bind the immutable cooked indices before derived map and GPU-cache
	// publication. Both ordinary directory loads and staged startup commits use
	// this entry point, so neither can bypass sparse cold-source recovery.
	m_sparseRasterTiles.clear();
	m_sparseRasterHierarchy.clear();
	m_sparseRegionInitialized.reset();
	m_sparseDirtySavedRegions.reset();
	m_sparseRasterIndex.clear();
	m_sparseHierarchyIndex.clear();
	m_sparseTileResidency.reset();
	m_bSparseBundleLoaded = m_sparseRasterIndex.open( rDir ) == OK;
	if ( m_bSparseBundleLoaded ) {
		m_sparseHierarchyIndex.open( rDir );
		m_sparseTileResidency.configureCold( 64u );
	}
	m_bSparseHierarchyStale = !m_bSparseBundleLoaded || m_sparseHierarchyIndex.getEntryCount() == 0u;
	m_samplingStorage.refreshActiveGeneration();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after loadDirectory publication" );
	}
#endif
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_defaultHeightMap.unref();
	m_defaultControlMap.unref();
	m_defaultColorMap.unref();
	m_defaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_mapEditService.reset();
	m_generatedHeightMaps.clear();
	m_generatedTerrainShadowMasks.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();
	if ( !bCookedRuntimeManifest ) {
		m_regionPersistence.publishStagedPaths( rStaged );
	}
	m_regionPersistence.finishPublication( m_regionRepository.getActiveRegionCount() );
	LOG( DEBUG,
		 "[terrain-data] Phase 3/4 complete: ",
		 m_regionRepository.getActiveRegionCount(),
		 " active regions committed; render-derived publication remains separate" );
	if ( bFinalizeDerived ) {
		finalizeDirectoryCandidateCold( bCookedRuntimeManifest );
	}
	( void ) rDir;
	return OK;
}

void Terrain3DData::finalizeDirectoryCandidateCold(
	const bool bCookedRuntimeManifest ) {
	LOG( DEBUG, "[terrain-data] Phase 4/4: cache policy and render-derived map publication" );
	m_regionPersistence.beginFinalize();
	configureResidentPool( 64 );
	configureCpuRegionCache( m_pTerrain->isCpuRegionCacheEffective(),
							 m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(),
							 m_pTerrain->getCpuRegionLoadBudgetLayers(),
							 m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
								  m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(),
								  m_pTerrain->getInstanceRegionLoadBudgetPages(),
								  m_pTerrain->getInstanceRegionLoadBudgetMb() );
	if ( m_residency.cpuRegionCache().isEnabled() ) {
		updateCpuRegionResidency( Vector3(), true );
	}
	updateMaps( REGION_TYPE_MAX, true, false );
	invalidateSamplingCache();
	if ( m_pTerrain->isInitialized() && m_pTerrain->instancerObject() ) {
		m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, true );
	}
	m_regionPersistence.finishDirectoryLoad( true );
	const t3dStartupLoadStats_t &loadStats = m_regionPersistence.getStartupLoadStats();
	const double				 elapsedMs = ( double ) ( loadStats.m_nTotalUsecLast ) / 1000.0;
	LOG( INFO,
		 "Loaded ",
		 m_regionRepository.getActiveRegionCount(),
		 " terrain regions (",
		 m_regionRepository.getSlotCount(),
		 " texture layers) in ",
		 vformat( "%.2f", elapsedMs ),
		 " ms; synchronous Resource deserialize ",
		 vformat( "%.2f", ( double ) ( loadStats.m_nDeserializeUsecLast ) / 1000.0 ),
		 " ms",
		 bCookedRuntimeManifest ? "; cooked manifest startup" : "" );
}

Error Terrain3DData::loadDirectory(
	const String &rDir ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr );
	Terrain3DColdPhaseScope		cold_scope;
	t3dStagedRepository_s		staged;
	bool						bBoundedCpuLoad = false;
	bool						bCookedRuntimeManifest = false;
	const Error prepare_error = prepareDirectoryCandidateCold( rDir, staged, bBoundedCpuLoad, bCookedRuntimeManifest );
	if ( prepare_error != OK ) {
		return prepare_error;
	}
	const Error result = commitDirectoryCandidateCold( rDir, staged, bBoundedCpuLoad, bCookedRuntimeManifest, true );
	if ( result != OK ) {
		return result;
	}
	// The staged repository commit already installed both indices and the
	// fixed decoded-tile cache before publishing derived maps.

	return OK;
}

// Loads a region from an explicit directory through the persistence layer.
void Terrain3DData::loadRegion(
	const Vector2i &rRegionLoc,
	const String   &rDir,
	const bool		bUpdate ) {
	const t3dRegionLoadResult_t result = m_regionPersistence.loadRegion( rRegionLoc, rDir );
	if ( !result.succeeded() ) {
		return;
	}
	const Ref< Terrain3DRegion > pRegion = result.m_region;
	if ( addRegionInternal( pRegion, bUpdate, true ) == OK ) {
		m_regionPersistence.publishLoadedRegionPath( result );
		pRegion->setModified( result.m_bStorageUpgrade || pRegion->isModified() );
		pRegion->setEdited( false );
		m_residency.cpuRegionCache().markSelectionDirty();
	}
}

bool Terrain3DData::refreshTerrainShadowMasksInternal(
	void ) {
	if ( m_residency.gpuRegionCache().isEnabled() ) {
		return m_residency.gpuRegionCache().refreshTerrainShadowMasks();
	}
	const int slot_count = m_regionRepository.getSlotCount();
	if ( slot_count <= 0 ) {
		m_generatedTerrainShadowMasks.clear();
		return true;
	}
	TypedArray< Image > shadow_masks;
	shadow_masks.resize( slot_count );
	for ( int slot = 0; slot < slot_count; ++slot ) {
		const Vector2i				 location = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[slot] );
		const Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : getRegionPtr( location );
		const Ref< Image >			 pShadowMask =
			makeTerrainShadowMaskImage( pRegion && !pRegion->isDeleted() ? pRegion : NULL );
		if ( pShadowMask.is_null() ) {
			return false;
		}
		shadow_masks[slot] = pShadowMask;
	}
	if ( m_generatedTerrainShadowMasks.getRid().is_valid() && m_generatedTerrainShadowMasks.size() == slot_count ) {
		for ( int slot = 0; slot < slot_count; ++slot ) {
			if ( !m_generatedTerrainShadowMasks.update( shadow_masks[slot], slot ) ) {
				return false;
			}
		}
		return true;
	}
	return m_generatedTerrainShadowMasks.create( shadow_masks, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
}

TypedArray< Image > Terrain3DData::getMaps(
	const t3dMapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Specified map type out of range" );
		return TypedArray< Image >();
	}
	switch ( eMapType ) {
	case TYPE_HEIGHT:
		return getHeightMaps();
		break;
	case TYPE_CONTROL:
		return getControlMaps();
		break;
	case TYPE_COLOR:
		return getColorMaps();
		break;
	default:
		break;
	}
	return TypedArray< Image >();
}

void Terrain3DData::updateMaps(
	const t3dMapType_t eMapType,
	const bool		   bAllRegions,
	const bool		   bGenerateMipmaps ) {
	( void ) bGenerateMipmaps; // GPU publication builds transient mip chains; CPU maps stay base-level only.
	if ( isFarAtlasRequired() ) {
		markAllGpuFarRegionsDirty( eMapType );
	}
	if ( m_residency.gpuRegionCache().isEnabled() ) {
		updateMapsBounded( eMapType, bAllRegions );
		return;
	}

	const bool bImplicitStateChanged = refreshImplicitMapState();
	if ( bImplicitStateChanged ) {
		configureResidentPool( 64 );
	}

	// A full rebuild also provisions spare texture-array layers. The spare layers
	// are the key to constant-time add/remove operations between rebuilds.
	const bool bSkipReserve = bAllRegions && m_bSkipSlotReserveOnce;
	const bool bSlotCapacityChanged =
		bAllRegions && !bSkipReserve && m_regionRepository.ensureSlotReserve( getRegionSlotReserve() );
	if ( bSkipReserve ) {
		m_bSkipSlotReserveOnce = false;
	}
	if ( bAllRegions ) {
		LOG( EXTREME, "Marking dirty maps of type: ", eMapType );
		if ( bSlotCapacityChanged || eMapType == REGION_TYPE_MAX ) {
			m_generatedHeightMaps.clear();
			m_generatedTerrainShadowMasks.clear();
			m_generatedControlMaps.clear();
			m_generatedColorMaps.clear();
			m_generatedDerivedMaps.clear();
		} else {
			switch ( eMapType ) {
			case TYPE_HEIGHT:
				m_generatedHeightMaps.clear();
				m_generatedTerrainShadowMasks.clear();
				m_generatedDerivedMaps.clear();
				break;
			case TYPE_CONTROL:
				m_generatedControlMaps.clear();
				break;
			case TYPE_COLOR:
				m_generatedColorMaps.clear();
				break;
			default:
				break;
			}
		}
	}

	bool bAnyChanged = bImplicitStateChanged;

	if ( m_regionRepository.isRegionMapDirty() ) {
		LOG( EXTREME, "Regenerating ", V2I( T3D_REGION_MAP_SIZE ), " active region map from fixed slot storage" );
		m_regionRepository.rebuildRegionMapFromSlots();
		bAnyChanged = true;
		if ( !m_bStartupBulkLoad ) {
			LOG( DEBUG, "Emitting region_map_changed" );
			emit_signal( "region_map_changed" );
		}
	}

	const bool bRebuildHeight = m_generatedHeightMaps.isDirty();
	const bool bRebuildControl = m_generatedControlMaps.isDirty();
	const bool bRebuildColor = m_generatedColorMaps.isDirty();
	const bool bRebuildDerived = m_generatedDerivedMaps.isDirty() || bRebuildHeight;

	// Full/startup rebuilds used to traverse the region slots once per map and
	// once again for the height range. Resolve each region once and populate all
	// requested arrays in a single pass; GPU array creation remains separate.
	if ( bRebuildHeight || bRebuildControl || bRebuildColor || bRebuildDerived ) {
		ensureDefaultMaps();
		const int			slot_count = m_regionRepository.getSlotCount();
		TypedArray< Image > gpu_control_maps;
		TypedArray< Image > gpu_color_maps;
		TypedArray< Image > gpu_shadow_masks;
		gpu_shadow_masks.resize( slot_count );
		if ( bRebuildHeight ) {
			m_heightMaps.resize( slot_count );
			m_mapEditService.resetHeightRange();
		}
		if ( bRebuildControl ) {
			m_controlMaps.resize( slot_count );
			gpu_control_maps.resize( slot_count );
		}
		if ( bRebuildColor ) {
			m_colorMaps.resize( slot_count );
			gpu_color_maps.resize( slot_count );
		}
		if ( bRebuildDerived ) {
			m_derivedMaps.resize( slot_count );
			eastl::vector< Vector2i, Terrain3DEastlAllocator > missing;
			missing.reserve( ( size_t ) ( m_regionRepository.getActiveRegionCount() ) );
			for ( int slot = 0; slot < slot_count; ++slot ) {
				const Vector2i		   location = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[slot] );
				Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : getRegionPtr( location );
				if ( pRegion && !pRegion->isDeleted() && !pRegion->hasValidDerivedNormalSlopeMap() ) {
					missing.push_back( location );
				}
			}
			regenerateDerivedMaps( missing );
		}

		for ( int slot = 0; slot < slot_count; ++slot ) {
			const Vector2i		   location = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[slot] );
			const Terrain3DRegion *pRegion = location == s_v2iMax ? NULL : getRegionPtr( location );
			const bool			   bValid = pRegion && !pRegion->isDeleted();
			if ( bRebuildHeight ) {
				m_heightMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : NULL, TYPE_HEIGHT );
				if ( bValid ) {
					updateMasterHeights( pRegion->getHeightRange() );
				}
			}
			if ( bRebuildControl ) {
				m_controlMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : NULL, TYPE_CONTROL );
				gpu_control_maps[slot] = bValid ? makeGpuRenderControlImage( location ) : m_defaultControlMap;
			}
			if ( bRebuildColor ) {
				m_colorMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : NULL, TYPE_COLOR );
				gpu_color_maps[slot] = makeGpuColorImage( m_colorMaps[slot] );
			}
			const Ref< Image > pShadowMask = makeTerrainShadowMaskImage( bValid ? pRegion : NULL );
			gpu_shadow_masks[slot] = pShadowMask;
			if ( pShadowMask.is_null() ) {
				LOG( ERROR, "Failed to prepare terrain shadowmask layer for region ", location );
				return;
			}
			if ( bRebuildDerived ) {
				const Ref< Image > pDerived = bValid ? pRegion->getDerivedNormalSlopeMap() : Ref< Image >();
				m_derivedMaps[slot] = pDerived.is_valid() ? pDerived : m_defaultDerivedMap;
			}
		}

		m_generatedTerrainShadowMasks.create( gpu_shadow_masks, T3D_RD_TEXTURE_LINEAR_DATA );
		if ( bRebuildHeight ) {
			m_generatedHeightMaps.create( m_heightMaps, T3D_RD_TEXTURE_LINEAR_DATA );
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "height_maps_changed" );
			}
		}
		if ( bRebuildControl ) {
			if ( m_bControlMapsImplicit ) {
				const TypedArray< Image > implicit_control = { m_defaultControlMap };
				m_generatedControlMaps.create( implicit_control, T3D_RD_TEXTURE_LINEAR_DATA );
			} else {
				m_generatedControlMaps.create( gpu_control_maps, T3D_RD_TEXTURE_LINEAR_DATA );
			}
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "control_maps_changed" );
			}
		}
		if ( bRebuildColor ) {
			if ( m_bColorMapsImplicit ) {
				const TypedArray< Image > implicit_color = { makeGpuColorImage( m_defaultColorMap ) };
				m_generatedColorMaps.create( implicit_color, T3D_RD_TEXTURE_SOURCE_COLOR );
			} else {
				m_generatedColorMaps.create( gpu_color_maps, T3D_RD_TEXTURE_SOURCE_COLOR );
			}
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "color_maps_changed" );
			}
		}
		if ( bRebuildDerived ) {
			m_generatedDerivedMaps.create( m_derivedMaps, T3D_RD_TEXTURE_LINEAR_DATA );
			bAnyChanged = true;
			if ( !IS_EDITOR ) {
				for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
					const Vector2i		   location = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[slot] );
					Terrain3DRegion *const pRegion = location == s_v2iMax ? NULL : getRegionPtr( location );
					if ( !pRegion || pRegion->isDeleted() ) {
						continue;
					}
					m_samplingStorage.syncResidentDerived( location, pRegion );
					pRegion->setDerivedNormalSlopeMapInternal( Ref< Image >() );
				}
				m_derivedMaps.clear();
				m_derivedMaps.resize( slot_count );
			}
		}
	}

	// If no map has been rebuilt, update only edited layers. Emit once per map
	// type rather than once per region to avoid multiplying downstream work.
	if ( !bAnyChanged ) {
		bool bHeightChanged = false;
		bool bControlChanged = false;
		bool bColorChanged = false;
		for ( int region_id = 0; region_id < m_regionRepository.getSlotCount(); ++region_id ) {
			const Vector2i region_loc = t3dUnpackRegionCoord( m_regionRepository.getSlotsData()[region_id] );
			if ( region_loc == s_v2iMax ) {
				continue;
			}
			const Terrain3DRegion *pRegion = getRegionPtr( region_loc );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->isEdited() ) {
				continue;
			}
			switch ( eMapType ) {
			case TYPE_HEIGHT:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), region_id );
				bHeightChanged = true;
				break;
			case TYPE_CONTROL:
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
				bControlChanged = true;
				break;
			case TYPE_COLOR:
				m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ),
											 region_id );
				bColorChanged = true;
				break;
			default:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), region_id );
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
				m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ),
											 region_id );
				bHeightChanged = true;
				bControlChanged = true;
				bColorChanged = true;
				break;
			}
		}
		if ( !m_bStartupBulkLoad ) {
			if ( bHeightChanged ) {
				emit_signal( "height_maps_changed" );
			}
			if ( bControlChanged ) {
				emit_signal( "control_maps_changed" );
			}
			if ( bColorChanged ) {
				emit_signal( "color_maps_changed" );
			}
		}
	}

	if ( !m_residency.gpuRegionCache().isEnabled() && isFarAtlasRequired() ) {
		ensureFarAtlases();
	}

	if ( bAnyChanged && !m_bStartupBulkLoad ) {
		LOG( DEBUG, "Emitting maps_changed" );
		emit_signal( "maps_changed" );
		m_pTerrain->snap();
	}
}

void Terrain3DData::updateMapRegions(
	const t3dMapType_t										  eMapType,
	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations ) {
	if ( rRegionLocations.empty() ) {
		return;
	}

	if ( refreshImplicitMapState() ) {
		updateMaps( eMapType, true, false );
		return;
	}

	eastl::vector< Vector2i, Terrain3DEastlAllocator > render_locations = rRegionLocations;
	if ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX ) {
		// The compiled render-control field uses a two-texel source halo. A control
		// edit can therefore change the rendered transition on any immediate
		// neighboring region even though only the authored owner map was modified.
		for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( rRegionLocations ).size() ); iRegion++ ) {
			const Vector2i &source_location = ( rRegionLocations )[iRegion];
			for ( int y = -1; y <= 1; ++y ) {
				for ( int x = -1; x <= 1; ++x ) {
					const Vector2i dependency_location = source_location + Vector2i( x, y );
					if ( dependency_location == source_location ) {
						continue;
					}
					Terrain3DRegion *const pDependencyRegion = getRegionPtr( dependency_location );
					if ( !pDependencyRegion || pDependencyRegion->isDeleted() ) {
						continue;
					}
					bool bAlreadyPresent = false;
					for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( render_locations ).size() ); iLocation++ ) {
						const Vector2i &existing_location = ( render_locations )[iLocation];
						if ( existing_location == dependency_location ) {
							bAlreadyPresent = true;
							break;
						}
					}
					if ( !bAlreadyPresent ) {
						render_locations.push_back( dependency_location );
					}
				}
			}
		}
	}

	bool bHeightChanged = false;
	bool bControlChanged = false;
	bool bColorChanged = false;
	bool bPhysicalMappingChanged = false;
	for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( render_locations ).size() ); iLocation++ ) {
		const Vector2i region_loc = ( render_locations )[iLocation];
		if ( isFarAtlasRequired() ) {
			markGpuFarRegionDirty( region_loc, eMapType );
		}
		Terrain3DRegion *const pRegion = getRegionPtr( region_loc );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const int repository_slot = getRegionId( region_loc );
		if ( repository_slot < 0 ) {
			continue;
		}
		if ( m_residency.gpuRegionCache().isEnabled() ) {
			if ( !m_residency.gpuRegionCache().isResident( region_loc ) ) {
				m_residency.gpuRegionCache().markSelectionDirty();
				continue;
			}
			bHeightChanged = bHeightChanged || eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX;
			bPhysicalMappingChanged =
				m_residency.gpuRegionCache().updateResidentLocation( region_loc,
																	 eMapType,
																	 getComputeBackendIfCreated() ) ||
				bPhysicalMappingChanged;
			continue;
		}

		switch ( eMapType ) {
		case TYPE_HEIGHT:
			m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), repository_slot );
			if ( pRegion->getDerivedNormalSlopeMap().is_valid() ) {
				m_derivedMaps[repository_slot] = pRegion->getDerivedNormalSlopeMap();
				m_generatedDerivedMaps.update( pRegion->getDerivedNormalSlopeMap(), repository_slot );
				if ( !IS_EDITOR ) {
					pRegion->setDerivedNormalSlopeMapInternal( Ref< Image >() );
					m_derivedMaps[repository_slot] = Ref< Image >();
				}
			}
			bHeightChanged = true;
			break;
		case TYPE_CONTROL:
			m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
			bControlChanged = true;
			break;
		case TYPE_COLOR:
			m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ),
										 repository_slot );
			bColorChanged = true;
			break;
		default:
			m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), repository_slot );
			m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
			m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ),
										 repository_slot );
			if ( pRegion->getDerivedNormalSlopeMap().is_valid() ) {
				m_derivedMaps[repository_slot] = pRegion->getDerivedNormalSlopeMap();
				m_generatedDerivedMaps.update( pRegion->getDerivedNormalSlopeMap(), repository_slot );
				if ( !IS_EDITOR ) {
					pRegion->setDerivedNormalSlopeMapInternal( Ref< Image >() );
					m_derivedMaps[repository_slot] = Ref< Image >();
				}
			}
			bHeightChanged = true;
			bControlChanged = true;
			bColorChanged = true;
			break;
		}
	}

	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}
	if ( bHeightChanged ) {
		emit_signal( "height_maps_changed" );
	}
	if ( bControlChanged ) {
		emit_signal( "control_maps_changed" );
	}
	if ( bColorChanged ) {
		emit_signal( "color_maps_changed" );
	}
	if ( !m_residency.gpuRegionCache().isEnabled() && isFarAtlasRequired() ) {
		ensureFarAtlases();
	}
}

void Terrain3DData::publishDerivedMapUpdates(
	const eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > &rUpdates ) {
	for ( size_t iUpdate = 0u; iUpdate < ( size_t ) ( ( rUpdates ).size() ); iUpdate++ ) {
		const t3dDerivedMapUpdate_t &update = ( rUpdates )[iUpdate];
		if ( !update.m_bSuccess ) {
			continue;
		}
		Terrain3DRegion *const pRegion = getRegionPtr( update.m_location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( m_residency.gpuRegionCache().isEnabled() ) {
			if ( m_residency.gpuRegionCache().isResident( update.m_location ) ) {
				m_residency.gpuRegionCache().updateResidentLocation( update.m_location,
																	 TYPE_HEIGHT,
																	 getComputeBackendIfCreated() );
			} else {
				m_residency.gpuRegionCache().markSelectionDirty();
			}
			continue;
		}

		const int		   repositorySlot = getRegionId( update.m_location );
		const Ref< Image > pDerivedMap = pRegion->getDerivedNormalSlopeMap();
		if ( repositorySlot >= 0 && pDerivedMap.is_valid() && !m_generatedDerivedMaps.isDirty() &&
			 repositorySlot < m_derivedMaps.size() && repositorySlot < m_generatedDerivedMaps.size() ) {
			m_derivedMaps[repositorySlot] = pDerivedMap;
			m_generatedDerivedMaps.update( pDerivedMap, repositorySlot );
		}
	}
}

void Terrain3DData::regenerateDerivedMaps(
	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
	const AABB												 &rEditedArea ) {
	if ( rRegionLocations.empty() ) {
		return;
	}
	const eastl::vector< t3dDerivedMapUpdate_t, Terrain3DEastlAllocator > &updates =
		m_mapEditService.regenerateDerivedMaps( rRegionLocations, rEditedArea, getComputeBackend() );
	publishDerivedMapUpdates( updates );
}

void Terrain3DData::commitMapEdits(
	const t3dMapType_t										  eMapType,
	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &rRegionLocations,
	const AABB												 &rEditedArea,
	const uint32_t											  nFlags ) {
	if ( eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX || rRegionLocations.empty() ) {
		return;
	}

	const bool				bKeepEdited = ( nFlags & MAP_EDIT_KEEP_EDITED ) != 0u;
	Terrain3DCompute *const pCompute = eMapType == TYPE_HEIGHT ? getComputeBackend() : NULL;
	t3dMapMutation_t mutation = m_mapEditService.commitMapEdits( eMapType, rRegionLocations, rEditedArea, pCompute );
	if ( !mutation.valid() ) {
		return;
	}

	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &activeLocations = mutation.m_changedLocations;
	const eastl::vector< Vector2i, Terrain3DEastlAllocator > &derivedLocations = mutation.m_derivedLocations;
	publishDerivedMapUpdates( mutation.m_derivedUpdates );

	if ( m_residency.gpuRegionCache().isEnabled() ) {
		for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( activeLocations ).size() ); iLocation++ ) {
			const Vector2i &location = ( activeLocations )[iLocation];
			markGpuFarRegionDirty( location, eMapType );
		}
	}
	if ( eMapType == TYPE_HEIGHT && isFarAtlasRequired() && m_pTerrain ) {
		const Ref< Terrain3DMaterial > pMaterial = m_pTerrain->materialResource();
		if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
			for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( derivedLocations ).size() ); iLocation++ ) {
				const Vector2i &location = ( derivedLocations )[iLocation];
				markGpuFarRegionDirty( location, TYPE_COLOR );
			}
		}
	}

	updateMapRegions( eMapType, activeLocations );
	m_mapEditService.finalizeMapEdits( mutation, bKeepEdited );
	for ( size_t i = 0; i < activeLocations.size(); ++i ) {
		publishSparseRasterEdit(
			eMapType,
			activeLocations[i],
			Terrain3DMapEditService::getRegionDirtyRect( activeLocations[i], mutation.m_editedArea ) );
	}

	if ( mutation.m_editedArea.size.x > 0.0f && mutation.m_editedArea.size.z > 0.0f ) {
		if ( IS_EDITOR && m_pTerrain ) {
			m_pTerrain->notifyTerrainMapsEdited( mutation.m_editedArea );
		}
		LOG( DEBUG, "Emitting maps_edited" );
		emit_signal( "maps_edited", mutation.m_editedArea );
	}

	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_INSTANCES ) != 0u && m_pTerrain->instancerObject() ) {
		m_pTerrain->instancerObject()->updateTransforms( mutation.m_editedArea );
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_COLLISION ) != 0u ) {
		Terrain3DCollision *const pCollision = m_pTerrain->collisionObject();
		if ( pCollision && pCollision->isEnabled() ) {
			if ( pCollision->isDynamicMode() ) {
				pCollision->update( s_v2iMax, true );
			} else {
				for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( activeLocations ).size() ); iLocation++ ) {
					const Vector2i &location = ( activeLocations )[iLocation];
					pCollision->update( location, true );
				}
			}
		}
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_SNAP ) != 0u ) {
		m_pTerrain->snap();
	}
	m_mapEditService.recycleMutation( eastl::move( mutation ) );
}

void Terrain3DData::commitMapEdit(
	const t3dMapType_t eMapType,
	const Vector2i	  &rRegionLocation,
	const Rect2i	  &rDirtyRect,
	const bool		   bUpdateGeometryDependents ) {
	if ( eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Invalid map type: ", eMapType );
		return;
	}

	AABB editedArea;
	if ( !m_mapEditService.buildRegionEditArea( rRegionLocation, rDirtyRect, editedArea ) ) {
		LOG( ERROR, "No active region found at: ", rRegionLocation );
		return;
	}

	uint32_t flags = 0u;
	if ( bUpdateGeometryDependents ) {
		flags = MAP_EDIT_SNAP;
		if ( eMapType == TYPE_HEIGHT || eMapType == TYPE_CONTROL ) {
			flags |= MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION;
		}
	}
	commitMapEdits( eMapType, { rRegionLocation }, editedArea, flags );
}

void Terrain3DData::setPixel(
	const t3dMapType_t eMapType,
	const Vector3	  &rGlobalPosition,
	const Color		  &rPixel ) {
	if ( eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Specified map type out of range" );
		return;
	}
	const Vector2i regionLocation = getRegionLocation( rGlobalPosition );
	if ( !ensureCpuRegionResident( regionLocation ) ) {
		LOG( ERROR, "Unable to load CPU map payload for region: ", regionLocation );
		return;
	}

	Vector2i	writtenLocation;
	Rect2i		dirtyRect;
	const Error writeError = m_mapEditService.setPixel( eMapType, rGlobalPosition, rPixel, writtenLocation, dirtyRect );
	if ( writeError != OK ) {
		LOG( ERROR, "Unable to edit terrain map at: ", rGlobalPosition, " error: ", writeError );
		return;
	}
	commitMapEdit( eMapType, writtenLocation, dirtyRect );
}

Color Terrain3DData::getPixel(
	const t3dMapType_t eMapType,
	const Vector3	  &rGlobalPosition ) const {
	if ( eMapType < 0 || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Specified map type out of range" );
		return s_colorNan;
	}
	const Vector2i		   region_loc = getRegionLocation( rGlobalPosition );
	const Terrain3DRegion *pRegion = getRegionPtr( region_loc );
	if ( !pRegion ) {
		return s_colorNan;
	}
	if ( pRegion->isDeleted() ) {
		return s_colorNan;
	}
	const Vector2i global_offset = region_loc * T3D_REGION_SIZE;
	const Vector3  descaled_pos = rGlobalPosition / s_fRegionVertexSpacing;
	Vector2i	   img_pos = Vector2i( descaled_pos.x - global_offset.x, descaled_pos.z - global_offset.y );
	img_pos = img_pos.clamp( s_v2iZero, V2I( T3D_REGION_SIZE - 1 ) );
	const Terrain3DActiveMapLease active = m_samplingStorage.acquireActive( region_loc );
	if ( active.valid() ) {
		const t3dActiveMapView_t &view = active.view();
		const int				  index = img_pos.y * T3D_REGION_SIZE + img_pos.x;
		switch ( eMapType ) {
		case TYPE_HEIGHT:
			return Color( t3dRegionCodecDecodeHeightU16( view.m_pHeight[index] ), 0.0f, 0.0f, 1.0f );
		default:
			break;
		}
	}
	Image *pMap = pRegion->getMapPtr( eMapType );
	if ( !pMap ) {
		if ( pRegion->hasChannel( eMapType ) ) {
			return s_colorNan;
		}
		if ( eMapType == TYPE_CONTROL ) {
			return COLOR_CONTROL;
		}
		if ( eMapType == TYPE_COLOR ) {
			return COLOR_ROUGHNESS;
		}
		return s_colorNan;
	}
	if ( eMapType == TYPE_HEIGHT ) {
		return Color( t3dRegionCodecReadHeightPixel( pMap, img_pos ), 0.0f, 0.0f, 1.0f );
	}
	return pMap->get_pixelv( img_pos );
}

static bool t3dDataIntersectRayAxis(
	const double fOrigin,
	const double fDirection,
	const double fMinimum,
	const double fMaximum,
	double		&fEnter,
	double		&fExit ) {
	if ( fabs( fDirection ) <= 1.0e-12 ) {
		return fOrigin >= fMinimum && fOrigin <= fMaximum;
	}
	double fNear = ( fMinimum - fOrigin ) / fDirection;
	double fFar = ( fMaximum - fOrigin ) / fDirection;
	if ( fNear > fFar ) {
		const double fSwap = fNear;
		fNear = fFar;
		fFar = fSwap;
	}
	if ( fNear > fEnter ) {
		fEnter = fNear;
	}
	if ( fFar < fExit ) {
		fExit = fFar;
	}
	return fEnter <= fExit;
}

static bool t3dDataSolveBilinearRayCell(
	const Vector3 &rOrigin,
	const Vector3 &rDirection,
	const double   fVertexSpacing,
	const int64_t  iCellX,
	const int64_t  iCellZ,
	const double   fHeight00,
	const double   fHeight10,
	const double   fHeight01,
	const double   fHeight11,
	const double   fEnter,
	const double   fExit,
	double		  &fHitDistance ) {
	const double fInverseSpacing = 1.0 / fVertexSpacing;
	const double fCellOriginX = ( double ) ( iCellX ) *fVertexSpacing;
	const double fCellOriginZ = ( double ) ( iCellZ ) *fVertexSpacing;
	const double fU0 = ( ( double ) ( rOrigin.x ) - fCellOriginX ) * fInverseSpacing;
	const double fV0 = ( ( double ) ( rOrigin.z ) - fCellOriginZ ) * fInverseSpacing;
	const double fDu = ( double ) ( rDirection.x ) * fInverseSpacing;
	const double fDv = ( double ) ( rDirection.z ) * fInverseSpacing;
	const double fHeightU = fHeight10 - fHeight00;
	const double fHeightV = fHeight01 - fHeight00;
	const double fHeightUv = fHeight00 - fHeight10 - fHeight01 + fHeight11;
	const double fSurface0 = fHeight00 + fHeightU * fU0 + fHeightV * fV0 + fHeightUv * fU0 * fV0;
	const double fSurface1 = fHeightU * fDu + fHeightV * fDv + fHeightUv * ( fU0 * fDv + fV0 * fDu );
	const double fSurface2 = fHeightUv * fDu * fDv;

	const double fA = -fSurface2;
	const double fB = ( double ) ( rDirection.y ) - fSurface1;
	const double fC = ( double ) ( rOrigin.y ) - fSurface0;
	const double fCoefficientEpsilon = 1.0e-12;
	const double fIntervalEpsilon = 1.0e-7;
	double		 fBest = INFINITY;

	if ( fabs( fA ) <= fCoefficientEpsilon ) {
		if ( fabs( fB ) <= fCoefficientEpsilon ) {
			if ( fabs( fC ) <= 1.0e-5 ) {
				fBest = fEnter;
			}
		} else {
			const double fRoot = -fC / fB;
			if ( fRoot >= fEnter - fIntervalEpsilon && fRoot <= fExit + fIntervalEpsilon && fRoot >= 0.0 &&
				 fB <= fCoefficientEpsilon ) {
				fBest = fRoot;
			}
		}
	} else {
		double fDiscriminant = fB * fB - 4.0 * fA * fC;
		if ( fDiscriminant >= -fCoefficientEpsilon ) {
			if ( fDiscriminant < 0.0 ) {
				fDiscriminant = 0.0;
			}
			const double fRootScale = sqrt( fDiscriminant );
			const double fInverseDenominator = 0.5 / fA;
			const double fRoot0 = ( -fB - fRootScale ) * fInverseDenominator;
			const double fRoot1 = ( -fB + fRootScale ) * fInverseDenominator;
			if ( fRoot0 >= fEnter - fIntervalEpsilon && fRoot0 <= fExit + fIntervalEpsilon && fRoot0 >= 0.0 &&
				 2.0 * fA * fRoot0 + fB <= fCoefficientEpsilon ) {
				fBest = fRoot0;
			}
			if ( fRoot1 >= fEnter - fIntervalEpsilon && fRoot1 <= fExit + fIntervalEpsilon && fRoot1 >= 0.0 &&
				 2.0 * fA * fRoot1 + fB <= fCoefficientEpsilon && fRoot1 < fBest ) {
				fBest = fRoot1;
			}
		}
	}
	if ( !isfinite( fBest ) ) {
		return false;
	}
	fHitDistance = fBest < fEnter ? fEnter : ( fBest > fExit ? fExit : fBest );
	return true;
}

real_t Terrain3DData::getHeight(
	const Vector3 &rGlobalPosition ) const {
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleHeight( rGlobalPosition );
}

bool Terrain3DData::intersectHeightfieldRay(
	const Vector3 &rOrigin,
	const Vector3 &rDirection,
	const real_t   fMaxDistance,
	Vector3		  &rHitPosition ) const {
	if ( !rOrigin.is_finite() || !rDirection.is_finite() || rDirection.is_zero_approx() || fMaxDistance <= 0.0f ) {
		return false;
	}
	const Vector3 direction = rDirection.normalized();
	const double  fSpacing = ( double ) ( getVertexSpacingInternal() );
	if ( fSpacing <= 0.0 ) {
		return false;
	}

	const double fWorldHalfExtent = ( double ) ( T3D_REGION_MAP_HALF * T3D_REGION_SIZE ) * fSpacing;
	double		 fRayEnter = 0.0;
	double		 fRayExit = ( double ) ( fMaxDistance );
	if ( !t3dDataIntersectRayAxis( ( double ) ( rOrigin.x ),
								   ( double ) ( direction.x ),
								   -fWorldHalfExtent,
								   fWorldHalfExtent,
								   fRayEnter,
								   fRayExit ) ||
		 !t3dDataIntersectRayAxis( ( double ) ( rOrigin.z ),
								   ( double ) ( direction.z ),
								   -fWorldHalfExtent,
								   fWorldHalfExtent,
								   fRayEnter,
								   fRayExit ) ||
		 fRayExit < 0.0 ) {
		return false;
	}
	if ( fRayEnter < 0.0 ) {
		fRayEnter = 0.0;
	}
	if ( fRayExit > ( double ) ( fMaxDistance ) ) {
		fRayExit = ( double ) ( fMaxDistance );
	}
	if ( fRayEnter > fRayExit ) {
		return false;
	}

	const double fHorizontalMagnitude = fabs( ( double ) ( direction.x ) ) + fabs( ( double ) ( direction.z ) );
	if ( fHorizontalMagnitude <= 1.0e-12 ) {
		const real_t fHeight = getHeight( rOrigin );
		if ( !isfinite( fHeight ) || direction.y >= -1.0e-12f ) {
			return false;
		}
		const double fDistance = ( ( double ) ( fHeight ) - ( double ) ( rOrigin.y ) ) / ( double ) ( direction.y );
		if ( fDistance < fRayEnter || fDistance > fRayExit ) {
			return false;
		}
		rHitPosition = rOrigin + direction * ( real_t ) ( fDistance );
		return true;
	}

	const Vector3 start = rOrigin + direction * ( real_t ) ( fRayEnter );
	const double  fGridX = ( double ) ( start.x ) / fSpacing;
	const double  fGridZ = ( double ) ( start.z ) / fSpacing;
	int64_t		  iCellX = ( int64_t ) ( floor( fGridX ) );
	int64_t		  iCellZ = ( int64_t ) ( floor( fGridZ ) );
	const int	  iStepX = direction.x > 0.0f ? 1 : ( direction.x < 0.0f ? -1 : 0 );
	const int	  iStepZ = direction.z > 0.0f ? 1 : ( direction.z < 0.0f ? -1 : 0 );
	if ( iStepX < 0 && fabs( fGridX - floor( fGridX ) ) <= 1.0e-10 ) {
		--iCellX;
	}
	if ( iStepZ < 0 && fabs( fGridZ - floor( fGridZ ) ) <= 1.0e-10 ) {
		--iCellZ;
	}

	double fNextX = INFINITY;
	double fNextZ = INFINITY;
	double fDeltaX = INFINITY;
	double fDeltaZ = INFINITY;
	if ( iStepX != 0 ) {
		const double fBoundaryX = ( double ) ( iCellX + ( iStepX > 0 ? 1 : 0 ) ) * fSpacing;
		fNextX = ( fBoundaryX - ( double ) ( rOrigin.x ) ) / ( double ) ( direction.x );
		fDeltaX = fSpacing / fabs( ( double ) ( direction.x ) );
		if ( fNextX < fRayEnter ) {
			fNextX = fRayEnter;
		}
	}
	if ( iStepZ != 0 ) {
		const double fBoundaryZ = ( double ) ( iCellZ + ( iStepZ > 0 ? 1 : 0 ) ) * fSpacing;
		fNextZ = ( fBoundaryZ - ( double ) ( rOrigin.z ) ) / ( double ) ( direction.z );
		fDeltaZ = fSpacing / fabs( ( double ) ( direction.z ) );
		if ( fNextZ < fRayEnter ) {
			fNextZ = fRayEnter;
		}
	}

	const int64_t iMaxCells = ( int64_t ) ( ceil( ( fRayExit - fRayEnter ) * fHorizontalMagnitude / fSpacing ) ) + 8;
	double		  fCellEnter = fRayEnter;
	for ( int64_t iCell = 0; iCell < iMaxCells && fCellEnter <= fRayExit + 1.0e-7; ++iCell ) {
		double fCellExit = fRayExit;
		if ( fNextX < fCellExit ) {
			fCellExit = fNextX;
		}
		if ( fNextZ < fCellExit ) {
			fCellExit = fNextZ;
		}
		if ( fCellExit < fCellEnter ) {
			fCellExit = fCellEnter;
		}

		const int64_t iRegionX = ( int64_t ) ( floor( ( double ) ( iCellX ) / ( double ) ( T3D_REGION_SIZE ) ) );
		const int64_t iRegionZ = ( int64_t ) ( floor( ( double ) ( iCellZ ) / ( double ) ( T3D_REGION_SIZE ) ) );
		const bool	  bRegionPresent =
			iRegionX >= INT32_MIN && iRegionX <= INT32_MAX && iRegionZ >= INT32_MIN && iRegionZ <= INT32_MAX &&
			m_regionRepository.isLocationValid( Vector2i( ( int32_t ) ( iRegionX ), ( int32_t ) ( iRegionZ ) ) );
		if ( bRegionPresent ) {
			Terrain3DSampler sampler( getSamplingSourceInternal() );
			const uint32_t	 nControl = sampler.sampleControlGrid( iCellX, iCellZ );
			if ( nControl != UINT32_MAX && !t3dIsHole( nControl ) ) {
				const double fHeight00 = ( double ) ( sampler.sampleHeightGrid( iCellX, iCellZ, false ) );
				const double fHeight10 = ( double ) ( sampler.sampleHeightGrid( iCellX + 1, iCellZ, false ) );
				const double fHeight01 = ( double ) ( sampler.sampleHeightGrid( iCellX, iCellZ + 1, false ) );
				const double fHeight11 = ( double ) ( sampler.sampleHeightGrid( iCellX + 1, iCellZ + 1, false ) );
				if ( isfinite( fHeight00 ) && isfinite( fHeight10 ) && isfinite( fHeight01 ) &&
					 isfinite( fHeight11 ) ) {
					double fHitDistance = 0.0;
					if ( t3dDataSolveBilinearRayCell( rOrigin,
													  direction,
													  fSpacing,
													  iCellX,
													  iCellZ,
													  fHeight00,
													  fHeight10,
													  fHeight01,
													  fHeight11,
													  fCellEnter,
													  fCellExit,
													  fHitDistance ) ) {
						rHitPosition = rOrigin + direction * ( real_t ) ( fHitDistance );
						return true;
					}
				}
			}
		}

		if ( fCellExit >= fRayExit - 1.0e-7 ) {
			break;
		}
		if ( fNextX < fNextZ - 1.0e-10 ) {
			iCellX += iStepX;
			fCellEnter = fNextX;
			fNextX += fDeltaX;
		} else if ( fNextZ < fNextX - 1.0e-10 ) {
			iCellZ += iStepZ;
			fCellEnter = fNextZ;
			fNextZ += fDeltaZ;
		} else {
			iCellX += iStepX;
			iCellZ += iStepZ;
			fCellEnter = fNextX < fNextZ ? fNextX : fNextZ;
			fNextX += fDeltaX;
			fNextZ += fDeltaZ;
		}
	}
	return false;
}

uint32_t Terrain3DData::getControl(
	const Vector3 &rGlobalPosition ) const {
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleControl( rGlobalPosition );
}

PackedFloat32Array Terrain3DData::getHeights(
	const PackedVector3Array &rGlobalPositions ) const {
	PackedFloat32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	Terrain3DSampler sampler( getSamplingSourceInternal() );
	sampler.sampleHeights( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
	return result;
}

PackedInt32Array Terrain3DData::getControls(
	const PackedVector3Array &rGlobalPositions ) const {
	PackedInt32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	Terrain3DSampler sampler( getSamplingSourceInternal() );
	sampler.sampleControls( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
	return result;
}

PackedVector3Array Terrain3DData::getNormals(
	const PackedVector3Array &rGlobalPositions ) const {
	PackedVector3Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	Terrain3DSampler sampler( getSamplingSourceInternal() );
	sampler.sampleNormals( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
	return result;
}

Vector3 Terrain3DData::getNormal(
	const Vector3 &rGlobalPosition ) const {
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleNormal( rGlobalPosition );
}

bool Terrain3DData::isInSlope(
	const Vector3 &rGlobalPosition,
	const Vector2 &rSlopeRange,
	const Vector3 &rNormal ) const {
	const Vector2 slope_range = CLAMP( rSlopeRange, s_v2Zero, V2( 90.f ) );
	if ( slope_range.y - slope_range.x > 89.99f ) {
		return true;
	}

	Vector3 slope_normal = rNormal;
	if ( !slope_normal.is_zero_approx() ) {
		slope_normal.normalize();
	} else {
		Terrain3DSampler sampler( getSamplingSourceInternal() );
		uint32_t		 control = UINT32_MAX;
		if ( !sampler.sampleControlAndSlopeNormal( rGlobalPosition, control, slope_normal ) ) {
			return false;
		}
	}

	const real_t fSlopeAngleDegrees =
		Math::rad_to_deg( Math::acos( CLAMP( slope_normal.dot( s_v3Up ), -1.0f, 1.0f ) ) );
	return slope_range.x <= fSlopeAngleDegrees && fSlopeAngleDegrees <= slope_range.y;
}

/**
 * Returns:
 * X = base index
 * Y = overlay index
 * Z = percentage blend between X and Y. Limited to the fixed values in RANGE.
 * Interpretation of this data is up to the gamedev. Unfortunately due to blending, this isn't
 * pixel perfect. I would have your player print this location as you walk around to see how the
 * blending values look, then consider that the overlay texture is visible starting at a blend
 * value of .3-.5, otherwise it's the base texture.
 **/
Vector3 Terrain3DData::getTextureId(
	const Vector3 &rGlobalPosition ) const {
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	const uint32_t	 control = sampler.sampleControl( rGlobalPosition );
	if ( control == UINT32_MAX || t3dIsHole( control ) ) {
		return s_v3Nan;
	}

	// If material available, autoshader enabled, and pixel set to auto
	if ( m_pTerrain ) {
		const Ref< Terrain3DMaterial > pTMaterial = m_pTerrain->materialResource();
		const bool					   bControlAuto = t3dIsAuto( control );
		if ( pTMaterial.is_valid() && pTMaterial->getAutoShaderEnabled() && bControlAuto ) {
			const real_t fAutoSlope = ( real_t ) ( pTMaterial->getShaderParam( "auto_slope" ) );
			const real_t fAutoHeightReduction = ( real_t ) ( pTMaterial->getShaderParam( "auto_height_reduction" ) );
			float		 fHeight = NAN;
			Vector3		 normal = s_v3Nan;
			if ( !sampler.sampleHeightAndNormal( rGlobalPosition, fHeight, normal ) ) {
				return s_v3Nan;
			}
			const uint32_t base_id = pTMaterial->getShaderParam( "auto_base_texture" );
			const uint32_t overlay_id = pTMaterial->getShaderParam( "auto_overlay_texture" );
			const real_t   fBlend =
				CLAMP( ( fAutoSlope * 2.f * ( normal.y - 1.f ) + 1.f ) - fAutoHeightReduction * .01f * fHeight,
					   0.f,
					   1.f );
			return Vector3( ( real_t ) ( base_id ), ( real_t ) ( overlay_id ), fBlend );
		}
	}

	// Else, just get textures from control map
	const uint32_t base_id = t3dGetBase( control );
	const uint32_t overlay_id = t3dGetOverlay( control );
	const real_t   fBlend = ( real_t ) ( t3dGetBlend( control ) ) / s_fByteMaxFloat;
	return Vector3( ( real_t ) ( base_id ), ( real_t ) ( overlay_id ), fBlend );
}

/**
 * Returns the location of a terrain vertex at a certain LOD. If there is a hole at the position, it returns
 * NAN in the vector's Y coordinate.
 * p_lod (0-8): Determines how many heights around the given global position will be sampled.
 * p_filter:
 *  HEIGHT_FILTER_NEAREST: Samples the height map at the exact coordinates given.
 *  HEIGHT_FILTER_MINIMUM: Samples (1 << p_lod) ** 2 heights around the given coordinates and returns the lowest.
 * p_global_position: X and Z coordinates of the vertex. Heights will be sampled around these coordinates.
 */
Vector3 Terrain3DData::getMeshVertex(
	const int32_t			nLod,
	const t3dHeightFilter_t filter,
	const Vector3		   &rGlobalPosition ) const {
	const int32_t	 step = 1 << CLAMP( nLod, 0, 8 );
	real_t			 fHeight = 0.0f;
	Terrain3DSampler sampler( getSamplingSourceInternal() );

	switch ( filter ) {
	case HEIGHT_FILTER_NEAREST: {
		fHeight = sampler.sampleHeight( rGlobalPosition );
	} break;
	case HEIGHT_FILTER_MINIMUM: {
		const double  fGridX = ( double ) ( rGlobalPosition.x ) / ( double ) ( s_fRegionVertexSpacing );
		const double  fGridZ = ( double ) ( rGlobalPosition.z ) / ( double ) ( s_fRegionVertexSpacing );
		const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
		const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
		const double  fWorldDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( s_fRegionVertexSpacing );
		const double  fWorldDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( s_fRegionVertexSpacing );
		if ( fWorldDx * fWorldDx + fWorldDz * fWorldDz < 0.0001 ) {
			float fMinimum = NAN;
			fHeight =
				sampler.minimumHeightRect( rounded_x - step / 2, rounded_z - step / 2, step, step, fMinimum, true )
					? fMinimum
					: NAN;
			break;
		}

		// Preserve bilinear sampling semantics for callers that request a minimum
		// filter at a fractional grid position. Mesh and navigation bake callers
		// use the grid-aligned bulk path above.
		fHeight = sampler.sampleHeight( rGlobalPosition );
		bool bInvalid = isnan( fHeight );
		for ( int32_t dz = -step / 2; dz < step / 2 && !bInvalid; ++dz ) {
			for ( int32_t dx = -step / 2; dx < step / 2; ++dx ) {
				const Vector3 position = rGlobalPosition + Vector3( dx, 0.f, dz ) * s_fRegionVertexSpacing;
				const real_t  fSample = sampler.sampleHeight( position );
				if ( isnan( fSample ) ) {
					bInvalid = true;
					break;
				}
				fHeight = MIN( fHeight, fSample );
			}
		}
		if ( bInvalid ) {
			fHeight = NAN;
		}
	} break;
	}
	return Vector3( rGlobalPosition.x, fHeight, rGlobalPosition.z );
}

void Terrain3DData::addEditedArea(
	const AABB &rArea ) {
	m_mapEditService.addEditedArea( rArea );
	LOG( DEBUG, "Emitting maps_edited" );
	emit_signal( "maps_edited", rArea );
}

bool Terrain3DData::ensureHeightRangeRegionResident(
	void *const		pContext,
	const Vector2i &rLocation ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	if ( pData->ensureCpuRegionResident( rLocation ) ) {
		return true;
	}
	UtilityFunctions::push_error(
		"Terrain3DData:calcHeightRange: Unable to page region for recursive height-range calculation: ",
		rLocation );
	return false;
}

Error Terrain3DData::prepareImportRegion(
	void *const				pContext,
	const Vector2i		   &rLocation,
	Ref< Terrain3DRegion > &rRegion ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	rRegion = pData->getRegion( rLocation );
	if ( rRegion.is_null() ) {
		rRegion.instantiate();
		rRegion->setLocation( rLocation );
		return pData->addRegion( rRegion, false );
	}
	if ( !rRegion->isDeleted() ) {
		if ( !pData->ensureCpuRegionResident( rLocation ) ) {
			UtilityFunctions::push_error(
				"Terrain3DData:importImages: Unable to page existing CPU terrain maps for image import at ",
				rLocation );
			return ERR_CANT_ACQUIRE_RESOURCE;
		}
		rRegion = pData->getRegion( rLocation );
		return rRegion.is_valid() ? OK : ERR_DOES_NOT_EXIST;
	}
	rRegion->clear();
	rRegion->setLocation( rLocation );
	return OK;
}

bool Terrain3DData::ensureImageExportRegionResident(
	void *const		pContext,
	const Vector2i &rLocation ) {
	Terrain3DData *const pData = ( Terrain3DData * ) ( pContext );
	if ( pData->ensureCpuRegionResident( rLocation ) ) {
		return true;
	}
	UtilityFunctions::push_error( "Terrain3DData:layeredToImage: Unable to page region for image export: ", rLocation );
	return false;
}

void Terrain3DData::calcHeightRange(
	const bool bRecursive ) {
	m_mapEditService.calculateHeightRange( bRecursive, Terrain3DData::ensureHeightRangeRegionResident, this );
	LOG( EXTREME, "Accumulated height range for all regions: ", m_mapEditService.getHeightRange() );
}

/**
 * Imports an Image set (Height, Control, Color) into Terrain3DData
 * It does NOT normalize values to 0-1. You must do that using get_min_max() and adjusting scale and offset.
 * Parameters:
 *	p_images - t3dMapType_t.TYPE_MAX sized array of Images for Height, Control, Color. Images can be blank or null
 *	p_global_position - X,0,Z location on the region map. Valid range is ~ (+/-8192, +/-8192)
 *	p_offset - Add this factor to all height values, can be negative
 *	p_scale - Scale all height values by this factor (applied after offset)
 */
void Terrain3DData::importImages(
	const TypedArray< Image > &rImages,
	const Vector3			  &rGlobalPosition,
	const real_t			   fOffset,
	const real_t			   fScale ) {
	IS_INIT_MESG( "Data not initialized", VOID );

	bool		bGenerateMipmaps = false;
	const Error error = m_mapEditService.importImages( rImages,
													   rGlobalPosition,
													   fOffset,
													   fScale,
													   Terrain3DData::prepareImportRegion,
													   this,
													   bGenerateMipmaps );
	if ( error != OK ) {
		LOG( ERROR, "Image import failed with error: ", error );
		return;
	}

	updateMaps( REGION_TYPE_MAX, true, bGenerateMipmaps );
}

/** Exports a specified map as one of r16/raw, exr, jpg, png, webp, res, tres
 * r16 or exr are recommended for roundtrip external editing
 * r16 can be edited by Krita, however you must know the dimensions and min/max before reimporting
 * res/tres stores in Godot's native format.
 */
Error Terrain3DData::exportImage(
	const String	  &rFileName,
	const t3dMapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Invalid map type specified: ", eMapType, " max: ", REGION_TYPE_MAX - 1 );
		return FAILED;
	}

	if ( rFileName.is_empty() ) {
		LOG( ERROR, "No file specified. Nothing to export" );
		return FAILED;
	}

	if ( getRegionCount() == 0 ) {
		LOG( ERROR, "No valid regions. Nothing to export" );
		return FAILED;
	}

	// Simple file name validation
	static const String s_badChars = "?*|%<>\"";
	for ( int i = 0; i < s_badChars.length(); ++i ) {
		for ( int j = 0; j < rFileName.length(); ++j ) {
			if ( s_badChars[i] == rFileName[j] ) {
				LOG( ERROR, String( "Invalid file path '" ) + rFileName + String( "'" ) );
				return FAILED;
			}
		}
	}

	// Update path delimeter
	String file_name = rFileName.replace( "\\", "/" );

	// Check if p_file_name has a path and prepend "res://" if not
	bool bIsSimpleFilename = true;
	for ( int i = 0; i < file_name.length(); ++i ) {
		const char32_t c = file_name[i];
		if ( c == '/' || c == ':' ) {
			bIsSimpleFilename = false;
			break;
		}
	}
	if ( bIsSimpleFilename ) {
		file_name = String( "res://" ) + file_name;
	}

	// Check if the file can be opened for writing
	Ref< FileAccess > pFileRef = FileAccess::open( file_name, FileAccess::ModeFlags::WRITE );
	if ( pFileRef.is_null() ) {
		LOG( ERROR, String( "Cannot open file '" ) + file_name + String( "' for writing" ) );
		return FAILED;
	}
	pFileRef->close();

	// Filename is validated. Begin export image generation
	Ref< Image > pImg = layeredToImage( eMapType );
	if ( pImg.is_null() || pImg->is_empty() ) {
		LOG( ERROR, "Cannot create an export image for map type: ", s_pszRegionMapTypeNames[eMapType] );
		return FAILED;
	}

	String ext = file_name.get_extension().to_lower();
	LOG( MESG,
		 "Saving ",
		 pImg->get_size(),
		 " sized ",
		 s_pszRegionMapTypeNames[eMapType],
		 " map in format ",
		 pImg->get_format(),
		 " as ",
		 ext,
		 " to: ",
		 file_name );
	const Vector2i minmax = Terrain3DUtil::getMinMax( pImg );
	LOG( MESG, "Minimum height: ", minmax.x, ", Maximum height: ", minmax.y );
	if ( ext == "r16" || ext == "raw" ) {
		Ref< FileAccess > pFile = FileAccess::open( file_name, FileAccess::WRITE );
		const real_t	  fHeightMin = minmax.x;
		const real_t	  fHeightMax = minmax.y;
		const real_t	  fHscale = 65535.0 / ( fHeightMax - fHeightMin );
		for ( int y = 0; y < pImg->get_height(); y++ ) {
			for ( int x = 0; x < pImg->get_width(); x++ ) {
				int h = ( int ) ( ( pImg->get_pixel( x, y ).r - fHeightMin ) * fHscale );
				h = CLAMP( h, 0, 65535 );
				pFile->store_16( h );
			}
		}
		return pFile->get_error();
	} else if ( ext == "exr" ) {
		return pImg->save_exr( file_name, ( eMapType == TYPE_HEIGHT ) ? true : false );
	} else if ( ext == "png" ) {
		return pImg->save_png( file_name );
	} else if ( ext == "jpg" ) {
		return pImg->save_jpg( file_name );
	} else if ( ext == "webp" ) {
		return pImg->save_webp( file_name );
	} else if ( ( ext == "res" ) || ( ext == "tres" ) ) {
		return ResourceSaver::get_singleton()->save( pImg, file_name, ResourceSaver::FLAG_COMPRESS );
	}

	LOG( ERROR, "No recognized file type. See docs for valid extensions" );
	return FAILED;
}

Ref< Image > Terrain3DData::layeredToImage(
	const t3dMapType_t eMapType ) const {
	return m_mapEditService.layeredToImage( eMapType,
											Terrain3DData::ensureImageExportRegionResident,
											const_cast< Terrain3DData * >( this ) );
}

void Terrain3DData::dump(
	const bool bVerbose ) const {
	const TypedArray< Vector2i > region_slots = getRegionSlots();
	LOG( MESG, "_region_locations (", m_regionRepository.getSlotCount(), "): ", region_slots );
	const Array keys = m_regionRepository.getRegionKeys();
	LOG( MESG, "_regions (", keys.size(), "):" );
	for ( size_t iKey = 0u; iKey < ( size_t ) ( ( keys ).size() ); iKey++ ) {
		const Vector2i		   region_loc = ( keys )[iKey];
		const Terrain3DRegion *pRegion = getRegionPtr( region_loc );
		if ( !pRegion ) {
			LOG( WARN, "No region found at: ", region_loc );
			continue;
		}
		pRegion->dump( bVerbose );
	}
	if ( bVerbose ) {
		for ( int i = 0; i < T3D_REGION_SLOTS; i++ ) {
			if ( m_regionRepository.getLogicalRegionMapData()[i] ) {
				LOG( MESG,
					 "Region map array index: ",
					 i,
					 " / ",
					 T3D_REGION_SLOTS - 1,
					 ", Region id: ",
					 m_regionRepository.getLogicalRegionMapData()[i] );
			}
		}
		Terrain3DUtil::dumpMaps( m_heightMaps, "Height maps" );
		Terrain3DUtil::dumpGentex( m_generatedHeightMaps, "height" );
		Terrain3DUtil::dumpMaps( m_controlMaps, "Control maps" );
		Terrain3DUtil::dumpGentex( m_generatedControlMaps, "control" );
		Terrain3DUtil::dumpMaps( m_colorMaps, "Color maps" );
		Terrain3DUtil::dumpGentex( m_generatedColorMaps, "color" );
	}
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DData::_bind_methods(
	void ) {
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_NEAREST );
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_MINIMUM );

	ClassDB::bind_integer_constant( get_class_static(), "", "WORLD_TEXEL_SIZE", T3D_ACTIVE_WORLD_TEXELS );
	ClassDB::bind_integer_constant( get_class_static(), "", "REGION_MAP_SIZE", T3D_REGION_MAP_SIZE );

	ClassDB::bind_method( D_METHOD( "get_region_count" ), &Terrain3DData::getRegionCount );
	ClassDB::bind_method( D_METHOD( "get_render_region_slots" ), &Terrain3DData::getRenderRegionSlots );
	ClassDB::bind_method( D_METHOD( "get_regions_active", "copy", "deep" ),
						  &Terrain3DData::getRegionsActive,
						  DEFVAL( false ),
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_regions_all" ), &Terrain3DData::getRegionsAll );
	ClassDB::bind_method( D_METHOD( "get_region_map" ), &Terrain3DData::getRegionMap );
	ClassDB::bind_method( D_METHOD( "get_region_map_size" ), &Terrain3DData::getRegionMapSize );
	ClassDB::bind_method( D_METHOD( "is_runtime_ready" ), &Terrain3DData::isRuntimeReady );
	ClassDB::bind_method( D_METHOD( "get_active_storage_generation" ), &Terrain3DData::getActiveStorageGeneration );
	ClassDB::bind_method( D_METHOD( "get_runtime_state" ), &Terrain3DData::getRuntimeState );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &Terrain3DData::getGpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_single_copy_baseline_stats" ), &Terrain3DData::getSingleCopyBaselineStats );
	ClassDB::bind_method( D_METHOD( "set_gpu_authoritative_pages", "enabled" ),
						  &Terrain3DData::setGpuAuthoritativePages );
	ClassDB::bind_method( D_METHOD( "get_gpu_authoritative_pages" ), &Terrain3DData::getGpuAuthoritativePages );
	ClassDB::bind_method( D_METHOD( "get_middle_height_cache_stats" ), &Terrain3DData::getMiddleHeightCacheStats );
	ClassDB::bind_method( D_METHOD( "get_startup_load_stats" ), &Terrain3DData::getStartupLoadStats );
	ClassDB::bind_method( D_METHOD( "get_sparse_raster_stats" ), &Terrain3DData::getSparseRasterStats );
	ClassDB::bind_method( D_METHOD( "get_runtime_residency_grid", "radius" ),
						  &Terrain3DData::getRuntimeResidencyGrid,
						  DEFVAL( 6 ) );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &Terrain3DData::getCpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ), &Terrain3DData::getInstanceRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "ensure_instance_region_resident", "location" ),
						  &Terrain3DData::ensureInstanceRegionResident );
	ClassDB::bind_method( D_METHOD( "ensure_cpu_region_resident", "region_location" ),
						  &Terrain3DData::ensureCpuRegionResident );
	ClassDB::bind_method( D_METHOD( "prepare_sampling_area", "global_position", "radius_texels" ),
						  &Terrain3DData::prepareSamplingArea,
						  DEFVAL( 1 ) );
	ClassDB::bind_static_method( "Terrain3DData",
								 D_METHOD( "get_region_map_index", "region_location" ),
								 &Terrain3DData::getRegionMapIndex );

	ClassDB::bind_method( D_METHOD( "do_for_regions", "area", "callback" ), &Terrain3DData::doForRegions );

	ClassDB::bind_method( D_METHOD( "get_region_location", "global_position" ), &Terrain3DData::getRegionLocation );
	ClassDB::bind_method( D_METHOD( "get_region_id", "region_location" ), &Terrain3DData::getRegionId );
	ClassDB::bind_method( D_METHOD( "get_region_idp", "global_position" ), &Terrain3DData::getRegionIdp );

	ClassDB::bind_method( D_METHOD( "has_region", "region_location" ), &Terrain3DData::hasRegion );
	ClassDB::bind_method( D_METHOD( "has_regionp", "global_position" ), &Terrain3DData::hasRegionp );
	ClassDB::bind_method( D_METHOD( "get_region", "region_location" ), &Terrain3DData::getRegion );
	ClassDB::bind_method( D_METHOD( "get_regionp", "global_position" ), &Terrain3DData::getRegionp );

	ClassDB::bind_method( D_METHOD( "set_region_modified", "region_location", "modified" ),
						  &Terrain3DData::setRegionModified );
	ClassDB::bind_method( D_METHOD( "is_region_modified", "region_location" ), &Terrain3DData::isRegionModified );
	ClassDB::bind_method( D_METHOD( "set_region_deleted", "region_location", "deleted" ),
						  &Terrain3DData::setRegionDeleted );
	ClassDB::bind_method( D_METHOD( "is_region_deleted", "region_location" ), &Terrain3DData::isRegionDeleted );

	ClassDB::bind_method( D_METHOD( "add_region_blankp", "global_position", "update" ),
						  &Terrain3DData::addRegionBlankp,
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region_blank", "region_location", "update" ),
						  &Terrain3DData::addRegionBlank,
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region", "region", "update" ), &Terrain3DData::addRegion, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "replace_region_data", "region_location", "data" ),
						  &Terrain3DData::replaceRegionData );
	ClassDB::bind_method( D_METHOD( "remove_regionp", "global_position", "update" ),
						  &Terrain3DData::removeRegionp,
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_regionl", "region_location", "update" ),
						  &Terrain3DData::removeRegionl,
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_region", "region", "update" ),
						  &Terrain3DData::removeRegion,
						  DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "save_directory", "directory" ), &Terrain3DData::saveDirectory );
	ClassDB::bind_method( D_METHOD( "save_region", "region_location", "directory", "save_16_bit" ),
						  &Terrain3DData::saveRegion,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "inspect_gpu_region_pages" ), &Terrain3DData::inspectGpuRegionPages );
#if defined( T3D_EDITOR_NATIVE )
	ClassDB::bind_method( D_METHOD( "cook_gpu_region_pages" ), &Terrain3DData::cookGpuRegionPages );
#endif
	ClassDB::bind_method( D_METHOD( "load_directory", "directory" ), &Terrain3DData::loadDirectory );
	ClassDB::bind_method( D_METHOD( "load_region", "region_location", "directory", "update" ),
						  &Terrain3DData::loadRegion,
						  DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "get_height_maps" ), &Terrain3DData::getHeightMaps );
	ClassDB::bind_method( D_METHOD( "get_control_maps" ), &Terrain3DData::getControlMaps );
	ClassDB::bind_method( D_METHOD( "get_color_maps" ), &Terrain3DData::getColorMaps );
	ClassDB::bind_method( D_METHOD( "get_maps", "map_type" ), &Terrain3DData::getMaps );
	ClassDB::bind_method( D_METHOD( "update_maps", "map_type", "all_regions", "generate_mipmaps" ),
						  &Terrain3DData::updateMaps,
						  DEFVAL( REGION_TYPE_MAX ),
						  DEFVAL( true ),
						  DEFVAL( false ) );
#if defined( T3D_EDITOR_NATIVE )
	ClassDB::bind_method( D_METHOD( "create_erosion_job", "settings" ), &Terrain3DData::createErosionJob );
#endif
	ClassDB::bind_method( D_METHOD( "get_height_maps_rid" ), &Terrain3DData::getHeightMapsRid );
	ClassDB::bind_method( D_METHOD( "get_derived_maps_rid" ), &Terrain3DData::getDerivedMapsRid );
	ClassDB::bind_method( D_METHOD( "get_control_maps_rid" ), &Terrain3DData::getControlMapsRid );
	ClassDB::bind_method( D_METHOD( "get_color_maps_rid" ), &Terrain3DData::getColorMapsRid );
	ClassDB::bind_method( D_METHOD( "are_control_maps_implicit" ), &Terrain3DData::areControlMapsImplicit );
	ClassDB::bind_method( D_METHOD( "are_color_maps_implicit" ), &Terrain3DData::areColorMapsImplicit );

	ClassDB::bind_method( D_METHOD( "set_pixel", "map_type", "global_position", "pixel" ), &Terrain3DData::setPixel );
	ClassDB::bind_method( D_METHOD( "commit_map_edit", "map_type", "region_location", "dirty_rect", "update_geometry_dependents" ),
						  &Terrain3DData::commitMapEdit,
						  DEFVAL( Rect2i() ),
						  DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "get_pixel", "map_type", "global_position" ), &Terrain3DData::getPixel );
	ClassDB::bind_method( D_METHOD( "set_height", "global_position", "height" ), &Terrain3DData::setHeight );
	ClassDB::bind_method( D_METHOD( "get_height", "global_position" ), &Terrain3DData::getHeight );
	ClassDB::bind_method( D_METHOD( "get_heights", "global_positions" ), &Terrain3DData::getHeights );
	ClassDB::bind_method( D_METHOD( "get_normals", "global_positions" ), &Terrain3DData::getNormals );
	ClassDB::bind_method( D_METHOD( "get_controls", "global_positions" ), &Terrain3DData::getControls );
	ClassDB::bind_method( D_METHOD( "set_color", "global_position", "color" ), &Terrain3DData::setColor );
	ClassDB::bind_method( D_METHOD( "get_color", "global_position" ), &Terrain3DData::getColor );
	ClassDB::bind_method( D_METHOD( "set_control", "global_position", "control" ), &Terrain3DData::setControl );
	ClassDB::bind_method( D_METHOD( "get_control", "global_position" ), &Terrain3DData::getControl );
	ClassDB::bind_method( D_METHOD( "set_roughness", "global_position", "roughness" ), &Terrain3DData::setRoughness );
	ClassDB::bind_method( D_METHOD( "get_roughness", "global_position" ), &Terrain3DData::getRoughness );

	ClassDB::bind_method( D_METHOD( "set_control_base_id", "global_position", "texture_id" ),
						  &Terrain3DData::setControlBaseId );
	ClassDB::bind_method( D_METHOD( "get_control_base_id", "global_position" ), &Terrain3DData::getControlBaseId );
	ClassDB::bind_method( D_METHOD( "set_control_overlay_id", "global_position", "texture_id" ),
						  &Terrain3DData::setControlOverlayId );
	ClassDB::bind_method( D_METHOD( "get_control_overlay_id", "global_position" ),
						  &Terrain3DData::getControlOverlayId );
	ClassDB::bind_method( D_METHOD( "set_control_blend", "global_position", "blend_value" ),
						  &Terrain3DData::setControlBlend );
	ClassDB::bind_method( D_METHOD( "get_control_blend", "global_position" ), &Terrain3DData::getControlBlend );
	ClassDB::bind_method( D_METHOD( "set_control_angle", "global_position", "degrees" ),
						  &Terrain3DData::setControlAngle );
	ClassDB::bind_method( D_METHOD( "get_control_angle", "global_position" ), &Terrain3DData::getControlAngle );
	ClassDB::bind_method( D_METHOD( "set_control_scale", "global_position", "percentage_modifier" ),
						  &Terrain3DData::setControlScale );
	ClassDB::bind_method( D_METHOD( "get_control_scale", "global_position" ), &Terrain3DData::getControlScale );
	ClassDB::bind_method( D_METHOD( "set_control_hole", "global_position", "enable" ), &Terrain3DData::setControlHole );
	ClassDB::bind_method( D_METHOD( "get_control_hole", "global_position" ), &Terrain3DData::getControlHole );
	ClassDB::bind_method( D_METHOD( "set_control_navigation", "global_position", "enable" ),
						  &Terrain3DData::setControlNavigation );
	ClassDB::bind_method( D_METHOD( "get_control_navigation", "global_position" ),
						  &Terrain3DData::getControlNavigation );
	ClassDB::bind_method( D_METHOD( "set_control_auto", "global_position", "enable" ), &Terrain3DData::setControlAuto );
	ClassDB::bind_method( D_METHOD( "get_control_auto", "global_position" ), &Terrain3DData::getControlAuto );

	ClassDB::bind_method( D_METHOD( "get_normal", "global_position" ), &Terrain3DData::getNormal );
	ClassDB::bind_method( D_METHOD( "is_in_slope", "global_position", "slope_range", "normal" ),
						  &Terrain3DData::isInSlope,
						  DEFVAL( s_v3Zero ) );
	ClassDB::bind_method( D_METHOD( "get_texture_id", "global_position" ), &Terrain3DData::getTextureId );
	ClassDB::bind_method( D_METHOD( "get_mesh_vertex", "lod", "filter", "global_position" ),
						  &Terrain3DData::getMeshVertex );

	ClassDB::bind_method( D_METHOD( "get_height_range" ), &Terrain3DData::getHeightRange );
	ClassDB::bind_method( D_METHOD( "calc_height_range", "recursive" ),
						  &Terrain3DData::calcHeightRange,
						  DEFVAL( false ) );

	ClassDB::bind_method( D_METHOD( "import_images", "images", "global_position", "offset", "scale" ),
						  &Terrain3DData::importImages,
						  DEFVAL( s_v3Zero ),
						  DEFVAL( 0.f ),
						  DEFVAL( 1.f ) );
	ClassDB::bind_method( D_METHOD( "export_image", "file_name", "map_type" ), &Terrain3DData::exportImage );
	ClassDB::bind_method( D_METHOD( "layered_to_image", "map_type" ), &Terrain3DData::layeredToImage );
	ClassDB::bind_method( D_METHOD( "dump", "verbose" ), &Terrain3DData::dump, DEFVAL( false ) );

	const int ro_flags = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Runtime Repository Status", "" );
	T3D_BIND_PROPERTY(
		Terrain3DData,
		setRegionLocations,
		getRegionLocations,
		"region_locations",
		PropertyInfo( Variant::ARRAY, "region_locations", PROPERTY_HINT_ARRAY_TYPE, "Vector2i", ro_flags ),
		"set_region_locations",
		"get_region_locations" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "height_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ),
				  "",
				  "get_height_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "control_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ),
				  "",
				  "get_control_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "color_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ),
				  "",
				  "get_color_maps" );

	ADD_SIGNAL( MethodInfo( "maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "gpu_region_page_table_changed" ) );
	ADD_SIGNAL( MethodInfo( "region_map_changed" ) );
	ADD_SIGNAL( MethodInfo( "height_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "control_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "color_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "maps_edited", PropertyInfo( Variant::AABB, "edited_area" ) ) );
}

// M39 extracted non-template header implementations.
bool Terrain3DData::isStartupCpuReady(
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t state = getStartupSnapshot();
	return state.m_nGeneration == nStartupGeneration && state.m_eStage >= T3D_STARTUP_CPU_READY;
}

bool Terrain3DData::isStartupCommitted(
	const uint32_t nStartupGeneration ) const {
	const t3dStartupDomainSnapshot_t state = getStartupSnapshot();
	return state.m_nGeneration == nStartupGeneration && state.m_eStage == T3D_STARTUP_COMMITTED;
}

int Terrain3DData::getGpuRegionRequiredCorePages(
	void ) const {
	const int radius = m_residency.gpuRegionCache().getRequiredRadius();
	const int diameter = radius * 2 + 1;
	return MIN( diameter * diameter, m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
}

void Terrain3DData::setColor(
	const Vector3 &rGlobalPosition,
	const Color	  &rColor ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	Color clr = rColor;
	clr.a = getRoughness( rGlobalPosition );
	if ( !isfinite( clr.a ) ) {
		return;
	}
	setPixel( TYPE_COLOR, rGlobalPosition, clr );
}

Color Terrain3DData::getColor(
	const Vector3 &rGlobalPosition ) const {
	Color clr = getPixel( TYPE_COLOR, rGlobalPosition );
	clr.a = 1.0f;
	return clr;
}

void Terrain3DData::setControlBaseId(
	const Vector3 &rGlobalPosition,
	const uint8_t  nBase ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	const uint8_t base = CLAMP( nBase, ( uint8_t ) ( 0 ), ( uint8_t ) ( 31 ) );
	setControl( rGlobalPosition, ( control & ~( 0x1Fu << 27 ) ) | t3dEncBase( base ) );
}

uint32_t Terrain3DData::getControlBaseId(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? UINT32_MAX : t3dGetBase( control );
}

void Terrain3DData::setControlOverlayId(
	const Vector3 &rGlobalPosition,
	const uint8_t  nOverlay ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	const uint8_t overlay = CLAMP( nOverlay, ( uint8_t ) ( 0 ), ( uint8_t ) ( 31 ) );
	setControl( rGlobalPosition, ( control & ~( 0x1F << 22 ) ) | t3dEncOverlay( overlay ) );
}

uint32_t Terrain3DData::getControlOverlayId(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? UINT32_MAX : t3dGetOverlay( control );
}

void Terrain3DData::setControlBlend(
	const Vector3 &rGlobalPosition,
	const real_t   fBlend ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	const uint8_t blend = ( uint8_t ) ( CLAMP( Math::round( fBlend * 255.f ), 0.f, 255.f ) );
	setControl( rGlobalPosition, ( control & ~( 0xFF << 14 ) ) | t3dEncBlend( blend ) );
}

real_t Terrain3DData::getControlBlend(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? NAN : ( real_t ) ( t3dGetBlend( control ) ) / 255.f;
}

void Terrain3DData::setControlAngle(
	const Vector3 &rGlobalPosition,
	const real_t   fAngle ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	const uint8_t uvrotation = ( uint8_t ) ( CLAMP( Math::round( fAngle / 22.5f ), 0.f, 15.f ) );
	setControl( rGlobalPosition, ( control & ~( 0xF << 10 ) ) | t3dEncUvRotation( uvrotation ) );
}

real_t Terrain3DData::getControlAngle(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	const real_t   fAngle = ( real_t ) ( t3dGetUvRotation( control ) ) * 22.5f;
	return control == UINT32_MAX ? NAN : fAngle;
}

void Terrain3DData::setControlScale(
	const Vector3 &rGlobalPosition,
	const real_t   fScale ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	eastl::array< uint32_t, 8 > scale_align = { 5, 6, 7, 0, 1, 2, 3, 4 };
	const uint8_t uvscale = scale_align[( uint8_t ) ( CLAMP( Math::round( ( fScale + 60.f ) / 20.f ), 0.f, 7.f ) )];
	setControl( rGlobalPosition, ( control & ~( 0x7 << 7 ) ) | t3dEncUvScale( uvscale ) );
}

real_t Terrain3DData::getControlScale(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t			  control = getControl( rGlobalPosition );
	eastl::array< real_t, 8 > scale_values = { 0.0f, 20.0f, 40.0f, 60.0f, 80.0f, -60.0f, -40.0f, -20.0f };
	const real_t			  fScale = scale_values[t3dGetUvScale( control )]; // select from array UI return values
	return control == UINT32_MAX ? NAN : fScale;
}

void Terrain3DData::setControlHole(
	const Vector3 &rGlobalPosition,
	const bool	   bHole ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	setControl( rGlobalPosition, ( control & ~( 0x1 << 2 ) ) | t3dEncHole( bHole ) );
}

bool Terrain3DData::getControlHole(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? false : t3dIsHole( control );
}

void Terrain3DData::setControlNavigation(
	const Vector3 &rGlobalPosition,
	const bool	   bNavigation ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	setControl( rGlobalPosition, ( control & ~( 0x1 << 1 ) ) | t3dEncNav( bNavigation ) );
}

bool Terrain3DData::getControlNavigation(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? false : t3dIsNav( control );
}

void Terrain3DData::setControlAuto(
	const Vector3 &rGlobalPosition,
	const bool	   bAuto ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	const uint32_t control = getControl( rGlobalPosition );
	if ( control == UINT32_MAX ) {
		return;
	}
	setControl( rGlobalPosition, ( control & ~( 0x1 ) ) | t3dEncAuto( bAuto ) );
}

bool Terrain3DData::getControlAuto(
	const Vector3 &rGlobalPosition ) const {
	const uint32_t control = getControl( rGlobalPosition );
	return control == UINT32_MAX ? false : t3dIsAuto( control );
}

void Terrain3DData::setRoughness(
	const Vector3 &rGlobalPosition,
	const real_t   fRoughness ) {
	if ( !rGlobalPosition.is_finite() || !ensureCpuRegionResident( getRegionLocation( rGlobalPosition ) ) ) {
		return;
	}
	Color clr = getPixel( TYPE_COLOR, rGlobalPosition );
	if ( !isfinite( clr.r ) || !isfinite( clr.g ) || !isfinite( clr.b ) ) {
		return;
	}
	clr.a = fRoughness;
	setPixel( TYPE_COLOR, rGlobalPosition, clr );
}

Ref< Terrain3DAssets > Terrain3DData::getErosionAssets(
	void ) const {
	return m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
}

Ref< Terrain3DMaterial > Terrain3DData::getErosionMaterial(
	void ) const {
	return m_pTerrain ? m_pTerrain->materialResource() : Ref< Terrain3DMaterial >();
}
