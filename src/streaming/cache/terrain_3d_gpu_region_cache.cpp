#include "core/utility/terrain_3d_object_bytes.h"
#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include "rendering/rd/core/terrain_3d_gpu_transfer_budget.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "diagnostics/core/terrain_3d_diagnostics.h"
#include "core/utility/terrain_3d_generation.h"
#include "Terrain3DTypeSupport.h"
#include "streaming/residency/terrain_3d_region_residency_primitives.h"
#include "streaming/cache/terrain_3d_gpu_region_cache.h"
#include "streaming/utility/terrain_3d_scope_timer.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "core/utility/logger.h"
#include "data/editing/terrain_3d_map_edit_service.h"
#include "data/cooking/terrain_3d_control_map_cooker.h"
#include "data/cooking/terrain_3d_region_image_cooker.h"
#include "data/model/terrain_3d_region.h"
#include "data/model/terrain_3d_data.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "eastl_config.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "rendering/material/terrain_3d_material.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"
#include "streaming/residency/terrain_3d_region_candidate_collector.h"
#include "terrain_3d.h"

#include <EASTL/numeric_limits.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static_assert( T3D_GPU_CACHE_MAX_LAYERS <= ( int ) ( T3D_TRACKED_BATCH_SLOT_COUNT ) );
static_assert( T3D_TRACKED_BATCH_UPDATE_COUNT >= 6u );

static int t3dGpuRegionCacheMaterialIntParam(
	Terrain3DMaterial *pMaterial,
	const char		  *pszName,
	const int		   nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int ) ( value );
}

EA_FORCE_INLINE static size_t t3dGpuRegionCacheGpuImageBytes(
	const Ref< Image > &rImage ) {
	return rImage.is_valid() ? ( size_t ) ( rImage->get_data().size() ) : 0u;
}

Terrain3DGpuRegionCache::Terrain3DGpuRegionCache(
	Terrain3DRegionRepository &rRepository,
	Terrain3DSamplingStorage  &rSamplingStorage,
	Terrain3DMapEditService	  &rMapEditService,
	Terrain3DRuntimePageStore &rRuntimePageStore,
	Terrain3DCpuRegionCache	  &rCpuRegionCache ) :
	m_regionRepository( rRepository ),
	m_samplingStorage( rSamplingStorage ),
	m_mapEditService( rMapEditService ),
	m_runtimePageStore( rRuntimePageStore ),
	m_cpuRegionCache( rCpuRegionCache ) {
	m_generatedHeightMaps.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedControlMaps.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedColorMaps.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedDerivedMaps.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedMiddleCompositeMaps.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedTerrainShadowMasks.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	for ( int i = 0; i < T3D_GPU_CACHE_MAX_LAYERS; ++i ) {
		m_szGpuRegionLocations[i] = t3dInvalidRegionCoord();
	}
	for ( int i = 0; i < T3D_GPU_CACHE_MAX_LAYERS; ++i ) {
		m_szGpuRegionPriorityLocations[i] = t3dInvalidRegionCoord();
	}
}

void Terrain3DGpuRegionCache::setDefaultMaps(
	const Ref< Image > &rHeight,
	const Ref< Image > &rControl,
	const Ref< Image > &rColor,
	const Ref< Image > &rDerived,
	const bool			bControlImplicit,
	const bool			bColorImplicit ) {
	const bool layoutChanged = bControlImplicit != m_bControlMapsImplicit || bColorImplicit != m_bColorMapsImplicit;
	m_defaultHeightMap = rHeight;
	m_defaultControlMap = rControl;
	m_defaultColorMap = rColor;
	m_defaultDerivedMap = rDerived;
	m_bControlMapsImplicit = bControlImplicit;
	m_bColorMapsImplicit = bColorImplicit;
	if ( layoutChanged && m_bGpuRegionCacheEnabled ) {
		reset( true );
	}
}

Ref< Image > Terrain3DGpuRegionCache::getRegionMapOrDefault(
	const Terrain3DRegion *pRegion,
	const t3dMapType_t	   eMapType ) const {
	if ( m_pTerrain && pRegion && pRegion->getMap( eMapType ).is_null() ) {
		const Terrain3DData *const pData = m_pTerrain->dataObject();
		if ( pData ) {
			const Ref< Image > cold = pData->getCookedSparseRegionChannel( pRegion, eMapType );
			if ( cold.is_valid() ) {
				return cold;
			}
			const Ref< Image > fallback = pData->getCookedSparseVisualFallback( pRegion, eMapType );
			if ( fallback.is_valid() ) {
				return fallback;
			}
		}
	}
	return t3dRegionImageResolveMap( pRegion, eMapType, m_defaultHeightMap, m_defaultControlMap, m_defaultColorMap );
}

Ref< Image > Terrain3DGpuRegionCache::makeGpuColorImage(
	const Ref< Image > &rImage ) const {
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	return t3dRegionImageCookGpuColor( rImage, m_defaultColorMap, divisor );
}

Ref< Image > Terrain3DGpuRegionCache::makeGpuDerivedImage(
	const Ref< Image > &rImage ) const {
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	return t3dRegionImageCookGpuDerived( rImage, m_defaultDerivedMap, divisor );
}

bool Terrain3DGpuRegionCache::refreshMiddleCompositeSources(
	const uint64_t nMaterialSignature,
	const int	   nWidth,
	const int	   nHeight ) {
	if ( nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	const int64_t scratch_bytes_64 = ( int64_t ) ( nWidth ) *nHeight * 4;
	if ( scratch_bytes_64 <= 0 || scratch_bytes_64 > INT32_MAX ) {
		return false;
	}
	const int scratch_bytes = ( int ) ( scratch_bytes_64 );
	const int scratch_layers = 1;
	if ( nMaterialSignature != m_nMiddleCompositeMaterialSignature ) {
		const Ref< Terrain3DAssets > pAssets = m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
		t3dRegionImagePrepareMiddleCompositeMaterials( pAssets,
													   m_middleCompositeMaterialColors,
													   m_middleCompositeMaterialScales,
													   m_middleCompositeMaterialImages,
													   T3D_ARRAY_COUNT( m_middleCompositeMaterialImages ) );
		m_nMiddleCompositeMaterialSignature = nMaterialSignature;
	}
	if ( scratch_bytes != m_nMiddleCompositeScratchBytes || scratch_layers != m_nMiddleCompositeScratchLayerCount ) {
		for ( int layer = scratch_layers; layer < m_nMiddleCompositeScratchLayerCount; ++layer ) {
			m_middleCompositeScratch[( size_t ) ( layer )].clear();
		}
		for ( int layer = 0; layer < scratch_layers; ++layer ) {
			m_middleCompositeScratch[( size_t ) ( layer )].resize( scratch_bytes );
			if ( m_middleCompositeScratch[( size_t ) ( layer )].size() != scratch_bytes ) {
				return false;
			}
		}
		m_nMiddleCompositeScratchBytes = scratch_bytes;
		m_nMiddleCompositeScratchLayerCount = scratch_layers;
	}
	return true;
}

Ref< Image > Terrain3DGpuRegionCache::makeMiddleCompositeImage(
	const Vector2i	   &rLocation,
	const Ref< Image > &rControl,
	const Ref< Image > &rColor,
	const int			nScratchSlot ) {
	const int scratch_slot = CLAMP( nScratchSlot, 0, MAX( 0, m_nMiddleCompositeScratchLayerCount - 1 ) );
	if ( scratch_slot >= m_nMiddleCompositeScratchLayerCount ) {
		return Ref< Image >();
	}
	PackedByteArray &output = m_middleCompositeScratch[( size_t ) ( scratch_slot )];
	return t3dRegionImageCookMiddleComposite( rLocation,
											  rControl,
											  rColor,
											  m_middleCompositeMaterialColors,
											  m_middleCompositeMaterialScales,
											  m_middleCompositeMaterialImages,
											  T3D_ARRAY_COUNT( m_middleCompositeMaterialImages ),
											  output );
}

Ref< Image > Terrain3DGpuRegionCache::makeTerrainShadowMaskImage(
	const Terrain3DRegion *const pRegion ) const {
	return t3dRegionImageCookTerrainShadowMask( pRegion, T3D_REGION_SIZE );
}

Ref< Image > Terrain3DGpuRegionCache::makeGpuRenderControlImage(
	const Vector2i &vLocation ) const {
	return t3dControlCookNeighborhoodImage(
		vLocation,
		[&]( const Vector2i &location, t3dControlCookSource_t &source ) -> bool {
			const Terrain3DRegion *pRegion = m_regionRepository.getRegionPtr( location );
			Ref< Terrain3DRegion > pLoadedSource;
			const bool			   bNeedsSourceLoad = !pRegion || pRegion->isDeleted() ||
										  ( pRegion->hasChannel( TYPE_CONTROL ) && pRegion->getControlMap().is_null() );
			if ( bNeedsSourceLoad && m_cpuRegionCache.loadSource( location, pLoadedSource, false ) ) {
				pRegion = pLoadedSource.ptr();
			}
			if ( !pRegion || pRegion->isDeleted() ) {
				return true;
			}
			Ref< Image > resolvedControl = pRegion->getControlMap();
			if ( pRegion->hasChannel( TYPE_CONTROL ) && resolvedControl.is_null() ) {
				const Terrain3DData *const pData = m_pTerrain ? m_pTerrain->dataObject() : NULL;
				if ( pData ) {
					resolvedControl = pData->getCookedSparseRegionChannel( pRegion, TYPE_CONTROL );
				}
				if ( pData && resolvedControl.is_null() ) {
					resolvedControl = pData->getCookedSparseVisualFallback( pRegion, TYPE_CONTROL );
				}
				if ( resolvedControl.is_null() ) {
					m_cpuRegionCache.requestSourceAsync( location );
					return false;
				}
			}
			source.m_bPresent = true;
			if ( !pRegion->hasChannel( TYPE_CONTROL ) || resolvedControl.is_null() ) {
				return true;
			}
			if ( !t3dControlCookSetExplicitSource( source, resolvedControl ) ) {
				return false;
			}
			return true;
		} );
}

int Terrain3DGpuRegionCache::calculateRequiredCorePagesForLods(
	const int nDetailLods ) const {
	if ( !m_pTerrain || T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	int		  detail_lods = nDetailLods;
	if ( detail_lods <= 0 ) {
		detail_lods = MAX( 1, m_pTerrain->getRuntimeNearShaderLods() );
	}
	detail_lods = CLAMP( detail_lods, 1, total_lods );
	const int radius = t3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(), detail_lods );
	const int diameter = radius * 2 + 1;
	int		  required_pages = MIN( diameter * diameter, T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE );
	const int active_regions = m_regionRepository.getActiveRegionCount();
	if ( active_regions > 0 ) {
		required_pages = MIN( required_pages, active_regions );
	}
	return required_pages;
}

uint64_t Terrain3DGpuRegionCache::calculateAuthoringGpuRegionBudgetBytes(
	void ) const {
	// m_nGpuRegionCacheBudgetBytes is already the exact effective allocation produced by
	// Terrain3D's aggregate memory-budget coordinator. Authoring may use more
	// physical layers than runtime, but it must fit those layers inside that same
	// coordinated GPU allocation instead of manufacturing extra editor memory.
	const uint64_t hard_cap = ( uint64_t ) ( T3D_EDITOR_AUTHORING_GPU_REGION_MAX_BUDGET_MB ) *T3D_GPU_CACHE_MIB;
	return m_nGpuRegionCacheBudgetBytes < hard_cap ? m_nGpuRegionCacheBudgetBytes : hard_cap;
}

int Terrain3DGpuRegionCache::calculateCapacityForLods(
	const int nDetailLods ) const {
	if ( !m_pTerrain || T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 ) {
		return 0;
	}
	( void ) ( nDetailLods );
	// Allocation is budget-driven. The physical page ceiling is an authored owner
	// policy, not a compile-time runtime constant. Editor Authoring retains its
	// independent ceiling so the same byte grant can be inspected at a wider page
	// count without changing shipping defaults.
	const bool bAuthoring = m_pTerrain->isEditorAuthoringMode();
	const int  maximum_layers =
		 bAuthoring ? CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, T3D_GPU_CACHE_EDITOR_MAX_LAYERS )
					: CLAMP( m_pTerrain->getGpuRegionCachePageLimit(), 16, T3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	const uint64_t budget_bytes = bAuthoring ? calculateAuthoringGpuRegionBudgetBytes() : m_nGpuRegionCacheBudgetBytes;
	const int	   requested = MIN( m_regionRepository.getActiveRegionCount(), maximum_layers );
	const int	   capacity = t3dGpuCacheCalculateCapacity( budget_bytes,
														maximum_layers,
														requested,
														m_bControlMapsImplicit,
														!isColorChannelEnabled(),
														isDerivedChannelEnabled(),
														m_pTerrain->getGpuRegionColorResolutionDivisor(),
														m_pTerrain->getGpuRegionDerivedResolutionDivisor(),
														m_nGpuRegionSparePages );
	return capacity;
}

int Terrain3DGpuRegionCache::calculateGpuRegionCacheCapacity(
	void ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		const int page_limit = CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, T3D_GPU_CACHE_EDITOR_MAX_LAYERS );
		return t3dGpuCacheCalculateCapacity( calculateAuthoringGpuRegionBudgetBytes(),
											 page_limit,
											 MIN( m_regionRepository.getActiveRegionCount(), page_limit ),
											 m_bControlMapsImplicit,
											 !isColorChannelEnabled(),
											 isDerivedChannelEnabled(),
											 m_pTerrain->getGpuRegionColorResolutionDivisor(),
											 m_pTerrain->getGpuRegionDerivedResolutionDivisor(),
											 m_nGpuRegionSparePages );
	}
	const int page_limit = m_pTerrain
							   ? CLAMP( m_pTerrain->getGpuRegionCachePageLimit(), 16, T3D_GPU_CACHE_RUNTIME_MAX_LAYERS )
							   : T3D_GPU_CACHE_RUNTIME_MAX_LAYERS;
	const int allocation_request = MIN( m_regionRepository.getActiveRegionCount(), page_limit );
	return t3dGpuCacheCalculateCapacity( m_nGpuRegionCacheBudgetBytes,
										 page_limit,
										 allocation_request,
										 m_bControlMapsImplicit,
										 !isColorChannelEnabled(),
										 isDerivedChannelEnabled(),
										 m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
										 m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1,
										 m_nGpuRegionSparePages );
}

int Terrain3DGpuRegionCache::calculateGpuRegionPhysicalLayers(
	const int nMappedCapacity ) const {
	bool bAuthoring = false;
	int	 maximum_layers = T3D_GPU_CACHE_RUNTIME_MAX_LAYERS;
	if ( m_pTerrain ) {
		bAuthoring = m_pTerrain->isEditorAuthoringMode();
		maximum_layers = bAuthoring
							 ? CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, T3D_GPU_CACHE_EDITOR_MAX_LAYERS )
							 : CLAMP( m_pTerrain->getGpuRegionCachePageLimit(), 16, T3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	}
	const uint64_t budget_bytes = bAuthoring ? calculateAuthoringGpuRegionBudgetBytes() : m_nGpuRegionCacheBudgetBytes;
	const int	   budget_layers =
		t3dGpuCacheCalculateBudgetPhysicalLayers( budget_bytes,
												  maximum_layers,
												  m_bControlMapsImplicit,
												  !isColorChannelEnabled(),
												  isDerivedChannelEnabled(),
												  m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
												  m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
	return t3dGpuCacheCalculatePhysicalLayers( nMappedCapacity, m_nGpuRegionSparePages, maximum_layers, budget_layers );
}

int Terrain3DGpuRegionCache::calculateGpuRegionRequiredRadius(
	void ) const {
	if ( !m_pTerrain ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	int		  detail_lods = m_pTerrain->getGpuRegionDetailLods();
	if ( detail_lods <= 0 ) {
		detail_lods = MAX( 1, m_pTerrain->getRuntimeNearShaderLods() );
	}
	detail_lods = CLAMP( detail_lods, 1, total_lods );
	return t3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(), detail_lods );
}

int Terrain3DGpuRegionCache::calculateGpuRegionRequestedPages(
	void ) const {
	if ( !m_pTerrain || m_regionRepository.getActiveRegionCount() <= 0 ) {
		return 0;
	}
	if ( m_pTerrain->isEditorAuthoringMode() ) {
		return MIN( m_regionRepository.getActiveRegionCount(), m_pTerrain->getEditorGpuRegionCachePages() );
	}
	const Dictionary streaming = m_pTerrain->getRegionStreamingStats();
	const int		 active = ( int ) ( streaming["active"] );
	const int		 hot = ( int ) ( streaming["hot"] );
	const int		 predicted = ( int ) ( streaming["predicted_promotions"] );
	return MIN( m_regionRepository.getActiveRegionCount(), active + hot + predicted );
}

size_t Terrain3DGpuRegionCache::estimateLayerBytes(
	void ) const {
	return t3dGpuCacheEstimateResidentLayerBytes( m_bControlMapsImplicit,
												  !isColorChannelEnabled(),
												  isDerivedChannelEnabled(),
												  m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
												  m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
}

bool Terrain3DGpuRegionCache::isColorChannelEnabled(
	void ) const {
	if ( m_bColorMapsImplicit ) {
		return false;
	}
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref< Terrain3DMaterial > pMaterial = m_pTerrain->materialResource();
	if ( pMaterial.is_null() ) {
		return true;
	}
	const bool near_needed =
		m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		( pMaterial->getNearAuthoredColorContribution() > 0.0f || pMaterial->getNearWetnessContribution() > 0.0f );
	const bool middle_needed =
		m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		( pMaterial->getMiddleAuthoredColorContribution() > 0.0f || pMaterial->getMiddleWetnessContribution() > 0.0f );
	return near_needed || middle_needed;
}

bool Terrain3DGpuRegionCache::isDerivedChannelEnabled(
	void ) const {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref< Terrain3DMaterial > pMaterial = m_pTerrain->materialResource();
	if ( pMaterial.is_null() ) {
		return true;
	}
	return ( m_pTerrain->getRuntimeNearShaderLods() > 0 &&
			 pMaterial->getNearTerrainNormalQuality() == TERRAIN_NORMAL_QUALITY_DETAILED ) ||
		   ( m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
			 pMaterial->getMiddleTerrainNormalQuality() == TERRAIN_NORMAL_QUALITY_DETAILED );
}

size_t Terrain3DGpuRegionCache::estimateSharedBytes(
	void ) const {
	return t3dGpuCacheEstimateSharedBytes( m_bControlMapsImplicit,
										   !isColorChannelEnabled(),
										   m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 );
}

int Terrain3DGpuRegionCache::estimateGpuRegionUploadCalls(
	const Terrain3DRegion *pRegion,
	const int			   nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	int calls = 3; // Height, middle composite, and terrain shadowmask are always published for a complete page.
	if ( isDerivedChannelEnabled() ) {
		++calls;
	}
	if ( !m_bControlMapsImplicit &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_CONTROL ) ) ) {
		++calls;
	}
	if ( isColorChannelEnabled() &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_COLOR ) ) ) {
		++calls;
	}
	return calls;
}

