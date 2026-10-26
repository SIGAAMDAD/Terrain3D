// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/terrain_3d_instance_publication_builder.h"

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "data/terrain_3d_half_buffer.h"
#include "data/terrain_3d_region.h"
#include "hlod/terrain_3d_hlod_streamer.h"
#include "instances/terrain_3d_instance_store.h"
#include "instances/terrain_3d_instance_occlusion.h"
#include "rendering/rd/terrain_3d_rd_instance_renderer.h"
#include "terrain_3d.h"

#include <math.h>
#include <stdint.h>
#include <godot_cpp/variant/utility_functions.hpp>

static int t3dInstancePublicationCategoryIndex( const t3dInstanceCategory_t eCategory ) {
	return CLAMP( ( int )( eCategory ), ( int )( INSTANCE_CATEGORY_CLUTTER ), ( int )( INSTANCE_CATEGORY_OCCLUDER ) );
}

static uint32_t t3dInstancePublicationCategoryFlags( const t3dInstanceCategory_t eCategory ) {
	const uint32_t category = ( uint32_t )( t3dInstancePublicationCategoryIndex( eCategory ) );
	return ( category << T3D_RD_INSTANCE_CATEGORY_SHIFT ) & T3D_RD_INSTANCE_CATEGORY_MASK;
}

static uint32_t t3dInstancePublicationPackSnormPair( const float x, const float y ) {
	const int16_t px = ( int16_t )( lround( CLAMP( x, -1.0f, 1.0f ) * 32767.0f ) );
	const int16_t py = ( int16_t )( lround( CLAMP( y, -1.0f, 1.0f ) * 32767.0f ) );
	return ( uint32_t )( ( uint16_t )( px ) ) | ( ( uint32_t )( ( uint16_t )( py ) ) << 16u );
}

static t3dRdInstanceRecord_t t3dInstancePublicationPackRecord( const Transform3D &rTransform, const uint32_t nColor ) {
	t3dRdInstanceRecord_t record;
	record.m_szPosition[0] = ( float )( rTransform.origin.x );
	record.m_szPosition[1] = ( float )( rTransform.origin.y );
	record.m_szPosition[2] = ( float )( rTransform.origin.z );
	const Vector3 scale = rTransform.basis.get_scale();
	const Quaternion rotation = rTransform.basis.orthonormalized().get_rotation_quaternion().normalized();
	record.m_nQuaternion01 = t3dInstancePublicationPackSnormPair( ( float )( rotation.x ), ( float )( rotation.y ) );
	record.m_nQuaternion23 = t3dInstancePublicationPackSnormPair( ( float )( rotation.z ), ( float )( rotation.w ) );
	record.m_nScale01 =
	        ( uint32_t )( t3dHalfEncode( ( float )( scale.x ) ) ) | ( ( uint32_t )( t3dHalfEncode( ( float )( scale.y ) ) ) << 16u );
	record.m_nScale2Flags = ( uint32_t )( t3dHalfEncode( ( float )( scale.z ) ) );
	record.m_nColorRgba8 = t3dRdPackGodotRgba32ForGpu( nColor );
	return record;
}

static float t3dInstancePublicationPointRectMinDistanceSquared(
        const Vector3 &rPoint, const float fMinX, const float fMinZ, const float fMaxX, const float fMaxZ ) {
	const float dx = ( float )( rPoint.x ) < fMinX ? fMinX - ( float )( rPoint.x )
	                                               : ( ( float )( rPoint.x ) > fMaxX ? ( float )( rPoint.x ) - fMaxX : 0.0f );
	const float dz = ( float )( rPoint.z ) < fMinZ ? fMinZ - ( float )( rPoint.z )
	                                               : ( ( float )( rPoint.z ) > fMaxZ ? ( float )( rPoint.z ) - fMaxZ : 0.0f );
	return dx * dx + dz * dz;
}

