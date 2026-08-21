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

#include <cstddef>
#include <cstdint>

class T3DRegion;
class T3DRuntimeWorldManifest;

typedef struct t3dRuntimeInstanceCellMetadata_s {
	uint16_t m_nMeshId = 0u;
	Vector2i m_cell = V2I_ZERO;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nOrdinal = 0u;
	AABB m_originBounds;
} t3dRuntimeInstanceCellMetadata_t;

typedef struct t3dRuntimeLightingReceiverRecord_s {
	NodePath m_path;
	Rect2 m_uvScale;
	int32_t m_iSliceIndex = 0;
	int32_t m_nSubInstance = 0;
} t3dRuntimeLightingReceiverRecord_t;

typedef struct t3dNavigationMaskView_s {
	const uint8_t *m_pValidBits = nullptr;
	const uint8_t *m_pNavigationBits = nullptr;
	uint32_t m_nPlaneBytes = 0u;
	uint32_t m_nSamples = 0u;
	uint16_t m_nWidth = 0u;

	EA_FORCE_INLINE bool isValidSample( const uint32_t nIndex ) const { return m_pValidBits && nIndex < m_nSamples && ( m_pValidBits[nIndex >> 3u] & ( uint8_t ) ( 1u << ( nIndex & 7u ) ) ) != 0u; }
	EA_FORCE_INLINE bool isNavigationSample( const uint32_t nIndex ) const { return m_pNavigationBits && nIndex < m_nSamples && ( m_pNavigationBits[nIndex >> 3u] & ( uint8_t ) ( 1u << ( nIndex & 7u ) ) ) != 0u; }
} t3dNavigationMaskView_t;

typedef struct t3dMinMaxLevelView_s {
	const uint8_t *m_pRanges = nullptr;
	uint32_t m_nRangeCount = 0u;
	uint16_t m_nSourceSide = 0u;
	uint16_t m_nLevelSide = 0u;
} t3dMinMaxLevelView_t;

typedef struct t3dInstanceMetadataView_s {
	const uint8_t *m_pRecords = nullptr;
	uint32_t m_nCellCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint16_t m_nMeshCount = 0u;
} t3dInstanceMetadataView_t;

typedef struct t3dLightingReceiverCursor_s {
	const uint8_t *m_pData = nullptr;
	size_t m_nSize = 0u;
	size_t m_iOffset = 0u;
	uint32_t m_nCount = 0u;
	uint32_t m_iIndex = 0u;
} t3dLightingReceiverCursor_t;

class T3DRuntimeBakeCodec {
public:
	static PackedByteArray encodeHoleMask( const Ref<Image> &rControl, int nRegionSize );
	static PackedByteArray encodeNavigationMask( const Ref<Image> &rControl, int nRegionSize );
	static PackedByteArray encodeNavigationMask( const PackedInt32Array &rControls, int nWidth );
	static bool getNavigationMaskView( const PackedByteArray &rBlob, t3dNavigationMaskView_t &rView );
	static bool decodeNavigationMask( const PackedByteArray &rBlob, int &nWidth,
		PackedByteArray &rValidBits, PackedByteArray &rNavigationBits );
	static PackedByteArray encodeMinMaxHierarchy( const Ref<Image> &rHeight, int nRegionSize );
	static bool getMinMaxLevelView( const PackedByteArray &rBlob, int nMaximumSide, t3dMinMaxLevelView_t &rView );
	static Vector2 getMinMaxRange( const t3dMinMaxLevelView_t &rView, uint32_t nIndex );
	static bool decodeMinMaxLevel( const PackedByteArray &rBlob, int nMaximumSide, int &nSourceSide,
		int &nLevelSide, eastl::vector<Vector2> &rRanges );
	static PackedByteArray encodeCollisionHeight( const PackedFloat32Array &rHeights, int nWidth );
	static PackedByteArray encodeCollisionBounds( float fMinHeight, float fMaxHeight, int nWidth );
	static bool decodeCollisionHeight( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth );
	static bool decodeCollisionBounds( const PackedByteArray &rBlob, float &fMinHeight, float &fMaxHeight, int &nWidth );
	static PackedByteArray encodeNodeDescriptors( const T3DRegion &rRegion, const T3DRuntimeWorldManifest &rManifest );
	static bool decodeNodeDescriptors(
		const PackedByteArray &rBlob, const T3DRuntimeWorldManifest &rManifest, T3DRegion &rRegion );
	static PackedByteArray encodeInstanceMetadata( const T3DRegion &rRegion );
	static bool getInstanceMetadataView( const PackedByteArray &rBlob, t3dInstanceMetadataView_t &rView );
	static t3dRuntimeInstanceCellMetadata_t getInstanceMetadataRecord( const t3dInstanceMetadataView_t &rView, uint32_t nIndex );
	static bool decodeInstanceMetadata( const PackedByteArray &rBlob,
		eastl::vector<t3dRuntimeInstanceCellMetadata_t> &rCells );
	static PackedByteArray encodeLightingReceivers( const T3DRegion &rRegion );
	static bool beginLightingReceivers( const PackedByteArray &rBlob, t3dLightingReceiverCursor_t &rCursor );
	static bool nextLightingReceiver( t3dLightingReceiverCursor_t &rCursor, t3dRuntimeLightingReceiverRecord_t &rReceiver );
	static bool decodeLightingReceivers( const PackedByteArray &rBlob, eastl::vector<t3dRuntimeLightingReceiverRecord_t> &rReceivers );
	static PackedByteArray encodeMultimeshFastStart( const T3DRegion &rRegion, int nMinimumCellInstances = 256 );
	static bool findMultimeshFastStart( const PackedByteArray &rBlob, uint16_t nMeshId,
		const Vector2i &rCell, PackedFloat32Array &rBuffer, uint32_t &nInstanceCount );
};

#endif // TERRAIN3D_RUNTIME_BAKE_CODEC_H
