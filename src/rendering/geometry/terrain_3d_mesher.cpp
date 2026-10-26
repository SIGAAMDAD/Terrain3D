// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/geometry/terrain_3d_mesher.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"

#include "core/utility/logger.h"
#include "eastl_config.h"
#include "rendering/geometry/terrain_3d_clipmap_bounds.h"
#include "terrain_3d.h"

#include <limits.h>
#include <math.h>
#include <string.h>


// -----------------------------------------------------------------------------
// Private functions

static bool t3dMesherPackStripCoordinate( const real_t fValue, int16_t &rPacked ) {
	const double value = ( double )( fValue );
	if ( !isfinite( value ) || value < ( double )( INT16_MIN ) || value > ( double )( INT16_MAX ) ) {
		return false;
	}
	const double rounded = round( value );
	if ( fabs( value - rounded ) > 0.0001 ) {
		return false;
	}
	rPacked = ( int16_t )( ( int64_t )( rounded ) );
	return true;
}

bool Terrain3DMesher::appendStripPart( const t3dMeshType_t type,
        const Vector2i &rSize,
        const Vector3 &rOrigin,
        const uint16_t nPartCode,
        const uint16_t nFlags ) {
	if ( type < LOD0_MESH || type >= MESH_TYPE_COUNT || rSize.x <= 0 || rSize.y <= 0 || rSize.x > ( int )( UINT16_MAX )
	        || rSize.y > ( int )( UINT16_MAX ) || nPartCode >= ( uint16_t )( T3D_CLIPMAP_PART_CODE_COUNT ) ) {
		return false;
	}
	const size_t type_index = ( size_t )( type );
	uint16_t &count = m_szStripPartCounts[type_index];
	if ( count >= ( uint16_t )( T3D_CLIPMAP_STRIP_PART_CAPACITY ) ) {
		return false;
	}
	const uint16_t layout_bits = nFlags & T3D_CLIPMAP_STRIP_LAYOUT_MASK;
	const uint16_t role_bits = nFlags & T3D_CLIPMAP_STRIP_ROLE_MASK;
	if ( layout_bits == 0u || ( layout_bits & ( layout_bits - 1u ) ) != 0u || role_bits == 0u || ( role_bits & ( role_bits - 1u ) ) != 0u
	        || ( nFlags & ( uint16_t )( ~T3D_CLIPMAP_STRIP_KNOWN_FLAG_MASK ) ) != 0u
	        || ( ( nFlags & T3D_CLIPMAP_STRIP_FLAG_SERPENTINE_ROWS ) != 0u && layout_bits != T3D_CLIPMAP_STRIP_FLAG_LAYOUT_OUTER_RING ) ) {
		return false;
	}
	t3dClipmapStripPart_t part;
	memset( &part, 0, sizeof( part ) );
	if ( !t3dMesherPackStripCoordinate( rOrigin.x, part.m_nOriginX ) || !t3dMesherPackStripCoordinate( rOrigin.z, part.m_nOriginZ ) ) {
		return false;
	}
	part.m_nCellWidth = ( uint16_t )( rSize.x );
	part.m_nCellHeight = ( uint16_t )( rSize.y );
	part.m_nPartCode = nPartCode;
	part.m_nFlags = nFlags;
	m_szStripParts[type_index][count++] = part;
	return true;
}

void Terrain3DMesher::clearStripParts( void ) {
	memset( m_szStripParts, 0, sizeof( m_szStripParts ) );
	memset( m_szStripPartCounts, 0, sizeof( m_szStripPartCounts ) );
}