size_t Terrain3DGpuRegionCache::estimateGpuRegionUploadBytes(
	const Terrain3DRegion *pRegion,
	const int			   nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0u;
	}
	const size_t full_texels = t3dGpuCacheTexelCount( 1 );
	const size_t derived_texels =
		t3dGpuCacheTexelCount( m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
	if ( full_texels == 0u || derived_texels == 0u ) {
		return 0u;
	}
	// The composite uses the same color divisor as the physical composite array.
	size_t bytes = full_texels * 2u +
				   t3dGpuCacheColorBytes( m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 ) +
				   t3dGpuCacheShadowBytes( 1 );
	if ( isDerivedChannelEnabled() ) {
		bytes += derived_texels * 2u;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	if ( !m_bControlMapsImplicit &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_CONTROL ) ) ) {
		bytes += full_texels * 4u;
	}
	if ( isColorChannelEnabled() &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_COLOR ) ) ) {
		bytes += t3dGpuCacheColorBytes( m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 );
	}
	return bytes;
}

int Terrain3DGpuRegionCache::getGpuRegionTextureUpdateCallsLastTick(
	void ) const {
	return m_nGpuRegionHeightUpdatesLastTick + m_nGpuRegionControlUpdatesLastTick + m_nGpuRegionColorUpdatesLastTick +
		   m_nGpuRegionDerivedUpdatesLastTick + m_nGpuRegionMiddleCompositeUpdatesLastTick +
		   m_nGpuRegionTerrainShadowUpdatesLastTick + m_nGpuRegionPageTableUpdatesLastTick;
}

bool Terrain3DGpuRegionCache::reserveGpuRegionUpload(
	const Terrain3DRegion *pRegion,
	const int			   nLayer ) {
	const uint64_t page_table_bytes =
		m_bPageTableUploadReserved ? 0u : ( uint64_t ) T3D_REGION_SLOTS * sizeof( t3dRdTerrainRegionGpu_t );
	const uint64_t bytes = estimateGpuRegionUploadBytes( pRegion, nLayer ) + page_table_bytes;
	const uint64_t calls = estimateGpuRegionUploadCalls( pRegion, nLayer ) + ( m_bPageTableUploadReserved ? 0u : 1u );
	if ( bytes > m_nGpuRegionUploadByteTokens ) {
		m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET;
		return false;
	}
	if ( calls > m_nGpuRegionUploadCallTokens ) {
		m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET;
		return false;
	}
	m_nGpuRegionUploadByteTokens -= bytes;
	m_nGpuRegionUploadCallTokens -= calls;
	m_bPageTableUploadReserved = true;
	return true;
}

bool Terrain3DGpuRegionCache::submitBudgetedGpuRegionUpload(
	const int		nLayer,
	const Vector2i &vLocation,
	const uint8_t	dirty_maps,
	const int		copy_from_layer ) {
	// Bound consecutive CPU preparations as well as GPU upload volume.
	if ( m_nGpuRegionRuntimePreparesLastTick > 0 && m_nGpuPreparationTickStartedUsec != 0u &&
		 Time::get_singleton()->get_ticks_usec() - m_nGpuPreparationTickStartedUsec >= 2000u ) {
		m_bGpuPreparationDeferred = true;
		m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING;
		return false;
	}
	Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	const bool	   page_table_reserved_before = m_bPageTableUploadReserved;
	const uint64_t expected_bytes = estimateGpuRegionUploadBytes( pRegion, nLayer );
	const uint64_t expected_calls = estimateGpuRegionUploadCalls( pRegion, nLayer );
	if ( !reserveGpuRegionUpload( pRegion, nLayer ) ) {
		return false;
	}
	t3dGpuTransferTicket_t transfer_ticket;
	const uint64_t		   page_table_bytes =
		page_table_reserved_before ? 0u : ( uint64_t ) T3D_REGION_SLOTS * sizeof( t3dRdTerrainRegionGpu_t );
	if ( !Terrain3DGpuTransferBudget::get().reserve(
			 T3D_GPU_TRANSFER_REGION,
			 expected_bytes + page_table_bytes,
			 ( uint32_t ) ( expected_calls + ( page_table_reserved_before ? 0u : 1u ) ),
			 transfer_ticket ) ) {
		m_nGpuRegionUploadByteTokens += expected_bytes + page_table_bytes;
		m_nGpuRegionUploadCallTokens += expected_calls + ( page_table_reserved_before ? 0u : 1u );
		m_bPageTableUploadReserved = page_table_reserved_before;
		m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET;
		return false;
	}
	const uint64_t bytes_before = m_nGpuRegionUploadBytesLastTick;
	const int	   calls_before = getGpuRegionTextureUpdateCallsLastTick();
	const bool	   uploaded =
		uploadGpuRegionLayer( nLayer, vLocation, REGION_TYPE_MAX, copy_from_layer < 0, dirty_maps, copy_from_layer );
	const uint64_t actual_bytes = m_nGpuRegionUploadBytesLastTick - bytes_before;
	const uint64_t actual_calls = ( uint64_t ) MAX( 0, getGpuRegionTextureUpdateCallsLastTick() - calls_before );
	// A not-yet-prepared page submits no GPU work. Return unused credit rather
	// than repeatedly charging a complete page while its CPU worker is pending.
	if ( expected_bytes > actual_bytes ) {
		m_nGpuRegionUploadByteTokens += expected_bytes - actual_bytes;
	}
	if ( expected_calls > actual_calls ) {
		m_nGpuRegionUploadCallTokens += expected_calls - actual_calls;
	}
	if ( !uploaded && !page_table_reserved_before ) {
		m_nGpuRegionUploadByteTokens += ( uint64_t ) T3D_REGION_SLOTS * sizeof( t3dRdTerrainRegionGpu_t );
		++m_nGpuRegionUploadCallTokens;
		m_bPageTableUploadReserved = false;
	}
	// The layer/page-table upload is an indivisible logical reservation. Refund
	// only work that demonstrably never reached a renderer submission.
	const uint64_t charged_bytes =
		actual_bytes + ( m_bPageTableUploadReserved && !page_table_reserved_before ? page_table_bytes : 0u );
	const uint32_t charged_calls =
		( uint32_t ) actual_calls + ( m_bPageTableUploadReserved && !page_table_reserved_before ? 1u : 0u );
	Terrain3DGpuTransferBudget::get().refund(
		transfer_ticket,
		transfer_ticket.bytes() > charged_bytes ? transfer_ticket.bytes() - charged_bytes : 0u,
		transfer_ticket.calls() > charged_calls ? transfer_ticket.calls() - charged_calls : 0u );
	if ( uploaded || actual_bytes != 0u || actual_calls != 0u ) {
		++m_nGpuRegionUploadsLastTick;
	}
	return uploaded;
}

void Terrain3DGpuRegionCache::trimGpuRegionCapacity(
	void ) {
	while ( m_nGpuRegionResidentCount > m_nGpuRegionCacheCapacity ) {
		int victim = -1;
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
			if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
				 m_szGpuRegionVictimReserved[layer] == 0u &&
				 ( victim < 0 || m_szGpuRegionLastUseTick[layer] < m_szGpuRegionLastUseTick[victim] ) ) {
				victim = layer;
			}
		}
		if ( victim < 0 ) {
			break;
		} // Pending replacements retain their old mapping until completion.
		invalidate( t3dUnpackRegionCoord( m_szGpuRegionLocations[victim] ) );
		++m_nGpuRegionEvictionsLastTick;
	}
}

void Terrain3DGpuRegionCache::refillGpuUploadTokens(
	const int nCallBurst ) {
	const uint64_t now = t3dSteadyClockUsec();
	const uint64_t frame_bytes =
		( uint64_t ) ( MAX( 1, m_nGpuRegionUploadBudgetMb ) ) * ( uint64_t ) ( T3D_GPU_CACHE_MIB );
	// Accumulate enough credit for one atomic page when it exceeds a single
	// frame allowance; refill rate remains the configured byte budget.
	const uint64_t byte_burst =
		MAX( frame_bytes,
			 ( uint64_t ) estimateLayerBytes() + ( uint64_t ) T3D_REGION_SLOTS * sizeof( t3dRdTerrainRegionGpu_t ) );
	const uint64_t call_burst = ( uint64_t ) ( MAX( 1, nCallBurst ) );
	if ( m_nGpuRegionUploadTokenRefillUsec == 0u || now <= m_nGpuRegionUploadTokenRefillUsec ) {
		m_nGpuRegionUploadByteTokens = frame_bytes;
		m_nGpuRegionUploadCallTokens = call_burst;
		m_nGpuRegionUploadTokenRefillUsec = now;
		return;
	}
	const uint64_t elapsed = now - m_nGpuRegionUploadTokenRefillUsec;
	const uint64_t byte_rate = frame_bytes * 60u;
	const uint64_t call_rate = call_burst * 60u;
	const uint64_t refill_usec = MIN( elapsed, 1000000ull );
	const uint64_t added_bytes = ( byte_rate * refill_usec ) / 1000000ull;
	const uint64_t added_calls = ( call_rate * refill_usec ) / 1000000ull;
	m_nGpuRegionUploadByteTokens = MIN( byte_burst, m_nGpuRegionUploadByteTokens + added_bytes );
	m_nGpuRegionUploadCallTokens = MIN( call_burst, m_nGpuRegionUploadCallTokens + added_calls );
	m_nGpuRegionUploadTokenRefillUsec = now;
}

void Terrain3DGpuRegionCache::reset(
	const bool bClearTextures ) {
	if ( bClearTextures && m_bGpuRegionCacheTexturesReady ) {
		++m_nStorageResetCount;
	}
	memset( m_szGpuRegionMap, 0, sizeof( m_szGpuRegionMap ) );
	for ( int i = 0; i < T3D_GPU_CACHE_MAX_LAYERS; ++i ) {
		m_szGpuRegionLocations[i] = t3dInvalidRegionCoord();
	}
	memset( m_szGpuRegionLastUseTick, 0, sizeof( m_szGpuRegionLastUseTick ) );
	memset( m_szGpuRegionLoadedTick, 0, sizeof( m_szGpuRegionLoadedTick ) );
	memset( m_szGpuRegionRetireAfterFrame, 0, sizeof( m_szGpuRegionRetireAfterFrame ) );
	memset( m_szGpuRegionSlotStates, TERRAIN3D_GPU_SLOT_FREE, sizeof( m_szGpuRegionSlotStates ) );
	memset( m_szGpuRegionPrefetchOrigin, 0, sizeof( m_szGpuRegionPrefetchOrigin ) );
	memset( m_szGpuRegionLayerChannelFlags, 0, sizeof( m_szGpuRegionLayerChannelFlags ) );
	memset( m_szGpuRegionMaterialMasks, 0, sizeof( m_szGpuRegionMaterialMasks ) );
	memset( m_szGpuRegionVictimReserved, 0, sizeof( m_szGpuRegionVictimReserved ) );
	for ( int layer = 0; layer < T3D_GPU_CACHE_MAX_LAYERS; ++layer ) {
		clearPendingGpuRegionUpload( layer );
	}
	m_nGpuRegionPendingUnmappedReservations = 0;
	for ( int i = 0; i < T3D_GPU_CACHE_MAX_LAYERS; ++i ) {
		m_szGpuRegionPriorityLocations[i] = t3dInvalidRegionCoord();
	}
	memset( m_szGpuRegionPriorityClasses, 0, sizeof( m_szGpuRegionPriorityClasses ) );
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_gpuTerrainShadowMasks.clear();
	m_nGpuRegionCacheCapacity = 0;
	m_nGpuRegionPhysicalLayerCount = 0;
	m_nGpuRegionSpareLayerCount = 0;
	m_nGpuRegionResidentCount = 0;
	m_nGpuRegionRetiredCount = 0;
	m_nGpuRegionDesiredLastTick = 0;
	m_nGpuRegionRequiredLastTick = 0;
	m_nGpuRegionGuardLastTick = 0;
	m_nGpuRegionPrefetchLastTick = 0;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	m_nGpuRegionRequestedPages = 0;
	m_nGpuRegionRequiredRadius = 0;
	m_nGpuRegionUploadsLastTick = 0;
	m_nGpuRegionUploadsSinceReset = 0u;
	m_nGpuRegionEvictionsLastTick = 0;
	m_nGpuRegionCookedHitsLastTick = 0;
	m_nGpuRegionCookedMissesLastTick = 0;
	m_nGpuRegionRuntimePreparesLastTick = 0;
	m_nGpuRegionReusedLayersLastTick = 0;
	m_nGpuRegionPageTableUpdatesLastTick = 0;
	m_nGpuRegionHeightUpdatesLastTick = 0;
	m_nGpuRegionControlUpdatesLastTick = 0;
	m_nGpuRegionColorUpdatesLastTick = 0;
	m_nGpuRegionDerivedUpdatesLastTick = 0;
	m_nGpuRegionMiddleCompositeUpdatesLastTick = 0;
	m_nGpuRegionTerrainShadowUpdatesLastTick = 0;
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	m_nGpuRegionPreparedPagesLastTick = 0;
	m_nGpuRegionPrepareWorkersLastTick = 0;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionMiddleCompositeBytesLastTick = 0u;
	m_nGpuRegionTerrainShadowBytesLastTick = 0u;
	m_nGpuRegionPrefetchUsedTotal = 0u;
	m_nGpuRegionPrefetchDiscardedTotal = 0u;
	m_nGpuRegionResidentTicksTotal = 0u;
	m_nGpuRegionDepartedPagesTotal = 0u;
	m_nGpuRegionTick = 0u;
	m_nGpuRegionLastPublicationTick = 0u;
	m_gpuRegionCacheCenter = s_v2iMax;
	m_gpuRegionPredictionDirection = s_v2iZero;
	m_nGpuRegionStreamingGenerationLast = 0u;
	m_nGpuRegionPriorityCount = 0;
	m_nGpuRegionServiceUsecLast = 0u;
	m_nGpuPreparePublicationUsecLast = 0u;
	m_nGpuPrepareEndToEndUsecLast = 0u;
	m_bPageTableChangedThisTick = false;
	memset( m_szGpuRegionRepublishPending, 0, sizeof( m_szGpuRegionRepublishPending ) );
	m_nGpuRegionRepublishPendingCount = 0;
	m_nGpuRegionRepublishCursor = 0;
	m_bGpuRegionCacheTexturesReady = false;
	m_nTextureRebuildFailures = 0u;
	m_nTextureRebuildRetryUsec = 0u;
	m_bGpuRegionCacheSelectionDirty = true;
	notePageTableChanged();
	if ( bClearTextures ) {
		m_generatedHeightMaps.clear();
		m_generatedControlMaps.clear();
		m_generatedColorMaps.clear();
		m_generatedDerivedMaps.clear();
		m_generatedMiddleCompositeMaps.clear();
		m_generatedTerrainShadowMasks.clear();
	}
}