static int t3dInstancePublicationSelectLod( const Terrain3DMeshAsset &rAsset,
        const Vector2i &rRegionLocation,
        const Vector2i &rCellLocation,
        const int nCellSize,
        const Vector3 &rTargetPosition ) {
	const int lod_count = rAsset.getLodCount();
	if ( lod_count <= 0 ) {
		return -1;
	}
	int first_lod = 0;
	int last_lod = MIN( MIN( rAsset.getLastLod(), lod_count - 1 ), rAsset.getCategoryMaximumLodCount() - 1 );
	if ( rAsset.getCastShadows() == s_shadowCastingOnly ) {
		first_lod = MAX( rAsset.getShadowImpostor(), 0 );
		last_lod = MIN( last_lod, rAsset.getLastShadowLod() );
	}
	if ( last_lod < first_lod ) {
		return -1;
	}

	const float region_x = ( float )( rRegionLocation.x * s_nRegionWorldSizeMeters );
	const float region_z = ( float )( rRegionLocation.y * s_nRegionWorldSizeMeters );
	const float min_x = region_x + ( float )( rCellLocation.x * nCellSize );
	const float min_z = region_z + ( float )( rCellLocation.y * nCellSize );
	const float max_x = min_x + ( float )( nCellSize );
	const float max_z = min_z + ( float )( nCellSize );
	const float distance_sq = t3dInstancePublicationPointRectMinDistanceSquared( rTargetPosition, min_x, min_z, max_x, max_z );

	for ( int lod = first_lod; lod <= last_lod; ++lod ) {
		const float end = ( float )( rAsset.getLodRangeEnd( lod ) );
		if ( end <= 0.001f || distance_sq < end * end ) {
			return lod;
		}
	}
	const float last_end = ( float )( rAsset.getLodRangeEnd( last_lod ) );
	const float fade = ( float )( rAsset.getFadeMargin() );
	return last_end <= 0.001f || distance_sq < ( last_end + fade ) * ( last_end + fade ) ? last_lod : -1;
}

static int t3dInstancePublicationResolveSourceLod( const Terrain3DMeshAsset &rAsset, const int nDesiredLod, const int nMinimumLod ) {
	for ( int lod = nDesiredLod; lod >= nMinimumLod; --lod ) {
		if ( lod < rAsset.getLodCount() && rAsset.getMesh( lod ).is_valid() ) {
			return lod;
		}
	}
	return -1;
}

static bool t3dInstancePublicationBuildConservativeSourceAabb( const Terrain3DMeshAsset &rAsset, AABB &rAabb ) {
	bool initialized = false;
	for ( int lod = 0; lod < rAsset.getLodCount(); ++lod ) {
		const Ref<Mesh> mesh = rAsset.getMesh( lod );
		if ( mesh.is_null() ) {
			continue;
		}
		const AABB source = mesh->get_aabb();
		if ( !source.has_surface() ) {
			continue;
		}
		if ( !initialized ) {
			rAabb = source;
			initialized = true;
		} else {
			rAabb.merge_with( source );
		}
	}
	return initialized;
}

static bool t3dInstancePublicationEnsureCellBounds(
        t3dInstanceCell_t &rCell, const AABB &rConservativeSourceAabb, t3dInstancePublicationDiagnostics_t &rDiagnostics ) {
	const bool source_changed = !rCell.m_bCachedAabbValid || !rCell.m_cachedSourceMeshAabb.is_equal_approx( rConservativeSourceAabb );
	if ( rCell.m_bCachedAabbValid && rCell.m_nCachedAabbRevision == rCell.m_nRevision && !source_changed ) {
		++rDiagnostics.m_nBoundsReused;
		return rCell.m_cachedAabb.has_surface();
	}

	bool initialized = false;
	AABB bounds;
	if ( rConservativeSourceAabb.has_surface() ) {
		for ( size_t iInstance = 0u; iInstance < rCell.m_transforms.size(); ++iInstance ) {
			const AABB transformed = rCell.getTransform( iInstance ).xform( rConservativeSourceAabb );
			if ( !initialized ) {
				bounds = transformed;
				initialized = true;
			} else {
				bounds.merge_with( transformed );
			}
		}
	}
	rCell.m_cachedAabb = initialized ? bounds : AABB();
	rCell.m_cachedSourceMeshAabb = rConservativeSourceAabb;
	rCell.m_nCachedAabbRevision = rCell.m_nRevision;
	if ( ++rCell.m_nCachedAabbGeneration == 0u ) {
		rCell.m_nCachedAabbGeneration = 1u;
	}
	rCell.m_bCachedAabbValid = true;
	++rDiagnostics.m_nBoundsRebuilt;
	return initialized;
}

static bool t3dInstancePublicationSameLayout( const t3dRdInstanceCell_t &a, const t3dRdInstanceCell_t &b ) {
	return a.m_nRegionX == b.m_nRegionX && a.m_nRegionZ == b.m_nRegionZ && a.m_nCellX == b.m_nCellX && a.m_nCellZ == b.m_nCellZ
	        && a.m_nMeshAssetId == b.m_nMeshAssetId && a.m_nFirstInstance == b.m_nFirstInstance && a.m_nInstanceCount == b.m_nInstanceCount;
}

