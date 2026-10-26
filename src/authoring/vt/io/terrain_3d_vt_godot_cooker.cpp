// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"

#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include <EASTL/sort.h>

#include "authoring/vt/build/terrain_3d_vt_reference_encoder.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "memory/core/terrain_3d_memory.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include "data/persistence/terrain_3d_file_publication.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "core/utility/terrain_3d_checked_math.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <stdint.h>
#include <string.h>

using namespace godot;

static bool t3d_vt_godot_extract_texture(
        const Ref<Texture2D> &rTexture, const Image::Format eFormat, PackedByteArray &rBytes, t3dVTCookImageView_t &rView ) {
	memset( &rView, 0, sizeof( rView ) );
	rBytes.clear();
	if ( rTexture.is_null() ) {
		return true;
	}
	Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	pImage = pImage->duplicate();
	if ( pImage.is_null() ) {
		return false;
	}
	if ( pImage->is_compressed() && pImage->decompress() != OK ) {
		return false;
	}
	if ( pImage->has_mipmaps() ) {
		pImage->clear_mipmaps();
	}
	pImage->convert( eFormat );
	if ( pImage->get_format() != eFormat ) {
		return false;
	}
	rBytes = pImage->get_data();
	if ( rBytes.is_empty() ) {
		return false;
	}
	uint32_t nComponents = 0u;
	switch ( eFormat ) {
		case Image::FORMAT_R8:
			nComponents = 1u;
			break;
		case Image::FORMAT_RG8:
			nComponents = 2u;
			break;
		case Image::FORMAT_RGBA8:
			nComponents = 4u;
			break;
		default:
			return false;
	}
	const uint64_t nRequiredBytes = ( uint64_t )pImage->get_width() * pImage->get_height() * nComponents;
	if ( nRequiredBytes != ( uint64_t )rBytes.size() ) {
		return false;
	}
	rView.m_pPixels = rBytes.ptr();
	rView.m_nWidth = ( uint32_t )pImage->get_width();
	rView.m_nHeight = ( uint32_t )pImage->get_height();
	rView.m_nComponents = nComponents;
	rView.m_nRowBytes = rView.m_nWidth * nComponents;
	return true;
}
static bool t3d_vt_godot_tile_has_cutout_alpha( const t3dVTEncodeTileRequest_t &rRequest ) {
	if ( rRequest.m_nComponents < 4u ) {
		return false;
	}
	const uint32_t nPixels = rRequest.m_nWidth * rRequest.m_nHeight;
	for ( uint32_t iPixel = 0u; iPixel < nPixels; ++iPixel ) {
		if ( rRequest.m_pSourcePixels[iPixel * rRequest.m_nComponents + 3u] < 128u ) {
			return true;
		}
	}
	return false;
}

static bool t3d_vt_godot_reference_fallback( const t3dVTEncodeTileRequest_t &rRequest, uint32_t &rWrittenBytes ) {
	t3dVTEncoderInterface_t ReferenceEncoder;
	t3d_vt_reference_encoder_make_interface( ReferenceEncoder );
	return ReferenceEncoder.m_pEncodeTile( ReferenceEncoder.m_pUser, rRequest, rWrittenBytes );
}

static bool t3d_vt_godot_encode_tile( void *pUser, const t3dVTEncodeTileRequest_t &rRequest, uint32_t &rWrittenBytes ) {
	( void )pUser;
	rWrittenBytes = 0u;
	const bool bV3 = rRequest.m_nContractVersion == T3D_VT_FILE_VERSION_V3;
	const uint32_t nBc1Format = bV3 ? T3D_VT_PHYSICAL_FORMAT_BC1_UNORM : T3D_VT_FORMAT_ALBEDO_BC1_SRGB;
	const uint32_t nBc7Format = bV3 ? T3D_VT_PHYSICAL_FORMAT_BC7_UNORM : T3D_VT_FORMAT_SURFACE_BC7_UNORM;
	const uint32_t nBc5Format = bV3 ? T3D_VT_PHYSICAL_FORMAT_BC5_UNORM : T3D_VT_FORMAT_NORMAL_BC5_UNORM;
	const uint32_t nHeightFormat = bV3 ? T3D_VT_PHYSICAL_FORMAT_R8_UNORM : T3D_VT_FORMAT_HEIGHT_R8_UNORM;
	if ( rRequest.m_nFormat == nHeightFormat ) {
		memcpy( rRequest.m_pDestination, rRequest.m_pSourcePixels, rRequest.m_nSourceBytes );
		rWrittenBytes = rRequest.m_nSourceBytes;
		return true;
	}
	const bool bAlbedo = bV3 ? rRequest.m_nChannel == T3D_VT_LOGICAL_CHANNEL_ALBEDO : rRequest.m_nFormat == T3D_VT_FORMAT_ALBEDO_BC1_SRGB;
	if ( bAlbedo && rRequest.m_nFormat == nBc1Format && t3d_vt_godot_tile_has_cutout_alpha( rRequest ) ) {
		return t3d_vt_godot_reference_fallback( rRequest, rWrittenBytes );
	}

	PackedByteArray SourceBytes;
	SourceBytes.resize( rRequest.m_nSourceBytes );
	memcpy( SourceBytes.ptrw(), rRequest.m_pSourcePixels, rRequest.m_nSourceBytes );
	Image::Format eSourceFormat = Image::FORMAT_RGBA8;
	if ( rRequest.m_nFormat == nBc5Format ) {
		eSourceFormat = Image::FORMAT_RG8;
	}
	Ref<Image> pImage =
	        Image::create_from_data( ( int32_t )rRequest.m_nWidth, ( int32_t )rRequest.m_nHeight, false, eSourceFormat, SourceBytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return t3d_vt_godot_reference_fallback( rRequest, rWrittenBytes );
	}

	Error eError = ERR_UNAVAILABLE;
	Image::Format eExpectedFormat = Image::FORMAT_RGBA8;
	if ( rRequest.m_nFormat == nBc1Format ) {
		pImage->convert( Image::FORMAT_RGB8 );
		eError = pImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RGB );
		eExpectedFormat = Image::FORMAT_DXT1;
	} else if ( rRequest.m_nFormat == nBc5Format ) {
		pImage->convert( Image::FORMAT_RG8 );
		eError = pImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RG );
		eExpectedFormat = Image::FORMAT_RGTC_RG;
	} else if ( rRequest.m_nFormat == nBc7Format ) {
		pImage->convert( Image::FORMAT_RGBA8 );
		eError = pImage->compress( Image::COMPRESS_BPTC, Image::COMPRESS_SOURCE_GENERIC );
		eExpectedFormat = Image::FORMAT_BPTC_RGBA;
	}
	if ( eError != OK || pImage->get_format() != eExpectedFormat ) {
		return t3d_vt_godot_reference_fallback( rRequest, rWrittenBytes );
	}
	const PackedByteArray Compressed = pImage->get_data();
	const uint32_t nExpectedBytes = bV3 ? t3d_vt_pack_expected_physical_tile_bytes_v3( rRequest.m_nFormat )
	                                  : t3d_vt_pack_expected_tile_bytes( rRequest.m_nFormat );
	if ( Compressed.size() != ( int64_t )nExpectedBytes || Compressed.size() > ( int64_t )rRequest.m_nDestinationCapacity ) {
		return t3d_vt_godot_reference_fallback( rRequest, rWrittenBytes );
	}
	memcpy( rRequest.m_pDestination, Compressed.ptr(), ( size_t )Compressed.size() );
	rWrittenBytes = ( uint32_t )Compressed.size();
	return true;
}

void t3d_vt_godot_make_encoder( t3dVTEncoderInterface_t &rEncoder ) {
	rEncoder.m_pUser = NULL;
	rEncoder.m_pEncodeTile = t3d_vt_godot_encode_tile;
}


static uint64_t t3d_vt_godot_hash_bytes( uint64_t nHash, const void *const pData, const uint64_t nBytes ) {
	return t3dHashFnv1a64( pData, nBytes, nHash );
}

static uint64_t t3d_vt_godot_hash_u64( uint64_t nHash, const uint64_t nValue ) {
	return t3d_vt_godot_hash_bytes( nHash, &nValue, sizeof( nValue ) );
}

static void t3d_vt_godot_clear_source_v3( t3dVTGodotSourceV3_t &rSource ) {
	for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
		rSource.m_ChannelBytes[iChannel].clear();
		memset( &rSource.m_Channels[iChannel], 0, sizeof( rSource.m_Channels[iChannel] ) );
		memset( &rSource.m_ChannelDesc[iChannel], 0, sizeof( rSource.m_ChannelDesc[iChannel] ) );
		rSource.m_ChannelDesc[iChannel].m_nLogicalChannel = ( uint8_t )iChannel;
	}
	rSource.m_nStableKey = 0u;
	rSource.m_nAuthoredChannelMask = 0u;
	rSource.m_nSourceFlags = 0u;
	rSource.m_nOrmSourceLayout = 0u;
	rSource.m_nConsumer = 0u;
	rSource.m_eHighestAvailableProfile = T3D_VT_FIXED_PROFILE_INVALID;
}

static uint32_t t3d_vt_godot_source_extent_v3( const t3dVTCookImageView_t &rView ) {
	uint32_t nExtent = rView.m_nWidth < rView.m_nHeight ? rView.m_nWidth : rView.m_nHeight;
	if ( nExtent > T3D_VT_MAX_AUTHORED_EXTENT ) {
		nExtent = T3D_VT_MAX_AUTHORED_EXTENT;
	}
	return nExtent;
}

static uint64_t t3d_vt_godot_channel_fingerprint_v3( const t3dVTCookImageView_t &rView,
        const PackedByteArray &rBytes,
        const t3dVTLogicalChannel_t Channel ) {
	if ( rView.m_pPixels == NULL || rBytes.is_empty() ) {
		return 0u;
	}
	uint64_t nHash = 1469598103934665603ULL;
	nHash = t3d_vt_godot_hash_u64( nHash, ( uint32_t )Channel );
	nHash = t3d_vt_godot_hash_u64( nHash, rView.m_nWidth );
	nHash = t3d_vt_godot_hash_u64( nHash, rView.m_nHeight );
	nHash = t3d_vt_godot_hash_u64( nHash, rView.m_nComponents );
	nHash = t3d_vt_godot_hash_bytes( nHash, rBytes.ptr(), ( uint64_t )rBytes.size() );
	return nHash == 0u ? 1u : nHash;
}

static void t3d_vt_godot_finalize_source_channel_v3( t3dVTGodotSourceV3_t &rSource,
        const t3dVTLogicalChannel_t Channel,
        const t3dVTConsumer_t Consumer,
        const uint32_t nWrapMode,
        const bool bAlphaScissor ) {
	const uint32_t iChannel = ( uint32_t )Channel;
	t3dVTGodotSourceChannelV3_t &desc = rSource.m_ChannelDesc[iChannel];
	const t3dVTCookImageView_t &view = rSource.m_Channels[iChannel];
	desc.m_nLogicalChannel = ( uint8_t )Channel;
	desc.m_nConsumer = ( uint8_t )Consumer;
	desc.m_nWrapMode = ( uint8_t )nWrapMode;
	desc.m_bAlphaScissor = bAlphaScissor ? 1u : 0u;
	if ( view.m_pPixels == NULL || rSource.m_ChannelBytes[iChannel].is_empty() ) {
		return;
	}
	desc.m_bAuthored = 1u;
	desc.m_nSourceWidth = view.m_nWidth;
	desc.m_nSourceHeight = view.m_nHeight;
	desc.m_nSourceExtent = t3d_vt_godot_source_extent_v3( view );
	desc.m_nSourceFingerprint = t3d_vt_godot_channel_fingerprint_v3( view, rSource.m_ChannelBytes[iChannel], Channel );
	rSource.m_nAuthoredChannelMask |= T3D_VT_LOGICAL_CHANNEL_BIT( Channel );
}

static void t3d_vt_godot_resolve_source_profile_v3( t3dVTGodotSourceV3_t &rSource ) {
	uint32_t nSourceExtent[T3D_VT_LOGICAL_CHANNEL_COUNT];
	for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
		nSourceExtent[iChannel] = rSource.m_ChannelDesc[iChannel].m_nSourceExtent;
	}
	rSource.m_eHighestAvailableProfile = t3d_vt_fixed_profile_highest_supported(
	        ( t3dVTConsumer_t )rSource.m_nConsumer, rSource.m_nAuthoredChannelMask, nSourceExtent );
}