bool Terrain3DGpuRegionCache::validatePublishedMappings(
	void ) const {
	for ( int map_index = 0; map_index < T3D_REGION_SLOTS; ++map_index ) {
		const int layer = ( int ) ( m_szGpuRegionMap[map_index] ) - 1;
		if ( layer < 0 ) {
			continue;
		}
		if ( layer >= m_nGpuRegionPhysicalLayerCount || m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
			 Terrain3DRegionRepository::getRegionMapIndex( t3dUnpackRegionCoord( m_szGpuRegionLocations[layer] ) ) !=
				 map_index ) {
			return false;
		}
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const int map_index =
			Terrain3DRegionRepository::getRegionMapIndex( t3dUnpackRegionCoord( m_szGpuRegionLocations[layer] ) );
		if ( map_index < 0 || m_szGpuRegionMap[map_index] != ( uint16_t ) ( layer + 1 ) ) {
			return false;
		}
	}
	return true;
}

void Terrain3DGpuRegionCache::notePageTableChanged(
	void ) {
	m_bPageTableChangedThisTick = true;
	m_nPhysicalPageTableGeneration = t3dNextGeneration( m_nPhysicalPageTableGeneration );
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	if ( !validatePublishedMappings() ) {
		LOG( ERROR, "GPU physical page table ownership invariant failed" );
	}
#endif
}

bool Terrain3DGpuRegionCache::rebuildTextures(
	void ) {
	const int capacity = calculateGpuRegionCacheCapacity();
	if ( m_nTextureRebuildFailures == 0u )
		LOG( INFO,
			 "[gpu-cache] Rebuild requested: mapped_capacity=",
			 capacity,
			 "; region_size=",
			 T3D_REGION_SIZE,
			 "; implicit_control=",
			 m_bControlMapsImplicit,
			 "; implicit_color=",
			 m_bColorMapsImplicit );
	if ( capacity <= 0 || m_defaultHeightMap.is_null() ) {
		return false;
	}

	const int	 physical_layers = calculateGpuRegionPhysicalLayers( capacity );
	const size_t budget_bytes = m_nGpuRegionCacheBudgetBytes > ( uint64_t ) ( eastl::numeric_limits< size_t >::max() )
									? eastl::numeric_limits< size_t >::max()
									: ( size_t ) ( m_nGpuRegionCacheBudgetBytes );
	const size_t layer_bytes = estimateLayerBytes();
	const size_t shared_bytes = estimateSharedBytes();
	const size_t estimated_bytes =
		physical_layers > 0 && layer_bytes <= ( SIZE_MAX - shared_bytes ) / ( size_t ) ( physical_layers )
			? shared_bytes + layer_bytes * ( size_t ) ( physical_layers )
			: SIZE_MAX;
	if ( physical_layers < capacity || estimated_bytes > budget_bytes ) {
		LOG( ERROR,
			 "GPU terrain page cache cannot satisfy its exact managed byte grant: mapped=",
			 capacity,
			 ", physical=",
			 physical_layers,
			 ", estimated=",
			 estimated_bytes,
			 ", budget=",
			 budget_bytes );
		return false;
	}

	const uint32_t previous_failures = m_nTextureRebuildFailures;
	const Vector2i previous_center = m_gpuRegionCacheCenter;
	++m_nPhysicalCacheRebuildCount;
	reset( true );
	m_nTextureRebuildFailures = previous_failures;
	m_nGpuRegionCacheCapacity = capacity;
	m_nGpuRegionPhysicalLayerCount = physical_layers;
	m_nGpuRegionSpareLayerCount = MAX( 0, m_nGpuRegionPhysicalLayerCount - capacity );
	m_gpuRegionCacheCenter = previous_center;
	m_nGpuRegionColorDivisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	m_nGpuRegionDerivedDivisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const bool bColorChannelEnabled = isColorChannelEnabled();
	const bool bDerivedChannelEnabled = isDerivedChannelEnabled();

	m_gpuHeightMaps.resize( m_nGpuRegionPhysicalLayerCount );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuHeightMaps[layer] = m_defaultHeightMap;
	}

	// Height is mandatory. Do not build optional channels after its allocation fails.
	const bool bHeightOk = m_generatedHeightMaps.create( m_gpuHeightMaps, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	if ( !bHeightOk ) {
		m_gpuHeightMaps.clear();
		if ( m_nTextureRebuildFailures == 0u ) {
			LOG( ERROR,
				 "Failed to create mandatory GPU height page cache with ",
				 capacity,
				 " mapped pages; retrying with backoff. RD failure=",
				 Terrain3DRdContext::getShared().getLastFailure(),
				 ". Check preceding RD texture allocation errors." );
		}
		return false;
	}

	const bool		   bControlSourceReady = m_defaultControlMap.is_valid();
	const bool		   bColorSourceReady = m_defaultColorMap.is_valid();
	const bool		   bDerivedSourceReady = m_defaultDerivedMap.is_valid();
	const Ref< Image > pDefaultGpuColor = bColorSourceReady ? makeGpuColorImage( m_defaultColorMap ) : Ref< Image >();
	const Ref< Image > pDefaultGpuDerived =
		bDerivedSourceReady ? makeGpuDerivedImage( m_defaultDerivedMap ) : Ref< Image >();
	const int control_layers =
		bControlSourceReady ? ( m_bControlMapsImplicit ? 1 : m_nGpuRegionPhysicalLayerCount ) : 0;
	const int color_layers =
		pDefaultGpuColor.is_valid() ? ( bColorChannelEnabled ? m_nGpuRegionPhysicalLayerCount : 1 ) : 0;
	const int derived_layers =
		pDefaultGpuDerived.is_valid() ? ( bDerivedChannelEnabled ? m_nGpuRegionPhysicalLayerCount : 1 ) : 0;
	m_gpuControlMaps.resize( control_layers );
	m_gpuColorMaps.resize( color_layers );
	m_gpuDerivedMaps.resize( derived_layers );
	for ( int layer = 0; layer < control_layers; ++layer ) {
		m_gpuControlMaps[layer] = m_defaultControlMap;
	}
	for ( int layer = 0; layer < color_layers; ++layer ) {
		m_gpuColorMaps[layer] = pDefaultGpuColor;
	}
	for ( int layer = 0; layer < derived_layers; ++layer ) {
		m_gpuDerivedMaps[layer] = pDefaultGpuDerived;
	}

	Ref< Image > pDefaultMiddleComposite;
	if ( bControlSourceReady && pDefaultGpuColor.is_valid() ) {
		const Ref< Terrain3DAssets > pAssets = m_pTerrain ? m_pTerrain->assetsResource() : Ref< Terrain3DAssets >();
		const uint64_t material_signature = pAssets.is_valid() ? pAssets->getRuntimeMaterialSourceSignature() : 0u;
		if ( refreshMiddleCompositeSources( material_signature,
											pDefaultGpuColor->get_width(),
											pDefaultGpuColor->get_height() ) ) {
			pDefaultMiddleComposite = makeMiddleCompositeImage( s_v2iZero, m_defaultControlMap, pDefaultGpuColor, 0 );
		}
	}
	const Ref< Image > pDefaultTerrainShadow = makeTerrainShadowMaskImage( NULL );
	if ( pDefaultMiddleComposite.is_valid() ) {
		m_gpuMiddleCompositeMaps.resize( m_nGpuRegionPhysicalLayerCount );
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
			m_gpuMiddleCompositeMaps[layer] = pDefaultMiddleComposite;
		}
	} else {
		m_gpuMiddleCompositeMaps.clear();
	}
	if ( pDefaultTerrainShadow.is_valid() ) {
		m_gpuTerrainShadowMasks.resize( m_nGpuRegionPhysicalLayerCount );
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
			m_gpuTerrainShadowMasks[layer] = pDefaultTerrainShadow;
		}
	} else {
		m_gpuTerrainShadowMasks.clear();
	}

	const bool bControlOk =
		control_layers > 0 && m_generatedControlMaps.create( m_gpuControlMaps, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	const bool bColorOk =
		color_layers > 0 && m_generatedColorMaps.create( m_gpuColorMaps, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
	const bool bDerivedOk =
		derived_layers > 0 && m_generatedDerivedMaps.create( m_gpuDerivedMaps, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	const bool bMiddleCompositeOk =
		m_gpuMiddleCompositeMaps.size() > 0 &&
		m_generatedMiddleCompositeMaps.create( m_gpuMiddleCompositeMaps, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
	const bool bTerrainShadowOk =
		m_gpuTerrainShadowMasks.size() > 0 &&
		m_generatedTerrainShadowMasks.create( m_gpuTerrainShadowMasks, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	m_bGpuRegionColorChannelAllocated = bColorOk && bColorChannelEnabled;
	m_bGpuRegionDerivedChannelAllocated = bDerivedOk && bDerivedChannelEnabled;
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_gpuTerrainShadowMasks.clear();
	m_bGpuRegionCacheTexturesReady = bHeightOk;
	if ( !bControlOk || !bColorOk || !bDerivedOk || !bMiddleCompositeOk || !bTerrainShadowOk ) {
		LOG( WARN,
			 "[gpu-cache] Height page cache is ready with optional channels degraded: control=",
			 bControlOk,
			 "; color=",
			 bColorOk,
			 "; derived=",
			 bDerivedOk,
			 "; middle_composite=",
			 bMiddleCompositeOk,
			 "; terrain_shadow=",
			 bTerrainShadowOk );
	}
	m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	m_nGpuRegionRequestedPages = calculateGpuRegionRequestedPages();
	m_nGpuRegionRequiredRadius = calculateGpuRegionRequiredRadius();
	LOG( INFO,
		 "[gpu-cache] Created bounded GPU region cache: ",
		 capacity,
		 " mapped pages, ",
		 m_nGpuRegionSpareLayerCount,
		 " spare/retirement pages, ",
		 m_nGpuRegionPhysicalLayerCount,
		 " physical layers, requested ",
		 m_nGpuRegionRequestedPages,
		 ", required radius ",
		 m_nGpuRegionRequiredRadius,
		 ", estimated bytes ",
		 estimated_bytes,
		 ", budget ",
		 budget_bytes );
	return true;
}

int Terrain3DGpuRegionCache::findGpuRegionSlot(
	const Vector2i &vLocation ) const {
	return t3dRegionResidencyFindMappedSlot(
		vLocation,
		m_szGpuRegionMap,
		m_szGpuRegionLocations,
		m_nGpuRegionPhysicalLayerCount,
		[&]( const int layer ) { return m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED; } );
}

int Terrain3DGpuRegionCache::findPendingGpuRegionUpload(
	const Vector2i &vLocation ) const {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H07_GPU_LAYER_SCAN );
	Terrain3DStreamingProfileWorkScope scanned( T3D_SP_H07_GPU_LAYER_SCAN );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		scanned.add();
		const t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[layer];
		if ( pending.m_nGeneration != 0u && t3dRegionCoordEquals( pending.m_location, vLocation ) ) {
			return layer;
		}
	}
	return -1;
}

int Terrain3DGpuRegionCache::findFreeGpuRegionSlot(
	void ) const {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H07_GPU_LAYER_SCAN );
	Terrain3DStreamingProfileWorkScope scanned( T3D_SP_H07_GPU_LAYER_SCAN );
	return t3dRegionResidencyFindFirstSlot( m_nGpuRegionPhysicalLayerCount, [&]( const int layer ) {
		scanned.add();
		return !t3dRegionCoordValid( m_szGpuRegionLocations[layer] ) &&
			   m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE && m_szGpuRegionVictimReserved[layer] == 0u;
	} );
}

void Terrain3DGpuRegionCache::clearPendingGpuRegionUpload(
	const int nLayer ) {
	if ( nLayer < 0 || nLayer >= T3D_GPU_CACHE_MAX_LAYERS ) {
		return;
	}
	t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[nLayer];
	pending.m_nGeneration = 0u;
	pending.m_nLastUseTick = 0u;
	pending.m_nLoadedTick = 0u;
	pending.m_nVictimLastUseTick = 0u;
	pending.m_location = t3dInvalidRegionCoord();
	pending.m_victimLocation = t3dInvalidRegionCoord();
	pending.m_nVictimLayer = -1;
	pending.m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_NONE;
	pending.m_nPrefetchOrigin = 0u;
}

void Terrain3DGpuRegionCache::recordGpuRegionDeparture(
	const int  nLayer,
	const bool bCountUnusedPrefetch ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return;
	}
	if ( m_szGpuRegionLoadedTick[nLayer] > 0u && m_nGpuRegionTick >= m_szGpuRegionLoadedTick[nLayer] ) {
		m_nGpuRegionResidentTicksTotal += m_nGpuRegionTick - m_szGpuRegionLoadedTick[nLayer];
		++m_nGpuRegionDepartedPagesTotal;
	}
	if ( bCountUnusedPrefetch && m_szGpuRegionPrefetchOrigin[nLayer] != 0u ) {
		++m_nGpuRegionPrefetchDiscardedTotal;
	}
	m_szGpuRegionLoadedTick[nLayer] = 0u;
	m_szGpuRegionPrefetchOrigin[nLayer] = 0u;
}

void Terrain3DGpuRegionCache::retireGpuRegionSlot(
	const int nLayer ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ||
		 m_szGpuRegionSlotStates[nLayer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
		return;
	}
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_RETIRED;
	m_szGpuRegionRetireAfterFrame[nLayer] = eastl::numeric_limits< uint64_t >::max();
	++m_nGpuRegionRetiredCount;
}

void Terrain3DGpuRegionCache::armPendingRetirements(
	void ) {
	Engine *const  pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	const uint64_t release_frame = current_frame + ( uint64_t ) ( MAX( 1, m_nGpuRegionRetireFrames ) );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED &&
			 m_szGpuRegionRetireAfterFrame[layer] == eastl::numeric_limits< uint64_t >::max() ) {
			m_szGpuRegionRetireAfterFrame[layer] = release_frame;
		}
	}
}

void Terrain3DGpuRegionCache::reclaimRetiredGpuRegionSlots(
	void ) {
	Engine *const  pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_RETIRED ||
			 m_szGpuRegionRetireAfterFrame[layer] == eastl::numeric_limits< uint64_t >::max() ||
			 current_frame < m_szGpuRegionRetireAfterFrame[layer] ) {
			continue;
		}
		m_szGpuRegionLocations[layer] = t3dInvalidRegionCoord();
		m_szGpuRegionLastUseTick[layer] = 0u;
		m_szGpuRegionLoadedTick[layer] = 0u;
		m_szGpuRegionPrefetchOrigin[layer] = 0u;
		m_szGpuRegionMaterialMasks[layer] = 0u;
		m_szGpuRegionRetireAfterFrame[layer] = 0u;
		m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
		m_nGpuRegionRetiredCount = MAX( 0, m_nGpuRegionRetiredCount - 1 );
		++m_nGpuRegionReusedLayersLastTick;
	}
}