bool Terrain3DMesher::generateStripParts( void ) {
	clearStripParts();
	const uint16_t lod0_layout = T3D_CLIPMAP_STRIP_FLAG_LAYOUT_LOD0;
	const uint16_t outer_layout = T3D_CLIPMAP_STRIP_FLAG_LAYOUT_OUTER_RING | T3D_CLIPMAP_STRIP_FLAG_SERPENTINE_ROWS;

	for ( size_t iTile = 0u; iTile < ( size_t )( T3D_MAX_TILE_INSTANCES ); ++iTile ) {
		if ( !appendStripPart( LOD0_MESH,
		             V2I( m_nMeshSize ),
		             m_szTilePosLod0[iTile],
		             T3D_CLIPMAP_PART_STATIC,
		             lod0_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_TILE ) ) {
			return false;
		}
	}
	for ( size_t iPair = 0u; iPair < ( size_t )( T3D_PAIR_INSTANCES ); ++iPair ) {
		if ( !appendStripPart( LOD0_MESH,
		             Vector2i( 2, m_nMeshSize * 4 + 2 ),
		             m_szTrimAPos[iPair],
		             T3D_CLIPMAP_PART_STATIC,
		             lod0_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_TRIM_A )
		        || !appendStripPart( LOD0_MESH,
		                Vector2i( m_nMeshSize * 4 + 2, 2 ),
		                m_szTrimBPos[iPair],
		                T3D_CLIPMAP_PART_STATIC,
		                lod0_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_TRIM_B ) ) {
			return false;
		}
	}

	for ( size_t iTile = 0u; iTile < ( size_t )( T3D_RING_TILE_INSTANCES ); ++iTile ) {
		if ( !appendStripPart( OUTER_RING_MESH,
		             V2I( m_nMeshSize ),
		             m_szTilePos[iTile],
		             T3D_CLIPMAP_PART_STATIC,
		             outer_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_TILE ) ) {
			return false;
		}
	}
	for ( size_t iPair = 0u; iPair < ( size_t )( T3D_PAIR_INSTANCES ); ++iPair ) {
		if ( !appendStripPart( OUTER_RING_MESH,
		             Vector2i( 4, m_nMeshSize ),
		             m_szFillAPos[iPair],
		             T3D_CLIPMAP_PART_STATIC,
		             outer_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_FILL_A )
		        || !appendStripPart( OUTER_RING_MESH,
		                Vector2i( m_nMeshSize, 4 ),
		                m_szFillBPos[iPair],
		                T3D_CLIPMAP_PART_STATIC,
		                outer_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_FILL_B ) ) {
			return false;
		}
	}

	for ( int i = 0; i < T3D_PAIR_INSTANCES; ++i ) {
		Vector3 edge_a;
		edge_a.z = -m_fOffsetA - 2.0f;
		edge_a.x = m_szEdgePos[( size_t )i][1];
		Vector3 edge_b;
		edge_b.z = m_szEdgePos[( size_t )i][1];
		edge_b.x = -m_fOffsetA;
		const uint16_t edge_x_code = i == 0 ? T3D_CLIPMAP_PART_EDGE_X0 : T3D_CLIPMAP_PART_EDGE_X1;
		const uint16_t edge_z_code = i == 0 ? T3D_CLIPMAP_PART_EDGE_Z0 : T3D_CLIPMAP_PART_EDGE_Z1;
		if ( !appendStripPart( LOD0_MESH,
		             Vector2i( 2, m_nMeshSize * 4 + 8 ),
		             edge_a,
		             edge_x_code,
		             lod0_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_X )
		        || !appendStripPart( LOD0_MESH,
		                Vector2i( m_nMeshSize * 4 + 4, 2 ),
		                edge_b,
		                edge_z_code,
		                lod0_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_Z )
		        || !appendStripPart( OUTER_RING_MESH,
		                Vector2i( 2, m_nMeshSize * 4 + 8 ),
		                edge_a,
		                edge_x_code,
		                outer_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_X )
		        || !appendStripPart( OUTER_RING_MESH,
		                Vector2i( m_nMeshSize * 4 + 4, 2 ),
		                edge_b,
		                edge_z_code,
		                outer_layout | T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_Z ) ) {
			return false;
		}
	}

	return m_szStripPartCounts[LOD0_MESH] == ( uint16_t )( T3D_CLIPMAP_STRIP_LOD0_PART_COUNT )
	        && m_szStripPartCounts[OUTER_RING_MESH] == ( uint16_t )( T3D_CLIPMAP_STRIP_OUTER_RING_PART_COUNT );
}

