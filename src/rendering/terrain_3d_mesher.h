// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_MESHER_CLASS_H
#define TERRAIN3D_MESHER_CLASS_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <EASTL/array.h>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/classes/geometry_instance3d.hpp>

class Terrain3D;

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

// Terrain3DMesher is now a CPU topology/snap-state producer only. RenderingDevice
// owns every GPU resource and every draw. Keeping the clipmap math here avoids
// duplicating the mature topology/stitch rules in the renderer.
class Terrain3DMesher {
	CLASS_NAME_STATIC( "Terrain3DMesher" );

private:
	Terrain3D *m_pTerrain = nullptr;
	eastl::array<Vector2, s_nMaxClipmapLevels> m_szLastLodPositions{};
	eastl::array<int, s_nMaxClipmapLevels> m_szLastLodTestX{};
	eastl::array<int, s_nMaxClipmapLevels> m_szLastLodTestZ{};
	eastl::array<real_t, s_nMaxClipmapLevels> m_fLodScales{};
	eastl::array<real_t, s_nMaxClipmapLevels> m_fLodSnapSteps{};
	eastl::array<real_t, s_nMaxClipmapLevels> m_fLodNextSnapSteps{};
	eastl::array<t3dClipmapGeometry_t, MESH_TYPE_COUNT> m_szMeshGeometry{};

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

	int m_nNearShaderLodCount = s_nMaxClipmapLevels;
	int m_nMiddleShaderLodCount = 0;
	int m_nClipmapUpdateBudget = s_nMaxClipmapLevels;
	int m_iDeferredLodCursor = 0;
	bool m_bForceFullSnap = true;
	bool m_bRequireCookedGeometryPack = false;
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
	void generateClipmap( void );
	void generateOffsetData( void );
	void generateLodCache( void );
	void clearClipmap( void );
	void clearMeshTypes( void );
	int getImmediateLodCount( void ) const;
	int getLodTier( int nLod ) const;
	bool lodCastsShadows( int nLod ) const;
	bool snapLevel( int nLod, const Vector3 &rSnappedTarget );
	size_t estimateGeometryPayloadBytes( int nFirstLod, int nLastLod ) const;

public:
	EA_FORCE_INLINE Terrain3DMesher( void ) {}
	EA_FORCE_INLINE ~Terrain3DMesher( void ) { destroy(); }

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
			bool bRequireCookedGeometryPack = false );
	void destroy( void );
	bool writeRuntimeGeometryPack( const String &rDataDirectory ) const;

	void snap( void );
	void snap( const Vector3 &rTargetPosition );
	void resetTargetPosition( void );
	void update( void ) {}
	void updateAabbs( real_t fCullMargin = -1.f, const Vector2 &rHeightRange = s_v2Max );
	void setTierLayout( int nNearShaderLodCount, int nMiddleShaderLodCount, int nClipmapUpdateBudget, bool bForceFullSnap = true );

	EA_FORCE_INLINE uint32_t getNearUpdatesLastSnap( void ) const { return m_nNearUpdatesLastSnap; }
	EA_FORCE_INLINE uint32_t getMiddleUpdatesLastSnap( void ) const { return m_nMiddleUpdatesLastSnap; }
	EA_FORCE_INLINE uint32_t getFarUpdatesLastSnap( void ) const { return m_nFarUpdatesLastSnap; }
	EA_FORCE_INLINE uint32_t getTeleportsLastSnap( void ) const { return m_nTeleportsLastSnap; }
	EA_FORCE_INLINE uint32_t getBufferUploadsLastSnap( void ) const { return m_nBufferUploadsLastSnap; }

	int getRenderBatchCount( void ) const;
	int getRenderBatchCountForTier( int nTier ) const;
	int getShadowBatchCountForTier( int nTier ) const;
	int getShadowBatchCount( void ) const;
	EA_FORCE_INLINE int getBaseMeshCount( void ) const { return MESH_TYPE_COUNT; }
	size_t getVisibleGeometryPayloadBytes( void ) const;
	EA_FORCE_INLINE size_t getShadowGeometryPayloadBytes( void ) const { return 0u; }
	size_t getStreamingBufferBytes( void ) const;

	EA_FORCE_INLINE void setLods( const int nLods ) { m_nLods = nLods; }
	EA_FORCE_INLINE int getLods( void ) const { return m_nLods; }
	EA_FORCE_INLINE void setTessellationLevel( const int nLevel ) { m_nTessellationLevel = nLevel; }
	EA_FORCE_INLINE int getTessellationLevel( void ) const { return m_nTessellationLevel; }
	EA_FORCE_INLINE void setMeshSize( const int nSize ) { m_nMeshSize = nSize; }
	EA_FORCE_INLINE int getMeshSize( void ) const { return m_nMeshSize; }
	EA_FORCE_INLINE void setVertexSpacing( const real_t fSpacing ) { m_fVertexSpacing = fSpacing; }
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const { return m_fVertexSpacing; }

	void setCastShadows( GeometryInstance3D::ShadowCastingSetting castShadows );
	EA_FORCE_INLINE const t3dClipmapGeometry_t &getClipmapGeometry( const t3dMeshType_t type ) const {
		return m_szMeshGeometry[( size_t )( type )];
	}
	int fillRdPatches( t3dRdPatch_t *pPatches, int nCapacity ) const;
	void setShadowLodCount( int nCount );
	void setTierShadowEnabled( const eastl::array<bool, 3> &bEnabled );
	void setTierShadowLodCounts( const eastl::array<int, 3> &nCounts );
};

#endif // TERRAIN3D_MESHER_CLASS_H