void Terrain3DGpuRegionCache::finalizePendingGpuRegionUploads(
	void ) {
	Terrain3DRdTextureManager *const pTextureManager = Terrain3DRdTextureManager::getSingleton();
	if ( !pTextureManager ) {
		return;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[layer];
		if ( pending.m_nGeneration == 0u ) {
			continue;
		}
		const t3dRdTextureBatchState_t batch_state =
			pTextureManager->getTrackedTextureLayerBatchState( ( uint32_t ) ( layer ), pending.m_nGeneration );
		if ( batch_state == T3D_RD_TEXTURE_BATCH_PENDING ) {
			continue;
		}
		const uint8_t publication = pending.m_nPublication;
		const int	  victim_layer = pending.m_nVictimLayer;
		if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_NEW && victim_layer < 0 ) {
			m_nGpuRegionPendingUnmappedReservations = MAX( 0, m_nGpuRegionPendingUnmappedReservations - 1 );
		}
		if ( victim_layer >= 0 && victim_layer < m_nGpuRegionPhysicalLayerCount ) {
			m_szGpuRegionVictimReserved[victim_layer] = 0u;
		}
		if ( batch_state != T3D_RD_TEXTURE_BATCH_SUCCEEDED ) {
			// A render-thread batch can fail after one or more preceding texture updates
			// succeeded. Keep the layer unpublished and force both optional channels to
			// be explicitly overwritten on its next full-page use.
			m_szGpuRegionLayerChannelFlags[layer] |= ( uint8_t ) ( REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR );
			m_szGpuRegionLocations[layer] = t3dInvalidRegionCoord();
			m_szGpuRegionLastUseTick[layer] = 0u;
			m_szGpuRegionLoadedTick[layer] = 0u;
			m_szGpuRegionPrefetchOrigin[layer] = 0u;
			m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
			if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH && victim_layer >= 0 &&
				 victim_layer < m_nGpuRegionPhysicalLayerCount &&
				 m_szGpuRegionSlotStates[victim_layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
				 t3dRegionCoordEquals( m_szGpuRegionLocations[victim_layer], pending.m_location ) ) {
				queueRepublish( pending.getLocation() );
			}
			clearPendingGpuRegionUpload( layer );
			m_bGpuRegionCacheSelectionDirty = true;
			continue;
		}

		bool				   bPublished = false;
		const int			   map_index = Terrain3DRegionRepository::getRegionMapIndex( pending.getLocation() );
		Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( pending.getLocation() );
		if ( map_index >= 0 && pRegion && !pRegion->isDeleted() ) {
			if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_NEW ) {
				bool bCapacityAvailable = false;
				if ( m_szGpuRegionMap[map_index] == 0u && victim_layer >= 0 &&
					 victim_layer < m_nGpuRegionPhysicalLayerCount &&
					 m_szGpuRegionSlotStates[victim_layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
					 t3dRegionCoordEquals( m_szGpuRegionLocations[victim_layer], pending.m_victimLocation ) &&
					 m_szGpuRegionLastUseTick[victim_layer] == pending.m_nVictimLastUseTick ) {
					const int victim_map_index =
						Terrain3DRegionRepository::getRegionMapIndex( pending.getVictimLocation() );
					if ( victim_map_index >= 0 &&
						 m_szGpuRegionMap[victim_map_index] == ( uint16_t ) ( victim_layer + 1 ) &&
						 m_szGpuRegionRepublishPending[victim_map_index] == 0u ) {
						m_szGpuRegionMap[victim_map_index] = 0u;
						recordGpuRegionDeparture( victim_layer, true );
						retireGpuRegionSlot( victim_layer );
						++m_nGpuRegionEvictionsLastTick;
						bCapacityAvailable = true;
					}
				}
				if ( m_szGpuRegionMap[map_index] == 0u && !bCapacityAvailable &&
					 m_nGpuRegionResidentCount < m_nGpuRegionCacheCapacity ) {
					++m_nGpuRegionResidentCount;
					bCapacityAvailable = true;
				}
				if ( bCapacityAvailable ) {
					m_szGpuRegionLocations[layer] = pending.m_location;
					m_szGpuRegionLastUseTick[layer] = pending.m_nLastUseTick;
					m_szGpuRegionLoadedTick[layer] = pending.m_nLoadedTick;
					m_szGpuRegionPrefetchOrigin[layer] = pending.m_nPrefetchOrigin;
					m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_MAPPED;
					m_szGpuRegionMap[map_index] = ( uint16_t ) ( layer + 1 );
					bPublished = true;
				}
			} else if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH && victim_layer >= 0 &&
						victim_layer < m_nGpuRegionPhysicalLayerCount &&
						m_szGpuRegionSlotStates[victim_layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
						t3dRegionCoordEquals( m_szGpuRegionLocations[victim_layer], pending.m_location ) &&
						m_szGpuRegionMap[map_index] == ( uint16_t ) ( victim_layer + 1 ) ) {
				m_szGpuRegionLocations[layer] = pending.m_location;
				m_szGpuRegionLastUseTick[layer] = pending.m_nLastUseTick;
				m_szGpuRegionLoadedTick[layer] = pending.m_nLoadedTick;
				m_szGpuRegionPrefetchOrigin[layer] = pending.m_nPrefetchOrigin;
				m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_MAPPED;
				m_szGpuRegionMap[map_index] = ( uint16_t ) ( layer + 1 );
				m_szGpuRegionLoadedTick[victim_layer] = 0u;
				m_szGpuRegionPrefetchOrigin[victim_layer] = 0u;
				retireGpuRegionSlot( victim_layer );
				bPublished = true;
			}
		}
		if ( bPublished ) {
			++m_nGpuRegionUploadsSinceReset;
			m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
			notePageTableChanged();
		} else {
			m_szGpuRegionLocations[layer] = t3dInvalidRegionCoord();
			m_szGpuRegionLastUseTick[layer] = 0u;
			m_szGpuRegionLoadedTick[layer] = 0u;
			m_szGpuRegionPrefetchOrigin[layer] = 0u;
			m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
			m_bGpuRegionCacheSelectionDirty = true;
		}
		clearPendingGpuRegionUpload( layer );
	}
}

void Terrain3DGpuRegionCache::invalidate(
	const Vector2i &vLocation ) {
	if ( !m_bGpuRegionCacheEnabled ) {
		return;
	}
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		return;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szGpuRegionMap[map_index] = 0u;
	}
	recordGpuRegionDeparture( layer, true );
	retireGpuRegionSlot( layer );
	m_nGpuRegionResidentCount = MAX( 0, m_nGpuRegionResidentCount - 1 );
	m_bGpuRegionCacheSelectionDirty = true;
	notePageTableChanged();
}

void Terrain3DGpuRegionCache::queueRepublish(
	const Vector2i &vLocation,
	const uint8_t	dirty_maps ) {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 || map_index >= T3D_REGION_SLOTS ) {
		return;
	}
	if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
		++m_nGpuRegionRepublishPendingCount;
	}
	m_szGpuRegionRepublishPending[map_index] |= dirty_maps;
	// Force the ordinary per-frame residency tick to revisit deferred authoring
	// publications even when the camera and streaming generation are stationary.
	m_bGpuRegionCacheSelectionDirty = true;
}

bool Terrain3DGpuRegionCache::republish(
	const Vector2i	 &vLocation,
	Terrain3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	queueRepublish( vLocation );
	return false; // Mapping publication happens in the bounded residency tick.
}

bool Terrain3DGpuRegionCache::uploadRepublish(
	const Vector2i &vLocation ) {
	const int old_layer = findGpuRegionSlot( vLocation );
	if ( old_layer < 0 ) {
		return false;
	}
	if ( findPendingGpuRegionUpload( vLocation ) >= 0 ) {
		return false;
	}
	if ( m_szGpuRegionVictimReserved[old_layer] != 0u ) {
		m_bGpuRegionCacheSelectionDirty = true;
		return false;
	}
	reclaimRetiredGpuRegionSlots();
	const int staging_layer = findFreeGpuRegionSlot();
	if ( staging_layer < 0 ) {
		// Authoring can commit several brush stamps before the renderer has advanced
		// enough frames to recycle retired layers. Coalesce the location and publish
		// the newest page later instead of dropping the edit or synchronizing the GPU.
		m_bGpuRegionCacheSelectionDirty = true;
		return false;
	}
	// Keep coherent staging, but copy unchanged channels from the live page.
	const int	  dirty_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	const uint8_t dirty_maps = dirty_index >= 0 ? m_szGpuRegionRepublishPending[dirty_index] : UINT8_MAX;
	if ( !submitBudgetedGpuRegionUpload( staging_layer, vLocation, dirty_maps, old_layer ) ) {
		return false;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_FREE;
		clearPendingGpuRegionUpload( staging_layer );
		return false;
	}
	t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[staging_layer];
	if ( pending.m_nGeneration != 0u ) {
		pending.m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH;
		pending.m_nVictimLayer = old_layer;
		pending.setVictimLocation( vLocation );
		pending.m_nLastUseTick = m_szGpuRegionLastUseTick[old_layer];
		pending.m_nLoadedTick = m_szGpuRegionLoadedTick[old_layer];
		pending.m_nPrefetchOrigin = m_szGpuRegionPrefetchOrigin[old_layer];
		m_szGpuRegionVictimReserved[old_layer] = 1u;
		if ( m_szGpuRegionRepublishPending[map_index] != 0u ) {
			m_szGpuRegionRepublishPending[map_index] = 0u;
			m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
		}
		return false;
	}
	m_szGpuRegionLocations[staging_layer] = t3dPackRegionCoord( vLocation );
	m_szGpuRegionLastUseTick[staging_layer] = m_nGpuRegionTick;
	m_szGpuRegionLoadedTick[staging_layer] = m_szGpuRegionLoadedTick[old_layer];
	m_szGpuRegionPrefetchOrigin[staging_layer] = m_szGpuRegionPrefetchOrigin[old_layer];
	m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
	m_szGpuRegionMap[map_index] = ( uint16_t ) ( staging_layer + 1 );
	if ( m_szGpuRegionRepublishPending[map_index] != 0u ) {
		m_szGpuRegionRepublishPending[map_index] = 0u;
		m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
	}
	m_szGpuRegionLoadedTick[old_layer] = 0u;
	m_szGpuRegionPrefetchOrigin[old_layer] = 0u;
	retireGpuRegionSlot( old_layer );
	++m_nGpuRegionUploadsSinceReset;
	m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	notePageTableChanged();
	return true;
}

int Terrain3DGpuRegionCache::processPendingGpuRegionRepublishes(
	const int nLimit ) {
	if ( m_nGpuRegionRepublishPendingCount <= 0 || nLimit <= 0 || m_nGpuRegionPhysicalLayerCount <= 0 ) {
		return 0;
	}
	int published = 0;
	int consumed = 0;
	int visited = 0;
	int cursor = m_nGpuRegionRepublishCursor;
	while ( visited < T3D_REGION_SLOTS && m_nGpuRegionRepublishPendingCount > 0 && consumed < nLimit ) {
		const int map_index = cursor;
		cursor = ( cursor + 1 ) % T3D_REGION_SLOTS;
		++visited;
		if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
			continue;
		}
		const int	   map_x = map_index % T3D_REGION_MAP_SIZE;
		const int	   map_y = map_index / T3D_REGION_MAP_SIZE;
		const Vector2i location( map_x - T3D_REGION_MAP_SIZE / 2, map_y - T3D_REGION_MAP_SIZE / 2 );
		if ( findPendingGpuRegionUpload( location ) >= 0 ) {
			continue;
		}
		if ( findGpuRegionSlot( location ) < 0 ) {
			// An evicted page will be uploaded from current source data if it becomes
			// desirable again, so no separate republish remains necessary.
			m_szGpuRegionRepublishPending[map_index] = 0u;
			m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
			continue;
		}
		if ( findFreeGpuRegionSlot() < 0 ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY;
			break;
		}
		if ( findPendingGpuRegionUpload( location ) >= 0 ) {
			continue;
		}
		const int before = m_nGpuRegionUploadsLastTick;
		if ( uploadRepublish( location ) ) {
			++published;
		}
		if ( m_bGpuPreparationDeferred ) {
			break;
		}
		consumed += m_nGpuRegionUploadsLastTick - before;
		if ( ( m_nGpuRegionStallFlagsLastTick & ( TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET |
												  TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) ) != 0u ) {
			break;
		}
	}
	m_nGpuRegionRepublishCursor = cursor;
	return published;
}

static void t3dGpuAppendMaterialMask(
	uint32_t	   nMask,
	uint8_t *const pIds,
	const int	   nCapacity,
	uint32_t	  &nEmittedMask,
	int			  &nCount ) {
	while ( nMask != 0u && nCount < nCapacity ) {
		int nId = 0;
		while ( nId < T3D_MAX_TEXTURE_ASSETS && ( nMask & ( 1u << ( uint32_t ) ( nId ) ) ) == 0u ) {
			++nId;
		}
		if ( nId >= T3D_MAX_TEXTURE_ASSETS ) {
			break;
		}
		const uint32_t nBit = 1u << ( uint32_t ) ( nId );
		nMask &= ~nBit;
		if ( ( nEmittedMask & nBit ) != 0u ) {
			continue;
		}
		pIds[nCount++] = ( uint8_t ) ( nId );
		nEmittedMask |= nBit;
	}
}

static bool t3dGpuAppendTextureBatchUpdate(
	t3dRdTextureBatchUpdate_t *const pUpdates,
	uint32_t						&nUpdateCount,
	Terrain3DGeneratedTexture		&rTexture,
	const Ref< Image >				&rImage,
	const int						 nLayer,
	const int						 copy_source = -1 ) {
	if ( nUpdateCount >= 6u ) {
		return false;
	}
	const bool ready = copy_source >= 0
						   ? rTexture.buildTrackedLayerCopy( copy_source, nLayer, pUpdates[nUpdateCount] )
						   : rTexture.buildTrackedLayerBatchUpdate( rImage, nLayer, pUpdates[nUpdateCount] );
	if ( !ready ) {
		return false;
	}
	++nUpdateCount;
	return true;
}

int Terrain3DGpuRegionCache::collectDesiredGpuRegions(
	const t3dResidencyFrameState_s &rFrame,
	const Vector2i				   &rCenter,
	Vector2i					   *pLocations,
	const int						nCapacity,
	t3dGpuCacheSelectionStats_t	   &rStats ) {
	rStats = t3dGpuCacheSelectionStats_s();
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, T3D_GPU_CACHE_MAX_LAYERS );
	uint8_t	  selected_map[T3D_REGION_SLOTS] = {};

	// Full surface pages are mandatory only for ACTIVE. HOT geometry is served by
	// the independent height cache below, so surface pressure can never move vertices.
	int output_count = t3dCollectDesiredRegionsForState( m_regionRepository,
														 rFrame,
														 rCenter,
														 T3D_REGION_MAP_SIZE,
														 TERRAIN3D_REGION_STREAMING_ACTIVE,
														 pLocations,
														 capacity,
														 0,
														 selected_map );
	rStats.m_nRequired = output_count;
	output_count = t3dCollectDesiredRegionsForState( m_regionRepository,
													 rFrame,
													 rCenter,
													 T3D_REGION_MAP_SIZE,
													 TERRAIN3D_REGION_STREAMING_HOT,
													 pLocations,
													 capacity,
													 output_count,
													 selected_map );
	rStats.m_nGuard = output_count - rStats.m_nRequired;
	const int prefetch_begin = output_count;
	output_count = t3dAppendPrefetchRegionsFromRepositorySlots( m_regionRepository,
																rFrame,
																pLocations,
																capacity,
																output_count,
																selected_map );
	rStats.m_nPrefetch = output_count - prefetch_begin;
	rStats.m_nWritten = output_count;
	return output_count;
}

int Terrain3DGpuRegionCache::fillMaterialPriorityIds(
	uint8_t *const pIds,
	const int	   nCapacity ) const {
	if ( !pIds || nCapacity <= 0 ) {
		return 0;
	}
	uint32_t emitted_mask = 0u;
	int		 count = 0;

	// Logical material zero is the stable fallback physical page.
	t3dGpuAppendMaterialMask( 1u, pIds, nCapacity, emitted_mask, count );
	for ( int priority = 0; priority < m_nGpuRegionPriorityCount && count < nCapacity; ++priority ) {
		const int layer = findGpuRegionSlot( t3dUnpackRegionCoord( m_szGpuRegionPriorityLocations[priority] ) );
		if ( layer >= 0 ) {
			t3dGpuAppendMaterialMask( m_szGpuRegionMaterialMasks[layer], pIds, nCapacity, emitted_mask, count );
		}
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && count < nCapacity; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED ) {
			t3dGpuAppendMaterialMask( m_szGpuRegionMaterialMasks[layer], pIds, nCapacity, emitted_mask, count );
		}
	}
	return count;
}

