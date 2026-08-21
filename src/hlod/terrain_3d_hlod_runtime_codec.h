// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once
#ifndef TERRAIN3D_HLOD_RUNTIME_CODEC_H
#define TERRAIN3D_HLOD_RUNTIME_CODEC_H

#include "Terrain3DConstants.h"
#include "eastl_config.h"

#include <EASTL/array.h>
#include <EASTL/vector.h>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dHlodRuntimeEntryRecord_s {
	int m_nLevel = 1;
	Vector2i m_cluster = s_v2iZero;
	Vector2i m_regionMin = s_v2iZero;
	Vector2i m_regionMax = s_v2iZero;
	AABB m_localBounds;
	String m_packagePath;
	real_t m_fGeometricError = 1.0f;
	int64_t m_nSourceSignature = 0;
	int64_t m_nBuildSignature = 0;
	int64_t m_nEstimatedGpuBytes = 0;
	int m_nParent = -1;
	int m_szChildren[4] = { -1, -1, -1, -1 };
} t3dHlodRuntimeEntryRecord_t;

typedef struct t3dHlodRuntimeNodePolicyRecord_s {
	int64_t m_nSpawnId = 0;
	uint8_t m_nMinLevel = 0;
} t3dHlodRuntimeNodePolicyRecord_t;

typedef struct t3dHlodRuntimeData_s {
	eastl::vector<t3dHlodRuntimeEntryRecord_t> m_entries;
	eastl::vector<int> m_roots;
	eastl::vector<uint8_t> m_instancerMinLevels;
	eastl::vector<t3dHlodRuntimeNodePolicyRecord_t> m_nodePolicies;
	eastl::array<uint32_t, s_nMaxRegionSlots + 1> m_szNodePolicyOffsets{};
} t3dHlodRuntimeData_t;

class T3DHLODRuntimeCodec {
public:
	static PackedByteArray encode( const Array &rEntries,
			const PackedByteArray &rInstancerMinLevels,
			const PackedInt32Array &rNodeRegionIndices,
			const PackedInt64Array &rNodeSpawnIds,
			const PackedByteArray &rNodeMinLevels );
	static bool decode( const PackedByteArray &rBlob, t3dHlodRuntimeData_t &rData );
};

#endif // TERRAIN3D_HLOD_RUNTIME_CODEC_H