static bool t3dInstancePublicationSameDescriptor( const t3dRdInstanceCell_t &a, const t3dRdInstanceCell_t &b ) {
	return t3dInstancePublicationSameLayout( a, b ) && a.m_nMeshSourceKey == b.m_nMeshSourceKey && a.m_nLod == b.m_nLod
	        && a.m_nRevision == b.m_nRevision && a.m_nSettingsRevision == b.m_nSettingsRevision
	        && a.m_nVisibilityLayers == b.m_nVisibilityLayers && a.m_nFlags == b.m_nFlags && a.m_fLodBegin == b.m_fLodBegin
	        && a.m_fLodEnd == b.m_fLodEnd && a.m_fFadeMargin == b.m_fFadeMargin && a.m_bounds.is_equal_approx( b.m_bounds )
	        && a.m_occlusionBounds.is_equal_approx( b.m_occlusionBounds ) && a.m_nOcclusionSourceRevision == b.m_nOcclusionSourceRevision
	        && a.m_nOcclusionProxyGeneration == b.m_nOcclusionProxyGeneration && a.m_nOcclusionProxyPolicy == b.m_nOcclusionProxyPolicy;
}

void t3dInstancePublicationDiagnostics_s::reset( void ) {
	for ( int category = 0; category < 3; ++category ) {
		m_categories[category] = t3dInstancePublicationCategoryDiagnostics_t();
	}
	m_nResidentSourceRecords = 0u;
	m_nPublishedRecords = 0u;
	m_nPublishedCells = 0u;
	m_nRenderableAssetSourceRecords = 0u;
	m_nSkippedMissingAssetRecords = 0u;
	m_nInvalidAssetIdRecords = 0u;
	m_nMissingAssetRecords = 0u;
	m_nDisabledAssetRecords = 0u;
	m_nNoLodAssetRecords = 0u;
	m_nAssetRegistryDeferredBuilds = 0u;
	m_nFirstInvalidAssetId = -1;
	m_nFirstMissingAssetId = -1;
	m_nFirstDisabledAssetId = -1;
	m_nFirstNoLodAssetId = -1;
	m_nRegionResidencyFilteredCells = 0u;
	m_nPackedRecordsReused = 0u;
	m_nPackedRecordsRebuilt = 0u;
	m_nBoundsReused = 0u;
	m_nBoundsRebuilt = 0u;
	m_nPolicyOnlyCellUpdates = 0u;
	m_nDirtyRecordRanges = 0u;
	m_nInstanceLayoutChanges = 0u;
}

typedef struct t3dInstancePublicationBuildState_s {
	const t3dInstancePublicationBuildContext_t *m_pContext = NULL;
	t3dRdInstanceSceneSnapshot_s *m_pSnapshot = NULL;
	t3dInstancePublicationDiagnostics_t *m_pDiagnostics = NULL;
	Terrain3DInstanceOcclusion *m_pOcclusion = NULL;
	const t3dRdInstanceCell_t *m_pPreviousCells = NULL;
	size_t m_nPreviousInstanceCount = 0u;
	size_t m_nPreviousCellCount = 0u;
	Ref<Terrain3DAssets> m_assets;
} t3dInstancePublicationBuildState_t;

static void t3dInstancePublicationMarkUnavailableAsset( t3dInstancePublicationBuildState_t &rState,
        const int nMeshId,
        const bool bAssetIdValid,
        const Ref<Terrain3DMeshAsset> &rAsset,
        const bool bAssetEnabled,
        const size_t nSourceCount ) {
	t3dInstancePublicationDiagnostics_t &diagnostics = *rState.m_pDiagnostics;
	diagnostics.m_nSkippedMissingAssetRecords += ( uint64_t )( nSourceCount );
	if ( !bAssetIdValid ) {
		diagnostics.m_nInvalidAssetIdRecords += ( uint64_t )( nSourceCount );
		if ( diagnostics.m_nFirstInvalidAssetId < 0 ) {
			diagnostics.m_nFirstInvalidAssetId = nMeshId;
		}
	} else if ( rAsset.is_null() ) {
		diagnostics.m_nMissingAssetRecords += ( uint64_t )( nSourceCount );
		if ( diagnostics.m_nFirstMissingAssetId < 0 ) {
			diagnostics.m_nFirstMissingAssetId = nMeshId;
		}
	} else if ( !bAssetEnabled ) {
		diagnostics.m_nDisabledAssetRecords += ( uint64_t )( nSourceCount );
		if ( diagnostics.m_nFirstDisabledAssetId < 0 ) {
			diagnostics.m_nFirstDisabledAssetId = nMeshId;
		}
	} else {
		diagnostics.m_nNoLodAssetRecords += ( uint64_t )( nSourceCount );
		if ( diagnostics.m_nFirstNoLodAssetId < 0 ) {
			diagnostics.m_nFirstNoLodAssetId = nMeshId;
		}
	}
}