bool t3d_vt_godot_extract_instance_source_v3(
        const Ref<Terrain3DRenderingMaterial> &rMaterial, uint64_t nStableKey, t3dVTGodotSourceV3_t &rSource ) {
	t3d_vt_godot_clear_source_v3( rSource );
	if ( rMaterial.is_null() ) {
		return false;
	}
	if ( nStableKey == 0u ) {
		nStableKey = rMaterial->getVtStableKey();
	}
	if ( nStableKey == 0u ) {
		return false;
	}
	if ( !t3d_vt_godot_extract_texture( rMaterial->getAlbedoTexture(),
	             Image::FORMAT_RGBA8,
	             rSource.m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_ALBEDO],
	             rSource.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] )
	        || !t3d_vt_godot_extract_texture( rMaterial->getNormalTexture(),
	                Image::FORMAT_RG8,
	                rSource.m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_NORMAL],
	                rSource.m_Channels[T3D_VT_LOGICAL_CHANNEL_NORMAL] )
	        || !t3d_vt_godot_extract_texture( rMaterial->getSurfaceTexture(),
	                Image::FORMAT_RGBA8,
	                rSource.m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_ORM],
	                rSource.m_Channels[T3D_VT_LOGICAL_CHANNEL_ORM] ) ) {
		return false;
	}

	rSource.m_nStableKey = nStableKey;
	rSource.m_nConsumer = ( uint32_t )T3D_VT_CONSUMER_INSTANCE;
	rSource.m_nOrmSourceLayout = ( uint32_t )rMaterial->getSurfaceTextureLayout();
	if ( rSource.m_nOrmSourceLayout > ( uint32_t )SURFACE_TEXTURE_ROUGHNESS_A ) {
		return false;
	}
	if ( rMaterial->usesDirectOrmChannels() && rSource.m_nOrmSourceLayout == ( uint32_t )SURFACE_TEXTURE_ORM ) {
		rSource.m_nSourceFlags |= T3D_VT_GODOT_SOURCE_V3_DIRECT_ORM;
	}
	if ( rMaterial->isAlphaScissor() || rMaterial->getBaseColor().a < 1.0f ) {
		rSource.m_nSourceFlags |= T3D_VT_GODOT_SOURCE_V3_HAS_ALPHA;
	}
	const bool bClamp = rMaterial->getSamplerClass() == ( int32_t )T3D_VT_SAMPLER_LINEAR_CLAMP;
	const uint32_t nWrapMode = bClamp ? T3D_VT_COOK_WRAP_CLAMP : T3D_VT_COOK_WRAP_REPEAT;
	t3d_vt_godot_finalize_source_channel_v3(
	        rSource, T3D_VT_LOGICAL_CHANNEL_ALBEDO, T3D_VT_CONSUMER_INSTANCE, nWrapMode, rMaterial->isAlphaScissor() );
	t3d_vt_godot_finalize_source_channel_v3(
	        rSource, T3D_VT_LOGICAL_CHANNEL_NORMAL, T3D_VT_CONSUMER_INSTANCE, nWrapMode, false );
	t3d_vt_godot_finalize_source_channel_v3(
	        rSource, T3D_VT_LOGICAL_CHANNEL_ORM, T3D_VT_CONSUMER_INSTANCE, nWrapMode, false );
	t3d_vt_godot_resolve_source_profile_v3( rSource );
	return true;
}

bool t3d_vt_godot_extract_terrain_source_v3( const Ref<Terrain3DAssets> &rAssets,
        const uint32_t nLogicalMaterialId,
        const uint64_t nStableKey,
        t3dVTGodotSourceV3_t &rSource ) {
	t3d_vt_godot_clear_source_v3( rSource );
	if ( rAssets.is_null() || !rAssets->usesPackedPbr() || nLogicalMaterialId >= ( uint32_t )rAssets->getTextureCount()
	        || nLogicalMaterialId >= T3D_MAX_TEXTURE_ASSETS || nStableKey == 0u ) {
		return false;
	}
	const Ref<Terrain3DTextureAsset> asset = rAssets->getTextureAsset( ( int )nLogicalMaterialId );
	if ( asset.is_null() ) {
		return false;
	}
	if ( !t3d_vt_godot_extract_texture( asset->getAlbedoTexture(),
	             Image::FORMAT_RGBA8,
	             rSource.m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_ALBEDO],
	             rSource.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] )
	        || !t3d_vt_godot_extract_texture( asset->getEffectiveSurfaceTexture(),
	                Image::FORMAT_RGBA8,
	                rSource.m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_PBR],
	                rSource.m_Channels[T3D_VT_LOGICAL_CHANNEL_PBR] )
	        || !t3d_vt_godot_extract_texture( asset->getEffectiveDisplacementTexture(),
	                Image::FORMAT_R8,
	                rSource.m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_HEIGHT],
	                rSource.m_Channels[T3D_VT_LOGICAL_CHANNEL_HEIGHT] ) ) {
		return false;
	}

	rSource.m_nStableKey = nStableKey;
	rSource.m_nConsumer = ( uint32_t )T3D_VT_CONSUMER_CLIPMAP;
	t3d_vt_godot_finalize_source_channel_v3(
	        rSource, T3D_VT_LOGICAL_CHANNEL_ALBEDO, T3D_VT_CONSUMER_CLIPMAP, T3D_VT_COOK_WRAP_REPEAT, false );
	t3d_vt_godot_finalize_source_channel_v3(
	        rSource, T3D_VT_LOGICAL_CHANNEL_PBR, T3D_VT_CONSUMER_CLIPMAP, T3D_VT_COOK_WRAP_REPEAT, false );
	t3d_vt_godot_finalize_source_channel_v3(
	        rSource, T3D_VT_LOGICAL_CHANNEL_HEIGHT, T3D_VT_CONSUMER_CLIPMAP, T3D_VT_COOK_WRAP_REPEAT, false );
	t3d_vt_godot_resolve_source_profile_v3( rSource );
	return true;
}

bool t3d_vt_godot_make_sparse_cook_source_v3( const t3dVTGodotSourceV3_t &rSource, t3dVTCookSparseSourceV3_t &rCookSource ) {
	memset( &rCookSource, 0, sizeof( rCookSource ) );
	if ( rSource.m_nStableKey == 0u || rSource.m_eHighestAvailableProfile == T3D_VT_FIXED_PROFILE_INVALID
	        || rSource.m_nConsumer > T3D_VT_CONSUMER_INSTANCE || rSource.m_nOrmSourceLayout > T3D_VT_SURFACE_SOURCE_ROUGHNESS_A ) {
		return false;
	}
	rCookSource.m_nStableKey = rSource.m_nStableKey;
	rCookSource.m_nAuthoredChannelMask = rSource.m_nAuthoredChannelMask;
	rCookSource.m_nOrmSourceLayout = rSource.m_nOrmSourceLayout;
	rCookSource.m_nConsumer = rSource.m_nConsumer;
	rCookSource.m_nAlphaThreshold = 128u;
	for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
		rCookSource.m_Channels[iChannel] = rSource.m_Channels[iChannel];
		rCookSource.m_nSourceFingerprint[iChannel] = rSource.m_ChannelDesc[iChannel].m_nSourceFingerprint;
		if ( rSource.m_ChannelDesc[iChannel].m_bAuthored != 0u ) {
			rCookSource.m_nWrapMode = rSource.m_ChannelDesc[iChannel].m_nWrapMode;
			if ( rSource.m_ChannelDesc[iChannel].m_bAlphaScissor != 0u ) {
				rCookSource.m_bAlphaScissor = 1u;
			}
		}
	}
	return true;
}

static Ref<Terrain3DRenderingMaterial> t3d_vt_godot_find_material_by_stable_key(
        const Ref<Terrain3DAssets> &rAssets, const uint64_t nStableKey ) {
	if ( rAssets.is_null() || nStableKey == 0u ) {
		return Ref<Terrain3DRenderingMaterial>();
	}
	const int nMeshCount = rAssets->getMeshCount();
	for ( int iMesh = 0; iMesh < nMeshCount; ++iMesh ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( iMesh );
		if ( asset.is_null() ) {
			continue;
		}
		const Ref<Terrain3DRenderingMaterial> overrideMaterial = asset->getRenderingMaterialOverride();
		if ( overrideMaterial.is_valid() && overrideMaterial->getVtStableKey() == nStableKey ) {
			return overrideMaterial;
		}
		const Ref<Terrain3DRenderingMaterial> overlayMaterial = asset->getRenderingMaterialOverlay();
		if ( overlayMaterial.is_valid() && overlayMaterial->getVtStableKey() == nStableKey ) {
			return overlayMaterial;
		}
		const TypedArray<Terrain3DRenderingMaterial> surfaceMaterials = asset->getSurfaceRenderingMaterials();
		for ( int iMaterial = 0; iMaterial < surfaceMaterials.size(); ++iMaterial ) {
			const Ref<Terrain3DRenderingMaterial> material = surfaceMaterials[iMaterial];
			if ( material.is_valid() && material->getVtStableKey() == nStableKey ) {
				return material;
			}
		}
	}
	return Ref<Terrain3DRenderingMaterial>();
}

bool t3d_vt_godot_extract_source_by_stable_key_v3(
        const Ref<Terrain3DAssets> &rAssets, const uint64_t nStableKey, t3dVTGodotSourceV3_t &rSource, bool &rAuthored ) {
	rAuthored = false;
	if ( rAssets.is_null() || nStableKey == 0u ) {
		t3d_vt_godot_clear_source_v3( rSource );
		return false;
	}
	uint32_t nLogicalMaterialId = 0u;
	if ( t3d_vt_decode_terrain_material_stable_key( nStableKey, nLogicalMaterialId ) ) {
		rAuthored = true;
		return t3d_vt_godot_extract_terrain_source_v3( rAssets, nLogicalMaterialId, nStableKey, rSource );
	}
	const Ref<Terrain3DRenderingMaterial> material = t3d_vt_godot_find_material_by_stable_key( rAssets, nStableKey );
	if ( material.is_valid() ) {
		rAuthored = true;
		return t3d_vt_godot_extract_instance_source_v3( material, nStableKey, rSource );
	}
	t3d_vt_godot_clear_source_v3( rSource );
	rSource.m_nStableKey = nStableKey;
	return true;
}
struct t3dVTEditorAllocationBudget_t {
	uint64_t m_nLimit = T3D_VT_COOK_WORKING_BYTE_LIMIT;
	uint64_t m_nLiveBytes = 0u;
};
struct t3dVTEditorAllocationHeader_t {
	void *m_pBase;
	uint64_t m_nChargedBytes;
};
static void *t3d_vt_godot_editor_allocate( void *pUser, const uint64_t nBytes, const uint32_t nAlignment ) {
	if ( nBytes == 0u || nBytes > SIZE_MAX || nAlignment == 0u || ( nAlignment & ( nAlignment - 1u ) ) != 0u ) { return NULL; }
	const uint32_t nEffectiveAlignment = MAX( nAlignment, ( uint32_t )alignof( t3dVTEditorAllocationHeader_t ) );
	const uint64_t nExtra = ( uint64_t )nEffectiveAlignment - 1u + sizeof( t3dVTEditorAllocationHeader_t );
	if ( nBytes > SIZE_MAX - nExtra ) { return NULL; }
	t3dVTEditorAllocationBudget_t *const pBudget = ( t3dVTEditorAllocationBudget_t * )pUser;
	if ( pBudget != NULL && ( pBudget->m_nLiveBytes > pBudget->m_nLimit || nBytes + nExtra > pBudget->m_nLimit - pBudget->m_nLiveBytes ) ) { return NULL; }
	void *const pBase = t3dMemoryAllocate( ( size_t )( nBytes + nExtra ) );
	if ( pBase == NULL ) { return NULL; }
	const uintptr_t nStart = ( uintptr_t )pBase + sizeof( t3dVTEditorAllocationHeader_t );
	const uintptr_t nAligned = ( nStart + nEffectiveAlignment - 1u ) & ~( ( uintptr_t )nEffectiveAlignment - 1u );
	t3dVTEditorAllocationHeader_t &header = ( ( t3dVTEditorAllocationHeader_t * )nAligned )[-1];
	header.m_pBase = pBase;
	header.m_nChargedBytes = nBytes + nExtra;
	if ( pBudget != NULL ) { pBudget->m_nLiveBytes += header.m_nChargedBytes; }
	return ( void * )nAligned;
}

