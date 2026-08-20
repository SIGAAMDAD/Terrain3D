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

struct T3DHLODRuntimeEntryRecord {
	int level = 1;
	Vector2i cluster = V2I_ZERO;
	Vector2i regionMin = V2I_ZERO;
	Vector2i regionMax = V2I_ZERO;
	AABB localBounds;
	String packagePath;
	real_t geometricError = 1.0f;
	int64_t sourceSignature = 0;
	int64_t buildSignature = 0;
	int64_t estimatedGpuBytes = 0;
	int parent = -1;
	int children[4] = {-1, -1, -1, -1};
};

struct T3DHLODRuntimeNodePolicyRecord {
	int64_t spawnId = 0;
	uint8_t minLevel = 0;
};

struct T3DHLODRuntimeData {
	eastl::vector<T3DHLODRuntimeEntryRecord> entries;
	eastl::vector<int> roots;
	eastl::vector<uint8_t> instancerMinLevels;
	eastl::vector<T3DHLODRuntimeNodePolicyRecord> nodePolicies;
	eastl::array<uint32_t, TERRAIN3D_MAX_REGION_SLOTS + 1> nodePolicyOffsets{};
};

class T3DHLODRuntimeCodec {
public:
	static PackedByteArray encode(
		const Array &rEntries,
		const PackedByteArray &rInstancerMinLevels,
		const PackedInt32Array &rNodeRegionIndices,
		const PackedInt64Array &rNodeSpawnIds,
		const PackedByteArray &rNodeMinLevels
	);
	static bool decode( const PackedByteArray &rBlob, T3DHLODRuntimeData &rData );
};

#endif // TERRAIN3D_HLOD_RUNTIME_CODEC_H