static bool t3dInstancePublicationAcceptCell( t3dInstancePublicationBuildState_t &rState,
        t3dInstanceMesh_t &rMesh,
        t3dInstanceCell_t &rSourceCell,
        const Ref<Terrain3DMeshAsset> &rAsset,
        const bool bAssetIdValid,
        const bool bAssetEnabled,
        const bool bAssetValid,
        const t3dHlodInstancerRepresentationState_t &rHlodState,
        bool &rHlodSourceCounted,
        t3dInstancePublicationCategoryDiagnostics_t &rCategory,
        const Vector2i &rRegionLocation,
        const AABB &rConservativeSourceAabb,
        const bool bConservativeBoundsValid,
        int &rDesiredLod,
        int &rSourceLod ) {
	const size_t source_count = rSourceCell.m_transforms.size();
	if ( source_count == 0u ) {
		return false;
	}

	t3dInstancePublicationDiagnostics_t &diagnostics = *rState.m_pDiagnostics;
	diagnostics.m_nResidentSourceRecords += ( uint64_t )( source_count );
	if ( !bAssetValid ) {
		t3dInstancePublicationMarkUnavailableAsset( rState, rMesh.m_nId, bAssetIdValid, rAsset, bAssetEnabled, source_count );
		return false;
	}
	diagnostics.m_nRenderableAssetSourceRecords += ( uint64_t )( source_count );

	if ( rHlodState.m_nMinimumLevel > 0u ) {
		if ( !rHlodSourceCounted ) {
			++rCategory.m_nHlodEligibleSources;
			if ( rHlodState.m_nActiveLevel >= rHlodState.m_nMinimumLevel && rHlodState.m_nResidentLevel >= rHlodState.m_nMinimumLevel ) {
				++rCategory.m_nHlodActiveReplacements;
			}
			rHlodSourceCounted = true;
		}
		const bool active_replacement = rHlodState.m_nActiveLevel >= rHlodState.m_nMinimumLevel;
		const bool resident_replacement = rHlodState.m_nResidentLevel >= rHlodState.m_nMinimumLevel;
		const bool retiring_replacement = rHlodState.m_nRetiringLevel >= rHlodState.m_nMinimumLevel;
		if ( active_replacement && resident_replacement ) {
			++rCategory.m_nHlodDetailSuppressedCells;
			return false;
		}
		if ( active_replacement && !resident_replacement ) {
			++rCategory.m_nHlodTransitionGapErrors;
			UtilityFunctions::push_error(
			        "Terrain3DInstancePublicationBuilder: HLOD detail suppression requested without resident replacement" );
		}
		++rCategory.m_nHlodDetailRetainedMissingProxy;
		if ( retiring_replacement && !active_replacement ) {
			++rCategory.m_nHlodTransitionOverlapCells;
		}
	}

	if ( rAsset->getVisibilityLayers() == 0u ) {
		++rCategory.m_nVisibilityLayerFilteredCells;
		return false;
	}
	rDesiredLod = t3dInstancePublicationSelectLod( *rAsset.ptr(),
	        rRegionLocation,
	        rSourceCell.m_location,
	        MAX( rState.m_pContext->m_nCellSize, 1 ),
	        rState.m_pContext->m_targetPosition );
	if ( rDesiredLod < 0 ) {
		return false;
	}
	const int minimum_source_lod = rAsset->getCastShadows() == s_shadowCastingOnly ? MAX( rAsset->getShadowImpostor(), 0 ) : 0;
	rSourceLod = t3dInstancePublicationResolveSourceLod( *rAsset.ptr(), rDesiredLod, minimum_source_lod );
	if ( rSourceLod < 0 ) {
		++rCategory.m_nLodMissingSourceSkips;
		return false;
	}
	if ( rSourceLod != rDesiredLod ) {
		++rCategory.m_nLodFallbacks;
	}
	const Ref<Mesh> source_mesh = rAsset->getMesh( rSourceLod );
	if ( source_mesh.is_null() || !bConservativeBoundsValid ) {
		++rCategory.m_nLodMissingSourceSkips;
		return false;
	}
	if ( source_count > UINT32_MAX || rState.m_pSnapshot->m_instances.size() + source_count > rState.m_pSnapshot->m_instances.capacity()
	        || rState.m_pSnapshot->m_cells.size() >= rState.m_pSnapshot->m_cells.capacity() ) {
		UtilityFunctions::push_error( "Terrain3DInstancePublicationBuilder: fixed snapshot capacity exceeded" );
		return false;
	}
	if ( !t3dInstancePublicationEnsureCellBounds( rSourceCell, rConservativeSourceAabb, diagnostics ) ) {
		++rCategory.m_nLodMissingSourceSkips;
		return false;
	}
	return true;
}