static void t3d_vt_godot_editor_free( void *pUser, void *pMemory ) {
	if ( pMemory != NULL ) {
		const t3dVTEditorAllocationHeader_t &header = ( ( t3dVTEditorAllocationHeader_t * )pMemory )[-1];
		t3dVTEditorAllocationBudget_t *const pBudget = ( t3dVTEditorAllocationBudget_t * )pUser;
		if ( pBudget != NULL ) { pBudget->m_nLiveBytes -= header.m_nChargedBytes; }
		t3dMemoryRelease( header.m_pBase );
	}
}

#define T3D_VT_EDITOR_CHANNEL_CACHE_MAGIC 0x38484356u
#define T3D_VT_EDITOR_CHANNEL_CACHE_VERSION 1u

// t3dVTEditorChannelCacheDiskHeaderV3_s centralized in Terrain3DTypes.h.


typedef struct t3dVTEditorFragmentV3_s {

	uint64_t m_nStableKey;
	uint64_t m_nKey;
	PackedByteArray m_Bytes;
} t3dVTEditorFragmentV3_t;


typedef struct t3dVTEditorUniquePayloadV3_s {

	t3dVTPackPayloadRecordV3_t m_Record;
	PackedByteArray m_Bytes;
} t3dVTEditorUniquePayloadV3_t;


// t3dVTEditorTimedEncoderV3_s centralized in Terrain3DTypes.h.


static uint64_t t3d_vt_godot_now_usec_v3( void ) {
	Time *const pTime = Time::get_singleton();
	return pTime ? pTime->get_ticks_usec() : 0u;
}

static String t3d_vt_godot_cache_key_text_v3( const uint64_t nKey ) {
	return String::num_uint64( nKey, 16 );
}

static String t3d_vt_godot_channel_cache_path_v3( const t3dVTEditorCookCacheV3_t &rCache, const uint64_t nKey ) {
	return rCache.m_CacheDirectory.is_empty() ? String()
	                                          : rCache.m_CacheDirectory.path_join( String( "channel_" ) + t3d_vt_godot_cache_key_text_v3( nKey ) + ".bin" );
}

static String t3d_vt_godot_allocation_cache_path_v3( const t3dVTEditorCookCacheV3_t &rCache, const uint64_t nKey ) {
	return rCache.m_CacheDirectory.is_empty() ? String()
	                                          : rCache.m_CacheDirectory.path_join( String( "allocation_" ) + t3d_vt_godot_cache_key_text_v3( nKey ) + ".t3dvt" );
}

static bool t3d_vt_godot_write_cache_atomic_v3( const String &rPath, const PackedByteArray &rBytes ) {
	if ( rPath.is_empty() || rBytes.is_empty() ) {
		return false;
	}
	const String temporary = rPath + String( ".tmp" );
	const String backup = rPath + String( ".bak" );
	Terrain3DFilePublication publication( rPath, temporary, backup );
	if ( publication.prepare() != OK ) {
		return false;
	}
	if ( FileAccess::file_exists( temporary ) ) {
		DirAccess::remove_absolute( temporary );
	}
	Ref<FileAccess> file = FileAccess::open( temporary, FileAccess::WRITE );
	if ( file.is_null() ) {
		return false;
	}
	file->store_buffer( rBytes );
	const Error eError = file->get_error();
	file->close();
	if ( eError != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	return publication.publish() == OK && publication.commit() == OK;
}

static bool t3d_vt_godot_timed_encode_tile_v3(
        void *pUser, const t3dVTEncodeTileRequest_t &rRequest, uint32_t &rWrittenBytes ) {
	t3dVTEditorTimedEncoderV3_t *const pTimed = ( t3dVTEditorTimedEncoderV3_t * )pUser;
	if ( pTimed == NULL || pTimed->m_Base.m_pEncodeTile == NULL ) {
		return false;
	}
	const uint64_t nStart = t3d_vt_godot_now_usec_v3();
	const bool bOk = pTimed->m_Base.m_pEncodeTile( pTimed->m_Base.m_pUser, rRequest, rWrittenBytes );
	const uint64_t nEnd = t3d_vt_godot_now_usec_v3();
	if ( nEnd >= nStart ) {
		pTimed->m_nEncodeUsec += nEnd - nStart;
	}
	return bOk;
}

void t3d_vt_godot_editor_cache_reset_v3( t3dVTEditorCookCacheV3_t &rCache ) {
	rCache.m_ChannelEntries.clear();
	rCache.m_AllocationEntries.clear();
	rCache.m_nLifetimeHits = 0u;
	rCache.m_nLifetimeMisses = 0u;
}

void t3d_vt_godot_editor_cache_set_directory_v3( t3dVTEditorCookCacheV3_t &rCache, const String &rDirectory ) {
	rCache.m_CacheDirectory = rDirectory;
	if ( !rDirectory.is_empty() ) {
		DirAccess::make_dir_recursive_absolute( rDirectory );
	}
}

static t3dVTEditorChannelCacheEntryV3_t *t3d_vt_godot_find_channel_cache_v3(
        t3dVTEditorCookCacheV3_t &rCache, const uint64_t nKey, const uint32_t nChannel, const uint32_t nTargetExtent ) {
	for ( size_t iEntry = 0u; iEntry < rCache.m_ChannelEntries.size(); ++iEntry ) {
		t3dVTEditorChannelCacheEntryV3_t &rEntry = rCache.m_ChannelEntries[iEntry];
		if ( rEntry.m_nKey == nKey && rEntry.m_nLogicalChannel == nChannel && rEntry.m_nTargetExtent == nTargetExtent
		        && !rEntry.m_Bytes.is_empty() ) {
			return &rEntry;
		}
	}
	return NULL;
}

static bool t3d_vt_godot_load_channel_cache_disk_v3( t3dVTEditorCookCacheV3_t &rCache,
        const uint64_t nKey,
        const uint32_t nChannel,
        const uint32_t nTargetExtent,
        t3dVTEditorChannelCacheEntryV3_t *&rpEntry ) {
	rpEntry = NULL;
	const String path = t3d_vt_godot_channel_cache_path_v3( rCache, nKey );
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const PackedByteArray fileBytes = FileAccess::get_file_as_bytes( path );
	if ( fileBytes.size() < ( int64_t )sizeof( t3dVTEditorChannelCacheDiskHeaderV3_t ) ) {
		return false;
	}
	t3dVTEditorChannelCacheDiskHeaderV3_t header;
	memcpy( &header, fileBytes.ptr(), sizeof( header ) );
	if ( header.m_nMagic != T3D_VT_EDITOR_CHANNEL_CACHE_MAGIC || header.m_nVersion != T3D_VT_EDITOR_CHANNEL_CACHE_VERSION
	        || header.m_nKey != nKey || header.m_nLogicalChannel != nChannel || header.m_nTargetExtent != nTargetExtent
	        || header.m_nPayloadBytes == 0u || header.m_nPayloadBytes > INT64_MAX
	        || ( uint64_t )fileBytes.size() != sizeof( header ) + header.m_nPayloadBytes
	        || header.m_Layout.m_nTotalBytes != header.m_nPayloadBytes || header.m_Layout.m_nMipCount == 0u
	        || header.m_Layout.m_nMipCount > T3D_VT_MAX_MIPS ) {
		return false;
	}
	const uint8_t *const pPayload = fileBytes.ptr() + sizeof( header );
	if ( t3d_vt_pack_checksum32( pPayload, header.m_nPayloadBytes ) != header.m_nPayloadChecksum ) {
		return false;
	}
	t3dVTEditorChannelCacheEntryV3_t entry;
	entry.m_nKey = nKey;
	entry.m_nLogicalChannel = nChannel;
	entry.m_nTargetExtent = nTargetExtent;
	entry.m_Layout = header.m_Layout;
	entry.m_Bytes.resize( ( int64_t )header.m_nPayloadBytes );
	if ( entry.m_Bytes.size() != ( int64_t )header.m_nPayloadBytes ) {
		return false;
	}
	memcpy( entry.m_Bytes.ptrw(), pPayload, ( size_t )header.m_nPayloadBytes );
	rCache.m_ChannelEntries.push_back( entry );
	rpEntry = &rCache.m_ChannelEntries.back();
	return true;
}

static void t3d_vt_godot_store_channel_cache_disk_v3(
        const t3dVTEditorCookCacheV3_t &rCache, const t3dVTEditorChannelCacheEntryV3_t &rEntry ) {
	const String path = t3d_vt_godot_channel_cache_path_v3( rCache, rEntry.m_nKey );
	if ( path.is_empty() || rEntry.m_Bytes.is_empty() ) {
		return;
	}
	t3dVTEditorChannelCacheDiskHeaderV3_t header;
	memset( &header, 0, sizeof( header ) );
	header.m_nMagic = T3D_VT_EDITOR_CHANNEL_CACHE_MAGIC;
	header.m_nVersion = T3D_VT_EDITOR_CHANNEL_CACHE_VERSION;
	header.m_nLogicalChannel = rEntry.m_nLogicalChannel;
	header.m_nTargetExtent = rEntry.m_nTargetExtent;
	header.m_nKey = rEntry.m_nKey;
	header.m_Layout = rEntry.m_Layout;
	header.m_nPayloadBytes = ( uint64_t )rEntry.m_Bytes.size();
	header.m_nPayloadChecksum = t3d_vt_pack_checksum32( rEntry.m_Bytes.ptr(), header.m_nPayloadBytes );
	PackedByteArray fileBytes;
	fileBytes.resize( ( int64_t )( sizeof( header ) + header.m_nPayloadBytes ) );
	if ( fileBytes.size() != ( int64_t )( sizeof( header ) + header.m_nPayloadBytes ) ) {
		return;
	}
	memcpy( fileBytes.ptrw(), &header, sizeof( header ) );
	memcpy( fileBytes.ptrw() + sizeof( header ), rEntry.m_Bytes.ptr(), ( size_t )header.m_nPayloadBytes );
	( void )t3d_vt_godot_write_cache_atomic_v3( path, fileBytes );
}

static t3dVTEditorAllocationCacheEntryV3_t *t3d_vt_godot_find_allocation_cache_v3(
        t3dVTEditorCookCacheV3_t &rCache, const uint64_t nStableKey, const uint64_t nKey ) {
	for ( size_t iEntry = 0u; iEntry < rCache.m_AllocationEntries.size(); ++iEntry ) {
		t3dVTEditorAllocationCacheEntryV3_t &rEntry = rCache.m_AllocationEntries[iEntry];
		if ( rEntry.m_nStableKey == nStableKey && rEntry.m_nKey == nKey && !rEntry.m_PackageBytes.is_empty() ) {
			return &rEntry;
		}
	}
	return NULL;
}

static bool t3d_vt_godot_validate_allocation_fragment_v3( const PackedByteArray &rBytes, const uint64_t nStableKey ) {
	if ( rBytes.is_empty() ) {
		return false;
	}
	t3dVTPackViewV3_t view;
	return t3d_vt_pack_open_view_v3( rBytes.ptr(), ( uint64_t )rBytes.size(), view ) && view.m_pHeader->m_nAllocationCount == 1u
	        && view.m_pAllocationKeys[0].m_nStableKey == nStableKey;
}

static bool t3d_vt_godot_load_allocation_cache_disk_v3( t3dVTEditorCookCacheV3_t &rCache,
        const uint64_t nStableKey,
        const uint64_t nKey,
        t3dVTEditorAllocationCacheEntryV3_t *&rpEntry ) {
	rpEntry = NULL;
	const String path = t3d_vt_godot_allocation_cache_path_v3( rCache, nKey );
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const PackedByteArray bytes = FileAccess::get_file_as_bytes( path );
	if ( !t3d_vt_godot_validate_allocation_fragment_v3( bytes, nStableKey ) ) {
		return false;
	}
	t3dVTEditorAllocationCacheEntryV3_t entry;
	entry.m_nStableKey = nStableKey;
	entry.m_nKey = nKey;
	entry.m_PackageBytes = bytes;
	rCache.m_AllocationEntries.push_back( entry );
	rpEntry = &rCache.m_AllocationEntries.back();
	return true;
}

static void t3d_vt_godot_store_allocation_cache_disk_v3(
        const t3dVTEditorCookCacheV3_t &rCache, const t3dVTEditorAllocationCacheEntryV3_t &rEntry ) {
	const String path = t3d_vt_godot_allocation_cache_path_v3( rCache, rEntry.m_nKey );
	if ( !path.is_empty() && !rEntry.m_PackageBytes.is_empty() ) {
		( void )t3d_vt_godot_write_cache_atomic_v3( path, rEntry.m_PackageBytes );
	}
}

static uint64_t t3d_vt_godot_channel_cache_key_v3( const t3dVTEditorCookCacheV3_t &rCache,
        const t3dVTGodotSourceV3_t &rSource,
        const t3dVTLogicalChannel_t Channel,
        const uint32_t nTargetExtent,
        const uint32_t nCompressionQuality ) {
	const t3dVTGodotSourceChannelV3_t &rDesc = rSource.m_ChannelDesc[( uint32_t )Channel];
	uint64_t nHash = 1469598103934665603ULL;
	nHash = t3d_vt_godot_hash_u64( nHash, rDesc.m_nSourceFingerprint );
	nHash = t3d_vt_godot_hash_u64( nHash, ( uint32_t )Channel );
	nHash = t3d_vt_godot_hash_u64( nHash, rSource.m_nConsumer );
	nHash = t3d_vt_godot_hash_u64( nHash, nTargetExtent );
	nHash = t3d_vt_godot_hash_u64( nHash, rDesc.m_nWrapMode );
	nHash = t3d_vt_godot_hash_u64( nHash, rDesc.m_bAlphaScissor );
	nHash = t3d_vt_godot_hash_u64( nHash, T3D_VT_PAGE_INTERIOR );
	nHash = t3d_vt_godot_hash_u64( nHash, T3D_VT_PAGE_GUTTER );
	nHash = t3d_vt_godot_hash_u64( nHash, rCache.m_nFilterAbi );
	nHash = t3d_vt_godot_hash_u64( nHash, rCache.m_nCompressorAbi );
	nHash = t3d_vt_godot_hash_u64( nHash, nCompressionQuality );
	return nHash == 0u ? 1u : nHash;
}

static uint64_t t3d_vt_godot_allocation_cache_key_v3( const t3dVTGodotSourceV3_t &rSource,
        const uint64_t nChannelKeys[T3D_VT_LOGICAL_CHANNEL_COUNT],
        const t3dVTCookerSettings_t &rSettings,
        const uint32_t nAllocationFlags ) {
	uint64_t nHash = 1469598103934665603ULL;
	nHash = t3d_vt_godot_hash_u64( nHash, rSource.m_nStableKey );
	nHash = t3d_vt_godot_hash_u64( nHash, rSource.m_nAuthoredChannelMask );
	nHash = t3d_vt_godot_hash_u64( nHash, rSource.m_nConsumer );
	nHash = t3d_vt_godot_hash_u64( nHash, ( uint32_t )rSource.m_eHighestAvailableProfile );
	nHash = t3d_vt_godot_hash_u64( nHash, rSource.m_nSourceFlags );
	nHash = t3d_vt_godot_hash_u64( nHash, rSource.m_nOrmSourceLayout );
	nHash = t3d_vt_godot_hash_u64( nHash, rSettings.m_nMipQuality );
	nHash = t3d_vt_godot_hash_u64( nHash, rSettings.m_nCompressionQuality );
	nHash = t3d_vt_godot_hash_u64( nHash, rSettings.m_nOuterCompression );
	nHash = t3d_vt_godot_hash_u64( nHash, nAllocationFlags );
	for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
		nHash = t3d_vt_godot_hash_u64( nHash, nChannelKeys[iChannel] );
	}
	return nHash == 0u ? 1u : nHash;
}

