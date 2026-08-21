// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MESHER_CLASS_H
#define TERRAIN3D_MESHER_CLASS_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"

#include <EASTL/array.h>
#include <cstdint>

class T3DTerrain;

typedef enum t3dMeshType_e {
	LOD0_MESH,
	OUTER_RING_MESH,
	MESH_TYPE_COUNT,
} t3dMeshType_t;

typedef struct t3dClipmapGeometry_s {
	PackedVector3Array m_vertices;
	PackedVector2Array m_metadata;
	PackedInt32Array m_indices;
	AABB m_aabb;
} t3dClipmapGeometry_t;

typedef struct t3dClipmapBatch_s {
	RID m_hMesh;
	RID m_hMultimesh;
	RID m_hInstance;
	PackedFloat32Array m_transformBuffer;
	Vector2 m_anchorPosition = s_v2Max;
	uint8_t m_nCount = 0;
	uint8_t m_nFirstLod = 0;
	uint8_t m_nLastLod = 0;
	uint8_t m_nTier = 0;
	bool m_bCastsShadows = false;
	bool m_bOwnsMesh = false;
} t3dClipmapBatch_t;

typedef struct t3dLodBatchSlot_s {
	int8_t m_nBatch = -1;
	int8_t m_nInstance = -1;
} t3dLodBatchSlot_t;

class T3DMesher {
	CLASS_NAME_STATIC( "T3DMesher" );

public: // Constants
private:
	T3DTerrain *m_pTerrain = nullptr;
	RID m_hScenario = RID();
	eastl::array<Vector2, s_nMaxClipmapLevels> m_szLastLodPositions{};
	eastl::array<int, s_nMaxClipmapLevels> m_szLastLodTestX{};
	eastl::array<int, s_nMaxClipmapLevels> m_szLastLodTestZ{};
	eastl::array<real_t, s_nMaxClipmapLevels> m_fLodScales{};
	eastl::array<real_t, s_nMaxClipmapLevels> m_fLodSnapSteps{};
	eastl::array<real_t, s_nMaxClipmapLevels> m_fLodNextSnapSteps{};
	Vector3 m_lastShaderTargetPosition = s_v3Max;

	eastl::array<RID, MESH_TYPE_COUNT> m_szMeshRids{};
	eastl::array<t3dClipmapGeometry_t, MESH_TYPE_COUNT> m_szMeshGeometry{};
	PackedVector4Array m_lodStateBuffer;
	eastl::array<t3dClipmapBatch_t, s_nMaxRenderBatches> m_szRenderBatches{};
	eastl::array<t3dLodBatchSlot_t, s_nMaxClipmapLevels> m_szLodBatchSlots{};
	eastl::array<bool, s_nMaxRenderBatches> m_bBatchDirty{};
	eastl::array<t3dClipmapBatch_t, 3> m_szShadowStateBatches{};
	eastl::array<bool, 3> m_bShadowStateBatchDirty{};
	int m_nShadowBatchCount = 0;
	int m_nRenderBatchCount = 0;
	int m_nClipmapLevelCount = 0;
	bool m_bStateMeshBatching = false;
	bool m_bRequireCookedGeometryPack = false;

	eastl::array<Vector3, s_nPairInstances> m_szTrimAPos{};
	eastl::array<Vector3, s_nPairInstances> m_szTrimBPos{};
	eastl::array<Vector3, s_nMaxTileInstances> m_szTilePosLod0{};
	eastl::array<Vector3, s_nPairInstances> m_szFillAPos{};
	eastl::array<Vector3, s_nPairInstances> m_szFillBPos{};
	eastl::array<Vector3, s_nRingTileInstances> m_szTilePos{};
	real_t m_fOffsetA = 0.f;
	real_t m_fOffsetB = 0.f;
	real_t m_fOffsetC = 0.f;
	eastl::array<Vector3, s_nPairInstances> m_szEdgePos{};

	eastl::array<RID, 3> m_szMaterials{};
	eastl::array<RID, 3> m_hszShadowMaterials{};
	int m_nNearShaderLodCount = s_nMaxClipmapLevels;
	int m_nMiddleShaderLodCount = 0;
	int m_nClipmapUpdateBudget = s_nMaxClipmapLevels;
	int m_iDeferredLodCursor = 0;
	bool m_bForceFullSnap = true;
	uint32_t m_nNearUpdatesLastSnap = 0u;
	uint32_t m_nMiddleUpdatesLastSnap = 0u;
	uint32_t m_nFarUpdatesLastSnap = 0u;
	uint32_t m_nTeleportsLastSnap = 0u;
	uint32_t m_nBufferUploadsLastSnap = 0u;
	int m_nTessellationLevel = 0;
	int m_nLods = 0;
	int m_nMeshSize = 0;
	real_t m_fVertexSpacing = 1.f;
	real_t m_fClipmapAabbMinY = -0.1f;
	real_t m_fClipmapAabbSizeY = 0.2f;
	uint32_t m_nRenderLayers = 1u;
	RenderingServer::ShadowCastingSetting m_castShadows = RenderingServer::SHADOW_CASTING_SETTING_OFF;
	GeometryInstance3D::GIMode m_giMode = GeometryInstance3D::GI_MODE_DISABLED;
	int m_nShadowLodCount = 0;
	eastl::array<int, 3> m_nTierShadowLodCounts{ -1, -1, -1 };
	eastl::array<bool, 3> m_bTierShadowEnabled{ true, true, true };