static void t3dInstancePublicationPopulateCell( t3dInstancePublicationBuildState_t &rState,
        const t3dInstanceMesh_t &rMesh,
        const t3dInstanceCell_t &rSourceCell,
        const Ref<Terrain3DMeshAsset> &rAsset,
        const Vector2i &rRegionLocation,
        const Vector3 &rRegionOffset,
        const int nDesiredLod,
        const int nSourceLod,
        t3dInstancePublicationCategoryDiagnostics_t &rCategory,
        t3dRdInstanceCell_t &rCell ) {
	rCell.m_nRegionX = rRegionLocation.x;
	rCell.m_nRegionZ = rRegionLocation.y;
	rCell.m_nCellX = ( int16_t )( CLAMP( rSourceCell.m_location.x, ( int )( INT16_MIN ), ( int )( INT16_MAX ) ) );
	rCell.m_nCellZ = ( int16_t )( CLAMP( rSourceCell.m_location.y, ( int )( INT16_MIN ), ( int )( INT16_MAX ) ) );
	rCell.m_nMeshAssetId = ( uint16_t )( rMesh.m_nId );
	rCell.m_nLod = ( int16_t )( nSourceLod );
	rCell.m_nMeshSourceKey = t3dRdRegularMeshSourceKey( rCell.m_nMeshAssetId, nSourceLod );
	rCell.m_nFirstInstance = ( uint32_t )( rState.m_pSnapshot->m_instances.size() );
	rCell.m_nInstanceCount = ( uint32_t )( rSourceCell.m_transforms.size() );
	rCell.m_nRevision = rSourceCell.m_nRevision;
	rCell.m_nSettingsRevision = rState.m_pContext->m_pMeshSettingsRevision ? rState.m_pContext->m_pMeshSettingsRevision[rMesh.m_nId] : 0u;
	rCell.m_nVisibilityLayers = rAsset->getVisibilityLayers();
	rCell.m_nFlags = t3dInstancePublicationCategoryFlags( rAsset->getInstanceCategory() ) | T3D_RD_INSTANCE_LOD_PRESELECTED;
	rCell.m_fLodBegin = ( float )( rAsset->getLodRangeBegin( nDesiredLod ) );
	rCell.m_fLodEnd = ( float )( rAsset->getLodRangeEnd( nDesiredLod ) );
	rCell.m_fFadeMargin = ( float )( rAsset->getFadeMargin() );
	if ( rCell.m_fFadeMargin > 0.0f ) {
		++rCategory.m_nFadeActiveCells;
	}
	const ShadowCasting cast = rAsset->getLodCastShadows( nSourceLod );
	if ( cast != s_shadowCastingOff ) {
		rCell.m_nFlags |= T3D_RD_INSTANCE_CAST_SHADOWS;
	}
	if ( cast == GeometryInstance3D::SHADOW_CASTING_SETTING_DOUBLE_SIDED ) {
		rCell.m_nFlags |= T3D_RD_INSTANCE_SHADOW_DOUBLE_SIDED;
	}
	if ( cast == s_shadowCastingOnly ) {
		rCell.m_nFlags |= T3D_RD_INSTANCE_SHADOWS_ONLY;
	}
	if ( rAsset->isHighlighted() ) {
		rCell.m_nFlags |= T3D_RD_INSTANCE_HIGHLIGHTED;
	}
	rCell.m_bounds = rSourceCell.m_cachedAabb;
	rCell.m_bounds.position += rRegionOffset;
#if T3D_INSTANCER_HIZ_OCCLUSION
	const t3dInstanceCategory_t instance_category = rAsset->getInstanceCategory();
	if ( instance_category != INSTANCE_CATEGORY_OCCLUDER ) {
		// Hi-Z occludee eligibility is independent from explicit proxy ownership.
		// Visible bounds are the conservative test volume for ordinary instances.
		rCell.m_occlusionBounds = rCell.m_bounds;
		rCell.m_nFlags |= T3D_RD_INSTANCE_HIZ_CELL;
	} else if ( rState.m_pOcclusion && rAsset->usesInstanceOcclusion() ) {
		t3dInstanceOcclusionCell_t occlusion;
		if ( rState.m_pOcclusion->getCellRecord( rRegionLocation, rSourceCell.m_location, rMesh.m_nId, occlusion )
		        && occlusion.m_nSourceRevision == rSourceCell.m_nRevision
		        && occlusion.m_nProxyGeneration == rAsset->getInstanceOcclusionProxyGeneration()
		        && occlusion.m_nInstanceCount == rSourceCell.m_transforms.size() ) {
			rCell.m_nOcclusionSourceRevision = occlusion.m_nSourceRevision;
			rCell.m_nOcclusionProxyGeneration = occlusion.m_nProxyGeneration;
			rCell.m_nOcclusionProxyPolicy = occlusion.m_nProxyPolicy;
			// Occluder-category instances contribute dedicated proxy depth but are
			// never tested against that history, preventing contributor self-occlusion.
			rCell.m_occlusionBounds = occlusion.m_localProxyBounds;
		}
	}
#endif
}

