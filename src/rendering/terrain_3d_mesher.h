// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MESHER_CLASS_H
#define TERRAIN3D_MESHER_CLASS_H

#include "Terrain3DConstants.h"

#include "eastl_config.h"
#include <EASTL/array.h>
#include <cstdint>

class T3DTerrain;


class T3DMesher {
	CLASS_NAME_STATIC( "T3DMesher" );

public: // Constants
	typedef enum t3dmeshType_e {
		LOD0_MESH,
		OUTER_RING_MESH,
		MESH_TYPE_COUNT,
	} t3dmeshType_t;
private:
	typedef struct t3dclipmapGeometry_s {
		PackedVector3Array vertices;
		PackedVector2Array metadata;
		PackedInt32Array indices;
		AABB aabb;
	} t3dclipmapGeometry_t;

	typedef struct t3dclipmapBatch_s {
		RID mesh;
		RID multimesh;
		RID instance;
		PackedFloat32Array transformBuffer;
		Vector2 anchorPosition = V2_MAX;
		uint8_t nCount = 0;
		uint8_t firstLod = 0;
		uint8_t lastLod = 0;
		uint8_t tier = 0;
		bool bCastsShadows = false;
		bool bOwnsMesh = false;
	} t3dclipmapBatch_t;

	typedef struct t3dlodBatchSlot_s {
		int8_t batch = -1;
		int8_t instance = -1;
	} t3dlodBatchSlot_t;

	T3DTerrain *m_pTerrain = nullptr;
	RID m_scenario = RID();
	eastl::array<Vector2, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_szLastLodPositions{};
	eastl::array<int, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_szLastLodTestX{};
	eastl::array<int, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_szLastLodTestZ{};
	eastl::array<real_t, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_fLodScales{};
	eastl::array<real_t, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_fLodSnapSteps{};
	eastl::array<real_t, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_fLodNextSnapSteps{};
	Vector3 m_lastShaderTargetPosition = V3_MAX;

	eastl::array<RID, MESH_TYPE_COUNT> m_szMeshRids{};
	eastl::array<t3dclipmapGeometry_t, MESH_TYPE_COUNT> m_szMeshGeometry{};
	PackedVector4Array m_lodStateBuffer;
	eastl::array<t3dclipmapBatch_t, TERRAIN3D_MAX_RENDER_BATCHES> m_szRenderBatches{};
	eastl::array<t3dlodBatchSlot_t, TERRAIN3D_MAX_CLIPMAP_LEVELS> m_szLodBatchSlots{};
	eastl::array<bool, TERRAIN3D_MAX_RENDER_BATCHES> m_bBatchDirty{};
	t3dclipmapBatch_t m_shadowStateBatch{};
	bool m_bShadowStateBatchDirty = false;
	bool m_bSeparateShadowStateBatch = false;
	int m_nRenderBatchCount = 0;
	int m_nClipmapLevelCount = 0;
	bool m_bStateMeshBatching = false;

	eastl::array<Vector3, TERRAIN3D_PAIR_INSTANCES> m_szTrimAPos{};
	eastl::array<Vector3, TERRAIN3D_PAIR_INSTANCES> m_szTrimBPos{};
	eastl::array<Vector3, TERRAIN3D_MAX_TILE_INSTANCES> m_szTilePosLod0{};
	eastl::array<Vector3, TERRAIN3D_PAIR_INSTANCES> m_szFillAPos{};
	eastl::array<Vector3, TERRAIN3D_PAIR_INSTANCES> m_szFillBPos{};
	eastl::array<Vector3, TERRAIN3D_RING_TILE_INSTANCES> m_szTilePos{};
	real_t m_fOffsetA = 0.f;
	real_t m_fOffsetB = 0.f;
	real_t m_fOffsetC = 0.f;
	eastl::array<Vector3, TERRAIN3D_PAIR_INSTANCES> m_szEdgePos{};