static bool t3d_vt_godot_prepare_channel_cache_v3( t3dVTEditorCookCacheV3_t &rCache,
        const t3dVTGodotSourceV3_t &rSource,
        t3dVTCookSparseSourceV3_t &rCookSource,
        const t3dVTLogicalChannel_t Channel,
        const t3dVTCookerSettings_t &rSettings,
        const t3dVTCookAllocator_t &rAllocator,
        t3dVTEditorCookTelemetryV3_t &rTelemetry,
        uint64_t &rChannelKey ) {
	const uint32_t nTargetExtent = t3d_vt_fixed_profile_channel_extent( rSource.m_eHighestAvailableProfile, Channel );
	if ( nTargetExtent == 0u || ( rSource.m_nAuthoredChannelMask & T3D_VT_LOGICAL_CHANNEL_BIT( Channel ) ) == 0u ) {
		rChannelKey = 0u;
		return true;
	}
	rChannelKey = t3d_vt_godot_channel_cache_key_v3( rCache, rSource, Channel, nTargetExtent, rSettings.m_nCompressionQuality );
	t3dVTEditorChannelCacheEntryV3_t *pEntry = t3d_vt_godot_find_channel_cache_v3( rCache, rChannelKey, ( uint32_t )Channel, nTargetExtent );
	if ( pEntry == NULL ) {
		( void )t3d_vt_godot_load_channel_cache_disk_v3( rCache, rChannelKey, ( uint32_t )Channel, nTargetExtent, pEntry );
	}
	if ( pEntry != NULL ) {
		++rTelemetry.m_nChannelCacheHits;
		++rCache.m_nLifetimeHits;
	} else {
		++rTelemetry.m_nChannelCacheMisses;
		++rCache.m_nLifetimeMisses;
		rTelemetry.m_nStage = T3D_VT_EDITOR_COOK_STAGE_FILTER;
		const uint64_t nStart = t3d_vt_godot_now_usec_v3();
		uint8_t *pMipBytes = NULL;
		t3dVTMipChainLayout_t layout;
		if ( !t3d_vt_cooker_build_channel_pyramid_v3(
		             rAllocator, rCookSource, rSource.m_eHighestAvailableProfile, Channel, pMipBytes, layout )
		        || pMipBytes == NULL || layout.m_nTotalBytes == 0u || layout.m_nTotalBytes > INT64_MAX ) {
			if ( pMipBytes != NULL ) {
				rAllocator.m_pFree( rAllocator.m_pUser, pMipBytes );
			}
			return false;
		}
		const uint64_t nEnd = t3d_vt_godot_now_usec_v3();
		if ( nEnd >= nStart ) {
			rTelemetry.m_nFilterUsec += nEnd - nStart;
		}
		t3dVTEditorChannelCacheEntryV3_t entry;
		entry.m_nKey = rChannelKey;
		entry.m_nTargetExtent = nTargetExtent;
		entry.m_nLogicalChannel = ( uint32_t )Channel;
		entry.m_Layout = layout;
		entry.m_Bytes.resize( ( int64_t )layout.m_nTotalBytes );
		if ( entry.m_Bytes.size() != ( int64_t )layout.m_nTotalBytes ) {
			rAllocator.m_pFree( rAllocator.m_pUser, pMipBytes );
			return false;
		}
		memcpy( entry.m_Bytes.ptrw(), pMipBytes, ( size_t )layout.m_nTotalBytes );
		rAllocator.m_pFree( rAllocator.m_pUser, pMipBytes );
		rCache.m_ChannelEntries.push_back( entry );
		pEntry = &rCache.m_ChannelEntries.back();
		t3d_vt_godot_store_channel_cache_disk_v3( rCache, *pEntry );
	}
	rCookSource.m_PreparedChannels[( uint32_t )Channel].m_pMipBytes = pEntry->m_Bytes.ptr();
	rCookSource.m_PreparedChannels[( uint32_t )Channel].m_Layout = pEntry->m_Layout;
	rCookSource.m_PreparedChannels[( uint32_t )Channel].m_nTargetExtent = pEntry->m_nTargetExtent;
	return true;
}

static bool t3d_vt_godot_cook_allocation_fragment_v3( const Ref<Terrain3DAssets> &rAssets,
        const uint64_t nStableKey,
        t3dVTEditorCookCacheV3_t &rCache,
        const t3dVTCookerSettings_t &rSettings,
        const t3dVTCookAllocator_t &rAllocator,
        t3dVTEditorCookTelemetryV3_t &rTelemetry,
        const bool bShippingImmutable,
        PackedByteArray &rFragmentBytes,
        uint64_t &rAllocationKey,
        String *const pError ) {
	rFragmentBytes.clear();
	rAllocationKey = 0u;
	rTelemetry.m_nStage = T3D_VT_EDITOR_COOK_STAGE_EXTRACT;
	const uint64_t nExtractStart = t3d_vt_godot_now_usec_v3();
	t3dVTGodotSourceV3_t source;
	bool bAuthored = false;
	if ( !t3d_vt_godot_extract_source_by_stable_key_v3( rAssets, nStableKey, source, bAuthored ) || !bAuthored
	        || source.m_eHighestAvailableProfile == T3D_VT_FIXED_PROFILE_INVALID ) {
		if ( pError ) {
			*pError = "VT V3 cooker could not extract an authored source";
		}
		return false;
	}
	const uint64_t nExtractEnd = t3d_vt_godot_now_usec_v3();
	if ( nExtractEnd >= nExtractStart ) {
		rTelemetry.m_nExtractUsec += nExtractEnd - nExtractStart;
	}
	t3dVTCookSparseSourceV3_t cookSource;
	if ( !t3d_vt_godot_make_sparse_cook_source_v3( source, cookSource ) ) {
		return false;
	}
	uint32_t nAllocationFlags = T3D_VT_ALLOCATION_MATERIAL_UV;
	if ( bShippingImmutable ) {
		nAllocationFlags |= T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE;
		nAllocationFlags |= cookSource.m_nWrapMode == T3D_VT_COOK_WRAP_CLAMP ? T3D_VT_ALLOCATION_CLAMP : T3D_VT_ALLOCATION_REPEAT;
		if ( ( source.m_nSourceFlags & T3D_VT_GODOT_SOURCE_V3_HAS_ALPHA ) != 0u ) {
			nAllocationFlags |= T3D_VT_ALLOCATION_HAS_ALPHA;
		}
		if ( ( source.m_nAuthoredChannelMask & T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_HEIGHT ) ) != 0u ) {
			nAllocationFlags |= T3D_VT_ALLOCATION_HAS_HEIGHT;
		}
	} else {
		nAllocationFlags |= T3D_VT_ALLOCATION_REPEAT | T3D_VT_ALLOCATION_EDITOR_DYNAMIC;
	}
	cookSource.m_nAllocationFlags = nAllocationFlags;
	uint64_t channelKeys[T3D_VT_LOGICAL_CHANNEL_COUNT];
	memset( channelKeys, 0, sizeof( channelKeys ) );
	for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
		const t3dVTLogicalChannel_t Channel = ( t3dVTLogicalChannel_t )iChannel;
		const uint32_t nTargetExtent = t3d_vt_fixed_profile_channel_extent( source.m_eHighestAvailableProfile, Channel );
		if ( nTargetExtent != 0u && ( source.m_nAuthoredChannelMask & T3D_VT_LOGICAL_CHANNEL_BIT( Channel ) ) != 0u ) {
			channelKeys[iChannel] = t3d_vt_godot_channel_cache_key_v3(
			        rCache, source, Channel, nTargetExtent, rSettings.m_nCompressionQuality );
		}
	}
	rAllocationKey = t3d_vt_godot_allocation_cache_key_v3( source, channelKeys, rSettings, nAllocationFlags );
	cookSource.m_nGenerationSeed = ( uint32_t )( rAllocationKey ^ ( rAllocationKey >> 32u ) );
	if ( cookSource.m_nGenerationSeed == 0u ) {
		cookSource.m_nGenerationSeed = 1u;
	}
	t3dVTEditorAllocationCacheEntryV3_t *pAllocationCache = t3d_vt_godot_find_allocation_cache_v3( rCache, nStableKey, rAllocationKey );
	if ( pAllocationCache == NULL ) {
		( void )t3d_vt_godot_load_allocation_cache_disk_v3( rCache, nStableKey, rAllocationKey, pAllocationCache );
	}
	if ( pAllocationCache != NULL ) {
		++rTelemetry.m_nAllocationCacheHits;
		++rCache.m_nLifetimeHits;
		rFragmentBytes = pAllocationCache->m_PackageBytes;
		return true;
	}
	++rTelemetry.m_nAllocationCacheMisses;
	++rTelemetry.m_nDirtyAllocations;
	++rCache.m_nLifetimeMisses;
	for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
		uint64_t nPreparedKey = 0u;
		if ( !t3d_vt_godot_prepare_channel_cache_v3( rCache,
		             source,
		             cookSource,
		             ( t3dVTLogicalChannel_t )iChannel,
		             rSettings,
		             rAllocator,
		             rTelemetry,
		             nPreparedKey )
		        || nPreparedKey != channelKeys[iChannel] ) {
			if ( pError ) {
				*pError = "VT V3 cooker could not prepare a channel pyramid";
			}
			return false;
		}
	}
	t3dVTEditorTimedEncoderV3_t timedEncoder;
	memset( &timedEncoder, 0, sizeof( timedEncoder ) );
	t3d_vt_godot_make_encoder( timedEncoder.m_Base );
	t3dVTEncoderInterface_t encoder;
	encoder.m_pUser = &timedEncoder;
	encoder.m_pEncodeTile = &t3d_vt_godot_timed_encode_tile_v3;
	t3dVTCookedSparsePackageV3_t package;
	rTelemetry.m_nStage = T3D_VT_EDITOR_COOK_STAGE_PAGE;
	const uint64_t nCookStart = t3d_vt_godot_now_usec_v3();
	const bool bCooked = t3d_vt_cooker_cook_sparse_pages_v3( rSettings, &cookSource, 1u, encoder, rAllocator, NULL, package );
	const uint64_t nCookEnd = t3d_vt_godot_now_usec_v3();
	rTelemetry.m_nEncodeUsec += timedEncoder.m_nEncodeUsec;
	if ( nCookEnd >= nCookStart ) {
		const uint64_t nTotal = nCookEnd - nCookStart;
		rTelemetry.m_nPageUsec += nTotal > timedEncoder.m_nEncodeUsec ? nTotal - timedEncoder.m_nEncodeUsec : 0u;
	}
	if ( !bCooked ) {
		if ( pError ) {
			*pError = "VT V3 cooker failed to cook an allocation";
		}
		return false;
	}
	t3dVTPackSourceV3_t packSource;
	if ( !t3d_vt_cooker_build_pack_source_v3( package, packSource ) ) {
		t3d_vt_cooker_release_sparse_package_v3( package );
		return false;
	}
	const uint64_t nBytes = t3d_vt_pack_compute_bytes_v3( packSource );
	if ( nBytes == 0u || nBytes > INT64_MAX ) {
		t3d_vt_cooker_release_sparse_package_v3( package );
		return false;
	}
	rFragmentBytes.resize( ( int64_t )nBytes );
	uint64_t nWritten = 0u;
	const bool bWritten = rFragmentBytes.size() == ( int64_t )nBytes
	        && t3d_vt_pack_write_v3( packSource, rFragmentBytes.ptrw(), nBytes, nWritten ) && nWritten == nBytes;
	t3d_vt_cooker_release_sparse_package_v3( package );
	if ( !bWritten || !t3d_vt_godot_validate_allocation_fragment_v3( rFragmentBytes, nStableKey ) ) {
		rFragmentBytes.clear();
		return false;
	}
	t3dVTEditorAllocationCacheEntryV3_t entry;
	entry.m_nStableKey = nStableKey;
	entry.m_nKey = rAllocationKey;
	entry.m_PackageBytes = rFragmentBytes;
	for ( size_t iEntry = 0u; iEntry < rCache.m_AllocationEntries.size(); ++iEntry ) {
		if ( rCache.m_AllocationEntries[iEntry].m_nStableKey == nStableKey ) {
			rCache.m_AllocationEntries[iEntry] = entry;
			t3d_vt_godot_store_allocation_cache_disk_v3( rCache, entry );
			return true;
		}
	}
	rCache.m_AllocationEntries.push_back( entry );
	t3d_vt_godot_store_allocation_cache_disk_v3( rCache, rCache.m_AllocationEntries.back() );
	return true;
}