void Terrain3DMesher::generateClipmap( void ) {
	clearClipmap();
	generateOffsetData();
	if ( !generateStripParts() ) {
		ERR_PRINT( "Terrain3D failed to build canonical procedural clipmap strip descriptors." );
		clearStripParts();
		return;
	}
	m_nClipmapLevelCount = CLAMP( m_nLods + m_nTessellationLevel, 0, T3D_MAX_CLIPMAP_LEVELS );
}
void Terrain3DMesher::generateOffsetData( void ) {
	LOG( INFO, "Computing fixed clipmap positioning offsets" );
	const Vector3 szTilePosLod0[T3D_MAX_TILE_INSTANCES] = {
		Vector3( 0, 0, m_nMeshSize ),
		Vector3( m_nMeshSize, 0, m_nMeshSize ),
		Vector3( m_nMeshSize, 0, 0 ),
		Vector3( m_nMeshSize, 0, -m_nMeshSize ),
		Vector3( m_nMeshSize, 0, -m_nMeshSize * 2 ),
		Vector3( 0, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize ),
		Vector3( -m_nMeshSize * 2, 0, 0 ),
		Vector3( -m_nMeshSize * 2, 0, m_nMeshSize ),
		Vector3( -m_nMeshSize, 0, m_nMeshSize ),
		s_v3Zero,
		Vector3( -m_nMeshSize, 0, 0 ),
		Vector3( 0, 0, -m_nMeshSize ),
		Vector3( -m_nMeshSize, 0, -m_nMeshSize ),
	};
	memcpy( m_szTilePosLod0, szTilePosLod0, sizeof( m_szTilePosLod0 ) );
	const Vector3 szTrimAPos[T3D_PAIR_INSTANCES] = { Vector3( m_nMeshSize * 2, 0, -m_nMeshSize * 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize * 2 - 2 ) };
	memcpy( m_szTrimAPos, szTrimAPos, sizeof( m_szTrimAPos ) );
	const Vector3 szTrimBPos[T3D_PAIR_INSTANCES] = { Vector3( -m_nMeshSize * 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, m_nMeshSize * 2 ) };
	memcpy( m_szTrimBPos, szTrimBPos, sizeof( m_szTrimBPos ) );
	const Vector3 szTilePos[T3D_RING_TILE_INSTANCES] = {
		Vector3( 2, 0, m_nMeshSize + 2 ),
		Vector3( m_nMeshSize + 2, 0, m_nMeshSize + 2 ),
		Vector3( m_nMeshSize + 2, 0, -2 ),
		Vector3( m_nMeshSize + 2, 0, -m_nMeshSize - 2 ),
		Vector3( m_nMeshSize + 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize + 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, m_nMeshSize + 2 ),
		Vector3( -m_nMeshSize + 2, 0, m_nMeshSize + 2 ),
	};
	memcpy( m_szTilePos, szTilePos, sizeof( m_szTilePos ) );
	m_fOffsetA = ( real_t )( m_nMeshSize * 2 ) + 2.f;
	m_fOffsetB = ( real_t )( m_nMeshSize * 2 ) + 4.f;
	m_fOffsetC = ( real_t )( m_nMeshSize * 2 ) + 6.f;
	const Vector3 szEdgePos[T3D_PAIR_INSTANCES] = { Vector3( m_fOffsetA, m_fOffsetA, -m_fOffsetB ),
		Vector3( m_fOffsetB, -m_fOffsetB, -m_fOffsetC ) };
	memcpy( m_szEdgePos, szEdgePos, sizeof( m_szEdgePos ) );
	const Vector3 szFillAPos[T3D_PAIR_INSTANCES] = { Vector3( m_nMeshSize - 2, 0, -m_nMeshSize * 2 - 2 ),
		Vector3( -m_nMeshSize - 2, 0, m_nMeshSize + 2 ) };
	memcpy( m_szFillAPos, szFillAPos, sizeof( m_szFillAPos ) );
	const Vector3 szFillBPos[T3D_PAIR_INSTANCES] = { Vector3( m_nMeshSize + 2, 0, m_nMeshSize - 2 ),
		Vector3( -m_nMeshSize * 2 - 2, 0, -m_nMeshSize - 2 ) };
	memcpy( m_szFillBPos, szFillBPos, sizeof( m_szFillBPos ) );
}

