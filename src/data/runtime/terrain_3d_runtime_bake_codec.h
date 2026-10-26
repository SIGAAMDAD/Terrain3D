// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RUNTIME_BAKE_CODEC_H
#define TERRAIN3D_RUNTIME_BAKE_CODEC_H

#include "Terrain3DTypes.h"

#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "Terrain3DConstants.h"
#include "eastl_config.h"

#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include "data/model/terrain_3d_region.h"
#include "data/runtime/terrain_3d_runtime_world_manifest.h"


// t3dRuntimeInstanceCellMetadata_s centralized in Terrain3DTypes.h.


// NodePath-backed runtime record intentionally remains outside the POD type authority.
typedef struct t3dRuntimeLightingReceiverRecord_s {
	NodePath m_path;
	Rect2 m_uvScale;
	int32_t m_iSliceIndex = 0;
	int32_t m_nSubInstance = 0;
} t3dRuntimeLightingReceiverRecord_t;



typedef struct t3dNavigationMaskView_s {

	const uint8_t *m_pValidBits = NULL;
	const uint8_t *m_pNavigationBits = NULL;
	uint32_t m_nPlaneBytes = 0u;
	uint32_t m_nSamples = 0u;
	uint16_t m_nWidth = 0u;

	EA_FORCE_INLINE bool isValidSample( const uint32_t nIndex ) const {
		return m_pValidBits && nIndex < m_nSamples && ( m_pValidBits[nIndex >> 3u] & ( uint8_t )( 1u << ( nIndex & 7u ) ) ) != 0u;
	}
	EA_FORCE_INLINE bool isNavigationSample( const uint32_t nIndex ) const {
		return m_pNavigationBits && nIndex < m_nSamples && ( m_pNavigationBits[nIndex >> 3u] & ( uint8_t )( 1u << ( nIndex & 7u ) ) ) != 0u;
	}
} t3dNavigationMaskView_t;


// t3dMinMaxLevelView_s centralized in Terrain3DTypes.h.


// t3dInstanceMetadataView_s centralized in Terrain3DTypes.h.


// t3dLightingReceiverCursor_s centralized in Terrain3DTypes.h.


class Terrain3DRuntimeBakeCodec {
	CLASS_NAME_STATIC( "Terrain3DRuntimeBakeCodec" );

public:
	static PackedByteArray encodeHoleMask( const Ref<Image> &rControl );
	static PackedByteArray encodeNavigationMask( const Ref<Image> &rControl );
	static PackedByteArray encodeNavigationMask( const PackedInt32Array &rControls );
	static bool getNavigationMaskView( const PackedByteArray &rBlob, t3dNavigationMaskView_t &rView );
	static bool decodeNavigationMask(
	        const PackedByteArray &rBlob, int &nWidth, PackedByteArray &rValidBits, PackedByteArray &rNavigationBits );
	static PackedByteArray encodeMinMaxHierarchy( const Ref<Image> &rHeight );
	static bool getMinMaxLevelView( const PackedByteArray &rBlob, int nMaximumSide, t3dMinMaxLevelView_t &rView );
	static Vector2 getMinMaxRange( const t3dMinMaxLevelView_t &rView, uint32_t nIndex );
	static bool decodeMinMaxLevel( const PackedByteArray &rBlob,
	        int nMaximumSide,
	        int &nSourceSide,
	        int &nLevelSide,
	        eastl::vector<Vector2, Terrain3DEastlAllocator> &rRanges );
	static PackedByteArray encodeCollisionHeight( const PackedFloat32Array &rHeights );
	static PackedByteArray encodeCollisionBounds( float fMinHeight, float fMaxHeight );
	static bool decodeCollisionHeight( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth );
	static bool decodeCollisionHeightInto( const PackedByteArray &rBlob, PackedFloat32Array &rHeights, int &nWidth );
	static bool decodeCollisionBounds( const PackedByteArray &rBlob, float &fMinHeight, float &fMaxHeight, int &nWidth );
	static PackedByteArray encodeNodeDescriptors( const Terrain3DRegion &rRegion, const Terrain3DRuntimeWorldManifest &rManifest );
	static bool decodeNodeDescriptors(
	        const PackedByteArray &rBlob, const Terrain3DRuntimeWorldManifest &rManifest, Terrain3DRegion &rRegion );
	static PackedByteArray encodeInstanceMetadata( const Terrain3DRegion &rRegion );
	static bool getInstanceMetadataView( const PackedByteArray &rBlob, t3dInstanceMetadataView_t &rView );
	static t3dRuntimeInstanceCellMetadata_t getInstanceMetadataRecord( const t3dInstanceMetadataView_t &rView, uint32_t nIndex );
	static bool decodeInstanceMetadata(
	        const PackedByteArray &rBlob, eastl::vector<t3dRuntimeInstanceCellMetadata_t, Terrain3DEastlAllocator> &rCells );
	static PackedByteArray encodeLightingReceivers( const Terrain3DRegion &rRegion );
	static bool beginLightingReceivers( const PackedByteArray &rBlob, t3dLightingReceiverCursor_t &rCursor );
	static bool nextLightingReceiver( t3dLightingReceiverCursor_t &rCursor, t3dRuntimeLightingReceiverRecord_s &rReceiver );
	static bool decodeLightingReceivers(
	        const PackedByteArray &rBlob, eastl::vector<t3dRuntimeLightingReceiverRecord_s, Terrain3DEastlAllocator> &rReceivers );
	static PackedByteArray encodeMultimeshFastStart( const Terrain3DRegion &rRegion, int nMinimumCellInstances = 256 );
	static bool findMultimeshFastStart(
	        const PackedByteArray &rBlob, uint16_t nMeshId, const Vector2i &rCell, PackedFloat32Array &rBuffer, uint32_t &nInstanceCount );
};

#endif // TERRAIN3D_RUNTIME_BAKE_CODEC_H