bool Terrain3DGpuRegionCache::beginGpuRegionLayerUpload(
	t3dGpuRegionLayerUploadState_t &rState,
	uint64_t					   &rPreparedRequestUsec ) {
	if ( !m_bGpuRegionCacheEnabled || !m_bGpuRegionCacheTexturesReady || rState.m_nLayer < 0 ||
		 rState.m_nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return false;
	}
	rState.m_pRegion = m_regionRepository.getRegionPtr( rState.m_location );
	if ( !rState.m_pRegion || rState.m_pRegion->isDeleted() ) {
		return false;
	}
	rState.m_nSourceChannelFlags = ( uint8_t ) ( rState.m_pRegion->getChannelFlags() );
	rState.m_nPreviousChannelFlags = m_szGpuRegionLayerChannelFlags[rState.m_nLayer];
	rState.m_bUploadHeight = rState.m_eMapType == TYPE_HEIGHT || rState.m_eMapType == REGION_TYPE_MAX;
	rState.m_bUploadControl = ( rState.m_eMapType == TYPE_CONTROL || rState.m_eMapType == REGION_TYPE_MAX ) &&
							  !m_bControlMapsImplicit && m_generatedControlMaps.getRid().is_valid() &&
							  t3dGpuCacheOptionalChannelNeedsUpdate( rState.m_nSourceChannelFlags,
																	 rState.m_nPreviousChannelFlags,
																	 ( uint8_t ) ( REGION_CHANNEL_CONTROL ) );
	rState.m_bUploadColor = ( rState.m_eMapType == TYPE_COLOR || rState.m_eMapType == REGION_TYPE_MAX ) &&
							isColorChannelEnabled() && m_generatedColorMaps.getRid().is_valid() &&
							t3dGpuCacheOptionalChannelNeedsUpdate( rState.m_nSourceChannelFlags,
																   rState.m_nPreviousChannelFlags,
																   ( uint8_t ) ( REGION_CHANNEL_COLOR ) );
	rState.m_nPreviousState = m_szGpuRegionSlotStates[rState.m_nLayer];
	m_szGpuRegionSlotStates[rState.m_nLayer] = TERRAIN3D_GPU_SLOT_STAGING;

	if ( rState.m_bAllowCookedPage && rState.m_eMapType == REGION_TYPE_MAX &&
		 rState.m_pRegion->runtimeMapsEvictableInternal() && m_pTerrain &&
		 m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		t3dRuntimeGpuPageResult_t page;
		rState.m_bCookedPage = m_runtimePageStore.takeGpuPage( rState.m_location, page );
		rState.m_nCookedMaterialMask = page.m_nMaterialMask;
		rPreparedRequestUsec = page.m_nRequestedUsec;
		if ( rState.m_bCookedPage ) {
			rState.m_pHeight = page.m_height;
			rState.m_pControl = page.m_control;
			rState.m_pColor = page.m_color;
			rState.m_pDerived = page.m_derived;
			rState.m_pMiddleComposite = page.m_middleComposite;
		}
		if ( !page.m_bAttempted && m_runtimePageStore.isPending( rState.m_location ) ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING;
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
		if ( !page.m_bAttempted && ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ) ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE;
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
		if ( rState.m_bCookedPage ) {
			++m_nGpuRegionCookedHitsLastTick;
		} else {
			++m_nGpuRegionCookedMissesLastTick;
		}
		m_runtimePageStore.takeTerrainShadowMask( rState.m_location, rState.m_pTerrainShadowMask );
	}
	if ( !rState.m_bCookedPage ) {
		if ( m_cpuRegionCache.isEnabled() && !m_cpuRegionCache.ensureResident( rState.m_location, true, false ) ) {
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
		++m_nGpuRegionRuntimePreparesLastTick;
	}
	rState.m_bDerivedChannelEnabled = isDerivedChannelEnabled() && m_generatedDerivedMaps.getRid().is_valid();
	rState.m_bMiddleCompositeAllocated = m_generatedMiddleCompositeMaps.getRid().is_valid();
	rState.m_bTerrainShadowAllocated = m_generatedTerrainShadowMasks.getRid().is_valid();
	if ( rState.m_nCopyFromLayer >= 0 && rState.m_nDirtyMaps != UINT8_MAX &&
		 rState.m_nSourceChannelFlags == m_szGpuRegionLayerChannelFlags[rState.m_nCopyFromLayer] ) {
		const bool height_dirty = ( rState.m_nDirtyMaps & ( 1u << TYPE_HEIGHT ) ) != 0u;
		const bool control_dirty = ( rState.m_nDirtyMaps & ( 1u << TYPE_CONTROL ) ) != 0u;
		const bool color_dirty = ( rState.m_nDirtyMaps & ( 1u << TYPE_COLOR ) ) != 0u;
		if ( !height_dirty ) {
			rState.m_nCopyChannels |= 1u | 8u;
		}
		if ( !control_dirty ) {
			rState.m_nCopyChannels |= 2u;
		}
		if ( !color_dirty ) {
			rState.m_nCopyChannels |= 4u;
		}
		if ( !control_dirty && !color_dirty ) {
			rState.m_nCopyChannels |= 16u;
		}
		rState.m_nCopyChannels |= 32u;
	}
	return true;
}

bool Terrain3DGpuRegionCache::resolveGpuRegionLayerImages(
	t3dGpuRegionLayerUploadState_t &rState ) {
	if ( rState.m_bUploadHeight && !rState.m_bCookedPage && ( rState.m_nCopyChannels & 1u ) == 0u ) {
		rState.m_pHeight = getRegionMapOrDefault( rState.m_pRegion, TYPE_HEIGHT );
		if ( rState.m_pHeight.is_null() ) {
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
		if ( rState.m_bDerivedChannelEnabled ) {
			rState.m_pDerived = rState.m_pRegion->getDerivedNormalSlopeMap();
			if ( rState.m_pDerived.is_null() ) {
				++m_nGpuRegionRuntimeDerivedBuildsLastTick;
				m_mapEditService.regenerateDerivedMaps( { rState.m_location }, AABB(), m_pCompute );
				rState.m_pDerived = rState.m_pRegion->getDerivedNormalSlopeMap();
			}
			if ( rState.m_pDerived.is_null() ) {
				rState.m_pDerived = m_defaultDerivedMap;
			}
			if ( rState.m_pDerived.is_null() ) {
				m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
				return false;
			}
			rState.m_pDerived = makeGpuDerivedImage( rState.m_pDerived );
			if ( rState.m_pDerived.is_null() ) {
				m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
				return false;
			}
		}
	}
	if ( rState.m_bUploadControl && ( rState.m_nCopyChannels & 2u ) == 0u ) {
		if ( rState.m_pControl.is_null() ) {
			rState.m_pControl = makeGpuRenderControlImage( rState.m_location );
		}
		if ( rState.m_pControl.is_null() ) {
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
	}
	if ( rState.m_bUploadColor && ( rState.m_nCopyChannels & 4u ) == 0u ) {
		if ( rState.m_pColor.is_null() ) {
			const Ref< Image > source_color = getRegionMapOrDefault( rState.m_pRegion, TYPE_COLOR );
			if ( source_color.is_valid() &&
				 ( !source_color->has_mipmaps() ||
				   ( m_pTerrain && m_pTerrain->getGpuRegionColorResolutionDivisor() > 1 ) ) ) {
				++m_nGpuRegionRuntimeColorMipsLastTick;
			}
			rState.m_pColor = makeGpuColorImage( source_color );
		}
		if ( rState.m_pColor.is_null() ) {
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
	}
	if ( ( rState.m_nCopyChannels & 32u ) == 0u && rState.m_bTerrainShadowAllocated &&
		 rState.m_eMapType == REGION_TYPE_MAX && rState.m_pTerrainShadowMask.is_null() ) {
		rState.m_pTerrainShadowMask = makeTerrainShadowMaskImage( rState.m_pRegion );
		if ( rState.m_pTerrainShadowMask.is_null() ) {
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
	} else if ( rState.m_bTerrainShadowAllocated && rState.m_pTerrainShadowMask.is_valid() ) {
		if ( rState.m_pTerrainShadowMask->get_width() != T3D_REGION_SIZE ||
			 rState.m_pTerrainShadowMask->get_height() != T3D_REGION_SIZE ) {
			rState.m_pTerrainShadowMask = rState.m_pTerrainShadowMask->duplicate();
			if ( rState.m_pTerrainShadowMask->has_mipmaps() ) {
				rState.m_pTerrainShadowMask->clear_mipmaps();
			}
			rState.m_pTerrainShadowMask->resize( T3D_REGION_SIZE, T3D_REGION_SIZE, Image::INTERPOLATE_BILINEAR );
			rState.m_pTerrainShadowMask->convert( Image::FORMAT_L8 );
			if ( rState.m_pTerrainShadowMask->generate_mipmaps() != OK ) {
				m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
				return false;
			}
		}
	}
	if ( ( rState.m_nCopyChannels & 16u ) == 0u && rState.m_bMiddleCompositeAllocated &&
		 rState.m_eMapType == REGION_TYPE_MAX && rState.m_pMiddleComposite.is_null() ) {
		Ref< Image > composite_control = rState.m_pControl;
		if ( composite_control.is_null() ) {
			composite_control = makeGpuRenderControlImage( rState.m_location );
		}
		Ref< Image > composite_color = rState.m_pColor;
		if ( composite_color.is_null() ) {
			composite_color = makeGpuColorImage( getRegionMapOrDefault( rState.m_pRegion, TYPE_COLOR ) );
		}
		rState.m_pMiddleComposite =
			makeMiddleCompositeImage( rState.m_location, composite_control, composite_color, 0 );
		if ( rState.m_pMiddleComposite.is_null() ) {
			m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
			return false;
		}
	}
	return true;
}

void Terrain3DGpuRegionCache::resolveGpuRegionLayerMetadata(
	t3dGpuRegionLayerUploadState_t &rState ) const {
	rState.m_nPublishedFlags = rState.m_nPreviousChannelFlags;
	if ( rState.m_eMapType == TYPE_CONTROL || rState.m_eMapType == REGION_TYPE_MAX ) {
		rState.m_nPublishedFlags &= ( uint8_t ) ( ~REGION_CHANNEL_CONTROL );
		rState.m_nPublishedFlags |= rState.m_nSourceChannelFlags & ( uint8_t ) ( REGION_CHANNEL_CONTROL );
	}
	if ( rState.m_eMapType == TYPE_COLOR || rState.m_eMapType == REGION_TYPE_MAX ) {
		rState.m_nPublishedFlags &= ( uint8_t ) ( ~REGION_CHANNEL_COLOR );
		rState.m_nPublishedFlags |= rState.m_nSourceChannelFlags & ( uint8_t ) ( REGION_CHANNEL_COLOR );
	}
	rState.m_nMaterialMask =
		rState.m_bCookedPage
			? rState.m_nCookedMaterialMask
			: ( rState.m_pControl.is_valid()
					? t3dControlCookMaterialMaskFromControlImage( rState.m_pControl )
					: ( ( rState.m_nCopyChannels & 2u ) != 0u ? m_szGpuRegionMaterialMasks[rState.m_nCopyFromLayer]
															  : 1u ) );
	if ( m_pTerrain ) {
		const Ref< Terrain3DMaterial > material = m_pTerrain->materialResource();
		if ( material.is_valid() && material->getAutoShaderEnabled() ) {
			const int auto_base =
				CLAMP( t3dGpuRegionCacheMaterialIntParam( material.ptr(), "auto_base_texture", 0 ), 0, 31 );
			const int auto_overlay =
				CLAMP( t3dGpuRegionCacheMaterialIntParam( material.ptr(), "auto_overlay_texture", 1 ), 0, 31 );
			rState.m_nMaterialMask |= 1u << ( uint32_t ) ( auto_base );
			rState.m_nMaterialMask |= 1u << ( uint32_t ) ( auto_overlay );
		}
	}
}

bool Terrain3DGpuRegionCache::submitGpuRegionLayerTrackedBatch(
	t3dGpuRegionLayerUploadState_t &rState ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_REGION_TEXTURE_UPLOAD );
	Terrain3DStreamingProfileBytesScope uploadBytes( T3D_SP_REGION_TEXTURE_UPLOAD );
	Terrain3DRdTextureManager *const texture_manager = Terrain3DRdTextureManager::getSingleton();
	if ( !texture_manager || m_szGpuRegionPendingUploads[rState.m_nLayer].m_nGeneration != 0u ) {
		m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
		return false;
	}
	t3dRdTextureBatchUpdate_t updates[6];
	uint32_t				  update_count = 0u;
	bool					  batch_ready =
		t3dGpuAppendTextureBatchUpdate( updates,
										update_count,
										m_generatedHeightMaps,
										rState.m_pHeight,
										rState.m_nLayer,
										( rState.m_nCopyChannels & 1u ) != 0u ? rState.m_nCopyFromLayer : -1 );
	if ( batch_ready && rState.m_bDerivedChannelEnabled ) {
		batch_ready =
			t3dGpuAppendTextureBatchUpdate( updates,
											update_count,
											m_generatedDerivedMaps,
											rState.m_pDerived,
											rState.m_nLayer,
											( rState.m_nCopyChannels & 8u ) != 0u ? rState.m_nCopyFromLayer : -1 );
	}
	if ( batch_ready && rState.m_bUploadControl ) {
		batch_ready =
			t3dGpuAppendTextureBatchUpdate( updates,
											update_count,
											m_generatedControlMaps,
											rState.m_pControl,
											rState.m_nLayer,
											( rState.m_nCopyChannels & 2u ) != 0u ? rState.m_nCopyFromLayer : -1 );
	}
	if ( batch_ready && rState.m_bUploadColor ) {
		batch_ready =
			t3dGpuAppendTextureBatchUpdate( updates,
											update_count,
											m_generatedColorMaps,
											rState.m_pColor,
											rState.m_nLayer,
											( rState.m_nCopyChannels & 4u ) != 0u ? rState.m_nCopyFromLayer : -1 );
	}
	if ( batch_ready && rState.m_bMiddleCompositeAllocated ) {
		batch_ready =
			t3dGpuAppendTextureBatchUpdate( updates,
											update_count,
											m_generatedMiddleCompositeMaps,
											rState.m_pMiddleComposite,
											rState.m_nLayer,
											( rState.m_nCopyChannels & 16u ) != 0u ? rState.m_nCopyFromLayer : -1 );
	}
	if ( batch_ready && rState.m_bTerrainShadowAllocated ) {
		batch_ready =
			t3dGpuAppendTextureBatchUpdate( updates,
											update_count,
											m_generatedTerrainShadowMasks,
											rState.m_pTerrainShadowMask,
											rState.m_nLayer,
											( rState.m_nCopyChannels & 32u ) != 0u ? rState.m_nCopyFromLayer : -1 );
	}
	if ( !batch_ready ) {
		m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
		if ( rState.m_bCookedPage && m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
			return uploadGpuRegionLayer( rState.m_nLayer, rState.m_location, rState.m_eMapType, false );
		}
		return false;
	}
	const uint64_t upload_generation = texture_manager->nextTrackedTextureBatchGeneration();
	if ( !texture_manager->submitTrackedTextureLayerBatch( ( uint32_t ) ( rState.m_nLayer ),
														   upload_generation,
														   ( uint32_t ) ( rState.m_nLayer ),
														   updates,
														   update_count ) ) {
		m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
		return false;
	}
	t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[rState.m_nLayer];
	pending.m_nGeneration = upload_generation;
	pending.setLocation( rState.m_location );
	m_generatedHeightMaps.commitLayerImage( rState.m_pHeight, rState.m_nLayer );
	++m_nGpuRegionHeightUpdatesLastTick;
	const size_t height_bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pHeight );
	m_nGpuRegionHeightBytesLastTick += height_bytes;
	m_nGpuRegionUploadBytesLastTick += height_bytes;
	if ( rState.m_bDerivedChannelEnabled ) {
		m_generatedDerivedMaps.commitLayerImage( rState.m_pDerived, rState.m_nLayer );
		++m_nGpuRegionDerivedUpdatesLastTick;
		const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pDerived );
		m_nGpuRegionDerivedBytesLastTick += bytes;
		m_nGpuRegionUploadBytesLastTick += bytes;
		uploadBytes.add( bytes );
	}
	if ( rState.m_bUploadControl ) {
		m_generatedControlMaps.commitLayerImage( rState.m_pControl, rState.m_nLayer );
		++m_nGpuRegionControlUpdatesLastTick;
		const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pControl );
		m_nGpuRegionControlBytesLastTick += bytes;
		m_nGpuRegionUploadBytesLastTick += bytes;
		uploadBytes.add( bytes );
	}
	if ( rState.m_bUploadColor ) {
		m_generatedColorMaps.commitLayerImage( rState.m_pColor, rState.m_nLayer );
		++m_nGpuRegionColorUpdatesLastTick;
		const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pColor );
		m_nGpuRegionColorBytesLastTick += bytes;
		m_nGpuRegionUploadBytesLastTick += bytes;
		uploadBytes.add( bytes );
	}
	if ( rState.m_bMiddleCompositeAllocated ) {
		m_generatedMiddleCompositeMaps.commitLayerImage( rState.m_pMiddleComposite, rState.m_nLayer );
		++m_nGpuRegionMiddleCompositeUpdatesLastTick;
		const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pMiddleComposite );
		m_nGpuRegionMiddleCompositeBytesLastTick += bytes;
		m_nGpuRegionUploadBytesLastTick += bytes;
		uploadBytes.add( bytes );
	}
	if ( rState.m_bTerrainShadowAllocated ) {
		m_generatedTerrainShadowMasks.commitLayerImage( rState.m_pTerrainShadowMask, rState.m_nLayer );
		++m_nGpuRegionTerrainShadowUpdatesLastTick;
		const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pTerrainShadowMask );
		m_nGpuRegionTerrainShadowBytesLastTick += bytes;
		m_nGpuRegionUploadBytesLastTick += bytes;
		uploadBytes.add( bytes );
	}
	m_szGpuRegionLayerChannelFlags[rState.m_nLayer] = rState.m_nPublishedFlags;
	m_szGpuRegionMaterialMasks[rState.m_nLayer] = rState.m_nMaterialMask;
	if ( rState.m_bDerivedChannelEnabled && !rState.m_bCookedPage && !IS_EDITOR &&
		 rState.m_pDerived != m_defaultDerivedMap ) {
		rState.m_pRegion->setDerivedNormalSlopeMapInternal( Ref< Image >() );
	}
	return true;
}