void Terrain3DMesher::generateLodCache( void ) {
	m_nClipmapLevelCount = CLAMP( m_nLods + m_nTessellationLevel, 0, T3D_MAX_CLIPMAP_LEVELS );
	refreshImmediateLodCount();
	for ( int lod = 0; lod < T3D_MAX_CLIPMAP_LEVELS; ++lod ) {
		m_szLastLodPositions[( size_t )( lod )] = s_v2Max;
		m_szLastLodTestX[( size_t )( lod )] = -1;
		m_szLastLodTestZ[( size_t )( lod )] = -1;
	}
	const real_t fDensity = ( real_t )( 1.0 ) / ( real_t )( ( uint64_t )( 1 ) << CLAMP( m_nTessellationLevel, 0, 30 ) );
	const real_t fBaseSpacing = m_fVertexSpacing * fDensity;
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) {
		const real_t fMultiplier = ( real_t )( ( uint64_t )( 1 ) << CLAMP( lod, 0, 30 ) );
		m_fLodScales[( size_t )( lod )] = fBaseSpacing * fMultiplier;
		m_fLodSnapSteps[( size_t )( lod )] = m_fLodScales[( size_t )( lod )] * 2.0f;
		m_fLodNextSnapSteps[( size_t )( lod )] = m_fLodSnapSteps[( size_t )( lod )] * 2.0f;
	}
}


void Terrain3DMesher::clearClipmap( void ) {
	m_nClipmapLevelCount = 0;
	refreshImmediateLodCount();
}

int Terrain3DMesher::getImmediateLodCount( void ) const {
	return m_runtimeConfig.m_nImmediateLodCount;
}

void Terrain3DMesher::refreshImmediateLodCount( void ) {
	const int continuity_ring = m_runtimeConfig.m_nMiddleShaderLodCount > 0 ? 1 : 0;
	m_runtimeConfig.m_nImmediateLodCount = CLAMP( m_runtimeConfig.m_nNearShaderLodCount + continuity_ring, 0, m_nClipmapLevelCount );
}

void Terrain3DMesher::compileTierRuntimeConfig(
        const int nNearShaderLodCount, const int nMiddleShaderLodCount, const int nClipmapUpdateBudget ) {
	const int total_levels = MAX( 0, m_nLods + m_nTessellationLevel );
	m_runtimeConfig.m_nNearShaderLodCount = CLAMP( nNearShaderLodCount, 0, total_levels );
	m_runtimeConfig.m_nMiddleShaderLodCount = CLAMP( nMiddleShaderLodCount, 0, total_levels - m_runtimeConfig.m_nNearShaderLodCount );
	m_runtimeConfig.m_nClipmapUpdateBudget = CLAMP( nClipmapUpdateBudget, 1, T3D_MAX_CLIPMAP_LEVELS );
	refreshImmediateLodCount();
}

void Terrain3DMesher::compileShadowRuntimeConfig(
        const int nShadowLodCount, const eastl::array<bool, 3> &bTierShadowEnabled, const eastl::array<int, 3> &nTierShadowLodCounts ) {
	m_runtimeConfig.m_nShadowLodCount = CLAMP( nShadowLodCount, 0, T3D_MAX_CLIPMAP_LEVELS );
	for ( int tier = 0; tier < 3; ++tier ) {
		m_runtimeConfig.m_szTierShadowEnabled[tier] = bTierShadowEnabled[( size_t )( tier )];
		m_runtimeConfig.m_szTierShadowLodCounts[tier] = CLAMP( nTierShadowLodCounts[( size_t )( tier )], -1, T3D_MAX_CLIPMAP_LEVELS );
	}
}

int Terrain3DMesher::getLodTier( const int nLod ) const {
	if ( nLod < 0 || nLod >= m_nClipmapLevelCount ) {
		return 2;
	}
	if ( nLod < m_runtimeConfig.m_nNearShaderLodCount ) {
		return 0;
	}
	if ( nLod < m_runtimeConfig.m_nNearShaderLodCount + m_runtimeConfig.m_nMiddleShaderLodCount ) {
		return 1;
	}
	return 2;
}

bool Terrain3DMesher::lodCastsShadows( const int nLod ) const {
	if ( m_castShadows == GeometryInstance3D::SHADOW_CASTING_SETTING_OFF || nLod < 0 || nLod >= m_nClipmapLevelCount ) {
		return false;
	}
	const int tier = getLodTier( nLod );
	if ( tier < 0 || tier >= 3 || !m_runtimeConfig.m_szTierShadowEnabled[tier] ) {
		return false;
	}
	int lod_limit = m_runtimeConfig.m_nShadowLodCount;
	if ( m_runtimeConfig.m_szTierShadowLodCounts[tier] >= 0 ) {
		lod_limit = m_runtimeConfig.m_szTierShadowLodCounts[tier];
	}
	return lod_limit > 0 && nLod < lod_limit;
}