static bool t3dInstancePublicationCommitCell( t3dInstancePublicationBuildState_t &rState,
        const t3dInstanceCell_t &rSourceCell,
        const Vector3 &rRegionOffset,
        const int nSourceLod,
        t3dInstancePublicationCategoryDiagnostics_t &rCategory,
        t3dRdInstanceCell_t &rCell ) {
	t3dRdInstanceSceneSnapshot_s &snapshot = *rState.m_pSnapshot;
	t3dInstancePublicationDiagnostics_t &diagnostics = *rState.m_pDiagnostics;
	const size_t source_count = rSourceCell.m_transforms.size();
	const size_t output_cell = snapshot.m_cells.size();
	t3dRdInstanceCell_t previous_cell;
	const bool has_previous = output_cell < rState.m_nPreviousCellCount;
	if ( has_previous ) {
		previous_cell = rState.m_pPreviousCells[output_cell];
	}
	const bool same_layout = has_previous && t3dInstancePublicationSameLayout( previous_cell, rCell );
	const bool reuse_records = same_layout && previous_cell.m_nRevision == rCell.m_nRevision;
	if ( reuse_records ) {
		if ( !snapshot.m_instances.resizeUninitialized( snapshot.m_instances.size() + source_count ) ) {
			return false;
		}
		diagnostics.m_nPackedRecordsReused += ( uint64_t )( source_count );
	} else {
		for ( size_t iInstance = 0u; iInstance < source_count; ++iInstance ) {
			Transform3D transform = rSourceCell.getTransform( iInstance );
			transform.origin += rRegionOffset;
			const uint32_t color = iInstance < rSourceCell.m_colors.size() ? rSourceCell.m_colors[iInstance] : s_nInstanceWhiteRgba8;
			if ( !snapshot.m_instances.pushBack( t3dInstancePublicationPackRecord( transform, color ) ) ) {
				UtilityFunctions::push_error( "Terrain3DInstancePublicationBuilder: record storage exceeded fixed capacity" );
				return false;
			}
		}
		diagnostics.m_nPackedRecordsRebuilt += ( uint64_t )( source_count );
		if ( same_layout ) {
			if ( !snapshot.m_dirtyRanges.pushBack( t3dRdInstanceDirtyRange_t{ rCell.m_nFirstInstance, rCell.m_nInstanceCount } ) ) {
				return false;
			}
			++diagnostics.m_nDirtyRecordRanges;
		}
	}

	const bool descriptor_changed = !has_previous || !t3dInstancePublicationSameDescriptor( previous_cell, rCell );
	if ( !same_layout ) {
		snapshot.m_bInstanceLayoutChanged = true;
		snapshot.m_bTopologyChanged = true;
	} else if ( previous_cell.m_nMeshSourceKey != rCell.m_nMeshSourceKey || previous_cell.m_nLod != rCell.m_nLod
	        || previous_cell.m_nSettingsRevision != rCell.m_nSettingsRevision || previous_cell.m_nFlags != rCell.m_nFlags ) {
		snapshot.m_bTopologyChanged = true;
	}
	if ( descriptor_changed ) {
		if ( !snapshot.m_dirtyCells.pushBack( ( uint32_t )( output_cell ) ) ) {
			return false;
		}
		if ( reuse_records ) {
			++diagnostics.m_nPolicyOnlyCellUpdates;
		}
	}
	if ( !snapshot.m_cells.pushBack( rCell ) ) {
		UtilityFunctions::push_error( "Terrain3DInstancePublicationBuilder: cell storage exceeded fixed capacity" );
		return false;
	}
	if ( nSourceLod <= 0 ) {
		++rCategory.m_nPublishedLod0Cells;
	} else if ( nSourceLod == 1 ) {
		++rCategory.m_nPublishedLod1Cells;
	} else {
		++rCategory.m_nPublishedHigherLodCells;
	}
	diagnostics.m_nPublishedRecords += ( uint64_t )( source_count );
	++diagnostics.m_nPublishedCells;
	return true;
}

