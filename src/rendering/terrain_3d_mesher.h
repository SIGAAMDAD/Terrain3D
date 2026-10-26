// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MESHER_CLASS_H
#define TERRAIN3D_MESHER_CLASS_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"
#include "rendering/rd/terrain_3d_rd_types.h"
#include "rendering/terrain_3d_clipmap_strip_contract.h"

#include <EASTL/array.h>

#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/geometry_instance3d.hpp>

class Terrain3D;

typedef enum t3dMeshType_e {
	LOD0_MESH,
	OUTER_RING_MESH,
	MESH_TYPE_COUNT,
} t3dMeshType_t;


typedef struct t3dMesherRuntimeConfig_s {
	int m_nNearShaderLodCount = s_nMaxClipmapLevels;
	int m_nMiddleShaderLodCount = 0;
	int m_nImmediateLodCount = s_nMaxClipmapLevels;
	int m_nClipmapUpdateBudget = s_nMaxClipmapLevels;
	int m_nShadowLodCount = 0;
	int m_szTierShadowLodCounts[3] = { -1, -1, -1 };
	bool m_szTierShadowEnabled[3] = { true, true, true };
} t3dMesherRuntimeConfig_t;

// Terrain3DMesher owns clipmap placement/snap truth and the canonical procedural
// strip-part descriptors. It owns no terrain vertex/index topology.
class Terrain3DMesher {
	CLASS_NAME_STATIC( "Terrain3DMesher" );

private:
	Terrain3D *m_pTerrain = NULL;
	Vector2 m_szLastLodPositions[s_nMaxClipmapLevels];
	int m_szLastLodTestX[s_nMaxClipmapLevels];
	int m_szLastLodTestZ[s_nMaxClipmapLevels];
	real_t m_fLodScales[s_nMaxClipmapLevels];
	real_t m_fLodSnapSteps[s_nMaxClipmapLevels];
	real_t m_fLodNextSnapSteps[s_nMaxClipmapLevels];
	t3dClipmapStripPart_t m_szStripParts[MESH_TYPE_COUNT][T3D_CLIPMAP_STRIP_PART_CAPACITY]{};
	uint16_t m_szStripPartCounts[MESH_TYPE_COUNT]{};

	Vector3 m_szTrimAPos[s_nPairInstances];
	Vector3 m_szTrimBPos[s_nPairInstances];
	Vector3 m_szTilePosLod0[s_nMaxTileInstances];
	Vector3 m_szFillAPos[s_nPairInstances];
	Vector3 m_szFillBPos[s_nPairInstances];
	Vector3 m_szTilePos[s_nRingTileInstances];
	real_t m_fOffsetA = 0.f;
	real_t m_fOffsetB = 0.f;
	real_t m_fOffsetC = 0.f;
	Vector3 m_szEdgePos[s_nPairInstances];

	t3dMesherRuntimeConfig_t m_runtimeConfig;
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
	int m_nClipmapLevelCount = 0;
	real_t m_fVertexSpacing = 1.f;
	real_t m_fClipmapAabbMinY = -0.1f;
	real_t m_fClipmapAabbSizeY = 0.2f;
	GeometryInstance3D::ShadowCastingSetting m_castShadows = GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;

	void generateClipmap( void );
	bool generateStripParts( void );
	bool appendStripPart( t3dMeshType_t type, const Vector2i &rSize, const Vector3 &rOrigin, uint16_t nPartCode, uint16_t nFlags );
	void clearStripParts( void );
	void generateOffsetData( void );
	void generateLodCache( void );
	void clearClipmap( void );
	void compileTierRuntimeConfig( int nNearShaderLodCount, int nMiddleShaderLodCount, int nClipmapUpdateBudget );
	void compileShadowRuntimeConfig(
	        int nShadowLodCount, const eastl::array<bool, 3> &bTierShadowEnabled, const eastl::array<int, 3> &nTierShadowLodCounts );
	void refreshImmediateLodCount( void );
	int getImmediateLodCount( void ) const;
	int getLodTier( int nLod ) const;
	bool lodCastsShadows( int nLod ) const;
	bool snapLevel( int nLod, const Vector3 &rSnappedTarget );

public:
	EA_FORCE_INLINE Terrain3DMesher( void ) {
	}
	EA_FORCE_INLINE ~Terrain3DMesher( void ) {
		destroy();
	}

	void initialize( Terrain3D *pTerrain,
	        int nMeshSize,
	        int nLods,
	        int nTessellationLevel,
	        real_t fVertexSpacing,
	        int nNearShaderLodCount,
	        int nMiddleShaderLodCount,
	        int nClipmapUpdateBudget,
	        GeometryInstance3D::ShadowCastingSetting castShadows,
	        int nShadowLodCount,
	        const eastl::array<bool, 3> &bTierShadowEnabled,
	        const eastl::array<int, 3> &nTierShadowLodCounts,
	        const Vector3 &rInitialTarget );
	void destroy( void );

	void snap( void );
	void snap( const Vector3 &rTargetPosition );
	void resetTargetPosition( void );
	void update( void ) {
	}
	void updateAabbs( real_t fCullMargin = -1.f, const Vector2 &rHeightRange = s_v2Max );
	void setTierLayout( int nNearShaderLodCount, int nMiddleShaderLodCount, int nClipmapUpdateBudget, bool bForceFullSnap = true );

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

	int getRenderBatchCount( void ) const;
	int getRenderBatchCountForTier( int nTier ) const;
	int getShadowBatchCountForTier( int nTier ) const;
	int getShadowBatchCount( void ) const;
	EA_FORCE_INLINE int getBaseMeshCount( void ) const {
		return MESH_TYPE_COUNT;
	}
	EA_FORCE_INLINE size_t getVisibleGeometryPayloadBytes( void ) const {
		return 0u;
	}
	EA_FORCE_INLINE size_t getShadowGeometryPayloadBytes( void ) const {
		return 0u;
	}
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

	void setCastShadows( GeometryInstance3D::ShadowCastingSetting castShadows );
	EA_FORCE_INLINE const t3dClipmapStripPart_t *getClipmapStripParts( const t3dMeshType_t type ) const {
		return m_szStripParts[( size_t )( type )];
	}
	EA_FORCE_INLINE uint16_t getClipmapStripPartCount( const t3dMeshType_t type ) const {
		return m_szStripPartCounts[( size_t )( type )];
	}
	int fillRdPatches( t3dRdPatch_t *pPatches, int nCapacity ) const;
	void setShadowLodCount( int nCount );
	void setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled );
	void setTierShadowLodCounts( const eastl::array<int, 3> &nCounts );
};

#endif // TERRAIN3D_MESHER_CLASS_H