int Terrain3DMesher::getRenderBatchCountForTier( const int nTier ) const {
	if ( nTier < 0 || nTier >= 3 ) {
		return 0;
	}
	int count = 0;
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) {
		count += getLodTier( lod ) == nTier ? 1 : 0;
	}
	return count;
}

int Terrain3DMesher::getRenderBatchCount( void ) const {
	return m_nClipmapLevelCount;
}

int Terrain3DMesher::getShadowBatchCountForTier( const int nTier ) const {
	if ( nTier < 0 || nTier >= 3 ) {
		return 0;
	}
	int count = 0;
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) {
		count += getLodTier( lod ) == nTier && lodCastsShadows( lod ) ? 1 : 0;
	}
	return count;
}

int Terrain3DMesher::getShadowBatchCount( void ) const {
	int count = 0;
	for ( int lod = 0; lod < m_nClipmapLevelCount; ++lod ) {
		count += lodCastsShadows( lod ) ? 1 : 0;
	}
	return count;
}

size_t Terrain3DMesher::getStreamingBufferBytes( void ) const {
	// Procedural clipmap topology owns no transient vertex/index streaming
	// payload. Live RD patch/visibility buffers are accounted by the renderer.
	return 0u;
}


void Terrain3DMesher::initialize( Terrain3DOwner *const pTerrain, const t3dMesherInitialization_t &rConfig ) {
	if ( !pTerrain ) {
		return;
	}
	m_pTerrain = pTerrain;
	m_nLods = rConfig.m_nLods;
	m_nTessellationLevel = rConfig.m_nTessellationLevel;
	m_nMeshSize = rConfig.m_nMeshSize;
	m_fVertexSpacing = rConfig.m_fVertexSpacing;
	m_castShadows = rConfig.m_castShadows;
	compileTierRuntimeConfig( rConfig.m_nNearShaderLodCount, rConfig.m_nMiddleShaderLodCount, rConfig.m_nClipmapUpdateBudget );
	compileShadowRuntimeConfig( rConfig.m_nShadowLodCount, rConfig.m_bTierShadowEnabled, rConfig.m_nTierShadowLodCounts );
	generateClipmap();
	generateLodCache();
	m_iDeferredLodCursor = getImmediateLodCount();
	updateAabbs();
	resetTargetPosition();
	// The caller selected the authoritative startup target before any residency
	// or render publication. Keep mesh initialization on that same target instead
	// of querying mutable camera state a second time.
	snap( rConfig.m_initialTarget );
}

void Terrain3DMesher::destroy( void ) {
	clearClipmap();
	clearStripParts();
	m_pTerrain = NULL;
	for ( int lod = 0; lod < T3D_MAX_CLIPMAP_LEVELS; ++lod ) {
		m_szLastLodPositions[( size_t )( lod )] = s_v2Max;
		m_szLastLodTestX[( size_t )( lod )] = -1;
		m_szLastLodTestZ[( size_t )( lod )] = -1;
	}
	memset( m_fLodScales, 0, sizeof( m_fLodScales ) );
	memset( m_fLodSnapSteps, 0, sizeof( m_fLodSnapSteps ) );
	memset( m_fLodNextSnapSteps, 0, sizeof( m_fLodNextSnapSteps ) );
}

void Terrain3DMesher::setTierLayout(
        const int nNearShaderLodCount, const int nMiddleShaderLodCount, const int nClipmapUpdateBudget, const bool bForceFullSnap ) {
	const int total_levels = MAX( 0, m_nLods + m_nTessellationLevel );
	const int near_count = CLAMP( nNearShaderLodCount, 0, total_levels );
	const int middle_count = CLAMP( nMiddleShaderLodCount, 0, total_levels - near_count );
	const int update_budget = CLAMP( nClipmapUpdateBudget, 1, T3D_MAX_CLIPMAP_LEVELS );
	const int old_immediate_count = getImmediateLodCount();
	const bool layout_changed =
	        m_runtimeConfig.m_nNearShaderLodCount != near_count || m_runtimeConfig.m_nMiddleShaderLodCount != middle_count;
	const bool budget_changed = m_runtimeConfig.m_nClipmapUpdateBudget != update_budget;
	if ( !layout_changed && !budget_changed ) {
		return;
	}
	compileTierRuntimeConfig( near_count, middle_count, update_budget );
	m_iDeferredLodCursor = getImmediateLodCount();
	m_bForceFullSnap = bForceFullSnap || old_immediate_count != m_iDeferredLodCursor;
	if ( layout_changed ) {
		resetTargetPosition();
		snap();
	}
}

