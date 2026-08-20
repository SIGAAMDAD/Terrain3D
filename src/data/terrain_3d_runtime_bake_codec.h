// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_RUNTIME_BAKE_CODEC_H
#define TERRAIN3D_RUNTIME_BAKE_CODEC_H

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include "eastl_config.h"
#include <EASTL/vector.h>

class Terrain3DRegion;
class T3DRuntimeWorldManifest;

struct T3DRuntimeInstanceCellMetadata {
	uint16_t meshId = 0u;
	Vector2i cell = V2I_ZERO;
	uint32_t instanceCount = 0u;
	uint32_t ordinal = 0u;
	AABB originBounds;
};

struct T3DRuntimeLightingReceiverRecord {
	NodePath path;
	Rect2 uvScale;
	int32_t sliceIndex = 0;
	int32_t subInstance = 0;
};

class T3DRuntimeBakeCodec {
public:
	static PackedByteArray encodeHoleMask( const Ref<Image> &rControl, int nRegionSize );
	static PackedByteArray encodeNavigationMask( const Ref<Image> &rControl, int nRegionSize );
	static PackedByteArray encodeNavigationMask( const PackedInt32Array &rControls, int nWidth );
	static bool decodeNavigationMask( const PackedByteArray &rBlob, int &nWidth,
		PackedByteArray &rValidBits, PackedByteArray &rNavigationBits );
	static PackedByteArray encodeMinMaxHierarchy( const Ref<Image> &rHeight, int nRegionSize );
	static bool decodeMinMaxLevel( const PackedByteArray &rBlob, int nMaximumSide, int &nSourceSide,
		int &nLevelSide, eastl::vector<Vector2> &rRanges );
	static PackedByteArray encodeCollisionHeight( const PackedFloat32Array &rHeights, int nWidth );
	static PackedByteArray encodeCollisionBounds( float fMinHeight, float fMaxHeight, int nWidth );
	static bool decodeCollisionHeight( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth );
	static bool decodeCollisionBounds( const PackedByteArray &rBlob, float &fMinHeight, float &fMaxHeight, int &nWidth );
	static PackedByteArray encodeNodeDescriptors( const Terrain3DRegion &rRegion, const T3DRuntimeWorldManifest &rManifest );
	static bool decodeNodeDescriptors(
		const PackedByteArray &rBlob, const T3DRuntimeWorldManifest &rManifest, Terrain3DRegion &rRegion );
	static PackedByteArray encodeInstanceMetadata( const Terrain3DRegion &rRegion );
	static bool decodeInstanceMetadata( const PackedByteArray &rBlob,
		eastl::vector<T3DRuntimeInstanceCellMetadata> &rCells );
	static PackedByteArray encodeLightingReceivers( const Terrain3DRegion &rRegion );
	static bool decodeLightingReceivers( const PackedByteArray &rBlob, eastl::vector<T3DRuntimeLightingReceiverRecord> &rReceivers );
	static PackedByteArray encodeMultimeshFastStart( const Terrain3DRegion &rRegion, int nMinimumCellInstances = 256 );
	static bool findMultimeshFastStart( const PackedByteArray &rBlob, uint16_t nMeshId,
		const Vector2i &rCell, PackedFloat32Array &rBuffer, uint32_t &nInstanceCount );
};

#endif // TERRAIN3D_RUNTIME_BAKE_CODEC_H