bool Terrain3DGpuRegionCache::uploadGpuRegionLayerDirect(
	t3dGpuRegionLayerUploadState_t &rState ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_REGION_TEXTURE_UPLOAD );
	Terrain3DStreamingProfileBytesScope uploadBytes( T3D_SP_REGION_TEXTURE_UPLOAD );
	bool upload_ok = true;
	if ( rState.m_bUploadHeight ) {
		upload_ok = m_generatedHeightMaps.update( rState.m_pHeight, rState.m_nLayer );
		if ( upload_ok ) {
			++m_nGpuRegionHeightUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pHeight );
			m_nGpuRegionHeightBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
			uploadBytes.add( bytes );
		}
		if ( upload_ok && rState.m_bDerivedChannelEnabled ) {
			upload_ok = m_generatedDerivedMaps.update( rState.m_pDerived, rState.m_nLayer );
		}
		if ( upload_ok && rState.m_bDerivedChannelEnabled ) {
			++m_nGpuRegionDerivedUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pDerived );
			m_nGpuRegionDerivedBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
			uploadBytes.add( bytes );
		}
		if ( rState.m_bDerivedChannelEnabled && !rState.m_bCookedPage && !IS_EDITOR &&
			 rState.m_pDerived != m_defaultDerivedMap ) {
			rState.m_pRegion->setDerivedNormalSlopeMapInternal( Ref< Image >() );
		}
	}
	if ( upload_ok && rState.m_bUploadControl ) {
		upload_ok = m_generatedControlMaps.update( rState.m_pControl, rState.m_nLayer );
		if ( upload_ok ) {
			++m_nGpuRegionControlUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pControl );
			m_nGpuRegionControlBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
			uploadBytes.add( bytes );
		}
	}
	if ( upload_ok && rState.m_bUploadColor ) {
		upload_ok = m_generatedColorMaps.update( rState.m_pColor, rState.m_nLayer );
		if ( upload_ok ) {
			++m_nGpuRegionColorUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pColor );
			m_nGpuRegionColorBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
			uploadBytes.add( bytes );
		}
	}
	if ( upload_ok && rState.m_bMiddleCompositeAllocated && rState.m_eMapType == REGION_TYPE_MAX ) {
		upload_ok = m_generatedMiddleCompositeMaps.update( rState.m_pMiddleComposite, rState.m_nLayer );
		if ( upload_ok ) {
			++m_nGpuRegionMiddleCompositeUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pMiddleComposite );
			m_nGpuRegionMiddleCompositeBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
			uploadBytes.add( bytes );
		}
	}
	if ( upload_ok && rState.m_bTerrainShadowAllocated && rState.m_eMapType == REGION_TYPE_MAX ) {
		upload_ok = m_generatedTerrainShadowMasks.update( rState.m_pTerrainShadowMask, rState.m_nLayer );
		if ( upload_ok ) {
			++m_nGpuRegionTerrainShadowUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( rState.m_pTerrainShadowMask );
			m_nGpuRegionTerrainShadowBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
			uploadBytes.add( bytes );
		}
	}
	if ( !upload_ok ) {
		m_szGpuRegionSlotStates[rState.m_nLayer] = rState.m_nPreviousState;
		// Retry on a later owner tick; preceding channel uploads consumed the
		// current reservation and the staging page remains unpublished.
		return false;
	}
	m_szGpuRegionLayerChannelFlags[rState.m_nLayer] = rState.m_nPublishedFlags;
	m_szGpuRegionMaterialMasks[rState.m_nLayer] = rState.m_nMaterialMask;
	m_szGpuRegionSlotStates[rState.m_nLayer] =
		t3dRegionCoordEquals( m_szGpuRegionLocations[rState.m_nLayer], rState.m_location ) ? TERRAIN3D_GPU_SLOT_MAPPED
																						   : TERRAIN3D_GPU_SLOT_READY;
	return true;
}

bool Terrain3DGpuRegionCache::uploadGpuRegionLayer(
	const int		   nLayer,
	const Vector2i	  &vLocation,
	const t3dMapType_t eMapType,
	const bool		   bAllowCookedPage,
	const uint8_t	   dirty_maps,
	const int		   copy_from_layer ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_REGION_UPLOAD_PREPARE );
	uint64_t						 prepared_request_usec = 0u;
	const Terrain3DProfileScopeTimer publication_timer( m_nGpuPreparePublicationUsecLast,
														prepared_request_usec,
														m_nGpuPrepareEndToEndUsecLast );
	t3dGpuRegionLayerUploadState_t	 state;
	state.m_nLayer = nLayer;
	state.m_location = vLocation;
	state.m_eMapType = eMapType;
	state.m_bAllowCookedPage = bAllowCookedPage;
	state.m_nDirtyMaps = dirty_maps;
	state.m_nCopyFromLayer = !Terrain3DRdContext::isOnRenderThread() ? copy_from_layer : -1;
	if ( !beginGpuRegionLayerUpload( state, prepared_request_usec ) || !resolveGpuRegionLayerImages( state ) ) {
		return false;
	}
	resolveGpuRegionLayerMetadata( state );
	if ( eMapType == REGION_TYPE_MAX && !Terrain3DRdContext::isOnRenderThread() ) {
		return submitGpuRegionLayerTrackedBatch( state );
	}
	return uploadGpuRegionLayerDirect( state );
}

bool Terrain3DGpuRegionCache::serviceRuntimePages(
	const t3dResidencyFrameState_s &rFrame ) {
	const int color_size = MAX( 1, T3D_REGION_SIZE / MAX( 1, rFrame.m_runtimePageConfig.m_nColorDivisor ) );
	if ( !refreshMiddleCompositeSources( rFrame.m_runtimePageConfig.m_nMaterialSourceSignature,
										 color_size,
										 color_size ) ) {
		return false;
	}
	t3dRuntimePageServiceState_t state;
	state.m_config = rFrame.m_runtimePageConfig;
	state.m_predictedCenter = rFrame.m_predictedCenter;
	state.m_nStreamingGeneration = rFrame.m_nStreamingGeneration;
	state.m_isWanted = &t3dResidencyFrameWantsRuntimePage;
	state.m_pWantedContext = const_cast< t3dResidencyFrameState_s * >( &rFrame );
	m_runtimePageStore.service( state );
	return true;
}

// Preserve the live allocation when the new grant maps to the same physical
// capacity/layout. Bookkeeping can advance without any RD work or map rebuild.
bool Terrain3DGpuRegionCache::applyBudgetGrantWithoutReconfigure(
    const uint64_t nBudgetBytes ) {
    if ( !m_bGpuRegionCacheTexturesReady || !m_bGpuRegionCacheEnabled || !m_pTerrain ) {
        return false;
    }
    const bool bAuthoring = m_pTerrain->isEditorAuthoringMode();
    const uint64_t hard_cap = ( uint64_t ) ( bAuthoring ? T3D_EDITOR_AUTHORING_GPU_REGION_MAX_BUDGET_MB
                                                     : T3D_GPU_REGION_CACHE_MAX_BUDGET_MB ) * T3D_GPU_CACHE_MIB;
    const uint64_t new_grant = nBudgetBytes < hard_cap ? nBudgetBytes : hard_cap;
    if ( new_grant == m_nGpuRegionCacheBudgetBytes ) {
        return true;
    }
    const uint64_t previous_grant = m_nGpuRegionCacheBudgetBytes;
    // These pure capacity calculators read the current grant. Temporarily
    // evaluate the candidate on the cold owner thread; do not publish it yet.
    m_nGpuRegionCacheBudgetBytes = new_grant;
    const int proposed_capacity = calculateGpuRegionCacheCapacity();
    const int proposed_layers = calculateGpuRegionPhysicalLayers( proposed_capacity );
    m_nGpuRegionCacheBudgetBytes = previous_grant;
    if ( proposed_capacity != m_nGpuRegionCacheCapacity || proposed_layers > m_nGpuRegionPhysicalLayerCount ||
         ( !bAuthoring && getReservedBytes() > new_grant ) ) {
        return false;
    }
    m_nGpuRegionCacheBudgetBytes = new_grant;
    ++m_nGrantChangeCount;
    return true;
}

bool Terrain3DGpuRegionCache::configure(
	const bool	   bEnabled,
	const uint64_t nBudgetBytes,
	const int	   nUploadLayers,
	const int	   nUploadBudgetMb,
	const int	   nSparePages,
	const int	   nRetireFrames ) {
	const bool	   bOldEnabled = m_bGpuRegionCacheEnabled;
	const uint64_t old_budget = m_nGpuRegionCacheBudgetBytes;
	const int	   old_upload_layers = m_nGpuRegionUploadBudgetLayers;
	const int	   old_upload_budget = m_nGpuRegionUploadBudgetMb;
	const int	   old_spare_pages = m_nGpuRegionSparePages;
	const int	   old_retire_frames = m_nGpuRegionRetireFrames;
	m_bGpuRegionCacheEnabled = bEnabled;
	const bool	   bAuthoring = m_pTerrain && m_pTerrain->isEditorAuthoringMode();
	const uint64_t hard_cap = ( uint64_t ) ( bAuthoring ? T3D_EDITOR_AUTHORING_GPU_REGION_MAX_BUDGET_MB
														: T3D_GPU_REGION_CACHE_MAX_BUDGET_MB ) *
							  T3D_GPU_CACHE_MIB;
	m_nGpuRegionCacheBudgetBytes = nBudgetBytes < hard_cap ? nBudgetBytes : hard_cap;
	m_nGpuRegionUploadBudgetLayers = CLAMP( nUploadLayers, 1, T3D_GPU_REGION_UPLOAD_MAX_LAYERS );
	m_nGpuRegionUploadBudgetMb = CLAMP( nUploadBudgetMb, 1, T3D_GPU_REGION_UPLOAD_MAX_BUDGET_MB );
	if ( old_upload_budget != m_nGpuRegionUploadBudgetMb ) {
		m_nGpuRegionUploadByteTokens = 0u;
		m_nGpuRegionUploadCallTokens = 0u;
		m_nGpuRegionUploadTokenRefillUsec = 0u;
	}
	const int page_limit =
		m_pTerrain && m_pTerrain->isEditorAuthoringMode()
			? CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, T3D_GPU_CACHE_EDITOR_MAX_LAYERS )
			: ( m_pTerrain ? CLAMP( m_pTerrain->getGpuRegionCachePageLimit(), 16, T3D_GPU_CACHE_RUNTIME_MAX_LAYERS )
						   : T3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	m_nGpuRegionSparePages = CLAMP( nSparePages, T3D_GPU_CACHE_MIN_SPARE_LAYERS, page_limit - 1 );
	m_nGpuRegionRetireFrames = CLAMP( nRetireFrames, 1, 16 );

	const int requested_pages = calculateGpuRegionRequestedPages();
	const int required_radius = calculateGpuRegionRequiredRadius();
	const int color_divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	const int derived_divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const int required_capacity = m_bGpuRegionCacheEnabled ? calculateGpuRegionCacheCapacity() : 0;
	const int required_physical_layers =
		m_bGpuRegionCacheEnabled ? calculateGpuRegionPhysicalLayers( required_capacity ) : 0;
	const bool color_channel_enabled = isColorChannelEnabled();
	const bool derived_channel_enabled = isDerivedChannelEnabled();
	const bool bGrantChanged = old_budget != m_nGpuRegionCacheBudgetBytes;
	if ( bGrantChanged ) {
		++m_nGrantChangeCount;
	}
	const bool bCapacityChanged = required_capacity != m_nGpuRegionCacheCapacity;
	const bool bLayoutChanged =
		m_bGpuRegionCacheEnabled && m_bGpuRegionCacheTexturesReady &&
		( color_divisor != m_nGpuRegionColorDivisor || derived_divisor != m_nGpuRegionDerivedDivisor ||
		  color_channel_enabled != m_bGpuRegionColorChannelAllocated ||
		  derived_channel_enabled != m_bGpuRegionDerivedChannelAllocated ||
		  required_physical_layers > m_nGpuRegionPhysicalLayerCount ||
		  getReservedBytes() > m_nGpuRegionCacheBudgetBytes );
	const bool bSelectionChanged =
		requested_pages != m_nGpuRegionRequestedPages || required_radius != m_nGpuRegionRequiredRadius;
	const bool bStorageChanged = bOldEnabled != m_bGpuRegionCacheEnabled || bLayoutChanged;
	if ( !bStorageChanged && !bSelectionChanged && !bCapacityChanged && !bGrantChanged &&
		 old_upload_layers == m_nGpuRegionUploadBudgetLayers && old_upload_budget == m_nGpuRegionUploadBudgetMb &&
		 old_spare_pages == m_nGpuRegionSparePages && old_retire_frames == m_nGpuRegionRetireFrames ) {
		return false;
	}
	m_nGpuRegionRequestedPages = requested_pages;
	m_nGpuRegionRequiredRadius = required_radius;
	m_bGpuRegionCacheSelectionDirty = m_bGpuRegionCacheSelectionDirty || bSelectionChanged || bCapacityChanged;
	if ( !bStorageChanged && m_bGpuRegionCacheTexturesReady ) {
		m_nGpuRegionCacheCapacity = required_capacity;
		m_nGpuRegionSpareLayerCount = MAX( 0, m_nGpuRegionPhysicalLayerCount - required_capacity );
	}
	if ( bStorageChanged && ( bOldEnabled || m_bGpuRegionCacheEnabled ) ) {
        m_nTextureRebuildFailures = 0u;
        m_nTextureRebuildRetryUsec = 0u;
		m_pStorageResetReason =
			bOldEnabled != m_bGpuRegionCacheEnabled
				? "enable/disable"
				: ( getReservedBytes() > m_nGpuRegionCacheBudgetBytes ? "allocation exceeds grant" : "texture layout" );
		reset( true );
	}
	return bStorageChanged;
}