	eastl::array<RID, 3> m_szMaterials{};
	int m_nNearShaderLodCount = TERRAIN3D_MAX_CLIPMAP_LEVELS;
	int m_nMiddleShaderLodCount = 0;
	int m_nClipmapUpdateBudget = TERRAIN3D_MAX_CLIPMAP_LEVELS;
	int m_iDeferredLodCursor = 0;
	bool m_bForceFullSnap = true;
	uint32_t m_nNearUpdatesLastSnap = 0u;
	uint32_t m_nMiddleUpdatesLastSnap = 0u;
	uint32_t m_nFarUpdatesLastSnap = 0u;
	uint32_t m_nTeleportsLastSnap = 0u;
	uint32_t m_nBufferUploadsLastSnap = 0u;
	int m_tessellationLevel = 0;
	int m_nLods = 0;
	int m_nMeshSize = 0;
	real_t m_fVertexSpacing = 1.f;
	real_t m_fClipmapAabbMinY = -0.1f;
	real_t m_fClipmapAabbSizeY = 0.2f;
	uint32_t m_renderLayers = 1u;
	RenderingServer::ShadowCastingSetting m_castShadows = RenderingServer::SHADOW_CASTING_SETTING_OFF;
	GeometryInstance3D::GIMode m_giMode = GeometryInstance3D::GI_MODE_DISABLED;
	int m_nShadowLodCount = 0;
	eastl::array<bool, 3> m_bTierShadowEnabled{ true, true, true };

	void generateMeshTypes( void );
	t3dclipmapGeometry_t generateCombinedGeometry( bool bLod0 );
	void appendGrid(
		PackedVector3Array &rVertices, PackedVector2Array &rMetadata,
		PackedInt32Array &rIndices, const Vector2i &vSize, const Vector3 &rOrigin,
		float fPartId, bool bStandardGrid
	);
	RID instantiateMesh(
		const PackedVector3Array &rVertices, const PackedVector2Array &rMetadata,
		const PackedInt32Array &rIndices, const AABB &rAabb, bool bIncludeBasis = true
	);
	RID instantiateStateMesh( int nFirstLod, int nLastLod );
	t3dclipmapBatch_t createBatch(
		const RID &rMesh, int nCount, int nFirstLod, int nLastLod,
		int nTier, bool bCastsShadows
	);
	t3dclipmapBatch_t createStateBatch( int nFirstLod, int nLastLod, int nTier, bool bCastsShadows );
	void freeBatch( t3dclipmapBatch_t &rBatch );
	void setBatchInstance(
		t3dclipmapBatch_t &rBatch, int iIndex, const Transform3D &rTransform,
		int iTestX, int iTestZ
	);
	void flushDirtyBatches( void );
	void refreshStateBatch( int iBatchIndex, eastl::array<bool, 3> &bDirtyTiers );
	void refreshShadowStateBatch( void );
	void updateBatchCustomAabb( int iBatchIndex );
	void configureStateMaterials( void );
	void rebuildRenderBatches( void );
	void updateInstanceRid(
		const RID &rRid, const RID &rMaterial,
		RenderingServer::ShadowCastingSetting castShadows, bool bVisible, bool bBakedLight,
		bool bDynamicGi
	);
	bool snapLevel( int nLod, const Vector3 &rSnappedTarget );
	void generateClipmap( void );
	void generateOffsetData( void );
	void generateLodCache( void );

	void clearClipmap( void );
	void clearMeshTypes( void );
	int getImmediateLodCount( void ) const;
	int getLodTier( int nLod ) const;
	bool lodCastsShadows( int nLod ) const;
	size_t estimateGeometryPayloadBytes( int nFirstLod, int nLastLod, bool bIncludeBasis ) const;

public:
	T3DMesher( void ) {}
	~T3DMesher( void ) {
		destroy();
	}

	void initialize(
		T3DTerrain *pTerrain, const int nMeshSize, const int nLods, const int nTessellationLevel,
		const real_t fVertexSpacing, const eastl::array<RID, 3> &rMaterials,
		const int nNearShaderLodCount, const int nMiddleShaderLodCount,
		const int nClipmapUpdateBudget, const uint32_t nRenderLayers,
		const RenderingServer::ShadowCastingSetting castShadows,
		const GeometryInstance3D::GIMode giMode, const int nShadowLodCount,
		const eastl::array<bool, 3> &bTierShadowEnabled, const bool bStateMeshBatching = false
	);
	void destroy( void );