static void t3d_vt_godot_sort_fragments_v3(
        eastl::vector<t3dVTEditorFragmentV3_t, Terrain3DEastlAllocator> &rFragments ) {
	for ( size_t i = 1u; i < rFragments.size(); ++i ) {
		t3dVTEditorFragmentV3_t value = rFragments[i];
		size_t j = i;
		while ( j > 0u && rFragments[j - 1u].m_nStableKey > value.m_nStableKey ) {
			rFragments[j] = rFragments[j - 1u];
			--j;
		}
		rFragments[j] = value;
	}
}

static bool t3d_vt_godot_payload_bytes_v3( const t3dVTPackViewV3_t &rView,
        const uint32_t nPayloadId,
        const uint8_t *&rpBytes,
        uint32_t &rBytes ) {
	rpBytes = NULL;
	rBytes = 0u;
	if ( nPayloadId >= rView.m_pHeader->m_nPayloadCount ) {
		return false;
	}
	const t3dVTPackPayloadRecordV3_t &rPayload = rView.m_pPayloads[nPayloadId];
	if ( rPayload.m_nClusterId >= rView.m_pHeader->m_nClusterCount || rPayload.m_nCodec != T3D_VT_PACK_CODEC_NONE
	        || rPayload.m_nStoredBytes != rPayload.m_nDecodedBytes ) {
		return false;
	}
	const t3dVTPackClusterRecordV3_t &rCluster = rView.m_pClusters[rPayload.m_nClusterId];
	if ( rCluster.m_nCodec != T3D_VT_PACK_CODEC_NONE || rPayload.m_nClusterOffset > rCluster.m_nDecodedBytes
	        || rPayload.m_nDecodedBytes > rCluster.m_nDecodedBytes - rPayload.m_nClusterOffset ) {
		return false;
	}
	rpBytes = rView.m_pPayloadData + rCluster.m_nPayloadDataOffset + rPayload.m_nClusterOffset;
	rBytes = rPayload.m_nDecodedBytes;
	return true;
}

struct t3dVTEditorPayloadIndexV3_t {
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_heads;
	eastl::vector<uint32_t, Terrain3DEastlAllocator> m_next;
	uint64_t m_nComparisons = 0u;
	bool prepare( const uint32_t nCount ) {
		uint64_t nBuckets = 1u;
		while ( nBuckets < ( uint64_t )nCount * 2u ) { nBuckets *= 2u; }
		return t3dVectorResizeCold( m_heads, ( size_t )nBuckets, UINT32_MAX )
		        && t3dContainerReserveCold( m_next, nCount );
	}
};

static uint32_t t3d_vt_godot_find_or_add_payload_v3(
        eastl::vector<t3dVTEditorUniquePayloadV3_t, Terrain3DEastlAllocator> &rPayloads,
        t3dVTEditorPayloadIndexV3_t &rIndex,
        const t3dVTPackPayloadRecordV3_t &rPayload,
        const uint8_t *const pBytes,
        const uint32_t nBytes ) {
	// Hash actual bytes, not just the supplied signature. Equality still checks
	// metadata and bytes so a hash collision never merges distinct payloads.
	uint64_t nHash = t3d_vt_pack_hash64( pBytes, nBytes ) * 1099511628211ull + rPayload.m_nContentSignature;
	nHash ^= ( uint64_t )rPayload.m_nFormat << 32u;
	nHash ^= rPayload.m_nFlags;
	nHash ^= nHash >> 30u;
	nHash *= 0xbf58476d1ce4e5b9ull;
	nHash ^= nHash >> 27u;
	nHash *= 0x94d049bb133111ebull;
	nHash ^= nHash >> 31u;
	const size_t nBucket = ( size_t )( nHash & ( rIndex.m_heads.size() - 1u ) );
	for ( uint32_t iPayload = rIndex.m_heads[nBucket]; iPayload != UINT32_MAX; iPayload = rIndex.m_next[iPayload] ) {
		++rIndex.m_nComparisons;
		const t3dVTEditorUniquePayloadV3_t &rExisting = rPayloads[iPayload];
		if ( rExisting.m_Record.m_nContentSignature == rPayload.m_nContentSignature && rExisting.m_Record.m_nFormat == rPayload.m_nFormat
		        && rExisting.m_Record.m_nFlags == rPayload.m_nFlags && rExisting.m_Bytes.size() == nBytes
		        && memcmp( rExisting.m_Bytes.ptr(), pBytes, nBytes ) == 0 ) {
			return iPayload;
		}
	}
	if ( rPayloads.size() == rPayloads.capacity() || rIndex.m_next.size() == rIndex.m_next.capacity() ) { return UINT32_MAX; }
	t3dVTEditorUniquePayloadV3_t entry;
	entry.m_Record = rPayload;
	entry.m_Record.m_nClusterId = 0u;
	entry.m_Record.m_nClusterOffset = 0u;
	entry.m_Record.m_nCodec = T3D_VT_PACK_CODEC_NONE;
	entry.m_Record.m_nStoredBytes = nBytes;
	entry.m_Record.m_nDecodedBytes = nBytes;
	if ( entry.m_Bytes.resize( nBytes ) != OK ) {
		return UINT32_MAX;
	}
	memcpy( entry.m_Bytes.ptrw(), pBytes, nBytes );
	rPayloads.push_back( entry );
	const uint32_t nId = ( uint32_t )rPayloads.size() - 1u;
	rIndex.m_next.push_back( rIndex.m_heads[nBucket] );
	rIndex.m_heads[nBucket] = nId;
	return nId;
}

struct t3dVTEditorAllocationInputV3_t {
	uint32_t nFragment;
	uint32_t nAllocation;
	uint64_t nStableKey;
};

static bool t3dVTAllocationInputLess( const t3dVTEditorAllocationInputV3_t &a, const t3dVTEditorAllocationInputV3_t &b ) {
	return a.nStableKey < b.nStableKey;
}

struct t3dVTEditorMergePlanV3_t {
	uint64_t m_nAllocations = 0u, m_nPages = 0u, m_nTails = 0u, m_nViews = 0u, m_nPayloads = 0u;
	uint64_t m_nInputBytes = 0u, m_nWorkingBytes = 0u;
};

static bool t3dVTMergeAddBytes( uint64_t &rBytes, const uint64_t nCount, const uint64_t nStride ) {
	uint64_t nBytes = 0u;
	return t3dCheckedMultiply( nCount, nStride, nBytes ) && t3dCheckedAdd( rBytes, nBytes, rBytes );
}

static bool t3dVTPlanMergeV3( const eastl::vector<t3dVTEditorFragmentV3_t, Terrain3DEastlAllocator> &rFragments,
        eastl::vector<t3dVTPackViewV3_t, Terrain3DEastlAllocator> &rViews, t3dVTEditorMergePlanV3_t &rPlan,
        const uint64_t nWorkingByteLimit ) {
	if ( rFragments.empty() || rFragments.size() > UINT32_MAX
	        || !t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rFragments.size(), sizeof( t3dVTPackViewV3_t ) )
	        || rPlan.m_nWorkingBytes > nWorkingByteLimit || !t3dVectorResizeCold( rViews, rFragments.size() ) ) { return false; }
	for ( size_t i = 0u; i < rFragments.size(); ++i ) {
		const PackedByteArray &bytes = rFragments[i].m_Bytes;
		t3dVTPackViewV3_t &view = rViews[i];
		if ( !t3d_vt_pack_open_view_v3( bytes.ptr(), ( uint64_t )bytes.size(), view )
		        || ( rFragments[i].m_nStableKey != 0u && ( view.m_pHeader->m_nAllocationCount != 1u
		                || view.m_pAllocationKeys[0].m_nStableKey != rFragments[i].m_nStableKey ) ) ) { return false; }
		const t3dVTPackHeaderV3_t &header = *view.m_pHeader;
		if ( !t3dCheckedAdd( rPlan.m_nInputBytes, ( uint64_t )bytes.size(), rPlan.m_nInputBytes )
		        || !t3dCheckedAdd( rPlan.m_nAllocations, ( uint64_t )header.m_nAllocationCount, rPlan.m_nAllocations )
		        || !t3dCheckedAdd( rPlan.m_nPages, ( uint64_t )header.m_nPageCount, rPlan.m_nPages )
		        || !t3dCheckedAdd( rPlan.m_nTails, ( uint64_t )header.m_nTailCount, rPlan.m_nTails )
		        || !t3dCheckedAdd( rPlan.m_nViews, ( uint64_t )header.m_nQualityViewCount, rPlan.m_nViews )
		        || !t3dCheckedAdd( rPlan.m_nPayloads, ( uint64_t )header.m_nPayloadCount, rPlan.m_nPayloads ) ) { return false; }
	}
	if ( rPlan.m_nAllocations > UINT32_MAX / T3D_VT_LOGICAL_CHANNEL_COUNT || rPlan.m_nPages > UINT32_MAX
	        || rPlan.m_nTails > UINT32_MAX || rPlan.m_nViews > UINT32_MAX || rPlan.m_nPayloads > UINT32_MAX / 4u ) { return false; }
	// Three complete input sizes cover unique byte copies, concatenation and
	// serialized output (including its record tables). Explicit vector capacity
	// is counted separately; bucket rounding is at most four times payloads.
	return t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rPlan.m_nInputBytes, 3u )
	        && t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rFragments.size(), sizeof( t3dVTEditorFragmentV3_t ) + sizeof( eastl::vector<uint32_t, Terrain3DEastlAllocator> ) )
	        && t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rPlan.m_nAllocations, sizeof( t3dVTEditorAllocationInputV3_t ) + sizeof( t3dVTPackAllocationRecordV3_t ) + sizeof( t3dVTPackAllocationKeyRecordV3_t ) + T3D_VT_LOGICAL_CHANNEL_COUNT * sizeof( t3dVTChannelAllocation_t ) )
	        && t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rPlan.m_nPages, sizeof( t3dVTPackPageRecordV3_t ) )
	        && t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rPlan.m_nTails, sizeof( t3dVTPackTailRecordV3_t ) )
	        && t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rPlan.m_nViews, sizeof( t3dVTPackQualityViewV3_t ) )
	        && t3dVTMergeAddBytes( rPlan.m_nWorkingBytes, rPlan.m_nPayloads, sizeof( t3dVTEditorUniquePayloadV3_t ) + sizeof( t3dVTPackPayloadRecordV3_t ) + 6u * sizeof( uint32_t ) )
	        && rPlan.m_nWorkingBytes <= nWorkingByteLimit;
}