t3dGpuRegionCacheUpdateResult_t Terrain3DGpuRegionCache::update(
	const t3dResidencyFrameState_s &rFrame,
	const bool						bForce,
	const int						nEffectiveUploadLayers,
	Terrain3DCompute			   *pCompute ) {
	const Terrain3DLastScopeTimer serviceTimer( m_nGpuRegionServiceUsecLast, rFrame.m_bProfilingEnabled );
	m_nGpuPreparationTickStartedUsec = Time::get_singleton()->get_ticks_usec();
	m_bGpuPreparationDeferred = false;
	m_nGpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_runtimePageStore.resetFrameStats();
	m_pCompute = pCompute;
	m_bPageTableChangedThisTick = false;
	m_bPageTableUploadReserved = false;
	m_nGpuPreparePublicationUsecLast = 0u;
	m_nGpuPrepareEndToEndUsecLast = 0u;
	m_runtimePageStore.poll( false );
	m_nGpuRegionUploadsLastTick = 0;
	m_nGpuRegionEvictionsLastTick = 0;
	m_nGpuRegionCookedHitsLastTick = 0;
	m_nGpuRegionCookedMissesLastTick = 0;
	m_nGpuRegionRuntimePreparesLastTick = 0;
	m_nGpuRegionReusedLayersLastTick = 0;
	m_nGpuRegionPageTableUpdatesLastTick = 0;
	m_nGpuRegionHeightUpdatesLastTick = 0;
	m_nGpuRegionControlUpdatesLastTick = 0;
	m_nGpuRegionColorUpdatesLastTick = 0;
	m_nGpuRegionDerivedUpdatesLastTick = 0;
	m_nGpuRegionMiddleCompositeUpdatesLastTick = 0;
	m_nGpuRegionTerrainShadowUpdatesLastTick = 0;
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	const t3dRuntimePageStats_t runtime_page_stats = m_runtimePageStore.getStats();
	m_nGpuRegionPreparedPagesLastTick = runtime_page_stats.m_nPreparedPages;
	m_nGpuRegionPrepareWorkersLastTick = runtime_page_stats.m_nPrepareWorkers;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionMiddleCompositeBytesLastTick = 0u;
	m_nGpuRegionTerrainShadowBytesLastTick = 0u;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	if ( !m_bGpuRegionCacheEnabled || T3D_REGION_SIZE <= 0 || s_fRegionVertexSpacing <= 0.0f ) {
		t3dGpuRegionCacheUpdateResult_t result;
		t3dClearTrivialObjectBytes( result );
		return result;
	}
	++m_nGpuRegionTick;
	finalizePendingGpuRegionUploads();
	trimGpuRegionCapacity();
	reclaimRetiredGpuRegionSlots();
	const uint64_t streaming_generation = rFrame.m_nStreamingGeneration;
	const Vector2i prediction_delta = rFrame.m_predictedCenter - rFrame.m_streamingCenter;
	m_gpuRegionPredictionDirection = Vector2i( SIGN( prediction_delta.x ), SIGN( prediction_delta.y ) );
	if ( !m_bGpuRegionCacheTexturesReady ) {
		const uint64_t now = Time::get_singleton()->get_ticks_usec();
		if ( now < m_nTextureRebuildRetryUsec ) {
			return t3dGpuRegionCacheUpdateResult_t{};
		}
		const uint32_t previous_failures = m_nTextureRebuildFailures;
		if ( !rebuildTextures() ) {
			// Preserve retry state across the storage reset inside rebuildTextures().
			m_nTextureRebuildFailures = MIN( previous_failures + 1u, 6u );
			const uint64_t delay = MIN( uint64_t( 1u << MIN( previous_failures, 5u ) ), uint64_t( 30u ) );
			m_nTextureRebuildRetryUsec = now + delay * 1000000u;
			return t3dGpuRegionCacheUpdateResult_t{};
		}
		m_nTextureRebuildFailures = 0u;
		m_nTextureRebuildRetryUsec = 0u;
	}
	const Vector2i center = rFrame.m_residencyCenter;
	m_bGpuRegionCameraMovedLastTick = m_gpuRegionCacheCenter != s_v2iMax && center != m_gpuRegionCacheCenter;
	const int runtime_upload_budget = nEffectiveUploadLayers;
	const int effective_upload_limit = MAX( 1, runtime_upload_budget );
	m_nGpuRegionEffectiveUploadLimitLastTick = effective_upload_limit;
	refillGpuUploadTokens( MAX( 7, effective_upload_limit * 6 + 1 ) );
	// Alternate the first claim on scarce byte/call tokens. Both paths share the
	// same layer counter; a continuous brush cannot monopolize cold residency.
	const bool republish_first = ( m_nGpuRegionTick & 1u ) == 0u;
	if ( republish_first ) {
		processPendingGpuRegionRepublishes( MAX( 1, effective_upload_limit / 2 ) );
	}
	if ( !bForce && center == m_gpuRegionCacheCenter && !m_bGpuRegionCacheSelectionDirty &&
		 streaming_generation == m_nGpuRegionStreamingGenerationLast ) {
		return t3dGpuRegionCacheUpdateResult_t{ m_bPageTableChangedThisTick };
	}

	Vector2i					desired[T3D_GPU_CACHE_MAX_LAYERS];
	t3dGpuCacheSelectionStats_t selection_stats;
	int desired_count = collectDesiredGpuRegions( rFrame, center, desired, m_nGpuRegionCacheCapacity, selection_stats );
	const int footprint_count = desired_count;
	m_nGpuRegionRequiredLastTick = selection_stats.m_nRequired;
	m_nGpuRegionGuardLastTick = selection_stats.m_nGuard;
	m_nGpuRegionPrefetchLastTick = selection_stats.m_nPrefetch;
	uint8_t desired_map[T3D_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
		const int layer = findGpuRegionSlot( desired[index] );
		if ( layer >= 0 ) {
			m_szGpuRegionLastUseTick[layer] = m_nGpuRegionTick;
			if ( index < selection_stats.m_nRequired && m_szGpuRegionPrefetchOrigin[layer] != 0u ) {
				++m_nGpuRegionPrefetchUsedTotal;
				m_szGpuRegionPrefetchOrigin[layer] = 0u;
			}
		}
	}

	const uint64_t hysteresis_ticks = ( uint64_t ) ( MAX( 0, rFrame.m_nGpuHysteresisTicks ) );
	const auto	   append_recent_layer = [&]( const int layer ) {
		const Vector2i location = t3dUnpackRegionCoord( m_szGpuRegionLocations[layer] );
		if ( location == s_v2iMax || m_szGpuRegionLastUseTick[layer] == 0u ||
			 m_nGpuRegionTick - m_szGpuRegionLastUseTick[layer] > hysteresis_ticks ) {
			return;
		}
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || desired_map[map_index] != 0u ) {
			return;
		}
		desired[desired_count++] = location;
		desired_map[map_index] = 1u;
		++m_nGpuRegionHysteresisLastTick;
	};
	if ( rFrame.m_bAuthoring && desired_count < m_nGpuRegionCacheCapacity ) {
		// E05: during authoring, preserve recently visited pages first. This is
		// a bounded cold-selection sort, not a render or upload hot-path sort.
		uint16_t recent_layers[T3D_GPU_CACHE_MAX_LAYERS];
		int		 recent_count = 0;
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
			if ( m_szGpuRegionLastUseTick[layer] != 0u &&
				 m_nGpuRegionTick - m_szGpuRegionLastUseTick[layer] <= hysteresis_ticks ) {
				recent_layers[recent_count++] = ( uint16_t ) layer;
			}
		}
		eastl::sort( recent_layers, recent_layers + recent_count, [this]( const uint16_t lhs, const uint16_t rhs ) {
			return m_szGpuRegionLastUseTick[lhs] > m_szGpuRegionLastUseTick[rhs];
		} );
		for ( int index = 0; index < recent_count && desired_count < m_nGpuRegionCacheCapacity; ++index ) {
			append_recent_layer( recent_layers[index] );
		}
	} else {
		// The shipping cache retains the previous physical-slot iteration order.
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && desired_count < m_nGpuRegionCacheCapacity;
			  ++layer ) {
			append_recent_layer( layer );
		}
	}
	m_nGpuRegionDesiredLastTick = desired_count;
	m_nGpuRegionPriorityCount = MIN( desired_count, T3D_GPU_CACHE_MAX_LAYERS );
	const int guard_end = selection_stats.m_nRequired + selection_stats.m_nGuard;
	const int priority_prefetch_end = guard_end + selection_stats.m_nPrefetch;
	for ( int index = 0; index < m_nGpuRegionPriorityCount; ++index ) {
		m_szGpuRegionPriorityLocations[index] = t3dPackRegionCoord( desired[index] );
		if ( index < selection_stats.m_nRequired ) {
			m_szGpuRegionPriorityClasses[index] = 1u;
		} else if ( index < guard_end ) {
			m_szGpuRegionPriorityClasses[index] = 2u;
		} else if ( index < priority_prefetch_end ) {
			m_szGpuRegionPriorityClasses[index] = 3u;
		} else {
			m_szGpuRegionPriorityClasses[index] = 4u;
		}
	}
	for ( int index = m_nGpuRegionPriorityCount; index < T3D_GPU_CACHE_MAX_LAYERS; ++index ) {
		m_szGpuRegionPriorityLocations[index] = t3dInvalidRegionCoord();
		m_szGpuRegionPriorityClasses[index] = 0u;
	}

	if ( rFrame.m_bCookedPagesEnabled ) {
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) >= 0 || findPendingGpuRegionUpload( desired[index] ) >= 0 ) {
				continue;
			}
			Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( desired[index] );
			if ( pRegion && !pRegion->isDeleted() && pRegion->runtimeMapsEvictableInternal() ) {
				const uint8_t priority = index < m_nGpuRegionPriorityCount ? m_szGpuRegionPriorityClasses[index] : 4u;
				uint16_t	  consumers = RUNTIME_PAGE_CONSUMER_GPU_DETAIL;
				const Ref< Terrain3DRegionLighting > lighting = pRegion->getLighting();
				if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
					consumers |= RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW;
				}
				m_runtimePageStore.request( desired[index], consumers, priority, rFrame.m_bCookedPagesEnabled );
			}
		}
		serviceRuntimePages( rFrame );
	}

	bool bMapChanged = false;
	bool bAllDesiredResident = true;
	for ( int desired_index = 0; desired_index < desired_count; ++desired_index ) {
		const Vector2i location = desired[desired_index];
		const int	   existing_layer = findGpuRegionSlot( location );
		if ( existing_layer >= 0 ) {
			m_szGpuRegionLastUseTick[existing_layer] = m_nGpuRegionTick;
			continue;
		}
		bAllDesiredResident = false;
		if ( findPendingGpuRegionUpload( location ) >= 0 ) {
			continue;
		}
		if ( m_nGpuRegionUploadsLastTick >= effective_upload_limit ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET;
			break;
		}

		const int target_layer = findFreeGpuRegionSlot();
		if ( target_layer < 0 ) {
			// Spare pages can be temporarily exhausted while old mappings remain
			// retired for in-flight rendering. Continue on a later frame rather than
			// synchronizing or overwriting a page still visible to the renderer.
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY;
			break;
		}
		Terrain3DRegion *const pUploadRegion = m_regionRepository.getRegionPtr( location );
		if ( !pUploadRegion || pUploadRegion->isDeleted() ) {
			continue;
		}

		int victim_layer = -1;
		if ( m_nGpuRegionResidentCount + m_nGpuRegionPendingUnmappedReservations >= m_nGpuRegionCacheCapacity ) {
			uint64_t oldest_tick = eastl::numeric_limits< uint64_t >::max();
			int		 farthest_distance = -1;
			for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
				const Vector2i old_location = t3dUnpackRegionCoord( m_szGpuRegionLocations[layer] );
				if ( old_location == s_v2iMax || m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
					continue;
				}
				if ( m_szGpuRegionVictimReserved[layer] != 0u ) {
					continue;
				}
				const int old_map_index = Terrain3DRegionRepository::getRegionMapIndex( old_location );
				if ( old_map_index >= 0 && desired_map[old_map_index] != 0u ) {
					continue;
				}
				const int	   distance = MAX( abs( old_location.x - center.x ), abs( old_location.y - center.y ) );
				const uint64_t last_use = m_szGpuRegionLastUseTick[layer];
				if ( last_use < oldest_tick || ( last_use == oldest_tick && distance > farthest_distance ) ) {
					oldest_tick = last_use;
					farthest_distance = distance;
					victim_layer = layer;
				}
			}
			if ( victim_layer < 0 ) {
				m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY;
				continue;
			}
		}

		// The staging layer is not referenced by the page table. All channel
		// updates therefore complete before any shader can sample the new page.
		if ( !submitBudgetedGpuRegionUpload( target_layer, location ) ) {
			if ( ( m_nGpuRegionStallFlagsLastTick & ( TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET |
													  TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET ) ) != 0u ) {
				break;
			}
			continue;
		}
		t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[target_layer];
		if ( pending.m_nGeneration != 0u ) {
			pending.m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_NEW;
			pending.m_nVictimLayer = victim_layer;
			pending.m_nLastUseTick = m_nGpuRegionTick;
			pending.m_nLoadedTick = m_nGpuRegionTick;
			const int prefetch_begin = selection_stats.m_nRequired + selection_stats.m_nGuard;
			const int prefetch_end = prefetch_begin + selection_stats.m_nPrefetch;
			pending.m_nPrefetchOrigin = desired_index >= prefetch_begin && desired_index < prefetch_end ? 1u : 0u;
			if ( victim_layer >= 0 ) {
				pending.m_victimLocation = m_szGpuRegionLocations[victim_layer];
				pending.m_nVictimLastUseTick = m_szGpuRegionLastUseTick[victim_layer];
				m_szGpuRegionVictimReserved[victim_layer] = 1u;
			} else {
				++m_nGpuRegionPendingUnmappedReservations;
			}
			continue;
		}

		if ( victim_layer >= 0 ) {
			const Vector2i old_location = t3dUnpackRegionCoord( m_szGpuRegionLocations[victim_layer] );
			const int	   old_map_index = Terrain3DRegionRepository::getRegionMapIndex( old_location );
			if ( old_map_index >= 0 ) {
				m_szGpuRegionMap[old_map_index] = 0u;
			}
			recordGpuRegionDeparture( victim_layer, true );
			retireGpuRegionSlot( victim_layer );
			++m_nGpuRegionEvictionsLastTick;
		} else {
			++m_nGpuRegionResidentCount;
		}
		m_szGpuRegionLocations[target_layer] = t3dPackRegionCoord( location );
		m_szGpuRegionLastUseTick[target_layer] = m_nGpuRegionTick;
		m_szGpuRegionLoadedTick[target_layer] = m_nGpuRegionTick;
		const int prefetch_begin = selection_stats.m_nRequired + selection_stats.m_nGuard;
		const int prefetch_end = prefetch_begin + selection_stats.m_nPrefetch;
		m_szGpuRegionPrefetchOrigin[target_layer] =
			desired_index >= prefetch_begin && desired_index < prefetch_end ? 1u : 0u;
		m_szGpuRegionSlotStates[target_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int new_map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( new_map_index >= 0 ) {
			m_szGpuRegionMap[new_map_index] = ( uint16_t ) ( target_layer + 1 );
		}
		++m_nGpuRegionUploadsSinceReset;
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
		bMapChanged = true;
	}

	if ( !republish_first ) {
		processPendingGpuRegionRepublishes( MAX( 0, effective_upload_limit - m_nGpuRegionUploadsLastTick ) );
	}
	if ( m_nGpuRegionRepublishPendingCount > 0 && m_nGpuRegionUploadsLastTick >= effective_upload_limit ) {
		m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET;
	}
	int		  resident_required = 0;
	const int written_required = MIN( selection_stats.m_nRequired, footprint_count );
	for ( int index = 0; index < written_required; ++index ) {
		if ( findGpuRegionSlot( desired[index] ) >= 0 ) {
			++resident_required;
		}
	}
	m_nGpuRegionMissingRequiredLastTick = selection_stats.m_nRequired - resident_required;
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) < 0 ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	m_gpuRegionCacheCenter = center;
	m_nGpuRegionStreamingGenerationLast = streaming_generation;
	m_bGpuRegionCacheSelectionDirty = !bAllDesiredResident || m_nGpuRegionMissingRequiredLastTick > 0 ||
									  m_nGpuRegionRepublishPendingCount > 0 ||
									  m_nGpuRegionResidentCount > m_nGpuRegionCacheCapacity;
	if ( bMapChanged ) {
		notePageTableChanged();
	}
	return t3dGpuRegionCacheUpdateResult_t{ m_bPageTableChangedThisTick };
}

bool Terrain3DGpuRegionCache::refreshTerrainShadowMasks(
	void ) {
	if ( !m_bGpuRegionCacheEnabled || !m_bGpuRegionCacheTexturesReady ) {
		return true;
	}
	( void ) republishAllResidents();
	return true;
}

bool Terrain3DGpuRegionCache::updateResidentLocation(
	const Vector2i	  &vLocation,
	const t3dMapType_t eMapType,
	Terrain3DCompute  *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	const uint8_t dirty_maps =
		eMapType >= TYPE_HEIGHT && eMapType < REGION_TYPE_MAX ? ( uint8_t ) ( 1u << eMapType ) : UINT8_MAX;
	queueRepublish( vLocation, dirty_maps );
	return false;
}

bool Terrain3DGpuRegionCache::republishAllResidents(
	Terrain3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
			 t3dRegionCoordValid( m_szGpuRegionLocations[layer] ) ) {
			queueRepublish( t3dUnpackRegionCoord( m_szGpuRegionLocations[layer] ) );
		}
	}
	return false;
}

TypedArray< Vector2i > Terrain3DGpuRegionCache::getRenderRegionSlots(
	void ) const {
	TypedArray< Vector2i > slots;
	slots.resize( m_nGpuRegionPhysicalLayerCount );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		slots[layer] = t3dUnpackRegionCoord( m_szGpuRegionLocations[layer] );
	}
	return slots;
}

int Terrain3DGpuRegionCache::getSlotStateForLocation(
	const Vector2i &vLocation ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( t3dRegionCoordEquals( m_szGpuRegionLocations[layer], vLocation ) ) {
			return ( int ) ( m_szGpuRegionSlotStates[layer] );
		}
	}
	return ( int ) ( TERRAIN3D_GPU_SLOT_FREE );
}

bool Terrain3DGpuRegionCache::isAuthoringFullResidencyPossible(
	void ) const {
	return !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ||
		   calculateGpuRegionCacheCapacity() >= m_regionRepository.getActiveRegionCount();
}

bool Terrain3DGpuRegionCache::isAuthoringReadbackSafe(
	void ) const {
	if ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() || !m_bGpuRegionCacheEnabled ) {
		return true;
	}
	if ( !m_bGpuRegionCacheTexturesReady || m_runtimePageStore.isPrepareJobActive() ||
		 m_bGpuRegionCacheSelectionDirty || m_nGpuRegionUploadsLastTick > 0 ) {
		return false;
	}
	return m_nGpuRegionTick >= m_nGpuRegionLastPublicationTick + 2u;
}

size_t Terrain3DGpuRegionCache::getReservedBytes(
	void ) const {
	const size_t shared_bytes = estimateSharedBytes();
	const size_t physical_texture_bytes = m_generatedHeightMaps.payloadBytes() + m_generatedControlMaps.payloadBytes() +
										  m_generatedColorMaps.payloadBytes() + m_generatedDerivedMaps.payloadBytes() +
										  m_generatedMiddleCompositeMaps.payloadBytes() +
										  m_generatedTerrainShadowMasks.payloadBytes();
	return shared_bytes + physical_texture_bytes;
}

size_t Terrain3DGpuRegionCache::getMinimumBytes(
	void ) const {
	const size_t shared_bytes = estimateSharedBytes();
	const size_t layer_bytes = estimateLayerBytes();
	if ( layer_bytes == 0u ) {
		return shared_bytes;
	}

	// The managed GPU-page grant must cover the configured detailed Near footprint,
	// not merely one page or one clipmap level. A zero detail override follows the
	// Near tier, so this remains coherent with runtime tier policy while Middle/Far
	// retain independent reduced/coarse representations.
	int		  minimum_mapped_pages = calculateRequiredCorePagesForLods( 0 );
	const int active_regions = m_regionRepository.getActiveRegionCount();
	if ( active_regions > 0 && minimum_mapped_pages > active_regions ) {
		minimum_mapped_pages = active_regions;
	}
	minimum_mapped_pages = MAX( minimum_mapped_pages, 1 );
	const size_t physical_layers = ( size_t ) ( minimum_mapped_pages + T3D_GPU_CACHE_MIN_SPARE_LAYERS );
	if ( physical_layers > ( SIZE_MAX - shared_bytes ) / layer_bytes ) {
		return SIZE_MAX;
	}
	return shared_bytes + layer_bytes * physical_layers;
}

size_t Terrain3DGpuRegionCache::getHardCapBytes(
	void ) const {
	const size_t shared_bytes = estimateSharedBytes();
	const size_t layer_bytes = estimateLayerBytes();
	const bool	 bAuthoring = m_pTerrain && m_pTerrain->isEditorAuthoringMode();
	const int	 page_limit =
		   bAuthoring
			   ? CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, T3D_GPU_CACHE_EDITOR_MAX_LAYERS )
			   : ( m_pTerrain ? CLAMP( m_pTerrain->getGpuRegionCachePageLimit(), 16, T3D_GPU_CACHE_RUNTIME_MAX_LAYERS )
							  : T3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	if ( layer_bytes == 0u || ( size_t ) ( page_limit ) > ( SIZE_MAX - shared_bytes ) / layer_bytes ) {
		return SIZE_MAX;
	}
	return shared_bytes + layer_bytes * ( size_t ) ( page_limit );
}