	void generateMeshTypes( void );
	bool loadRuntimeGeometryPack( const String &rDataDirectory );
	t3dClipmapGeometry_t generateCombinedGeometry( bool bLod0 );
	void appendGrid( PackedVector3Array &rVertices,
			PackedVector2Array &rMetadata,
			PackedInt32Array &rIndices,
			int &nVertexWrite,
			int &nIndexWrite,
			const Vector2i &vSize,
			const Vector3 &rOrigin,
			float fPartId,
			bool bStandardGrid );
	RID instantiateMesh( const PackedVector3Array &rVertices,
			const PackedVector2Array &rMetadata,
			const PackedInt32Array &rIndices,
			const AABB &rAabb,
			bool bIncludeBasis = true );
	RID instantiateStateMesh( int nFirstLod, int nLastLod );
	t3dClipmapBatch_t
	createBatch( const RID &rMesh, int nCount, int nFirstLod, int nLastLod, int nTier, bool bCastsShadows );
	t3dClipmapBatch_t createStateBatch( int nFirstLod, int nLastLod, int nTier, bool bCastsShadows );
	void freeBatch( t3dClipmapBatch_t &rBatch );
	void
	setBatchInstance( t3dClipmapBatch_t &rBatch, int iIndex, const Transform3D &rTransform, int iTestX, int iTestZ );
	void flushDirtyBatches( void );
	void refreshStateBatch( int iBatchIndex, eastl::array<bool, 3> &bDirtyTiers );
	void refreshShadowStateBatch( int nTier );
	void updateBatchCustomAabb( int iBatchIndex );
	void configureStateMaterials( void );
	void rebuildRenderBatches( void );
	void updateInstanceRid( const RID &rRid,
			const RID &rMaterial,
			RenderingServer::ShadowCastingSetting castShadows,
			bool bVisible,
			bool bBakedLight,
			bool bDynamicGi );
	bool snapLevel( int nLod, const Vector3 &rSnappedTarget );
	void generateClipmap( void );
	void generateOffsetData( void );
	void generateLodCache( void );

	void clearClipmap( void );
	void clearMeshTypes( void );
	int getImmediateLodCount( void ) const;
	int getLodTier( int nLod ) const;
	bool lodCastsShadows( int nLod ) const;
	bool usesDedicatedShadowBatches( void ) const;
	size_t estimateGeometryPayloadBytes( int nFirstLod, int nLastLod, bool bIncludeBasis ) const;

public:
	EA_FORCE_INLINE T3DMesher( void ) {}
	EA_FORCE_INLINE ~T3DMesher( void ) {
		destroy();
	}

	void initialize( T3DTerrain *pTerrain,
			const int nMeshSize,
			const int nLods,
			const int nTessellationLevel,
			const real_t fVertexSpacing,
			const eastl::array<RID, 3> &rMaterials,
			const eastl::array<RID, 3> &rShadowMaterials,
			const int nNearShaderLodCount,
			const int nMiddleShaderLodCount,
			const int nClipmapUpdateBudget,
			const uint32_t nRenderLayers,
			const RenderingServer::ShadowCastingSetting castShadows,
			const GeometryInstance3D::GIMode giMode,
			const int nShadowLodCount,
			const eastl::array<bool, 3> &bTierShadowEnabled,
			const eastl::array<int, 3> &nTierShadowLodCounts,
			const bool bStateMeshBatching = false,
			const bool bRequireCookedGeometryPack = false );
	void destroy( void );
	bool writeRuntimeGeometryPack( const String &rDataDirectory ) const;

	void snap( void );
	void snap( const Vector3 &rTargetPosition );
	void resetTargetPosition( void );
	void update( void );
	void updateAabbs( const real_t fCullMargin = -1.f, const Vector2 &rHeightRange = s_v2Max );

	void setTierLayout( int nNearShaderLodCount,
			int nMiddleShaderLodCount,
			int nClipmapUpdateBudget,
			bool bForceFullSnap = true );