static bool t3d_vt_godot_merge_allocation_fragments_v3(
        const eastl::vector<t3dVTEditorFragmentV3_t, Terrain3DEastlAllocator> &rFragments,
        const uint32_t nPackageGeneration,
        PackedByteArray &rPackageBytes, const uint64_t nWorkingByteLimit = T3D_VT_COOK_WORKING_BYTE_LIMIT,
        t3dVTMergeStatistics_t *const pStatistics = NULL ) {
	if ( rFragments.empty() ) {
		return false;
	}
	eastl::vector<t3dVTPackAllocationRecordV3_t, Terrain3DEastlAllocator> allocations;
	eastl::vector<t3dVTPackAllocationKeyRecordV3_t, Terrain3DEastlAllocator> keys;
	eastl::vector<t3dVTChannelAllocation_t, Terrain3DEastlAllocator> channels;
	eastl::vector<t3dVTPackPageRecordV3_t, Terrain3DEastlAllocator> pages;
	eastl::vector<t3dVTPackTailRecordV3_t, Terrain3DEastlAllocator> tails;
	eastl::vector<t3dVTPackQualityViewV3_t, Terrain3DEastlAllocator> views;
	eastl::vector<t3dVTEditorUniquePayloadV3_t, Terrain3DEastlAllocator> uniquePayloads;
	t3dVTEditorMergePlanV3_t plan;
	t3dVTEditorPayloadIndexV3_t payloadIndex;
	eastl::vector<t3dVTPackViewV3_t, Terrain3DEastlAllocator> inputViews;
	eastl::vector<eastl::vector<uint32_t, Terrain3DEastlAllocator>, Terrain3DEastlAllocator> payloadMaps;
	eastl::vector<t3dVTEditorAllocationInputV3_t, Terrain3DEastlAllocator> inputs;
	if ( !t3dVTPlanMergeV3( rFragments, inputViews, plan, nWorkingByteLimit ) ) { return false; }
	if ( pStatistics != NULL ) { pStatistics->m_nWorkingBytes = plan.m_nWorkingBytes; }
	if ( !t3dVectorResizeCold( payloadMaps, rFragments.size() )
	        || !t3dContainerReserveCold( inputs, ( size_t )plan.m_nAllocations )
	        || !t3dContainerReserveCold( allocations, ( size_t )plan.m_nAllocations )
	        || !t3dContainerReserveCold( keys, ( size_t )plan.m_nAllocations )
	        || !t3dContainerReserveCold( channels, ( size_t )plan.m_nAllocations * T3D_VT_LOGICAL_CHANNEL_COUNT )
	        || !t3dContainerReserveCold( pages, ( size_t )plan.m_nPages )
	        || !t3dContainerReserveCold( tails, ( size_t )plan.m_nTails )
	        || !t3dContainerReserveCold( views, ( size_t )plan.m_nViews )
	        || !t3dContainerReserveCold( uniquePayloads, ( size_t )plan.m_nPayloads )
	        || !payloadIndex.prepare( ( uint32_t )plan.m_nPayloads ) ) { return false; }
	for ( uint32_t iFragment = 0u; iFragment < ( uint32_t )rFragments.size(); ++iFragment ) {
		t3dVTPackViewV3_t &view = inputViews[iFragment];
		for ( uint32_t iAllocation = 0u; iAllocation < view.m_pHeader->m_nAllocationCount; ++iAllocation ) {
			inputs.push_back( { iFragment, iAllocation, view.m_pAllocationKeys[iAllocation].m_nStableKey } );
		}
		eastl::vector<uint32_t, Terrain3DEastlAllocator> &map = payloadMaps[iFragment];
		if ( !t3dVectorResizeCold( map, view.m_pHeader->m_nPayloadCount ) ) { return false; }
		for ( uint32_t iPayload = 0u; iPayload < view.m_pHeader->m_nPayloadCount; ++iPayload ) {
			const uint8_t *pBytes = NULL;
			uint32_t nBytes = 0u;
			if ( !t3d_vt_godot_payload_bytes_v3( view, iPayload, pBytes, nBytes ) ) { return false; }
			map[iPayload] = t3d_vt_godot_find_or_add_payload_v3( uniquePayloads, payloadIndex, view.m_pPayloads[iPayload], pBytes, nBytes );
			if ( map[iPayload] == UINT32_MAX ) { return false; }
		}
	}
	if ( pStatistics != NULL ) { pStatistics->m_nPayloadComparisons = payloadIndex.m_nComparisons; }
	eastl::sort( inputs.begin(), inputs.end(), &t3dVTAllocationInputLess );
	for ( size_t iInput = 0u; iInput < inputs.size(); ++iInput ) {
		const t3dVTEditorAllocationInputV3_t &input = inputs[iInput];
		if ( iInput != 0u && inputs[iInput - 1u].nStableKey == input.nStableKey ) { return false; }
		const t3dVTPackViewV3_t &view = inputViews[input.nFragment];
		const eastl::vector<uint32_t, Terrain3DEastlAllocator> &payloadMap = payloadMaps[input.nFragment];
		const uint32_t nNewAllocation = ( uint32_t )allocations.size();
		allocations.push_back( view.m_pAllocations[input.nAllocation] );
		t3dVTPackAllocationKeyRecordV3_t key = view.m_pAllocationKeys[input.nAllocation];
		key.m_nAllocationId = nNewAllocation;
		keys.push_back( key );
		for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
			t3dVTChannelAllocation_t channel = view.m_pChannelAllocations[input.nAllocation * T3D_VT_LOGICAL_CHANNEL_COUNT + iChannel];
			if ( channel.m_nMipCount != 0u ) {
				const uint32_t nOldBase = channel.m_nBaseVirtualPage;
				const uint32_t nNewBase = ( uint32_t )pages.size();
				for ( uint32_t iPage = 0u; iPage < channel.m_nPageCount; ++iPage ) {
					t3dVTPackPageRecordV3_t page = view.m_pPages[nOldBase + iPage];
					page.m_nVirtualPage = nNewBase + iPage;
					if ( page.m_nParentPage != T3D_VT_INVALID_PAGE ) {
						page.m_nParentPage = page.m_nParentPage - nOldBase + nNewBase;
					}
					page.m_nAllocationId = nNewAllocation;
					page.m_nPayloadId = payloadMap[page.m_nPayloadId];
					pages.push_back( page );
				}
				t3dVTPackTailRecordV3_t tail = view.m_pTails[channel.m_nTailIndex];
				tail.m_nCanonicalAllocationId = nNewAllocation;
				tail.m_nPayloadId = payloadMap[tail.m_nPayloadId];
				channel.m_nBaseVirtualPage = nNewBase;
				channel.m_nTailIndex = ( uint32_t )tails.size();
				tails.push_back( tail );
			}
			channels.push_back( channel );
		}
		for ( uint32_t iView = 0u; iView < view.m_pHeader->m_nQualityViewCount; ++iView ) {
			if ( view.m_pQualityViews[iView].m_nAllocationId != input.nAllocation ) { continue; }
			t3dVTPackQualityViewV3_t quality = view.m_pQualityViews[iView];
			quality.m_nAllocationId = nNewAllocation;
			views.push_back( quality );
		}
	}
	if ( allocations.empty() || uniquePayloads.empty() || tails.empty() || views.empty() ) {
		return false;
	}
	eastl::vector<t3dVTPackPayloadRecordV3_t, Terrain3DEastlAllocator> payloads;
	if ( !t3dVectorResizeCold( payloads, uniquePayloads.size() ) ) { return false; }
	uint64_t nPayloadBytes = 0u;
	for ( size_t iPayload = 0u; iPayload < uniquePayloads.size(); ++iPayload ) {
		nPayloadBytes += ( uint64_t )uniquePayloads[iPayload].m_Bytes.size();
		if ( nPayloadBytes > INT64_MAX || nPayloadBytes > UINT32_MAX ) {
			return false;
		}
	}
	PackedByteArray payloadBytes;
	if ( payloadBytes.resize( ( int64_t )nPayloadBytes ) != OK ) {
		return false;
	}
	uint32_t nPayloadOffset = 0u;
	for ( uint32_t iPayload = 0u; iPayload < ( uint32_t )uniquePayloads.size(); ++iPayload ) {
		payloads[iPayload] = uniquePayloads[iPayload].m_Record;
		payloads[iPayload].m_nClusterId = 0u;
		payloads[iPayload].m_nClusterOffset = nPayloadOffset;
		memcpy( payloadBytes.ptrw() + nPayloadOffset, uniquePayloads[iPayload].m_Bytes.ptr(), ( size_t )uniquePayloads[iPayload].m_Bytes.size() );
		nPayloadOffset += ( uint32_t )uniquePayloads[iPayload].m_Bytes.size();
	}
	t3dVTPackClusterRecordV3_t cluster;
	memset( &cluster, 0, sizeof( cluster ) );
	cluster.m_nPayloadDataOffset = 0u;
	cluster.m_nContentSignature = t3d_vt_pack_hash64( payloadBytes.ptr(), nPayloadBytes );
	cluster.m_nStoredBytes = ( uint32_t )nPayloadBytes;
	cluster.m_nDecodedBytes = ( uint32_t )nPayloadBytes;
	cluster.m_nFirstPayload = 0u;
	cluster.m_nPayloadCount = ( uint32_t )payloads.size();
	cluster.m_nCodec = T3D_VT_PACK_CODEC_NONE;
	cluster.m_nChecksum = t3d_vt_pack_checksum32( payloadBytes.ptr(), nPayloadBytes );
	t3dVTPackFormatFamilyRecordV3_t formatFamilies[T3D_VT_LOGICAL_CHANNEL_COUNT];
	if ( !t3d_vt_pack_init_format_family_records_v3( formatFamilies, T3D_VT_LOGICAL_CHANNEL_COUNT ) ) {
		return false;
	}
	t3dVTPackSourceV3_t packSource;
	memset( &packSource, 0, sizeof( packSource ) );
	packSource.m_pAllocations = allocations.data();
	packSource.m_nAllocationCount = ( uint32_t )allocations.size();
	packSource.m_pAllocationKeys = keys.data();
	packSource.m_pChannelAllocations = channels.data();
	packSource.m_nChannelAllocationCount = ( uint32_t )channels.size();
	packSource.m_pPages = pages.empty() ? NULL : pages.data();
	packSource.m_nPageCount = ( uint32_t )pages.size();
	packSource.m_pPayloads = payloads.data();
	packSource.m_nPayloadCount = ( uint32_t )payloads.size();
	packSource.m_pClusters = &cluster;
	packSource.m_nClusterCount = 1u;
	packSource.m_pTails = tails.data();
	packSource.m_nTailCount = ( uint32_t )tails.size();
	packSource.m_pQualityViews = views.data();
	packSource.m_nQualityViewCount = ( uint32_t )views.size();
	packSource.m_pFormatFamilies = formatFamilies;
	packSource.m_nFormatFamilyCount = T3D_VT_LOGICAL_CHANNEL_COUNT;
	packSource.m_pPayloadData = payloadBytes.ptr();
	packSource.m_nPayloadDataBytes = nPayloadBytes;
	packSource.m_nPackageGeneration = nPackageGeneration == 0u ? 1u : nPackageGeneration;
	const uint64_t nBytes = t3d_vt_pack_compute_bytes_v3( packSource );
	if ( nBytes == 0u || nBytes > INT64_MAX ) {
		return false;
	}
	PackedByteArray output;
	if ( output.resize( ( int64_t )nBytes ) != OK ) { return false; }
	uint64_t nWritten = 0u;
	if ( !t3d_vt_pack_write_v3( packSource, output.ptrw(), nBytes, nWritten ) || nWritten != nBytes ) { return false; }
	rPackageBytes = output;
	return true;
}