	void snap( void );
	void snap( const Vector3 &rTargetPosition );
	void resetTargetPosition( void );
	void update( void );
	void updateAabbs( const real_t fCullMargin = -1.f, const Vector2 &rHeightRange = V2_MAX );

	void setTierLayout(
		int nNearShaderLodCount, int nMiddleShaderLodCount,
		int nClipmapUpdateBudget, bool bForceFullSnap = true
	);

	void setMaterials( const eastl::array<RID, 3> &rMaterials ) {
		m_szMaterials = rMaterials;
		m_lastShaderTargetPosition = V3_MAX;
		configureStateMaterials();
		update();
	}
	RID getMaterial( void ) const {
		return m_szMaterials[0];
	}
	uint32_t getNearUpdatesLastSnap( void ) const { return m_nNearUpdatesLastSnap; }
	uint32_t getMiddleUpdatesLastSnap( void ) const { return m_nMiddleUpdatesLastSnap; }
	uint32_t getFarUpdatesLastSnap( void ) const { return m_nFarUpdatesLastSnap; }
	uint32_t getTeleportsLastSnap( void ) const { return m_nTeleportsLastSnap; }
	uint32_t getBufferUploadsLastSnap( void ) const { return m_nBufferUploadsLastSnap; }
	int getRenderBatchCount( void ) const { return m_nRenderBatchCount; }
	int getRenderBatchCountForTier( const int nTier ) const {
		int count = 0;
		for ( int index = 0; index < m_nRenderBatchCount; ++index ) {
			count += ( int ) ( m_szRenderBatches[( size_t ) ( index )].tier ) == nTier ? 1 : 0;
		}
		return count;
	}
	int getShadowBatchCountForTier( const int nTier ) const {
		int count = 0;
		for ( int index = 0; index < m_nRenderBatchCount; ++index ) {
			const t3dclipmapBatch_t &batch = m_szRenderBatches[( size_t ) ( index )];
			count += ( int ) ( batch.tier ) == nTier && batch.bCastsShadows ? 1 : 0;
		}
		if ( m_bSeparateShadowStateBatch && ( int ) ( m_shadowStateBatch.tier ) == nTier ) {
			++count;
		}
		return count;
	}
	int getShadowBatchCount( void ) const { return m_bSeparateShadowStateBatch ? 1 : 0; }
	int getBaseMeshCount( void ) const {
		return m_bStateMeshBatching ? m_nRenderBatchCount + ( ( m_bSeparateShadowStateBatch && m_shadowStateBatch.bOwnsMesh ) ? 1 : 0 ) : MESH_TYPE_COUNT;
	}
	size_t getVisibleGeometryPayloadBytes( void ) const;
	size_t getShadowGeometryPayloadBytes( void ) const;
	size_t getStreamingBufferBytes( void ) const;
	void setLods( const int nLods ) {
		m_nLods = nLods;
	}
	int getLods( void ) const {
		return m_nLods;
	}
	void setTessellationLevel( const int nLevel ) {
		m_tessellationLevel = nLevel;
	}
	int getTessellationLevel( void ) const {
		return m_tessellationLevel;
	}
	void setMeshSize( const int nSize ) {
		m_nMeshSize = nSize;
	}
	int getMeshSize( void ) const {
		return m_nMeshSize;
	}
	void setVertexSpacing( const real_t fSpacing ) {
		m_fVertexSpacing = fSpacing;
	}
	real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}
	void setRenderLayers( const uint32_t nLayers ) {
		if ( m_renderLayers == nLayers ) {
			return;
		}
		m_renderLayers = nLayers;
		update();
	}
	uint32_t getRenderLayers( void ) const {
		return m_renderLayers;
	}
	void setCastShadows( RenderingServer::ShadowCastingSetting castShadows );
	void setGiMode( const GeometryInstance3D::GIMode giMode ) {
		m_giMode = giMode;
		update();
	}
	void setShadowLodCount( int nCount );
	void setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled );
};

#endif // TERRAIN3D_MESHER_CLASS_H