static bool t3dInstancePublicationBuildMesh( t3dInstancePublicationBuildState_t &rState,
        t3dInstanceMesh_t &rMesh,
        const Vector2i &rRegionLocation,
        const Vector3 &rRegionOffset ) {
	const bool asset_id_valid = rMesh.m_nId >= 0 && rMesh.m_nId < s_nMaxMeshAssets;
	const Ref<Terrain3DMeshAsset> asset = asset_id_valid ? rState.m_assets->getMeshAsset( rMesh.m_nId ) : Ref<Terrain3DMeshAsset>();
	const bool asset_enabled = asset.is_valid() && asset->isEnabled();
	const bool asset_valid = asset_enabled && asset->getLodCount() > 0;
	const int category_index = asset_valid ? t3dInstancePublicationCategoryIndex( asset->getInstanceCategory() ) : 0;
	t3dInstancePublicationCategoryDiagnostics_t &category = rState.m_pDiagnostics->m_categories[category_index];
	t3dHlodInstancerRepresentationState_t hlod_state;
	if ( asset_valid && rState.m_pContext->m_pHlodStreamer && asset->getInstanceCategory() != INSTANCE_CATEGORY_CLUTTER ) {
		hlod_state = rState.m_pContext->m_pHlodStreamer->getInstancerRepresentationState( rRegionLocation, rMesh.m_nId );
	}
	bool hlod_source_counted = false;
	AABB conservative_source_aabb;
	const bool conservative_bounds_valid =
	        asset_valid && t3dInstancePublicationBuildConservativeSourceAabb( *asset.ptr(), conservative_source_aabb );

	for ( size_t iCell = 0u; iCell < rMesh.m_cells.size(); ++iCell ) {
		t3dInstanceCell_t &source_cell = rMesh.m_cells[iCell];
		int desired_lod = -1;
		int source_lod = -1;
		if ( !t3dInstancePublicationAcceptCell( rState,
		             rMesh,
		             source_cell,
		             asset,
		             asset_id_valid,
		             asset_enabled,
		             asset_valid,
		             hlod_state,
		             hlod_source_counted,
		             category,
		             rRegionLocation,
		             conservative_source_aabb,
		             conservative_bounds_valid,
		             desired_lod,
		             source_lod ) ) {
			continue;
		}
		t3dRdInstanceCell_t cell;
		t3dInstancePublicationPopulateCell(
		        rState, rMesh, source_cell, asset, rRegionLocation, rRegionOffset, desired_lod, source_lod, category, cell );
		if ( !t3dInstancePublicationCommitCell( rState, source_cell, rRegionOffset, source_lod, category, cell ) ) {
			return false;
		}
	}
	return true;
}