bool t3d_vt_godot_cook_source_fragment_v3( const t3dVTCookSparseSourceV3_t &rSource, PackedByteArray &rBytes, const uint64_t nWorkingByteLimit ) {
	rBytes.clear();
	t3dVTCookerSettings_t settings{};
	t3d_vt_cooker_default_settings( settings );
	t3dVTCookAllocator_t allocator{};
	t3dVTEditorAllocationBudget_t budget;
	budget.m_nLimit = nWorkingByteLimit;
	allocator.m_pUser = &budget;
	allocator.m_pAllocate = &t3d_vt_godot_editor_allocate;
	allocator.m_pFree = &t3d_vt_godot_editor_free;
	t3dVTEncoderInterface_t encoder{};
	t3d_vt_godot_make_encoder( encoder );
	t3dVTCookedSparsePackageV3_t package{};
	const bool bCooked = t3d_vt_cooker_cook_sparse_pages_v3( settings, &rSource, 1u, encoder, allocator, NULL, package );
	bool bWritten = false;
	if ( bCooked ) {
		t3dVTPackSourceV3_t packSource{};
		if ( t3d_vt_cooker_build_pack_source_v3( package, packSource ) ) {
			const uint64_t nBytes = t3d_vt_pack_compute_bytes_v3( packSource );
			if ( budget.m_nLiveBytes <= budget.m_nLimit && nBytes != 0u && nBytes <= INT64_MAX
			        && nBytes <= budget.m_nLimit - budget.m_nLiveBytes && rBytes.resize( ( int64_t )nBytes ) == OK ) {
				uint64_t nWritten = 0u;
				bWritten = t3d_vt_pack_write_v3( packSource, rBytes.ptrw(), nBytes, nWritten ) && nWritten == nBytes;
			}
		}
	}
	t3d_vt_cooker_release_sparse_package_v3( package );
	if ( !bWritten ) { rBytes.clear(); }
	return bWritten;
}

bool t3d_vt_godot_merge_packages_v3( const PackedByteArray *pPackages, const uint32_t nCount,
	        const uint32_t nGeneration, PackedByteArray &rPackageBytes, const uint64_t nWorkingByteLimit,
	        t3dVTMergeStatistics_t *const pStatistics ) {
	if ( pStatistics != NULL ) { *pStatistics = t3dVTMergeStatistics_t{}; }
	if ( pPackages == NULL || nCount == 0u ) { return false; }
	eastl::vector<t3dVTEditorFragmentV3_t, Terrain3DEastlAllocator> fragments;
	if ( ( uint64_t )nCount * sizeof( t3dVTEditorFragmentV3_t ) > nWorkingByteLimit
	        || !t3dContainerReserveCold( fragments, nCount ) ) { return false; }
	for ( uint32_t i = 0u; i < nCount; ++i ) {
		if ( pPackages[i].is_empty() ) { continue; }
		t3dVTEditorFragmentV3_t fragment{};
		fragment.m_Bytes = pPackages[i];
		fragments.push_back( fragment );
	}
	return t3d_vt_godot_merge_allocation_fragments_v3( fragments, nGeneration, rPackageBytes, nWorkingByteLimit, pStatistics );
}

bool t3d_vt_godot_cook_editor_package_incremental_v3( const Ref<Terrain3DAssets> &rAssets,
        const uint64_t *const pStableKeys,
        const t3dRenderingMaterialGpu_t *const pMaterialRecords,
        const uint32_t nMaterialCount,
        t3dVTEditorCookCacheV3_t &rCache,
        PackedByteArray &rPackageBytes,
        uint64_t &rFingerprint,
        uint32_t &rPackageGeneration,
        t3dVTEditorCookTelemetryV3_t &rTelemetry,
        String *const pError ) {
	( void )pMaterialRecords; // Runtime-only scalar/material state must not invalidate page payloads.
	rPackageBytes.clear();
	rFingerprint = 0u;
	rPackageGeneration = 0u;
	memset( &rTelemetry, 0, sizeof( rTelemetry ) );
	if ( rAssets.is_null() || pStableKeys == NULL || nMaterialCount == 0u ) {
		if ( pError ) {
			*pError = "VT editor V3 incremental cooker has no material source";
		}
		return false;
	}
	t3dVTCookerSettings_t settings;
	t3d_vt_cooker_default_settings( settings );
	t3dVTCookAllocator_t allocator;
	allocator.m_pUser = NULL;
	allocator.m_pAllocate = &t3d_vt_godot_editor_allocate;
	allocator.m_pFree = &t3d_vt_godot_editor_free;
	eastl::vector<t3dVTEditorFragmentV3_t, Terrain3DEastlAllocator> fragments;
	if ( !t3dContainerReserveCold( fragments, nMaterialCount ) ) {
		if ( pError ) {
			*pError = "VT editor V3 incremental cooker could not allocate fragment staging";
		}
		return false;
	}
	for ( uint32_t iMaterial = 0u; iMaterial < nMaterialCount; ++iMaterial ) {
		if ( pStableKeys[iMaterial] == 0u ) {
			if ( pError ) {
				*pError = "VT editor V3 incremental cooker encountered a zero stable key";
			}
			return false;
		}
		t3dVTEditorFragmentV3_t fragment;
		fragment.m_nStableKey = pStableKeys[iMaterial];
		if ( !t3d_vt_godot_cook_allocation_fragment_v3( rAssets,
		             fragment.m_nStableKey,
		             rCache,
		             settings,
		             allocator,
		             rTelemetry,
		             false,
		             fragment.m_Bytes,
		             fragment.m_nKey,
		             pError ) ) {
			return false;
		}
		fragments.push_back( fragment );
	}
	t3d_vt_godot_sort_fragments_v3( fragments );
	uint64_t nFingerprint = 1469598103934665603ULL;
	nFingerprint = t3d_vt_godot_hash_u64( nFingerprint, nMaterialCount );
	for ( size_t iFragment = 0u; iFragment < fragments.size(); ++iFragment ) {
		nFingerprint = t3d_vt_godot_hash_u64( nFingerprint, fragments[iFragment].m_nStableKey );
		nFingerprint = t3d_vt_godot_hash_u64( nFingerprint, fragments[iFragment].m_nKey );
	}
	rFingerprint = nFingerprint == 0u ? 1u : nFingerprint;
	rPackageGeneration = ( uint32_t )( rFingerprint ^ ( rFingerprint >> 32u ) );
	if ( rPackageGeneration == 0u ) {
		rPackageGeneration = 1u;
	}
	rTelemetry.m_nStage = T3D_VT_EDITOR_COOK_STAGE_PACK;
	const uint64_t nPackStart = t3d_vt_godot_now_usec_v3();
	if ( !t3d_vt_godot_merge_allocation_fragments_v3( fragments, rPackageGeneration, rPackageBytes ) ) {
		if ( pError ) {
			*pError = "VT editor V3 incremental cooker could not stage the deterministic package";
		}
		return false;
	}
	const uint64_t nPackEnd = t3d_vt_godot_now_usec_v3();
	if ( nPackEnd >= nPackStart ) {
		rTelemetry.m_nPackUsec += nPackEnd - nPackStart;
	}
	t3dVTPackViewV3_t finalView;
	if ( !t3d_vt_pack_open_view_v3( rPackageBytes.ptr(), ( uint64_t )rPackageBytes.size(), finalView )
	        || finalView.m_pHeader->m_nAllocationCount != nMaterialCount ) {
		rPackageBytes.clear();
		if ( pError ) {
			*pError = "VT editor V3 incremental cooker staged an invalid package";
		}
		return false;
	}
	rTelemetry.m_nStage = T3D_VT_EDITOR_COOK_STAGE_COMPLETE;
	if ( pError ) {
		*pError = String();
	}
	return true;
}

static bool t3d_vt_godot_validate_shipping_package_v3( const Ref<Terrain3DAssets> &rAssets,
        const PackedByteArray &rPackageBytes,
        const uint32_t nExpectedAllocationCount,
        String *const pError ) {
	t3dVTPackViewV3_t view;
	if ( rPackageBytes.is_empty() || !t3d_vt_pack_open_view_v3( rPackageBytes.ptr(), ( uint64_t )rPackageBytes.size(), view )
	        || view.m_pHeader->m_nVersion != T3D_VT_FILE_VERSION_V3 || view.m_pHeader->m_nAllocationCount != nExpectedAllocationCount
	        || view.m_pHeader->m_nFullySupportedProfileMask == 0u ) {
		if ( pError ) {
			*pError = "VT shipping V3 package failed structural validation";
		}
		return false;
	}
	uint32_t nViewCursor = 0u;
	for ( uint32_t iAllocation = 0u; iAllocation < nExpectedAllocationCount; ++iAllocation ) {
		const t3dVTPackAllocationRecordV3_t &allocation = view.m_pAllocations[iAllocation];
		const t3dVTPackAllocationKeyRecordV3_t &key = view.m_pAllocationKeys[iAllocation];
		if ( ( allocation.m_nFlags & T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE ) == 0u
		        || ( allocation.m_nFlags & T3D_VT_ALLOCATION_EDITOR_DYNAMIC ) != 0u ) {
			if ( pError ) {
				*pError = "VT shipping V3 package contains a non-shipping allocation";
			}
			return false;
		}
		t3dVTGodotSourceV3_t source;
		bool bAuthored = false;
		if ( !t3d_vt_godot_extract_source_by_stable_key_v3( rAssets, key.m_nStableKey, source, bAuthored ) || !bAuthored
		        || source.m_eHighestAvailableProfile == T3D_VT_FIXED_PROFILE_INVALID
		        || allocation.m_nConsumer != source.m_nConsumer
		        || allocation.m_nHighestAvailableProfile != ( uint8_t )source.m_eHighestAvailableProfile ) {
			if ( pError ) {
				*pError = "VT shipping V3 allocation/source contract mismatch";
			}
			return false;
		}
		uint32_t sourceExtent[T3D_VT_LOGICAL_CHANNEL_COUNT];
		uint32_t nExpectedAllocationMask = 0u;
		for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
			sourceExtent[iChannel] = source.m_ChannelDesc[iChannel].m_nSourceExtent;
			if ( ( source.m_nAuthoredChannelMask & T3D_VT_LOGICAL_CHANNEL_BIT( iChannel ) ) != 0u
			        && t3d_vt_fixed_profile_channel_extent(
			                   source.m_eHighestAvailableProfile, ( t3dVTLogicalChannel_t )iChannel ) != 0u ) {
				nExpectedAllocationMask |= T3D_VT_LOGICAL_CHANNEL_BIT( iChannel );
			}
		}
		if ( allocation.m_nChannelMask != nExpectedAllocationMask ) {
			if ( pError ) {
				*pError = "VT shipping V3 package is missing or misroutes an authored channel";
			}
			return false;
		}
		for ( uint32_t iProfile = 0u; iProfile <= allocation.m_nHighestAvailableProfile; ++iProfile ) {
			const t3dVTFixedProfile_t Profile = ( t3dVTFixedProfile_t )iProfile;
			if ( !t3d_vt_fixed_profile_source_supports(
			             Profile, ( t3dVTConsumer_t )source.m_nConsumer, source.m_nAuthoredChannelMask, sourceExtent )
			        || nViewCursor >= view.m_pHeader->m_nQualityViewCount ) {
				if ( pError ) {
					*pError = "VT shipping V3 quality view would require an upscale";
				}
				return false;
			}
			const t3dVTPackQualityViewV3_t &qualityView = view.m_pQualityViews[nViewCursor++];
			if ( qualityView.m_nAllocationId != iAllocation || qualityView.m_nProfile != iProfile ) {
				if ( pError ) {
					*pError = "VT shipping V3 quality-view ordering is invalid";
				}
				return false;
			}
			uint32_t nExpectedViewMask = 0u;
			for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
				const uint32_t nBit = T3D_VT_LOGICAL_CHANNEL_BIT( iChannel );
				const bool bEnabled = ( nExpectedAllocationMask & nBit ) != 0u
				        && t3d_vt_fixed_profile_channel_extent( Profile, ( t3dVTLogicalChannel_t )iChannel ) != 0u;
				if ( !bEnabled ) {
					if ( qualityView.m_nBaseMip[iChannel] != T3D_VT_PACK_INVALID_MIP
					        || qualityView.m_nMaxMip[iChannel] != T3D_VT_PACK_INVALID_MIP ) {
						return false;
					}
					continue;
				}
				nExpectedViewMask |= nBit;
				const t3dVTChannelAllocation_t &channel =
				        view.m_pChannelAllocations[iAllocation * T3D_VT_LOGICAL_CHANNEL_COUNT + iChannel];
				uint8_t nExpectedBaseMip = T3D_VT_PACK_INVALID_MIP;
				if ( !t3d_vt_fixed_profile_quality_base_mip( source.m_eHighestAvailableProfile,
				             Profile,
				             ( t3dVTLogicalChannel_t )iChannel,
				             channel.m_nMipCount,
				             nExpectedBaseMip )
				        || qualityView.m_nBaseMip[iChannel] != nExpectedBaseMip
				        || qualityView.m_nMaxMip[iChannel] != channel.m_nMipCount - 1u ) {
					if ( pError ) {
						*pError = "VT shipping V3 quality-view mip mapping is invalid";
					}
					return false;
				}
			}
			if ( qualityView.m_nChannelMask != nExpectedViewMask ) {
				if ( pError ) {
					*pError = "VT shipping V3 quality-view channel mask is invalid";
				}
				return false;
			}
		}
	}
	if ( nViewCursor != view.m_pHeader->m_nQualityViewCount ) {
		if ( pError ) {
			*pError = "VT shipping V3 package advertises unmatched quality views";
		}
		return false;
	}
	return true;
}