void Terrain3DMesher::setCastShadows( const GeometryInstance3D::ShadowCastingSetting castShadows ) {
	m_castShadows = castShadows;
}
void Terrain3DMesher::setShadowLodCount( const int nCount ) {
	m_runtimeConfig.m_nShadowLodCount = CLAMP( nCount, 0, T3D_MAX_CLIPMAP_LEVELS );
}
void Terrain3DMesher::setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled ) {
	for ( int tier = 0; tier < 3; ++tier ) {
		m_runtimeConfig.m_szTierShadowEnabled[tier] = bEnabled[( size_t )( tier )];
	}
}
void Terrain3DMesher::setTierShadowLodCounts( const eastl::array<int, 3> &nCounts ) {
	for ( int tier = 0; tier < 3; ++tier ) {
		m_runtimeConfig.m_szTierShadowLodCounts[tier] = CLAMP( nCounts[( size_t )( tier )], -1, T3D_MAX_CLIPMAP_LEVELS );
	}
}

void Terrain3DMesher::resetTargetPosition( void ) {
	for ( int lod = 0; lod < T3D_MAX_CLIPMAP_LEVELS; ++lod ) {
		m_szLastLodPositions[( size_t )( lod )] = s_v2Max;
		m_szLastLodTestX[( size_t )( lod )] = -1;
		m_szLastLodTestZ[( size_t )( lod )] = -1;
	}
	m_bForceFullSnap = true;
	m_iDeferredLodCursor = getImmediateLodCount();
}

void Terrain3DMesher::snap( void ) {
	if ( m_pTerrain ) {
		snap( m_pTerrain->getClipmapTargetPosition() );
	}
}

bool Terrain3DMesher::snapLevel( const int nLod, const Vector3 &rSnappedTarget ) {
	const real_t fSnapStep = m_fLodSnapSteps[( size_t )( nLod )];
	const real_t fNextSnapStep = m_fLodNextSnapSteps[( size_t )( nLod )];
	Vector3 pos = s_v3Zero;
	pos.x = Math::round( rSnappedTarget.x / fSnapStep ) * fSnapStep;
	pos.z = Math::round( rSnappedTarget.z / fSnapStep ) * fSnapStep;
	const real_t fNextX = Math::round( rSnappedTarget.x / fNextSnapStep ) * fNextSnapStep;
	const real_t fNextZ = Math::round( rSnappedTarget.z / fNextSnapStep ) * fNextSnapStep;
	const int test_x = CLAMP( ( int )( Math::round( ( pos.x - fNextX ) / fSnapStep ) ) + 1, 0, 2 );
	const int test_z = CLAMP( ( int )( Math::round( ( pos.z - fNextZ ) / fSnapStep ) ) + 1, 0, 2 );
	const Vector2 snapped_lod_pos( pos.x, pos.z );
	if ( m_szLastLodPositions[( size_t )( nLod )] == snapped_lod_pos && m_szLastLodTestX[( size_t )( nLod )] == test_x
	        && m_szLastLodTestZ[( size_t )( nLod )] == test_z ) {
		return false;
	}
	m_szLastLodPositions[( size_t )( nLod )] = snapped_lod_pos;
	m_szLastLodTestX[( size_t )( nLod )] = test_x;
	m_szLastLodTestZ[( size_t )( nLod )] = test_z;
	const int tier = getLodTier( nLod );
	if ( tier == 0 ) {
		++m_nNearUpdatesLastSnap;
	} else if ( tier == 1 ) {
		++m_nMiddleUpdatesLastSnap;
	} else {
		++m_nFarUpdatesLastSnap;
	}
	return true;
}

