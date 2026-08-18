// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_INSTANCE_PAGE_CODEC_H
#define TERRAIN3D_INSTANCE_PAGE_CODEC_H

#include <cstddef>
#include <cstdint>

#include <godot_cpp/variant/packed_byte_array.hpp>

#include "assets/terrain_3d_assets.h"
#include "instances/terrain_3d_instance_store.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DInstancePageCodec {
public:
	static bool encode(
		const T3DInstanceStore &rStore, PackedByteArray &rPage,
		uint32_t *pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount
	);
	static bool decode(
		const PackedByteArray &rPage, T3DInstanceStore &rStore,
		uint32_t *pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount
	);
	static bool inspect(
		const PackedByteArray &rPage, uint32_t *pMeshCounts,
		uint32_t &nInstanceCount, uint16_t &nMeshCount
	);
};

#endif // TERRAIN3D_INSTANCE_PAGE_CODEC_H
