// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RUNTIME_WORLD_MANIFEST_H
#define TERRAIN3D_RUNTIME_WORLD_MANIFEST_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"
#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <cstdint>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

class T3DRegion;
class T3DRegionRepository;
class T3DRuntimePageStore;
struct T3DRuntimePageConfig;
struct T3DStagedRepository;

static constexpr uint64_t TERRAIN3D_RUNTIME_WORLD_MANIFEST_MAGIC = 0x31464e4d57334454ull; // T3DWMNF1
static constexpr uint32_t TERRAIN3D_RUNTIME_WORLD_MANIFEST_VERSION = 3u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_N = 1u << 0u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_NE = 1u << 1u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_E = 1u << 2u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_SE = 1u << 3u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_S = 1u << 4u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_SW = 1u << 5u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_W = 1u << 6u;
static constexpr uint16_t TERRAIN3D_RUNTIME_WORLD_ADJ_NW = 1u << 7u;

struct T3DRuntimeWorldRegionRecord {
	Vector2i location = V2I_MAX;
	Vector2 heightRange = V2_ZERO;
	uint32_t channelFlags = 0u;
	uint32_t materialMask = 1u;
	uint64_t pageSourceSignature = 0u;
	uint32_t instanceCount = 0u;
	uint16_t instanceMeshCount = 0u;
	uint16_t streamedNodeCount = 0u;
	Vector2i nodeCoverageMin = V2I_ZERO;
	Vector2i nodeCoverageMax = V2I_ZERO;
	uint32_t sceneDependencyFirst = 0u;
	uint16_t sceneDependencyCount = 0u;
	uint16_t flags = 0u;
	uint32_t lightmapDependency = UINT32_MAX;
	uint32_t lightmapDirectionalSignatureDependency = UINT32_MAX;
	uint32_t lightmapReceiverCount = 0u;
	uint32_t lightmapContributorCount = 0u;
	uint64_t lightmapEstimatedGpuBytes = 0u;
	int64_t lightmapBakeSignature = 0;
	uint16_t lightmapShadowmaskMode = 0u;
	uint16_t lightmapFlags = 0u;
	uint16_t lightmapTextureCount = 0u;
	uint16_t lightmapShadowmaskTextureCount = 0u;
	uint16_t lightmapClusterSizeRegions = 1u;
	uint16_t lightmapContextRadiusRegions = 1u;
	uint16_t lightmapTerrainProxyLod = 2u;
	uint16_t reserved = 0u;
	eastl::array<uint32_t, TERRAIN3D_MAX_MESH_ASSETS> instanceMeshCounts{};
};

// Immutable cooked-world topology/dependency index. Runtime residency keeps
// mutable slot state elsewhere; this object only owns cook-time facts.
class T3DRuntimeWorldManifest {
public:
	void reset( void );
	bool buildAuthoringIndex( const T3DRegionRepository &rRepository );
	bool write(
		const String &rDataDirectory, const T3DRegionRepository &rRepository,
		const T3DRuntimePageStore &rPageStore, const T3DRuntimePageConfig &rConfig,
		PackedStringArray *pErrors = nullptr
	);
	bool load( const String &rDataDirectory, PackedStringArray *pErrors = nullptr );
	bool stageRepository( const String &rDataDirectory, T3DStagedRepository &rStaged ) const;

	String getPath( const String &rDataDirectory ) const;
	EA_FORCE_INLINE bool isLoaded( void ) const { return m_bLoaded; }
	EA_FORCE_INLINE int getRegionSize( void ) const { return m_nRegionSize; }
	EA_FORCE_INLINE int getRegionCount( void ) const { return ( int ) ( m_regions.size() ); }
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const { return m_fVertexSpacing; }
	EA_FORCE_INLINE Vector2i getWorldRegionMin( void ) const { return m_worldRegionMin; }
	EA_FORCE_INLINE Vector2i getWorldRegionMax( void ) const { return m_worldRegionMax; }
	bool hasRegion( const Vector2i &rLocation ) const;
	EA_FORCE_INLINE uint64_t getWorldSignature( void ) const { return m_nWorldSignature; }
	EA_FORCE_INLINE uint64_t getMaterialSignature( void ) const { return m_nMaterialSignature; }
	EA_FORCE_INLINE Vector2 getWorldHeightRange( void ) const { return m_worldHeightRange; }
	const T3DRuntimeWorldRegionRecord *findRegion( const Vector2i &rLocation ) const;
	EA_FORCE_INLINE uint8_t getRegionAdjacencyMask( const Vector2i &rLocation ) const { const T3DRuntimeWorldRegionRecord *record = findRegion( rLocation ); return record ? ( uint8_t ) ( record->flags & 0xffu ) : 0u; }
	int findSceneDependency( const String &rPath ) const;
	const String &getSceneDependency( uint32_t nIndex ) const;
	EA_FORCE_INLINE const eastl::vector<uint16_t> &getRegionSceneDependencies( void ) const { return m_regionSceneDependencies; }
	EA_FORCE_INLINE const eastl::vector<T3DRuntimeWorldRegionRecord> &getRegions( void ) const { return m_regions; }

private:
	bool m_bLoaded = false;
	int m_nRegionSize = 0;
	int m_nRegionMapSize = 0;
	real_t m_fVertexSpacing = 1.0;
	Vector2i m_worldRegionMin = V2I_ZERO;
	Vector2i m_worldRegionMax = V2I_ZERO;
	uint64_t m_nWorldSignature = 0u;
	uint64_t m_nMaterialSignature = 0u;
	Vector2 m_worldHeightRange = V2_ZERO;
	eastl::vector<T3DRuntimeWorldRegionRecord> m_regions;
	eastl::vector<String> m_sceneDependencies;
	eastl::vector<uint16_t> m_regionSceneDependencies;
	eastl::array<int16_t, TERRAIN3D_MAX_REGION_SLOTS> m_regionLookup{};
	eastl::array<uint64_t, TERRAIN3D_MAX_REGION_SLOTS / 64> m_topologyBits{};

	void rebuildLookup( void );
};

#endif // TERRAIN3D_RUNTIME_WORLD_MANIFEST_H