t3dGpuRegionCacheStats_t Terrain3DGpuRegionCache::getStats(
	void ) const {
	t3dGpuRegionCacheStats_t stats;
	stats.m_bEnabled = m_bGpuRegionCacheEnabled;
	stats.m_bTexturesReady = m_bGpuRegionCacheTexturesReady;
	stats.m_bSelectionPending = m_bGpuRegionCacheSelectionDirty;
	stats.m_bCameraMovedLastTick = m_bGpuRegionCameraMovedLastTick;
	stats.m_nBudgetBytes = m_nGpuRegionCacheBudgetBytes;
	stats.m_nBudgetMb = ( int ) ( m_nGpuRegionCacheBudgetBytes / ( uint64_t ) ( T3D_GPU_CACHE_MIB ) );
	stats.m_nCapacity = m_nGpuRegionCacheCapacity;
	stats.m_nPhysicalLayers = m_nGpuRegionPhysicalLayerCount;
	stats.m_nSpareLayers = m_nGpuRegionSpareLayerCount;
	stats.m_nResident = m_nGpuRegionResidentCount;
	stats.m_nRetired = m_nGpuRegionRetiredCount;
	stats.m_nDesiredLastTick = m_nGpuRegionDesiredLastTick;
	stats.m_nRequiredLastTick = m_nGpuRegionRequiredLastTick;
	stats.m_nGuardLastTick = m_nGpuRegionGuardLastTick;
	stats.m_nPrefetchLastTick = m_nGpuRegionPrefetchLastTick;
	stats.m_nHysteresisLastTick = m_nGpuRegionHysteresisLastTick;
	stats.m_nMissingRequiredLastTick = m_nGpuRegionMissingRequiredLastTick;
	stats.m_nRequestedPages = m_nGpuRegionRequestedPages;
	stats.m_nRequiredRadius = m_nGpuRegionRequiredRadius;
	stats.m_nUploadsLastTick = m_nGpuRegionUploadsLastTick;
	stats.m_nEvictionsLastTick = m_nGpuRegionEvictionsLastTick;
	stats.m_nCookedHitsLastTick = m_nGpuRegionCookedHitsLastTick;
	stats.m_nCookedMissesLastTick = m_nGpuRegionCookedMissesLastTick;
	stats.m_nRuntimePreparesLastTick = m_nGpuRegionRuntimePreparesLastTick;
	stats.m_nReusedLayersLastTick = m_nGpuRegionReusedLayersLastTick;
	stats.m_nPageTableUpdatesLastTick = m_nGpuRegionPageTableUpdatesLastTick;
	stats.m_nHeightUpdatesLastTick = m_nGpuRegionHeightUpdatesLastTick;
	stats.m_nControlUpdatesLastTick = m_nGpuRegionControlUpdatesLastTick;
	stats.m_nColorUpdatesLastTick = m_nGpuRegionColorUpdatesLastTick;
	stats.m_nDerivedUpdatesLastTick = m_nGpuRegionDerivedUpdatesLastTick;
	stats.m_nMiddleCompositeUpdatesLastTick = m_nGpuRegionMiddleCompositeUpdatesLastTick;
	stats.m_nTerrainShadowUpdatesLastTick = m_nGpuRegionTerrainShadowUpdatesLastTick;
	stats.m_nRuntimeColorMipsLastTick = m_nGpuRegionRuntimeColorMipsLastTick;
	stats.m_nRuntimeDerivedBuildsLastTick = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats.m_nPreparedPagesLastTick = m_nGpuRegionPreparedPagesLastTick;
	stats.m_nPrepareWorkersLastTick = m_nGpuRegionPrepareWorkersLastTick;
	stats.m_nEffectiveUploadLimitLastTick = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats.m_nStallFlagsLastTick = m_nGpuRegionStallFlagsLastTick;
	stats.m_nUploadBytesLastTick = m_nGpuRegionUploadBytesLastTick;
	stats.m_nHeightBytesLastTick = m_nGpuRegionHeightBytesLastTick;
	stats.m_nControlBytesLastTick = m_nGpuRegionControlBytesLastTick;
	stats.m_nColorBytesLastTick = m_nGpuRegionColorBytesLastTick;
	stats.m_nDerivedBytesLastTick = m_nGpuRegionDerivedBytesLastTick;
	stats.m_nMiddleCompositeBytesLastTick = m_nGpuRegionMiddleCompositeBytesLastTick;
	stats.m_nTerrainShadowBytesLastTick = m_nGpuRegionTerrainShadowBytesLastTick;
	stats.m_nServiceUsecLast = m_nGpuRegionServiceUsecLast;
	stats.m_nPreparePublicationUsecLast = m_nGpuPreparePublicationUsecLast;
	stats.m_nPrepareEndToEndUsecLast = m_nGpuPrepareEndToEndUsecLast;
	return stats;
}

Dictionary Terrain3DGpuRegionCache::getStatsDictionary(
	const bool bDetailed ) const {
	Dictionary stats;
	t3dAppendRegionCacheActivity( stats,
								  m_nGpuRegionResidentCount,
								  m_nGpuRegionDesiredLastTick,
								  m_nGpuRegionEvictionsLastTick );
	const t3dRuntimePageStats_t runtime = m_runtimePageStore.getStats();
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["grant_change_count"] = ( int64_t ) m_nGrantChangeCount;
	stats["physical_cache_rebuild_count"] = ( int64_t ) m_nPhysicalCacheRebuildCount;
	stats["storage_reset_count"] = ( int64_t ) m_nStorageResetCount;
	stats["storage_reset_reason"] = String( m_pStorageResetReason );
	stats["budget_bytes"] = ( int64_t ) ( m_nGpuRegionCacheBudgetBytes );
	stats["budget_mb"] = ( int64_t ) ( m_nGpuRegionCacheBudgetBytes / ( uint64_t ) ( T3D_GPU_CACHE_MIB ) );
	const uint64_t allocation_budget_bytes = m_pTerrain && m_pTerrain->isEditorAuthoringMode()
												 ? calculateAuthoringGpuRegionBudgetBytes()
												 : m_nGpuRegionCacheBudgetBytes;
	stats["allocation_budget_bytes"] = ( int64_t ) ( allocation_budget_bytes );
	stats["allocation_budget_mb"] = ( int64_t ) ( allocation_budget_bytes / ( uint64_t ) ( T3D_GPU_CACHE_MIB ) );
	const int runtime_page_limit =
		m_pTerrain ? CLAMP( m_pTerrain->getGpuRegionCachePageLimit(), 16, T3D_GPU_CACHE_RUNTIME_MAX_LAYERS )
				   : T3D_GPU_CACHE_RUNTIME_MAX_LAYERS;
	const int editor_page_limit =
		m_pTerrain ? CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, T3D_GPU_CACHE_EDITOR_MAX_LAYERS )
				   : T3D_GPU_CACHE_EDITOR_MAX_LAYERS;
	stats["configured_page_limit"] =
		m_pTerrain && m_pTerrain->isEditorAuthoringMode() ? editor_page_limit : runtime_page_limit;
	stats["runtime_page_limit"] = runtime_page_limit;
	stats["editor_page_limit"] = editor_page_limit;
	stats["required_core_pages"] = calculateRequiredCorePagesForLods( 0 );
	stats["functional_minimum_bytes"] = ( int64_t ) ( getMinimumBytes() );
	stats["hard_cap_bytes"] = ( int64_t ) ( getHardCapBytes() );
	stats["height_texture_ready"] = m_generatedHeightMaps.getRid().is_valid();
	stats["control_texture_ready"] = m_generatedControlMaps.getRid().is_valid();
	stats["color_texture_ready"] = m_generatedColorMaps.getRid().is_valid();
	stats["derived_texture_ready"] = m_generatedDerivedMaps.getRid().is_valid();
	stats["middle_composite_texture_ready"] = m_generatedMiddleCompositeMaps.getRid().is_valid();
	stats["terrain_shadow_texture_ready"] = m_generatedTerrainShadowMasks.getRid().is_valid();
	stats["capacity"] = m_nGpuRegionCacheCapacity;
	stats["physical_layers"] = m_nGpuRegionPhysicalLayerCount;
	stats["spare_layers"] = m_nGpuRegionSpareLayerCount;
	stats["retired_layers"] = m_nGpuRegionRetiredCount;
	stats["retire_frames"] = m_nGpuRegionRetireFrames;
	stats["republish_pending"] = m_nGpuRegionRepublishPendingCount;
	stats["requested_pages"] = m_nGpuRegionRequestedPages;
	stats["required_radius_regions"] = m_nGpuRegionRequiredRadius;
	stats["required_last_tick"] = m_nGpuRegionRequiredLastTick;
	stats["guard_last_tick"] = m_nGpuRegionGuardLastTick;
	stats["prefetch_last_tick"] = m_nGpuRegionPrefetchLastTick;
	stats["hysteresis_last_tick"] = m_nGpuRegionHysteresisLastTick;
	stats["missing_required_last_tick"] = m_nGpuRegionMissingRequiredLastTick;
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nGpuRegionStallFlagsLastTick );
	stats["upload_layer_budget"] = m_nGpuRegionUploadBudgetLayers;
	stats["upload_budget_mb"] = m_nGpuRegionUploadBudgetMb;
	stats["effective_upload_limit_last_tick"] = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t ) ( m_nGpuRegionUploadsSinceReset );
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["page_table_updates_last_tick"] = m_nGpuRegionPageTableUpdatesLastTick;
	stats["height_updates_last_tick"] = m_nGpuRegionHeightUpdatesLastTick;
	stats["control_updates_last_tick"] = m_nGpuRegionControlUpdatesLastTick;
	stats["color_updates_last_tick"] = m_nGpuRegionColorUpdatesLastTick;
	stats["derived_updates_last_tick"] = m_nGpuRegionDerivedUpdatesLastTick;
	stats["middle_composite_updates_last_tick"] = m_nGpuRegionMiddleCompositeUpdatesLastTick;
	stats["terrain_shadow_updates_last_tick"] = m_nGpuRegionTerrainShadowUpdatesLastTick;
	stats["runtime_color_mips_last_tick"] = m_nGpuRegionRuntimeColorMipsLastTick;
	stats["runtime_derived_builds_last_tick"] = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	// IO00: FileAccess-level source traffic, kept separate from VT in-memory payload copies.
	stats["source_io_open_calls"] = ( int64_t ) runtime.m_nSourceFileOpenCalls;
	stats["source_io_range_read_calls"] = ( int64_t ) runtime.m_nSourceRangeReadCalls;
	stats["source_io_range_read_bytes"] = ( int64_t ) runtime.m_nSourceRangeReadBytes;
	stats["source_io_distinct_range_bytes_window"] = ( int64_t ) runtime.m_nSourceDistinctRangeBytes;
	stats["source_io_duplicate_range_reads_window"] = ( int64_t ) runtime.m_nSourceDuplicateRangeReads;
	stats["source_io_duplicate_range_bytes_window"] = ( int64_t ) runtime.m_nSourceDuplicateRangeBytes;
	stats["source_io_cache_hits"] = ( int64_t ) runtime.m_nSourceCacheHits;
	stats["source_io_cache_hit_bytes"] = ( int64_t ) runtime.m_nSourceCacheHitBytes;
	stats["source_io_singleflight_joins"] = ( int64_t ) runtime.m_nSourceInFlightJoins;
	stats["source_io_cache_resident_bytes"] = ( int64_t ) runtime.m_nSourceCacheResidentBytes;
	stats["source_io_cache_evictions"] = ( int64_t ) runtime.m_nSourceCacheEvictions;
	stats["source_io_metadata_hits"] = ( int64_t ) runtime.m_nSourceMetadataHits;
	stats["source_io_metadata_reads"] = ( int64_t ) runtime.m_nSourceMetadataReads;
	stats["source_io_pack_hits"] = ( int64_t ) runtime.m_nSourcePackHits;
	stats["source_io_pack_opens"] = ( int64_t ) runtime.m_nSourcePackOpens;
	stats["source_io_read_chunks"] = ( int64_t ) runtime.m_nSourceReadChunks;
	stats["source_io_read_waits"] = ( int64_t ) runtime.m_nSourceReadWaits;
	stats["source_io_priority_promotions"] = ( int64_t ) runtime.m_nSourceReadPromotions;
	stats["prepare_job_active"] = runtime.m_bPrepareJobActive;
	stats["prepare_job_pending_pages"] = runtime.m_nPendingPages;
	stats["prepare_ready_pages"] = runtime.m_nPreparedPages;
	stats["prepare_byte_budget"] = ( int64_t ) runtime.m_nPreparedByteBudget;
	stats["prepare_reserved_bytes"] = ( int64_t ) runtime.m_nPreparedReservedBytes;
	stats["prepare_payload_bytes_per_slot"] = ( int64_t ) runtime.m_nPayloadBytesPerSlot;
	stats["prepare_worker_scratch_reserved_bytes"] = ( int64_t ) runtime.m_nWorkerScratchReservedBytes;
	stats["prepare_configured_workers"] = runtime.m_nConfiguredPrepareWorkers;
	stats["prepare_effective_workers"] = runtime.m_nEffectivePrepareWorkers;
	stats["prepare_capacity_failure_latched"] = runtime.m_bCapacityFailureLatched;
	stats["prepare_publication_usec_last"] = ( int64_t ) ( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t ) ( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nGpuRegionServiceUsecLast );
	stats["upload_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionUploadBytesLastTick );
	stats["height_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionHeightBytesLastTick );
	stats["derived_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionDerivedBytesLastTick );
	stats["control_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionControlBytesLastTick );
	stats["color_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionColorBytesLastTick );
	stats["middle_composite_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionMiddleCompositeBytesLastTick );
	stats["terrain_shadow_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionTerrainShadowBytesLastTick );
	stats["upload_byte_tokens"] = ( int64_t ) ( m_nGpuRegionUploadByteTokens );
	stats["upload_call_tokens"] = ( int64_t ) ( m_nGpuRegionUploadCallTokens );
	stats["reserved_bytes"] = ( int64_t ) ( getReservedBytes() );
	stats["estimated_layer_bytes"] = ( int64_t ) ( estimateLayerBytes() );
	stats["estimated_shared_bytes"] = ( int64_t ) ( estimateSharedBytes() );
	stats["authoring_bounded_pool_covers_all_regions"] = isAuthoringFullResidencyPossible();
	stats["authoring_readback_safe"] = isAuthoringReadbackSafe();
	stats["control_implicit"] = m_bControlMapsImplicit;
	stats["color_implicit"] = m_bColorMapsImplicit;
	stats["color_resolution_divisor"] = m_nGpuRegionColorDivisor;
	stats["derived_resolution_divisor"] = m_nGpuRegionDerivedDivisor;
	stats["center"] = m_gpuRegionCacheCenter;
	stats["selection_pending"] = m_bGpuRegionCacheSelectionDirty;
	if ( !bDetailed ) {
		return stats;
	}
	stats["prediction_direction"] = m_gpuRegionPredictionDirection;
	stats["streaming_generation_last"] = ( int64_t ) ( m_nGpuRegionStreamingGenerationLast );
	stats["prefetch_used_total"] = ( int64_t ) ( m_nGpuRegionPrefetchUsedTotal );
	stats["prefetch_discarded_total"] = ( int64_t ) ( m_nGpuRegionPrefetchDiscardedTotal );
	const uint64_t completed_prefetches = m_nGpuRegionPrefetchUsedTotal + m_nGpuRegionPrefetchDiscardedTotal;
	stats["prefetch_accuracy"] =
		completed_prefetches > 0u ? ( double ) ( m_nGpuRegionPrefetchUsedTotal ) / ( double ) ( completed_prefetches )
								  : -1.0;
	stats["departed_pages_total"] = ( int64_t ) ( m_nGpuRegionDepartedPagesTotal );
	stats["average_resident_ticks"] =
		m_nGpuRegionDepartedPagesTotal > 0u
			? ( double ) ( m_nGpuRegionResidentTicksTotal ) / ( double ) ( m_nGpuRegionDepartedPagesTotal )
			: 0.0;
	return stats;
}

uint64_t Terrain3DGpuRegionCache::getPhysicalChannelBytes(
	uint32_t channel ) const {
	switch ( channel ) {
	case 0u:
		return m_generatedHeightMaps.payloadBytes();
	case 1u:
		return m_generatedControlMaps.payloadBytes();
	case 2u:
		return m_generatedColorMaps.payloadBytes();
	case 3u:
		return m_generatedDerivedMaps.payloadBytes();
	case 4u:
		return m_generatedMiddleCompositeMaps.payloadBytes();
	case 5u:
		return m_generatedTerrainShadowMasks.payloadBytes();
	default:
		return 0u;
	}
}