bool t3d_vt_godot_cook_runtime_package( const Ref<Terrain3DAssets> &rAssets,
        const uint64_t *const pStableKeys,
        const t3dRenderingMaterialGpu_t *const pMaterialRecords,
        const uint32_t nMaterialCount,
        PackedByteArray &rPackageBytes,
        uint32_t &rPackageGeneration,
        String *const pError ) {
	( void )pMaterialRecords;
	rPackageBytes.clear();
	rPackageGeneration = 0u;
	if ( rAssets.is_null() || pStableKeys == NULL || nMaterialCount == 0u ) {
		if ( pError ) {
			*pError = "VT shipping V3 cooker has no material source";
		}
		return false;
	}
	t3dVTCookerSettings_t settings;
	t3d_vt_cooker_default_settings( settings );
	t3dVTCookAllocator_t allocator;
	allocator.m_pUser = NULL;
	allocator.m_pAllocate = &t3d_vt_godot_editor_allocate;
	allocator.m_pFree = &t3d_vt_godot_editor_free;
	t3dVTEditorCookCacheV3_t cache;
	t3d_vt_godot_editor_cache_reset_v3( cache );
	t3dVTEditorCookTelemetryV3_t telemetry;
	memset( &telemetry, 0, sizeof( telemetry ) );
	eastl::vector<t3dVTEditorFragmentV3_t, Terrain3DEastlAllocator> fragments;
	if ( !t3dContainerReserveCold( fragments, nMaterialCount ) ) {
		if ( pError ) {
			*pError = "VT shipping V3 cooker could not allocate fragment staging";
		}
		return false;
	}
	for ( uint32_t iMaterial = 0u; iMaterial < nMaterialCount; ++iMaterial ) {
		if ( pStableKeys[iMaterial] == 0u ) {
			if ( pError ) {
				*pError = "VT shipping V3 cooker encountered a zero stable key";
			}
			return false;
		}
		t3dVTEditorFragmentV3_t fragment;
		fragment.m_nStableKey = pStableKeys[iMaterial];
		if ( !t3d_vt_godot_cook_allocation_fragment_v3( rAssets,
		             fragment.m_nStableKey,
		             cache,
		             settings,
		             allocator,
		             telemetry,
		             true,
		             fragment.m_Bytes,
		             fragment.m_nKey,
		             pError ) ) {
			return false;
		}
		fragments.push_back( fragment );
	}
	t3d_vt_godot_sort_fragments_v3( fragments );
	uint64_t nFingerprint = 1469598103934665603ULL;
	nFingerprint = t3d_vt_godot_hash_u64( nFingerprint, nMaterialCount );
	for ( size_t iFragment = 0u; iFragment < fragments.size(); ++iFragment ) {
		nFingerprint = t3d_vt_godot_hash_u64( nFingerprint, fragments[iFragment].m_nStableKey );
		nFingerprint = t3d_vt_godot_hash_u64( nFingerprint, fragments[iFragment].m_nKey );
	}
	rPackageGeneration = ( uint32_t )( nFingerprint ^ ( nFingerprint >> 32u ) );
	if ( rPackageGeneration == 0u ) {
		rPackageGeneration = 1u;
	}
	if ( !t3d_vt_godot_merge_allocation_fragments_v3( fragments, rPackageGeneration, rPackageBytes )
	        || !t3d_vt_godot_validate_shipping_package_v3( rAssets, rPackageBytes, nMaterialCount, pError ) ) {
		rPackageBytes.clear();
		rPackageGeneration = 0u;
		if ( pError && pError->is_empty() ) {
			*pError = "VT shipping V3 package validation failed";
		}
		return false;
	}
	if ( pError ) {
		*pError = String();
	}
	return true;
}


bool t3d_vt_godot_collect_asset_materials(
        const Ref<Terrain3DAssets> &rAssets, t3dVTGodotAssetMaterials_t &rMaterials, String *const pError ) {
	rMaterials.m_stableKeys.clear();
	rMaterials.m_records.clear();
	uint32_t rMaterialCount = 0u;
	if ( rAssets.is_null() ) {
		if ( pError ) {
			*pError = "VT asset material collector has no Terrain3DAssets source";
		}
		return false;
	}
	const uint32_t nTerrainMaterialCount =
	        rAssets->usesPackedPbr() ? ( uint32_t )MIN( rAssets->getTextureCount(), T3D_MAX_TEXTURE_ASSETS ) : 0u;
	uint32_t nMaxMaterialRefs = nTerrainMaterialCount;
	const int nMeshCount = rAssets->getMeshCount();
	for ( int iMesh = 0; iMesh < nMeshCount; ++iMesh ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( iMesh );
		if ( asset.is_null() || !asset->isEnabled() ) {
			continue;
		}
		const TypedArray<Terrain3DRenderingMaterial> surfaceMaterials = asset->getSurfaceRenderingMaterials();
		if ( surfaceMaterials.size() > INT32_MAX - 2 || nMaxMaterialRefs > UINT32_MAX - ( uint32_t )surfaceMaterials.size() - 2u ) {
			if ( pError ) {
				*pError = "VT asset material collector material reference count overflowed";
			}
			return false;
		}
		nMaxMaterialRefs += ( uint32_t )surfaceMaterials.size() + 2u;
	}
	if ( nMaxMaterialRefs == 0u ) {
		if ( pError ) {
			*pError = String();
		}
		return true;
	}
	if ( !t3dContainerReserveCold( rMaterials.m_stableKeys, nMaxMaterialRefs )
	        || !t3dContainerReserveCold( rMaterials.m_records, nMaxMaterialRefs ) ) {
		if ( pError ) {
			*pError = "VT asset material collector could not allocate material identity tables";
		}
		return false;
	}
	t3dVectorResizeWithinCapacity( rMaterials.m_stableKeys, nMaxMaterialRefs );
	t3dVectorResizeWithinCapacity( rMaterials.m_records, nMaxMaterialRefs );
	uint64_t *const pStableKeys = rMaterials.m_stableKeys.data();
	t3dRenderingMaterialGpu_t *const pMaterialRecords = rMaterials.m_records.data();
	for ( uint32_t iMaterial = 0u; iMaterial < nTerrainMaterialCount; ++iMaterial ) {
		const Ref<Terrain3DTextureAsset> asset = rAssets->getTextureAsset( ( int )iMaterial );
		if ( asset.is_null() ) {
			continue;
		}
		pStableKeys[rMaterialCount] = t3d_vt_terrain_material_stable_key( iMaterial );
		memset( &pMaterialRecords[rMaterialCount], 0, sizeof( pMaterialRecords[rMaterialCount] ) );
		pMaterialRecords[rMaterialCount].m_nVtAllocation = T3D_VT_INVALID_ALLOCATION;
		pMaterialRecords[rMaterialCount].m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
		++rMaterialCount;
	}
	auto appendMaterial = [&]( const Ref<Terrain3DRenderingMaterial> &rMaterial ) -> bool {
		if ( rMaterial.is_null() ) {
			return true;
		}
		// Scalar-only instance materials do not own VT payloads. Their GPU record
		// remains valid with T3D_VT_INVALID_ALLOCATION and the renderer supplies
		// neutral texture backing. Do not manufacture a required ALBEDO channel.
		// If any texture is authored, keep the material in the VT set so a missing
		// required ALBEDO still fails validation instead of being silently hidden.
		if ( rMaterial->getAlbedoTexture().is_null() && rMaterial->getNormalTexture().is_null()
		        && rMaterial->getSurfaceTexture().is_null() ) {
			return true;
		}
		const uint64_t nStableKey = rMaterial->getVtStableKey();
		uint32_t nReservedTerrainId = 0u;
		if ( nStableKey == 0u || t3d_vt_decode_terrain_material_stable_key( nStableKey, nReservedTerrainId )
		        || t3d_vt_is_terrain_region_stable_key( nStableKey ) ) {
			return false;
		}
		for ( uint32_t iKey = 0u; iKey < rMaterialCount; ++iKey ) {
			if ( pStableKeys[iKey] == nStableKey ) {
				return true;
			}
		}
		if ( rMaterialCount >= nMaxMaterialRefs
		        || !t3d_vt_godot_compile_material_record( rMaterial, 0u, pMaterialRecords[rMaterialCount] ) ) {
			return false;
		}
		pStableKeys[rMaterialCount++] = nStableKey;
		return true;
	};
	bool bCollected = true;
	for ( int iMesh = 0; iMesh < nMeshCount && bCollected; ++iMesh ) {
		const Ref<Terrain3DMeshAsset> asset = rAssets->getMeshAsset( iMesh );
		if ( asset.is_null() || !asset->isEnabled() ) {
			continue;
		}
		bCollected = asset->ensureRenderingMaterialStableKeysCold() && appendMaterial( asset->getRenderingMaterialOverride() )
		        && appendMaterial( asset->getRenderingMaterialOverlay() );
		const TypedArray<Terrain3DRenderingMaterial> surfaceMaterials = asset->getSurfaceRenderingMaterials();
		for ( int iMaterial = 0; bCollected && iMaterial < surfaceMaterials.size(); ++iMaterial ) {
			bCollected = appendMaterial( surfaceMaterials[iMaterial] );
		}
	}
	if ( !bCollected ) {
		rMaterials.m_stableKeys.clear();
		rMaterials.m_records.clear();
		if ( pError ) {
			*pError = "VT asset material collector could not establish stable material identity";
		}
		return false;
	}
	t3dVectorResizeWithinCapacity( rMaterials.m_stableKeys, rMaterialCount );
	t3dVectorResizeWithinCapacity( rMaterials.m_records, rMaterialCount );
	if ( pError ) {
		*pError = String();
	}
	return true;
}

bool t3d_vt_godot_cook_runtime_assets_package( const Ref<Terrain3DAssets> &rAssets,
        PackedByteArray &rPackageBytes,
        uint32_t &rPackageGeneration,
        uint32_t &rMaterialCount,
        String *const pError ) {
	rPackageBytes.clear();
	rPackageGeneration = 0u;
	rMaterialCount = 0u;
	t3dVTGodotAssetMaterials_t materials;
	if ( !t3d_vt_godot_collect_asset_materials( rAssets, materials, pError ) ) {
		return false;
	}
	rMaterialCount = ( uint32_t )materials.m_stableKeys.size();
	if ( rMaterialCount == 0u ) {
		return true;
	}
	return t3d_vt_godot_cook_runtime_package(
	        rAssets, materials.m_stableKeys.data(), materials.m_records.data(), rMaterialCount, rPackageBytes, rPackageGeneration, pError );
}

bool t3d_vt_godot_compile_material_record(
        const Ref<Terrain3DRenderingMaterial> &rMaterial, const uint32_t nAllocationId, t3dRenderingMaterialGpu_t &rRecord ) {
	if ( rMaterial.is_null() || nAllocationId == T3D_VT_INVALID_ALLOCATION ) {
		return false;
	}
	t3dRenderingMaterialVTBindings_t Bindings;
	Bindings.m_nVtAllocation = nAllocationId;
	Bindings.m_nOverlayVtAllocation = T3D_VT_INVALID_ALLOCATION;
	return rMaterial->compileVtRecordCold( Bindings, rRecord );
}
