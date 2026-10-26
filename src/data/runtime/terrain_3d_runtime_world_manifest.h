// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RUNTIME_WORLD_MANIFEST_H
#define TERRAIN3D_RUNTIME_WORLD_MANIFEST_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DConstants.h"
#include "eastl_config.h"

#include <EASTL/vector.h>
#include <stdint.h>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"
typedef struct t3dRuntimeWorldRegionRecord_s
{
	t3dRegionCoord_t m_location = t3dInvalidRegionCoord();
	Vector2			 m_heightRange = s_v2Zero;
	uint32_t		 m_nChannelFlags = 0u;
	uint32_t		 m_nMaterialMask = 1u;
	uint64_t		 m_nPageSourceSignature = 0u;
	uint32_t		 m_nInstanceCount = 0u;
	uint16_t		 m_nInstanceMeshCount = 0u;
	uint16_t		 m_nStreamedNodeCount = 0u;
	Vector2i		 m_nodeCoverageMin = s_v2iZero;
	Vector2i		 m_nodeCoverageMax = s_v2iZero;
	uint32_t		 m_nSceneDependencyFirst = 0u;
	uint16_t		 m_nSceneDependencyCount = 0u;
	uint16_t		 m_nFlags = 0u;
	uint32_t		 m_nLightmapDependency = UINT32_MAX;
	uint32_t		 m_nLightmapDirectionalSignatureDependency = UINT32_MAX;
	uint32_t		 m_nLightmapReceiverCount = 0u;
	uint32_t		 m_nLightmapContributorCount = 0u;
	uint64_t		 m_nLightmapEstimatedGpuBytes = 0u;
	int64_t			 m_nLightmapBakeSignature = 0;
	uint16_t		 m_nLightmapShadowmaskMode = 0u;
	uint16_t		 m_nLightmapFlags = 0u;
	uint16_t		 m_nLightmapTextureCount = 0u;
	uint16_t		 m_nLightmapShadowmaskTextureCount = 0u;
	uint16_t		 m_nLightmapClusterSizeRegions = 1u;
	uint16_t		 m_nLightmapContextRadiusRegions = 1u;
	uint16_t		 m_nLightmapTerrainProxyLod = 2u;
	uint16_t		 m_nReserved = 0u;
	uint32_t		 m_szInstanceMeshCounts[T3D_MAX_MESH_ASSETS]{};
} t3dRuntimeWorldRegionRecord_t;

// Immutable cooked-world topology/dependency index. Runtime residency keeps
// mutable slot state elsewhere; this object only owns cook-time facts.
class Terrain3DRuntimeWorldManifest
{
public:
	Terrain3DRuntimeWorldManifest( void );
	void reset( void );
	void releaseRuntimeAllocatorStorage( void );
	bool buildAuthoringIndex( const Terrain3DRegionRepository &rRepository );
	bool write( const String					&rDataDirectory,
				const Terrain3DRegionRepository &rRepository,
				const Terrain3DRuntimePageStore &rPageStore,
				const t3dRuntimePageConfig_s	&rConfig,
				PackedStringArray				*pErrors = NULL );
	bool load( const String &rDataDirectory, PackedStringArray *pErrors = NULL );
	bool stageRepository( const String &rDataDirectory, t3dStagedRepository_s &rStaged ) const;

	String				 getPath( const String &rDataDirectory ) const;
	EA_FORCE_INLINE bool isLoaded(
		void ) const {
		return m_bLoaded;
	}
	EA_FORCE_INLINE int getRegionSize(
		void ) const {
		return T3D_REGION_SIZE;
	}
	EA_FORCE_INLINE int getRegionCount(
		void ) const {
		return ( int ) ( m_regions.size() );
	}
	EA_FORCE_INLINE real_t getVertexSpacing(
		void ) const {
		return ( real_t ) ( s_fRegionVertexSpacing );
	}
	EA_FORCE_INLINE Vector2i getWorldRegionMin(
		void ) const {
		return t3dUnpackRegionCoord( m_worldRegionMin );
	}
	EA_FORCE_INLINE Vector2i getWorldRegionMax(
		void ) const {
		return t3dUnpackRegionCoord( m_worldRegionMax );
	}
	bool					 hasRegion( const Vector2i &rLocation ) const;
	EA_FORCE_INLINE uint64_t getWorldSignature(
		void ) const {
		return m_nWorldSignature;
	}
	EA_FORCE_INLINE uint64_t getMaterialSignature(
		void ) const {
		return m_nMaterialSignature;
	}
	EA_FORCE_INLINE Vector2 getWorldHeightRange(
		void ) const {
		return m_worldHeightRange;
	}
	const t3dRuntimeWorldRegionRecord_t *findRegion( const Vector2i &rLocation ) const;
	EA_FORCE_INLINE uint8_t				 getRegionAdjacencyMask(
					 const Vector2i &rLocation ) const {
		const t3dRuntimeWorldRegionRecord_t *record = findRegion( rLocation );
		return record ? ( uint8_t ) ( record->m_nFlags & 0xffu ) : 0u;
	}
	int					  findSceneDependency( const String &rPath ) const;
	const String		 &getSceneDependency( uint32_t nIndex ) const;
	EA_FORCE_INLINE const eastl::vector< uint16_t > &getRegionSceneDependencies(
		void ) const {
		return m_regionSceneDependencies;
	}
	EA_FORCE_INLINE const eastl::vector< t3dRuntimeWorldRegionRecord_t > &getRegions(
		void ) const {
		return m_regions;
	}

private:
	bool										   m_bLoaded = false;
	t3dRegionCoord_t							   m_worldRegionMin = t3dPackRegionCoord( s_v2iZero );
	t3dRegionCoord_t							   m_worldRegionMax = t3dPackRegionCoord( s_v2iZero );
	uint64_t									   m_nWorldSignature = 0u;
	uint64_t									   m_nMaterialSignature = 0u;
	Vector2										   m_worldHeightRange = s_v2Zero;
	eastl::vector< t3dRuntimeWorldRegionRecord_t > m_regions;
	eastl::vector< String >						   m_sceneDependencies;
	eastl::vector< uint16_t >					   m_regionSceneDependencies;
	int16_t										   m_regionLookup[T3D_REGION_SLOTS]{};
	uint64_t									   m_topologyBits[T3D_REGION_SLOTS / 64]{};

	void rebuildLookup( void );
};

#endif // TERRAIN3D_RUNTIME_WORLD_MANIFEST_H