void Terrain3DMesher::snap( const Vector3 &rTargetPos ) {
	if ( m_nClipmapLevelCount <= 0 ) {
		return;
	}
	m_nNearUpdatesLastSnap = m_nMiddleUpdatesLastSnap = m_nFarUpdatesLastSnap = 0u;
	m_nTeleportsLastSnap = 0u;
	m_nBufferUploadsLastSnap = 0u;
	const real_t base_spacing = m_fLodScales[0] > 0.0f ? m_fLodScales[0] : m_fVertexSpacing;
	const Vector3 snapped_target( Math::floor( rTargetPos.x / base_spacing ) * base_spacing,
	        rTargetPos.y,
	        Math::floor( rTargetPos.z / base_spacing ) * base_spacing );
	const int immediate_count = getImmediateLodCount();
	for ( int lod = 0; lod < immediate_count; ++lod ) {
		snapLevel( lod, snapped_target );
	}
	if ( m_bForceFullSnap ) {
		for ( int lod = immediate_count; lod < m_nClipmapLevelCount; ++lod ) {
			snapLevel( lod, snapped_target );
		}
		m_bForceFullSnap = false;
		m_iDeferredLodCursor = immediate_count;
		return;
	}
	const int deferred_count = m_nClipmapLevelCount - immediate_count;
	if ( deferred_count <= 0 ) {
		return;
	}
	int updated = 0;
	int scanned = 0;
	int lod = CLAMP( m_iDeferredLodCursor, immediate_count, m_nClipmapLevelCount - 1 );
	while ( scanned < deferred_count && updated < m_runtimeConfig.m_nClipmapUpdateBudget ) {
		if ( snapLevel( lod, snapped_target ) ) {
			++updated;
		}
		++scanned;
		if ( ++lod >= m_nClipmapLevelCount ) {
			lod = immediate_count;
		}
	}
	m_iDeferredLodCursor = lod;
}

int Terrain3DMesher::fillRdPatches( t3dRdPatch_t *const pPatches, const int nCapacity ) const {
	if ( !pPatches || nCapacity <= 0 ) {
		return 0;
	}
	const int count = MIN( MIN( m_nClipmapLevelCount, nCapacity ), T3D_MAX_CLIPMAP_LEVELS );
	int written = 0;
	for ( int lod = 0; lod < count; ++lod ) {
		const Vector2 &position = m_szLastLodPositions[( size_t )( lod )];
		if ( position == s_v2Max || m_szLastLodTestX[( size_t )( lod )] < 0 || m_szLastLodTestZ[( size_t )( lod )] < 0 ) {
			continue;
		}
		t3dRdPatch_t &patch = pPatches[written++];
		patch.m_fOriginX = ( float )( position.x );
		patch.m_fOriginZ = ( float )( position.y );
		patch.m_fScale = ( float )( m_fLodScales[( size_t )( lod )] );
		patch.m_nLod = ( uint32_t )( lod );
		patch.m_nTier = ( uint32_t )( getLodTier( lod ) );
		patch.m_nStitchX = ( uint32_t )( m_szLastLodTestX[( size_t )( lod )] );
		patch.m_nStitchZ = ( uint32_t )( m_szLastLodTestZ[( size_t )( lod )] );
		patch.m_nFlags = lodCastsShadows( lod ) ? 1u : 0u;
		patch.m_fBoundsMinY = ( float )( m_fClipmapAabbMinY );
		patch.m_fBoundsMaxY = ( float )( m_fClipmapAabbMinY + m_fClipmapAabbSizeY );
	}
	return written;
}

void Terrain3DMesher::updateAabbs( const real_t fCullMargin, const Vector2 &rHeightRange ) {
	if ( !m_pTerrain || !m_pTerrain->dataObject() ) {
		return;
	}
	const real_t cull_margin = fCullMargin < 0.0f ? m_pTerrain->getCullMargin() : fCullMargin;
	const Vector2 height_range = rHeightRange.x == FLT_MAX ? m_pTerrain->dataObject()->getHeightRange() : rHeightRange;
	const t3dClipmapVerticalBounds_t vertical_bounds =
	        t3dClipmapVerticalBounds( ( double )( height_range.x ), ( double )( height_range.y ), ( double )( cull_margin ) );
	m_fClipmapAabbMinY = ( real_t )( vertical_bounds.m_fMinY );
	m_fClipmapAabbSizeY = ( real_t )( vertical_bounds.m_fSizeY );
}