	EA_FORCE_INLINE void setMaterials( const eastl::array<RID, 3> &rMaterials ) {
		m_szMaterials = rMaterials;
		m_lastShaderTargetPosition = s_v3Max;
		configureStateMaterials();
		update();
	}
	EA_FORCE_INLINE void setShadowMaterials( const eastl::array<RID, 3> &rMaterials ) {
		m_hszShadowMaterials = rMaterials;
		m_lastShaderTargetPosition = s_v3Max;
		configureStateMaterials();
		update();
	}
	EA_FORCE_INLINE RID getMaterial( void ) const {
		return m_szMaterials[0];
	}
	EA_FORCE_INLINE uint32_t getNearUpdatesLastSnap( void ) const {
		return m_nNearUpdatesLastSnap;
	}
	EA_FORCE_INLINE uint32_t getMiddleUpdatesLastSnap( void ) const {
		return m_nMiddleUpdatesLastSnap;
	}
	EA_FORCE_INLINE uint32_t getFarUpdatesLastSnap( void ) const {
		return m_nFarUpdatesLastSnap;
	}
	EA_FORCE_INLINE uint32_t getTeleportsLastSnap( void ) const {
		return m_nTeleportsLastSnap;
	}
	EA_FORCE_INLINE uint32_t getBufferUploadsLastSnap( void ) const {
		return m_nBufferUploadsLastSnap;
	}
	EA_FORCE_INLINE int getRenderBatchCount( void ) const {
		return m_nRenderBatchCount;
	}
	int getRenderBatchCountForTier( const int nTier ) const {
		int count = 0;
		for ( int index = 0; index < m_nRenderBatchCount; ++index ) {
			count += ( int )( m_szRenderBatches[( size_t )( index )].m_nTier ) == nTier ? 1 : 0;
		}
		return count;
	}
	int getShadowBatchCountForTier( const int nTier ) const {
		int count = 0;
		for ( int index = 0; index < m_nRenderBatchCount; ++index ) {
			const t3dClipmapBatch_t &batch = m_szRenderBatches[( size_t )( index )];
			count += ( int )( batch.m_nTier ) == nTier && batch.m_bCastsShadows ? 1 : 0;
		}
		for ( int index = 0; index < m_nShadowBatchCount; ++index ) {
			count += ( int )( m_szShadowStateBatches[( size_t )( index )].m_nTier ) == nTier ? 1 : 0;
		}
		return count;
	}
	int getShadowBatchCount( void ) const {
		if ( m_nShadowBatchCount > 0 ) {
			return m_nShadowBatchCount;
		}
		int count = 0;
		for ( int index = 0; index < m_nRenderBatchCount; ++index ) {
			count += m_szRenderBatches[( size_t )( index )].m_bCastsShadows ? 1 : 0;
		}
		return count;
	}
	int getBaseMeshCount( void ) const {
		if ( !m_bStateMeshBatching ) {
			return MESH_TYPE_COUNT;
		}
		int count = m_nRenderBatchCount;
		for ( int index = 0; index < m_nShadowBatchCount; ++index ) {
			count += m_szShadowStateBatches[( size_t )( index )].m_bOwnsMesh ? 1 : 0;
		}
		return count;
	}
	size_t getVisibleGeometryPayloadBytes( void ) const;
	size_t getShadowGeometryPayloadBytes( void ) const;
	size_t getStreamingBufferBytes( void ) const;
	EA_FORCE_INLINE void setLods( const int nLods ) {
		m_nLods = nLods;
	}
	EA_FORCE_INLINE int getLods( void ) const {
		return m_nLods;
	}
	EA_FORCE_INLINE void setTessellationLevel( const int nLevel ) {
		m_nTessellationLevel = nLevel;
	}
	EA_FORCE_INLINE int getTessellationLevel( void ) const {
		return m_nTessellationLevel;
	}
	EA_FORCE_INLINE void setMeshSize( const int nSize ) {
		m_nMeshSize = nSize;
	}
	EA_FORCE_INLINE int getMeshSize( void ) const {
		return m_nMeshSize;
	}
	EA_FORCE_INLINE void setVertexSpacing( const real_t fSpacing ) {
		m_fVertexSpacing = fSpacing;
	}
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}
	void setRenderLayers( const uint32_t nLayers ) {
		if ( m_nRenderLayers == nLayers ) {
			return;
		}
		m_nRenderLayers = nLayers;
		update();
	}
	EA_FORCE_INLINE uint32_t getRenderLayers( void ) const {
		return m_nRenderLayers;
	}
	void setCastShadows( RenderingServer::ShadowCastingSetting castShadows );
	EA_FORCE_INLINE void setGiMode( const GeometryInstance3D::GIMode giMode ) {
		m_giMode = giMode;
		update();
	}
	void setShadowLodCount( int nCount );
	void setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled );
	void setTierShadowLodCounts( const eastl::array<int, 3> &nCounts );
};

#endif // TERRAIN3D_MESHER_CLASS_H