static bool t3dInstancePublicationBuildRegion( t3dInstancePublicationBuildState_t &rState, Terrain3DRegion *const pRegion ) {
	if ( !pRegion || pRegion->isDeleted() ) {
		return true;
	}
	Terrain3DInstanceStore *const pStore = pRegion->getResidentInstanceStoreInternalMut();
	if ( !pStore ) {
		return true;
	}
	const Vector2i region_location = pRegion->getLocation();
	if ( rState.m_pContext->m_bRespectRegionResidency
	        && rState.m_pContext->m_pTerrain->getRegionStreamingStateInternal( region_location ) < TERRAIN3D_REGION_STREAMING_HOT ) {
		for ( size_t iMesh = 0u; iMesh < pStore->meshes().size(); ++iMesh ) {
			for ( size_t iCell = 0u; iCell < pStore->meshes()[iMesh].m_cells.size(); ++iCell ) {
				if ( !pStore->meshes()[iMesh].m_cells[iCell].m_transforms.empty() ) {
					++rState.m_pDiagnostics->m_nRegionResidencyFilteredCells;
				}
			}
		}
		return true;
	}
	const real_t region_world_size = ( real_t )( s_nRegionWorldSizeMeters );
	const Vector3 region_offset(
	        ( real_t )( region_location.x ) * region_world_size, 0.0, ( real_t )( region_location.y ) * region_world_size );
	for ( size_t iMesh = 0u; iMesh < pStore->meshesMut().size(); ++iMesh ) {
		if ( !t3dInstancePublicationBuildMesh( rState, pStore->meshesMut()[iMesh], region_location, region_offset ) ) {
			return false;
		}
	}
	return true;
}

static void t3dInstancePublicationFinalizeSnapshot( t3dInstancePublicationBuildState_t &rState ) {
	t3dRdInstanceSceneSnapshot_s &snapshot = *rState.m_pSnapshot;
	if ( rState.m_nPreviousInstanceCount != snapshot.m_instances.size() || rState.m_nPreviousCellCount != snapshot.m_cells.size() ) {
		snapshot.m_bInstanceLayoutChanged = true;
		snapshot.m_bTopologyChanged = true;
	}
	if ( snapshot.m_bInstanceLayoutChanged ) {
		++rState.m_pDiagnostics->m_nInstanceLayoutChanges;
		snapshot.m_dirtyRanges.reset();
		snapshot.m_dirtyCells.reset();
	}
}

bool Terrain3DInstancePublicationBuilder::build( const t3dInstancePublicationBuildContext_t &rContext,
        t3dRdInstanceSceneSnapshot_s &rSnapshot,
        t3dInstancePublicationDiagnostics_t &rDiagnostics ) {
	rDiagnostics.reset();
	t3dInstancePublicationBuildState_t state;
	state.m_pContext = &rContext;
	state.m_pSnapshot = &rSnapshot;
	state.m_pDiagnostics = &rDiagnostics;
	state.m_nPreviousInstanceCount = rSnapshot.m_instances.size();
	state.m_nPreviousCellCount = rSnapshot.m_cells.size();
	state.m_pPreviousCells = rSnapshot.m_cells.data();

	rSnapshot.m_instances.reset();
	rSnapshot.m_cells.reset();
	rSnapshot.m_dirtyRanges.reset();
	rSnapshot.m_dirtyCells.reset();
	rSnapshot.m_bInstanceLayoutChanged = state.m_nPreviousInstanceCount == 0u && state.m_nPreviousCellCount == 0u;
	rSnapshot.m_bTopologyChanged = rSnapshot.m_bInstanceLayoutChanged;
	rSnapshot.m_nGeneration = rContext.m_nGeneration;
	if ( !rContext.m_pTerrain || !rContext.m_pTerrain->getData() || rContext.m_pTerrain->getAssets().is_null() ) {
		if ( state.m_nPreviousInstanceCount != 0u || state.m_nPreviousCellCount != 0u ) {
			rSnapshot.m_bInstanceLayoutChanged = true;
			rSnapshot.m_bTopologyChanged = true;
			++rDiagnostics.m_nInstanceLayoutChanges;
		}
		return true;
	}

	state.m_assets = rContext.m_pTerrain->getAssets();
	if ( !state.m_assets->isInitializedFor( rContext.m_pTerrain ) ) {
		++rDiagnostics.m_nAssetRegistryDeferredBuilds;
		if ( state.m_nPreviousInstanceCount != 0u || state.m_nPreviousCellCount != 0u ) {
			rSnapshot.m_bInstanceLayoutChanged = true;
			rSnapshot.m_bTopologyChanged = true;
			++rDiagnostics.m_nInstanceLayoutChanges;
		}
		return true;
	}
	state.m_pOcclusion = rContext.m_pTerrain->getInstanceOcclusion();
	Terrain3DData *const pData = rContext.m_pTerrain->getData();
	Terrain3DRegion *const *const ppRegions = pData->getRegionPtrMapInternal();
	for ( int iRegion = 0; iRegion < s_nRegionSlots; ++iRegion ) {
		if ( !t3dInstancePublicationBuildRegion( state, ppRegions[iRegion] ) ) {
			return false;
		}
	}
	t3dInstancePublicationFinalizeSnapshot( state );
	return true;
}
