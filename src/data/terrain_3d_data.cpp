// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include "eastl_config.h"
#include <EASTL/bitset.h>
#include <cstring>

#include "core/logger.h"
#include "data/terrain_3d_data.h"
#include "rendering/terrain_3d_compute.h"
#include "memory/terrain_3d_resident_pool.h"
#include "authoring/terrain_3d_erosion_job.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "authoring/terrain_3d_editor.h"
#include "rendering/terrain_3d_material.h"
#include "assets/terrain_3d_assets.h"
#include "instances/terrain_3d_instancer.h"
#include "instances/terrain_3d_instance_page_codec.h"
#include "simd/terrain_3d_parallel.h"
#include "memory/terrain_3d_linear_arena.h"
#include "data/terrain_3d_sampler.h"
#include "simd/terrain_3d_simd.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_page_format.h"

typedef struct t3dfarSurfaceBakeData_s {
	eastl::array<uint8_t, TERRAIN3D_MAX_TEXTURE_ASSETS> populated = {};
	int nCount = 0;
	int fallback_texture = 0;
} t3dfarSurfaceBakeData_t;

namespace {

template <typename Callback>
static bool terrain3dForEachSquareRing( const Vector2i &rCenter, const int nMaxRadius, Callback &&rCallback ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !rCallback( rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !rCallback( rCenter + Vector2i( x, -radius ) ) ||
				!rCallback( rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !rCallback( rCenter + Vector2i( -radius, y ) ) ||
				!rCallback( rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

typedef struct t3dcontrolCookSource_s {
	PackedByteArray nBytes;
	bool bPresent = false;
	bool bImplicit = true;
} t3dcontrolCookSource_t;

static bool terrain3dReadCookControl(
	const eastl::array<t3dcontrolCookSource_t, 9> &rSources,
	const int nRegionSize, const int iX, const int iZ, uint32_t &nControl
) {
	int region_dx = 0;
	int region_dz = 0;
	int local_x = iX;
	int local_z = iZ;
	if ( local_x < 0 ) {
		region_dx = -1;
		local_x += nRegionSize;
	} else if ( local_x >= nRegionSize ) {
		region_dx = 1;
		local_x -= nRegionSize;
	}
	if ( local_z < 0 ) {
		region_dz = -1;
		local_z += nRegionSize;
	} else if ( local_z >= nRegionSize ) {
		region_dz = 1;
		local_z -= nRegionSize;
	}
	const t3dcontrolCookSource_t &source =
		rSources[( size_t ) ( ( region_dz + 1 ) * 3 + ( region_dx + 1 ) )];
	if ( !source.bPresent ) {
		return false;
	}
	if ( source.bImplicit ) {
		nControl = TERRAIN3D_REGION_DEFAULT_CONTROL;
		return true;
	}
	const size_t offset = ( size_t ) ( local_z * nRegionSize + local_x ) * sizeof( uint32_t );
	std::memcpy( &nControl, source.nBytes.ptr() + offset, sizeof( uint32_t ) );
	return true;
}

static uint32_t terrain3dReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset );

static uint32_t terrain3dMaterialMaskFromControlImage( const Ref<Image> &rControl ) {
	if ( rControl.is_null() || rControl->is_empty() || rControl->get_format() != Image::FORMAT_RF ) {
		return 1u;
	}
	const PackedByteArray bytes = rControl->get_data();
	const uint8_t *const pRead = bytes.ptr();
	if ( !pRead || bytes.size() < 4 ) {
		return 1u;
	}
	uint32_t mask = 0u;
	const size_t texels = ( size_t ) ( bytes.size() ) / 4u;
	for ( size_t index = 0; index < texels; ++index ) {
		const uint32_t control = terrain3dReadU32Unaligned( pRead, index * 4u );
		mask |= 1u << ( uint32_t ) ( getBase( control ) & 31u );
		mask |= 1u << ( uint32_t ) ( getOverlay( control ) & 31u );
	}
	return mask != 0u ? mask : 1u;
}

static void terrain3dFnv1a64Append( uint64_t &nHash, const uint8_t *pData, const size_t nSize ) {
	for ( size_t index = 0u; index < nSize; ++index ) {
		nHash ^= ( uint64_t ) ( pData[index] );
		nHash *= 1099511628211ull;
	}
}

static void terrain3dFnv1a64AppendU32( uint64_t &nHash, const uint32_t nValue ) {
	terrain3dFnv1a64Append( nHash, ( const uint8_t * ) ( &nValue ), sizeof( nValue ) );
}

static void terrain3dFnv1a64AppendU64( uint64_t &nHash, const uint64_t nValue ) {
	terrain3dFnv1a64Append( nHash, ( const uint8_t * ) ( &nValue ), sizeof( nValue ) );
}

static void terrain3dFnv1a64AppendF32( uint64_t &nHash, const float fValue ) {
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ) );
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	terrain3dFnv1a64AppendU32( nHash, bits );
}

static bool terrain3dFnv1a64AppendR16HeightEdge(
	uint64_t &nHash, const Ref<Image> &rHeight,
	const int nRegionSize, const bool bFirstColumn
) {
	if ( rHeight.is_null() || rHeight->get_width() != nRegionSize || rHeight->get_height() != nRegionSize ||
		rHeight->get_format() != Image::FORMAT_R16 || rHeight->has_mipmaps() ) {
		return false;
	}
	const PackedByteArray bytes = rHeight->get_data();
	const size_t sample_bytes = sizeof( uint16_t );
	const size_t row_bytes = ( size_t ) ( nRegionSize ) * sample_bytes;
	const size_t expected_bytes = row_bytes * ( size_t ) ( nRegionSize );
	if ( ( size_t ) ( bytes.size() ) != expected_bytes ) {
		return false;
	}
	if ( !bFirstColumn ) {
		terrain3dFnv1a64Append( nHash, bytes.ptr(), row_bytes );
		return true;
	}
	for ( int row = 0; row < nRegionSize; ++row ) {
		terrain3dFnv1a64Append( nHash, bytes.ptr() + ( size_t ) ( row ) * row_bytes, sample_bytes );
	}
	return true;
}

class T3DLastScopeTimer {
	uint64_t _started_usec = 0u;
	uint64_t *_elapsed_usec = nullptr;

public:
	T3DLastScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		_elapsed_usec = &nElapsedUsec;
		Time *time = Time::get_singleton();
		_started_usec = time ? time->get_ticks_usec() : 0u;
	}
	~T3DLastScopeTimer( void ) {
		if ( !_elapsed_usec ) {
			return;
		}
		Time *time = Time::get_singleton();
		const uint64_t finished_usec = time ? time->get_ticks_usec() : _started_usec;
		*_elapsed_usec = finished_usec >= _started_usec ? finished_usec - _started_usec : 0u;
	}
};

class T3DProfileScopeTimer {
	uint64_t _started_usec = 0u;
	uint64_t *_elapsed_usec = nullptr;
	const uint64_t *_request_usec = nullptr;
	uint64_t *_end_to_end_usec = nullptr;

public:
	T3DProfileScopeTimer(
		uint64_t &nElapsedUsec, const uint64_t &nRequestUsec,
		uint64_t &nEndToEndUsec
	) :
			_elapsed_usec( &nElapsedUsec ), _request_usec( &nRequestUsec ),
			_end_to_end_usec( &nEndToEndUsec ) {
		Time *time = Time::get_singleton();
		_started_usec = time ? time->get_ticks_usec() : 0u;
	}

	~T3DProfileScopeTimer( void ) {
		Time *time = Time::get_singleton();
		const uint64_t finished_usec = time ? time->get_ticks_usec() : _started_usec;
		if ( _elapsed_usec ) {
			*_elapsed_usec = MAX( *_elapsed_usec, finished_usec - _started_usec );
		}
		if ( _end_to_end_usec && _request_usec && *_request_usec > 0u && finished_usec >= *_request_usec ) {
			*_end_to_end_usec = MAX( *_end_to_end_usec, finished_usec - *_request_usec );
		}
	}
};

static uint32_t terrain3dReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint16_t terrain3dReadU16Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint16_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint8_t terrain3dColorToByte( const float fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( fValue * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
}

static void terrain3dFillFarSurfaceEmpty( PackedByteArray &nBytes ) {
	uint8_t *pWrite = nBytes.ptrw();
	if ( !pWrite ) {
		return;
	}
	const size_t pixel_count = ( size_t ) ( nBytes.size() ) / 4u;
	for ( size_t index = 0u; index < pixel_count; ++index ) {
		const size_t offset = index * 4u;
		pWrite[offset] = 255u;
		pWrite[offset + 1u] = 255u;
		pWrite[offset + 2u] = 255u;
		pWrite[offset + 3u] = 0u;
	}
}

static uint8_t terrain3dFarSurfaceAlpha(
	const float fSolidCoverage, const float fColorAlpha,
	const int nMaterialId
) {
	if ( fSolidCoverage < 0.5f ) {
		return 0u;
	}
	// Bit 7 marks solid terrain. Bits 2-6 carry the dominant terrain texture
	// layer. Bits 0-1 retain coarse color-map alpha for wetness. This lets
	// the far shader recover one real terrain material sample without another
	// control/material atlas lookup.
	const uint32_t material = ( uint32_t ) ( CLAMP( nMaterialId, 0, 31 ) );
	const uint32_t surface_alpha = ( uint32_t ) ( CLAMP(
		Math::round( CLAMP( fColorAlpha, 0.0f, 1.0f ) * 3.0f ), 0.0f, 3.0f ) );
	return ( uint8_t ) ( 0x80u | ( material << 2u ) | surface_alpha );
}

static float terrain3dMaterialFloatParam(
	T3DMaterial *pMaterial, const char *pszName,
	const float fDefault
) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? fDefault : ( float ) ( value );
}

static int terrain3dMaterialIntParam(
	T3DMaterial *pMaterial, const char *pszName,
	const int nDefault
) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int ) ( value );
}

static int terrain3dFarResolveMaterialId(
	const t3dfarSurfaceBakeData_t &rBakeData,
	const int nTextureId
) {
	if ( nTextureId >= 0 && nTextureId < rBakeData.nCount &&
		rBakeData.populated[( size_t ) ( nTextureId )] != 0u ) {
		return nTextureId;
	}
	return rBakeData.fallback_texture;
}

static void terrain3dBuildFarSurfaceBakeData(
	T3DTerrain *pTerrain,
	t3dfarSurfaceBakeData_t &rBakeData
) {
	rBakeData = t3dfarSurfaceBakeData_s();
	if ( !pTerrain ) {
		return;
	}
	const Ref<T3DAssets> pAssets = pTerrain->getAssets();
	if ( pAssets.is_null() ) {
		return;
	}
	rBakeData.nCount = MIN( pAssets->getTextureCount(), TERRAIN3D_MAX_TEXTURE_ASSETS );
	bool bFoundFallback = false;
	for ( int index = 0; index < rBakeData.nCount; ++index ) {
		const bool bPopulated = pAssets->getTextureAsset( index ).is_valid();
		rBakeData.populated[( size_t ) ( index )] = bPopulated ? 1u : 0u;
		if ( bPopulated && !bFoundFallback ) {
			rBakeData.fallback_texture = index;
			bFoundFallback = true;
		}
	}
}

typedef enum t3dgpuRegionSlotState_e : uint8_t {
	TERRAIN3D_GPU_SLOT_FREE = 0u,
	TERRAIN3D_GPU_SLOT_STAGING = 1u,
	TERRAIN3D_GPU_SLOT_READY = 2u,
	TERRAIN3D_GPU_SLOT_MAPPED = 3u,
	TERRAIN3D_GPU_SLOT_RETIRED = 4u,
} t3dgpuRegionSlotState_t;

static size_t terrain3dGpuImageBytes( const Ref<Image> &rImage ) {
	return rImage.is_valid() ? ( size_t ) ( rImage->get_data().size() ) : 0u;
}

static void terrain3dRuntimePageWriteU32(
	PackedByteArray &nBytes, const uint32_t nOffset,
	const uint32_t nValue
) {
	if ( nOffset > ( uint32_t ) ( nBytes.size() ) || ( uint32_t ) ( nBytes.size() ) - nOffset < 4u ) {
		return;
	}
	uint8_t *pWrite = nBytes.ptrw() + nOffset;
	pWrite[0] = ( uint8_t ) ( nValue & 0xffu );
	pWrite[1] = ( uint8_t ) ( ( nValue >> 8u ) & 0xffu );
	pWrite[2] = ( uint8_t ) ( ( nValue >> 16u ) & 0xffu );
	pWrite[3] = ( uint8_t ) ( ( nValue >> 24u ) & 0xffu );
}

static uint32_t terrain3dRuntimePageReadU32( const PackedByteArray &arrBytes, const uint32_t nOffset ) {
	if ( nOffset > ( uint32_t ) ( arrBytes.size() ) || ( uint32_t ) ( arrBytes.size() ) - nOffset < 4u ) {
		return 0u;
	}
	const uint8_t *pRead = arrBytes.ptr() + nOffset;
	return ( uint32_t ) ( pRead[0] ) | ( ( uint32_t ) ( pRead[1] ) << 8u ) | ( ( uint32_t ) ( pRead[2] ) << 16u ) |
		( ( uint32_t ) ( pRead[3] ) << 24u );
}

static void terrain3dRuntimePageEncodeHeader(
	PackedByteArray &nBytes,
	const t3druntimePageHeader_t &rHeader
) {
	uint32_t offset = 0u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.magic );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.version );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.nHeaderBytes );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.nRegionSize );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.colorDivisor );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.derivedDivisor );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.channelFlags );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.nTotalBytes );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.channelCount );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.channelTableOffset );
	offset += 4u;
	for ( uint32_t channel = 0u; channel < TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT; ++channel ) {
		const t3druntimePageChannelInfo_t &info = rHeader.szChannels[channel];
		terrain3dRuntimePageWriteU32( nBytes, offset, info.type );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.format );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.compression );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.iOffset );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.compressedBytes );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.uncompressedBytes );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.nWidth );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.height );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.mipCount );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.alignment );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.checksum );
		offset += 4u;
		terrain3dRuntimePageWriteU32( nBytes, offset, info.signature );
		offset += 4u;
	}
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.sourceSignatureLo );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.sourceSignatureHi );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.blendRecipeVersion );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.materialMask );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.middleHeightDivisor );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.worldSignatureLo );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.worldSignatureHi );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, ( uint32_t ) ( rHeader.regionX ) );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, ( uint32_t ) ( rHeader.regionZ ) );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.boundsMinYBits );
	offset += 4u;
	terrain3dRuntimePageWriteU32( nBytes, offset, rHeader.boundsMaxYBits );
}

static t3druntimePageHeader_t terrain3dRuntimePageDecodeHeader( const PackedByteArray &arrBytes ) {
	t3druntimePageHeader_t header;
	uint32_t offset = 0u;
	header.magic = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.version = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.nHeaderBytes = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.nRegionSize = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.colorDivisor = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.derivedDivisor = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.channelFlags = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.nTotalBytes = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.channelCount = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.channelTableOffset = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	for ( uint32_t channel = 0u; channel < TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT; ++channel ) {
		t3druntimePageChannelInfo_t &info = header.szChannels[channel];
		info.type = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.format = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.compression = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.iOffset = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.compressedBytes = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.uncompressedBytes = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.nWidth = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.height = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.mipCount = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.alignment = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.checksum = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
		info.signature = terrain3dRuntimePageReadU32( arrBytes, offset );
		offset += 4u;
	}
	header.sourceSignatureLo = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.sourceSignatureHi = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.blendRecipeVersion = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.materialMask = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.middleHeightDivisor = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.worldSignatureLo = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.worldSignatureHi = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.regionX = ( int32_t ) ( terrain3dRuntimePageReadU32( arrBytes, offset ) );
	offset += 4u;
	header.regionZ = ( int32_t ) ( terrain3dRuntimePageReadU32( arrBytes, offset ) );
	offset += 4u;
	header.boundsMinYBits = terrain3dRuntimePageReadU32( arrBytes, offset );
	offset += 4u;
	header.boundsMaxYBits = terrain3dRuntimePageReadU32( arrBytes, offset );
	return header;
}

static bool terrain3dRuntimePageAddChannel(
	PackedByteArray &nFileBytes,
	t3druntimePageHeader_t &rHeader, const t3druntimePageChannel_t channel,
	const Ref<Image> &rImage
) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return false;
	}
	const PackedByteArray image_bytes = rImage->get_data();
	if ( image_bytes.is_empty() || image_bytes.size() > INT32_MAX ) {
		return false;
	}
	const uint32_t aligned_offset = terrain3dRuntimePageAlign( ( uint32_t ) ( nFileBytes.size() ) );
	if ( aligned_offset == 0u || aligned_offset > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ||
		( uint32_t ) ( image_bytes.size() ) > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES - aligned_offset ) {
		return false;
	}
	const uint32_t total_bytes = aligned_offset + ( uint32_t ) ( image_bytes.size() );
	const uint32_t previous_bytes = ( uint32_t ) ( nFileBytes.size() );
	nFileBytes.resize( total_bytes );
	if ( aligned_offset > previous_bytes ) {
		std::memset( nFileBytes.ptrw() + previous_bytes, 0, aligned_offset - previous_bytes );
	}
	std::memcpy( nFileBytes.ptrw() + aligned_offset, image_bytes.ptr(), ( size_t ) ( image_bytes.size() ) );
	if ( rHeader.channelCount >= TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT ||
		terrain3dRuntimePageFindChannel( rHeader, channel ) ) {
		return false;
	}
	t3druntimePageChannelInfo_t &info = rHeader.szChannels[rHeader.channelCount++];
	info.type = ( uint32_t ) ( channel );
	info.format = ( uint32_t ) ( rImage->get_format() );
	info.compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	info.iOffset = aligned_offset;
	info.compressedBytes = ( uint32_t ) ( image_bytes.size() );
	info.uncompressedBytes = ( uint32_t ) ( image_bytes.size() );
	info.checksum = terrain3dRuntimePageChecksum( image_bytes.ptr(), ( size_t ) ( image_bytes.size() ) );
	info.nWidth = ( uint32_t ) ( rImage->get_width() );
	info.height = ( uint32_t ) ( rImage->get_height() );
	info.mipCount = rImage->has_mipmaps() ? ( uint32_t ) ( rImage->get_mipmap_count() + 1 ) : 1u;
	info.alignment = TERRAIN3D_RUNTIME_PAGE_ALIGNMENT;
	info.signature = info.checksum;
	rHeader.nTotalBytes = total_bytes;
	return true;
}

static bool terrain3dRuntimePageAddBlob(
	PackedByteArray &nFileBytes, t3druntimePageHeader_t &rHeader,
	const t3druntimePageChannel_t channel, const PackedByteArray &rPayload
) {
	if ( rPayload.is_empty() || rHeader.channelCount >= TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT ||
		terrain3dRuntimePageFindChannel( rHeader, channel ) ) {
		return false;
	}
	const uint32_t aligned_offset = terrain3dRuntimePageAlign( ( uint32_t ) ( nFileBytes.size() ) );
	if ( aligned_offset == 0u || ( uint64_t ) ( aligned_offset ) + ( uint64_t ) ( rPayload.size() ) >
		TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ) {
		return false;
	}
	const uint32_t previous_bytes = ( uint32_t ) ( nFileBytes.size() );
	const uint32_t payload_bytes = ( uint32_t ) ( rPayload.size() );
	nFileBytes.resize( aligned_offset + payload_bytes );
	if ( aligned_offset > previous_bytes ) {
		std::memset( nFileBytes.ptrw() + previous_bytes, 0, aligned_offset - previous_bytes );
	}
	std::memcpy( nFileBytes.ptrw() + aligned_offset, rPayload.ptr(), payload_bytes );
	t3druntimePageChannelInfo_t &info = rHeader.szChannels[rHeader.channelCount++];
	info.type = ( uint32_t ) ( channel );
	info.format = UINT32_MAX;
	info.iOffset = aligned_offset;
	info.compressedBytes = payload_bytes;
	info.uncompressedBytes = payload_bytes;
	info.nWidth = payload_bytes;
	info.height = 1u;
	info.mipCount = 1u;
	info.checksum = terrain3dRuntimePageChecksum( rPayload.ptr(), payload_bytes );
	info.signature = info.checksum;
	rHeader.nTotalBytes = aligned_offset + payload_bytes;
	return true;
}

static PackedByteArray terrain3dRuntimePageReadBlob(
	const Ref<FileAccess> &rFile, const t3druntimePageChannelInfo_t &rInfo,
	uint64_t &nReadUsec, uint64_t &nValidationUsec
) {
	if ( rFile.is_null() || rInfo.format != UINT32_MAX ||
		rInfo.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ||
		rInfo.compressedBytes == 0u || rInfo.compressedBytes != rInfo.uncompressedBytes ) {
		return PackedByteArray();
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.iOffset );
	PackedByteArray bytes = rFile->get_buffer( rInfo.compressedBytes );
	nReadUsec += ( pTime ? pTime->get_ticks_usec() : read_started ) - read_started;
	const uint64_t validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const bool valid = bytes.size() == ( int64_t ) ( rInfo.compressedBytes ) &&
		terrain3dRuntimePageChecksum( bytes.ptr(), ( size_t ) ( bytes.size() ) ) == rInfo.checksum;
	nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	return valid ? bytes : PackedByteArray();
}

static bool terrain3dRuntimePageExpectedImageBytes(
	const t3druntimePageChannelInfo_t &rInfo, uint64_t &rExpectedBytes
) {
	uint32_t bytes_per_pixel = 0u;
	switch ( Image::Format( rInfo.format ) ) {
		case Image::FORMAT_R16:
		case Image::FORMAT_RG8:
			bytes_per_pixel = 2u;
			break;
		case Image::FORMAT_RF:
		case Image::FORMAT_RGBA8:
			bytes_per_pixel = 4u;
			break;
		default:
			return false;
	}
	if ( rInfo.nWidth == 0u || rInfo.height == 0u || rInfo.mipCount == 0u ) {
		return false;
	}
	uint32_t width = rInfo.nWidth;
	uint32_t height = rInfo.height;
	uint64_t total = 0u;
	for ( ;; ) {
		const uint64_t level_bytes = ( uint64_t ) ( width ) * ( uint64_t ) ( height ) * bytes_per_pixel;
		if ( level_bytes > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES || total > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES - level_bytes ) {
			return false;
		}
		total += level_bytes;
		if ( rInfo.mipCount == 1u || ( width == 1u && height == 1u ) ) {
			break;
		}
		width = MAX( 1u, width >> 1u );
		height = MAX( 1u, height >> 1u );
	}
	rExpectedBytes = total;
	return true;
}

static Ref<Image> terrain3dRuntimePageReadImage(
	const Ref<FileAccess> &rFile,
	const t3druntimePageChannelInfo_t &rInfo, uint64_t &nReadUsec,
	uint64_t &nValidationUsec, uint64_t &nMaterializeUsec,
	PackedByteArray *const pValidatedNativeBytes = nullptr
) {
	if ( rFile.is_null() || rInfo.compressedBytes == 0u ||
		rInfo.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ) {
		return Ref<Image>();
	}
	Time *pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.iOffset );
	const PackedByteArray image_bytes = rFile->get_buffer( rInfo.compressedBytes );
	const uint64_t read_finished = pTime ? pTime->get_ticks_usec() : read_started;
	nReadUsec += read_finished - read_started;
	const uint64_t validation_started = read_finished;
	uint64_t expected_bytes = 0u;
	const bool bPayloadShapeValid = terrain3dRuntimePageExpectedImageBytes( rInfo, expected_bytes ) &&
		expected_bytes == ( uint64_t ) ( rInfo.uncompressedBytes );
	const bool bPayloadValid = bPayloadShapeValid && image_bytes.size() == ( int64_t ) ( rInfo.compressedBytes ) &&
		terrain3dRuntimePageChecksum( image_bytes.ptr(), ( size_t ) ( image_bytes.size() ) ) == rInfo.checksum;
	const uint64_t validation_finished = pTime ? pTime->get_ticks_usec() : validation_started;
	nValidationUsec += validation_finished - validation_started;
	if ( !bPayloadValid ) {
		return Ref<Image>();
	}
	if ( pValidatedNativeBytes ) {
		*pValidatedNativeBytes = image_bytes;
	}
	const uint64_t materialize_started = validation_finished;
	const Ref<Image> pImage = Image::create_from_data( ( int ) ( rInfo.nWidth ), ( int ) ( rInfo.height ),
		rInfo.mipCount > 1u, Image::Format( rInfo.format ), image_bytes );
	const bool bImageValid = pImage.is_valid() && !pImage->is_empty() &&
		pImage->get_data().size() == ( int64_t ) ( rInfo.uncompressedBytes );
	const uint64_t materialize_finished = pTime ? pTime->get_ticks_usec() : materialize_started;
	nMaterializeUsec += materialize_finished - materialize_started;
	if ( !bImageValid ) {
		return Ref<Image>();
	}
	return pImage;
}

bool terrain3dRegionLocationValid( const Vector2i &vLocation, const int nMapSize ) {
	if ( nMapSize <= 0 ) {
		return false;
	}
	const int half = nMapSize / 2;
	return vLocation.x >= -half && vLocation.x < half && vLocation.y >= -half && vLocation.y < half;
}

// Returns the half-open region-coordinate bounds touched by a positive-sized
// area. Computing the exclusive end independently is required for offset and
// negative-coordinate rectangles; ceil(area.size / region_size) is only valid
// when the area starts on a region boundary.
Rect2i getRegionBoundsForArea( const Rect2i &rArea, const int nRegionSize ) {
	if ( nRegionSize <= 0 || rArea.size.x <= 0 || rArea.size.y <= 0 ) {
		return Rect2i();
	}

	const Vector2i begin = V2I_DIVIDE_FLOOR( rArea.position, nRegionSize );
	const Vector2i end = V2I_DIVIDE_CEIL( rArea.get_end(), nRegionSize );
	return Rect2i( begin, end - begin );
}

Rect2i getRegionDirtyRect( const Vector2i &vLocation, const int nRegionSize, const real_t fVertexSpacing,
		const AABB &rEditedArea, const int nMargin = 0 ) {
	const Rect2i bounds( V2I_ZERO, V2I( nRegionSize ) );
	if ( nRegionSize <= 0 || fVertexSpacing <= 0.0f || rEditedArea.size.x <= 0.0f ||
		rEditedArea.size.z <= 0.0f ) {
		return bounds;
	}
	const Vector3 world_end = rEditedArea.get_end();
	const int64_t global_begin_x = ( int64_t ) ( Math::floor( ( double ) ( rEditedArea.position.x / fVertexSpacing ) ) ) - nMargin;
	const int64_t global_begin_z = ( int64_t ) ( Math::floor( ( double ) ( rEditedArea.position.z / fVertexSpacing ) ) ) - nMargin;
	const int64_t global_end_x = ( int64_t ) ( Math::ceil( ( double ) ( world_end.x / fVertexSpacing ) ) ) + nMargin + 1;
	const int64_t global_end_z = ( int64_t ) ( Math::ceil( ( double ) ( world_end.z / fVertexSpacing ) ) ) + nMargin + 1;
	const int64_t region_origin_x = ( int64_t ) ( vLocation.x ) * nRegionSize;
	const int64_t region_origin_z = ( int64_t ) ( vLocation.y ) * nRegionSize;
	const int begin_x = ( int ) ( CLAMP<int64_t>( global_begin_x - region_origin_x, 0, nRegionSize ) );
	const int begin_z = ( int ) ( CLAMP<int64_t>( global_begin_z - region_origin_z, 0, nRegionSize ) );
	const int end_x = ( int ) ( CLAMP<int64_t>( global_end_x - region_origin_x, 0, nRegionSize ) );
	const int end_z = ( int ) ( CLAMP<int64_t>( global_end_z - region_origin_z, 0, nRegionSize ) );
	const Rect2i rect( Vector2i( begin_x, begin_z ), Vector2i( end_x - begin_x, end_z - begin_z ) );
	return rect.size.x > 0 && rect.size.y > 0 ? rect : bounds;
}

typedef struct t3dderivedMapTask_s {
	Vector2i location;
	Rect2i dirty_rect;
	bool available = false;
	bool success = false;
	eastl::vector<uint8_t> output;
} t3dderivedMapTask_t;

typedef struct t3dderivedMapBuildContext_s {
	T3DData *data = nullptr;
	eastl::vector<t3dderivedMapTask_t> *pTasks = nullptr;
	int nRegionSize = 0;
	float fVertexSpacing = 1.0f;
} t3dderivedMapBuildContext_t;

void terrain3dBuildDerivedMapRange( void *pContext, const size_t nBegin, const size_t nEnd, const int32_t ) {
	t3dderivedMapBuildContext_t *context = ( t3dderivedMapBuildContext_t * ) ( pContext );
	T3DSampler sampler( context->data );
	for ( size_t task_index = nBegin; task_index < nEnd; ++task_index ) {
		t3dderivedMapTask_t &task = ( *context->pTasks )[task_index];
		if ( task.success ) {
			continue;
		}
		const int width = task.dirty_rect.size.x;
		const int height = task.dirty_rect.size.y;
		if ( !task.available || width <= 0 || height <= 0 ) {
			continue;
		}
		const int halo_stride = width + 2;
		eastl::vector<float> halo( ( size_t ) ( halo_stride ) * ( size_t ) ( height + 2 ) );
		const int64_t region_base_x = ( int64_t ) ( task.location.x ) * context->nRegionSize;
		const int64_t region_base_z = ( int64_t ) ( task.location.y ) * context->nRegionSize;
		const int64_t halo_grid_x = region_base_x + task.dirty_rect.position.x - 1;
		const int64_t halo_grid_z = region_base_z + task.dirty_rect.position.y - 1;
		sampler.copyHeightRect( halo_grid_x, halo_grid_z, width + 2, height + 2, halo.data(), halo_stride, false );
		for ( int y = -1; y <= height; ++y ) {
			for ( int x = -1; x <= width; ++x ) {
				float &sample = halo[( size_t ) ( y + 1 ) * ( size_t ) ( halo_stride ) + ( size_t ) ( x + 1 )];
				if ( std::isfinite( sample ) ) {
					continue;
				}
				const int local_x = task.dirty_rect.position.x + x;
				const int local_y = task.dirty_rect.position.y + y;
				const int clamped_x = CLAMP( local_x, 0, context->nRegionSize - 1 );
				const int clamped_y = CLAMP( local_y, 0, context->nRegionSize - 1 );
				sample = sampler.sampleHeightGrid( region_base_x + clamped_x, region_base_z + clamped_y, false );
				sample = std::isfinite( sample ) ? sample : 0.0f;
			}
		}
		task.output.resize( ( size_t ) ( width ) * ( size_t ) ( height ) * 2u );
		terrain3d_simd::derivedNormalsRg8( halo.data(), width, height, halo_stride, context->fVertexSpacing,
			task.output.data() );
		task.success = true;
	}
}

} // namespace

///////////////////////////
// Private Functions
///////////////////////////

bool T3DData::setRegionSizeInternal( const int nRegionSize ) {
	if ( nRegionSize != 128 ) {
		LOG( T3DTerrain::ERROR, "This Terrain3D fork requires 128-sample regions. Requested: ", nRegionSize );
		return false;
	}
	if ( !isValidRegionSize( nRegionSize ) || TERRAIN3D_ACTIVE_WORLD_TEXELS % nRegionSize != 0 ) {
		LOG( T3DTerrain::ERROR, "Region size ", nRegionSize, " cannot partition the fixed ", TERRAIN3D_ACTIVE_WORLD_TEXELS,
			"x", TERRAIN3D_ACTIVE_WORLD_TEXELS, " active terrain bank" );
		return false;
	}
	const bool bRegionSizeChanged = m_nRegionSize != nRegionSize;
	const bool bStorageOk = m_bOwnsActiveStorage
		? terrain3dActiveStorageReconfigure( this, nRegionSize )
		: terrain3dActiveStorageClaim( this, nRegionSize );
	if ( !bStorageOk ) {
		LOG( T3DTerrain::ERROR, "Unable to claim process-static active terrain storage. Only one Terrain3DData owner is legal." );
		return false;
	}
	m_bOwnsActiveStorage = true;
	m_nRegionSize = nRegionSize;
	m_regionSizev = V2I( nRegionSize );
	m_nRegionMapSize = TERRAIN3D_ACTIVE_WORLD_TEXELS / nRegionSize;
	if ( bRegionSizeChanged && m_bGpuRegionCacheEnabled ) {
		resetGpuRegionCache( true );
	}
	if ( bRegionSizeChanged && m_bCpuRegionCacheEnabled ) {
		resetCpuRegionCache( false );
		m_nCpuRegionCacheCapacity = calculateCpuRegionCacheCapacity();
	}
	return true;
}

Error T3DData::rehydrateActiveStorage( void ) {
	if ( !m_bOwnsActiveStorage || !terrain3dActiveStorageIsOwner( this ) || m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ) {
		return ERR_UNCONFIGURED;
	}

	// The physical bank is intentionally smaller than the logical world. Clear its
	// descriptors here and let the normal spatial residency pass publish only the
	// current working set. This avoids a synchronous all-world Image -> byte-array
	// rebuild during scene activation.
	if ( !terrain3dActiveStorageReplaceHeightPages( this, m_nRegionSize, nullptr, nullptr, 0u ) ) {
		LOG( T3DTerrain::ERROR, "Failed to reset active terrain page descriptors during scene activation" );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	m_nActiveStorageGeneration = terrain3dActiveStorageGetOwnerGeneration( this );
	m_bActiveHeightResidencyDirty = true;
	return OK;
}

bool T3DData::syncActiveRegion( const Vector2i &vLocation, const T3DRegion *pRegion ) {
	if ( !m_bOwnsActiveStorage ) {
		return false;
	}
	return terrain3dActiveStorageSyncRegion( this, vLocation, pRegion );
}

void T3DData::clear( void ) {
	if ( m_bOwnsActiveStorage ) {
		terrain3dActiveStorageReset( this );
	}
	if ( m_pResidentPool ) {
		m_pResidentPool->resetResidency();
	}
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	LOG( T3DTerrain::INFO, "Clearing data" );
	m_bRegionMapDirty = true;
	std::fill_n( m_szRegionMap, TERRAIN3D_MAX_REGION_SLOTS, 0 );
	m_regions.clear();
	std::fill_n( m_szRegionPtrMap, TERRAIN3D_MAX_REGION_SLOTS, nullptr );
	m_nRegionLocationCount = 0;
	m_nFreeRegionSlotCount = 0;
	m_nActiveRegionCount = 0;
	m_fDerivedHaloScratch.clear();
	m_activeLocationScratch.clear();
	m_derivedLocationScratch.clear();
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_pDefaultHeightMap.unref();
	m_pDefaultControlMap.unref();
	m_pDefaultColorMap.unref();
	m_pDefaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_editedArea = AABB();
	m_masterHeightRange = V2_ZERO;
	m_generatedHeightMaps.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();
	m_generatedMiddleCompositeMaps.clear();
	resetGpuRegionCache( false );
	m_generatedFarHeightMap.clear();
	m_generatedFarColorMap.clear();
	m_pGpuFarHeightImage.unref();
	m_pGpuFarColorImage.unref();
	m_gpuFarHeightDirtyRegions.reset();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarHeightDirty = true;
	m_bGpuFarColorDirty = true;
	m_bGpuFarHeightDirtyAll = true;
	m_bGpuFarColorDirtyAll = true;
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
	m_nGpuFarHeightDirtyTexelsLast = 0u;
	m_nGpuFarColorDirtyTexelsLast = 0u;
	m_nGpuFarHeightRebuiltBytesLast = 0u;
	m_nGpuFarColorRebuiltBytesLast = 0u;
	m_nGpuFarHeightUploadedBytesLast = 0u;
	m_nGpuFarColorUploadedBytesLast = 0u;
	m_nGpuFarHeightPublicationUsecLast = 0u;
	m_nGpuFarColorPublicationUsecLast = 0u;
	m_nGpuFarHeightPublicationCount = 0u;
	m_nGpuFarColorPublicationCount = 0u;
	m_nGpuFarHeightUploadedBytesTotal = 0u;
	m_nGpuFarColorUploadedBytesTotal = 0u;
	invalidateSamplingCache();
}

void T3DData::invalidateSamplingCache( void ) {
	const uint64_t next = m_nSamplingGeneration.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( next == 0u ) {
		m_nSamplingGeneration.store( 1u, std::memory_order_release );
	}
}

void T3DData::publishRegionSamplingGeneration( const Vector2i &vLocation ) {
	const int index = getRegionMapIndex( vLocation );
	if ( index < 0 ) {
		return;
	}
	std::atomic<uint32_t> &generation = m_szRegionSamplingGenerations[( size_t ) ( index )];
	const uint32_t next = generation.fetch_add( 1u, std::memory_order_acq_rel ) + 1u;
	if ( next == 0u ) {
		generation.store( 1u, std::memory_order_release );
	}
}

void T3DData::ensureDefaultMaps( void ) {
	if ( m_nRegionSize <= 0 ) {
		return;
	}
	if ( m_pDefaultHeightMap.is_null() || m_pDefaultHeightMap->get_width() != m_nRegionSize ) {
		m_pDefaultHeightMap = Util::getFilledImage( m_regionSizev, Terrain3DRegionCodec::defaultHeightColor(), false,
									 Image::FORMAT_R16 );
	}
	if ( m_pDefaultControlMap.is_null() || m_pDefaultControlMap->get_width() != m_nRegionSize ) {
		m_pDefaultControlMap = Util::getFilledImage( m_regionSizev, COLOR_CONTROL, false, Image::FORMAT_RF );
	}
	if ( m_pDefaultColorMap.is_null() || m_pDefaultColorMap->get_width() != m_nRegionSize ) {
		m_pDefaultColorMap = Util::getFilledImage( m_regionSizev, COLOR_ROUGHNESS, false, Image::FORMAT_RGBA8 );
	}
	if ( m_pDefaultDerivedMap.is_null() || m_pDefaultDerivedMap->get_width() != m_nRegionSize ) {
		m_pDefaultDerivedMap = Util::getFilledImage( m_regionSizev, Color( 0.5f, 0.5f, 0.0f, 1.0f ), false,
									  Image::FORMAT_RG8 );
	}
}

Ref<Image> T3DData::getRegionMapOrDefault( const T3DRegion *pRegion, const t3dmapType_t eMapType ) const {
	if ( pRegion && !pRegion->isDeleted() ) {
		Ref<Image> pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
		}
	}
	switch ( eMapType ) {
		case T3DRegion::TYPE_HEIGHT:
			return m_pDefaultHeightMap;
		case T3DRegion::TYPE_CONTROL:
			return m_pDefaultControlMap;
		case T3DRegion::TYPE_COLOR:
			return m_pDefaultColorMap;
		default:
			return Ref<Image>();
	}
}

bool T3DData::hasMaterializedMap( const t3dmapType_t eMapType ) const {
	if ( eMapType != T3DRegion::TYPE_CONTROL && eMapType != T3DRegion::TYPE_COLOR ) {
		return true;
	}
	for ( const T3DRegion *region : m_szRegionPtrMap ) {
		if ( region && !region->isDeleted() && region->hasChannel( eMapType ) ) {
			return true;
		}
	}
	return false;
}

bool T3DData::refreshImplicitMapState( void ) {
	const Ref<T3DMaterial> pMaterial = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	const bool bForceFullArrays = pMaterial.is_valid() &&
		( pMaterial->isShaderOverrideEnabled() || pMaterial->isBufferShaderOverrideEnabled() );
	const bool bControlImplicit = !bForceFullArrays && !hasMaterializedMap( T3DRegion::TYPE_CONTROL );
	const bool bColorImplicit = !bForceFullArrays && !hasMaterializedMap( T3DRegion::TYPE_COLOR );
	const bool bChanged = bControlImplicit != m_bControlMapsImplicit || bColorImplicit != m_bColorMapsImplicit;
	if ( bControlImplicit != m_bControlMapsImplicit ) {
		m_bControlMapsImplicit = bControlImplicit;
		m_generatedControlMaps.clear();
	}
	if ( bColorImplicit != m_bColorMapsImplicit ) {
		m_bColorMapsImplicit = bColorImplicit;
		m_generatedColorMaps.clear();
	}
	return bChanged;
}

Ref<Image> T3DData::makeGpuColorImage( const Ref<Image> &rImage ) const {
	Ref<Image> pSource = rImage.is_valid() ? rImage : m_pDefaultColorMap;
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	if ( divisor <= 1 && pSource->has_mipmaps() ) {
		return pSource;
	}
	Ref<Image> pGpuImage;
	pGpuImage.instantiate();
	pGpuImage->copy_from( pSource );
	// Resizing an image that already owns a mip chain and then generating mips again
	// can carry stale lower levels through rehydration. Rebuild from the base level
	// exactly once so the GPU page always contains the canonical chain for its size.
	if ( pGpuImage->has_mipmaps() ) {
		pGpuImage->clear_mipmaps();
	}
	if ( divisor > 1 ) {
		pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ),
			MAX( 1, pSource->get_height() / divisor ), Image::INTERPOLATE_BILINEAR );
	}
	if ( pGpuImage->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pGpuImage;
}

Ref<Image> T3DData::makeGpuDerivedImage( const Ref<Image> &rImage ) const {
	Ref<Image> pSource = rImage.is_valid() ? rImage : m_pDefaultDerivedMap;
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	if ( divisor <= 1 ) {
		return pSource;
	}
	Ref<Image> pGpuImage;
	pGpuImage.instantiate();
	pGpuImage->copy_from( pSource );
	pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ),
		MAX( 1, pSource->get_height() / divisor ), Image::INTERPOLATE_BILINEAR );
	return pGpuImage;
}

Ref<Image> T3DData::makeMiddleCompositeImage(
	const Vector2i &rLocation, const Ref<Image> &rControl, const Ref<Image> &rColor
) const {
	if ( rControl.is_null() || rColor.is_null() || rControl->get_format() != Image::FORMAT_RF ||
		rColor->get_format() != Image::FORMAT_RGBA8 ) {
		return Ref<Image>();
	}
	const int width = rColor->get_width();
	const int height = rColor->get_height();
	if ( width <= 0 || height <= 0 ) {
		return Ref<Image>();
	}
	const PackedByteArray control_bytes = rControl->get_data();
	const PackedByteArray color_bytes = rColor->get_data();
	if ( control_bytes.size() < ( int64_t ) ( rControl->get_width() ) * rControl->get_height() * 4 ||
		color_bytes.size() < ( int64_t ) ( width ) * height * 4 ) {
		return Ref<Image>();
	}
	PackedColorArray material_colors;
	const Ref<T3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<T3DAssets>();
	if ( pAssets.is_valid() ) {
		material_colors = pAssets->getTextureColors();
	}
	const PackedFloat32Array material_scales = pAssets.is_valid() ?
		pAssets->getTextureUvScales() : PackedFloat32Array();
	eastl::array<Ref<Image>, TERRAIN3D_MAX_TEXTURE_ASSETS> material_images;
	if ( pAssets.is_valid() ) {
		for ( int material = 0; material < MIN( pAssets->getTextureCount(), TERRAIN3D_MAX_TEXTURE_ASSETS ); ++material ) {
			const Ref<T3DTextureAsset> pAsset = pAssets->getTextureAsset( material );
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getAlbedoTexture() : Ref<Texture2D>();
			Ref<Image> pMaterialImage = pTexture.is_valid() ? pTexture->get_image() : Ref<Image>();
			// Imported DDS/VRAM textures commonly expose compressed Images. get_pixel()
			// rejects those and logs once per sample, so prepare one private readable
			// copy per material before entering the composite's texel loops. If the
			// platform cannot decompress that format, the bake falls back to its tint.
			if ( pMaterialImage.is_valid() && pMaterialImage->is_compressed() ) {
				pMaterialImage = pMaterialImage->duplicate();
				if ( pMaterialImage->decompress() != OK ) {
					pMaterialImage.unref();
				}
			}
			if ( pMaterialImage.is_valid() && pMaterialImage->is_empty() ) {
				pMaterialImage.unref();
			}
			material_images[( size_t ) ( material )] = pMaterialImage;
		}
	}
	PackedByteArray output;
	output.resize( width * height * 4 );
	uint8_t *const pWrite = output.ptrw();
	const uint8_t *const pControl = control_bytes.ptr();
	const uint8_t *const pColor = color_bytes.ptr();
	for ( int y = 0; y < height; ++y ) {
		const int control_y = MIN( rControl->get_height() - 1, y * rControl->get_height() / height );
		for ( int x = 0; x < width; ++x ) {
			const int control_x = MIN( rControl->get_width() - 1, x * rControl->get_width() / width );
			const uint32_t control = terrain3dReadU32Unaligned( pControl,
				( size_t ) ( control_y * rControl->get_width() + control_x ) * 4u );
			const int base = ( int ) ( getBase( control ) );
			const int overlay = ( int ) ( getOverlay( control ) );
			const Color base_color = base < material_colors.size() ? material_colors[base] : COLOR_WHITE;
			const Color overlay_color = overlay < material_colors.size() ? material_colors[overlay] : base_color;
			const float control_scale = 0.9f - ( float ) ( ( ( getUvScale( control ) + 3u ) % 8u ) + 1u ) * 0.1f;
			const float angle = ( float ) ( getUvRotation( control ) ) * Math_TAU / 16.0f;
			const float cs = Math::cos( angle );
			const float sn = Math::sin( angle );
			const float local_x = ( ( float ) ( x ) + 0.5f ) * ( float ) ( m_nRegionSize ) / ( float ) ( width );
			const float local_y = ( ( float ) ( y ) + 0.5f ) * ( float ) ( m_nRegionSize ) / ( float ) ( height );
			const float world_x = ( ( float ) ( rLocation.x * m_nRegionSize ) + local_x ) * ( float ) ( m_fVertexSpacing );
			const float world_y = ( ( float ) ( rLocation.y * m_nRegionSize ) + local_y ) * ( float ) ( m_fVertexSpacing );
			auto sample_material = [&]( const int material_id, const Color &rTint ) -> Color {
				if ( material_id < 0 || material_id >= TERRAIN3D_MAX_TEXTURE_ASSETS ||
					material_images[( size_t ) ( material_id )].is_null() ) {
					return rTint;
				}
				const Ref<Image> &pImage = material_images[( size_t ) ( material_id )];
				const float asset_scale = material_id < material_scales.size() ? material_scales[material_id] : 0.1f;
				const float source_u = ( cs * world_x + sn * world_y ) * control_scale * asset_scale;
				const float source_v = ( cs * world_y - sn * world_x ) * control_scale * asset_scale;
				const float wrapped_u = source_u - Math::floor( source_u );
				const float wrapped_v = source_v - Math::floor( source_v );
				const int source_x = CLAMP( ( int ) ( wrapped_u * pImage->get_width() ), 0, pImage->get_width() - 1 );
				const int source_y = CLAMP( ( int ) ( wrapped_v * pImage->get_height() ), 0, pImage->get_height() - 1 );
				return pImage->get_pixel( source_x, source_y ) * rTint;
			};
			const Color base_sample = sample_material( base, base_color );
			const Color overlay_sample = sample_material( overlay, overlay_color );
			float blend = ( float ) ( getBlend( control ) ) / 255.0f;
			blend = CLAMP( ( blend - 0.12f ) / 0.76f, 0.0f, 1.0f );
			blend = blend * blend * ( 3.0f - 2.0f * blend );
			const Color material_color = base_sample.lerp( overlay_sample, blend );
			const size_t offset = ( size_t ) ( y * width + x ) * 4u;
			const float authored_r = ( float ) ( pColor[offset] ) / 255.0f;
			const float authored_g = ( float ) ( pColor[offset + 1u] ) / 255.0f;
			const float authored_b = ( float ) ( pColor[offset + 2u] ) / 255.0f;
			pWrite[offset] = terrain3dColorToByte( authored_r * material_color.r );
			pWrite[offset + 1u] = terrain3dColorToByte( authored_g * material_color.g );
			pWrite[offset + 2u] = terrain3dColorToByte( authored_b * material_color.b );
			pWrite[offset + 3u] = isHole( control ) ? 0u :
				( uint8_t ) ( 0x80u | ( ( uint32_t ) ( pColor[offset + 3u] ) * 127u + 127u ) / 255u );
		}
	}
	Ref<Image> pComposite = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, output );
	if ( pComposite.is_null() || pComposite->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pComposite;
}

Ref<Image> T3DData::makeGpuRenderControlImage( const Vector2i &vLocation ) const {
	eastl::array<t3dcontrolCookSource_t, 9> sources;
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = vLocation + Vector2i( dx, dz );
			const int source_index = ( dz + 1 ) * 3 + ( dx + 1 );
			t3dcontrolCookSource_t &source = sources[( size_t ) ( source_index )];
			const T3DRegion *pRegion = getRegionPtr( location );
			Ref<T3DRegion> pLoadedSource;
			const bool bNeedsSourceLoad = !pRegion || pRegion->isDeleted() ||
				( pRegion->hasChannel( T3DRegion::T3DRegion::TYPE_CONTROL ) && pRegion->getControlMap().is_null() );
			if ( bNeedsSourceLoad && loadCpuRegionSource( location, pLoadedSource ) ) {
				pRegion = pLoadedSource.ptr();
			}
			if ( !pRegion || pRegion->isDeleted() ) {
				continue;
			}
			source.bPresent = true;
			if ( !pRegion->hasChannel( T3DRegion::T3DRegion::TYPE_CONTROL ) || pRegion->getControlMap().is_null() ) {
				source.bImplicit = true;
				continue;
			}
			const Ref<Image> pControl = pRegion->getControlMap();
			if ( pControl->get_width() != m_nRegionSize || pControl->get_height() != m_nRegionSize ||
				pControl->get_format() != Image::FORMAT_RF || pControl->has_mipmaps() ) {
				return Ref<Image>();
			}
			source.nBytes = pControl->get_data();
			if ( source.nBytes.size() != m_nRegionSize * m_nRegionSize * ( int ) ( sizeof( uint32_t ) ) ) {
				return Ref<Image>();
			}
			source.bImplicit = false;
		}
	}


	PackedByteArray output;
	output.resize( m_nRegionSize * m_nRegionSize * ( int ) ( sizeof( uint32_t ) ) );
	uint8_t *const pWrite = output.ptrw();
	for ( int z = 0; z < m_nRegionSize; ++z ) {
		for ( int x = 0; x < m_nRegionSize; ++x ) {
			uint32_t center = TERRAIN3D_REGION_DEFAULT_CONTROL;
			if ( !terrain3dReadCookControl( sources, m_nRegionSize, x, z, center ) ) {
				std::memcpy( pWrite + ( size_t ) ( z * m_nRegionSize + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}
			if ( isAuto( center ) || isHole( center ) ) {
				std::memcpy( pWrite + ( size_t ) ( z * m_nRegionSize + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}

			eastl::array<uint64_t, TERRAIN3D_MAX_TEXTURE_ASSETS> material_weights{};
			for ( int dz = -TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; dz <= TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; ++dz ) {
				for ( int dx = -TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; dx <= TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; ++dx ) {
					uint32_t pControl = TERRAIN3D_REGION_DEFAULT_CONTROL;
					if ( !terrain3dReadCookControl( sources, m_nRegionSize, x + dx, z + dz, pControl ) || isAuto( pControl ) || isHole( pControl ) ) {
						continue;
					}
					const uint64_t spatial_weight = ( uint64_t ) ( TERRAIN3D_RENDER_CONTROL_KERNEL[dx + TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS] * TERRAIN3D_RENDER_CONTROL_KERNEL[dz + TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS] );
					const uint8_t base = getBase( pControl );
					const uint8_t overlay = getOverlay( pControl );
					const uint8_t blend = getBlend( pControl );
					if ( base == overlay ) {
						material_weights[( size_t ) ( base )] += spatial_weight * 255u;
					} else {
						material_weights[( size_t ) ( base )] += spatial_weight * ( uint64_t ) ( 255u - blend );
						material_weights[( size_t ) ( overlay )] += spatial_weight * ( uint64_t ) ( blend );
					}
				}
			}

			int strongest = -1;
			int second = -1;
			for ( int material = 0; material < TERRAIN3D_MAX_TEXTURE_ASSETS; ++material ) {
				if ( strongest < 0 || material_weights[( size_t ) ( material )] > material_weights[( size_t ) ( strongest )] ) {
					second = strongest;
					strongest = material;
				} else if ( second < 0 || material_weights[( size_t ) ( material )] > material_weights[( size_t ) ( second )] ) {
					second = material;
				}
			}
			if ( strongest < 0 || material_weights[( size_t ) ( strongest )] == 0u ) {
				std::memcpy( pWrite + ( size_t ) ( z * m_nRegionSize + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}
			if ( second < 0 || material_weights[( size_t ) ( second )] == 0u ) {
				second = strongest;
			}

			// Runtime controls use a canonical pair order so neighboring fragments that
			// contain the same two materials address the same texture-array layers in the
			// same order. This materially improves wave/warp texture-cache coherence.
			// The runtime blend shader is pair-order symmetric, so no authored semantic
			// role is lost by sorting the compiled representation.
			int base = strongest;
			int overlay = second;
			if ( base > overlay ) {
				std::swap( base, overlay );
			}
			const uint64_t base_weight = material_weights[( size_t ) ( base )];
			const uint64_t overlay_weight = material_weights[( size_t ) ( overlay )];
			const uint64_t pair_weight = base_weight + overlay_weight;
			uint8_t blend = 0u;
			if ( base != overlay && pair_weight > 0u ) {
				blend = ( uint8_t ) ( CLAMP( ( int ) ( ( overlay_weight * 255u + pair_weight / 2u ) / pair_weight ), 0, 255 ) );
			}
			const uint32_t cooked = ( center & ~TERRAIN3D_RENDER_CONTROL_MATERIAL_MASK ) | encBase( ( uint8_t ) ( base ) ) |
				encOverlay( ( uint8_t ) ( overlay ) ) | encBlend( blend );
			std::memcpy( pWrite + ( size_t ) ( z * m_nRegionSize + x ) * sizeof( uint32_t ), &cooked, sizeof( uint32_t ) );
		}
	}
	return Image::create_from_data( m_nRegionSize, m_nRegionSize, false, Image::FORMAT_RF, output );
}

uint64_t T3DData::computeGpuRuntimePageSourceSignature( const T3DRegion *pRegion ) const {
	if ( !pRegion ) {
		return 0u;
	}
	uint64_t hash = 1469598103934665603ull;
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_nRegionSize ) );
	terrain3dFnv1a64AppendF32( hash, ( float ) ( m_fVertexSpacing ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_nGpuRegionColorDivisor ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_nGpuRegionDerivedDivisor ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_pTerrain ? MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() ) : 1 ) );
	terrain3dFnv1a64AppendU32( hash, TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION );
	const Ref<T3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<T3DAssets>();
	terrain3dFnv1a64AppendU64( hash, pAssets.is_valid() ? pAssets->getRuntimeMaterialSourceSignature() : 0u );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( pRegion->getLocation().x ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( pRegion->getLocation().y ) );

	Ref<T3DRegion> pLoadedOwner;
	const T3DRegion *pOwner = pRegion;
	if ( pOwner->getHeightMap().is_null() && loadCpuRegionSource( pOwner->getLocation(), pLoadedOwner ) ) {
		pOwner = pLoadedOwner.ptr();
	}
	if ( !pOwner ) {
		return 0u;
	}
	const Ref<Image> pHeight = pOwner->getHeightMap();
	if ( pHeight.is_valid() ) {
		const PackedByteArray bytes = pHeight->get_data();
		terrain3dFnv1a64Append( hash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
	}
	const Ref<Image> pColor = pOwner->getColorMap();
	if ( pColor.is_valid() ) {
		const PackedByteArray bytes = pColor->get_data();
		terrain3dFnv1a64Append( hash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
	}
	const PackedByteArray instance_page = pOwner->getInstancePage();
	if ( !instance_page.is_empty() ) {
		terrain3dFnv1a64Append( hash, instance_page.ptr(), ( size_t ) ( instance_page.size() ) );
	}

	// Derived normals use forward differences, so this page depends only on the
	// right neighbor's first height column and the bottom neighbor's first row.
	// Hash only those boundary samples instead of whole neighboring height maps.
	const Vector2i height_dependency_offsets[2] = { Vector2i( 1, 0 ), Vector2i( 0, 1 ) };
	for ( int dependency = 0; dependency < 2; ++dependency ) {
		const Vector2i location = pRegion->getLocation() + height_dependency_offsets[dependency];
		const T3DRegion *region = getRegionPtr( location );
		Ref<T3DRegion> pLoadedSource;
		if ( ( !region || region->getHeightMap().is_null() ) && loadCpuRegionSource( location, pLoadedSource ) ) {
			region = pLoadedSource.ptr();
		}
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( dependency ) );
		if ( !region || region->isDeleted() ||
			!terrain3dFnv1a64AppendR16HeightEdge( hash, region->getHeightMap(), m_nRegionSize, dependency == 0 ) ) {
			terrain3dFnv1a64AppendU32( hash, 0u );
			continue;
		}
		terrain3dFnv1a64AppendU32( hash, 1u );
	}

	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = pRegion->getLocation() + Vector2i( dx, dz );
			const T3DRegion *region = getRegionPtr( location );
			Ref<T3DRegion> pLoadedSource;
			if ( ( !region || region->getControlMap().is_null() ) && loadCpuRegionSource( location, pLoadedSource ) ) {
				region = pLoadedSource.ptr();
			}
			terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( dx + 1 ) );
			terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( dz + 1 ) );
			if ( !region || region->isDeleted() ) {
				terrain3dFnv1a64AppendU32( hash, 0u );
				continue;
			}
			terrain3dFnv1a64AppendU32( hash, 1u );
			const Ref<Image> pControl = region->getControlMap();
			if ( pControl.is_valid() ) {
				const PackedByteArray bytes = pControl->get_data();
				terrain3dFnv1a64Append( hash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
			} else {
				terrain3dFnv1a64AppendU32( hash, TERRAIN3D_REGION_DEFAULT_CONTROL );
			}
		}
	}
	return hash;
}

bool T3DData::readGpuRuntimePageSourceSignature( const String &rPagePath, uint64_t &nSignature ) const {
	nSignature = 0u;
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return false;
	}
	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) {
		return false;
	}
	const PackedByteArray header_bytes = pFile->get_buffer( TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES );
	if ( header_bytes.size() != TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) {
		return false;
	}
	const t3druntimePageHeader_t header = terrain3dRuntimePageDecodeHeader( header_bytes );
	if ( !terrain3dRuntimePageValidateHeader( header, pFile->get_length() ) ) {
		return false;
	}
	nSignature = ( uint64_t ) ( header.sourceSignatureLo ) | ( ( uint64_t ) ( header.sourceSignatureHi ) << 32u );
	return nSignature != 0u;
}

String T3DData::getGpuRuntimePagePath( const T3DRegion *pRegion ) const {
	if ( !pRegion ) {
		return String();
	}
	if ( m_pTerrain && !m_pTerrain->getDataDirectory().is_empty() ) {
		const String source_path = m_pTerrain->getDataDirectory().trim_suffix( "/" ) + String( "/" ) +
			Util::locationToFilename( pRegion->getLocation() );
		return source_path.get_basename() + String( ".t3dpage" );
	}
	if ( pRegion->get_path().is_empty() ) {
		return String();
	}
	return pRegion->get_path().get_basename() + String( ".t3dpage" );
}

bool T3DData::loadGpuRuntimePageSnapshot( t3dpreparedGpuRuntimePage_t &rPage ) {
	rPage.height.unref();
	rPage.heightBytes.clear();
	rPage.pControl.unref();
	rPage.pColor.unref();
	rPage.pDerived.unref();
	rPage.pMiddleHeight.unref();
	rPage.pMiddleComposite.unref();
	rPage.instancePage.clear();
	rPage.materialMask = 1u;
	rPage.nFileOpenUsec = 0u;
	rPage.nFileReadUsec = 0u;
	rPage.nValidationUsec = 0u;
	rPage.nMaterializeUsec = 0u;
	Time *const pTime = Time::get_singleton();
	uint64_t validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	if ( rPage.pagePath.is_empty() || !FileAccess::file_exists( rPage.pagePath ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	const uint64_t page_modified = FileAccess::get_modified_time( rPage.pagePath );
	const uint64_t source_modified = rPage.sourcePath.is_empty() ? 0u :
		FileAccess::get_modified_time( rPage.sourcePath );
	if ( source_modified > 0u && page_modified > 0u && page_modified < source_modified ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	const uint64_t open_started = pTime ? pTime->get_ticks_usec() : 0u;
	const Ref<FileAccess> pFile = FileAccess::open( rPage.pagePath, FileAccess::READ );
	const uint64_t open_finished = pTime ? pTime->get_ticks_usec() : open_started;
	rPage.nFileOpenUsec += open_finished - open_started;
	if ( pFile.is_null() ) {
		return false;
	}
	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const uint64_t file_length = pFile->get_length();
	if ( file_length < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ||
		file_length > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES || file_length > ( uint64_t ) ( INT32_MAX ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	const uint64_t header_read_started = pTime ? pTime->get_ticks_usec() : 0u;
	const PackedByteArray header_bytes = pFile->get_buffer( TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES );
	const uint64_t header_read_finished = pTime ? pTime->get_ticks_usec() : header_read_started;
	rPage.nFileReadUsec += header_read_finished - header_read_started;
	if ( header_bytes.size() != TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) {
		return false;
	}
	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const t3druntimePageHeader_t header = terrain3dRuntimePageDecodeHeader( header_bytes );
	if ( !terrain3dRuntimePageValidateHeader( header, file_length ) ||
		header.nRegionSize != ( uint32_t ) ( rPage.nRegionSize ) ||
		header.colorDivisor != ( uint32_t ) ( rPage.colorDivisor ) ||
		header.derivedDivisor != ( uint32_t ) ( rPage.derivedDivisor ) ||
		header.middleHeightDivisor != ( uint32_t ) ( rPage.middleHeightDivisor ) ||
		header.regionX != rPage.location.x || header.regionZ != rPage.location.y ||
		header.blendRecipeVersion != TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION ||
		( header.sourceSignatureLo == 0u && header.sourceSignatureHi == 0u ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	const uint32_t optional_mask = ( uint32_t ) ( T3DRegion::CHANNEL_CONTROL | T3DRegion::CHANNEL_COLOR );
	const uint32_t expected_flags = rPage.expectedChannelFlags & optional_mask;
	if ( ( header.channelFlags & optional_mask ) != expected_flags ||
		( header.channelFlags & ( uint32_t ) ( T3DRegion::CHANNEL_DERIVED ) ) == 0u ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;

	const t3druntimePageChannelInfo_t *const pHeightChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_HEIGHT );
	const t3druntimePageChannelInfo_t *const pDerivedChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_DERIVED );
	const t3druntimePageChannelInfo_t *const pMiddleHeightChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT );
	const t3druntimePageChannelInfo_t *const pMiddleCompositeChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE );
	if ( !pHeightChannel || !pDerivedChannel || !pMiddleHeightChannel || !pMiddleCompositeChannel ) {
		return false;
	}
	rPage.height = terrain3dRuntimePageReadImage( pFile,
		*pHeightChannel, rPage.nFileReadUsec,
		rPage.nValidationUsec, rPage.nMaterializeUsec, &rPage.heightBytes );
	rPage.pDerived = terrain3dRuntimePageReadImage( pFile,
		*pDerivedChannel, rPage.nFileReadUsec,
		rPage.nValidationUsec, rPage.nMaterializeUsec );
	rPage.pMiddleHeight = terrain3dRuntimePageReadImage( pFile,
		*pMiddleHeightChannel, rPage.nFileReadUsec,
		rPage.nValidationUsec, rPage.nMaterializeUsec );
	rPage.pMiddleComposite = terrain3dRuntimePageReadImage( pFile,
		*pMiddleCompositeChannel, rPage.nFileReadUsec,
		rPage.nValidationUsec, rPage.nMaterializeUsec );
	rPage.materialMask = header.materialMask;
	if ( ( expected_flags & ( uint32_t ) ( T3DRegion::CHANNEL_CONTROL ) ) != 0u ) {
		const t3druntimePageChannelInfo_t *const pControlChannel =
			terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_CONTROL );
		if ( !pControlChannel ) {
			return false;
		}
		rPage.pControl = terrain3dRuntimePageReadImage( pFile,
			*pControlChannel, rPage.nFileReadUsec,
			rPage.nValidationUsec, rPage.nMaterializeUsec );
	}
	if ( ( expected_flags & ( uint32_t ) ( T3DRegion::CHANNEL_COLOR ) ) != 0u ) {
		const t3druntimePageChannelInfo_t *const pColorChannel =
			terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLOR );
		if ( !pColorChannel ) {
			return false;
		}
		rPage.pColor = terrain3dRuntimePageReadImage( pFile,
			*pColorChannel, rPage.nFileReadUsec,
			rPage.nValidationUsec, rPage.nMaterializeUsec );
	}
	const t3druntimePageChannelInfo_t *const pInstanceChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES );
	if ( pInstanceChannel ) {
		rPage.instancePage = terrain3dRuntimePageReadBlob(
			pFile, *pInstanceChannel, rPage.nFileReadUsec, rPage.nValidationUsec );
		if ( rPage.instancePage.is_empty() ) {
			return false;
		}
	}
	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const int color_size = MAX( 1, rPage.nRegionSize / rPage.colorDivisor );
	const int derived_size = MAX( 1, rPage.nRegionSize / rPage.derivedDivisor );
	const int middle_height_size = MAX( 1, ( rPage.nRegionSize + rPage.middleHeightDivisor - 1 ) / rPage.middleHeightDivisor );
	if ( rPage.height.is_null() || rPage.height->get_width() != rPage.nRegionSize ||
		rPage.height->get_height() != rPage.nRegionSize ||
		rPage.height->get_format() != Image::FORMAT_R16 || rPage.height->has_mipmaps() ||
		rPage.pDerived.is_null() || rPage.pDerived->get_width() != derived_size ||
		rPage.pDerived->get_height() != derived_size || rPage.pDerived->get_format() != Image::FORMAT_RG8 ||
		rPage.pDerived->has_mipmaps() || rPage.pMiddleHeight.is_null() ||
		rPage.pMiddleHeight->get_width() != middle_height_size ||
		rPage.pMiddleHeight->get_height() != middle_height_size ||
		rPage.pMiddleHeight->get_format() != Image::FORMAT_R16 ||
		rPage.pMiddleHeight->has_mipmaps() || rPage.pMiddleComposite.is_null() ||
		rPage.pMiddleComposite->get_width() != color_size ||
		rPage.pMiddleComposite->get_height() != color_size ||
		rPage.pMiddleComposite->get_format() != Image::FORMAT_RGBA8 ||
		!rPage.pMiddleComposite->has_mipmaps() ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	if ( rPage.pControl.is_valid() && ( rPage.pControl->get_width() != rPage.nRegionSize ||
		rPage.pControl->get_height() != rPage.nRegionSize || rPage.pControl->get_format() != Image::FORMAT_RF ||
		rPage.pControl->has_mipmaps() ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	if ( rPage.pColor.is_valid() && ( rPage.pColor->get_width() != color_size ||
		rPage.pColor->get_height() != color_size || rPage.pColor->get_format() != Image::FORMAT_RGBA8 ||
		!rPage.pColor->has_mipmaps() ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	return true;
}

bool T3DData::loadGpuRuntimePage(
	T3DRegion *const pRegion, Ref<Image> &rHeight,
	Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived
) {
	rHeight.unref();
	rControl.unref();
	rColor.unref();
	rDerived.unref();
	if ( !pRegion || !m_pTerrain || !m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		return false;
	}
	t3dpreparedGpuRuntimePage_t page;
	page.location = pRegion->getLocation();
	page.pagePath = getGpuRuntimePagePath( pRegion );
	page.sourcePath = pRegion->get_path();
	page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
	page.nRegionSize = m_nRegionSize;
	page.colorDivisor = m_nGpuRegionColorDivisor;
	page.derivedDivisor = m_nGpuRegionDerivedDivisor;
	page.middleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
	page.bAttempted = true;
	page.bLoaded = loadGpuRuntimePageSnapshot( page );
	if ( page.bLoaded ) {
		rHeight = page.height;
		rControl = page.pControl;
		rColor = page.pColor;
		rDerived = page.pDerived;
	}
	return page.bLoaded;
}

void T3DData::prepareGpuRuntimePageTask( void *const pContext, const uint32_t iIndex ) {
	t3dgpuPrepareJobContext_t *const context = ( t3dgpuPrepareJobContext_t * ) ( pContext );
	if ( !context || !context->pPages || iIndex >= ( uint32_t ) ( context->nPageCount ) ) {
		return;
	}
	t3dpreparedGpuRuntimePage_t &page = context->pPages[iIndex];
	Time *const pTime = Time::get_singleton();
	page.nWorkerStartedUsec = pTime ? pTime->get_ticks_usec() : page.nRequestedUsec;
	page.bAttempted = true;
	page.bLoaded = loadGpuRuntimePageSnapshot( page );
	page.bInstanceAttempted = !page.instancePage.is_empty();
	if ( page.bInstanceAttempted ) {
		uint32_t mesh_counts[TERRAIN3D_MAX_MESH_ASSETS] = {};
		uint32_t instance_count = 0u;
		uint16_t mesh_count = 0u;
		page.bInstanceLoaded = T3DInstancePageCodec::decode( page.instancePage, page.preparedInstanceStore,
			mesh_counts, instance_count, mesh_count );
	}
	page.nWorkerFinishedUsec = pTime ? pTime->get_ticks_usec() : page.nWorkerStartedUsec;
}

void T3DData::pollGpuRuntimePagePrepare( const bool bWait ) {
	if ( !m_bGpuPrepareJobActive ) {
		return;
	}
	WorkerThreadPool *const pool = WorkerThreadPool::get_singleton();
	if ( pool && !bWait && !pool->is_group_task_completed( m_nGpuPrepareGroupId ) ) {
		return;
	}
	if ( pool && m_nGpuPrepareGroupId != WorkerThreadPool::INVALID_TASK_ID ) {
		pool->wait_for_group_task_completion( m_nGpuPrepareGroupId );
	}
	m_nGpuPrepareQueueWaitUsecLast = 0u;
	m_nGpuPrepareFileOpenUsecLast = 0u;
	m_nGpuPrepareFileReadUsecLast = 0u;
	m_nGpuPrepareValidationUsecLast = 0u;
	m_nGpuPrepareMaterializeUsecLast = 0u;
	m_nGpuPrepareWorkerUsecLast = 0u;
	for ( int work_index = 0; work_index < m_nGpuPrepareWorkPageCount; ++work_index ) {
		t3dpreparedGpuRuntimePage_t &work_page = m_szGpuPrepareWorkPages[( size_t ) ( work_index )];
		if ( work_page.nWorkerStartedUsec >= work_page.nRequestedUsec ) {
			m_nGpuPrepareQueueWaitUsecLast = MAX( m_nGpuPrepareQueueWaitUsecLast,
				work_page.nWorkerStartedUsec - work_page.nRequestedUsec );
		}
		if ( work_page.nWorkerFinishedUsec >= work_page.nWorkerStartedUsec ) {
			m_nGpuPrepareWorkerUsecLast = MAX( m_nGpuPrepareWorkerUsecLast,
				work_page.nWorkerFinishedUsec - work_page.nWorkerStartedUsec );
		}
		m_nGpuPrepareFileOpenUsecLast += work_page.nFileOpenUsec;
		m_nGpuPrepareFileReadUsecLast += work_page.nFileReadUsec;
		m_nGpuPrepareValidationUsecLast += work_page.nValidationUsec;
		m_nGpuPrepareMaterializeUsecLast += work_page.nMaterializeUsec;

		const int map_index = getRegionMapIndex( work_page.location );
		if ( map_index >= 0 ) {
			work_page.consumerMask |= m_szRuntimePagePrepareConsumers[map_index];
			if ( m_szRuntimePagePrepareConsumers[map_index] != 0u ) {
				m_nRuntimePagePrepareQueuedCount = MAX( 0, m_nRuntimePagePrepareQueuedCount - 1 );
			}
			m_szRuntimePagePrepareConsumers[map_index] = 0u;
			m_szRuntimePagePreparePriorities[map_index] = 0u;
		}
		int destination = -1;
		for ( int ready_index = 0; ready_index < m_nGpuPreparedPageCount; ++ready_index ) {
			if ( m_szGpuPreparedPages[( size_t ) ( ready_index )].location == work_page.location ) {
				destination = ready_index;
				work_page.consumerMask |= m_szGpuPreparedPages[( size_t ) ( ready_index )].consumerMask;
				break;
			}
		}
		if ( destination < 0 && m_nGpuPreparedPageCount < TERRAIN3D_GPU_CACHE_MAX_LAYERS ) {
			destination = m_nGpuPreparedPageCount++;
		}
		if ( destination >= 0 ) {
			m_szGpuPreparedPages[( size_t ) ( destination )] = std::move( work_page );
		}
		work_page = t3dpreparedGpuRuntimePage_t{};
	}
	m_nGpuPrepareLatencyUsecLast = Time::get_singleton()->get_ticks_usec() - m_nGpuPrepareJobStartedUsec;
	m_nGpuRegionPreparedPagesLastTick = 0;
	for ( int index = 0; index < m_nGpuPreparedPageCount; ++index ) {
		m_nGpuRegionPreparedPagesLastTick += m_szGpuPreparedPages[( size_t ) ( index )].bLoaded ? 1 : 0;
	}
	m_nGpuPrepareWorkPageCount = 0;
	m_gpuPrepareContext.pPages = nullptr;
	m_gpuPrepareContext.nPageCount = 0;
	m_nGpuPrepareGroupId = WorkerThreadPool::INVALID_TASK_ID;
	m_bGpuPrepareJobActive = false;
}


bool T3DData::isGpuRuntimePagePreparePending( const Vector2i &vLocation ) const {
	if ( !m_bGpuPrepareJobActive ) {
		return false;
	}
	for ( int index = 0; index < m_nGpuPrepareWorkPageCount; ++index ) {
		if ( m_szGpuPrepareWorkPages[( size_t ) ( index )].location == vLocation ) {
			return true;
		}
	}
	return false;
}

void T3DData::requestGpuRuntimePage(
	const Vector2i &vLocation, const uint8_t nConsumers, const uint8_t nPriority
) {
	if ( nConsumers == 0u || !m_pTerrain || !m_pTerrain->isGpuRegionCookedPagesEnabled() ||
		!isRegionLocationValidInternal( vLocation ) || !hasRegion( vLocation ) ) {
		return;
	}
	for ( int ready_index = 0; ready_index < m_nGpuPreparedPageCount; ++ready_index ) {
		t3dpreparedGpuRuntimePage_t &ready = m_szGpuPreparedPages[( size_t ) ( ready_index )];
		if ( ready.location == vLocation ) {
			ready.consumerMask |= nConsumers;
			return;
		}
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return;
	}
	if ( m_szRuntimePagePrepareConsumers[map_index] == 0u ) {
		++m_nRuntimePagePrepareQueuedCount;
	}
	m_szRuntimePagePrepareConsumers[map_index] |= nConsumers;
	const uint8_t priority = MAX( ( uint8_t ) ( 1u ), nPriority );
	if ( m_szRuntimePagePreparePriorities[map_index] == 0u ||
		priority < m_szRuntimePagePreparePriorities[map_index] ) {
		m_szRuntimePagePreparePriorities[map_index] = priority;
	}
}

void T3DData::serviceGpuRuntimePagePrepare( void ) {
	pollGpuRuntimePagePrepare( false );
	if ( !m_pTerrain || !m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		return;
	}

	const uint64_t streaming_generation = m_pTerrain->getRegionStreamingGeneration();
	if ( !m_pTerrain->isEditorAuthoringMode() &&
		streaming_generation != m_nRuntimePagePrepareStreamingGenerationLast ) {
		// Direction/state changes can invalidate speculative demand while an asynchronous
		// batch is in flight. Sweep only on a streaming-generation change; stationary
		// frames with an empty queue now do no fixed-table work here.
		for ( int map_index = 0; map_index < TERRAIN3D_MAX_REGION_SLOTS; ++map_index ) {
			if ( m_szRuntimePagePrepareConsumers[map_index] == 0u ) {
				continue;
			}
			const int map_x = map_index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
			const int map_y = map_index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
			const Vector2i location( map_x - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
				map_y - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2 );
			const bool wanted = hasRegion( location ) &&
				( m_pTerrain->getRegionStreamingStateInternal( location ) >= TERRAIN3D_REGION_STREAMING_HOT ||
					m_pTerrain->isRegionStreamingPrefetchInternal( location ) );
			if ( !wanted ) {
				m_szRuntimePagePrepareConsumers[map_index] = 0u;
				m_szRuntimePagePreparePriorities[map_index] = 0u;
				m_nRuntimePagePrepareQueuedCount = MAX( 0, m_nRuntimePagePrepareQueuedCount - 1 );
			}
		}
		for ( int ready_index = m_nGpuPreparedPageCount - 1; ready_index >= 0; --ready_index ) {
			const Vector2i location = m_szGpuPreparedPages[( size_t ) ( ready_index )].location;
			const bool wanted = hasRegion( location ) &&
				( m_pTerrain->getRegionStreamingStateInternal( location ) >= TERRAIN3D_REGION_STREAMING_HOT ||
					m_pTerrain->isRegionStreamingPrefetchInternal( location ) );
			if ( wanted ) {
				continue;
			}
			m_szGpuPreparedPages[( size_t ) ( ready_index )] =
				std::move( m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] );
			m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] = t3dpreparedGpuRuntimePage_t{};
			--m_nGpuPreparedPageCount;
		}
	}
	m_nRuntimePagePrepareStreamingGenerationLast = streaming_generation;
	if ( m_bGpuPrepareJobActive || m_nRuntimePagePrepareQueuedCount <= 0 ) {
		return;
	}
	const int free_ready_slots = TERRAIN3D_GPU_CACHE_MAX_LAYERS - m_nGpuPreparedPageCount;
	if ( free_ready_slots <= 0 ) {
		return;
	}
	const int batch_limit = MIN( free_ready_slots, MIN( TERRAIN3D_GPU_CACHE_MAX_LAYERS,
		MAX( 2, MAX( 1, m_pTerrain->getRuntimePagePrepareThreads() ) * 2 ) ) );
	m_nGpuPrepareWorkPageCount = 0;
	const Vector2i predicted_center = m_pTerrain->getRegionStreamingPredictedCenterInternal();
	// Pending page work is sparse. Select directly from authored regions instead of
	// probing every coordinate in successively larger logical-map rings. This keeps
	// service cost proportional to actual terrain content and the small batch limit.
	while ( m_nGpuPrepareWorkPageCount < batch_limit && m_nRuntimePagePrepareQueuedCount > 0 ) {
		int best_slot = -1;
		int best_map_index = -1;
		uint8_t best_priority = UINT8_MAX;
		int best_distance = INT32_MAX;
		for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
			const Vector2i location = m_szRegionLocations[slot];
			if ( location == V2I_MAX ) {
				continue;
			}
			const int map_index = getRegionMapIndex( location );
			if ( map_index < 0 || m_szRuntimePagePrepareConsumers[map_index] == 0u ||
				isGpuRuntimePagePreparePending( location ) ) {
				continue;
			}
			T3DRegion *const pRegion = getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->runtimeMapsEvictable() ) {
				m_szRuntimePagePrepareConsumers[map_index] = 0u;
				m_szRuntimePagePreparePriorities[map_index] = 0u;
				m_nRuntimePagePrepareQueuedCount = MAX( 0, m_nRuntimePagePrepareQueuedCount - 1 );
				continue;
			}
			const uint8_t priority = m_szRuntimePagePreparePriorities[map_index];
			const int distance = MAX( Math::abs( location.x - predicted_center.x ),
				Math::abs( location.y - predicted_center.y ) );
			if ( priority < best_priority || ( priority == best_priority && distance < best_distance ) ||
				( priority == best_priority && distance == best_distance && map_index < best_map_index ) ) {
				best_slot = slot;
				best_map_index = map_index;
				best_priority = priority;
				best_distance = distance;
			}
		}
		if ( best_slot < 0 || best_map_index < 0 ) {
			break;
		}

		const Vector2i location = m_szRegionLocations[best_slot];
		T3DRegion *const pRegion = getRegionPtr( location );
		t3dpreparedGpuRuntimePage_t &page =
			m_szGpuPrepareWorkPages[( size_t ) ( m_nGpuPrepareWorkPageCount++ )];
		page = t3dpreparedGpuRuntimePage_t{};
		page.location = location;
		page.pagePath = getGpuRuntimePagePath( pRegion );
		page.sourcePath = pRegion->get_path();
		page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
		page.nRegionSize = m_nRegionSize;
		page.colorDivisor = m_nGpuRegionColorDivisor;
		page.derivedDivisor = m_nGpuRegionDerivedDivisor;
		page.middleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
		page.instancePage = !m_pTerrain->isEditorAuthoringMode() && pRegion->hasInstances() ?
			pRegion->getInstancePage() : PackedByteArray();
		page.consumerMask = m_szRuntimePagePrepareConsumers[best_map_index];
		page.nRequestedUsec = Time::get_singleton()->get_ticks_usec();
		m_szRuntimePagePrepareConsumers[best_map_index] = 0u;
		m_szRuntimePagePreparePriorities[best_map_index] = 0u;
		m_nRuntimePagePrepareQueuedCount = MAX( 0, m_nRuntimePagePrepareQueuedCount - 1 );
	}
	if ( m_nGpuPrepareWorkPageCount <= 0 ) {
		return;
	}

	m_nGpuPrepareJobWorkers = MIN( m_nGpuPrepareWorkPageCount,
		MAX( 1, m_pTerrain->getRuntimePagePrepareThreads() ) );
	m_gpuPrepareContext.pPages = m_szGpuPrepareWorkPages.data();
	m_gpuPrepareContext.nPageCount = m_nGpuPrepareWorkPageCount;
	m_nGpuRegionPrepareWorkersLastTick = m_nGpuPrepareJobWorkers;
	m_nGpuPrepareJobStartedUsec = Time::get_singleton()->get_ticks_usec();
	WorkerThreadPool *const pool = WorkerThreadPool::get_singleton();
	if ( !pool ) {
		for ( int index = 0; index < m_nGpuPrepareWorkPageCount; ++index ) {
			prepareGpuRuntimePageTask( &m_gpuPrepareContext, ( uint32_t ) ( index ) );
		}
		m_bGpuPrepareJobActive = true;
		m_nGpuPrepareGroupId = WorkerThreadPool::INVALID_TASK_ID;
		pollGpuRuntimePagePrepare( true );
		return;
	}
	m_nGpuPrepareGroupId = pool->add_native_group_task( &prepareGpuRuntimePageTask, &m_gpuPrepareContext,
		m_nGpuPrepareWorkPageCount, m_nGpuPrepareJobWorkers, false, "Terrain3D cooked page preparation" );
	if ( m_nGpuPrepareGroupId == WorkerThreadPool::INVALID_TASK_ID ) {
		for ( int index = 0; index < m_nGpuPrepareWorkPageCount; ++index ) {
			prepareGpuRuntimePageTask( &m_gpuPrepareContext, ( uint32_t ) ( index ) );
		}
		m_bGpuPrepareJobActive = true;
		pollGpuRuntimePagePrepare( true );
		return;
	}
	m_bGpuPrepareJobActive = true;
}


bool T3DData::takePreparedRuntimeHeight(
	const Vector2i &vLocation, const uint8_t nConsumer, Ref<Image> &rHeight, const bool bMiddle,
	PackedByteArray *const pNativeHeightBytes
) {
	pollGpuRuntimePagePrepare( false );
	rHeight.unref();
	if ( pNativeHeightBytes ) {
		pNativeHeightBytes->clear();
	}
	for ( int index = 0; index < m_nGpuPreparedPageCount; ++index ) {
		t3dpreparedGpuRuntimePage_t &page = m_szGpuPreparedPages[( size_t ) ( index )];
		if ( page.location != vLocation || ( page.consumerMask & nConsumer ) == 0u ) {
			continue;
		}
		if ( page.bLoaded ) {
			rHeight = bMiddle ? page.pMiddleHeight : page.height;
			if ( pNativeHeightBytes && !bMiddle ) {
				*pNativeHeightBytes = page.heightBytes;
			}
		}
		page.consumerMask &= ( uint8_t ) ( ~nConsumer );
		const bool loaded = page.bLoaded && rHeight.is_valid() &&
			( !pNativeHeightBytes || bMiddle || !pNativeHeightBytes->is_empty() );
		if ( page.consumerMask == 0u ) {
			m_szGpuPreparedPages[( size_t ) ( index )] =
				std::move( m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] );
			m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] = t3dpreparedGpuRuntimePage_t{};
			--m_nGpuPreparedPageCount;
		}
		return loaded;
	}
	return false;
}

bool T3DData::takePreparedRuntimeInstance( const Vector2i &vLocation, T3DInstanceStore &rStore ) {
	pollGpuRuntimePagePrepare( false );
	for ( int index = 0; index < m_nGpuPreparedPageCount; ++index ) {
		t3dpreparedGpuRuntimePage_t &page = m_szGpuPreparedPages[( size_t ) ( index )];
		if ( page.location != vLocation ||
			( page.consumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) == 0u ) {
			continue;
		}
		const bool loaded = page.bInstanceAttempted && page.bInstanceLoaded;
		if ( loaded ) {
			rStore = eastl::move( page.preparedInstanceStore );
		}
		page.consumerMask &= ( uint8_t ) ( ~RUNTIME_PAGE_CONSUMER_INSTANCE_STORE );
		if ( page.consumerMask == 0u ) {
			m_szGpuPreparedPages[( size_t ) ( index )] =
				std::move( m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] );
			m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] = t3dpreparedGpuRuntimePage_t{};
			--m_nGpuPreparedPageCount;
		}
		return loaded;
	}
	return false;
}

bool T3DData::takePreparedGpuRuntimePage(
	const Vector2i &vLocation, Ref<Image> &rHeight,
	Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived, Ref<Image> &rMiddleComposite,
	uint32_t &rMaterialMask, bool &bAttempted
) {
	pollGpuRuntimePagePrepare( false );
	bAttempted = false;
	rMaterialMask = 1u;
	for ( int index = 0; index < m_nGpuPreparedPageCount; ++index ) {
		t3dpreparedGpuRuntimePage_t &page = m_szGpuPreparedPages[( size_t ) ( index )];
		if ( page.location != vLocation || ( page.consumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) == 0u ) {
			continue;
		}
		bAttempted = page.bAttempted;
		const uint64_t consumed_usec = Time::get_singleton()->get_ticks_usec();
		m_nGpuPrepareConsumedRequestUsec = page.nRequestedUsec;
		if ( page.nWorkerFinishedUsec > 0u && consumed_usec >= page.nWorkerFinishedUsec ) {
			m_nGpuPrepareReadyWaitUsecLast = MAX( m_nGpuPrepareReadyWaitUsecLast,
				consumed_usec - page.nWorkerFinishedUsec );
		}
		if ( page.bLoaded ) {
			rHeight = page.height;
			rControl = page.pControl;
			rColor = page.pColor;
			rDerived = page.pDerived;
			rMiddleComposite = page.pMiddleComposite;
			rMaterialMask = page.materialMask;
		}
		const bool loaded = page.bLoaded;
		page.consumerMask &= ( uint8_t ) ( ~RUNTIME_PAGE_CONSUMER_GPU_DETAIL );
		if ( page.consumerMask == 0u ) {
			m_szGpuPreparedPages[( size_t ) ( index )] =
				std::move( m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] );
			m_szGpuPreparedPages[( size_t ) ( m_nGpuPreparedPageCount - 1 )] = t3dpreparedGpuRuntimePage_t{};
			--m_nGpuPreparedPageCount;
		}
		return loaded;
	}
	return false;
}


bool T3DData::writeGpuRuntimePage( T3DRegion *const pRegion, String *const pError ) {
	if ( pError ) {
		*pError = String();
	}
	auto fail = [pError]( const String &rMessage ) -> bool {
		if ( pError ) {
			*pError = rMessage;
		}
		return false;
	};
	if ( !pRegion || pRegion->isDeleted() || !m_pTerrain ) {
		return fail( "region/terrain owner is unavailable" );
	}
	const String page_path = getGpuRuntimePagePath( pRegion );
	if ( page_path.is_empty() ) {
		return fail( "runtime page path is empty" );
	}
	const Ref<Image> pHeight = pRegion->getHeightMap();
	if ( pHeight.is_null() || pHeight->get_width() != m_nRegionSize || pHeight->get_height() != m_nRegionSize ||
		pHeight->get_format() != Image::FORMAT_R16 || pHeight->has_mipmaps() ) {
		return fail( "height source is missing or has an invalid size/format" );
	}
	Ref<Image> pControl;
	if ( pRegion->hasChannel( T3DRegion::T3DRegion::TYPE_CONTROL ) ) {
		pControl = makeGpuRenderControlImage( pRegion->getLocation() );
		if ( pControl.is_null() || pControl->get_width() != m_nRegionSize || pControl->get_height() != m_nRegionSize ||
			pControl->get_format() != Image::FORMAT_RF || pControl->has_mipmaps() ) {
			return fail( "render-control page synthesis failed" );
		}
	}
	Ref<Image> pColor;
	if ( pRegion->hasChannel( T3DRegion::T3DRegion::TYPE_COLOR ) ) {
		pColor = makeGpuColorImage( pRegion->getColorMap() );
		if ( pColor.is_null() ) {
			return fail( "color page synthesis failed" );
		}
	}
	Ref<Image> pMiddleHeight = makeMiddleHeightImage( pRegion->getLocation(), true );
	if ( pMiddleHeight.is_null() || pMiddleHeight->is_empty() ) {
		return fail( "middle-height page synthesis failed" );
	}
	Ref<Image> pDerived = pRegion->getDerivedNormalSlopeMap();
	if ( pDerived.is_null() ) {
		regenerateDerivedMaps( {pRegion->getLocation()} );
		pDerived = pRegion->getDerivedNormalSlopeMap();
	}
	pDerived = makeGpuDerivedImage( pDerived );
	if ( pDerived.is_null() ) {
		return fail( "derived normal/slope page synthesis failed" );
	}
	const Ref<Image> pMiddleComposite = makeMiddleCompositeImage( pRegion->getLocation(),
		pControl.is_valid() ? pControl : m_pDefaultControlMap,
		pColor.is_valid() ? pColor : makeGpuColorImage( m_pDefaultColorMap ) );
	if ( pMiddleComposite.is_null() ) {
		return fail( "Middle composite page synthesis failed" );
	}

	t3druntimePageHeader_t header;
	header.nRegionSize = ( uint32_t ) ( m_nRegionSize );
	header.colorDivisor = ( uint32_t ) ( m_nGpuRegionColorDivisor );
	header.derivedDivisor = ( uint32_t ) ( m_nGpuRegionDerivedDivisor );
	header.channelFlags = ( uint32_t ) ( pRegion->getChannelFlags() ) |
		( uint32_t ) ( T3DRegion::CHANNEL_DERIVED );
	const uint64_t source_signature = computeGpuRuntimePageSourceSignature( pRegion );
	if ( source_signature == 0u ) {
		return fail( "source signature generation failed" );
	}
	header.sourceSignatureLo = ( uint32_t ) ( source_signature & 0xffffffffu );
	header.sourceSignatureHi = ( uint32_t ) ( source_signature >> 32u );
	header.blendRecipeVersion = TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION;
	header.materialMask = pControl.is_valid() ? terrain3dMaterialMaskFromControlImage( pControl ) : 1u;
	header.middleHeightDivisor = ( uint32_t ) ( MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() ) );
	uint64_t world_signature = 1469598103934665603ULL;
	terrain3dFnv1a64AppendU32( world_signature, ( uint32_t ) ( m_nRegionSize ) );
	terrain3dFnv1a64AppendU32( world_signature, ( uint32_t ) ( m_nGpuRegionColorDivisor ) );
	terrain3dFnv1a64AppendU32( world_signature, ( uint32_t ) ( m_nGpuRegionDerivedDivisor ) );
	terrain3dFnv1a64AppendU32( world_signature, TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION );
	header.worldSignatureLo = ( uint32_t ) ( world_signature & 0xffffffffu );
	header.worldSignatureHi = ( uint32_t ) ( world_signature >> 32u );
	header.regionX = pRegion->getLocation().x;
	header.regionZ = pRegion->getLocation().y;
	const Vector2 height_range = pRegion->getHeightRange();
	const float min_height = ( float ) ( height_range.x );
	const float max_height = ( float ) ( height_range.y );
	std::memcpy( &header.boundsMinYBits, &min_height, sizeof( uint32_t ) );
	std::memcpy( &header.boundsMaxYBits, &max_height, sizeof( uint32_t ) );
	PackedByteArray file_bytes;
	file_bytes.resize( TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES );
	std::memset( file_bytes.ptrw(), 0, TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES );
	const PackedByteArray instance_page = pRegion->getInstancePage();
	if ( !terrain3dRuntimePageAddChannel( file_bytes, header, TERRAIN3D_RUNTIME_PAGE_HEIGHT, pHeight ) ||
		( pControl.is_valid() && !terrain3dRuntimePageAddChannel( file_bytes, header,
			TERRAIN3D_RUNTIME_PAGE_CONTROL, pControl ) ) ||
		( pColor.is_valid() && !terrain3dRuntimePageAddChannel( file_bytes, header,
			TERRAIN3D_RUNTIME_PAGE_COLOR, pColor ) ) ||
		!terrain3dRuntimePageAddChannel( file_bytes, header, TERRAIN3D_RUNTIME_PAGE_DERIVED, pDerived ) ||
		!terrain3dRuntimePageAddChannel( file_bytes, header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT, pMiddleHeight ) ||
		!terrain3dRuntimePageAddChannel( file_bytes, header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE, pMiddleComposite ) ||
		( !instance_page.is_empty() && !terrain3dRuntimePageAddBlob( file_bytes, header,
			TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES, instance_page ) ) ) {
		return fail( "runtime page channel serialization failed" );
	}
	terrain3dRuntimePageEncodeHeader( file_bytes, header );
	if ( !terrain3dRuntimePageValidateHeader( header, ( uint64_t ) ( file_bytes.size() ) ) ) {
		return fail( "generated runtime page header failed validation" );
	}

	const String temporary_path = page_path + String( ".tmp" );
	const String backup_path = page_path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		return fail( "temporary runtime page could not be opened for writing" );
	}
	pFile->store_buffer( file_bytes );
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return fail( "temporary runtime page write failed" );
	}
	const bool bHadPrevious = FileAccess::file_exists( page_path );
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( bHadPrevious && DirAccess::rename_absolute( page_path, backup_path ) != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return fail( "existing runtime page could not be moved to backup" );
	}
	if ( DirAccess::rename_absolute( temporary_path, page_path ) != OK ) {
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, page_path );
		}
		DirAccess::remove_absolute( temporary_path );
		return fail( "temporary runtime page could not be published" );
	}

	// A successful write is not a successful cook until the published page can
	// be parsed, checksum-validated, and materialized through the same loader used
	// by runtime streaming. Restore the previous known-good page on validation failure.
	t3dpreparedGpuRuntimePage_t validation_page;
	validation_page.location = pRegion->getLocation();
	validation_page.pagePath = page_path;
	validation_page.sourcePath = pRegion->get_path();
	validation_page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
	validation_page.nRegionSize = m_nRegionSize;
	validation_page.colorDivisor = m_nGpuRegionColorDivisor;
	validation_page.derivedDivisor = m_nGpuRegionDerivedDivisor;
	validation_page.middleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
	if ( !loadGpuRuntimePageSnapshot( validation_page ) ) {
		DirAccess::remove_absolute( page_path );
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, page_path );
		}
		return fail( "published runtime page failed reload validation" );
	}
	if ( bHadPrevious ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( IS_EDITOR && page_path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( page_path );
		}
	}
	return true;
}

// Returns the number of spare texture-array layers retained after a full rebuild.
int T3DData::getRegionSlotReserve( void ) const {
	// Packaged terrain topology is static: spare layers waste VRAM and provide no
	// runtime benefit. The editor retains a small 16-layer cushion for iterative
	// add/remove operations instead of the previous 64-layer reserve at 128.
	if ( !IS_EDITOR ) {
		return 0;
	}
	if ( m_nRegionSize <= 128 ) {
		return 16;
	}
	if ( m_nRegionSize <= 256 ) {
		return 8;
	}
	if ( m_nRegionSize <= 512 ) {
		return 2;
	}
	return 1;
}

int T3DData::findRegionSlot( const Vector2i &rRegionLoc ) const {
	if ( !isRegionLocationValidInternal( rRegionLoc ) ) {
		return -1;
	}
	const int map_index = getRegionMapIndex( rRegionLoc );
	if ( map_index < 0 ) {
		return -1;
	}
	const int slot = m_szRegionMap[map_index] - 1;
	if ( slot < 0 || slot >= m_nRegionLocationCount ) {
		return -1;
	}
	return m_szRegionLocations[slot] == rRegionLoc ? slot : -1;
}

int T3DData::takeFreeRegionSlot( void ) {
	while ( m_nFreeRegionSlotCount > 0 ) {
		const int slot = m_szFreeRegionSlots[--m_nFreeRegionSlotCount];
		if ( slot >= 0 && slot < m_nRegionLocationCount && Vector2i( m_szRegionLocations[slot] ) == V2I_MAX ) {
			return slot;
		}
	}
	return -1;
}

void T3DData::releaseRegionSlot( const int iSlot ) {
	if ( iSlot < 0 || iSlot >= m_nRegionLocationCount ) {
		return;
	}
	m_szRegionLocations[iSlot] = V2I_MAX;
	if ( m_nFreeRegionSlotCount >= TERRAIN3D_MAX_REGION_SLOTS ) {
		LOG( T3DTerrain::ERROR, "Free region-slot stack overflow while releasing slot ", iSlot );
		return;
	}
	m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( iSlot );
}

void T3DData::rebuildFreeRegionSlots( void ) {
	m_nFreeRegionSlotCount = 0;
	// Reverse insertion keeps the lowest available slot at the back, preserving
	// deterministic layer reuse without a per-add scan.
	for ( int slot = m_nRegionLocationCount - 1; slot >= 0; --slot ) {
		if ( Vector2i( m_szRegionLocations[slot] ) == V2I_MAX ) {
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
		}
	}
}

bool T3DData::ensureRegionSlotReserve( void ) {
	if ( m_nActiveRegionCount <= 0 ) {
		return false;
	}

	int free_slots = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			++free_slots;
		}
	}

	const int desired_free = MIN( getRegionSlotReserve(), m_nRegionMapSize * m_nRegionMapSize - m_nActiveRegionCount );
	const int slots_to_add = desired_free - free_slots;
	if ( slots_to_add <= 0 ) {
		return false;
	}

	for ( int i = 0; i < slots_to_add && m_nRegionLocationCount < m_nRegionMapSize * m_nRegionMapSize; ++i ) {
		const int slot = m_nRegionLocationCount++;
		m_szRegionLocations[slot] = V2I_MAX;
		m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
	}
	return true;
}

bool T3DData::canUpdateRegionSlotsIncrementally( void ) const {
	if ( m_bGpuRegionCacheEnabled ) {
		return false;
	}
	const int slot_count = m_nRegionLocationCount;
	return !m_bRegionMapDirty && slot_count > 0 && !m_generatedHeightMaps.isDirty() &&
		   !m_generatedControlMaps.isDirty() && !m_generatedColorMaps.isDirty() &&
		   !m_generatedDerivedMaps.isDirty() && m_generatedHeightMaps.size() == slot_count &&
		   m_generatedControlMaps.size() == slot_count && m_generatedColorMaps.size() == slot_count &&
		   m_generatedDerivedMaps.size() == slot_count && m_heightMaps.size() == slot_count &&
		   m_controlMaps.size() == slot_count && m_colorMaps.size() == slot_count && m_derivedMaps.size() == slot_count;
}

void T3DData::rebuildRegionMapFromSlots( void ) {
	std::fill_n( m_szRegionMap, TERRAIN3D_MAX_REGION_SLOTS, 0 );
	m_nActiveRegionCount = 0;
	m_nFreeRegionSlotCount = 0;

	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
			continue;
		}

		const T3DRegion *pRegion = getRegionPtr( location );
		const int map_index = getRegionMapIndex( location );
		if ( !pRegion || pRegion->isDeleted() || map_index < 0 || !isRegionLocationValidInternal( location ) ) {
			m_szRegionLocations[slot] = V2I_MAX;
			m_szFreeRegionSlots[m_nFreeRegionSlotCount++] = ( uint16_t ) ( slot );
			continue;
		}

		m_szRegionMap[map_index] = slot + 1;
		++m_nActiveRegionCount;
	}
	m_bRegionMapDirty = false;
}

int T3DData::calculateGpuRegionRequiredCorePagesForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int radius = terrain3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(),
		detail_lods, ( double ) ( m_fVertexSpacing ), m_nRegionSize );
	const int diameter = radius * 2 + 1;
	return MIN( diameter * diameter, m_nRegionMapSize * m_nRegionMapSize );
}

int T3DData::calculateAuthoringGpuRegionBudgetMb( void ) const {
	// m_nGpuRegionCacheBudgetMb is already the effective allocation produced by
	// Terrain3D's aggregate memory-budget coordinator. Authoring may use more
	// physical layers than runtime, but it must fit those layers inside that same
	// coordinated GPU allocation instead of manufacturing extra editor memory.
	return CLAMP( MAX( 1, m_nGpuRegionCacheBudgetMb ),
		TERRAIN3D_GPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_GPU_REGION_CACHE_MAX_BUDGET_MB );
}

int T3DData::calculateGpuRegionCapacityForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int required_core = calculateGpuRegionRequiredCorePagesForLods( detail_lods );
	// Allocation is budget-driven. The unified region policy determines which pages
	// are desirable; legacy GPU guard/prefetch controls must not define a second
	// spatial footprint or reduce the usable physical cache.
	const int requested = MIN( m_nActiveRegionCount, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	const int capacity = terrain3dGpuCacheCalculateCapacity( m_nRegionSize, m_nRegionMapSize,
		m_nGpuRegionCacheBudgetMb, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS, requested, m_bControlMapsImplicit,
		!isGpuRegionColorChannelEnabled(), isGpuRegionDerivedChannelEnabled(), m_pTerrain->getGpuRegionColorResolutionDivisor(),
		m_pTerrain->getGpuRegionDerivedResolutionDivisor(), m_pTerrain->getGpuFarQualityDivisor(),
		m_nGpuRegionSparePages, m_pTerrain->getGpuFarHeightAtlasSide() );
	return MAX( required_core > 0 ? 1 : 0, capacity );
}

int T3DData::calculateGpuRegionCacheCapacity( void ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		const int page_limit = CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16,
			TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS );
		return terrain3dGpuCacheCalculateCapacity( m_nRegionSize, m_nRegionMapSize,
			calculateAuthoringGpuRegionBudgetMb(), page_limit,
			MIN( m_nActiveRegionCount, page_limit ), m_bControlMapsImplicit, !isGpuRegionColorChannelEnabled(),
			isGpuRegionDerivedChannelEnabled(),
			m_pTerrain->getGpuRegionColorResolutionDivisor(),
			m_pTerrain->getGpuRegionDerivedResolutionDivisor(),
			m_pTerrain->getGpuFarQualityDivisor(), m_nGpuRegionSparePages,
			m_pTerrain->getGpuFarHeightAtlasSide() );
	}
	const int allocation_request = MIN( m_nActiveRegionCount, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	return terrain3dGpuCacheCalculateCapacity( m_nRegionSize, m_nRegionMapSize,
		m_nGpuRegionCacheBudgetMb, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS,
		allocation_request, m_bControlMapsImplicit, !isGpuRegionColorChannelEnabled(),
		isGpuRegionDerivedChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1, m_nGpuRegionSparePages,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DData::calculateGpuRegionPhysicalLayers( const int nMappedCapacity ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		return terrain3dGpuCacheCalculatePhysicalLayers( nMappedCapacity, m_nGpuRegionSparePages,
			CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS ) );
	}
	return terrain3dGpuCacheCalculatePhysicalLayers( nMappedCapacity,
		m_nGpuRegionSparePages, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
}

int T3DData::calculateGpuRegionRequiredRadius( void ) const {
	if ( !m_pTerrain ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	int detail_lods = m_pTerrain->getGpuRegionDetailLods();
	if ( detail_lods <= 0 || detail_lods > total_lods ) {
		detail_lods = total_lods;
	}
	return terrain3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(),
		detail_lods, ( double ) ( m_fVertexSpacing ), m_nRegionSize );
}

int T3DData::calculateGpuRegionRequestedPages( void ) const {
	if ( !m_pTerrain || m_nActiveRegionCount <= 0 ) {
		return 0;
	}
	if ( m_pTerrain->isEditorAuthoringMode() ) {
		return MIN( m_nActiveRegionCount, m_pTerrain->getEditorGpuRegionCachePages() );
	}
	const Dictionary streaming = m_pTerrain->getRegionStreamingStats();
	const int active = ( int ) ( streaming["active"] );
	const int hot = ( int ) ( streaming["hot"] );
	const int predicted = ( int ) ( streaming["predicted_promotions"] );
	return MIN( m_nActiveRegionCount, active + hot + predicted );
}

size_t T3DData::estimateGpuRegionLayerBytes( void ) const {
	return terrain3dGpuCacheEstimateLayerBytes( m_nRegionSize, m_bControlMapsImplicit,
		!isGpuRegionColorChannelEnabled(), isGpuRegionDerivedChannelEnabled(),
		m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
}

bool T3DData::isGpuRegionColorChannelEnabled( void ) const {
	if ( m_bColorMapsImplicit ) {
		return false;
	}
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	const bool near_needed = m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		( pMaterial->getNearAuthoredColorContribution() > 0.0f || pMaterial->getNearWetnessContribution() > 0.0f );
	const bool middle_needed = m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		( pMaterial->getMiddleAuthoredColorContribution() > 0.0f || pMaterial->getMiddleWetnessContribution() > 0.0f );
	return near_needed || middle_needed;
}

bool T3DData::isGpuRegionDerivedChannelEnabled( void ) const {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	return ( m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		pMaterial->getNearTerrainNormalQuality() == T3DMaterial::TERRAIN_NORMAL_QUALITY_DETAILED ) ||
		( m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		pMaterial->getMiddleTerrainNormalQuality() == T3DMaterial::TERRAIN_NORMAL_QUALITY_DETAILED );
}

size_t T3DData::estimateGpuRegionSharedBytes( void ) const {
	return terrain3dGpuCacheEstimateSharedBytes( m_nRegionSize, m_bControlMapsImplicit,
		!isGpuRegionColorChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DData::getGpuFarHeightAtlasSide( void ) const {
	return m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
}

int T3DData::getGpuFarSurfaceAtlasSide( void ) const {
	return m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
}

size_t T3DData::getGpuFarHeightBytes( void ) const {
	const size_t nSide = ( size_t ) ( getGpuFarHeightAtlasSide() );
	return nSide * nSide * 2u;
}

size_t T3DData::getGpuFarColorBytes( void ) const {
	const size_t nSide = ( size_t ) ( getGpuFarSurfaceAtlasSide() );
	return nSide * nSide * 4u;
}

int T3DData::estimateGpuRegionUploadCalls( const T3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	int calls = 1; // Height is always published.
	if ( isGpuRegionDerivedChannelEnabled() ) {
		++calls;
	}
	if ( !m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL ) ) ) {
		++calls;
	}
	if ( isGpuRegionColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_COLOR ) ) ) {
		++calls;
	}
	return calls;
}

size_t T3DData::estimateGpuRegionUploadBytes( const T3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0u;
	}
	const size_t full_texels = terrain3dGpuCacheTexelCount( m_nRegionSize, 1 );
	const size_t derived_texels = terrain3dGpuCacheTexelCount( m_nRegionSize,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
	if ( full_texels == 0u || derived_texels == 0u ) {
		return 0u;
	}
	size_t bytes = full_texels * 2u;
	if ( isGpuRegionDerivedChannelEnabled() ) {
		bytes += derived_texels * 2u;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	if ( !m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL ) ) ) {
		bytes += full_texels * 4u;
	}
	if ( isGpuRegionColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_COLOR ) ) ) {
		bytes += terrain3dGpuCacheColorBytes( m_nRegionSize,
			m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 );
	}
	return bytes;
}

int T3DData::getGpuRegionTextureUpdateCallsLastTick( void ) const {
	return m_nGpuRegionHeightUpdatesLastTick + m_nGpuRegionControlUpdatesLastTick +
		m_nGpuRegionColorUpdatesLastTick + m_nGpuRegionDerivedUpdatesLastTick +
		m_nGpuRegionPageTableUpdatesLastTick + m_nGpuFarHeightUpdatesLastTick +
		m_nGpuFarColorUpdatesLastTick;
}

void T3DData::resetGpuRegionCache( const bool bClearTextures ) {
	pollGpuRuntimePagePrepare( true );
	m_nGpuPreparedPageCount = 0;
	m_nGpuPrepareWorkPageCount = 0;
	m_nRuntimePagePrepareQueuedCount = 0;
	m_nRuntimePagePrepareStreamingGenerationLast = 0u;
	std::fill_n( m_szRuntimePagePrepareConsumers, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szRuntimePagePreparePriorities, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szGpuRegionLastUseTick, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionLoadedTick, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionRetireAfterFrame, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionSlotStates, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
	std::fill_n( m_szGpuRegionPrefetchOrigin, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionLayerChannelFlags, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionMaterialMasks, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint32_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionPriorityLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szGpuRegionPriorityClasses, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( 0 ) );
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_nGpuRegionCacheCapacity = 0;
	m_nGpuRegionPhysicalLayerCount = 0;
	m_nGpuRegionSpareLayerCount = 0;
	m_nGpuRegionResidentCount = 0;
	m_nGpuRegionRetiredCount = 0;
	m_nGpuRegionDesiredLastTick = 0;
	m_nGpuRegionRequiredLastTick = 0;
	m_nGpuRegionGuardLastTick = 0;
	m_nGpuRegionPrefetchLastTick = 0;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	m_nGpuRegionRequestedPages = 0;
	m_nGpuRegionRequiredRadius = 0;
	m_nGpuRegionUploadsLastTick = 0;
	m_nGpuRegionUploadsSinceReset = 0u;
	m_nGpuRegionEvictionsLastTick = 0;
	m_nGpuRegionCookedHitsLastTick = 0;
	m_nGpuRegionCookedMissesLastTick = 0;
	m_nGpuRegionRuntimePreparesLastTick = 0;
	m_nGpuRegionReusedLayersLastTick = 0;
	m_nGpuRegionPageTableUpdatesLastTick = 0;
	m_nGpuRegionHeightUpdatesLastTick = 0;
	m_nGpuRegionControlUpdatesLastTick = 0;
	m_nGpuRegionColorUpdatesLastTick = 0;
	m_nGpuRegionDerivedUpdatesLastTick = 0;
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	m_nGpuRegionPreparedPagesLastTick = m_nGpuPreparedPageCount;
	m_nGpuRegionPrepareWorkersLastTick = m_bGpuPrepareJobActive ? m_nGpuPrepareJobWorkers : 0;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuFarHeightUpdatesLastTick = 0;
	m_nGpuFarColorUpdatesLastTick = 0;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionPrefetchUsedTotal = 0u;
	m_nGpuRegionPrefetchDiscardedTotal = 0u;
	m_nGpuRegionResidentTicksTotal = 0u;
	m_nGpuRegionDepartedPagesTotal = 0u;
	m_nGpuRegionTick = 0u;
	m_nGpuRegionLastPublicationTick = 0u;
	m_gpuRegionCacheCenter = V2I_MAX;
	m_gpuRegionPredictionDirection = V2I_ZERO;
	m_nGpuRegionStreamingGenerationLast = 0u;
	m_nGpuRegionPriorityCount = 0;
	std::memset( m_szGpuRegionRepublishPending, 0, sizeof( m_szGpuRegionRepublishPending ) );
	m_nGpuRegionRepublishPendingCount = 0;
	m_nGpuRegionRepublishCursor = 0;
	m_bGpuFarHeightDirty = m_bGpuFarHeightDirty || m_pGpuFarHeightImage.is_null();
	m_bGpuFarColorDirty = m_bGpuFarColorDirty || m_pGpuFarColorImage.is_null();
	m_nGpuFarHeightUpdatesLastTick = 0;
	m_nGpuFarColorUpdatesLastTick = 0;
	m_nGpuFarHeightRegionsLastTick = 0;
	m_nGpuFarColorRegionsLastTick = 0;
	m_bGpuRegionCacheTexturesReady = false;
	m_bGpuRegionCacheSelectionDirty = true;
	if ( bClearTextures ) {
		m_generatedHeightMaps.clear();
		m_generatedControlMaps.clear();
		m_generatedColorMaps.clear();
		m_generatedDerivedMaps.clear();
		m_generatedMiddleCompositeMaps.clear();
		if ( !isFarAtlasRequired() ) {
			m_generatedFarHeightMap.clear();
			m_generatedFarColorMap.clear();
		}
	}
}

bool T3DData::rebuildGpuRegionCacheTextures( void ) {
	ensureDefaultMaps();
	const int capacity = calculateGpuRegionCacheCapacity();
	LOG( T3DTerrain::INFO, "[gpu-cache] Rebuild requested: mapped_capacity=", capacity,
		"; region_size=", m_nRegionSize, "; implicit_control=", m_bControlMapsImplicit,
		"; implicit_color=", m_bColorMapsImplicit );
	if ( capacity <= 0 || m_pDefaultHeightMap.is_null() || m_pDefaultControlMap.is_null() ||
		m_pDefaultColorMap.is_null() || m_pDefaultDerivedMap.is_null() ) {
		return false;
	}

	const Vector2i previous_center = m_gpuRegionCacheCenter;
	resetGpuRegionCache( true );
	m_nGpuRegionCacheCapacity = capacity;
	m_nGpuRegionPhysicalLayerCount = calculateGpuRegionPhysicalLayers( capacity );
	m_nGpuRegionSpareLayerCount = MAX( 0, m_nGpuRegionPhysicalLayerCount - capacity );
	m_gpuRegionCacheCenter = previous_center;
	m_nGpuRegionColorDivisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	m_nGpuRegionDerivedDivisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const bool bColorChannelEnabled = isGpuRegionColorChannelEnabled();
	const bool bDerivedChannelEnabled = isGpuRegionDerivedChannelEnabled();
	m_bGpuRegionColorChannelAllocated = bColorChannelEnabled;
	m_bGpuRegionDerivedChannelAllocated = bDerivedChannelEnabled;

	m_gpuHeightMaps.resize( m_nGpuRegionPhysicalLayerCount );
	m_gpuDerivedMaps.resize( bDerivedChannelEnabled ? m_nGpuRegionPhysicalLayerCount : 1 );
	const int control_layers = m_bControlMapsImplicit ? 1 : m_nGpuRegionPhysicalLayerCount;
	const int color_layers = bColorChannelEnabled ? m_nGpuRegionPhysicalLayerCount : 1;
	m_gpuControlMaps.resize( control_layers );
	m_gpuColorMaps.resize( color_layers );
	m_gpuMiddleCompositeMaps.resize( m_nGpuRegionPhysicalLayerCount );
	const Ref<Image> pDefaultGpuColor = makeGpuColorImage( m_pDefaultColorMap );
	const Ref<Image> pDefaultGpuDerived = makeGpuDerivedImage( m_pDefaultDerivedMap );
	if ( pDefaultGpuColor.is_null() || pDefaultGpuDerived.is_null() ) {
		return false;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuHeightMaps[layer] = m_pDefaultHeightMap;
	}
	for ( int layer = 0; layer < m_gpuDerivedMaps.size(); ++layer ) {
		m_gpuDerivedMaps[layer] = pDefaultGpuDerived;
	}
	for ( int layer = 0; layer < control_layers; ++layer ) {
		m_gpuControlMaps[layer] = m_pDefaultControlMap;
	}
	for ( int layer = 0; layer < color_layers; ++layer ) {
		m_gpuColorMaps[layer] = pDefaultGpuColor;
	}
	const Ref<Image> pDefaultMiddleComposite = makeMiddleCompositeImage( V2I_ZERO, m_pDefaultControlMap, pDefaultGpuColor );
	if ( pDefaultMiddleComposite.is_null() ) {
		return false;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuMiddleCompositeMaps[layer] = pDefaultMiddleComposite;
	}

	const bool bHeightOk = m_generatedHeightMaps.create( m_gpuHeightMaps ).is_valid();
	const bool bControlOk = m_generatedControlMaps.create( m_gpuControlMaps ).is_valid();
	const bool bColorOk = m_generatedColorMaps.create( m_gpuColorMaps ).is_valid();
	const bool bDerivedOk = m_generatedDerivedMaps.create( m_gpuDerivedMaps ).is_valid();
	const bool bMiddleCompositeOk = m_generatedMiddleCompositeMaps.create( m_gpuMiddleCompositeMaps ).is_valid();
	// RenderingServer owns the uploaded layers. Retaining these Image arrays would
	// duplicate the physical GPU cache on the CPU side and defeat bounded residency.
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_bGpuRegionCacheTexturesReady = bHeightOk && bControlOk && bColorOk && bDerivedOk && bMiddleCompositeOk;
	if ( m_bGpuRegionCacheTexturesReady ) {
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	LOG( T3DTerrain::DEBUG, "[gpu-cache] Physical arrays uploaded: height=", bHeightOk, "; control=", bControlOk,
		"; color=", bColorOk, "; derived=", bDerivedOk, "; middle_composite=", bMiddleCompositeOk,
		"; physical_layers=", m_nGpuRegionPhysicalLayerCount );
	if ( !m_bGpuRegionCacheTexturesReady ) {
		LOG( T3DTerrain::ERROR, "Failed to create bounded GPU region cache with ", capacity, " physical layers" );
		return false;
	}
	if ( ( m_pGpuFarHeightImage.is_null() || m_pGpuFarColorImage.is_null() ) &&
		!loadCookedGpuFarAtlases() ) {
		markAllGpuFarRegionsDirty( T3DRegion::TYPE_MAX );
	}
	if ( !m_bGpuFarHeightDirty && m_pGpuFarHeightImage.is_valid() ) {
		if ( !m_generatedFarHeightMap.create( m_pGpuFarHeightImage ).is_valid() ) {
			return false;
		}
	} else if ( !rebuildGpuFarHeightAtlas() ) {
		LOG( T3DTerrain::ERROR, "Failed to create the coarse whole-world terrain height atlas" );
		return false;
	}
	if ( !m_bGpuFarColorDirty && m_pGpuFarColorImage.is_valid() ) {
		if ( !m_generatedFarColorMap.create( m_pGpuFarColorImage ).is_valid() ) {
			return false;
		}
	} else if ( !rebuildGpuFarColorAtlas() ) {
		LOG( T3DTerrain::ERROR, "Failed to create the coarse whole-world terrain color atlas" );
		return false;
	}
	const size_t budget_bytes = ( size_t ) ( m_nGpuRegionCacheBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	const size_t layer_bytes = estimateGpuRegionLayerBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t estimated_bytes = shared_bytes + layer_bytes * ( size_t ) ( m_nGpuRegionPhysicalLayerCount );
	if ( m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateFarAtlasTextures();
		}
	}
	if ( estimated_bytes > budget_bytes ) {
		LOG( T3DTerrain::WARN, "One terrain region requires approximately ", layer_bytes,
			" GPU bytes and shared channels require ", shared_bytes,
			" bytes. The selected cache exceeds the configured budget of ", budget_bytes,
			" bytes because one physical layer is the minimum legal configuration." );
	}
	m_nGpuRegionRequestedPages = calculateGpuRegionRequestedPages();
	m_nGpuRegionRequiredRadius = calculateGpuRegionRequiredRadius();
	LOG( T3DTerrain::INFO, "[gpu-cache] Created footprint-driven GPU region cache: ", capacity, " mapped pages, ",
		m_nGpuRegionSpareLayerCount, " spare/retirement pages, ",
		m_nGpuRegionPhysicalLayerCount, " physical layers, requested ", m_nGpuRegionRequestedPages, ", required radius ", m_nGpuRegionRequiredRadius,
		", estimated bytes ", estimated_bytes, ", budget ", budget_bytes );
	return true;
}

String T3DData::getGpuFarAtlasPath( const String &rDirectory ) const {
	const String directory = !rDirectory.is_empty() ? rDirectory :
		( m_pTerrain ? m_pTerrain->getDataDirectory() : String() );
	if ( directory.is_empty() ) {
		return String();
	}
	return directory.path_join( "terrain3d_far_atlas.t3dfar" );
}

uint64_t T3DData::computeGpuFarAtlasSourceSignature( void ) const {
	uint64_t hash = 1469598103934665603ull;
	terrain3dFnv1a64AppendU32( hash, TERRAIN3D_GPU_FAR_ATLAS_VERSION );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_nRegionSize ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_nRegionMapSize ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( getGpuFarHeightAtlasSide() ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( getGpuFarSurfaceAtlasSide() ) );
	terrain3dFnv1a64AppendF32( hash, ( float ) ( m_fVertexSpacing ) );

	// Iterate the fixed spatial table rather than region slot order so the same
	// persisted world produces the same signature regardless of load/allocation order.
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_nActiveRegionCount ) );
	for ( int map_index = 0; map_index < TERRAIN3D_MAX_REGION_SLOTS; ++map_index ) {
		const Vector2i location(
			map_index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
			map_index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
		);
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( map_index ) );
		const String path = pRegion->get_path();
		const uint64_t modified = !path.is_empty() && FileAccess::file_exists( path ) ?
			FileAccess::get_modified_time( path ) : 0u;
		terrain3dFnv1a64AppendU64( hash, modified );
	}

	t3dfarSurfaceBakeData_t bake_data;
	terrain3dBuildFarSurfaceBakeData( m_pTerrain, bake_data );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( bake_data.nCount ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( bake_data.fallback_texture ) );
	for ( int index = 0; index < bake_data.nCount; ++index ) {
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( index ) );
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( bake_data.populated[( size_t ) ( index )] ) );
	}

	const Ref<T3DMaterial> pMaterialRef = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	T3DMaterial *const pMaterial = pMaterialRef.ptr();
	terrain3dFnv1a64AppendU32( hash, pMaterial ? 1u : 0u );
	if ( pMaterial ) {
		terrain3dFnv1a64AppendU32( hash, pMaterial->getAutoShaderEnabled() ? 1u : 0u );
		terrain3dFnv1a64AppendF32( hash, terrain3dMaterialFloatParam( pMaterial, "auto_slope", 1.0f ) );
		terrain3dFnv1a64AppendF32( hash,
			terrain3dMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f ) );
		terrain3dFnv1a64AppendU32( hash,
			( uint32_t ) ( terrain3dMaterialIntParam( pMaterial, "auto_base_texture", 0 ) ) );
		terrain3dFnv1a64AppendU32( hash,
			( uint32_t ) ( terrain3dMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ) ) );
	}
	return hash;
}

bool T3DData::isFarAtlasRequired( void ) const {
	return m_bGpuRegionCacheEnabled || ( m_pTerrain && m_pTerrain->isRuntimeShaderTieringActive() );
}

bool T3DData::initializeGpuFarAtlasImages( void ) {
	const int nHeightAtlasSize = getGpuFarHeightAtlasSide();
	const int nSurfaceAtlasSize = getGpuFarSurfaceAtlasSide();
	const int nHeightTexelCount = nHeightAtlasSize * nHeightAtlasSize;
	const int nSurfaceTexelCount = nSurfaceAtlasSize * nSurfaceAtlasSize;
	if ( m_pGpuFarHeightImage.is_null() || m_pGpuFarHeightImage->get_width() != nHeightAtlasSize ||
		m_pGpuFarHeightImage->get_height() != nHeightAtlasSize ||
		m_pGpuFarHeightImage->get_format() != Image::FORMAT_R16 ) {
		const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
		PackedByteArray height_bytes;
		height_bytes.resize( ( int64_t ) ( nHeightTexelCount ) * 2 );
		uint8_t *const pWrite = height_bytes.ptrw();
		for ( int index = 0; index < nHeightTexelCount; ++index ) {
			std::memcpy( pWrite + ( size_t ) ( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		m_pGpuFarHeightImage = Image::create_from_data( nHeightAtlasSize, nHeightAtlasSize, false,
			Image::FORMAT_R16, height_bytes );
	}
	if ( m_pGpuFarColorImage.is_null() || m_pGpuFarColorImage->get_width() != nSurfaceAtlasSize ||
		m_pGpuFarColorImage->get_height() != nSurfaceAtlasSize ||
		m_pGpuFarColorImage->get_format() != Image::FORMAT_RGBA8 ) {
		PackedByteArray color_bytes;
		color_bytes.resize( ( int64_t ) ( nSurfaceTexelCount ) * 4 );
		terrain3dFillFarSurfaceEmpty( color_bytes );
		m_pGpuFarColorImage = Image::create_from_data( nSurfaceAtlasSize, nSurfaceAtlasSize, false,
			Image::FORMAT_RGBA8, color_bytes );
	}
	return m_pGpuFarHeightImage.is_valid() && !m_pGpuFarHeightImage->is_empty() &&
		m_pGpuFarColorImage.is_valid() && !m_pGpuFarColorImage->is_empty();
}

bool T3DData::loadCookedGpuFarAtlases( void ) {
	const String path = getGpuFarAtlasPath();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < TERRAIN3D_GPU_FAR_ATLAS_HEADER_BYTES ) {
		return false;
	}
	const uint64_t magic = pFile->get_64();
	const uint32_t version = pFile->get_32();
	const uint32_t region_size = pFile->get_32();
	const uint32_t region_map_size = pFile->get_32();
	const uint32_t height_atlas_size = pFile->get_32();
	const uint32_t surface_atlas_size = pFile->get_32();
	const uint32_t height_size = pFile->get_32();
	const uint32_t color_size = pFile->get_32();
	const uint32_t height_checksum = pFile->get_32();
	const uint32_t color_checksum = pFile->get_32();
	const uint64_t source_signature = pFile->get_64();
	const int nExpectedHeightAtlasSize = getGpuFarHeightAtlasSide();
	const int nExpectedSurfaceAtlasSize = getGpuFarSurfaceAtlasSide();
	const uint32_t expected_height_size = ( uint32_t ) ( nExpectedHeightAtlasSize * nExpectedHeightAtlasSize * 2 );
	const uint32_t expected_color_size = ( uint32_t ) ( nExpectedSurfaceAtlasSize * nExpectedSurfaceAtlasSize * 4 );
	if ( magic != TERRAIN3D_GPU_FAR_ATLAS_MAGIC || version != TERRAIN3D_GPU_FAR_ATLAS_VERSION ||
		region_size != ( uint32_t ) ( m_nRegionSize ) || region_map_size != ( uint32_t ) ( m_nRegionMapSize ) ||
		height_atlas_size != ( uint32_t ) ( nExpectedHeightAtlasSize ) ||
		surface_atlas_size != ( uint32_t ) ( nExpectedSurfaceAtlasSize ) ||
		height_size != expected_height_size || color_size != expected_color_size ||
		pFile->get_length() != TERRAIN3D_GPU_FAR_ATLAS_HEADER_BYTES + ( uint64_t ) ( height_size ) +
			( uint64_t ) ( color_size ) ) {
		return false;
	}
	const uint64_t expected_source_signature = computeGpuFarAtlasSourceSignature();
	if ( source_signature == 0u || source_signature != expected_source_signature ) {
		LOG( T3DTerrain::INFO, "[far-cache] Rejecting cooked atlas with stale source signature: cached=",
			source_signature, ", current=", expected_source_signature );
		return false;
	}
	const PackedByteArray height_bytes = pFile->get_buffer( height_size );
	const PackedByteArray color_bytes = pFile->get_buffer( color_size );
	if ( height_bytes.size() != height_size || color_bytes.size() != color_size ||
		terrain3dRuntimePageChecksum( height_bytes.ptr(), ( size_t ) ( height_bytes.size() ) ) != height_checksum ||
		terrain3dRuntimePageChecksum( color_bytes.ptr(), ( size_t ) ( color_bytes.size() ) ) != color_checksum ) {
		return false;
	}
	m_pGpuFarHeightImage = Image::create_from_data( nExpectedHeightAtlasSize,
		nExpectedHeightAtlasSize, false, Image::FORMAT_R16, height_bytes );
	m_pGpuFarColorImage = Image::create_from_data( nExpectedSurfaceAtlasSize,
		nExpectedSurfaceAtlasSize, false, Image::FORMAT_RGBA8, color_bytes );
	if ( m_pGpuFarHeightImage.is_null() || m_pGpuFarColorImage.is_null() ) {
		return false;
	}
	m_bGpuFarHeightDirty = false;
	m_bGpuFarColorDirty = false;
	m_bGpuFarHeightDirtyAll = false;
	m_bGpuFarColorDirtyAll = false;
	m_gpuFarHeightDirtyRegions.reset();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarCookedLoaded = true;
	m_bGpuFarCookedSaved = false;
	m_nGpuFarCookIoUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	return true;
}

bool T3DData::writeCookedGpuFarAtlases( const String &rDirectory ) {
	const String path = getGpuFarAtlasPath( rDirectory );
	if ( path.is_empty() ) {
		return false;
	}
	// A loaded/saved clean atlas is already authoritative. Do not rewrite a large
	// generated file merely because the scene itself was saved.
	if ( !m_bGpuFarHeightDirty && !m_bGpuFarColorDirty &&
		( m_bGpuFarCookedLoaded || m_bGpuFarCookedSaved ) && FileAccess::file_exists( path ) ) {
		return true;
	}
	if ( !initializeGpuFarAtlasImages() ) {
		return false;
	}
	if ( m_bGpuFarHeightDirty && !rebuildGpuFarHeightAtlas() ) {
		return false;
	}
	if ( m_bGpuFarColorDirty && !rebuildGpuFarColorAtlas() ) {
		return false;
	}
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	const PackedByteArray height_bytes = m_pGpuFarHeightImage->get_data();
	const PackedByteArray color_bytes = m_pGpuFarColorImage->get_data();
	const uint64_t source_signature = computeGpuFarAtlasSourceSignature();
	const String temporary_path = path + String( ".tmp" );
	const String backup_path = path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		return false;
	}
	pFile->store_64( TERRAIN3D_GPU_FAR_ATLAS_MAGIC );
	pFile->store_32( TERRAIN3D_GPU_FAR_ATLAS_VERSION );
	pFile->store_32( ( uint32_t ) ( m_nRegionSize ) );
	pFile->store_32( ( uint32_t ) ( m_nRegionMapSize ) );
	pFile->store_32( ( uint32_t ) ( getGpuFarHeightAtlasSide() ) );
	pFile->store_32( ( uint32_t ) ( getGpuFarSurfaceAtlasSide() ) );
	pFile->store_32( ( uint32_t ) ( height_bytes.size() ) );
	pFile->store_32( ( uint32_t ) ( color_bytes.size() ) );
	pFile->store_32( terrain3dRuntimePageChecksum( height_bytes.ptr(), ( size_t ) ( height_bytes.size() ) ) );
	pFile->store_32( terrain3dRuntimePageChecksum( color_bytes.ptr(), ( size_t ) ( color_bytes.size() ) ) );
	pFile->store_64( source_signature );
	pFile->store_buffer( height_bytes );
	pFile->store_buffer( color_bytes );
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return false;
	}
	const bool bHadPrevious = FileAccess::file_exists( path );
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( bHadPrevious && DirAccess::rename_absolute( path, backup_path ) != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary_path, path ) != OK ) {
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, path );
		}
		return false;
	}
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	m_bGpuFarCookedSaved = true;
	m_bGpuFarCookedLoaded = false;
	if ( IS_EDITOR && path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( path );
		}
	}
	m_nGpuFarCookIoUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	return true;
}

void T3DData::markAllGpuFarRegionsDirty( const t3dmapType_t eMapType ) {
	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarHeightDirty = true;
		m_bGpuFarHeightDirtyAll = true;
		m_gpuFarHeightDirtyRegions.reset();
	}
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarColorDirty = true;
		m_bGpuFarColorDirtyAll = true;
		m_gpuFarColorDirtyRegions.reset();
	}
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
}

void T3DData::markGpuFarRegionDirty( const Vector2i &vLocation, const t3dmapType_t eMapType ) {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return;
	}
	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarHeightDirty = true;
		if ( !m_bGpuFarHeightDirtyAll ) {
			m_gpuFarHeightDirtyRegions.set( ( size_t ) ( map_index ) );
		}
	}
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarColorDirty = true;
		if ( !m_bGpuFarColorDirtyAll ) {
			m_gpuFarColorDirtyRegions.set( ( size_t ) ( map_index ) );
		}
	}
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
}

bool T3DData::patchGpuFarHeightRegion(
	const Vector2i &vLocation,
	PackedByteArray &nAtlasBytes
) {
	const int nAtlasSize = getGpuFarHeightAtlasSide();
	if ( m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ||
		nAtlasSize % m_nRegionMapSize != 0 ) {
		return false;
	}
	const int samples_per_region = nAtlasSize / m_nRegionMapSize;
	if ( samples_per_region <= 0 || m_nRegionSize % samples_per_region != 0 ) {
		return false;
	}
	const int atlas_half = m_nRegionMapSize / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( atlas_origin_x < 0 || atlas_origin_y < 0 ||
		atlas_origin_x + samples_per_region > nAtlasSize ||
		atlas_origin_y + samples_per_region > nAtlasSize ) {
		return false;
	}
	const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
	const uint16_t *height = nullptr;
	PackedByteArray region_height_bytes;
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	T3DActiveMapLease lease;
	if ( pRegion && !pRegion->isDeleted() ) {
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( vLocation, true ) ) {
			return false;
		}
		lease = acquireActiveMap( vLocation );
		if ( lease.valid() ) {
			height = lease.view().pHeight;
		} else {
			const Ref<Image> pHeight = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
			if ( pHeight.is_valid() && pHeight->get_format() == Image::FORMAT_R16 ) {
				region_height_bytes = pHeight->get_data();
				if ( region_height_bytes.size() >= ( int64_t ) ( m_nRegionSize ) * m_nRegionSize * 2 ) {
					height = ( const uint16_t * ) ( region_height_bytes.ptr() );
				}
			}
		}
	}
	const int source_step = m_nRegionSize / samples_per_region;
	uint8_t *const pWrite = nAtlasBytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int y = 0; y < samples_per_region; ++y ) {
		const int source_y = MIN( m_nRegionSize - 1, y * source_step );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int source_x = MIN( m_nRegionSize - 1, x * source_step );
			const uint16_t encoded = height ? height[source_y * m_nRegionSize + source_x] : default_height;
			const int atlas_index = ( atlas_origin_y + y ) * nAtlasSize + atlas_origin_x + x;
			std::memcpy( pWrite + ( size_t ) ( atlas_index ) * 2u, &encoded, sizeof( encoded ) );
		}
	}
	return true;
}

bool T3DData::patchGpuFarColorRegion(
	const Vector2i &vLocation,
	PackedByteArray &nAtlasBytes, const t3dfarSurfaceBakeData_t &rBakeData
) {
	const int nAtlasSize = getGpuFarSurfaceAtlasSide();
	if ( m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ||
		nAtlasSize % m_nRegionMapSize != 0 ) {
		return false;
	}
	const int samples_per_region = nAtlasSize / m_nRegionMapSize;
	const int atlas_half = m_nRegionMapSize / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( samples_per_region <= 0 || atlas_origin_x < 0 || atlas_origin_y < 0 ||
		atlas_origin_x + samples_per_region > nAtlasSize ||
		atlas_origin_y + samples_per_region > nAtlasSize ) {
		return false;
	}
	uint8_t *pAtlasWrite = nAtlasBytes.ptrw();
	if ( !pAtlasWrite ) {
		return false;
	}
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		for ( int y = 0; y < samples_per_region; ++y ) {
			for ( int x = 0; x < samples_per_region; ++x ) {
				const int atlas_index = ( atlas_origin_y + y ) * nAtlasSize + atlas_origin_x + x;
				uint8_t *pixel = pAtlasWrite + ( size_t ) ( atlas_index ) * 4u;
				pixel[0] = 255u;
				pixel[1] = 255u;
				pixel[2] = 255u;
				pixel[3] = 0u;
			}
		}
		return true;
	}
	if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( vLocation, true ) ) {
		return false;
	}
	const Ref<Image> pSourceColor = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR );
	const Ref<Image> pSourceControl = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_CONTROL );
	const Ref<Image> pSourceHeight = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
	const Ref<Image> pSourceDerived = pRegion->getDerivedNormalSlopeMap();
	if ( pSourceColor.is_null() || pSourceControl.is_null() ||
		pSourceColor->get_format() != Image::FORMAT_RGBA8 ||
		pSourceControl->get_format() != Image::FORMAT_RF ) {
		return false;
	}
	const PackedByteArray color_bytes = pSourceColor->get_data();
	const PackedByteArray control_bytes = pSourceControl->get_data();
	const PackedByteArray height_bytes = pSourceHeight.is_valid() ? pSourceHeight->get_data() : PackedByteArray();
	const PackedByteArray derived_bytes = pSourceDerived.is_valid() ? pSourceDerived->get_data() : PackedByteArray();
	const uint8_t *pColorRead = color_bytes.ptr();
	const uint8_t *pControlRead = control_bytes.ptr();
	const uint8_t *pHeightRead = height_bytes.ptr();
	const uint8_t *pDerivedRead = derived_bytes.ptr();
	const int color_width = pSourceColor->get_width();
	const int color_height = pSourceColor->get_height();
	const bool bHeightValid = pSourceHeight.is_valid() && pSourceHeight->get_format() == Image::FORMAT_R16 &&
		height_bytes.size() >= ( int64_t ) ( m_nRegionSize ) * m_nRegionSize * 2;
	const int derived_width = pSourceDerived.is_valid() ? pSourceDerived->get_width() : 0;
	const int derived_height = pSourceDerived.is_valid() ? pSourceDerived->get_height() : 0;
	const bool bDerivedValid = pSourceDerived.is_valid() && pSourceDerived->get_format() == Image::FORMAT_RG8 &&
		derived_width > 0 && derived_height > 0 &&
		derived_bytes.size() >= ( int64_t ) ( derived_width ) * derived_height * 2;
	if ( !pColorRead || !pControlRead || color_width <= 0 || color_height <= 0 ||
		control_bytes.size() < ( int64_t ) ( m_nRegionSize ) * m_nRegionSize * 4 ) {
		return false;
	}

	const Ref<T3DMaterial> pMaterialRef = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	T3DMaterial *const pMaterial = pMaterialRef.ptr();
	const bool bAutoShaderEnabled = pMaterial && pMaterial->getAutoShaderEnabled();
	const float fAutoSlope = terrain3dMaterialFloatParam( pMaterial, "auto_slope", 1.0f );
	const float fAutoHeightReduction = terrain3dMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f );
	const int auto_layer_max = MAX( rBakeData.nCount - 1, 0 );
	const int auto_base_texture = CLAMP( terrain3dMaterialIntParam( pMaterial, "auto_base_texture", 0 ), 0, auto_layer_max );
	const int auto_overlay_texture = CLAMP( terrain3dMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ), 0, auto_layer_max );
	const int source_step = MAX( 1, m_nRegionSize / samples_per_region );

	for ( int y = 0; y < samples_per_region; ++y ) {
		const int region_y0 = y * m_nRegionSize / samples_per_region;
		const int region_y1 = MAX( region_y0 + 1, ( y + 1 ) * m_nRegionSize / samples_per_region );
		const int color_y0 = y * color_height / samples_per_region;
		const int color_y1 = MAX( color_y0 + 1, ( y + 1 ) * color_height / samples_per_region );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int region_x0 = x * m_nRegionSize / samples_per_region;
			const int region_x1 = MAX( region_x0 + 1, ( x + 1 ) * m_nRegionSize / samples_per_region );
			const int color_x0 = x * color_width / samples_per_region;
			const int color_x1 = MAX( color_x0 + 1, ( x + 1 ) * color_width / samples_per_region );
			uint64_t sum_r = 0u;
			uint64_t sum_g = 0u;
			uint64_t sum_b = 0u;
			uint64_t sum_a = 0u;
			int color_samples = 0;
			for ( int color_y = color_y0; color_y < MIN( color_y1, color_height ); ++color_y ) {
				for ( int color_x = color_x0; color_x < MIN( color_x1, color_width ); ++color_x ) {
					const size_t color_offset = ( size_t ) ( color_y * color_width + color_x ) * 4u;
					sum_r += pColorRead[color_offset];
					sum_g += pColorRead[color_offset + 1u];
					sum_b += pColorRead[color_offset + 2u];
					sum_a += pColorRead[color_offset + 3u];
					++color_samples;
				}
			}
			const int center_x = MIN( m_nRegionSize - 1, region_x0 + ( region_x1 - region_x0 ) / 2 );
			const int center_y = MIN( m_nRegionSize - 1, region_y0 + ( region_y1 - region_y0 ) / 2 );
			const int center_index = center_y * m_nRegionSize + center_x;
			const uint32_t control = terrain3dReadU32Unaligned( pControlRead, ( size_t ) ( center_index ) * 4u );
			float fSolidCoverage = 0.0f;
			for ( int probe_y = 0; probe_y < 2; ++probe_y ) {
				for ( int probe_x = 0; probe_x < 2; ++probe_x ) {
					const int px = MIN( m_nRegionSize - 1, region_x0 + ( ( probe_x * 2 + 1 ) * source_step ) / 4 );
					const int py = MIN( m_nRegionSize - 1, region_y0 + ( ( probe_y * 2 + 1 ) * source_step ) / 4 );
					const uint32_t probe_control = terrain3dReadU32Unaligned( pControlRead,
						( size_t ) ( py * m_nRegionSize + px ) * 4u );
					fSolidCoverage += isHole( probe_control ) ? 0.0f : 0.25f;
				}
			}

			int base_id = ( int ) ( getBase( control ) );
			int overlay_id = ( int ) ( getOverlay( control ) );
			float fBlend = ( float ) ( getBlend( control ) ) / TERRAIN3D_BYTE_MAX_FLOAT;
			if ( bAutoShaderEnabled && isAuto( control ) ) {
				float fNormalY = 1.0f;
				float fHeight = 0.0f;
				if ( bHeightValid ) {
					fHeight = Terrain3DRegionCodec::decodeHeightU16( terrain3dReadU16Unaligned(
						pHeightRead, ( size_t ) ( center_index ) * 2u ) );
				}
				if ( bDerivedValid ) {
					const int derived_x = MIN( derived_width - 1, center_x * derived_width / m_nRegionSize );
					const int derived_y = MIN( derived_height - 1, center_y * derived_height / m_nRegionSize );
					const size_t derived_offset = ( size_t ) ( derived_y * derived_width + derived_x ) * 2u;
					const float fNormalX = ( float ) ( pDerivedRead[derived_offset] ) / TERRAIN3D_BYTE_MAX_FLOAT * 2.0f - 1.0f;
					const float fNormalZ = ( float ) ( pDerivedRead[derived_offset + 1u] ) / TERRAIN3D_BYTE_MAX_FLOAT * 2.0f - 1.0f;
					fNormalY = Math::sqrt( MAX( 1.0f - fNormalX * fNormalX - fNormalZ * fNormalZ, 0.0f ) );
				} else if ( bHeightValid ) {
					const int sample_x = center_y * m_nRegionSize + MIN( m_nRegionSize - 1, center_x + 1 );
					const int sample_z = MIN( m_nRegionSize - 1, center_y + 1 ) * m_nRegionSize + center_x;
					const float fHeightX = Terrain3DRegionCodec::decodeHeightU16( terrain3dReadU16Unaligned(
						pHeightRead, ( size_t ) ( sample_x ) * 2u ) );
					const float fHeightZ = Terrain3DRegionCodec::decodeHeightU16( terrain3dReadU16Unaligned(
						pHeightRead, ( size_t ) ( sample_z ) * 2u ) );
					const float fSpacing = MAX( ( float ) ( m_fVertexSpacing ), 0.0001f );
					const float fDx = fHeightX - fHeight;
					const float fDz = fHeightZ - fHeight;
					fNormalY = fSpacing / Math::sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
				}
				base_id = auto_base_texture;
				overlay_id = auto_overlay_texture;
				fBlend = CLAMP( fAutoSlope * 2.0f * ( fNormalY - 1.0f ) + 1.0f -
					fAutoHeightReduction * 0.01f * fHeight, 0.0f, 1.0f );
			}
			base_id = terrain3dFarResolveMaterialId( rBakeData, base_id );
			overlay_id = terrain3dFarResolveMaterialId( rBakeData, overlay_id );
			const int dominant_material = fBlend >= 0.5f ? overlay_id : base_id;
			const float fInvSampleCount = color_samples > 0 ? 1.0f / ( float ) ( color_samples ) : 0.0f;
			const float fSurfaceR = ( float ) ( sum_r ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT;
			const float fSurfaceG = ( float ) ( sum_g ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT;
			const float fSurfaceB = ( float ) ( sum_b ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT;
			const float fSurfaceAlpha = color_samples > 0 ? ( float ) ( sum_a ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT : 0.5f;
			const int atlas_index = ( atlas_origin_y + y ) * nAtlasSize + atlas_origin_x + x;
			const size_t atlas_offset = ( size_t ) ( atlas_index ) * 4u;
			pAtlasWrite[atlas_offset] = terrain3dColorToByte( fSurfaceR );
			pAtlasWrite[atlas_offset + 1u] = terrain3dColorToByte( fSurfaceG );
			pAtlasWrite[atlas_offset + 2u] = terrain3dColorToByte( fSurfaceB );
			pAtlasWrite[atlas_offset + 3u] = terrain3dFarSurfaceAlpha(
				fSolidCoverage, fSurfaceAlpha, dominant_material );
		}
	}
	return true;
}

bool T3DData::rebuildGpuFarHeightAtlas( void ) {
	if ( !isFarAtlasRequired() || !m_bOwnsActiveStorage || !initializeGpuFarAtlasImages() ) {
		return false;
	}
	const int nAtlasSize = getGpuFarHeightAtlasSide();
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	PackedByteArray atlas_bytes = m_pGpuFarHeightImage->get_data();
	m_nGpuFarHeightRegionsLastTick = 0;
	m_nGpuFarHeightDirtyTexelsLast = 0u;
	m_nGpuFarHeightRebuiltBytesLast = 0u;
	m_nGpuFarHeightUploadedBytesLast = 0u;
	m_nGpuFarHeightPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t ) ( nAtlasSize ) * ( uint64_t ) ( nAtlasSize );
	const int samples_per_region = m_nRegionMapSize > 0 ? nAtlasSize / m_nRegionMapSize : 0;
	const uint64_t region_texels = samples_per_region > 0 ? ( uint64_t ) ( samples_per_region ) * ( uint64_t ) ( samples_per_region ) : 0u;
	if ( m_bGpuFarHeightDirtyAll ) {
		m_nGpuFarHeightDirtyTexelsLast = atlas_texels;
		const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
		uint8_t *const pWrite = atlas_bytes.ptrw();
		for ( int index = 0; index < nAtlasSize * nAtlasSize; ++index ) {
			std::memcpy( pWrite + ( size_t ) ( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
			const Vector2i location = m_szRegionLocations[slot];
			if ( location != V2I_MAX && patchGpuFarHeightRegion( location, atlas_bytes ) ) {
				++m_nGpuFarHeightRegionsLastTick;
			}
		}
	} else {
		m_nGpuFarHeightDirtyTexelsLast = ( uint64_t ) ( m_gpuFarHeightDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
			if ( !m_gpuFarHeightDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location(
				( int ) ( index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
				( int ) ( index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
			);
			if ( patchGpuFarHeightRegion( location, atlas_bytes ) ) {
				++m_nGpuFarHeightRegionsLastTick;
			}
		}
	}
	m_nGpuFarHeightRebuiltBytesLast = m_bGpuFarHeightDirtyAll ? ( uint64_t ) ( atlas_bytes.size() ) :
		( uint64_t ) ( m_nGpuFarHeightRegionsLastTick ) * region_texels * 2u;
	m_pGpuFarHeightImage->set_data( nAtlasSize, nAtlasSize,
		false, Image::FORMAT_R16, atlas_bytes );
	m_nGpuFarHeightProcessingUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	const uint64_t publication_started_usec = Time::get_singleton()->get_ticks_usec();
	const bool bUploaded = m_generatedFarHeightMap.getRid().is_valid() ?
		m_generatedFarHeightMap.update2d( m_pGpuFarHeightImage ) :
		m_generatedFarHeightMap.create( m_pGpuFarHeightImage ).is_valid();
	m_nGpuFarHeightPublicationUsecLast = Time::get_singleton()->get_ticks_usec() - publication_started_usec;
	if ( !bUploaded ) {
		return false;
	}
	m_nGpuFarHeightUploadedBytesLast = ( uint64_t ) ( atlas_bytes.size() );
	m_nGpuFarHeightUploadedBytesTotal += m_nGpuFarHeightUploadedBytesLast;
	++m_nGpuFarHeightPublicationCount;
	m_bGpuFarHeightDirty = false;
	m_bGpuFarHeightDirtyAll = false;
	m_gpuFarHeightDirtyRegions.reset();
	++m_nGpuFarHeightUpdatesLastTick;
	return true;
}

bool T3DData::rebuildGpuFarColorAtlas( void ) {
	if ( !isFarAtlasRequired() || !initializeGpuFarAtlasImages() ) {
		return false;
	}
	const int nAtlasSize = getGpuFarSurfaceAtlasSide();
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	PackedByteArray atlas_bytes = m_pGpuFarColorImage->get_data();
	t3dfarSurfaceBakeData_t bake_data;
	terrain3dBuildFarSurfaceBakeData( m_pTerrain, bake_data );
	m_nGpuFarColorRegionsLastTick = 0;
	m_nGpuFarColorDirtyTexelsLast = 0u;
	m_nGpuFarColorRebuiltBytesLast = 0u;
	m_nGpuFarColorUploadedBytesLast = 0u;
	m_nGpuFarColorPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t ) ( nAtlasSize ) * ( uint64_t ) ( nAtlasSize );
	const int samples_per_region = m_nRegionMapSize > 0 ? nAtlasSize / m_nRegionMapSize : 0;
	const uint64_t region_texels = samples_per_region > 0 ? ( uint64_t ) ( samples_per_region ) * ( uint64_t ) ( samples_per_region ) : 0u;
	if ( m_bGpuFarColorDirtyAll ) {
		m_nGpuFarColorDirtyTexelsLast = atlas_texels;
		terrain3dFillFarSurfaceEmpty( atlas_bytes );
		for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
			const Vector2i location = m_szRegionLocations[slot];
			if ( location != V2I_MAX && patchGpuFarColorRegion( location, atlas_bytes, bake_data ) ) {
				++m_nGpuFarColorRegionsLastTick;
			}
		}
	} else {
		m_nGpuFarColorDirtyTexelsLast = ( uint64_t ) ( m_gpuFarColorDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
			if ( !m_gpuFarColorDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location(
				( int ) ( index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
				( int ) ( index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
			);
			if ( patchGpuFarColorRegion( location, atlas_bytes, bake_data ) ) {
				++m_nGpuFarColorRegionsLastTick;
			}
		}
	}
	m_nGpuFarColorRebuiltBytesLast = m_bGpuFarColorDirtyAll ? ( uint64_t ) ( atlas_bytes.size() ) :
		( uint64_t ) ( m_nGpuFarColorRegionsLastTick ) * region_texels * 4u;
	m_pGpuFarColorImage->set_data( nAtlasSize, nAtlasSize,
		false, Image::FORMAT_RGBA8, atlas_bytes );
	m_nGpuFarColorProcessingUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	const uint64_t publication_started_usec = Time::get_singleton()->get_ticks_usec();
	const bool bUploaded = m_generatedFarColorMap.getRid().is_valid() ?
		m_generatedFarColorMap.update2d( m_pGpuFarColorImage ) :
		m_generatedFarColorMap.create( m_pGpuFarColorImage ).is_valid();
	m_nGpuFarColorPublicationUsecLast = Time::get_singleton()->get_ticks_usec() - publication_started_usec;
	if ( !bUploaded ) {
		return false;
	}
	m_nGpuFarColorUploadedBytesLast = ( uint64_t ) ( atlas_bytes.size() );
	m_nGpuFarColorUploadedBytesTotal += m_nGpuFarColorUploadedBytesLast;
	++m_nGpuFarColorPublicationCount;
	m_bGpuFarColorDirty = false;
	m_bGpuFarColorDirtyAll = false;
	m_gpuFarColorDirtyRegions.reset();
	++m_nGpuFarColorUpdatesLastTick;
	return true;
}

int T3DData::findGpuRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int layer = ( int ) ( m_szGpuRegionMap[map_index] ) - 1;
	if ( layer < 0 || layer >= m_nGpuRegionPhysicalLayerCount ||
		m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
		m_szGpuRegionLocations[layer] != vLocation ) {
		return -1;
	}
	return layer;
}

int T3DData::findFreeGpuRegionSlot( void ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionLocations[layer] == V2I_MAX &&
			m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE ) {
			return layer;
		}
	}
	return -1;
}

void T3DData::recordGpuRegionDeparture( const int nLayer, const bool bCountUnusedPrefetch ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return;
	}
	if ( m_szGpuRegionLoadedTick[nLayer] > 0u && m_nGpuRegionTick >= m_szGpuRegionLoadedTick[nLayer] ) {
		m_nGpuRegionResidentTicksTotal += m_nGpuRegionTick - m_szGpuRegionLoadedTick[nLayer];
		++m_nGpuRegionDepartedPagesTotal;
	}
	if ( bCountUnusedPrefetch && m_szGpuRegionPrefetchOrigin[nLayer] != 0u ) {
		++m_nGpuRegionPrefetchDiscardedTotal;
	}
	m_szGpuRegionLoadedTick[nLayer] = 0u;
	m_szGpuRegionPrefetchOrigin[nLayer] = 0u;
}

void T3DData::retireGpuRegionSlot( const int nLayer ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ||
		m_szGpuRegionSlotStates[nLayer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
		return;
	}
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_RETIRED;
	m_szGpuRegionRetireAfterFrame[nLayer] = std::numeric_limits<uint64_t>::max();
	++m_nGpuRegionRetiredCount;
}

void T3DData::armPendingGpuRegionRetirements( void ) {
	Engine *const pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	const uint64_t release_frame = current_frame + ( uint64_t ) ( MAX( 1, m_nGpuRegionRetireFrames ) );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED &&
			m_szGpuRegionRetireAfterFrame[layer] == std::numeric_limits<uint64_t>::max() ) {
			m_szGpuRegionRetireAfterFrame[layer] = release_frame;
		}
	}
}

void T3DData::reclaimRetiredGpuRegionSlots( void ) {
	Engine *const pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_RETIRED ||
			m_szGpuRegionRetireAfterFrame[layer] == std::numeric_limits<uint64_t>::max() ||
			current_frame < m_szGpuRegionRetireAfterFrame[layer] ) {
			continue;
		}
		m_szGpuRegionLocations[layer] = V2I_MAX;
		m_szGpuRegionLastUseTick[layer] = 0u;
		m_szGpuRegionLoadedTick[layer] = 0u;
		m_szGpuRegionPrefetchOrigin[layer] = 0u;
		m_szGpuRegionMaterialMasks[layer] = 0u;
		m_szGpuRegionRetireAfterFrame[layer] = 0u;
		m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
		m_nGpuRegionRetiredCount = MAX( 0, m_nGpuRegionRetiredCount - 1 );
		++m_nGpuRegionReusedLayersLastTick;
	}
}

void T3DData::invalidateGpuRegionLocation( const Vector2i &vLocation ) {
	if ( !m_bGpuRegionCacheEnabled ) {
		return;
	}
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		return;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szGpuRegionMap[map_index] = 0u;
	}
	recordGpuRegionDeparture( layer, true );
	retireGpuRegionSlot( layer );
	m_nGpuRegionResidentCount = MAX( 0, m_nGpuRegionResidentCount - 1 );
	m_bGpuRegionCacheSelectionDirty = true;
}

bool T3DData::uploadGpuRegionLayer(
	const int nLayer, const Vector2i &vLocation,
	const t3dmapType_t eMapType
) {
	m_nGpuPrepareConsumedRequestUsec = 0u;
	const T3DProfileScopeTimer publicationTimer(
		m_nGpuPreparePublicationUsecLast,
		m_nGpuPrepareConsumedRequestUsec, m_nGpuPrepareEndToEndUsecLast
	);
	if ( !m_bGpuRegionCacheEnabled || !m_bGpuRegionCacheTexturesReady || nLayer < 0 ||
		nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return false;
	}
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	ensureDefaultMaps();

	const uint8_t source_channel_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t previous_channel_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	const bool bUploadHeight = eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
	// Optional channels are updated only when either the incoming page or the
	// previous physical contents are explicit. Reusing default-on-default layers
	// requires no texture update, while explicit-to-default transitions still clear
	// stale physical contents before publication.
	const bool bUploadControl = ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX ) &&
		!m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
			source_channel_flags, previous_channel_flags, ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL ) );
	const bool bUploadColor = ( eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) &&
		isGpuRegionColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
			source_channel_flags, previous_channel_flags, ( uint8_t ) ( T3DRegion::CHANNEL_COLOR ) );
	Ref<Image> pHeight;
	Ref<Image> pDerived;
	Ref<Image> pControl;
	Ref<Image> pColor;
	Ref<Image> pMiddleComposite;
	const uint8_t previous_state = m_szGpuRegionSlotStates[nLayer];
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_STAGING;

	bool bCookedPage = false;
	uint32_t cooked_material_mask = 1u;
	if ( eMapType == T3DRegion::TYPE_MAX && pRegion->runtimeMapsEvictable() && m_pTerrain &&
		m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		bool bPreparedAttempted = false;
		bCookedPage = takePreparedGpuRuntimePage( vLocation, pHeight, pControl, pColor, pDerived, pMiddleComposite,
			cooked_material_mask, bPreparedAttempted );
		if ( !bPreparedAttempted && isGpuRuntimePagePreparePending( vLocation ) ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING;
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( !bPreparedAttempted ) {
			// Normal residency updates schedule cooked-page work before publication.
			// An unscheduled evictable page is deferred rather than synchronously reading
			// storage on the physics thread.
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE;
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( bCookedPage ) {
			++m_nGpuRegionCookedHitsLastTick;
		} else {
			++m_nGpuRegionCookedMissesLastTick;
		}
	}
	if ( !bCookedPage ) {
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( vLocation, true ) ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		++m_nGpuRegionRuntimePreparesLastTick;
	}

	// Resolve the complete publication set before overwriting a physical layer.
	// The page table continues to reference the previous logical region until all
	// source images are known to be valid.
	const bool bDerivedChannelEnabled = isGpuRegionDerivedChannelEnabled();
	if ( bUploadHeight && !bCookedPage ) {
		pHeight = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
		if ( pHeight.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( bDerivedChannelEnabled ) {
			pDerived = pRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() ) {
				++m_nGpuRegionRuntimeDerivedBuildsLastTick;
				regenerateDerivedMaps( {vLocation} );
				pDerived = pRegion->getDerivedNormalSlopeMap();
			}
			if ( pDerived.is_null() ) {
				pDerived = m_pDefaultDerivedMap;
			}
			if ( pDerived.is_null() ) {
				m_szGpuRegionSlotStates[nLayer] = previous_state;
				return false;
			}
			pDerived = makeGpuDerivedImage( pDerived );
			if ( pDerived.is_null() ) {
				m_szGpuRegionSlotStates[nLayer] = previous_state;
				return false;
			}
		}
	}
	if ( bUploadControl ) {
		if ( pControl.is_null() ) {
			pControl = makeGpuRenderControlImage( vLocation );
		}
		if ( pControl.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}
	if ( bUploadColor ) {
		if ( pColor.is_null() ) {
			const Ref<Image> pSourceColor = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR );
			if ( pSourceColor.is_valid() && ( !pSourceColor->has_mipmaps() ||
				( m_pTerrain && m_pTerrain->getGpuRegionColorResolutionDivisor() > 1 ) ) ) {
				++m_nGpuRegionRuntimeColorMipsLastTick;
			}
			pColor = makeGpuColorImage( pSourceColor );
		}
		if ( pColor.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}
	if ( eMapType == T3DRegion::TYPE_MAX && pMiddleComposite.is_null() ) {
		Ref<Image> pCompositeControl = pControl;
		if ( pCompositeControl.is_null() ) {
			pCompositeControl = makeGpuRenderControlImage( vLocation );
		}
		Ref<Image> pCompositeColor = pColor;
		if ( pCompositeColor.is_null() ) {
			pCompositeColor = makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) );
		}
		pMiddleComposite = makeMiddleCompositeImage( vLocation, pCompositeControl, pCompositeColor );
		if ( pMiddleComposite.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}

	bool bUploadOk = true;
	if ( bUploadHeight ) {
		bUploadOk = m_generatedHeightMaps.update( pHeight, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionHeightUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pHeight );
			m_nGpuRegionHeightBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
		if ( bUploadOk && bDerivedChannelEnabled ) {
			bUploadOk = m_generatedDerivedMaps.update( pDerived, nLayer );
		}
		if ( bUploadOk && bDerivedChannelEnabled ) {
			++m_nGpuRegionDerivedUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pDerived );
			m_nGpuRegionDerivedBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
		if ( bDerivedChannelEnabled && !bCookedPage && !IS_EDITOR && pDerived != m_pDefaultDerivedMap ) {
			pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
		}
	}
	if ( bUploadOk && bUploadControl ) {
		bUploadOk = m_generatedControlMaps.update( pControl, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionControlUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pControl );
			m_nGpuRegionControlBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
	}
	if ( bUploadOk && bUploadColor ) {
		bUploadOk = m_generatedColorMaps.update( pColor, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionColorUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pColor );
			m_nGpuRegionColorBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
	}
	if ( bUploadOk && eMapType == T3DRegion::TYPE_MAX ) {
		bUploadOk = m_generatedMiddleCompositeMaps.update( pMiddleComposite, nLayer );
		if ( bUploadOk ) {
			m_nGpuRegionUploadBytesLastTick += terrain3dGpuImageBytes( pMiddleComposite );
		}
	}
	if ( !bUploadOk ) {
		m_szGpuRegionSlotStates[nLayer] = previous_state;
		return false;
	}
	uint8_t published_flags = previous_channel_flags;
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX ) {
		published_flags &= ( uint8_t ) ( ~T3DRegion::CHANNEL_CONTROL );
		published_flags |= source_channel_flags & ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL );
	}
	if ( eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) {
		published_flags &= ( uint8_t ) ( ~T3DRegion::CHANNEL_COLOR );
		published_flags |= source_channel_flags & ( uint8_t ) ( T3DRegion::CHANNEL_COLOR );
	}
	m_szGpuRegionLayerChannelFlags[nLayer] = published_flags;
	uint32_t material_mask = bCookedPage ? cooked_material_mask :
		( pControl.is_valid() ? terrain3dMaterialMaskFromControlImage( pControl ) : 1u );
	if ( m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
			const int auto_base = CLAMP( terrain3dMaterialIntParam( pMaterial.ptr(), "auto_base_texture", 0 ), 0, 31 );
			const int auto_overlay = CLAMP( terrain3dMaterialIntParam( pMaterial.ptr(), "auto_overlay_texture", 1 ), 0, 31 );
			material_mask |= 1u << ( uint32_t ) ( auto_base );
			material_mask |= 1u << ( uint32_t ) ( auto_overlay );
		}
	}
	m_szGpuRegionMaterialMasks[nLayer] = material_mask;
	m_szGpuRegionSlotStates[nLayer] = m_szGpuRegionLocations[nLayer] == vLocation ?
		TERRAIN3D_GPU_SLOT_MAPPED : TERRAIN3D_GPU_SLOT_READY;
	return true;
}

void T3DData::queueGpuRegionRepublish( const Vector2i &vLocation ) {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 || map_index >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return;
	}
	if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
		m_szGpuRegionRepublishPending[map_index] = 1u;
		++m_nGpuRegionRepublishPendingCount;
	}
	// Force the ordinary per-frame residency tick to revisit deferred authoring
	// publications even when the camera and streaming generation are stationary.
	m_bGpuRegionCacheSelectionDirty = true;
}

bool T3DData::republishGpuRegionLocation( const Vector2i &vLocation ) {
	const int old_layer = findGpuRegionSlot( vLocation );
	if ( old_layer < 0 ) {
		return false;
	}
	reclaimRetiredGpuRegionSlots();
	const int staging_layer = findFreeGpuRegionSlot();
	if ( staging_layer < 0 ) {
		// Authoring can commit several brush stamps before the renderer has advanced
		// enough frames to recycle retired layers. Coalesce the location and publish
		// the newest page later instead of dropping the edit or synchronizing the GPU.
		queueGpuRegionRepublish( vLocation );
		return false;
	}
	// A staging slot starts from default channel contents, so publish the entire
	// page even when only one source channel changed. The old mapped page remains
	// valid until the replacement contains a coherent cross-channel snapshot.
	if ( !uploadGpuRegionLayer( staging_layer, vLocation, T3DRegion::TYPE_MAX ) ) {
		return false;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_FREE;
		return false;
	}
	m_szGpuRegionLocations[staging_layer] = vLocation;
	m_szGpuRegionLastUseTick[staging_layer] = m_nGpuRegionTick;
	m_szGpuRegionLoadedTick[staging_layer] = m_szGpuRegionLoadedTick[old_layer];
	m_szGpuRegionPrefetchOrigin[staging_layer] = m_szGpuRegionPrefetchOrigin[old_layer];
	m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
	m_szGpuRegionMap[map_index] = ( uint16_t ) ( staging_layer + 1 );
	if ( m_szGpuRegionRepublishPending[map_index] != 0u ) {
		m_szGpuRegionRepublishPending[map_index] = 0u;
		m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
	}
	m_szGpuRegionLoadedTick[old_layer] = 0u;
	m_szGpuRegionPrefetchOrigin[old_layer] = 0u;
	retireGpuRegionSlot( old_layer );
	return true;
}

int T3DData::processPendingGpuRegionRepublishes( const int nLimit ) {
	if ( m_nGpuRegionRepublishPendingCount <= 0 || nLimit <= 0 || m_nGpuRegionPhysicalLayerCount <= 0 ) {
		return 0;
	}
	int published = 0;
	int visited = 0;
	int cursor = m_nGpuRegionRepublishCursor;
	while ( visited < TERRAIN3D_MAX_REGION_SLOTS && m_nGpuRegionRepublishPendingCount > 0 && published < nLimit ) {
		const int map_index = cursor;
		cursor = ( cursor + 1 ) % TERRAIN3D_MAX_REGION_SLOTS;
		++visited;
		if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
			continue;
		}
		const int map_x = map_index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
		const int map_y = map_index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
		const Vector2i location( map_x - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
			map_y - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2 );
		if ( findGpuRegionSlot( location ) < 0 ) {
			// An evicted page will be uploaded from current source data if it becomes
			// desirable again, so no separate republish remains necessary.
			m_szGpuRegionRepublishPending[map_index] = 0u;
			m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
			continue;
		}
		if ( findFreeGpuRegionSlot() < 0 ) {
			break;
		}
		if ( republishGpuRegionLocation( location ) ) {
			++published;
		}
	}
	m_nGpuRegionRepublishCursor = cursor;
	return published;
}

int T3DData::collectDesiredGpuRegions(
	const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity, t3dgpuCacheSelectionStats_t &rStats
) {
	rStats = t3dgpuCacheSelectionStats_s();
	if ( !pLocations || nCapacity <= 0 || !m_pTerrain ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_GPU_CACHE_MAX_LAYERS );
	int output_count = 0;
	uint8_t selected_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	auto append_location = [&]( const Vector2i &location, const t3dregionStreamingState_t wanted ) {
		if ( output_count >= capacity ) {
			return false;
		}
		if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
			m_pTerrain->getRegionStreamingStateInternal( location ) != wanted ) {
			return true;
		}
		const int map_index = getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u ) {
			return true;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
		return output_count < capacity;
	};
	auto append_state = [&]( const t3dregionStreamingState_t wanted ) {
		terrain3dForEachSquareRing( rCenter, m_nRegionMapSize,
			[&]( const Vector2i &location ) { return append_location( location, wanted ); } );
	};

	// Full surface pages are mandatory only for ACTIVE. HOT geometry is served by
	// the independent height cache below, so surface pressure can never move vertices.
	append_state( TERRAIN3D_REGION_STREAMING_ACTIVE );
	rStats.required = output_count;
	append_state( TERRAIN3D_REGION_STREAMING_HOT );
	rStats.guard = output_count - rStats.required;
	const int prefetch_begin = output_count;
	for ( int slot = 0; slot < m_nRegionLocationCount && output_count < capacity; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		const int map_index = location == V2I_MAX ? -1 : getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || !hasRegion( location ) ||
			!m_pTerrain->isRegionStreamingPrefetchInternal( location ) ) {
			continue;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
	}
	rStats.prefetch = output_count - prefetch_begin;
	rStats.written = output_count;
	return output_count;
}

void T3DData::updateActiveHeightResidency( const bool bForce ) {
	m_nActiveHeightReconciliationsLastTick = 0;
	m_nActiveHeightSlotsExaminedLastTick = 0;
	if ( !m_pTerrain || !m_bOwnsActiveStorage || !terrain3dActiveStorageIsOwner( this ) ) {
		return;
	}
	const bool authoring = m_pTerrain->isEditorAuthoringMode();
	const uint64_t streaming_generation = m_pTerrain->getRegionStreamingGeneration();
	const Vector2i active_center = getRegionLocation( m_pTerrain->getClipmapTargetPosition() );
	if ( !bForce && !m_bActiveHeightResidencyDirty &&
		streaming_generation == m_nActiveHeightStreamingGenerationLast &&
		active_center == m_activeHeightCenter ) {
		return;
	}
	m_nActiveHeightReconciliationsLastTick = 1;
	const bool cooked_runtime = !authoring && m_pTerrain->isGpuRegionCookedPagesEnabled();
	const int active_radius = 2; // 5x5 working set; bounded below the 32-page bank.
	bool bAllRequiredResident = true;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		++m_nActiveHeightSlotsExaminedLastTick;
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX || !hasRegion( location ) ) {
			continue;
		}
		const Vector2i active_delta = location - active_center;
		const bool inside_active_bank =
			MAX( ABS( active_delta.x ), ABS( active_delta.y ) ) <= active_radius;
		const bool required = inside_active_bank && ( authoring ||
			m_pTerrain->getRegionStreamingStateInternal( location ) >= TERRAIN3D_REGION_STREAMING_HOT );
		const bool predicted = !authoring && m_pTerrain->isRegionStreamingPrefetchInternal( location );
		const bool resident = terrain3dActiveStorageContains( this, location );
		if ( !required ) {
			if ( resident ) {
				terrain3dActiveStorageInvalidate( this, location );
			} else if ( cooked_runtime && predicted ) {
				requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT, 3u );
			}
			continue;
		}
		if ( resident ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			bAllRequiredResident = false;
			continue;
		}

		bool published = false;
		if ( cooked_runtime ) {
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT, 1u );
			Ref<Image> pPreparedHeight;
			PackedByteArray prepared_height_bytes;
			if ( takePreparedRuntimeHeight( location, RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT,
				pPreparedHeight, false, &prepared_height_bytes ) && pPreparedHeight.is_valid() ) {
				published = terrain3dActiveStorageSyncHeightPage(
					this, location, prepared_height_bytes.ptr(), ( size_t ) ( prepared_height_bytes.size() ) );
			}
		}

		// Already-resident authoritative data can still publish immediately. Runtime
		// never forces a synchronous source restore here; the CPU cache prewarms that
		// resource independently and the cooked page supplies height in the meantime.
		if ( !published && pRegion->getHeightMap().is_valid() ) {
			published = syncActiveRegion( location, pRegion );
		}
		if ( published ) {
			continue;
		}
		bAllRequiredResident = false;
		if ( m_bCpuRegionCacheEnabled && !authoring ) {
			requestCpuRegionSourceAsync( location );
			continue;
		}
		if ( authoring && !syncActiveRegion( location, pRegion ) ) {
			UtilityFunctions::push_error( vformat(
				"Terrain3D: Failed to publish required authoring height payload for region %s.", location ) );
		}
	}
	m_nActiveHeightStreamingGenerationLast = streaming_generation;
	m_activeHeightCenter = active_center;
	m_bActiveHeightResidencyDirty = !bAllRequiredResident;
}


void T3DData::resetMiddleHeightCache( void ) {
	m_generatedMiddleHeightMaps.clear();
	m_generatedMiddleHeightRegionMap.clear();
	std::fill_n( m_szMiddleHeightRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightLocations, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szMiddleHeightLastUseTick, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightRetireAfterFrame, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightSlotStates, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS,
		( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
	m_nMiddleHeightPhysicalLayerCount = 0;
	m_nMiddleHeightMappedCapacity = 0;
	m_nMiddleHeightResidentCount = 0;
	m_nMiddleHeightDesiredLastTick = 0;
	m_nMiddleHeightMissingRequiredLastTick = 0;
	m_nMiddleHeightUploadsLastTick = 0;
	m_nMiddleHeightEvictionsLastTick = 0;
	m_nMiddleHeightTick = 0u;
	m_middleHeightCenter = V2I_MAX;
	m_bMiddleHeightTexturesReady = false;
	m_bMiddleHeightPageTableDirty = true;
	m_bMiddleHeightSelectionDirty = true;
	m_nMiddleHeightStreamingGenerationLast = 0u;
}

bool T3DData::configureMiddleHeightCache( void ) {
	if ( !m_pTerrain || m_nRegionSize <= 0 || m_nRegionMapSize <= 0 || m_nActiveRegionCount <= 0 ) {
		return false;
	}
	ensureDefaultMaps();
	const bool authoring = m_pTerrain->isEditorAuthoringMode();
	const int hot_radius = authoring ? m_nRegionMapSize : m_pTerrain->getStreamingHotRadius();
	const int diameter = hot_radius * 2 + 1;
	const int required_ceiling = authoring ? m_nActiveRegionCount :
		MIN( m_nActiveRegionCount, diameter * diameter );
	const int boundary_spares = authoring ? 0 : MAX( TERRAIN3D_GPU_HEIGHT_CACHE_MIN_SPARE_LAYERS,
		4 * hot_radius + 4 );
	m_nMiddleHeightMappedCapacity = MIN( required_ceiling, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS );
	m_nMiddleHeightPhysicalLayerCount = MIN( m_nActiveRegionCount,
		MIN( TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, m_nMiddleHeightMappedCapacity + boundary_spares ) );
	if ( m_nMiddleHeightPhysicalLayerCount <= 0 || m_nMiddleHeightMappedCapacity < required_ceiling ) {
		UtilityFunctions::push_error( "Terrain3D: Middle height cache cannot satisfy the configured Active + Hot footprint." );
		return false;
	}

	const int divisor = authoring ? 1 : MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
	const int height_side = MAX( 1, ( m_nRegionSize + divisor - 1 ) / divisor );
	PackedByteArray default_height_bytes;
	default_height_bytes.resize( ( int64_t ) ( height_side ) * ( int64_t ) ( height_side ) * 2 );
	const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
	for ( int index = 0; index < height_side * height_side; ++index ) {
		std::memcpy( default_height_bytes.ptrw() + ( size_t ) ( index ) * 2u,
			&default_height, sizeof( default_height ) );
	}
	const Ref<Image> pMiddleDefault = Image::create_from_data( height_side, height_side, false,
		Image::FORMAT_R16, default_height_bytes );
	if ( pMiddleDefault.is_null() ) {
		return false;
	}
	TypedArray<Image> layers;
	layers.resize( m_nMiddleHeightPhysicalLayerCount );
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		layers[layer] = pMiddleDefault;
	}
	if ( !m_generatedMiddleHeightMaps.create( layers ).is_valid() ) {
		UtilityFunctions::push_error( "Terrain3D: Failed to allocate the Middle height-only texture array." );
		resetMiddleHeightCache();
		return false;
	}
	std::fill_n( m_szMiddleHeightRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightLocations, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szMiddleHeightSlotStates, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS,
		( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
	m_bMiddleHeightTexturesReady = true;
	m_bMiddleHeightPageTableDirty = true;
	return updateMiddleHeightPageTableTexture();
}

int T3DData::findMiddleHeightSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int layer = ( int ) ( m_szMiddleHeightRegionMap[map_index] ) - 1;
	if ( layer < 0 || layer >= m_nMiddleHeightPhysicalLayerCount ||
		m_szMiddleHeightSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
		m_szMiddleHeightLocations[layer] != vLocation ) {
		return -1;
	}
	return layer;
}

Ref<Image> T3DData::makeMiddleHeightImage( const Vector2i &vLocation, const bool bRuntimeResolution ) const {
	T3DActiveMapLease lease = acquireActiveMap( vLocation );
	const uint16_t *pActiveHeight = nullptr;
	PackedByteArray source_bytes;
	const uint8_t *pSourceBytes = nullptr;
	if ( lease.valid() && lease.view().nRegionSize == m_nRegionSize && lease.view().pHeight ) {
		pActiveHeight = lease.view().pHeight;
	} else {
		const T3DRegion *const pRegion = getRegionPtr( vLocation );
		const Ref<Image> pHeight = pRegion && !pRegion->isDeleted() ? pRegion->getHeightMap() : Ref<Image>();
		if ( pHeight.is_null() || pHeight->get_width() != m_nRegionSize || pHeight->get_height() != m_nRegionSize ||
			pHeight->get_format() != Image::FORMAT_R16 || pHeight->has_mipmaps() ) {
			return Ref<Image>();
		}
		source_bytes = pHeight->get_data();
		const size_t expected_source_bytes = ( size_t ) ( m_nRegionSize ) * ( size_t ) ( m_nRegionSize ) * sizeof( uint16_t );
		if ( ( size_t ) ( source_bytes.size() ) != expected_source_bytes ) {
			return Ref<Image>();
		}
		pSourceBytes = source_bytes.ptr();
	}
	const bool use_runtime_resolution = bRuntimeResolution || ( m_pTerrain && !m_pTerrain->isEditorAuthoringMode() );
	const int divisor = m_pTerrain && use_runtime_resolution ?
		MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() ) : 1;
	const int side = MAX( 1, ( m_nRegionSize + divisor - 1 ) / divisor );
	const size_t byte_count = ( size_t ) ( side ) * ( size_t ) ( side ) * sizeof( uint16_t );
	PackedByteArray bytes;
	bytes.resize( ( int64_t ) ( byte_count ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return Ref<Image>();
	}
	if ( divisor == 1 ) {
		if ( pActiveHeight ) {
			std::memcpy( pWrite, pActiveHeight, byte_count );
		} else {
			std::memcpy( pWrite, pSourceBytes, byte_count );
		}
	} else {
		for ( int y = 0; y < side; ++y ) {
			const int source_y = MIN( y * divisor, m_nRegionSize - 1 );
			for ( int x = 0; x < side; ++x ) {
				const int source_x = MIN( x * divisor, m_nRegionSize - 1 );
				const size_t source_offset = ( size_t ) ( source_y * m_nRegionSize + source_x ) * sizeof( uint16_t );
				uint16_t value = 0u;
				if ( pActiveHeight ) {
					value = pActiveHeight[source_y * m_nRegionSize + source_x];
				} else {
					std::memcpy( &value, pSourceBytes + source_offset, sizeof( value ) );
				}
				std::memcpy( pWrite + ( size_t ) ( y * side + x ) * sizeof( uint16_t ),
					&value, sizeof( value ) );
			}
		}
	}
	return Image::create_from_data( side, side, false, Image::FORMAT_R16, bytes );
}

bool T3DData::uploadMiddleHeightLayer(
	const int nLayer, const Vector2i &vLocation, const Ref<Image> &rPreparedHeight
) {
	if ( !m_bMiddleHeightTexturesReady || nLayer < 0 || nLayer >= m_nMiddleHeightPhysicalLayerCount ) {
		return false;
	}
	Ref<Image> pHeight = rPreparedHeight;
	if ( pHeight.is_null() ) {
		pHeight = makeMiddleHeightImage( vLocation );
	}
	if ( pHeight.is_null() || pHeight->is_empty() || !m_generatedMiddleHeightMaps.update( pHeight, nLayer ) ) {
		return false;
	}
	++m_nMiddleHeightUploadsLastTick;
	return true;
}


int T3DData::collectDesiredMiddleHeightRegions(
	const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity, int &rRequiredCount
) const {
	rRequiredCount = 0;
	if ( !m_pTerrain || !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS );
	int output_count = 0;
	uint8_t selected_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int state_value = ( int ) ( TERRAIN3D_REGION_STREAMING_ACTIVE );
		state_value >= ( int ) ( TERRAIN3D_REGION_STREAMING_HOT ) && output_count < capacity; --state_value ) {
		const t3dregionStreamingState_t wanted = ( t3dregionStreamingState_t ) ( state_value );
		terrain3dForEachSquareRing( rCenter, m_nRegionMapSize, [&]( const Vector2i &location ) {
			if ( output_count >= capacity ) {
				return false;
			}
			if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
				m_pTerrain->getRegionStreamingStateInternal( location ) != wanted ) {
				return true;
			}
			const int map_index = getRegionMapIndex( location );
			if ( map_index < 0 || selected_map[map_index] != 0u ) {
				return true;
			}
			pLocations[output_count++] = location;
			selected_map[map_index] = 1u;
			return output_count < capacity;
		} );
	}
	rRequiredCount = output_count;
	for ( int slot = 0; slot < m_nRegionLocationCount && output_count < capacity; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		const int map_index = location == V2I_MAX ? -1 : getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || !hasRegion( location ) ||
			!m_pTerrain->isRegionStreamingPrefetchInternal( location ) ) {
			continue;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
	}
	return output_count;
}

bool T3DData::updateMiddleHeightPageTableTexture( void ) {
	if ( m_nRegionMapSize <= 0 ) {
		return false;
	}
	PackedByteArray bytes;
	bytes.resize( m_nRegionMapSize * m_nRegionMapSize * ( int ) ( sizeof( uint16_t ) ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	std::memset( pWrite, 0, ( size_t ) ( bytes.size() ) );
	const int half = m_nRegionMapSize / 2;
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szMiddleHeightLocations[layer];
		const int x = location.x + half;
		const int y = location.y + half;
		if ( ( uint32_t ) ( x ) >= ( uint32_t ) ( m_nRegionMapSize ) ||
			( uint32_t ) ( y ) >= ( uint32_t ) ( m_nRegionMapSize ) ) {
			continue;
		}
		const uint16_t value = ( uint16_t ) ( layer + 1 );
		std::memcpy( pWrite + ( size_t ) ( y * m_nRegionMapSize + x ) * sizeof( uint16_t ),
			&value, sizeof( uint16_t ) );
	}
	const Ref<Image> pImage = Image::create_from_data( m_nRegionMapSize, m_nRegionMapSize, false,
		Image::FORMAT_R16, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bExisting = m_generatedMiddleHeightRegionMap.getRid().is_valid();
	bool ok = bExisting ? m_generatedMiddleHeightRegionMap.update2d( pImage ) :
		m_generatedMiddleHeightRegionMap.create( pImage ).is_valid();
	if ( !ok && bExisting ) {
		m_generatedMiddleHeightRegionMap.clear();
		ok = m_generatedMiddleHeightRegionMap.create( pImage ).is_valid();
	}
	m_bMiddleHeightPageTableDirty = !ok;
	return ok;
}

void T3DData::updateMiddleHeightResidency( const Vector2i &rCenter, const bool bForce ) {
	m_nMiddleHeightUploadsLastTick = 0;
	m_nMiddleHeightEvictionsLastTick = 0;
	m_nMiddleHeightMissingRequiredLastTick = 0;
	if ( !m_pTerrain || !m_pTerrain->isRuntimeShaderTieringActive() || m_nActiveRegionCount <= 0 ) {
		return;
	}
	++m_nMiddleHeightTick;
	const uint64_t streaming_generation = m_pTerrain->getRegionStreamingGeneration();
	bool bRetirementPending = false;
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED ) {
			bRetirementPending = true;
			break;
		}
	}
	if ( !bForce && m_bMiddleHeightTexturesReady && rCenter == m_middleHeightCenter &&
		streaming_generation == m_nMiddleHeightStreamingGenerationLast &&
		!m_bMiddleHeightSelectionDirty && !m_bMiddleHeightPageTableDirty && !bRetirementPending ) {
		return;
	}
	const bool large_jump = m_middleHeightCenter != V2I_MAX &&
		MAX( Math::abs( rCenter.x - m_middleHeightCenter.x ), Math::abs( rCenter.y - m_middleHeightCenter.y ) ) > 1;
	if ( !m_bMiddleHeightTexturesReady ) {
		if ( !configureMiddleHeightCache() ) {
			return;
		}
	} else if ( large_jump ) {
		std::fill_n( m_szMiddleHeightRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
		std::fill_n( m_szMiddleHeightLocations, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, V2I_MAX );
		std::fill_n( m_szMiddleHeightSlotStates, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS,
			( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
		std::fill_n( m_szMiddleHeightRetireAfterFrame, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, 0u );
		m_nMiddleHeightResidentCount = 0;
		m_bMiddleHeightPageTableDirty = true;
	}
	m_middleHeightCenter = rCenter;
	m_nMiddleHeightStreamingGenerationLast = streaming_generation;

	Engine *const pEngine = Engine::get_singleton();
	const uint64_t frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nMiddleHeightTick;
	bRetirementPending = false;
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED ) {
			if ( frame >= m_szMiddleHeightRetireAfterFrame[layer] ) {
				m_szMiddleHeightSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
				m_szMiddleHeightLocations[layer] = V2I_MAX;
				m_szMiddleHeightRetireAfterFrame[layer] = 0u;
			} else {
				bRetirementPending = true;
			}
		}
	}

	Vector2i desired[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS];
	int required_count = 0;
	const int desired_count = collectDesiredMiddleHeightRegions( rCenter, desired,
		m_nMiddleHeightMappedCapacity, required_count );
	m_nMiddleHeightDesiredLastTick = desired_count;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}

	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szMiddleHeightLocations[layer];
		const int map_index = getRegionMapIndex( location );
		if ( map_index >= 0 && desired_map[map_index] != 0u ) {
			m_szMiddleHeightLastUseTick[layer] = m_nMiddleHeightTick;
			continue;
		}
		if ( map_index >= 0 ) {
			m_szMiddleHeightRegionMap[map_index] = 0u;
		}
		m_szMiddleHeightSlotStates[layer] = TERRAIN3D_GPU_SLOT_RETIRED;
		m_szMiddleHeightRetireAfterFrame[layer] = frame + ( uint64_t ) ( MAX( 1, m_nGpuRegionRetireFrames ) );
		m_nMiddleHeightResidentCount = MAX( 0, m_nMiddleHeightResidentCount - 1 );
		++m_nMiddleHeightEvictionsLastTick;
		m_bMiddleHeightPageTableDirty = true;
		bRetirementPending = true;
	}

	const int upload_budget = bForce ? TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS :
		MAX( 1, getEffectiveGpuRegionUploadBudgetLayers() );
	const bool cooked_runtime = m_pTerrain->isGpuRegionCookedPagesEnabled();
	bool bAllDesiredResident = true;
	for ( int index = 0; index < desired_count; ++index ) {
		const Vector2i location = desired[index];
		if ( findMiddleHeightSlot( location ) >= 0 ) {
			continue;
		}
		bAllDesiredResident = false;
		const bool required = index < required_count;
		if ( cooked_runtime ) {
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, required ? 2u : 3u );
		}
		if ( !bForce && m_nMiddleHeightUploadsLastTick >= upload_budget ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}
		int free_layer = -1;
		for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
			if ( m_szMiddleHeightSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE ) {
				free_layer = layer;
				break;
			}
		}
		if ( free_layer < 0 ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}

		Ref<Image> pPreparedHeight;
		if ( cooked_runtime && !takePreparedRuntimeHeight( location,
			RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, pPreparedHeight, true ) ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}
		if ( !uploadMiddleHeightLayer( free_layer, location, pPreparedHeight ) ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}
		m_szMiddleHeightLocations[free_layer] = location;
		m_szMiddleHeightLastUseTick[free_layer] = m_nMiddleHeightTick;
		m_szMiddleHeightSlotStates[free_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int map_index = getRegionMapIndex( location );
		if ( map_index >= 0 ) {
			m_szMiddleHeightRegionMap[map_index] = ( uint16_t ) ( free_layer + 1 );
		}
		++m_nMiddleHeightResidentCount;
		m_bMiddleHeightPageTableDirty = true;
	}
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findMiddleHeightSlot( desired[index] ) < 0 ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	if ( m_bMiddleHeightPageTableDirty || bForce ) {
		updateMiddleHeightPageTableTexture();
	}
	m_bMiddleHeightSelectionDirty = !bAllDesiredResident || bRetirementPending || m_bMiddleHeightPageTableDirty;
}


Dictionary T3DData::getMiddleHeightCacheStats( void ) const {
	Dictionary stats;
	stats["resident"] = m_nMiddleHeightResidentCount;
	stats["mapped_capacity"] = m_nMiddleHeightMappedCapacity;
	stats["physical_layers"] = m_nMiddleHeightPhysicalLayerCount;
	stats["desired_last_tick"] = m_nMiddleHeightDesiredLastTick;
	stats["missing_required_last_tick"] = m_nMiddleHeightMissingRequiredLastTick;
	stats["uploads_last_tick"] = m_nMiddleHeightUploadsLastTick;
	stats["evictions_last_tick"] = m_nMiddleHeightEvictionsLastTick;
	stats["page_table_valid"] = m_generatedMiddleHeightRegionMap.getRid().is_valid();
	stats["height_array_valid"] = m_generatedMiddleHeightMaps.getRid().is_valid();
	stats["resolution_divisor"] = m_pTerrain && !m_pTerrain->isEditorAuthoringMode() ?
		m_pTerrain->getGpuMiddleHeightResolutionDivisor() : 1;
	stats["reserved_bytes"] = ( int64_t ) ( getMiddleHeightCacheReservedBytes() );
	return stats;
}

int T3DData::fillGpuMaterialPriorityIds( uint8_t *const pIds, const int nCapacity ) const {
	if ( !pIds || nCapacity <= 0 ) {
		return 0;
	}
	uint32_t emitted_mask = 0u;
	int count = 0;
	auto append_mask = [&]( uint32_t mask ) {
		while ( mask != 0u && count < nCapacity ) {
			int id = 0;
			while ( id < TERRAIN3D_MAX_TEXTURE_ASSETS && ( mask & ( 1u << ( uint32_t ) ( id ) ) ) == 0u ) {
				++id;
			}
			if ( id >= TERRAIN3D_MAX_TEXTURE_ASSETS ) {
				break;
			}
			const uint32_t bit = 1u << ( uint32_t ) ( id );
			mask &= ~bit;
			if ( ( emitted_mask & bit ) != 0u ) {
				continue;
			}
			pIds[count++] = ( uint8_t ) ( id );
			emitted_mask |= bit;
		}
	};

	// Logical material zero is the stable fallback physical page.
	append_mask( 1u );
	for ( int priority = 0; priority < m_nGpuRegionPriorityCount && count < nCapacity; ++priority ) {
		const int layer = findGpuRegionSlot( m_szGpuRegionPriorityLocations[priority] );
		if ( layer >= 0 ) {
			append_mask( m_szGpuRegionMaterialMasks[layer] );
		}
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && count < nCapacity; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED ) {
			append_mask( m_szGpuRegionMaterialMasks[layer] );
		}
	}
	return count;
}

size_t T3DData::getMiddleHeightCacheReservedBytes( void ) const {
	if ( m_nMiddleHeightPhysicalLayerCount <= 0 || m_nRegionSize <= 0 ) {
		return 0u;
	}
	const int divisor = m_pTerrain && !m_pTerrain->isEditorAuthoringMode() ?
		MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() ) : 1;
	const size_t side = ( size_t ) ( MAX( 1, ( m_nRegionSize + divisor - 1 ) / divisor ) );
	const size_t page_table = ( size_t ) ( MAX( 0, m_nRegionMapSize ) ) *
		( size_t ) ( MAX( 0, m_nRegionMapSize ) ) * sizeof( uint16_t );
	return side * side * sizeof( uint16_t ) * ( size_t ) ( m_nMiddleHeightPhysicalLayerCount ) + page_table;
}

int T3DData::calculateCpuRegionCacheCapacity( void ) const {
	return terrain3dCpuCacheCalculateCapacity( m_nRegionSize, m_nCpuRegionCacheBudgetMb,
		TERRAIN3D_CPU_CACHE_MAX_LAYERS );
}

size_t T3DData::estimateCpuRegionLayerBytes( void ) const {
	return terrain3dCpuCacheEstimateLayerBytes( m_nRegionSize );
}

void T3DData::resetCpuRegionCache( const bool bReleaseCleanMaps ) {
	if ( bReleaseCleanMaps ) {
		for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
			evictCpuRegionSlot( slot );
		}
	}
	std::fill_n( m_szCpuRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szCpuRegionLocations, TERRAIN3D_CPU_CACHE_MAX_LAYERS, V2I_MAX );
	m_nCpuRegionCacheCapacity = 0;
	m_nCpuRegionResidentCount = 0;
	m_nCpuRegionDesiredLastTick = 0;
	m_nCpuRegionLoadsLastTick = 0;
	m_nCpuRegionLoadsSinceReset = 0u;
	m_nCpuRegionEvictionsLastTick = 0;
	m_nCpuRegionPinnedLastTick = 0;
	m_nCpuRegionLoadBytesLastTick = 0u;
	m_nCpuRegionAsyncRequestsLastTick = 0;
	m_nCpuRegionAsyncReadyLastTick = 0;
	m_nCpuRegionAsyncPendingLastTick = 0;
	m_nCpuRegionAsyncOutstanding = 0;
	m_nCpuRegionPrefetchStreamingGenerationLast = 0u;
	std::fill_n( m_szCpuRegionAsyncState, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	m_cpuRegionCacheCenter = V2I_MAX;
	m_bCpuRegionCacheSelectionDirty = true;
}

int T3DData::findCpuRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int slot = ( int ) ( m_szCpuRegionMap[map_index] ) - 1;
	if ( slot < 0 || slot >= m_nCpuRegionCacheCapacity || m_szCpuRegionLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void T3DData::invalidateCpuRegionLocation( const Vector2i &vLocation ) {
	const int slot = findCpuRegionSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szCpuRegionMap[map_index] = 0u;
	}
	m_szCpuRegionLocations[slot] = V2I_MAX;
	m_nCpuRegionResidentCount = MAX( 0, m_nCpuRegionResidentCount - 1 );
	m_bCpuRegionCacheSelectionDirty = true;
}

bool T3DData::loadCpuRegionSource( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) const {
	rSource.unref();
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayload() ) {
		rSource = Ref<T3DRegion>( pRegion );
		return true;
	}
	const String path = pRegion->get_path();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const Ref<Resource> pResource = ResourceLoader::get_singleton()->load( path, "Terrain3DRegion",
		ResourceLoader::CACHE_MODE_IGNORE );
	Ref<T3DRegion> pLoaded = pResource;
	if ( pLoaded.is_null() || pLoaded->getRegionSize() != m_nRegionSize ) {
		return false;
	}
	const Ref<Image> pHeight = pLoaded->getHeightMap();
	if ( pHeight.is_null() || pHeight->get_format() != Image::FORMAT_R16 ||
		!Terrain3DRegionCodec::hasValidHeightPayload( pHeight, m_nRegionSize ) ) {
		return false;
	}
	pLoaded->setModified( false );
	pLoaded->markRuntimeMapsClean();
	pLoaded->sanitizeMaps();
	rSource = pLoaded;
	return true;
}

bool T3DData::requestCpuRegionSourceAsync( const Vector2i &vLocation ) {
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 || !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayload() ||
		m_szCpuRegionAsyncState[map_index] != 0u ) {
		return pRegion && !pRegion->isDeleted();
	}
	const String path = pRegion->get_path();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		m_szCpuRegionAsyncState[map_index] = 2u;
		return false;
	}
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return false;
	}
	const Error error = loader->load_threaded_request( path, "Terrain3DRegion", true,
		ResourceLoader::CACHE_MODE_IGNORE );
	if ( error != OK && error != ERR_BUSY ) {
		m_szCpuRegionAsyncState[map_index] = 2u;
		return false;
	}
	m_szCpuRegionAsyncState[map_index] = 1u;
	++m_nCpuRegionAsyncOutstanding;
	++m_nCpuRegionAsyncRequestsLastTick;
	return true;
}

bool T3DData::takeCpuRegionSourceAsync( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) {
	rSource.unref();
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 || !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayload() ) {
		rSource = Ref<T3DRegion>( pRegion );
		return true;
	}
	if ( m_szCpuRegionAsyncState[map_index] != 1u ) {
		requestCpuRegionSourceAsync( vLocation );
		return false;
	}
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return false;
	}
	const String path = pRegion->get_path();
	const ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( path );
	if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
		++m_nCpuRegionAsyncPendingLastTick;
		return false;
	}
	if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
		m_szCpuRegionAsyncState[map_index] = 2u;
		m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
		return false;
	}
	const Ref<Resource> pResource = loader->load_threaded_get( path );
	Ref<T3DRegion> pLoaded = pResource;
	m_szCpuRegionAsyncState[map_index] = 0u;
	m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
	if ( pLoaded.is_null() || pLoaded->getRegionSize() != m_nRegionSize ) {
		return false;
	}
	const Ref<Image> pHeight = pLoaded->getHeightMap();
	if ( pHeight.is_null() || pHeight->get_format() != Image::FORMAT_R16 ||
		!Terrain3DRegionCodec::hasValidHeightPayload( pHeight, m_nRegionSize ) ) {
		return false;
	}
	pLoaded->setModified( false );
	pLoaded->markRuntimeMapsClean();
	pLoaded->sanitizeMaps();
	rSource = pLoaded;
	++m_nCpuRegionAsyncReadyLastTick;
	return true;
}

void T3DData::prewarmCpuRegionSources( void ) {
	if ( !m_pTerrain || !m_bCpuRegionCacheEnabled || m_pTerrain->isEditorAuthoringMode() ) {
		return;
	}
	const Vector2i center = m_pTerrain->getRegionStreamingPredictedCenterInternal();
	const int radius = MAX( 0, m_pTerrain->getStreamingActiveRadius() );
	for ( int y = -radius; y <= radius; ++y ) {
		for ( int x = -radius; x <= radius; ++x ) {
			const Vector2i location = center + Vector2i( x, y );
			if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
				findCpuRegionSlot( location ) >= 0 ) {
				continue;
			}
			T3DRegion *const pRegion = getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && !pRegion->hasRuntimeMapPayload() ) {
				requestCpuRegionSourceAsync( location );
			}
		}
	}
}

void T3DData::reapCpuRegionSourceAsync( void ) {
	if ( m_nCpuRegionAsyncOutstanding <= 0 || !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return;
	}
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return;
	}
	const Vector2i predicted_center = m_pTerrain->getRegionStreamingPredictedCenterInternal();
	const int active_radius = MAX( 0, m_pTerrain->getStreamingActiveRadius() );
	for ( int slot = 0; slot < m_nRegionLocationCount && m_nCpuRegionAsyncOutstanding > 0; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		const int map_index = location == V2I_MAX ? -1 : getRegionMapIndex( location );
		if ( map_index < 0 || m_szCpuRegionAsyncState[map_index] == 0u ) {
			continue;
		}
		const bool active_now = m_pTerrain->getRegionStreamingStateInternal( location ) ==
			TERRAIN3D_REGION_STREAMING_ACTIVE;
		const bool predicted_active = MAX( Math::abs( location.x - predicted_center.x ),
			Math::abs( location.y - predicted_center.y ) ) <= active_radius;
		if ( active_now || predicted_active ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->get_path().is_empty() ) {
			m_szCpuRegionAsyncState[map_index] = 0u;
			m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
			continue;
		}
		const String path = pRegion->get_path();
		const ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
			continue;
		}
		if ( status == ResourceLoader::THREAD_LOAD_LOADED ) {
			// Collect and immediately release stale completed requests.
			const Ref<Resource> discard = loader->load_threaded_get( path );
			( void ) discard;
		}
		m_szCpuRegionAsyncState[map_index] = 0u;
		m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
	}
}


bool T3DData::publishCpuRegionSlot(
	const int iSlot, const Vector2i &vLocation,
	const Ref<T3DRegion> &rSource
) {
	if ( iSlot < 0 || iSlot >= m_nCpuRegionCacheCapacity || rSource.is_null() ) {
		return false;
	}
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	pRegion->importRuntimeMapsUnmodified( rSource.ptr() );
	if ( !pRegion->hasRuntimeMapPayload() ) {
		return false;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return false;
	}
	m_szCpuRegionLocations[iSlot] = vLocation;
	m_szCpuRegionMap[map_index] = ( uint8_t ) ( iSlot + 1 );
	++m_nCpuRegionResidentCount;
	publishRegionSamplingGeneration( vLocation );
	return true;
}

bool T3DData::evictCpuRegionSlot( const int iSlot ) {
	if ( iSlot < 0 || iSlot >= m_nCpuRegionCacheCapacity ) {
		return false;
	}
	const Vector2i location = m_szCpuRegionLocations[iSlot];
	if ( location == V2I_MAX ) {
		return true;
	}
	T3DRegion *const pRegion = getRegionPtr( location );
	if ( pRegion && pRegion->hasRuntimeMapPayload() && !pRegion->runtimeMapsEvictable() ) {
		return false;
	}
	if ( m_pResidentPool ) {
		m_pResidentPool->invalidate( location );
	}
	if ( pRegion ) {
		pRegion->releaseRuntimeMapsUnmodified();
	}
	const int map_index = getRegionMapIndex( location );
	if ( map_index >= 0 ) {
		m_szCpuRegionMap[map_index] = 0u;
	}
	m_szCpuRegionLocations[iSlot] = V2I_MAX;
	m_nCpuRegionResidentCount = MAX( 0, m_nCpuRegionResidentCount - 1 );
	publishRegionSamplingGeneration( location );
	return true;
}

bool T3DData::ensureCpuRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	if ( !m_bCpuRegionCacheEnabled ) {
		return true;
	}
	const int existing = findCpuRegionSlot( vLocation );
	if ( existing >= 0 ) {
		return true;
	}
	T3DRegion *const pExistingRegion = getRegionPtr( vLocation );
	if ( pExistingRegion && pExistingRegion->hasRuntimeMapPayload() &&
		!pExistingRegion->runtimeMapsEvictable() ) {
		// Dirty, undo-owned, deleted, and pathless resources are deliberately
		// outside the bounded clean cache. Their payload is already available and
		// remains pinned until persistence/lifecycle state makes it evictable.
		return true;
	}
	Ref<T3DRegion> pSource;
	if ( !loadCpuRegionSource( vLocation, pSource ) ) {
		return false;
	}
	int target_slot = -1;
	for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
		if ( m_szCpuRegionLocations[slot] == V2I_MAX ) {
			target_slot = slot;
			break;
		}
	}
	if ( target_slot < 0 ) {
		int farthest_distance = -1;
		const Vector2i eviction_center = m_cpuRegionCacheCenter == V2I_MAX ? vLocation : m_cpuRegionCacheCenter;
		const t3dcpuCacheCoord_t center = {eviction_center.x, eviction_center.y};
		for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
			const Vector2i old_location = m_szCpuRegionLocations[slot];
			T3DRegion *const pOldRegion = getRegionPtr( old_location );
			if ( !pOldRegion || !pOldRegion->runtimeMapsEvictable() ) {
				continue;
			}
			const int distance = terrain3dCpuCacheDistance(
				{old_location.x, old_location.y}, center );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				target_slot = slot;
			}
		}
	}
	if ( target_slot < 0 ) {
		return false;
	}
	if ( m_szCpuRegionLocations[target_slot] != V2I_MAX ) {
		if ( !evictCpuRegionSlot( target_slot ) ) {
			return false;
		}
		++m_nCpuRegionEvictionsLastTick;
	}
	if ( !publishCpuRegionSlot( target_slot, vLocation, pSource ) ) {
		return false;
	}
	if ( bDemandLoad ) {
		++m_nCpuRegionDemandLoads;
	}
	return true;
}

void T3DData::releaseUntrackedCpuMaps( void ) {
	if ( !m_bCpuRegionCacheEnabled ) {
		return;
	}
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX || findCpuRegionSlot( location ) >= 0 ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || !pRegion->hasRuntimeMapPayload() || !pRegion->runtimeMapsEvictable() ) {
			continue;
		}
		if ( m_pResidentPool ) {
			m_pResidentPool->invalidate( location );
		}
		pRegion->releaseRuntimeMapsUnmodified();
		publishRegionSamplingGeneration( location );
	}
}

bool T3DData::restoreAllCpuMaps( void ) {
	bool bSuccess = true;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayload() ) {
			continue;
		}
		Ref<T3DRegion> pSource;
		if ( !loadCpuRegionSource( location, pSource ) ) {
			bSuccess = false;
			continue;
		}
		pRegion->importRuntimeMapsUnmodified( pSource.ptr() );
		publishRegionSamplingGeneration( location );
	}
	return bSuccess;
}

int T3DData::collectDesiredCpuRegions(
	const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity
) const {
	if ( !pLocations || nCapacity <= 0 || !m_pTerrain ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_CPU_CACHE_MAX_LAYERS );
	int output_count = 0;
	terrain3dForEachSquareRing( rCenter, m_nRegionMapSize, [&]( const Vector2i &location ) {
		if ( output_count >= capacity ) {
			return false;
		}
		if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
			m_pTerrain->getRegionStreamingStateInternal( location ) != TERRAIN3D_REGION_STREAMING_ACTIVE ) {
			return true;
		}
		pLocations[output_count++] = location;
		return output_count < capacity;
	} );
	return output_count;
}

void T3DData::setRuntimePageBudgetOverrides(
	const int nGpuUploadLayers,
	const int nCpuLoadLayers, const int nInstanceLoadPages
) {
	m_nRuntimeGpuRegionUploadBudgetLayers = nGpuUploadLayers < 0 ? -1 : CLAMP( nGpuUploadLayers, 0, 64 );
	m_nRuntimeCpuRegionLoadBudgetLayers = nCpuLoadLayers < 0 ? -1 : CLAMP( nCpuLoadLayers, 0, 64 );
	m_nRuntimeInstanceRegionLoadBudgetPages = nInstanceLoadPages < 0 ? -1 : CLAMP( nInstanceLoadPages, 0, 64 );
}

void T3DData::clearRuntimePageBudgetOverrides( void ) {
	m_nRuntimeGpuRegionUploadBudgetLayers = -1;
	m_nRuntimeCpuRegionLoadBudgetLayers = -1;
	m_nRuntimeInstanceRegionLoadBudgetPages = -1;
}

int T3DData::getEffectiveGpuRegionUploadBudgetLayers( void ) const {
	return m_nRuntimeGpuRegionUploadBudgetLayers < 0 ? m_nGpuRegionUploadBudgetLayers :
		MIN( m_nGpuRegionUploadBudgetLayers, m_nRuntimeGpuRegionUploadBudgetLayers );
}

int T3DData::getEffectiveCpuRegionLoadBudgetLayers( void ) const {
	return m_nRuntimeCpuRegionLoadBudgetLayers < 0 ? m_nCpuRegionLoadBudgetLayers :
		MIN( m_nCpuRegionLoadBudgetLayers, m_nRuntimeCpuRegionLoadBudgetLayers );
}

int T3DData::getEffectiveInstanceRegionLoadBudgetPages( void ) const {
	return m_nRuntimeInstanceRegionLoadBudgetPages < 0 ? m_nInstanceRegionLoadBudgetPages :
		MIN( m_nInstanceRegionLoadBudgetPages, m_nRuntimeInstanceRegionLoadBudgetPages );
}

void T3DData::configureCpuRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadLayers, const int nLoadBudgetMb
) {
	const bool bOldEnabled = m_bCpuRegionCacheEnabled;
	const int old_budget = m_nCpuRegionCacheBudgetMb;
	m_bCpuRegionCacheEnabled = bEnabled;
	m_nCpuRegionCacheBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_CPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_CPU_REGION_CACHE_MAX_BUDGET_MB );
	m_nCpuRegionLoadBudgetLayers = CLAMP( nLoadLayers, 1, TERRAIN3D_CPU_REGION_LOAD_MAX_LAYERS );
	m_nCpuRegionLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, TERRAIN3D_CPU_REGION_LOAD_MAX_BUDGET_MB );
	if ( !bOldEnabled && !m_bCpuRegionCacheEnabled ) {
		return;
	}
	if ( bOldEnabled && !m_bCpuRegionCacheEnabled ) {
		if ( !restoreAllCpuMaps() ) {
			m_bCpuRegionCacheEnabled = true;
			m_bCpuRegionCacheSelectionDirty = true;
			LOG( T3DTerrain::ERROR, "CPU region cache remains enabled because one or more region payloads could not be restored" );
			return;
		}
		resetCpuRegionCache( false );
		return;
	}
	const int capacity = calculateCpuRegionCacheCapacity();
	if ( !bOldEnabled || old_budget != m_nCpuRegionCacheBudgetMb || capacity != m_nCpuRegionCacheCapacity ) {
		resetCpuRegionCache( false );
		m_nCpuRegionCacheCapacity = capacity;
		m_bCpuRegionCacheSelectionDirty = true;
	}
}

bool T3DData::ensureCpuRegionResident( const Vector2i &vLocation ) {
	if ( !m_bCpuRegionCacheEnabled ) {
		return hasRegion( vLocation );
	}
	if ( !hasRegion( vLocation ) ) {
		return false;
	}
	return ensureCpuRegionResidentInternal( vLocation, true );
}

bool T3DData::isCpuRegionPayloadAvailable( const Vector2i &vLocation ) const {
	if ( !m_bCpuRegionCacheEnabled ) {
		return true;
	}
	const T3DRegion *pRegion = getRegionPtr( vLocation );
	return pRegion && !pRegion->isDeleted() && pRegion->hasRuntimeMapPayload();
}

bool T3DData::ensureCpuSampleArea( const Vector3 &rWorldPosition, const int nRadiusTexels ) {
	if ( !m_bCpuRegionCacheEnabled || m_nRegionSize <= 0 || m_fVertexSpacing <= 0.0f ) {
		return true;
	}
	const int radius = MAX( 0, nRadiusTexels ) + 1;
	const Vector2i center(
		( int ) ( Math::floor( rWorldPosition.x / m_fVertexSpacing ) ),
		( int ) ( Math::floor( rWorldPosition.z / m_fVertexSpacing ) )
	);
	const Vector2i extent( radius * 2 + 1, radius * 2 + 1 );
	const Rect2i bounds = getRegionBoundsForArea( Rect2i( center - Vector2i( radius, radius ), extent ), m_nRegionSize );
	bool bSuccess = true;
	for ( int y = bounds.position.y; y < bounds.get_end().y; ++y ) {
		for ( int x = bounds.position.x; x < bounds.get_end().x; ++x ) {
			const Vector2i location( x, y );
			if ( hasRegion( location ) && !ensureCpuRegionResident( location ) ) {
				bSuccess = false;
			}
		}
	}
	return bSuccess;
}

void T3DData::updateCpuRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	const T3DLastScopeTimer serviceTimer( m_nCpuRegionServiceUsecLast,
		m_pTerrain && m_pTerrain->isRuntimeProfilingEnabled() );
	m_nCpuRegionLoadsLastTick = 0;
	m_nCpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nCpuRegionEvictionsLastTick = 0;
	m_nCpuRegionPinnedLastTick = 0;
	m_nCpuRegionLoadBytesLastTick = 0u;
	m_nCpuRegionAsyncRequestsLastTick = 0;
	m_nCpuRegionAsyncReadyLastTick = 0;
	m_nCpuRegionAsyncPendingLastTick = 0;
	if ( !m_bCpuRegionCacheEnabled || m_nCpuRegionCacheCapacity <= 0 || m_nRegionSize <= 0 ||
		m_fVertexSpacing <= 0.0f ) {
		return;
	}

	// Predictive CPU requests only need to be reconsidered when the shared streaming
	// policy changes. Crossing a boundary should normally promote an already-loaded
	// Terrain3DRegion resource instead of invoking ResourceLoader::load() in physics.
	const uint64_t streaming_generation = m_pTerrain ? m_pTerrain->getRegionStreamingGeneration() : 0u;
	if ( bForce || streaming_generation != m_nCpuRegionPrefetchStreamingGenerationLast ) {
		prewarmCpuRegionSources();
		m_nCpuRegionPrefetchStreamingGenerationLast = streaming_generation;
	}
	reapCpuRegionSourceAsync();

	const Vector2i raw_center = getRegionLocation( rWorldPosition );
	const t3dgpuCacheCoord_t clamped = terrain3dGpuCacheClampCenter(
		{raw_center.x, raw_center.y}, m_nRegionMapSize );
	const Vector2i center( clamped.x, clamped.y );
	if ( !bForce && center == m_cpuRegionCacheCenter && !m_bCpuRegionCacheSelectionDirty ) {
		return;
	}
	Vector2i desired[TERRAIN3D_CPU_CACHE_MAX_LAYERS];
	const int desired_count = collectDesiredCpuRegions( center, desired, m_nCpuRegionCacheCapacity );
	m_nCpuRegionDesiredLastTick = desired_count;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}
	const size_t layer_bytes = estimateCpuRegionLayerBytes();
	const size_t byte_budget = ( size_t ) ( m_nCpuRegionLoadBudgetMb ) * TERRAIN3D_CPU_CACHE_MIB;
	const int load_layer_budget = getEffectiveCpuRegionLoadBudgetLayers();
	bool bAllDesiredResident = true;
	for ( int desired_index = 0; desired_index < desired_count; ++desired_index ) {
		const Vector2i location = desired[desired_index];
		if ( findCpuRegionSlot( location ) >= 0 ) {
			continue;
		}
		T3DRegion *const pAvailableRegion = getRegionPtr( location );
		if ( pAvailableRegion && pAvailableRegion->hasRuntimeMapPayload() &&
			!pAvailableRegion->runtimeMapsEvictable() ) {
			continue;
		}
		bAllDesiredResident = false;
		if ( !bForce && m_nCpuRegionLoadsLastTick >= load_layer_budget ) {
			m_nCpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET;
			break;
		}
		if ( !bForce && m_nCpuRegionLoadsLastTick > 0 &&
			m_nCpuRegionLoadBytesLastTick + layer_bytes > byte_budget ) {
			m_nCpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET;
			break;
		}
		Ref<T3DRegion> pSource;
		const bool bSourceReady = bForce ? loadCpuRegionSource( location, pSource ) :
			takeCpuRegionSourceAsync( location, pSource );
		if ( !bSourceReady ) {
			if ( !bForce ) {
				requestCpuRegionSourceAsync( location );
			}
			continue;
		}
		int target_slot = -1;
		for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
			if ( m_szCpuRegionLocations[slot] == V2I_MAX ) {
				target_slot = slot;
				break;
			}
		}
		if ( target_slot < 0 ) {
			int farthest_distance = -1;
			for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
				const Vector2i old_location = m_szCpuRegionLocations[slot];
				const int old_map_index = getRegionMapIndex( old_location );
				if ( old_map_index >= 0 && desired_map[old_map_index] != 0u ) {
					continue;
				}
				T3DRegion *const pOldRegion = getRegionPtr( old_location );
				if ( !pOldRegion || !pOldRegion->runtimeMapsEvictable() ) {
					++m_nCpuRegionPinnedLastTick;
					continue;
				}
				const int distance = MAX( std::abs( old_location.x - center.x ),
					std::abs( old_location.y - center.y ) );
				if ( distance > farthest_distance ) {
					farthest_distance = distance;
					target_slot = slot;
				}
			}
		}
		if ( target_slot < 0 ) {
			m_nCpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY;
			continue;
		}
		if ( m_szCpuRegionLocations[target_slot] != V2I_MAX ) {
			if ( !evictCpuRegionSlot( target_slot ) ) {
				continue;
			}
			++m_nCpuRegionEvictionsLastTick;
		}
		if ( !publishCpuRegionSlot( target_slot, location, pSource ) ) {
			continue;
		}
		++m_nCpuRegionLoadsLastTick;
		++m_nCpuRegionLoadsSinceReset;
		m_nCpuRegionLoadBytesLastTick += layer_bytes;
	}
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			const Vector2i location = desired[index];
			T3DRegion *const pAvailableRegion = getRegionPtr( location );
			const bool bPinnedAvailable = pAvailableRegion && pAvailableRegion->hasRuntimeMapPayload() &&
				!pAvailableRegion->runtimeMapsEvictable();
			if ( findCpuRegionSlot( location ) < 0 && !bPinnedAvailable ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	m_cpuRegionCacheCenter = center;
	m_bCpuRegionCacheSelectionDirty = !bAllDesiredResident;
	releaseUntrackedCpuMaps();
}


Dictionary T3DData::getCpuRegionCacheStatsFast( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bCpuRegionCacheEnabled;
	stats["budget_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["capacity"] = m_nCpuRegionCacheCapacity;
	stats["resident"] = m_nCpuRegionResidentCount;
	stats["desired_last_tick"] = m_nCpuRegionDesiredLastTick;
	stats["loads_last_tick"] = m_nCpuRegionLoadsLastTick;
	stats["loads_since_reset"] = ( int64_t ) ( m_nCpuRegionLoadsSinceReset );
	stats["evictions_last_tick"] = m_nCpuRegionEvictionsLastTick;
	stats["pinned_last_tick"] = m_nCpuRegionPinnedLastTick;
	stats["demand_loads"] = m_nCpuRegionDemandLoads;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nCpuRegionLoadBytesLastTick );
	stats["async_requests_last_tick"] = m_nCpuRegionAsyncRequestsLastTick;
	stats["async_ready_last_tick"] = m_nCpuRegionAsyncReadyLastTick;
	stats["async_pending_last_tick"] = m_nCpuRegionAsyncPendingLastTick;
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nCpuRegionServiceUsecLast );
	stats["selection_pending"] = m_bCpuRegionCacheSelectionDirty;
	stats["reserved_bytes"] = ( int64_t ) ( getCpuRegionCacheReservedBytes() );
	stats["active_height_bytes"] = ( int64_t ) ( getActiveStorageReservedBytes() );
	stats["active_height_physical_capacity"] = terrain3dActiveStorageGetPhysicalCapacity();
	stats["active_height_reconciliations_last_tick"] = m_nActiveHeightReconciliationsLastTick;
	stats["active_height_slots_examined_last_tick"] = m_nActiveHeightSlotsExaminedLastTick;
	stats["active_height_reader_retries"] = ( int64_t ) ( terrain3dActiveStorageGetReaderRetryCount() );
	stats["active_height_reader_mutexes"] = ( int64_t ) ( terrain3dActiveStorageGetReaderMutexCount() );
	stats["active_height_publications"] = ( int64_t ) ( terrain3dActiveStorageGetPublicationCount() );
	return stats;
}

Dictionary T3DData::getCpuRegionCacheStats( void ) const {
	Dictionary stats;
	int pinned_payload_count = 0;
	int total_payload_count = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		const T3DRegion *pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || !pRegion->hasRuntimeMapPayload() ) {
			continue;
		}
		++total_payload_count;
		if ( findCpuRegionSlot( location ) < 0 ) {
			++pinned_payload_count;
		}
	}
	stats["enabled"] = m_bCpuRegionCacheEnabled;
	stats["budget_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["capacity"] = m_nCpuRegionCacheCapacity;
	stats["resident"] = m_nCpuRegionResidentCount;
	stats["desired_last_tick"] = m_nCpuRegionDesiredLastTick;
	stats["load_layer_budget"] = m_nCpuRegionLoadBudgetLayers;
	stats["effective_load_layer_budget"] = getEffectiveCpuRegionLoadBudgetLayers();
	stats["load_budget_mb"] = m_nCpuRegionLoadBudgetMb;
	stats["loads_last_tick"] = m_nCpuRegionLoadsLastTick;
	stats["loads_since_reset"] = ( int64_t ) ( m_nCpuRegionLoadsSinceReset );
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nCpuRegionStallFlagsLastTick );
	stats["evictions_last_tick"] = m_nCpuRegionEvictionsLastTick;
	stats["pinned_last_tick"] = m_nCpuRegionPinnedLastTick;
	stats["pinned_payloads"] = pinned_payload_count;
	stats["total_payloads"] = total_payload_count;
	stats["demand_loads"] = m_nCpuRegionDemandLoads;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nCpuRegionLoadBytesLastTick );
	stats["async_requests_last_tick"] = m_nCpuRegionAsyncRequestsLastTick;
	stats["async_ready_last_tick"] = m_nCpuRegionAsyncReadyLastTick;
	stats["async_pending_last_tick"] = m_nCpuRegionAsyncPendingLastTick;
	const size_t layer_bytes = estimateCpuRegionLayerBytes();
	const size_t worst_case_bytes = layer_bytes * ( size_t ) ( MAX( 0, m_nCpuRegionCacheCapacity ) );
	const size_t budget_bytes = ( size_t ) ( m_nCpuRegionCacheBudgetMb ) * TERRAIN3D_CPU_CACHE_MIB;
	const size_t active_height_bytes = getActiveStorageReservedBytes();
	const size_t default_map_bytes = m_nRegionSize > 0 ? layer_bytes : 0u;
	const size_t payload_bytes = layer_bytes * ( size_t ) ( MAX( 0, total_payload_count ) );
	const size_t pinned_bytes = layer_bytes * ( size_t ) ( MAX( 0, pinned_payload_count ) );
	const size_t total_map_bytes = active_height_bytes + default_map_bytes + payload_bytes;
	stats["estimated_layer_bytes"] = ( int64_t ) ( layer_bytes );
	stats["estimated_worst_case_bytes"] = ( int64_t ) ( worst_case_bytes );
	stats["estimated_payload_bytes"] = ( int64_t ) ( payload_bytes );
	stats["estimated_pinned_bytes"] = ( int64_t ) ( pinned_bytes );
	stats["active_height_bytes"] = ( int64_t ) ( active_height_bytes );
	stats["active_height_physical_capacity"] = terrain3dActiveStorageGetPhysicalCapacity();
	stats["active_height_reconciliations_last_tick"] = m_nActiveHeightReconciliationsLastTick;
	stats["active_height_slots_examined_last_tick"] = m_nActiveHeightSlotsExaminedLastTick;
	stats["active_height_reader_retries"] = ( int64_t ) ( terrain3dActiveStorageGetReaderRetryCount() );
	stats["active_height_reader_mutexes"] = ( int64_t ) ( terrain3dActiveStorageGetReaderMutexCount() );
	stats["active_height_publications"] = ( int64_t ) ( terrain3dActiveStorageGetPublicationCount() );
	stats["default_map_bytes"] = ( int64_t ) ( default_map_bytes );
	stats["estimated_total_map_bytes"] = ( int64_t ) ( total_map_bytes );
	stats["target_map_ceiling_bytes"] = ( int64_t ) ( TERRAIN3D_XBOX360_CPU_MAP_CEILING_BYTES );
	stats["budget_overcommitted"] = worst_case_bytes > budget_bytes;
	stats["runtime_overcommitted"] = total_payload_count > m_nCpuRegionCacheCapacity;
	stats["target_map_ceiling_exceeded"] = total_map_bytes > TERRAIN3D_XBOX360_CPU_MAP_CEILING_BYTES;
	stats["center"] = m_cpuRegionCacheCenter;
	stats["selection_pending"] = m_bCpuRegionCacheSelectionDirty;
	return stats;
}

void T3DData::emitGpuRegionMapChanged( void ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	++m_nGpuRegionPageTableUpdatesLastTick;
	emit_signal( "gpu_region_page_table_changed" );
	armPendingGpuRegionRetirements();
}


void T3DData::resetInstanceRegionCache( const bool bRestoreAll ) {
	if ( bRestoreAll ) {
		for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
			const Vector2i location = m_szRegionLocations[slot];
			T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && pRegion->hasInstances() ) {
				pRegion->ensureInstanceStoreResident();
			}
		}
	} else {
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			evictInstanceRegionSlot( slot );
		}
	}
	std::fill_n( m_szInstanceRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szInstanceRegionLocations, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES, V2I_MAX );
	m_nInstanceRegionCacheCapacity = TERRAIN3D_INSTANCE_CACHE_MAX_PAGES;
	m_nInstanceRegionResidentCount = 0;
	m_nInstanceRegionDesiredLastTick = 0;
	m_nInstanceRegionLoadsLastTick = 0;
	m_nInstanceRegionEvictionsLastTick = 0;
	m_nInstanceRegionLoadBytesLastTick = 0u;
	m_nInstanceRegionResidentBytes = 0u;
	m_instanceRegionCacheCenter = V2I_MAX;
	m_nInstancePrefetchStreamingGenerationLast = 0u;
	m_bInstanceRegionCacheSelectionDirty = true;
}

int T3DData::findInstanceRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int slot = ( int ) ( m_szInstanceRegionMap[map_index] ) - 1;
	if ( slot < 0 || slot >= m_nInstanceRegionCacheCapacity || m_szInstanceRegionLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void T3DData::invalidateInstanceRegionLocation( const Vector2i &vLocation ) {
	const int slot = findInstanceRegionSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	evictInstanceRegionSlot( slot );
	m_bInstanceRegionCacheSelectionDirty = true;
}

bool T3DData::evictInstanceRegionSlot( const int iSlot ) {
	if ( !terrain3dInstanceCacheIndexValid( iSlot, m_nInstanceRegionCacheCapacity ) ) {
		return false;
	}
	const Vector2i location = m_szInstanceRegionLocations[iSlot];
	if ( location == V2I_MAX ) {
		return true;
	}
	T3DRegion *const pRegion = getRegionPtr( location );
	size_t resident_bytes = 0u;
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		resident_bytes = pRegion->getInstanceResidentBytes();
		const PackedByteArray page = pRegion->getInstancePage();
		if ( page.is_empty() && pRegion->hasInstances() ) {
			return false;
		}
	}
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->destroyRegionMmis( location );
	}
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		if ( !pRegion->evictInstanceStore() ) {
			return false;
		}
		m_nInstanceRegionResidentBytes = resident_bytes <= m_nInstanceRegionResidentBytes ?
			m_nInstanceRegionResidentBytes - resident_bytes : 0u;
	}
	const int map_index = getRegionMapIndex( location );
	if ( map_index >= 0 ) {
		m_szInstanceRegionMap[map_index] = 0u;
	}
	m_szInstanceRegionLocations[iSlot] = V2I_MAX;
	m_nInstanceRegionResidentCount = MAX( 0, m_nInstanceRegionResidentCount - 1 );
	return true;
}

bool T3DData::ensureInstanceRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	T3DRegion *pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( !m_bInstanceRegionCacheEnabled ) {
		return pRegion->ensureInstanceStoreResident();
	}
	if ( findInstanceRegionSlot( vLocation ) >= 0 ) {
		return true;
	}
	if ( !pRegion->hasInstances() ) {
		return true;
	}
	int target_slot = -1;
	for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
		if ( m_szInstanceRegionLocations[slot] == V2I_MAX ) {
			target_slot = slot;
			break;
		}
	}
	if ( target_slot < 0 ) {
		int farthest_distance = -1;
		const Vector2i center = m_instanceRegionCacheCenter == V2I_MAX ? vLocation : m_instanceRegionCacheCenter;
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			const Vector2i old_location = m_szInstanceRegionLocations[slot];
			const int distance = terrain3dInstanceCacheDistance(
				{old_location.x, old_location.y}, {center.x, center.y} );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				target_slot = slot;
			}
		}
	}
	if ( target_slot < 0 ) {
		return false;
	}
	if ( m_szInstanceRegionLocations[target_slot] != V2I_MAX && !evictInstanceRegionSlot( target_slot ) ) {
		return false;
	}
	if ( !pRegion->ensureInstanceStoreResident() ) {
		return false;
	}
	const size_t bytes = pRegion->getInstanceResidentBytes();
	const size_t budget = ( size_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	while ( bDemandLoad && m_nInstanceRegionResidentCount > 0 &&
		m_nInstanceRegionResidentBytes + bytes > budget ) {
		int farthest_slot = -1;
		int farthest_distance = -1;
		const Vector2i center = m_instanceRegionCacheCenter == V2I_MAX ? vLocation : m_instanceRegionCacheCenter;
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			const Vector2i old_location = m_szInstanceRegionLocations[slot];
			if ( old_location == V2I_MAX || old_location == vLocation ) {
				continue;
			}
			const int distance = terrain3dInstanceCacheDistance(
				{old_location.x, old_location.y}, {center.x, center.y} );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				farthest_slot = slot;
			}
		}
		if ( farthest_slot < 0 || !evictInstanceRegionSlot( farthest_slot ) ) {
			break;
		}
		++m_nInstanceRegionEvictionsLastTick;
	}
	if ( !bDemandLoad && !terrain3dInstanceCacheCanStream( m_nInstanceRegionResidentBytes, bytes, budget,
		m_nInstanceRegionResidentCount ) ) {
		pRegion->evictInstanceStore();
		return false;
	}
	m_szInstanceRegionLocations[target_slot] = vLocation;
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szInstanceRegionMap[map_index] = ( uint8_t ) ( target_slot + 1 );
	}
	++m_nInstanceRegionResidentCount;
	m_nInstanceRegionResidentBytes += bytes;
	if ( bDemandLoad ) {
		++m_nInstanceRegionDemandLoads;
	}
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, vLocation, false );
	}
	return true;
}

int T3DData::collectDesiredInstanceRegions(
	const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity
) const {
	if ( !pLocations || nCapacity <= 0 || !m_pTerrain ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES );
	int output_count = 0;
	for ( int state_value = ( int ) ( TERRAIN3D_REGION_STREAMING_ACTIVE );
		state_value >= ( int ) ( TERRAIN3D_REGION_STREAMING_HOT ) && output_count < capacity; --state_value ) {
		const t3dregionStreamingState_t wanted = ( t3dregionStreamingState_t ) ( state_value );
		terrain3dForEachSquareRing( rCenter, m_nRegionMapSize, [&]( const Vector2i &location ) {
			if ( output_count >= capacity ) {
				return false;
			}
			if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
				m_pTerrain->getRegionStreamingStateInternal( location ) != wanted ) {
				return true;
			}
			const T3DRegion *pRegion = getRegionPtr( location );
			if ( pRegion && pRegion->hasInstances() ) {
				pLocations[output_count++] = location;
			}
			return output_count < capacity;
		} );
	}
	return output_count;
}

void T3DData::configureInstanceRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadPages, const int nLoadBudgetMb
) {
	const bool bWasEnabled = m_bInstanceRegionCacheEnabled;
	m_bInstanceRegionCacheEnabled = bEnabled;
	m_nInstanceRegionCacheBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_INSTANCE_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_INSTANCE_REGION_CACHE_MAX_BUDGET_MB );
	m_nInstanceRegionLoadBudgetPages = CLAMP( nLoadPages, 1, TERRAIN3D_INSTANCE_REGION_LOAD_MAX_PAGES );
	m_nInstanceRegionLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, TERRAIN3D_INSTANCE_REGION_LOAD_MAX_BUDGET_MB );
	if ( bWasEnabled && !bEnabled ) {
		resetInstanceRegionCache( true );
		return;
	}
	if ( !bWasEnabled && bEnabled ) {
		resetInstanceRegionCache( false );
	}
	m_bInstanceRegionCacheSelectionDirty = true;
}

void T3DData::updateInstanceRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	const T3DLastScopeTimer serviceTimer( m_nInstanceRegionServiceUsecLast,
		m_pTerrain && m_pTerrain->isRuntimeProfilingEnabled() );
	m_nInstanceRegionLoadsLastTick = 0;
	m_nInstanceRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nInstanceRegionEvictionsLastTick = 0;
	m_nInstanceRegionLoadBytesLastTick = 0u;
	if ( !m_bInstanceRegionCacheEnabled || m_nRegionSize <= 0 || m_fVertexSpacing <= 0.0f ) {
		return;
	}
	const bool async_runtime = m_pTerrain && !m_pTerrain->isEditorAuthoringMode() &&
		m_pTerrain->isGpuRegionCookedPagesEnabled();
	const uint64_t streaming_generation = m_pTerrain ? m_pTerrain->getRegionStreamingGeneration() : 0u;
	if ( async_runtime && ( bForce || streaming_generation != m_nInstancePrefetchStreamingGenerationLast ) ) {
		// Predictive instance demand changes only when the streaming generation changes.
		// Avoid walking the authored region table on stationary frames.
		for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
			const Vector2i location = m_szRegionLocations[slot];
			T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() ||
				pRegion->isInstanceStoreResident() || findInstanceRegionSlot( location ) >= 0 ||
				!m_pTerrain->isRegionStreamingPrefetchInternal( location ) ) {
				continue;
			}
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, 3u );
		}
		m_nInstancePrefetchStreamingGenerationLast = streaming_generation;
	}
	const Vector2i raw_center = getRegionLocation( rWorldPosition );
	const t3dgpuCacheCoord_t clamped = terrain3dGpuCacheClampCenter(
		{raw_center.x, raw_center.y}, m_nRegionMapSize );
	const Vector2i center( clamped.x, clamped.y );
	if ( !bForce && center == m_instanceRegionCacheCenter && !m_bInstanceRegionCacheSelectionDirty ) {
		return;
	}
	Vector2i desired[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES];
	const int desired_count = collectDesiredInstanceRegions( center, desired, m_nInstanceRegionCacheCapacity );
	m_nInstanceRegionDesiredLastTick = desired_count;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}
	const size_t resident_budget = ( size_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	while ( m_nInstanceRegionResidentCount > 1 && m_nInstanceRegionResidentBytes > resident_budget ) {
		int farthest_slot = -1;
		int farthest_distance = -1;
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			const Vector2i location = m_szInstanceRegionLocations[slot];
			if ( location == V2I_MAX ) {
				continue;
			}
			const int distance = terrain3dInstanceCacheDistance(
				{location.x, location.y}, {center.x, center.y} );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				farthest_slot = slot;
			}
		}
		if ( farthest_slot < 0 || !evictInstanceRegionSlot( farthest_slot ) ) {
			break;
		}
		++m_nInstanceRegionEvictionsLastTick;
	}
	for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
		const Vector2i location = m_szInstanceRegionLocations[slot];
		const int map_index = getRegionMapIndex( location );
		if ( location != V2I_MAX && ( map_index < 0 || desired_map[map_index] == 0u ) ) {
			if ( evictInstanceRegionSlot( slot ) ) {
				++m_nInstanceRegionEvictionsLastTick;
			}
		}
	}
	const size_t load_budget = ( size_t ) ( m_nInstanceRegionLoadBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	const int load_page_budget = getEffectiveInstanceRegionLoadBudgetPages();
	bool bComplete = true;
	for ( int index = 0; index < desired_count; ++index ) {
		const Vector2i location = desired[index];
		if ( findInstanceRegionSlot( location ) >= 0 ) {
			continue;
		}
		bComplete = false;
		if ( !bForce && m_nInstanceRegionLoadsLastTick >= load_page_budget ) {
			m_nInstanceRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET;
			break;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		const size_t estimated = pRegion ? MAX<size_t>( pRegion->getInstancePageBytes(), 1u ) : 0u;
		if ( !bForce && m_nInstanceRegionLoadsLastTick > 0 &&
			m_nInstanceRegionLoadBytesLastTick + estimated > load_budget ) {
			m_nInstanceRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET;
			break;
		}
		if ( async_runtime && pRegion && !pRegion->isInstanceStoreResident() ) {
			const uint8_t priority = m_pTerrain->getRegionStreamingStateInternal( location ) ==
				TERRAIN3D_REGION_STREAMING_ACTIVE ? 1u : 2u;
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, priority );
			T3DInstanceStore prepared_store;
			if ( !takePreparedRuntimeInstance( location, prepared_store ) ) {
				continue;
			}
			pRegion->m_instanceStore = eastl::move( prepared_store );
			pRegion->m_bInstanceStoreResident = true;
			pRegion->m_bInstanceStoreModified = false;
		}
		if ( ensureInstanceRegionResidentInternal( location, false ) ) {
			++m_nInstanceRegionLoadsLastTick;
			m_nInstanceRegionLoadBytesLastTick += estimated;
		}
	}
	if ( !bComplete ) {
		bComplete = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findInstanceRegionSlot( desired[index] ) < 0 ) {
				bComplete = false;
				break;
			}
		}
	}
	m_instanceRegionCacheCenter = center;
	m_bInstanceRegionCacheSelectionDirty = !bComplete;
}

void T3DData::updateRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	// Finish prior cooked-page work before any consumer runs, then launch the next
	// union batch after Active/Middle/Near have published this frame's demand.
	serviceGpuRuntimePagePrepare();
	updateCpuRegionResidency( rWorldPosition, bForce );
	updateActiveHeightResidency( bForce );
	updateInstanceRegionResidency( rWorldPosition, bForce );
	// The dedicated Middle height cache is a runtime optimization. Authoring uses
	// authoritative full-resolution region data and the Near authoring shader, so
	// allocating a second height texture array in the editor would only duplicate
	// memory and would scale with authored-world size.
	if ( m_nRegionMapSize > 0 && m_pTerrain && !m_pTerrain->isEditorAuthoringMode() &&
		m_pTerrain->getRuntimeMiddleShaderLods() > 0 ) {
		const Vector2i raw_center = getRegionLocation( rWorldPosition );
		const t3dgpuCacheCoord_t clamped = terrain3dGpuCacheClampCenter(
			{ raw_center.x, raw_center.y }, m_nRegionMapSize );
		updateMiddleHeightResidency( Vector2i( clamped.x, clamped.y ), bForce );
	} else if ( m_bMiddleHeightTexturesReady ) {
		resetMiddleHeightCache();
	}
	updateGpuRegionResidency( rWorldPosition, bForce );
	serviceGpuRuntimePagePrepare();
}

void T3DData::resetRuntimeStreamingCaches( void ) {
	resetCpuRegionCache( true );
	resetInstanceRegionCache( false );
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	configureCpuRegionCache( m_bCpuRegionCacheEnabled, m_nCpuRegionCacheBudgetMb,
		m_nCpuRegionLoadBudgetLayers, m_nCpuRegionLoadBudgetMb );
	configureInstanceRegionCache( m_bInstanceRegionCacheEnabled, m_nInstanceRegionCacheBudgetMb,
		m_nInstanceRegionLoadBudgetPages, m_nInstanceRegionLoadBudgetMb );
	configureGpuRegionCache( m_bGpuRegionCacheEnabled, m_nGpuRegionCacheBudgetMb,
		m_nGpuRegionUploadBudgetLayers, m_nGpuRegionUploadBudgetMb, m_nGpuRegionSparePages,
		m_nGpuRegionRetireFrames );
}

void T3DData::appendRuntimeProfileSample( t3druntimeProfileSample_t &rSample ) const {
	rSample.stallFlags |= m_nGpuRegionStallFlagsLastTick | m_nCpuRegionStallFlagsLastTick |
		m_nInstanceRegionStallFlagsLastTick;
	rSample.gpuResident = m_nGpuRegionResidentCount;
	rSample.nGpuCapacity = m_nGpuRegionCacheCapacity;
	rSample.nGpuUploads = m_nGpuRegionUploadsLastTick;
	rSample.nGpuEvictions = m_nGpuRegionEvictionsLastTick;
	rSample.gpuMissingRequired = m_nGpuRegionMissingRequiredLastTick;
	rSample.nGpuUploadBytes = m_nGpuRegionUploadBytesLastTick;
	rSample.nGpuPreparedPages = m_nGpuRegionPreparedPagesLastTick;
	rSample.nGpuPrepareWorkers = m_nGpuRegionPrepareWorkersLastTick;
	rSample.gpuEffectiveUploadLimit = m_nGpuRegionEffectiveUploadLimitLastTick;
	rSample.gpuCameraMoved = m_bGpuRegionCameraMovedLastTick ? 1 : 0;
	rSample.nGpuRequestedPages = m_nGpuRegionRequestedPages;
	rSample.nGpuRequiredPages = m_nGpuRegionRequiredLastTick;
	rSample.nGpuGuardPages = m_nGpuRegionGuardLastTick;
	rSample.nGpuPrefetchPages = m_nGpuRegionPrefetchLastTick;
	rSample.nGpuHysteresisPages = m_nGpuRegionHysteresisLastTick;
	rSample.gpuPreparedQueue = m_nGpuPreparedPageCount;
	rSample.nGpuPreparePendingPages = m_bGpuPrepareJobActive ? m_nGpuPrepareWorkPageCount : 0;
	rSample.gpuPrepareJobActive = m_bGpuPrepareJobActive ? 1 : 0;
	rSample.nGpuCookedHits = m_nGpuRegionCookedHitsLastTick;
	rSample.nGpuCookedMisses = m_nGpuRegionCookedMissesLastTick;
	rSample.gpuRuntimePrepares = m_nGpuRegionRuntimePreparesLastTick;
	rSample.nGpuFarDirtyRegions = ( m_bGpuFarHeightDirtyAll || m_bGpuFarColorDirtyAll ) ?
		m_nActiveRegionCount : ( int ) ( ( m_gpuFarHeightDirtyRegions | m_gpuFarColorDirtyRegions ).count() );
	rSample.gpuFarRegionsProcessed = m_nGpuFarHeightRegionsLastTick + m_nGpuFarColorRegionsLastTick;
	rSample.nGpuPrepareLatencyUsec = m_nGpuPrepareLatencyUsecLast;
	rSample.nGpuPrepareQueueWaitUsec = m_nGpuPrepareQueueWaitUsecLast;
	rSample.nGpuPrepareFileOpenUsec = m_nGpuPrepareFileOpenUsecLast;
	rSample.nGpuPrepareFileReadUsec = m_nGpuPrepareFileReadUsecLast;
	rSample.nGpuPrepareValidationUsec = m_nGpuPrepareValidationUsecLast;
	rSample.nGpuPrepareMaterializeUsec = m_nGpuPrepareMaterializeUsecLast;
	rSample.nGpuPrepareWorkerUsec = m_nGpuPrepareWorkerUsecLast;
	rSample.nGpuPrepareReadyWaitUsec = m_nGpuPrepareReadyWaitUsecLast;
	rSample.nGpuPreparePublicationUsec = m_nGpuPreparePublicationUsecLast;
	rSample.nGpuPrepareEndToEndUsec = m_nGpuPrepareEndToEndUsecLast;
	rSample.nGpuCacheServiceUsec = m_nGpuRegionServiceUsecLast;
	rSample.cpuResident = m_nCpuRegionResidentCount;
	rSample.cpuLoads = m_nCpuRegionLoadsLastTick;
	rSample.nCpuEvictions = m_nCpuRegionEvictionsLastTick;
	rSample.nCpuLoadBytes = m_nCpuRegionLoadBytesLastTick;
	rSample.nCpuCacheServiceUsec = m_nCpuRegionServiceUsecLast;
	rSample.cpuDesired = m_nCpuRegionDesiredLastTick;
	rSample.cpuSelectionPending = m_bCpuRegionCacheSelectionDirty ? 1 : 0;
	rSample.cpuPinned = m_nCpuRegionPinnedLastTick;
	rSample.instanceResident = m_nInstanceRegionResidentCount;
	rSample.instanceLoads = m_nInstanceRegionLoadsLastTick;
	rSample.nInstanceEvictions = m_nInstanceRegionEvictionsLastTick;
	rSample.nInstanceLoadBytes = m_nInstanceRegionLoadBytesLastTick;
	rSample.nInstanceCacheServiceUsec = m_nInstanceRegionServiceUsecLast;
	rSample.instanceDesired = m_nInstanceRegionDesiredLastTick;
	rSample.instanceSelectionPending = m_bInstanceRegionCacheSelectionDirty ? 1 : 0;
}

Dictionary T3DData::getInstanceRegionCacheStatsFast( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bInstanceRegionCacheEnabled;
	stats["budget_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["capacity"] = m_nInstanceRegionCacheCapacity;
	stats["resident"] = m_nInstanceRegionResidentCount;
	stats["resident_bytes"] = ( int64_t ) ( m_nInstanceRegionResidentBytes );
	stats["desired_last_tick"] = m_nInstanceRegionDesiredLastTick;
	stats["loads_last_tick"] = m_nInstanceRegionLoadsLastTick;
	stats["evictions_last_tick"] = m_nInstanceRegionEvictionsLastTick;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nInstanceRegionLoadBytesLastTick );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nInstanceRegionServiceUsecLast );
	stats["demand_loads"] = m_nInstanceRegionDemandLoads;
	stats["selection_pending"] = m_bInstanceRegionCacheSelectionDirty;
	return stats;
}

Dictionary T3DData::getInstanceRegionCacheStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bInstanceRegionCacheEnabled;
	stats["budget_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["capacity"] = m_nInstanceRegionCacheCapacity;
	stats["resident"] = m_nInstanceRegionResidentCount;
	stats["resident_bytes"] = ( int64_t ) ( m_nInstanceRegionResidentBytes );
	stats["desired_last_tick"] = m_nInstanceRegionDesiredLastTick;
	stats["load_page_budget"] = m_nInstanceRegionLoadBudgetPages;
	stats["effective_load_page_budget"] = getEffectiveInstanceRegionLoadBudgetPages();
	stats["load_budget_mb"] = m_nInstanceRegionLoadBudgetMb;
	stats["loads_last_tick"] = m_nInstanceRegionLoadsLastTick;
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nInstanceRegionStallFlagsLastTick );
	stats["evictions_last_tick"] = m_nInstanceRegionEvictionsLastTick;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nInstanceRegionLoadBytesLastTick );
	stats["demand_loads"] = m_nInstanceRegionDemandLoads;
	stats["center"] = m_instanceRegionCacheCenter;
	stats["selection_pending"] = m_bInstanceRegionCacheSelectionDirty;
	stats["budget_overcommitted"] = m_nInstanceRegionResidentBytes >
		( size_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	return stats;
}

void T3DData::updateMapsBounded( const t3dmapType_t eMapType, const bool bAllRegions ) {
	const bool bImplicitStateChanged = refreshImplicitMapState();
	bool bTopologyChanged = false;
	if ( m_bRegionMapDirty ) {
		rebuildRegionMapFromSlots();
		bTopologyChanged = true;
		m_bGpuRegionCacheSelectionDirty = true;
		if ( !m_bStartupBulkLoad ) {
			emit_signal( "region_map_changed" );
		}
	}

	const int required_capacity = calculateGpuRegionCacheCapacity();
	const bool bRebuild = !m_bGpuRegionCacheTexturesReady || bImplicitStateChanged ||
		required_capacity != m_nGpuRegionCacheCapacity || m_generatedHeightMaps.isDirty() ||
		m_generatedControlMaps.isDirty() || m_generatedColorMaps.isDirty() ||
		m_generatedDerivedMaps.isDirty();
	if ( bRebuild && !rebuildGpuRegionCacheTextures() ) {
		return;
	}

	bool bHeightChanged = false;
	bool bControlChanged = false;
	bool bColorChanged = false;
	bool bPhysicalMappingChanged = false;
	if ( bAllRegions && !bRebuild ) {
		Vector2i resident_locations[TERRAIN3D_GPU_CACHE_MAX_LAYERS];
		int resident_location_count = 0;
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
			if ( m_szGpuRegionLocations[layer] != V2I_MAX &&
				m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED ) {
				resident_locations[resident_location_count++] = m_szGpuRegionLocations[layer];
			}
		}
		for ( int index = 0; index < resident_location_count; ++index ) {
			bHeightChanged = bHeightChanged || eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX;
			if ( republishGpuRegionLocation( resident_locations[index] ) ) {
				bPhysicalMappingChanged = true;
			}
		}
	}

	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		calcHeightRange();
	}
	if ( bRebuild || bTopologyChanged ) {
		const Vector2i center = m_gpuRegionCacheCenter == V2I_MAX ? V2I_ZERO : m_gpuRegionCacheCenter;
		const real_t fWorldRegionSize = ( real_t ) ( m_nRegionSize ) * m_fVertexSpacing;
		const Vector3 centerPosition(
			( ( real_t ) ( center.x ) + 0.5f ) * fWorldRegionSize, 0.0f,
			( ( real_t ) ( center.y ) + 0.5f ) * fWorldRegionSize
		);
		// Initial population obeys the same upload ceiling as ordinary traversal.
		// Rebuilding the physical arrays must not synchronously refill the cache.
		updateGpuRegionResidency( centerPosition, false );
	}

	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}
	if ( !m_bStartupBulkLoad ) {
		if ( bHeightChanged || bRebuild ) {
			emit_signal( "height_maps_changed" );
		}
		if ( bControlChanged || bRebuild || bImplicitStateChanged ) {
			emit_signal( "control_maps_changed" );
		}
		if ( bColorChanged || bRebuild || bImplicitStateChanged ) {
			emit_signal( "color_maps_changed" );
		}
		if ( bRebuild || bImplicitStateChanged ) {
			emit_signal( "maps_changed" );
		}
	}
}

void T3DData::emitRegionLayoutChanged( const bool bTextureLayersChanged ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	LOG( T3DTerrain::DEBUG, "Emitting region_map_changed" );
	emit_signal( "region_map_changed" );
	if ( bTextureLayersChanged ) {
		LOG( T3DTerrain::DEBUG, "Emitting height_maps_changed" );
		emit_signal( "height_maps_changed" );
		LOG( T3DTerrain::DEBUG, "Emitting control_maps_changed" );
		emit_signal( "control_maps_changed" );
		LOG( T3DTerrain::DEBUG, "Emitting color_maps_changed" );
		emit_signal( "color_maps_changed" );
	}
	LOG( T3DTerrain::DEBUG, "Emitting maps_changed" );
	emit_signal( "maps_changed" );
	m_pTerrain->snap();
}

TypedArray<Vector2i> T3DData::getRegionLocations( void ) const {
	TypedArray<Vector2i> active_locations;
	active_locations.resize( m_nActiveRegionCount );
	int write_index = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const T3DRegion *pRegion = getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() ) {
			active_locations[write_index++] = location;
		}
	}
	if ( write_index != active_locations.size() ) {
		active_locations.resize( write_index );
	}
	return active_locations;
}

TypedArray<Image> T3DData::getActiveMaps( const t3dmapType_t eMapType ) const {
	TypedArray<Image> maps;
	if ( m_bCpuRegionCacheEnabled ) {
		LOG( T3DTerrain::ERROR, "Whole-world map arrays are unavailable while bounded CPU residency is enabled. "
			"Page one region with ensure_cpu_region_resident() and read that Terrain3DRegion, or disable the CPU cache." );
		return maps;
	}
	maps.resize( m_nActiveRegionCount );
	int write_index = 0;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const T3DRegion *pRegion = getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() ) {
			maps[write_index++] = getRegionMapOrDefault( pRegion, eMapType );
		}
	}
	if ( write_index != maps.size() ) {
		maps.resize( write_index );
	}
	return maps;
}

TypedArray<Image> T3DData::getHeightMaps( void ) const {
	return getActiveMaps( T3DRegion::TYPE_HEIGHT );
}

TypedArray<Image> T3DData::getControlMaps( void ) const {
	return getActiveMaps( T3DRegion::TYPE_CONTROL );
}

TypedArray<Image> T3DData::getColorMaps( void ) const {
	return getActiveMaps( T3DRegion::TYPE_COLOR );
}

// Structured to work with do_for_regions. Should be renamed when copy_paste is expanded.
void T3DData::copyPasteDfr(
	const T3DRegion *pSrcRegion, const Rect2i &rSrcRect,
	const Rect2i &rDstRect, const T3DRegion *pDstRegion
) {
	if ( !pSrcRegion || !pDstRegion ) {
		return;
	}
	for ( int i = 0; i < T3DRegion::TYPE_MAX; i++ ) {
		const t3dmapType_t map_type = ( t3dmapType_t ) ( i );
		Image *const pSrcImage = pSrcRegion->getMapPtr( map_type );
		Image *pDstImage = pDstRegion->getMapPtr( map_type );
		if ( pSrcImage ) {
			pDstImage = const_cast<T3DRegion *>( pDstRegion )->getMapPtrForWrite( map_type );
			if ( pDstImage ) {
				pDstImage->blit_rect( pSrcImage, rSrcRect, rDstRect.position );
			}
		} else if ( pDstImage && map_type != T3DRegion::TYPE_HEIGHT ) {
			// Copying an implicit source channel must clear painted destination data
			// back to the channel default rather than silently leaving it unchanged.
			pDstImage->fill_rect( rDstRect, TERRAIN3D_REGION_MAP_DEFAULT_COLORS[map_type] );
		}
	}
	m_pTerrain->getInstancer()->copyPasteDfr( pSrcRegion, rSrcRect, pDstRegion );
}

///////////////////////////
// Public Functions
///////////////////////////

T3DData::T3DData( void ) {
	for ( std::atomic<uint32_t> &generation : m_szRegionSamplingGenerations ) {
		generation.store( 1u, std::memory_order_relaxed );
	}
	std::fill_n( m_szGpuRegionLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
}

T3DData::~T3DData( void ) {
	clear();
	// The far-field atlas is intentionally retained across normal data reloads,
	// but destruction must release its RenderingServer RIDs and retained Images.
	m_generatedFarHeightMap.clear();
	m_generatedFarColorMap.clear();
	if ( m_bOwnsActiveStorage ) {
		terrain3dActiveStorageRelease( this );
		m_bOwnsActiveStorage = false;
	}
	if ( m_pResidentPool ) {
		memdelete( m_pResidentPool );
		m_pResidentPool = nullptr;
	}
	if ( m_pCompute ) {
		memdelete( m_pCompute );
		m_pCompute = nullptr;
	}
}

T3DCompute *T3DData::getComputeBackend( void ) {
	if ( !m_pCompute ) {
		m_pCompute = memnew( T3DCompute );
	}
	return m_pCompute;
}

T3DResidentMapLease T3DData::acquireResidentMap(
	const Vector2i &vLocation,
	const T3DRegion *pRegion
) const {
	if ( m_bOwnsActiveStorage || !m_pResidentPool || !pRegion || pRegion->isDeleted() ) {
		return {};
	}
	return m_pResidentPool->acquire( vLocation, pRegion );
}

size_t T3DData::getResidentPoolReservedBytes( void ) const {
	return m_pResidentPool ? m_pResidentPool->getReservedBytes() : 0u;
}

void T3DData::configureResidentPool( const int nCapacity ) {
	if ( m_bOwnsActiveStorage ) {
		if ( m_pResidentPool ) {
			memdelete( m_pResidentPool );
			m_pResidentPool = nullptr;
		}
		return;
	}
	if ( !m_pResidentPool ) {
		m_pResidentPool = memnew( T3DResidentRegionPool );
	}
	if ( m_nRegionSize <= 0 ) {
		return;
	}
	// Runtime CPU queries overwhelmingly consume height and occasionally packed
	// control. Derived normals stay GPU-resident and are reconstructed from the
	// height plane for non-editor CPU queries, avoiding an otherwise mostly idle
	// 2 MiB RG8 plane in the standardized 64 x 128-sample resident window.
	uint32_t channels = T3DResidentRegionPool::CHANNEL_HEIGHT;
	if ( !m_bControlMapsImplicit ) {
		channels |= T3DResidentRegionPool::CHANNEL_CONTROL;
	}
	const size_t bytes_per_region = T3DResidentRegionPool::estimateSlotBytes( m_nRegionSize, channels );
	const int budget_capacity =
		bytes_per_region > 0u ? ( int ) ( MAX<size_t>( 1u, TERRAIN3D_DEFAULT_RESIDENT_POOL_BUDGET / bytes_per_region ) ) : 1;
	const int capacity = CLAMP( MIN( nCapacity, budget_capacity ), 1, m_nRegionMapSize * m_nRegionMapSize );
	const int old_region_size = m_pResidentPool->getRegionSize();
	const int old_capacity = m_pResidentPool->getCapacity();
	const uint32_t old_channels = m_pResidentPool->getChannels();
	if ( !m_pResidentPool->configure( m_nRegionSize, capacity, channels ) ) {
		LOG( T3DTerrain::WARN, "Unable to configure experimental resident region map pool" );
	} else if ( old_region_size != m_nRegionSize || old_capacity != capacity || old_channels != channels ) {
		invalidateSamplingCache();
	}
}

void T3DData::configureGpuRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nUploadLayers, const int nUploadBudgetMb, const int nSparePages,
	const int nRetireFrames
) {
	const bool bOldEnabled = m_bGpuRegionCacheEnabled;
	const int old_budget = m_nGpuRegionCacheBudgetMb;
	const int old_upload_layers = m_nGpuRegionUploadBudgetLayers;
	const int old_upload_budget = m_nGpuRegionUploadBudgetMb;
	const int old_spare_pages = m_nGpuRegionSparePages;
	const int old_retire_frames = m_nGpuRegionRetireFrames;
	m_bGpuRegionCacheEnabled = bEnabled;
	m_nGpuRegionCacheBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_GPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_GPU_REGION_CACHE_MAX_BUDGET_MB );
	m_nGpuRegionUploadBudgetLayers = CLAMP( nUploadLayers, 1, TERRAIN3D_GPU_REGION_UPLOAD_MAX_LAYERS );
	m_nGpuRegionUploadBudgetMb = CLAMP( nUploadBudgetMb, 1, TERRAIN3D_GPU_REGION_UPLOAD_MAX_BUDGET_MB );
	m_nGpuRegionSparePages = CLAMP( nSparePages, TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS,
		TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS - 1 );
	m_nGpuRegionRetireFrames = CLAMP( nRetireFrames, 1, 16 );

	const int requested_pages = calculateGpuRegionRequestedPages();
	const int required_radius = calculateGpuRegionRequiredRadius();
	const int color_divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	const int derived_divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const int required_capacity = m_bGpuRegionCacheEnabled ? calculateGpuRegionCacheCapacity() : 0;
	const int required_physical_layers = m_bGpuRegionCacheEnabled ?
		calculateGpuRegionPhysicalLayers( required_capacity ) : 0;
	const bool color_channel_enabled = isGpuRegionColorChannelEnabled();
	const bool derived_channel_enabled = isGpuRegionDerivedChannelEnabled();
	const bool bLayoutChanged = m_bGpuRegionCacheEnabled && m_bGpuRegionCacheTexturesReady &&
		( required_capacity != m_nGpuRegionCacheCapacity || color_divisor != m_nGpuRegionColorDivisor ||
			derived_divisor != m_nGpuRegionDerivedDivisor ||
			color_channel_enabled != m_bGpuRegionColorChannelAllocated ||
			derived_channel_enabled != m_bGpuRegionDerivedChannelAllocated ||
			required_physical_layers != m_nGpuRegionPhysicalLayerCount );
	const bool bSelectionChanged = requested_pages != m_nGpuRegionRequestedPages ||
		required_radius != m_nGpuRegionRequiredRadius;
	const bool bStorageChanged = bOldEnabled != m_bGpuRegionCacheEnabled ||
		( m_bGpuRegionCacheEnabled && old_budget != m_nGpuRegionCacheBudgetMb ) || bLayoutChanged;
	if ( !bStorageChanged && !bSelectionChanged && old_upload_layers == m_nGpuRegionUploadBudgetLayers &&
		old_upload_budget == m_nGpuRegionUploadBudgetMb && old_spare_pages == m_nGpuRegionSparePages &&
		old_retire_frames == m_nGpuRegionRetireFrames ) {
		return;
	}
	m_nGpuRegionRequestedPages = requested_pages;
	m_nGpuRegionRequiredRadius = required_radius;
	m_bGpuRegionCacheSelectionDirty = m_bGpuRegionCacheSelectionDirty || bSelectionChanged;
	if ( !bOldEnabled && !m_bGpuRegionCacheEnabled ) {
		return;
	}
	if ( bStorageChanged ) {
		resetGpuRegionCache( true );
		if ( m_pTerrain && m_nRegionSize > 0 ) {
			updateMaps( T3DRegion::TYPE_MAX, true, false );
		}
	}
}

void T3DData::updateGpuRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	const T3DLastScopeTimer serviceTimer( m_nGpuRegionServiceUsecLast,
		m_pTerrain && m_pTerrain->isRuntimeProfilingEnabled() );
	m_nGpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nGpuPrepareLatencyUsecLast = 0u;
	m_nGpuPrepareQueueWaitUsecLast = 0u;
	m_nGpuPrepareFileOpenUsecLast = 0u;
	m_nGpuPrepareFileReadUsecLast = 0u;
	m_nGpuPrepareValidationUsecLast = 0u;
	m_nGpuPrepareMaterializeUsecLast = 0u;
	m_nGpuPrepareWorkerUsecLast = 0u;
	m_nGpuPrepareReadyWaitUsecLast = 0u;
	m_nGpuPreparePublicationUsecLast = 0u;
	m_nGpuPrepareEndToEndUsecLast = 0u;
	pollGpuRuntimePagePrepare( false );
	m_nGpuRegionUploadsLastTick = 0;
	m_nGpuRegionEvictionsLastTick = 0;
	m_nGpuRegionCookedHitsLastTick = 0;
	m_nGpuRegionCookedMissesLastTick = 0;
	m_nGpuRegionRuntimePreparesLastTick = 0;
	m_nGpuRegionReusedLayersLastTick = 0;
	m_nGpuRegionPageTableUpdatesLastTick = 0;
	m_nGpuRegionHeightUpdatesLastTick = 0;
	m_nGpuRegionControlUpdatesLastTick = 0;
	m_nGpuRegionColorUpdatesLastTick = 0;
	m_nGpuRegionDerivedUpdatesLastTick = 0;
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	m_nGpuRegionPreparedPagesLastTick = m_nGpuPreparedPageCount;
	m_nGpuRegionPrepareWorkersLastTick = m_bGpuPrepareJobActive ? m_nGpuPrepareJobWorkers : 0;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuFarHeightUpdatesLastTick = 0;
	m_nGpuFarColorUpdatesLastTick = 0;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	if ( !m_bGpuRegionCacheEnabled || m_nRegionSize <= 0 || m_fVertexSpacing <= 0.0f ) {
		return;
	}
	++m_nGpuRegionTick;
	reclaimRetiredGpuRegionSlots();
	const uint64_t streaming_generation = m_pTerrain ? m_pTerrain->getRegionStreamingGeneration() : 0u;
	if ( m_pTerrain ) {
		const Vector2i streaming_center = m_pTerrain->getRegionStreamingCenterInternal();
		const Vector2i predicted_center = m_pTerrain->getRegionStreamingPredictedCenterInternal();
		const Vector2i prediction_delta = predicted_center - streaming_center;
		m_gpuRegionPredictionDirection = Vector2i( SIGN( prediction_delta.x ), SIGN( prediction_delta.y ) );
	}
	if ( !m_bGpuRegionCacheTexturesReady && !rebuildGpuRegionCacheTextures() ) {
		return;
	}
	const T3DEditor *pTerrainEditor = m_pTerrain ? m_pTerrain->getEditor() : nullptr;
	const bool bDeferFarAtlasPublish = m_pTerrain && m_pTerrain->isEditorAuthoringMode() &&
		pTerrainEditor && pTerrainEditor->isOperating();
	// Brush motion can commit several stamps per frame. Keep the resident page
	// updates immediate, but publish the full far atlases once after the stroke.
	if ( !bDeferFarAtlasPublish ) {
		if ( m_bGpuFarHeightDirty && !rebuildGpuFarHeightAtlas() ) {
			return;
		}
		if ( m_bGpuFarColorDirty && !rebuildGpuFarColorAtlas() ) {
			return;
		}
	}

	const Vector2i raw_center = getRegionLocation( rWorldPosition );
	const t3dgpuCacheCoord_t clamped_center = terrain3dGpuCacheClampCenter(
		{ raw_center.x, raw_center.y }, m_nRegionMapSize );
	const Vector2i center( clamped_center.x, clamped_center.y );
	m_bGpuRegionCameraMovedLastTick = m_gpuRegionCacheCenter != V2I_MAX &&
		center != m_gpuRegionCacheCenter;
	const int runtime_upload_budget = getEffectiveGpuRegionUploadBudgetLayers();
	const int effective_upload_limit = MAX( 1, runtime_upload_budget );
	m_nGpuRegionEffectiveUploadLimitLastTick = effective_upload_limit;
	const int deferred_republish_count = processPendingGpuRegionRepublishes( effective_upload_limit );
	if ( deferred_republish_count > 0 ) {
		m_nGpuRegionUploadsLastTick += deferred_republish_count;
		m_nGpuRegionUploadsSinceReset += ( uint64_t ) ( deferred_republish_count );
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	if ( !bForce && center == m_gpuRegionCacheCenter && !m_bGpuRegionCacheSelectionDirty &&
		streaming_generation == m_nGpuRegionStreamingGenerationLast ) {
		if ( deferred_republish_count > 0 ) {
			emitGpuRegionMapChanged();
		}
		return;
	}

	Vector2i desired[TERRAIN3D_GPU_CACHE_MAX_LAYERS];
	t3dgpuCacheSelectionStats_t selection_stats;
	int desired_count = collectDesiredGpuRegions( center, desired, m_nGpuRegionCacheCapacity, selection_stats );
	const int footprint_count = desired_count;
	m_nGpuRegionRequiredLastTick = selection_stats.required;
	m_nGpuRegionGuardLastTick = selection_stats.guard;
	m_nGpuRegionPrefetchLastTick = selection_stats.prefetch;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
		const int layer = findGpuRegionSlot( desired[index] );
		if ( layer >= 0 ) {
			m_szGpuRegionLastUseTick[layer] = m_nGpuRegionTick;
			if ( index < selection_stats.required && m_szGpuRegionPrefetchOrigin[layer] != 0u ) {
				++m_nGpuRegionPrefetchUsedTotal;
				m_szGpuRegionPrefetchOrigin[layer] = 0u;
			}
		}
	}

	const uint64_t hysteresis_ticks = ( uint64_t ) ( m_pTerrain ? m_pTerrain->getGpuRegionHysteresisTicks() :
		TERRAIN3D_GPU_CACHE_DEFAULT_HYSTERESIS_TICKS );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && desired_count < m_nGpuRegionCacheCapacity; ++layer ) {
		const Vector2i location = m_szGpuRegionLocations[layer];
		if ( location == V2I_MAX || m_szGpuRegionLastUseTick[layer] == 0u ||
			m_nGpuRegionTick - m_szGpuRegionLastUseTick[layer] > hysteresis_ticks ) {
			continue;
		}
		const int map_index = getRegionMapIndex( location );
		if ( map_index < 0 || desired_map[map_index] != 0u ) {
			continue;
		}
		desired[desired_count++] = location;
		desired_map[map_index] = 1u;
		++m_nGpuRegionHysteresisLastTick;
	}
	m_nGpuRegionDesiredLastTick = desired_count;
	m_nGpuRegionPriorityCount = MIN( desired_count, TERRAIN3D_GPU_CACHE_MAX_LAYERS );
	const int guard_end = selection_stats.required + selection_stats.guard;
	const int priority_prefetch_end = guard_end + selection_stats.prefetch;
	for ( int index = 0; index < m_nGpuRegionPriorityCount; ++index ) {
		m_szGpuRegionPriorityLocations[index] = desired[index];
		if ( index < selection_stats.required ) {
			m_szGpuRegionPriorityClasses[index] = 1u;
		} else if ( index < guard_end ) {
			m_szGpuRegionPriorityClasses[index] = 2u;
		} else if ( index < priority_prefetch_end ) {
			m_szGpuRegionPriorityClasses[index] = 3u;
		} else {
			m_szGpuRegionPriorityClasses[index] = 4u;
		}
	}
	for ( int index = m_nGpuRegionPriorityCount; index < TERRAIN3D_GPU_CACHE_MAX_LAYERS; ++index ) {
		m_szGpuRegionPriorityLocations[index] = V2I_MAX;
		m_szGpuRegionPriorityClasses[index] = 0u;
	}

	if ( m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) >= 0 ) {
				continue;
			}
			T3DRegion *const pRegion = getRegionPtr( desired[index] );
			if ( pRegion && !pRegion->isDeleted() && pRegion->runtimeMapsEvictable() ) {
				const uint8_t priority = index < m_nGpuRegionPriorityCount ?
					m_szGpuRegionPriorityClasses[index] : 4u;
				requestGpuRuntimePage( desired[index], RUNTIME_PAGE_CONSUMER_GPU_DETAIL, priority );
			}
		}
		serviceGpuRuntimePagePrepare();
	}

	bool bMapChanged = false;
	bool bAllDesiredResident = true;
	const size_t byte_budget = ( size_t ) ( m_nGpuRegionUploadBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	const int update_call_budget = MAX( 5, effective_upload_limit * 3 + 1 );
	for ( int desired_index = 0; desired_index < desired_count; ++desired_index ) {
		const Vector2i location = desired[desired_index];
		const int existing_layer = findGpuRegionSlot( location );
		if ( existing_layer >= 0 ) {
			m_szGpuRegionLastUseTick[existing_layer] = m_nGpuRegionTick;
			continue;
		}
		bAllDesiredResident = false;
		if ( m_nGpuRegionUploadsLastTick >= effective_upload_limit ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET;
			break;
		}

		const int target_layer = findFreeGpuRegionSlot();
		if ( target_layer < 0 ) {
			// Spare pages can be temporarily exhausted while old mappings remain
			// retired for in-flight rendering. Continue on a later frame rather than
			// synchronizing or overwriting a page still visible to the renderer.
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY;
			break;
		}
		T3DRegion *const pUploadRegion = getRegionPtr( location );
		if ( !pUploadRegion || pUploadRegion->isDeleted() ) {
			continue;
		}
		const size_t expected_bytes = estimateGpuRegionUploadBytes( pUploadRegion, target_layer );
		const int expected_calls = estimateGpuRegionUploadCalls( pUploadRegion, target_layer );
		const int page_table_reserve = bMapChanged ? 0 : 1;
		if ( m_nGpuRegionUploadsLastTick > 0 &&
			m_nGpuRegionUploadBytesLastTick + expected_bytes > byte_budget ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET;
			break;
		}
		if ( m_nGpuRegionUploadsLastTick > 0 &&
			getGpuRegionTextureUpdateCallsLastTick() + expected_calls + page_table_reserve >
				update_call_budget ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET;
			break;
		}

		int victim_layer = -1;
		if ( m_nGpuRegionResidentCount >= m_nGpuRegionCacheCapacity ) {
			uint64_t oldest_tick = std::numeric_limits<uint64_t>::max();
			int farthest_distance = -1;
			for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
				const Vector2i old_location = m_szGpuRegionLocations[layer];
				if ( old_location == V2I_MAX || m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
					continue;
				}
				const int old_map_index = getRegionMapIndex( old_location );
				if ( old_map_index >= 0 && desired_map[old_map_index] != 0u ) {
					continue;
				}
				const int distance = MAX( std::abs( old_location.x - center.x ), std::abs( old_location.y - center.y ) );
				const uint64_t last_use = m_szGpuRegionLastUseTick[layer];
				if ( last_use < oldest_tick || ( last_use == oldest_tick && distance > farthest_distance ) ) {
					oldest_tick = last_use;
					farthest_distance = distance;
					victim_layer = layer;
				}
			}
			if ( victim_layer < 0 ) {
				m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY;
				continue;
			}
		}

		// The staging layer is not referenced by the page table. All channel
		// updates therefore complete before any shader can sample the new page.
		if ( !uploadGpuRegionLayer( target_layer, location, T3DRegion::TYPE_MAX ) ) {
			continue;
		}

		if ( victim_layer >= 0 ) {
			const Vector2i old_location = m_szGpuRegionLocations[victim_layer];
			const int old_map_index = getRegionMapIndex( old_location );
			if ( old_map_index >= 0 ) {
				m_szGpuRegionMap[old_map_index] = 0u;
			}
			recordGpuRegionDeparture( victim_layer, true );
			retireGpuRegionSlot( victim_layer );
			++m_nGpuRegionEvictionsLastTick;
		} else {
			++m_nGpuRegionResidentCount;
		}
		m_szGpuRegionLocations[target_layer] = location;
		m_szGpuRegionLastUseTick[target_layer] = m_nGpuRegionTick;
		m_szGpuRegionLoadedTick[target_layer] = m_nGpuRegionTick;
		const int prefetch_begin = selection_stats.required + selection_stats.guard;
		const int prefetch_end = prefetch_begin + selection_stats.prefetch;
		m_szGpuRegionPrefetchOrigin[target_layer] = desired_index >= prefetch_begin &&
			desired_index < prefetch_end ? 1u : 0u;
		m_szGpuRegionSlotStates[target_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int new_map_index = getRegionMapIndex( location );
		if ( new_map_index >= 0 ) {
			m_szGpuRegionMap[new_map_index] = ( uint16_t ) ( target_layer + 1 );
		}
		++m_nGpuRegionUploadsLastTick;
		++m_nGpuRegionUploadsSinceReset;
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
		bMapChanged = true;
	}

	int resident_required = 0;
	const int written_required = MIN( selection_stats.required, footprint_count );
	for ( int index = 0; index < written_required; ++index ) {
		if ( findGpuRegionSlot( desired[index] ) >= 0 ) {
			++resident_required;
		}
	}
	m_nGpuRegionMissingRequiredLastTick = selection_stats.required - resident_required;
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) < 0 ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	m_gpuRegionCacheCenter = center;
	m_nGpuRegionStreamingGenerationLast = streaming_generation;
	m_bGpuRegionCacheSelectionDirty = !bAllDesiredResident || m_nGpuRegionMissingRequiredLastTick > 0 ||
		m_nGpuRegionRepublishPendingCount > 0;
	if ( bMapChanged || deferred_republish_count > 0 ) {
		emitGpuRegionMapChanged();
	}
}

void T3DData::invalidateGpuFarSurface( void ) {
	if ( !isFarAtlasRequired() ) {
		return;
	}
	// Surface quality may change the atlas dimensions. Recreate only the surface
	// representation; the Far height lattice is geometry-owned and stays intact.
	m_generatedFarColorMap.clear();
	m_pGpuFarColorImage.unref();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarColorDirty = true;
	m_bGpuFarColorDirtyAll = true;
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
	if ( !m_bGpuRegionCacheEnabled ) {
		ensureFarAtlases();
	}
}

void T3DData::invalidateGpuFarAtlasLayout( void ) {
	m_generatedFarHeightMap.clear();
	m_generatedFarColorMap.clear();
	m_pGpuFarHeightImage.unref();
	m_pGpuFarColorImage.unref();
	m_gpuFarHeightDirtyRegions.reset();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarHeightDirty = true;
	m_bGpuFarColorDirty = true;
	m_bGpuFarHeightDirtyAll = true;
	m_bGpuFarColorDirtyAll = true;
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
	if ( isFarAtlasRequired() && m_bOwnsActiveStorage && m_nRegionSize > 0 && m_nRegionMapSize > 0 ) {
		ensureFarAtlases();
	}
}

bool T3DData::ensureFarAtlases( void ) {
	if ( !isFarAtlasRequired() || !m_bOwnsActiveStorage || m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ) {
		return false;
	}
	if ( ( m_pGpuFarHeightImage.is_null() || m_pGpuFarColorImage.is_null() ) &&
		!loadCookedGpuFarAtlases() ) {
		markAllGpuFarRegionsDirty( T3DRegion::TYPE_MAX );
	}
	if ( m_bGpuFarHeightDirty || !m_generatedFarHeightMap.getRid().is_valid() ) {
		if ( !rebuildGpuFarHeightAtlas() ) {
			return false;
		}
	}
	if ( m_bGpuFarColorDirty || !m_generatedFarColorMap.getRid().is_valid() ) {
		if ( !rebuildGpuFarColorAtlas() ) {
			return false;
		}
	}
	const bool bReady = m_generatedFarHeightMap.getRid().is_valid() &&
		m_generatedFarColorMap.getRid().is_valid();
	if ( bReady && m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateFarAtlasTextures();
		}
	}
	return bReady;
}

Dictionary T3DData::getRuntimeResidencyGrid( const int nRadius ) const {
	Dictionary grid;
	const int radius = CLAMP( nRadius, 1, 15 );
	Vector2i center;
	Vector2i predicted_center;
	Dictionary streaming_stats;
	if ( m_pTerrain ) {
		streaming_stats = m_pTerrain->getRegionStreamingStats();
		center = streaming_stats["center"];
		predicted_center = streaming_stats["predicted_center"];
	}
	grid["center"] = center;
	grid["predicted_center"] = predicted_center;
	grid["radius"] = radius;
	grid["motion_direction"] = predicted_center - center;

	Array cells;
	const int diameter = radius * 2 + 1;
	cells.resize( diameter * diameter );
	int write_index = 0;
	for ( int z = center.y - radius; z <= center.y + radius; ++z ) {
		for ( int x = center.x - radius; x <= center.x + radius; ++x ) {
			const Vector2i location( x, z );
			Dictionary cell;
			cell["location"] = location;
			cell["offset"] = location - center;
			const int map_index = getRegionMapIndex( location );
			const bool bValid = isRegionLocationValidInternal( location );
			const bool bAuthored = bValid && hasRegion( location );
			const t3dregionStreamingState_t state = m_pTerrain && bValid ?
				m_pTerrain->getRegionStreamingStateInternal( location ) : TERRAIN3D_REGION_STREAMING_UNLOADED;
			const bool predicted = m_pTerrain && bValid && m_pTerrain->isRegionStreamingPrefetchInternal( location );
			cell["valid_region_coordinate"] = bValid;
			cell["active"] = bAuthored;
			cell["streaming_state"] = ( int ) ( state );
			cell["streaming_state_name"] = String( terrain3dStreamingStateName( state ) );
			cell["predicted"] = predicted;
			// Compatibility field for older profiler consumers: 1/2/3 remain the
			// high-to-low surface-page priority classes.
			cell["desired_class"] = state == TERRAIN3D_REGION_STREAMING_ACTIVE ? 1 :
				( state == TERRAIN3D_REGION_STREAMING_HOT ? 2 : ( predicted ? 3 : 0 ) );
			const int gpu_slot = findGpuRegionSlot( location );
			cell["gpu_resident"] = gpu_slot >= 0;
			cell["gpu_layer"] = gpu_slot;
			cell["middle_height_resident"] = findMiddleHeightSlot( location ) >= 0;
			cell["cpu_resident"] = findCpuRegionSlot( location ) >= 0;
			cell["instance_resident"] = findInstanceRegionSlot( location ) >= 0;
			bool bPrepared = false;
			for ( int prepared_index = 0; prepared_index < m_nGpuPreparedPageCount; ++prepared_index ) {
				if ( m_szGpuPreparedPages[( size_t ) ( prepared_index )].location == location ) {
					bPrepared = true;
					break;
				}
			}
			cell["prepared"] = bPrepared;
			cell["prepare_pending"] = isGpuRuntimePagePreparePending( location );
			int slot_state = TERRAIN3D_GPU_SLOT_FREE;
			for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
				if ( m_szGpuRegionLocations[layer] == location ) {
					slot_state = ( int ) ( m_szGpuRegionSlotStates[layer] );
					break;
				}
			}
			cell["gpu_slot_state"] = slot_state;
			cell["far_height_dirty"] = map_index >= 0 &&
				( m_bGpuFarHeightDirtyAll || m_gpuFarHeightDirtyRegions.test( ( size_t ) ( map_index ) ) );
			cell["far_color_dirty"] = map_index >= 0 &&
				( m_bGpuFarColorDirtyAll || m_gpuFarColorDirtyRegions.test( ( size_t ) ( map_index ) ) );
			cells[write_index++] = cell;
		}
	}
	grid["cells"] = cells;
	grid["active_count"] = streaming_stats.get( "active", 0 );
	grid["hot_count"] = streaming_stats.get( "hot", 0 );
	grid["cold_count"] = streaming_stats.get( "cold", 0 );
	grid["unloaded_count"] = streaming_stats.get( "unloaded", 0 );
	grid["predicted_count"] = streaming_stats.get( "predicted_promotions", 0 );
	grid["required_count"] = streaming_stats.get( "active", 0 );
	grid["guard_count"] = streaming_stats.get( "hot", 0 );
	grid["prefetch_count"] = streaming_stats.get( "predicted_promotions", 0 );
	return grid;
}

Dictionary T3DData::getGpuRegionCacheStatsFast( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["budget_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["allocation_budget_mb"] = m_pTerrain && m_pTerrain->isEditorAuthoringMode() ?
		calculateAuthoringGpuRegionBudgetMb() : m_nGpuRegionCacheBudgetMb;
	stats["capacity"] = m_nGpuRegionCacheCapacity;
	stats["physical_layers"] = m_nGpuRegionPhysicalLayerCount;
	stats["resident"] = m_nGpuRegionResidentCount;
	stats["republish_pending"] = m_nGpuRegionRepublishPendingCount;
	stats["requested_pages"] = m_nGpuRegionRequestedPages;
	stats["required_last_tick"] = m_nGpuRegionRequiredLastTick;
	stats["missing_required_last_tick"] = m_nGpuRegionMissingRequiredLastTick;
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t ) ( m_nGpuRegionUploadsSinceReset );
	stats["evictions_last_tick"] = m_nGpuRegionEvictionsLastTick;
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	const int cooked_attempts = m_nGpuRegionCookedHitsLastTick + m_nGpuRegionCookedMissesLastTick;
	stats["cooked_hit_ratio_last_tick"] = cooked_attempts > 0 ?
		( double ) ( m_nGpuRegionCookedHitsLastTick ) / ( double ) ( cooked_attempts ) : -1.0;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["prepare_job_active"] = m_bGpuPrepareJobActive;
	stats["prepare_job_pending_pages"] = m_bGpuPrepareJobActive ? m_nGpuPrepareWorkPageCount : 0;
	stats["prepare_ready_pages"] = m_nGpuPreparedPageCount;
	stats["prepare_publication_usec_last"] = ( int64_t ) ( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t ) ( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nGpuRegionServiceUsecLast );
	stats["far_height_dirty_regions"] = m_bGpuFarHeightDirtyAll ? m_nActiveRegionCount : ( int ) ( m_gpuFarHeightDirtyRegions.count() );
	stats["far_color_dirty_regions"] = m_bGpuFarColorDirtyAll ? m_nActiveRegionCount : ( int ) ( m_gpuFarColorDirtyRegions.count() );
	stats["far_height_processing_usec_last"] = ( int64_t ) ( m_nGpuFarHeightProcessingUsecLast );
	stats["far_color_processing_usec_last"] = ( int64_t ) ( m_nGpuFarColorProcessingUsecLast );
	stats["far_height_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarHeightDirtyTexelsLast );
	stats["far_color_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarColorDirtyTexelsLast );
	stats["far_height_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightRebuiltBytesLast );
	stats["far_color_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarColorRebuiltBytesLast );
	stats["far_height_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesLast );
	stats["far_color_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesLast );
	stats["far_height_publication_usec_last"] = ( int64_t ) ( m_nGpuFarHeightPublicationUsecLast );
	stats["far_color_publication_usec_last"] = ( int64_t ) ( m_nGpuFarColorPublicationUsecLast );
	stats["far_height_publication_count"] = ( int64_t ) ( m_nGpuFarHeightPublicationCount );
	stats["far_color_publication_count"] = ( int64_t ) ( m_nGpuFarColorPublicationCount );
	stats["far_height_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesTotal );
	stats["far_color_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesTotal );
	stats["far_partial_publication_available"] = false;
	stats["far_publication_path"] = "RenderingServer full Texture2D update";
	stats["upload_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionUploadBytesLastTick );
	stats["reserved_bytes"] = ( int64_t ) ( getGpuRegionCacheReservedBytes() );
	stats["authoring_bounded_pool_covers_all_regions"] = isAuthoringGpuFullResidencyPossible();
	stats["authoring_readback_safe"] = isAuthoringGpuReadbackSafe();
	return stats;
}

bool T3DData::isAuthoringGpuFullResidencyPossible( void ) const {
	return !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ||
		calculateGpuRegionCacheCapacity() >= m_nActiveRegionCount;
}

bool T3DData::isAuthoringGpuReadbackSafe( void ) const {
	if ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	if ( !m_bGpuRegionCacheEnabled ) {
		return true;
	}
	if ( !m_bGpuRegionCacheTexturesReady || m_bGpuPrepareJobActive ||
		m_bGpuRegionCacheSelectionDirty || m_nGpuRegionUploadsLastTick > 0 ) {
		return false;
	}
	return m_nGpuRegionTick >= m_nGpuRegionLastPublicationTick + 2u;
}

Dictionary T3DData::getStartupLoadStats( void ) const {
	Dictionary stats;
	stats["total_usec_last"] = ( int64_t ) ( m_nStartupLoadTotalUsecLast );
	stats["enumeration_usec_last"] = ( int64_t ) ( m_nStartupLoadEnumerationUsecLast );
	stats["deserialize_usec_last"] = ( int64_t ) ( m_nStartupLoadDeserializeUsecLast );
	stats["staging_usec_last"] = ( int64_t ) ( m_nStartupLoadStagingUsecLast );
	stats["publish_usec_last"] = ( int64_t ) ( m_nStartupLoadPublishUsecLast );
	stats["finalize_usec_last"] = ( int64_t ) ( m_nStartupLoadFinalizeUsecLast );
	stats["region_files_last"] = m_nStartupLoadRegionFilesLast;
	stats["regions_published_last"] = m_nStartupLoadRegionsPublishedLast;
	stats["succeeded_last"] = m_bStartupLoadSucceededLast;
	stats["format"] = "legacy_godot_resources";
	stats["world_manifest_used"] = false;
	stats["resource_deserialize_thread"] = "caller/main";
	return stats;
}

Dictionary T3DData::getGpuRegionCacheStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["budget_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["capacity"] = m_nGpuRegionCacheCapacity;
	stats["physical_layers"] = m_nGpuRegionPhysicalLayerCount;
	stats["spare_layers"] = m_nGpuRegionSpareLayerCount;
	stats["retired_layers"] = m_nGpuRegionRetiredCount;
	stats["retire_frames"] = m_nGpuRegionRetireFrames;
	stats["resident"] = m_nGpuRegionResidentCount;
	stats["republish_pending"] = m_nGpuRegionRepublishPendingCount;
	stats["requested_pages"] = m_nGpuRegionRequestedPages;
	stats["required_radius_regions"] = m_nGpuRegionRequiredRadius;
	stats["desired_last_tick"] = m_nGpuRegionDesiredLastTick;
	stats["required_last_tick"] = m_nGpuRegionRequiredLastTick;
	stats["guard_last_tick"] = m_nGpuRegionGuardLastTick;
	stats["prefetch_last_tick"] = m_nGpuRegionPrefetchLastTick;
	stats["hysteresis_last_tick"] = m_nGpuRegionHysteresisLastTick;
	stats["missing_required_last_tick"] = m_nGpuRegionMissingRequiredLastTick;
	const int required_resident = MAX( 0, m_nGpuRegionRequiredLastTick - m_nGpuRegionMissingRequiredLastTick );
	stats["required_resident_last_tick"] = required_resident;
	stats["required_residency_ratio"] = m_nGpuRegionRequiredLastTick > 0 ?
		( double ) ( required_resident ) / ( double ) ( m_nGpuRegionRequiredLastTick ) : 1.0;
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nGpuRegionStallFlagsLastTick );
	stats["prediction_direction"] = m_gpuRegionPredictionDirection;
	stats["streaming_generation_last"] = ( int64_t ) ( m_nGpuRegionStreamingGenerationLast );
	stats["upload_layer_budget"] = m_nGpuRegionUploadBudgetLayers;
	stats["runtime_upload_layer_budget"] = getEffectiveGpuRegionUploadBudgetLayers();
	stats["upload_budget_mb"] = m_nGpuRegionUploadBudgetMb;
	stats["upload_update_budget"] = MAX( 5, getEffectiveGpuRegionUploadBudgetLayers() * 3 + 1 );
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t ) ( m_nGpuRegionUploadsSinceReset );
	stats["evictions_last_tick"] = m_nGpuRegionEvictionsLastTick;
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	const int cooked_attempts = m_nGpuRegionCookedHitsLastTick + m_nGpuRegionCookedMissesLastTick;
	stats["cooked_hit_ratio_last_tick"] = cooked_attempts > 0 ?
		( double ) ( m_nGpuRegionCookedHitsLastTick ) / ( double ) ( cooked_attempts ) : -1.0;
	stats["prefetch_used_total"] = ( int64_t ) ( m_nGpuRegionPrefetchUsedTotal );
	stats["prefetch_discarded_total"] = ( int64_t ) ( m_nGpuRegionPrefetchDiscardedTotal );
	const uint64_t completed_prefetches = m_nGpuRegionPrefetchUsedTotal + m_nGpuRegionPrefetchDiscardedTotal;
	stats["prefetch_accuracy"] = completed_prefetches > 0u ?
		( double ) ( m_nGpuRegionPrefetchUsedTotal ) / ( double ) ( completed_prefetches ) : -1.0;
	int pending_prefetches = 0;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		pending_prefetches += m_szGpuRegionPrefetchOrigin[layer] != 0u ? 1 : 0;
	}
	stats["prefetch_pending"] = pending_prefetches;
	Array pending_requests;
	const int pending_request_limit = 16;
	int pending_request_count = 0;
	for ( int index = 0; index < m_nGpuRegionPriorityCount; ++index ) {
		const Vector2i location = m_szGpuRegionPriorityLocations[index];
		if ( location == V2I_MAX || findGpuRegionSlot( location ) >= 0 ) {
			continue;
		}
		++pending_request_count;
		if ( pending_requests.size() >= pending_request_limit ) {
			continue;
		}
		Dictionary request;
		request["priority_rank"] = index;
		request["location"] = location;
		request["offset"] = location - m_gpuRegionCacheCenter;
		request["distance_regions"] = MAX( std::abs( location.x - m_gpuRegionCacheCenter.x ),
			std::abs( location.y - m_gpuRegionCacheCenter.y ) );
		request["prediction_alignment"] = ( location.x - m_gpuRegionCacheCenter.x ) * m_gpuRegionPredictionDirection.x +
			( location.y - m_gpuRegionCacheCenter.y ) * m_gpuRegionPredictionDirection.y;
		const uint8_t request_class = m_szGpuRegionPriorityClasses[index];
		request["class_id"] = ( int ) ( request_class );
		request["class"] = request_class == 1u ? "required" :
			request_class == 2u ? "guard" : request_class == 3u ? "prefetch" : "hysteresis";
		bool bPrepared = false;
		for ( int prepared_index = 0; prepared_index < m_nGpuPreparedPageCount; ++prepared_index ) {
			if ( m_szGpuPreparedPages[( size_t ) ( prepared_index )].location == location ) {
				bPrepared = true;
				break;
			}
		}
		request["prepared"] = bPrepared;
		request["prepare_pending"] = isGpuRuntimePagePreparePending( location );
		const T3DRegion *pRegion = getRegionPtr( location );
		request["active"] = pRegion && !pRegion->isDeleted();
		request["runtime_maps_evictable"] = pRegion && pRegion->runtimeMapsEvictable();
		pending_requests.push_back( request );
	}
	stats["pending_requests"] = pending_requests;
	stats["pending_requests_reported"] = pending_requests.size();
	stats["pending_requests_total"] = pending_request_count;
	stats["departed_pages_total"] = ( int64_t ) ( m_nGpuRegionDepartedPagesTotal );
	stats["average_resident_ticks"] = m_nGpuRegionDepartedPagesTotal > 0u ?
		( double ) ( m_nGpuRegionResidentTicksTotal ) / ( double ) ( m_nGpuRegionDepartedPagesTotal ) : 0.0;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["reused_layers_last_tick"] = m_nGpuRegionReusedLayersLastTick;
	stats["page_table_updates_last_tick"] = m_nGpuRegionPageTableUpdatesLastTick;
	stats["height_updates_last_tick"] = m_nGpuRegionHeightUpdatesLastTick;
	stats["control_updates_last_tick"] = m_nGpuRegionControlUpdatesLastTick;
	stats["color_updates_last_tick"] = m_nGpuRegionColorUpdatesLastTick;
	stats["derived_updates_last_tick"] = m_nGpuRegionDerivedUpdatesLastTick;
	stats["runtime_color_mips_last_tick"] = m_nGpuRegionRuntimeColorMipsLastTick;
	stats["runtime_derived_builds_last_tick"] = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats["prepared_pages_last_tick"] = m_nGpuRegionPreparedPagesLastTick;
	stats["prepare_workers_last_tick"] = m_nGpuRegionPrepareWorkersLastTick;
	stats["prepare_job_active"] = m_bGpuPrepareJobActive;
	stats["prepare_job_pending_pages"] = m_bGpuPrepareJobActive ? m_nGpuPrepareWorkPageCount : 0;
	stats["prepare_ready_pages"] = m_nGpuPreparedPageCount;
	stats["prepare_latency_usec_last"] = ( int64_t ) ( m_nGpuPrepareLatencyUsecLast );
	stats["prepare_queue_wait_usec_last"] = ( int64_t ) ( m_nGpuPrepareQueueWaitUsecLast );
	stats["prepare_file_open_usec_last"] = ( int64_t ) ( m_nGpuPrepareFileOpenUsecLast );
	stats["prepare_file_read_usec_last"] = ( int64_t ) ( m_nGpuPrepareFileReadUsecLast );
	stats["prepare_validation_usec_last"] = ( int64_t ) ( m_nGpuPrepareValidationUsecLast );
	stats["prepare_materialize_usec_last"] = ( int64_t ) ( m_nGpuPrepareMaterializeUsecLast );
	stats["prepare_worker_usec_last"] = ( int64_t ) ( m_nGpuPrepareWorkerUsecLast );
	stats["prepare_ready_wait_usec_last"] = ( int64_t ) ( m_nGpuPrepareReadyWaitUsecLast );
	stats["prepare_publication_usec_last"] = ( int64_t ) ( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t ) ( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nGpuRegionServiceUsecLast );
	stats["effective_upload_limit_last_tick"] = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats["camera_moved_last_tick"] = m_bGpuRegionCameraMovedLastTick;
	stats["far_height_updates_last_tick"] = m_nGpuFarHeightUpdatesLastTick;
	stats["far_color_updates_last_tick"] = m_nGpuFarColorUpdatesLastTick;
	stats["far_height_regions_processed_last_tick"] = m_nGpuFarHeightRegionsLastTick;
	stats["far_color_regions_processed_last_tick"] = m_nGpuFarColorRegionsLastTick;
	stats["far_height_dirty_regions"] = m_bGpuFarHeightDirtyAll ? m_nActiveRegionCount :
		( int ) ( m_gpuFarHeightDirtyRegions.count() );
	stats["far_color_dirty_regions"] = m_bGpuFarColorDirtyAll ? m_nActiveRegionCount :
		( int ) ( m_gpuFarColorDirtyRegions.count() );
	stats["far_height_processing_usec_last"] = ( int64_t ) ( m_nGpuFarHeightProcessingUsecLast );
	stats["far_color_processing_usec_last"] = ( int64_t ) ( m_nGpuFarColorProcessingUsecLast );
	stats["far_height_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarHeightDirtyTexelsLast );
	stats["far_color_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarColorDirtyTexelsLast );
	stats["far_height_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightRebuiltBytesLast );
	stats["far_color_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarColorRebuiltBytesLast );
	stats["far_height_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesLast );
	stats["far_color_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesLast );
	stats["far_height_publication_usec_last"] = ( int64_t ) ( m_nGpuFarHeightPublicationUsecLast );
	stats["far_color_publication_usec_last"] = ( int64_t ) ( m_nGpuFarColorPublicationUsecLast );
	stats["far_height_publication_count"] = ( int64_t ) ( m_nGpuFarHeightPublicationCount );
	stats["far_color_publication_count"] = ( int64_t ) ( m_nGpuFarColorPublicationCount );
	stats["far_height_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesTotal );
	stats["far_color_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesTotal );
	stats["far_partial_publication_available"] = false;
	stats["far_publication_path"] = "RenderingServer full Texture2D update";
	stats["far_cook_io_usec_last"] = ( int64_t ) ( m_nGpuFarCookIoUsecLast );
	stats["far_cooked_loaded"] = m_bGpuFarCookedLoaded;
	stats["far_cooked_saved"] = m_bGpuFarCookedSaved;
	stats["far_height_bytes"] = ( int64_t ) ( getGpuFarHeightBytes() );
	stats["far_color_bytes"] = ( int64_t ) ( getGpuFarColorBytes() );
	stats["far_quality_divisor"] = m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1;
	stats["far_height_atlas_side"] = getGpuFarHeightAtlasSide();
	stats["far_surface_atlas_side"] = getGpuFarSurfaceAtlasSide();
	stats["texture_update_calls_last_tick"] = getGpuRegionTextureUpdateCallsLastTick();
	stats["upload_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionUploadBytesLastTick );
	stats["height_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionHeightBytesLastTick );
	stats["control_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionControlBytesLastTick );
	stats["color_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionColorBytesLastTick );
	stats["derived_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionDerivedBytesLastTick );
	const size_t layer_bytes = estimateGpuRegionLayerBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t worst_case_bytes = shared_bytes + layer_bytes * ( size_t ) ( MAX( 0, m_nGpuRegionPhysicalLayerCount ) );
	const int allocation_budget_mb = m_pTerrain && m_pTerrain->isEditorAuthoringMode() ?
		calculateAuthoringGpuRegionBudgetMb() : m_nGpuRegionCacheBudgetMb;
	const size_t budget_bytes = ( size_t ) ( allocation_budget_mb ) * TERRAIN3D_GPU_CACHE_MIB;
	stats["estimated_layer_bytes"] = ( int64_t ) ( layer_bytes );
	stats["estimated_shared_bytes"] = ( int64_t ) ( shared_bytes );
	stats["estimated_worst_case_bytes"] = ( int64_t ) ( worst_case_bytes );
	stats["allocation_budget_mb"] = allocation_budget_mb;
	stats["budget_overcommitted"] = worst_case_bytes > budget_bytes;
	stats["authoring_bounded_pool_covers_all_regions"] = isAuthoringGpuFullResidencyPossible();
	stats["authoring_readback_safe"] = isAuthoringGpuReadbackSafe();
	stats["control_implicit"] = m_bControlMapsImplicit;
	stats["color_implicit"] = m_bColorMapsImplicit;
	stats["color_resolution_divisor"] = m_nGpuRegionColorDivisor;
	stats["derived_resolution_divisor"] = m_nGpuRegionDerivedDivisor;
	stats["cooked_pages_enabled"] = m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled();
	stats["cook_on_save"] = m_pTerrain && m_pTerrain->isGpuRegionCookOnSave();
	stats["center"] = m_gpuRegionCacheCenter;
	stats["selection_pending"] = m_bGpuRegionCacheSelectionDirty;
	return stats;
}

size_t T3DData::getGpuRegionCacheReservedBytes( void ) const {
	const size_t fixed_far_bytes = getGpuFarHeightBytes() + getGpuFarColorBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	return ( shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u ) +
		estimateGpuRegionLayerBytes() * ( size_t ) ( MAX( 0, m_nGpuRegionPhysicalLayerCount ) );
}

size_t T3DData::getGpuRegionCacheMinimumBytes( void ) const {
	const size_t fixed_far_bytes = getGpuFarHeightBytes() + getGpuFarColorBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateGpuRegionLayerBytes() *
		( size_t ) ( TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS + 1 );
}

size_t T3DData::getGpuRegionCacheHardCapBytes( void ) const {
	const size_t fixed_far_bytes = getGpuFarHeightBytes() + getGpuFarColorBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateGpuRegionLayerBytes() * ( size_t ) ( TERRAIN3D_GPU_CACHE_MAX_LAYERS );
}

size_t T3DData::getCpuRegionCacheReservedBytes( void ) const {
	return estimateCpuRegionLayerBytes() * ( size_t ) ( MAX( 0, m_nCpuRegionCacheCapacity ) );
}

size_t T3DData::getCpuRegionCacheMinimumBytes( void ) const {
	return estimateCpuRegionLayerBytes() * ( size_t ) ( TERRAIN3D_CPU_CACHE_MIN_SAMPLE_LAYERS );
}

size_t T3DData::getCpuRegionCacheHardCapBytes( void ) const {
	return estimateCpuRegionLayerBytes() * ( size_t ) ( TERRAIN3D_CPU_CACHE_MAX_LAYERS );
}

size_t T3DData::getMemoryTransientStagingBytes( void ) const {
	const size_t prepared_page_bytes = estimateCpuRegionLayerBytes();
	size_t bytes = prepared_page_bytes * ( size_t ) ( MAX( 0, m_nGpuPreparedPageCount + m_nGpuPrepareWorkPageCount ) );
	bytes += m_fDerivedHaloScratch.capacity() * sizeof( float );
	bytes += m_activeLocationScratch.capacity() * sizeof( Vector2i );
	bytes += m_derivedLocationScratch.capacity() * sizeof( Vector2i );
	if ( m_pGpuFarHeightImage.is_valid() ) {
		bytes += getGpuFarHeightBytes();
	}
	if ( m_pGpuFarColorImage.is_valid() ) {
		bytes += getGpuFarColorBytes();
	}
	return bytes;
}

size_t T3DData::getMemoryFixedRepositoryBytes( void ) const {
	return sizeof( *this ) + getActiveStorageReservedBytes();
}

Error T3DData::initialize( T3DTerrain *const pTerrain ) {
	if ( !pTerrain ) {
		LOG( T3DTerrain::ERROR, "Initialization failed, p_terrain is null" );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_bRuntimeActive && m_pTerrain == pTerrain && m_bOwnsActiveStorage && terrain3dActiveStorageIsOwner( this ) ) {
		const uint32_t current_generation = terrain3dActiveStorageGetOwnerGeneration( this );
		if ( current_generation != 0u && current_generation != m_nActiveStorageGeneration ) {
			m_nActiveStorageGeneration = current_generation;
			resetMiddleHeightCache();
			resetGpuRegionCache( true );
		}
		return OK;
	}
	LOG( T3DTerrain::INFO, "Initializing data runtime" );
	m_pTerrain = pTerrain;
	m_fVertexSpacing = m_pTerrain->getVertexSpacing();

	const uint32_t previous_generation = m_nActiveStorageGeneration;
	if ( !setRegionSizeInternal( m_pTerrain->getRegionSize() ) ) {
		m_bRuntimeActive = false;
		return ERR_ALREADY_IN_USE;
	}
	m_nActiveStorageGeneration = terrain3dActiveStorageGetOwnerGeneration( this );
	if ( m_nActiveStorageGeneration == 0u ) {
		deactivateRuntimeStorage( pTerrain );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if ( previous_generation != m_nActiveStorageGeneration ) {
		resetMiddleHeightCache();
		resetGpuRegionCache( true );
	}

	ensureDefaultMaps();
	const bool bLoadInitialDirectory = !m_bRepositoryLoaded && !m_pTerrain->getDataDirectory().is_empty();
	if ( bLoadInitialDirectory ) {
		m_bStartupBulkLoad = true;
		m_bSkipSlotReserveOnce = true;
	}

	configureCpuRegionCache( m_pTerrain->isCpuRegionCacheEffective(),
		m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(), m_pTerrain->getCpuRegionLoadBudgetLayers(),
		m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
		m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(), m_pTerrain->getInstanceRegionLoadBudgetPages(),
		m_pTerrain->getInstanceRegionLoadBudgetMb() );
	configureGpuRegionCache( m_pTerrain->isGpuRegionCacheEffective(),
		m_pTerrain->getEffectiveGpuRegionCacheBudgetMb(), m_pTerrain->getGpuRegionUploadBudgetLayers(),
		m_pTerrain->getGpuRegionUploadBudgetMb(), m_pTerrain->getGpuRegionSparePages(),
		m_pTerrain->getGpuRegionRetireFrames() );

	bool bLoadedDirectoryThisActivation = false;
	if ( bLoadInitialDirectory ) {
		const Error load_error = loadDirectory( m_pTerrain->getDataDirectory() );
		m_bStartupBulkLoad = false;
		m_bSkipSlotReserveOnce = false;
		if ( load_error != OK && load_error != ERR_FILE_NOT_FOUND ) {
			LOG( T3DTerrain::ERROR, "Failed to load terrain directory: ", m_pTerrain->getDataDirectory(), " error: ", load_error );
			deactivateRuntimeStorage( pTerrain );
			return load_error;
		}
		bLoadedDirectoryThisActivation = load_error == OK;
	}
	m_bRepositoryLoaded = true;

	// A returning scene owns a complete repository but process-static height pages
	// were deliberately released while inactive. Rebuild the logical map from the
	// retained slot table and repopulate active storage before any renderer sees it.
	if ( !bLoadedDirectoryThisActivation && ( m_nActiveRegionCount > 0 || m_nRegionLocationCount > 0 ) ) {
		const Error rehydrate_error = rehydrateActiveStorage();
		if ( rehydrate_error != OK ) {
			deactivateRuntimeStorage( pTerrain );
			return rehydrate_error;
		}
	} else {
		rebuildRegionMapFromSlots();
		m_nActiveStorageGeneration = terrain3dActiveStorageGetOwnerGeneration( this );
	}

	configureResidentPool( 64 );
	m_bRuntimeActive = true;
	return OK;
}

void T3DData::deactivateRuntimeStorage( T3DTerrain *const pTerrain ) {
	if ( pTerrain && m_pTerrain && pTerrain != m_pTerrain ) {
		return;
	}
	if ( !m_bRuntimeActive && !m_bOwnsActiveStorage ) {
		return;
	}
	m_bRuntimeActive = false;
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	if ( m_pResidentPool ) {
		m_pResidentPool->resetResidency();
	}
	if ( m_bOwnsActiveStorage ) {
		terrain3dActiveStorageRelease( this );
		m_bOwnsActiveStorage = false;
	}
}

Dictionary T3DData::getRuntimeState( void ) const {
	Dictionary state;
	state["terrain_initialized"] = m_pTerrain && m_pTerrain->isInitialized();
	state["repository_loaded"] = m_bRepositoryLoaded;
	state["runtime_active"] = m_bRuntimeActive;
	state["active_storage_attached"] = m_bOwnsActiveStorage && terrain3dActiveStorageIsOwner( this );
	state["active_storage_generation"] = ( int64_t ) ( getActiveStorageGeneration() );
	state["region_map_size"] = m_nRegionMapSize;
	state["logical_region_count"] = m_nActiveRegionCount;
	state["gpu_resident_page_count"] = m_nGpuRegionResidentCount;
	state["middle_height_resident_page_count"] = m_nMiddleHeightResidentCount;
	state["middle_height_page_table_valid"] = m_generatedMiddleHeightRegionMap.getRid().is_valid();
	return state;
}

void T3DData::setRegionLocations( const TypedArray<Vector2i> &vLocations ) {
	LOG( T3DTerrain::INFO, "Reconciling active region locations with array sized: ", vLocations.size() );

	// Region locations exposed by the public API are compact, while the internal
	// texture arrays deliberately retain holes. Replacing the internal slot array
	// with the compact list changes every layer index after the first hole and
	// leaves the existing GPU arrays in the old order. Undo/redo used to do exactly
	// that, producing apparently random height corruption until a manual edit
	// reuploaded the affected layer.
	Dictionary desired_locations;
	TypedArray<Vector2i> unique_locations;
	for ( const Vector2i location : vLocations ) {
		if ( location == V2I_MAX || !isRegionLocationValidInternal( location ) || desired_locations.has( location ) ) {
			continue;
		}
		desired_locations[location] = true;
		unique_locations.push_back( location );
	}

	const bool bArraysReady = canUpdateRegionSlotsIncrementally();
	const int old_slot_count = m_nRegionLocationCount;
	bool bSlotsChanged = false;

	Dictionary existing_slots;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		if ( !desired_locations.has( location ) ) {
			if ( m_pResidentPool ) {
				m_pResidentPool->invalidate( location );
			}
			if ( m_bOwnsActiveStorage ) {
				terrain3dActiveStorageInvalidate( this, location );
			}
			invalidateGpuRegionLocation( location );
			invalidateCpuRegionLocation( location );
			invalidateInstanceRegionLocation( location );
			m_szRegionLocations[slot] = V2I_MAX;
			bSlotsChanged = true;
		} else {
			existing_slots[location] = slot;
		}
	}

	rebuildFreeRegionSlots();
	eastl::vector<int> added_slots;
	added_slots.reserve( unique_locations.size() );
	for ( const Vector2i location : unique_locations ) {
		if ( existing_slots.has( location ) ) {
			continue;
		}

		int slot = takeFreeRegionSlot();
		if ( slot < 0 ) {
			if ( m_nRegionLocationCount >= TERRAIN3D_MAX_REGION_SLOTS ) {
				LOG( T3DTerrain::ERROR, "Region slot capacity exceeded while restoring location ", location );
				continue;
			}
			slot = m_nRegionLocationCount++;
			m_szRegionLocations[slot] = location;
		} else {
			m_szRegionLocations[slot] = location;
		}
		added_slots.push_back( slot );
		bSlotsChanged = true;
	}

	m_nActiveRegionCount = unique_locations.size();
	if ( !bSlotsChanged ) {
		if ( m_bRegionMapDirty ) {
			updateMaps( T3DRegion::TYPE_MAX, false, false );
		}
		return;
	}

	m_bRegionMapDirty = true;
	const bool bCanUploadAddedLayers = bArraysReady && m_nRegionLocationCount == old_slot_count;
	if ( !bCanUploadAddedLayers ) {
		updateMaps( T3DRegion::TYPE_MAX, true, false );
		if ( m_pTerrain && m_pTerrain->getInstancer() ) {
			m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, false );
		}
		invalidateSamplingCache();
		return;
	}

	rebuildRegionMapFromSlots();
	bool bTextureLayersChanged = false;
	for ( const int slot : added_slots ) {
		if ( slot < 0 || slot >= m_nRegionLocationCount ) {
			continue;
		}
		const Vector2i location = m_szRegionLocations[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			// The requested active set and the region dictionary disagree. A full
			// rebuild safely sanitizes the slot table rather than exposing stale data.
			updateMaps( T3DRegion::TYPE_MAX, true, false );
			if ( m_pTerrain && m_pTerrain->getInstancer() ) {
			m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, false );
		}
			invalidateSamplingCache();
			return;
		}

		if ( !syncActiveRegion( location, pRegion ) ) {
			LOG( T3DTerrain::ERROR, "Unable to restore active terrain storage for region ", location );
			continue;
		}
		ensureDefaultMaps();
		m_heightMaps[slot] = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
		m_controlMaps[slot] = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_CONTROL );
		m_colorMaps[slot] = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR );
		Ref<Image> pDerived = pRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( {location} );
			pDerived = pRegion->getDerivedNormalSlopeMap();
		}
		pDerived = pDerived.is_valid() ? pDerived : m_pDefaultDerivedMap;
		m_derivedMaps[slot] = pDerived;
		m_generatedHeightMaps.update( m_heightMaps[slot], slot );
		m_generatedControlMaps.update( makeGpuRenderControlImage( location ), slot );
		m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[slot] ), slot );
		m_generatedDerivedMaps.update( pDerived, slot );
		if ( !IS_EDITOR ) {
			if ( m_pResidentPool ) {
				m_pResidentPool->syncDerived( location, pRegion );
			}
			pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
			m_derivedMaps[slot] = Ref<Image>();
		}
		bTextureLayersChanged = true;
	}

	calcHeightRange();
	emitRegionLayoutChanged( bTextureLayersChanged );
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
			m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, false );
		}
	invalidateSamplingCache();
}

// Returns an array of active regions, optionally a shallow or deep copy
TypedArray<Vector2i> T3DData::getRegionSlots( void ) const {
	TypedArray<Vector2i> slots;
	slots.resize( m_nRegionLocationCount );
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		slots[slot] = m_szRegionLocations[slot];
	}
	return slots;
}

TypedArray<Vector2i> T3DData::getRenderRegionSlots( void ) const {
	if ( !m_bGpuRegionCacheEnabled ) {
		return getRegionSlots();
	}
	TypedArray<Vector2i> slots;
	slots.resize( m_nGpuRegionPhysicalLayerCount );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		slots[layer] = m_szGpuRegionLocations[layer];
	}
	return slots;
}

PackedInt32Array T3DData::getRegionMap( void ) const {
	PackedInt32Array region_map;
	if ( m_nRegionMapSize <= 0 ) {
		return region_map;
	}
	region_map.resize( m_nRegionMapSize * m_nRegionMapSize );
	int32_t *pWrite = region_map.ptrw();
	const int half = m_nRegionMapSize / 2;
	const uint16_t *pSourceMap = m_bGpuRegionCacheEnabled ? m_szGpuRegionMap : m_szRegionMap;
	for ( int y = 0; y < m_nRegionMapSize; ++y ) {
		for ( int x = 0; x < m_nRegionMapSize; ++x ) {
			const Vector2i location( x - half, y - half );
			const int source_index = getRegionMapIndex( location );
			pWrite[y * m_nRegionMapSize + x] = source_index >= 0 ? pSourceMap[source_index] : 0;
		}
	}
	return region_map;
}

PackedInt32Array T3DData::getLogicalRegionMap( void ) const {
	PackedInt32Array region_map;
	if ( m_nRegionMapSize <= 0 ) {
		return region_map;
	}
	region_map.resize( m_nRegionMapSize * m_nRegionMapSize );
	int32_t *pWrite = region_map.ptrw();
	const int half = m_nRegionMapSize / 2;
	for ( int y = 0; y < m_nRegionMapSize; ++y ) {
		for ( int x = 0; x < m_nRegionMapSize; ++x ) {
			const Vector2i location( x - half, y - half );
			const int source_index = getRegionMapIndex( location );
			pWrite[y * m_nRegionMapSize + x] = source_index >= 0 && m_szRegionMap[source_index] != 0u ? 1 : 0;
		}
	}
	return region_map;
}

TypedArray<T3DRegion> T3DData::getRegionsActive( const bool bCopy, const bool bDeep ) const {
	TypedArray<T3DRegion> region_arr;
	if ( bCopy && m_bCpuRegionCacheEnabled ) {
		LOG( T3DTerrain::ERROR, "Whole-repository region copies are unavailable while bounded CPU residency is enabled. "
			"Disable the CPU cache before requesting copied Terrain3DRegion resources." );
		return region_arr;
	}
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i region_loc = m_szRegionLocations[slot];
		Ref<T3DRegion> pRegion = getRegion( region_loc );
		if ( pRegion.is_valid() ) {
			region_arr.push_back( bCopy ? pRegion->duplicate( bDeep ) : pRegion );
		}
	}
	return region_arr;
}

// Calls the callback function for every region within the given (descaled) area
// The callable receives: source Terrain3DRegion, source Rect2i, dest Rect2i, (bindings)
// Used with change_region_size, dest Terrain3DRegion is bound as the 4th parameter
void T3DData::doForRegions( const Rect2i &rArea, const Callable &rCallback ) {
	const Rect2i location_bounds = getRegionBoundsForArea( rArea, m_nRegionSize );
	if ( location_bounds.size.x <= 0 || location_bounds.size.y <= 0 ) {
		return;
	}
	LOG( T3DTerrain::DEBUG, "Processing global area: ", rArea, " -> ", location_bounds );
	Point2i current_region_loc;
	for ( int y = location_bounds.position.y; y < location_bounds.get_end().y; y++ ) {
		current_region_loc.y = y;
		for ( int x = location_bounds.position.x; x < location_bounds.get_end().x; x++ ) {
			current_region_loc.x = x;
			const T3DRegion *pRegion = getRegionPtr( current_region_loc );
			if ( pRegion && !pRegion->isDeleted() ) {
				if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResident( current_region_loc ) ) {
					LOG( T3DTerrain::ERROR, "Unable to page CPU terrain maps for region callback: ", current_region_loc );
					continue;
				}
				pRegion = getRegionPtr( current_region_loc );
				LOG( T3DTerrain::DEBUG, "Current region: ", current_region_loc );
				const Rect2i regionArea = rArea.intersection( Rect2i( current_region_loc * m_nRegionSize, m_regionSizev ) );
				LOG( T3DTerrain::DEBUG, "Region bounds: ", Rect2i( current_region_loc * m_nRegionSize, m_regionSizev ) );
				LOG( T3DTerrain::DEBUG, "Region area: ", regionArea );
				const Rect2i dstCoords( regionArea.position - rArea.position, regionArea.size );
				const Rect2i srcCoords( regionArea.position - ( pRegion->getLocation() * m_regionSizev ), dstCoords.size );
				LOG( T3DTerrain::DEBUG, "src map coords: ", srcCoords );
				LOG( T3DTerrain::DEBUG, "dst map coords: ", dstCoords );
				rCallback.call( pRegion, srcCoords, dstCoords );
			}
		}
	}
}

void T3DData::changeRegionSize( const int nNewSize ) {
	LOG( T3DTerrain::INFO, "Changing region size from: ", m_nRegionSize, " to ", nNewSize );
	if ( !m_pTerrain ) {
		LOG( T3DTerrain::WARN, "Region-size migration requested before Terrain3DData has an initialized Terrain3D owner. "
			"Deferring migration until Terrain3D initialization." );
		return;
	}
	if ( nNewSize != 128 ) {
		LOG( T3DTerrain::ERROR, "This Terrain3D fork is specialized for 128-sample regions and rejects all other sizes" );
		return;
	}
	if ( !isValidRegionSize( nNewSize ) ) {
		LOG( T3DTerrain::ERROR, "Invalid region size: ", nNewSize, ". Must be power of 2, 64-2048" );
		return;
	}
	if ( nNewSize == m_nRegionSize ) {
		return;
	}

	const TypedArray<T3DRegion> old_regions = getRegionsActive();
	if ( old_regions.is_empty() ) {
		m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( nNewSize ) );
		return;
	}

	// Preflight the complete conversion before allocating or deleting anything.
	// The world remains 4096x4096 texels; changing region size changes only the
	// number of logical regions that partition that fixed extent.
	Vector2i worldMin( INT32_MAX, INT32_MAX );
	Vector2i worldMax( INT32_MIN, INT32_MIN );
	for ( const Ref<T3DRegion> pRegion : old_regions ) {
		if ( pRegion.is_null() ) {
			continue;
		}
		const Vector2i region_begin = pRegion->getLocation() * m_nRegionSize;
		const Vector2i region_end = region_begin + m_regionSizev;
		worldMin.x = MIN( worldMin.x, region_begin.x );
		worldMin.y = MIN( worldMin.y, region_begin.y );
		worldMax.x = MAX( worldMax.x, region_end.x );
		worldMax.y = MAX( worldMax.y, region_end.y );
	}

	const Rect2i converted_bounds = getRegionBoundsForArea( Rect2i( worldMin, worldMax - worldMin ), nNewSize );
	const Vector2i converted_last = converted_bounds.get_end() - Vector2i( 1, 1 );
	const int target_map_size = TERRAIN3D_ACTIVE_WORLD_TEXELS / nNewSize;
	if ( !terrain3dRegionLocationValid( converted_bounds.position, target_map_size ) ||
		!terrain3dRegionLocationValid( converted_last, target_map_size ) ) {
		LOG( T3DTerrain::ERROR, "Cannot change region size from ", m_nRegionSize, " to ", nNewSize,
			" without losing data. Converted region coordinates would span ", converted_bounds.position, " through ",
			converted_last, ", but valid coordinates are ", -target_map_size / 2, " through ",
			target_map_size / 2 - 1, ". Source data was left unchanged." );
		return;
	}

	// Copy each source region directly into its intersecting destination regions.
	// This removes Callable/Variant dispatch and repeated source-region lookups
	// from the hot path, which is particularly costly when splitting down to 64.
	Dictionary new_regions_by_location;
	TypedArray<T3DRegion> new_regions;
	for ( const Ref<T3DRegion> pSrcRegion : old_regions ) {
		if ( pSrcRegion.is_null() ) {
			continue;
		}
		if ( !ensureCpuRegionResident( pSrcRegion->getLocation() ) ) {
			LOG( T3DTerrain::ERROR, "Unable to page source region during region-size conversion: ", pSrcRegion->getLocation() );
			return;
		}

		const Rect2i srcArea( pSrcRegion->getLocation() * m_nRegionSize, m_regionSizev );
		const Rect2i target_bounds = getRegionBoundsForArea( srcArea, nNewSize );
		for ( int y = target_bounds.position.y; y < target_bounds.get_end().y; y++ ) {
			for ( int x = target_bounds.position.x; x < target_bounds.get_end().x; x++ ) {
				const Vector2i targetLocation( x, y );
				Ref<T3DRegion> pDstRegion;
				if ( new_regions_by_location.has( targetLocation ) ) {
					pDstRegion = new_regions_by_location[targetLocation];
				} else {
					pDstRegion.instantiate();
					pDstRegion->setLocation( targetLocation );
					pDstRegion->setRegionSize( nNewSize );
					pDstRegion->setVertexSpacing( m_fVertexSpacing );
					pDstRegion->setModified( true );
					pDstRegion->sanitizeMaps();
					new_regions_by_location[targetLocation] = pDstRegion;
					new_regions.push_back( pDstRegion );
				}

				const Rect2i dstArea( targetLocation * nNewSize, V2I( nNewSize ) );
				const Rect2i overlap = srcArea.intersection( dstArea );
				if ( overlap.size.x <= 0 || overlap.size.y <= 0 ) {
					continue;
				}

				const Rect2i srcRect( overlap.position - srcArea.position, overlap.size );
				const Rect2i dstRect( overlap.position - dstArea.position, overlap.size );
				copyPasteDfr( pSrcRegion.ptr(), srcRect, dstRect, pDstRegion.ptr() );
			}
		}
	}

	// Remove old data
	m_pTerrain->getInstancer()->destroy();
	for ( const Ref<T3DRegion> pRegion : old_regions ) {
		removeRegion( pRegion, false );
	}

	// Change region size
	m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( nNewSize ) );

	// Add new regions and rebuild
	for ( const Ref<T3DRegion> pRegion : new_regions ) {
		addRegion( pRegion, false );
	}

	calcHeightRange( true );
	updateMaps( T3DRegion::TYPE_MAX, true, true );
	m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, true );
}


void T3DData::setRegionModified( const Vector2i &rRegionLoc, const bool bModified ) {
	T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setModified( bModified );
}

bool T3DData::isRegionModified( const Vector2i &rRegionLoc ) const {
	T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return false;
	}
	return pRegion->isModified();
}

void T3DData::setRegionDeleted( const Vector2i &rRegionLoc, const bool bDeleted ) {
	T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setDeleted( bDeleted );
}

bool T3DData::isRegionDeleted( const Vector2i &rRegionLoc ) const {
	const T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return true;
	}
	return pRegion->isDeleted();
}

Ref<T3DRegion> T3DData::addRegionBlankp( const Vector3 &rGlobalPosition, const bool bUpdate ) {
	return addRegionBlank( getRegionLocation( rGlobalPosition ), bUpdate );
}

Ref<T3DRegion> T3DData::addRegionBlank( const Vector2i &rRegionLoc, const bool bUpdate ) {
	Ref<T3DRegion> pRegion;
	pRegion.instantiate();
	pRegion->setLocation( rRegionLoc );
	pRegion->setRegionSize( m_nRegionSize );
	pRegion->setVertexSpacing( m_fVertexSpacing );
	if ( addRegion( pRegion, bUpdate ) == OK ) {
		pRegion->setModified( true );
		return pRegion;
	}
	return Ref<T3DRegion>();
}

/** Adds a Terrain3DRegion to the terrain
 * Marks region as modified
 *	p_update - rebuild the maps if true. Set to false if bulk adding many regions.
 */
Error T3DData::addRegion( const Ref<T3DRegion> &rRegion, const bool bUpdate ) {
	if ( rRegion.is_null() ) {
		return ERR_INVALID_PARAMETER;
	}
	return addRegionInternal( rRegion, bUpdate, true );
}

Error T3DData::replaceRegionData( const Vector2i &rRegionLoc, const Dictionary &dictData ) {
	if ( !ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( T3DTerrain::ERROR, "Cannot load CPU map payload for replacement region: ", rRegionLoc );
		return ERR_CANT_OPEN;
	}
	Ref<T3DRegion> pCurrent = getRegion( rRegionLoc );
	if ( pCurrent.is_null() || pCurrent->isDeleted() || getRegionId( rRegionLoc ) < 0 ) {
		LOG( T3DTerrain::ERROR, "Cannot replace data for inactive region: ", rRegionLoc );
		return ERR_DOES_NOT_EXIST;
	}

	// Mutate a detached deep copy first. If validation fails, the currently active
	// resource, render state, collision, and GPU layers remain untouched.
	Ref<T3DRegion> pReplacement = pCurrent->duplicate( true );
	if ( pReplacement.is_null() ) {
		return ERR_CANT_CREATE;
	}
	pReplacement->setData( dictData );
	pReplacement->setLocation( rRegionLoc ); // Repository location is authoritative.
	pReplacement->setDeleted( false );
	pReplacement->setEdited( false );
	pReplacement->setVertexSpacing( m_fVertexSpacing );
	if ( pReplacement->getRegionSize() != m_nRegionSize ) {
		LOG( T3DTerrain::ERROR, "Replacement region size ", pReplacement->getRegionSize(), " does not match terrain region size ",
			m_nRegionSize );
		return ERR_INVALID_DATA;
	}
	pReplacement->sanitizeMaps();
	if ( !pReplacement->validateMapSize( pReplacement->getHeightMap() ) ||
		( pReplacement->getControlMap().is_valid() && !pReplacement->validateMapSize( pReplacement->getControlMap() ) ) ||
		( pReplacement->getColorMap().is_valid() && !pReplacement->validateMapSize( pReplacement->getColorMap() ) ) ) {
		LOG( T3DTerrain::ERROR, "Replacement snapshot contains invalid map dimensions" );
		return ERR_INVALID_DATA;
	}
	pReplacement->setModified( true );
	return addRegionInternal( pReplacement, true, false );
}

Error T3DData::addRegionInternal(
	const Ref<T3DRegion> &rRegion, const bool bUpdate,
	const bool bSanitize
) {
	if ( rRegion.is_null() ) {
		LOG( T3DTerrain::ERROR, "Provided region is null. Returning" );
		return FAILED;
	}
	const Vector2i region_loc = rRegion->getLocation();
	LOG( T3DTerrain::DEBUG, "Adding region at location ", region_loc, ", update maps: ", bUpdate ? "yes" : "no" );

	const int map_index = getRegionMapIndex( region_loc );
	if ( map_index < 0 || !isRegionLocationValidInternal( region_loc ) ) {
		LOG( T3DTerrain::ERROR, "Location ", region_loc, " out of active bounds. Max: ", -m_nRegionMapSize / 2, " to ",
			m_nRegionMapSize / 2 - 1 );
		return FAILED;
	}

	if ( bSanitize ) {
		rRegion->sanitizeMaps();
	}
	rRegion->setDeleted( false );
	int existing_slot = getRegionId( region_loc );
	// A dirty map can occur during bulk operations. Only fall back to a slot scan
	// when a region resource already exists at this location; genuinely new
	// regions keep the constant-time region-map path.
	const T3DRegion *pStoredRegion = getRegionPtr( region_loc );
	const bool bHadPriorRegionResource = pStoredRegion != nullptr;
	if ( existing_slot < 0 && pStoredRegion && !pStoredRegion->isDeleted() ) {
		existing_slot = findRegionSlot( region_loc );
	}
	if ( !syncActiveRegion( region_loc, rRegion.ptr() ) ) {
		if ( pStoredRegion && !pStoredRegion->isDeleted() ) {
			syncActiveRegion( region_loc, pStoredRegion );
		}
		LOG( T3DTerrain::ERROR, "Unable to publish region ", region_loc, " into process-static active storage" );
		return ERR_INVALID_DATA;
	}
	const bool bReplacingExistingRegion = existing_slot >= 0;
	const bool bRequiresInstanceReconciliation = bHadPriorRegionResource || rRegion->hasInstances();
	if ( m_pResidentPool ) {
		// The region resource is authoritative. Drop any mirror of the previous
		// resource so the next sampler acquisition repopulates the same fixed slot.
		m_pResidentPool->invalidate( region_loc );
	}
	m_regions[region_loc] = rRegion;
	m_szRegionPtrMap[map_index] = rRegion.ptr();

	// Overwriting an active slot never requires reallocating the texture arrays.
	if ( bReplacingExistingRegion && bUpdate && canUpdateRegionSlotsIncrementally() ) {
		ensureDefaultMaps();
		m_heightMaps[existing_slot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_HEIGHT );
		m_controlMaps[existing_slot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_CONTROL );
		m_colorMaps[existing_slot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_COLOR );
		Ref<Image> pDerived = rRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( {region_loc} );
			pDerived = rRegion->getDerivedNormalSlopeMap();
		}
		if ( pDerived.is_null() ) {
			pDerived = m_pDefaultDerivedMap;
		}
		m_derivedMaps[existing_slot] = pDerived;
		m_generatedHeightMaps.update( m_heightMaps[existing_slot], existing_slot );
		m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), existing_slot );
		m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[existing_slot] ), existing_slot );
		m_generatedDerivedMaps.update( pDerived, existing_slot );
		calcHeightRange();
		emitRegionLayoutChanged( true );
		// Region replacement is authoritative. Reconcile rendered state even when
		// the replacement contains no instances, otherwise removed cells can remain.
		m_pTerrain->getInstancer()->updateMmis( -1, region_loc, false );
		invalidateSamplingCache();
		return OK;
	}

	if ( !bReplacingExistingRegion ) {
		// Reuse a retained blank layer. This also applies to bulk operations so a
		// region-size conversion cannot accumulate old holes plus new slots.
		const int free_slot = takeFreeRegionSlot();
		if ( free_slot >= 0 ) {
			m_szRegionLocations[free_slot] = region_loc;
			++m_nActiveRegionCount;
			if ( bUpdate && canUpdateRegionSlotsIncrementally() ) {
				m_szRegionMap[map_index] = free_slot + 1;
				ensureDefaultMaps();
				m_heightMaps[free_slot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_HEIGHT );
				m_controlMaps[free_slot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_CONTROL );
				m_colorMaps[free_slot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_COLOR );
				regenerateDerivedMaps( {region_loc} );
				Ref<Image> pDerived = rRegion->getDerivedNormalSlopeMap();
				if ( pDerived.is_null() ) {
					pDerived = m_pDefaultDerivedMap;
				}

				m_derivedMaps[free_slot] = pDerived;
				m_generatedHeightMaps.update( m_heightMaps[free_slot], free_slot );
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), free_slot );
				m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[free_slot] ), free_slot );
				m_generatedDerivedMaps.update( pDerived, free_slot );
				updateMasterHeights( rRegion->getHeightRange() );
				emitRegionLayoutChanged( true );
				if ( bRequiresInstanceReconciliation ) {
					m_pTerrain->getInstancer()->updateMmis( -1, region_loc, false );
				}
				invalidateSamplingCache();
				return OK;
			}
			m_bRegionMapDirty = true;
		} else {
				if ( m_nRegionLocationCount >= TERRAIN3D_MAX_REGION_SLOTS ) {
					LOG( T3DTerrain::ERROR, "Region slot capacity exceeded while adding region ", region_loc );
					return ERR_OUT_OF_MEMORY;
				}
				m_szRegionLocations[m_nRegionLocationCount++] = region_loc;
			++m_nActiveRegionCount;
			m_bRegionMapDirty = true;
		}
	}

	LOG( T3DTerrain::DEBUG, "Storing region ", region_loc, " version ", vformat( "%.3f", rRegion->getVersion() ),
		" active count: ", m_nActiveRegionCount, ", slots: ", m_nRegionLocationCount );
	if ( bUpdate ) {
		updateMaps( T3DRegion::TYPE_MAX, true, false );
		// Existing regions require authoritative reconciliation even when the
		// replacement is empty. A genuinely new empty region has no stale render
		// state and should not schedule an otherwise pointless pre-draw pass.
		if ( bRequiresInstanceReconciliation ) {
			m_pTerrain->getInstancer()->updateMmis( -1, region_loc, false );
		}
	}
	invalidateSamplingCache();
	return OK;
}

void T3DData::removeRegionp( const Vector3 &rGlobalPosition, const bool bUpdate ) {
	const Ref<T3DRegion> pRegion = getRegion( getRegionLocation( rGlobalPosition ) );
	removeRegion( pRegion, bUpdate );
}

void T3DData::removeRegionl( const Vector2i &rRegionLoc, const bool bUpdate ) {
	const Ref<T3DRegion> pRegion = getRegion( rRegionLoc );
	removeRegion( pRegion, bUpdate );
}

// Remove region marks the region for deletion, and removes it from the active arrays indexed by ID
// It remains stored in _regions and the file remains on disk until saved, when both are removed
void T3DData::removeRegion( const Ref<T3DRegion> &rRegion, const bool bUpdate ) {
	if ( rRegion.is_null() ) {
		LOG( T3DTerrain::ERROR, "Region not found or is null. Returning" );
		return;
	}

	const Vector2i region_loc = rRegion->getLocation();
	int region_slot = getRegionId( region_loc );
	if ( region_slot < 0 ) {
		region_slot = findRegionSlot( region_loc );
	}
	LOG( T3DTerrain::INFO, "Marking region ", region_loc, " for deletion. update maps: ", bUpdate ? "yes" : "no" );
	if ( region_slot < 0 ) {
		LOG( T3DTerrain::ERROR, "Region ", region_loc, " not found in active region slots. Returning" );
		return;
	}
	if ( m_pResidentPool ) {
		m_pResidentPool->invalidate( region_loc );
	}
	if ( m_bOwnsActiveStorage ) {
		terrain3dActiveStorageInvalidate( this, region_loc );
	}
	invalidateGpuRegionLocation( region_loc );
	invalidateCpuRegionLocation( region_loc );
	invalidateInstanceRegionLocation( region_loc );

	rRegion->setDeleted( true );
	releaseRegionSlot( region_slot );
	m_nActiveRegionCount = MAX( 0, m_nActiveRegionCount - 1 );
	const int map_index = getRegionMapIndex( region_loc );
	if ( map_index >= 0 && map_index < TERRAIN3D_MAX_REGION_SLOTS ) {
		m_szRegionMap[map_index] = 0;
	} else {
		m_bRegionMapDirty = true;
	}
	// Far atlases are whole-world caches and remain authoritative after near-page
	// residency is removed. Dirty the deleted tile so the incremental rebuild
	// writes empty height/surface data and prevents stale terrain from surviving
	// at distance or being persisted into the cooked .t3dfar cache.
	markGpuFarRegionDirty( region_loc, T3DRegion::TYPE_MAX );
	calcHeightRange();

	invalidateSamplingCache();
	if ( bUpdate ) {
		// The texture layers can remain allocated because the region map no longer
		// references this slot. Retaining the layer makes removal constant-time and
		// lets a later add reuse it with a single texture_2d_update per map.
		emitRegionLayoutChanged( false );
		m_pTerrain->getInstancer()->destroyRegionMmis( region_loc );
	}
}

void T3DData::saveDirectory( const String &rDir ) {
	LOG( T3DTerrain::INFO, "Saving data files to ", rDir );
	const Array locations = m_regions.keys();
	for ( const Vector2i region_loc : locations ) {
		saveRegion( region_loc, rDir, m_pTerrain->getSave16Bit() );
	}
	if ( m_pTerrain && m_pTerrain->isGpuRegionCookOnSave() && !writeCookedGpuFarAtlases( rDir ) ) {
		LOG( T3DTerrain::ERROR, "Terrain regions saved, but cooked far-atlas generation failed" );
	}
	// saveRegion() updates only the exact resource paths Terrain3D changed. Do
	// not force a project-wide EditorFileSystem scan from a scene-save callback:
	// scans are allowed to reload changed resources and make save-on-focus-loss
	// turn every editor refocus into unnecessary reload work.
}

// You may need to do a file system scan to update FileSystem panel
void T3DData::saveRegion( const Vector2i &rRegionLoc, const String &rDir, const bool b16Bit ) {
	Ref<T3DRegion> pRegion = getRegion( rRegionLoc );
	if ( pRegion.is_null() ) {
		LOG( T3DTerrain::ERROR, "No region found at: ", rRegionLoc );
		return;
	}
	const String fname = Util::locationToFilename( rRegionLoc );
	const String path = rDir + String( "/" ) + fname;
	// If region marked for deletion, remove from disk and from _regions, but don't free in case stored in undo
	if ( pRegion->isDeleted() ) {
		LOG( T3DTerrain::DEBUG, "Removing ", rRegionLoc, " from _regions" );
		m_regions.erase( rRegionLoc );
		const int map_index = getRegionMapIndex( rRegionLoc );
		if ( map_index >= 0 ) {
			m_szRegionPtrMap[map_index] = nullptr;
		}
		LOG( T3DTerrain::DEBUG, "File to be deleted: ", path );
		if ( FileAccess::file_exists( path ) ) {
			Ref<DirAccess> pDa = DirAccess::open( rDir );
			if ( pDa.is_null() ) {
				LOG( T3DTerrain::ERROR, "Cannot open directory for writing: ", rDir, " error: ", DirAccess::get_open_error() );
				return;
			}
			const Error err = pDa->remove( fname );
			if ( err != OK ) {
				LOG( T3DTerrain::ERROR, "Could not remove file: ", fname, ", error code: ", err );
				return;
			}
			LOG( T3DTerrain::INFO, "File ", path, " deleted" );
		} else {
			LOG( T3DTerrain::INFO, "File to delete ", path, " doesn't exist. (Maybe from add, undo, save)" );
		}
		const String page_path = path.get_basename() + String( ".t3dpage" );
		if ( FileAccess::file_exists( page_path ) ) {
			DirAccess::remove_absolute( page_path );
			if ( IS_EDITOR && page_path.begins_with( "res://" ) ) {
				EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
				if ( pFilesystem && !pFilesystem->is_scanning() ) {
					pFilesystem->update_file( page_path );
				}
			}
		}
		if ( m_pTerrain && m_pTerrain->isGpuRegionCookOnSave() ) {
			// Deleting one source changes the compiled two-texel control halo of all
			// immediate neighbors and the forward-normal edge of its left/top
			// predecessors. Re-cook the whole 3x3 neighborhood after repository
			// removal so no page can retain data from the deleted region.
			for ( int dz = -1; dz <= 1; ++dz ) {
				for ( int dx = -1; dx <= 1; ++dx ) {
					if ( dx == 0 && dz == 0 ) {
						continue;
					}
					const Vector2i cook_location = rRegionLoc + Vector2i( dx, dz );
					T3DRegion *const pCookRegion = getRegionPtr( cook_location );
					if ( !pCookRegion || pCookRegion->isDeleted() ) {
						continue;
					}
					if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( cook_location, true ) ) {
						LOG( T3DTerrain::ERROR, "Region deleted, but neighboring runtime page source could not be loaded: ", cook_location );
						continue;
					}
					if ( !writeGpuRuntimePage( pCookRegion ) ) {
						LOG( T3DTerrain::ERROR, "Region deleted, but neighboring runtime GPU page cooking failed: ", cook_location );
					}
				}
			}
		}
		if ( IS_EDITOR && path.begins_with( "res://" ) ) {
			EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
			if ( pFilesystem && !pFilesystem->is_scanning() ) {
				pFilesystem->update_file( path );
			}
		}
		return;
	}
	if ( !pRegion->isModified() ) {
		return;
	}
	if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( T3DTerrain::ERROR, "Unable to page CPU terrain maps before saving region: ", rRegionLoc );
		return;
	}
	const Error err = pRegion->save( path, b16Bit );
	if ( !( err == OK || err == ERR_SKIP ) ) {
		LOG( T3DTerrain::ERROR, "Could not save file: ", path, ", error: ", UtilityFunctions::error_string( err ), " (", err, ")" );
	} else if ( err == OK ) {
		if ( IS_EDITOR && path.begins_with( "res://" ) ) {
			EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
			if ( pFilesystem && !pFilesystem->is_scanning() ) {
				pFilesystem->update_file( path );
			}
		}
		if ( !( m_pTerrain && m_pTerrain->isGpuRegionCookOnSave() ) ) {
			return;
		}
		// The compiled render-control field uses a two-texel cross-region halo. A
		// source edit can therefore change the transition field of any immediate
		// neighbor even though that neighbor's authored resource did not change.
		for ( int dz = -1; dz <= 1; ++dz ) {
			for ( int dx = -1; dx <= 1; ++dx ) {
				const Vector2i cook_location = rRegionLoc + Vector2i( dx, dz );
				T3DRegion *const pCookRegion = getRegionPtr( cook_location );
				if ( !pCookRegion || pCookRegion->isDeleted() ) {
					continue;
				}
				if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( cook_location, true ) ) {
					LOG( T3DTerrain::ERROR, "Region saved, but neighboring runtime page source could not be loaded: ", cook_location );
					continue;
				}
				if ( !writeGpuRuntimePage( pCookRegion ) ) {
					LOG( T3DTerrain::ERROR, "Region saved, but runtime GPU page cooking failed: ", cook_location );
				}
			}
		}
	}
}

bool T3DData::collectGpuRegionPageSources(
	String &rDataDirectory, PackedStringArray &rSourceFiles,
	eastl::vector<Vector2i> &rLocations, PackedStringArray &rErrors
) const {
	rDataDirectory = m_pTerrain ? m_pTerrain->getDataDirectory().trim_suffix( "/" ) : String();
	rSourceFiles.clear();
	rLocations.clear();
	rErrors.clear();
	if ( !m_pTerrain ) {
		rErrors.push_back( "Terrain3D owner is unavailable" );
		return false;
	}
	if ( rDataDirectory.is_empty() ) {
		rErrors.push_back( "Terrain3D data_directory is empty" );
		return false;
	}

	rSourceFiles = Util::getFiles( rDataDirectory, "terrain3d*.res" );
	rSourceFiles.sort();
	if ( rSourceFiles.is_empty() ) {
		rErrors.push_back( "No Terrain3D region .res sources were found in " + rDataDirectory );
		return false;
	}

	eastl::bitset<TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE> source_locations;
	rLocations.reserve( ( size_t ) ( rSourceFiles.size() ) );
	for ( const String &fname : rSourceFiles ) {
		const Vector2i location = Util::filenameToLocation( fname );
		const int map_index = getRegionMapIndex( location );
		if ( map_index < 0 || Util::locationToFilename( location ) != fname ) {
			rErrors.push_back( "Malformed or out-of-range Terrain3D region source: " + fname );
			continue;
		}
		if ( source_locations.test( ( size_t ) ( map_index ) ) ) {
			rErrors.push_back( "Duplicate Terrain3D region source location " + String( location ) );
			continue;
		}
		source_locations.set( ( size_t ) ( map_index ) );

		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			rErrors.push_back( "Region source " + fname + " exists in the data directory but is not loaded" );
			continue;
		}
		const String expected_path = rDataDirectory.path_join( fname ).simplify_path();
		const String loaded_path = pRegion->get_path().simplify_path();
		if ( loaded_path != expected_path ) {
			rErrors.push_back( "Region " + String( location ) + " is loaded from " +
				( loaded_path.is_empty() ? String( "<unsaved resource>" ) : loaded_path ) +
				" instead of data-directory source " + expected_path );
			continue;
		}
		rLocations.push_back( location );
	}

	const TypedArray<Vector2i> loaded_locations = getRegionLocations();
	for ( const Vector2i location : loaded_locations ) {
		const int map_index = getRegionMapIndex( location );
		if ( map_index < 0 || !source_locations.test( ( size_t ) ( map_index ) ) ) {
			rErrors.push_back( "Loaded region " + String( location ) +
				" has no matching .res source in " + rDataDirectory );
		}
	}
	return rErrors.is_empty();
}

Dictionary T3DData::inspectGpuRegionPages( void ) {
	Dictionary result;
	PackedStringArray source_files;
	PackedStringArray errors;
	eastl::vector<Vector2i> locations;
	String data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int discovered = source_files.size();

	result["data_directory"] = data_dir;
	result["discovered"] = discovered;
	result["valid"] = ( int ) ( locations.size() );
	result["preflight_failed"] = !preflight_ok;
	if ( !preflight_ok ) {
		result["current"] = 0;
		result["missing"] = 0;
		result["stale"] = 0;
		result["invalid"] = 0;
		result["source_unavailable"] = errors.size();
		result["needs_cook"] = discovered;
		result["orphan_count"] = 0;
		result["orphan_pages"] = PackedStringArray();
		result["needs_cook_regions"] = PackedStringArray();
		result["errors"] = errors;
		result["complete"] = false;
		return result;
	}

	PackedStringArray needs_cook_regions;
	PackedStringArray orphan_pages;
	int current = 0;
	int missing = 0;
	int stale = 0;
	int invalid = 0;
	int source_unavailable = 0;
	for ( const Vector2i location : locations ) {
		T3DRegion *const pRegion = getRegionPtr( location );
		const String source_path = data_dir.path_join( Util::locationToFilename( location ) );
		const String page_path = source_path.get_basename() + String( ".t3dpage" );
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( location, true ) ) {
			++source_unavailable;
			needs_cook_regions.push_back( String( location ) );
			errors.push_back( "Region " + String( location ) + ": unable to materialize source maps" );
			continue;
		}

		const uint64_t expected_signature = computeGpuRuntimePageSourceSignature( pRegion );
		if ( expected_signature == 0u ) {
			++source_unavailable;
			needs_cook_regions.push_back( String( location ) );
			errors.push_back( "Region " + String( location ) + ": source signature generation failed" );
			continue;
		}
		if ( !FileAccess::file_exists( page_path ) ) {
			++missing;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}

		uint64_t existing_signature = 0u;
		if ( !readGpuRuntimePageSourceSignature( page_path, existing_signature ) ) {
			++invalid;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}
		if ( existing_signature != expected_signature ) {
			++stale;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}

		t3dpreparedGpuRuntimePage_t validation_page;
		validation_page.location = location;
		validation_page.pagePath = page_path;
		validation_page.sourcePath = source_path;
		validation_page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
		validation_page.nRegionSize = m_nRegionSize;
		validation_page.colorDivisor = m_nGpuRegionColorDivisor;
		validation_page.derivedDivisor = m_nGpuRegionDerivedDivisor;
		validation_page.middleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
		if ( !loadGpuRuntimePageSnapshot( validation_page ) ) {
			++invalid;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}
		++current;
	}

	PackedStringArray page_files = Util::getFiles( data_dir, "terrain3d*.t3dpage" );
	page_files.sort();
	for ( const String &fname : page_files ) {
		const String source_name = fname.get_basename() + String( ".res" );
		const String source_path = data_dir.path_join( source_name );
		if ( !FileAccess::file_exists( source_path ) ) {
			orphan_pages.push_back( data_dir.path_join( fname ) );
		}
	}

	releaseUntrackedCpuMaps();
	const int needs_cook = missing + stale + invalid + source_unavailable;
	result["current"] = current;
	result["missing"] = missing;
	result["stale"] = stale;
	result["invalid"] = invalid;
	result["source_unavailable"] = source_unavailable;
	result["needs_cook"] = needs_cook;
	result["orphan_count"] = orphan_pages.size();
	result["orphan_pages"] = orphan_pages;
	result["needs_cook_regions"] = needs_cook_regions;
	result["errors"] = errors;
	result["complete"] = source_unavailable == 0 && current + missing + stale + invalid == discovered;
	return result;
}

Dictionary T3DData::cookGpuRegionPages( void ) {
	Dictionary result;
	PackedStringArray source_files;
	PackedStringArray errors;
	eastl::vector<Vector2i> locations;
	String data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int discovered = source_files.size();
	const int valid = ( int ) ( locations.size() );
	if ( !preflight_ok ) {
		releaseUntrackedCpuMaps();
		result["data_directory"] = data_dir;
		result["far_atlas_cooked"] = false;
		result["material_cache"] = Dictionary();
		result["material_cache_cooked"] = false;
		result["preflight_failed"] = true;
		result["discovered"] = discovered;
		result["valid"] = valid;
		result["cooked"] = 0;
		result["unchanged"] = 0;
		result["usable"] = 0;
		result["skipped"] = 0;
		result["failed"] = errors.size();
		result["orphan_removed"] = 0;
		result["orphan_failed"] = 0;
		result["region_count"] = m_nActiveRegionCount;
		result["complete"] = false;
		result["errors"] = errors;
		return result;
	}

	int cooked = 0;
	int unchanged = 0;
	int failed = 0;
	int orphan_removed = 0;
	int orphan_failed = 0;
	for ( const Vector2i location : locations ) {
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( location, true ) ) {
			++failed;
			errors.push_back( "Region " + String( location ) + ": unable to materialize source maps" );
			continue;
		}

		const uint64_t expected_signature = computeGpuRuntimePageSourceSignature( pRegion );
		if ( expected_signature == 0u ) {
			++failed;
			errors.push_back( "Region " + String( location ) + ": source signature generation failed" );
			continue;
		}
		const String source_path = data_dir.path_join( Util::locationToFilename( location ) );
		const String page_path = source_path.get_basename() + String( ".t3dpage" );
		uint64_t existing_signature = 0u;
		bool bCurrent = readGpuRuntimePageSourceSignature( page_path, existing_signature ) &&
			existing_signature == expected_signature;
		if ( bCurrent ) {
			t3dpreparedGpuRuntimePage_t validation_page;
			validation_page.location = location;
			validation_page.pagePath = page_path;
			validation_page.sourcePath = source_path;
			validation_page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
			validation_page.nRegionSize = m_nRegionSize;
			validation_page.colorDivisor = m_nGpuRegionColorDivisor;
			validation_page.derivedDivisor = m_nGpuRegionDerivedDivisor;
			validation_page.middleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
			bCurrent = loadGpuRuntimePageSnapshot( validation_page );
		}
		if ( bCurrent ) {
			++unchanged;
			continue;
		}
		String cook_error;
		if ( writeGpuRuntimePage( pRegion, &cook_error ) ) {
			++cooked;
		} else {
			++failed;
			errors.push_back( "Region " + String( location ) + ": " +
				( cook_error.is_empty() ? String( "cook failed" ) : cook_error ) );
		}
	}

	PackedStringArray page_files = Util::getFiles( data_dir, "terrain3d*.t3dpage" );
	for ( const String &fname : page_files ) {
		const String source_name = fname.get_basename() + String( ".res" );
		const String source_path = data_dir.path_join( source_name );
		if ( FileAccess::file_exists( source_path ) ) {
			continue;
		}
		const String orphan_path = data_dir.path_join( fname );
		const Error remove_error = DirAccess::remove_absolute( orphan_path );
		if ( remove_error == OK ) {
			++orphan_removed;
		} else {
			++orphan_failed;
			errors.push_back( "Unable to remove orphan runtime page: " + orphan_path );
		}
	}

	const bool bFarAtlasCooked = writeCookedGpuFarAtlases();
	if ( !bFarAtlasCooked ) {
		errors.push_back( "Far atlas cook failed" );
	}
	Dictionary material_cache_result;
	bool bMaterialCacheCooked = true;
	if ( m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled() ) {
		const Ref<T3DAssets> pAssets = m_pTerrain->getAssets();
		if ( pAssets.is_valid() ) {
			material_cache_result = pAssets->cookRuntimeMaterialCache();
			bMaterialCacheCooked = ( bool ) ( material_cache_result.get( "complete", false ) );
			if ( !bMaterialCacheCooked ) {
				errors.push_back( "Runtime material cache cook failed: " +
					String( material_cache_result.get( "error", "unknown error" ) ) );
			}
		}
	}
	releaseUntrackedCpuMaps();
	const int usable = cooked + unchanged;
	result["data_directory"] = data_dir;
	result["far_atlas_cooked"] = bFarAtlasCooked;
	result["material_cache"] = material_cache_result;
	result["material_cache_cooked"] = bMaterialCacheCooked;
	result["preflight_failed"] = false;
	result["discovered"] = discovered;
	result["valid"] = valid;
	result["cooked"] = cooked;
	result["unchanged"] = unchanged;
	result["usable"] = usable;
	result["skipped"] = 0;
	result["failed"] = failed;
	result["orphan_removed"] = orphan_removed;
	result["orphan_failed"] = orphan_failed;
	result["region_count"] = m_nActiveRegionCount;
	result["complete"] = failed == 0 && usable == valid && valid == discovered &&
		orphan_failed == 0 && bFarAtlasCooked && bMaterialCacheCooked;
	result["errors"] = errors;
	return result;
}

Error T3DData::loadDirectory( const String &rDir ) {
	if ( rDir.is_empty() ) {
		LOG( T3DTerrain::ERROR, "Specified directory name is blank" );
		return ERR_INVALID_PARAMETER;
	}

	const uint64_t load_started_usec = Time::get_singleton()->get_ticks_usec();
	m_nStartupLoadTotalUsecLast = 0u;
	m_nStartupLoadEnumerationUsecLast = 0u;
	m_nStartupLoadDeserializeUsecLast = 0u;
	m_nStartupLoadStagingUsecLast = 0u;
	m_nStartupLoadPublishUsecLast = 0u;
	m_nStartupLoadFinalizeUsecLast = 0u;
	m_nStartupLoadRegionFilesLast = 0;
	m_nStartupLoadRegionsPublishedLast = 0;
	m_bStartupLoadSucceededLast = false;
	LOG( T3DTerrain::INFO, "Loading region files from ", rDir );
	PackedStringArray files = Util::getFiles( rDir, "terrain3d*.res" );
	files.sort();
	m_nStartupLoadEnumerationUsecLast = Time::get_singleton()->get_ticks_usec() - load_started_usec;
	m_nStartupLoadRegionFilesLast = files.size();
	if ( files.is_empty() ) {
		LOG( T3DTerrain::INFO, "No Terrain3D region files found in: ", rDir );
		return ERR_FILE_NOT_FOUND;
	}

	typedef struct t3dpendingRegion_s {
		Vector2i location;
		String path;
		Ref<T3DRegion> pRegion;
		PackedByteArray staged_height;
		int active_slot = -1;
	} t3dpendingRegion_t;
	eastl::vector<t3dpendingRegion_t> pending;
	pending.reserve( files.size() );
	eastl::bitset<TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE> occupied;
	int target_region_size = 0;
	const bool bBoundedCpuLoad = m_bCpuRegionCacheEnabled;

	ResourceLoader *const pLoader = ResourceLoader::get_singleton();
	LOG( T3DTerrain::INFO, "[terrain-data] Candidate repository: ", files.size(), " region files; CPU cache ",
		bBoundedCpuLoad ? "bounded" : "authoring/unbounded", "; GPU cache ",
		m_bGpuRegionCacheEnabled ? "enabled" : "disabled" );
	for ( const String &fname : files ) {
		t3dpendingRegion_t entry;
		entry.location = Util::filenameToLocation( fname );
		entry.path = rDir + String( "/" ) + fname;
		pending.push_back( entry );
	}

	// Region Resource property setters sanitize images, decode instance payloads,
	// and emit diagnostics through the GDExtension boundary. Keep directory
	// deserialization on the caller/main thread. Queuing the complete directory
	// through ResourceLoader worker subthreads made those setters execute
	// concurrently during editor authoring and was the only load path whose
	// threading policy changed when the CPU residency cache was disabled.
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 1/4: synchronous region deserialization and validation" );
	const uint64_t deserialize_started_usec = Time::get_singleton()->get_ticks_usec();

	// Phase 1: load and validate the complete candidate repository. The current
	// world remains untouched until every file is known to be compatible.
	for ( int file_index = 0; file_index < files.size(); ++file_index ) {
		const String fname = files[file_index];
		t3dpendingRegion_t &entry = pending[( size_t ) ( file_index )];
		const String &path = entry.path;
		const Vector2i location = entry.location;
		const int map_index = getRegionMapIndex( location );
		if ( map_index < 0 ) {
			LOG( T3DTerrain::ERROR, "Invalid or out-of-bounds region file name: ", fname );
			return ERR_FILE_CORRUPT;
		}
		if ( occupied.test( ( size_t ) ( map_index ) ) ) {
			LOG( T3DTerrain::ERROR, "Duplicate region location in directory: ", location );
			return ERR_ALREADY_EXISTS;
		}
		occupied.set( ( size_t ) ( map_index ) );

		Ref<T3DRegion> region =
			pLoader->load( path, "Terrain3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
		if ( region.is_null() ) {
			LOG( T3DTerrain::ERROR, "Cannot load region at ", path );
			return ERR_FILE_CORRUPT;
		}
		const int region_size = region->getRegionSize();
		if ( !isValidRegionSize( region_size ) ) {
			LOG( T3DTerrain::ERROR, "Invalid region size ", region_size, " in file: ", path );
			return ERR_FILE_CORRUPT;
		}
		const Ref<Image> pLoadedHeight = region->getHeightMap();
		if ( pLoadedHeight.is_valid() &&
			( pLoadedHeight->get_format() == Image::FORMAT_R16 || pLoadedHeight->get_format() == Image::FORMAT_RF ) &&
			!Terrain3DRegionCodec::hasValidHeightPayload( pLoadedHeight, region_size ) ) {
			LOG( T3DTerrain::ERROR, "Malformed height payload in file: ", path, ". Format: ", pLoadedHeight->get_format(),
				", bytes: ", pLoadedHeight->get_data().size() );
			return ERR_FILE_CORRUPT;
		}
		if ( target_region_size == 0 ) {
			target_region_size = region_size;
		} else if ( target_region_size != region_size ) {
			LOG( T3DTerrain::ERROR, "Region size mismatch. First loaded: ", target_region_size, " next: ", region_size,
				" in file: ", path );
			return ERR_FILE_CORRUPT;
		}
		const int target_map_size = TERRAIN3D_ACTIVE_WORLD_TEXELS / target_region_size;
		if ( !terrain3dRegionLocationValid( location, target_map_size ) ) {
			LOG( T3DTerrain::ERROR, "Region ", location, " is outside the fixed ", TERRAIN3D_ACTIVE_WORLD_TEXELS,
				"-texel world for region size ", target_region_size );
			return ERR_FILE_CORRUPT;
		}

		const bool bStorageUpgrade = region->getVersion() < TERRAIN3D_DATA_CURRENT_VERSION || pLoadedHeight.is_null() ||
			pLoadedHeight->get_format() != Image::FORMAT_R16 ||
			( region->getColorMap().is_valid() && region->getColorMap()->has_mipmaps() );
		// Resource property setters are mutation-oriented and may mark a freshly
		// deserialized resource dirty. Establish the on-disk state as the clean
		// baseline before applying explicit version/format normalization.
		region->setModified( false );
		region->markRuntimeMapsClean();
		region->setLocation( location );
		region->setVersion( TERRAIN3D_DATA_CURRENT_VERSION );
		region->setDeleted( false );
		region->setEdited( false );
		region->setVertexSpacing( m_fVertexSpacing );
		region->sanitizeMaps();
		region->setModified( bStorageUpgrade || region->isModified() );

		if ( bBoundedCpuLoad ) {
			const Ref<Image> pStagedHeight = region->getHeightMap();
			if ( pStagedHeight.is_null() || pStagedHeight->get_format() != Image::FORMAT_R16 ) {
				LOG( T3DTerrain::ERROR, "Unable to stage R16 height payload from: ", path );
				return ERR_FILE_CORRUPT;
			}
			const PackedByteArray height_bytes = pStagedHeight->get_data();
			const size_t region_texel_count = ( size_t ) ( target_region_size ) * ( size_t ) ( target_region_size );
			const size_t expected_height_bytes = region_texel_count * sizeof( uint16_t );
			if ( ( size_t ) ( height_bytes.size() ) < expected_height_bytes ) {
				LOG( T3DTerrain::ERROR, "Truncated staged height payload in: ", path );
				return ERR_FILE_CORRUPT;
			}
			const int half_map = target_map_size / 2;
			const int active_x = location.x + half_map;
			const int active_y = location.y + half_map;
			entry.active_slot = active_y * target_map_size + active_x;
			entry.staged_height = height_bytes;
			if ( !region->isModified() ) {
				region->releaseRuntimeMapsUnmodified();
			}
		}
		entry.pRegion = region;
	}

	m_nStartupLoadDeserializeUsecLast = Time::get_singleton()->get_ticks_usec() - deserialize_started_usec;
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 1/4 complete: validated ", pending.size(),
		" regions at size ", target_region_size );
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 2/4: staging replacement repository tables" );
	const uint64_t staging_started_usec = Time::get_singleton()->get_ticks_usec();

	// Phase 2: construct the complete replacement repository off to the side.
	// No current-world state is released until every slot, map entry, and resource
	// reference for the candidate repository is ready.
	Dictionary staged_regions;
	T3DRegion *pStagedRegionPtrs[TERRAIN3D_MAX_REGION_SLOTS] = {};
	Vector2i staged_region_locations[TERRAIN3D_MAX_REGION_SLOTS] = {};
	const int staged_region_location_count = ( int ) ( pending.size() );
	if ( staged_region_location_count > TERRAIN3D_MAX_REGION_SLOTS ) {
		LOG( T3DTerrain::ERROR, "Directory contains ", staged_region_location_count,
			" regions, exceeding the fixed region-slot capacity of ", TERRAIN3D_MAX_REGION_SLOTS );
		return ERR_OUT_OF_MEMORY;
	}
	uint16_t staged_region_map[TERRAIN3D_MAX_REGION_SLOTS] = {};

	for ( int slot = 0; slot < ( int ) ( pending.size() ); ++slot ) {
		t3dpendingRegion_t &entry = pending[( size_t ) ( slot )];
		const int map_index = getRegionMapIndex( entry.location );
		staged_regions[entry.location] = entry.pRegion;
		pStagedRegionPtrs[( size_t ) ( map_index )] = entry.pRegion.ptr();
		staged_region_locations[slot] = entry.location;
		staged_region_map[map_index] = slot + 1;
	}

	m_nStartupLoadStagingUsecLast = Time::get_singleton()->get_ticks_usec() - staging_started_usec;
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 2/4 complete: repository tables staged" );
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 3/4: publishing active storage and region ownership" );
	const uint64_t publish_started_usec = Time::get_singleton()->get_ticks_usec();

	// Phase 3: publish the complete validated repository in one state transition.
	// Generated textures and CPU map mirrors are rebuilt from that complete state
	// afterward; malformed input can no longer destroy the previous repository.
	if ( m_pResidentPool ) {
		m_pResidentPool->resetResidency();
	}
	const int previous_region_size = m_nRegionSize;
	bool bActiveStorageReady = false;
	if ( bBoundedCpuLoad ) {
		const uint8_t *pStagedHeightPages[TERRAIN3D_MAX_REGION_SLOTS] = {};
		int staged_height_slots[TERRAIN3D_MAX_REGION_SLOTS] = {};
		size_t staged_height_count = 0u;
		for ( const t3dpendingRegion_t &entry : pending ) {
			if ( entry.active_slot < 0 || entry.staged_height.is_empty() ) {
				LOG( T3DTerrain::ERROR, "Missing staged height payload for region ", entry.location );
				return ERR_FILE_CORRUPT;
			}
			pStagedHeightPages[staged_height_count] = entry.staged_height.ptr();
			staged_height_slots[staged_height_count] = entry.active_slot;
			++staged_height_count;
		}
		bActiveStorageReady = terrain3dActiveStorageReplaceHeightPages( this, target_region_size,
			pStagedHeightPages, staged_height_slots, staged_height_count );
		if ( bActiveStorageReady ) {
			bActiveStorageReady = setRegionSizeInternal( target_region_size );
		}
	} else {
		if ( !setRegionSizeInternal( target_region_size ) ) {
			return ERR_ALREADY_IN_USE;
		}
		// A same-size repository replacement does not pass through the reconfigure
		// clear path. Invalidate every old slot before publishing candidate regions
		// so omitted locations cannot retain stale process-static payloads.
		terrain3dActiveStorageReset( this );
		bActiveStorageReady = true;
		for ( t3dpendingRegion_t &entry : pending ) {
			if ( !syncActiveRegion( entry.location, entry.pRegion.ptr() ) ) {
				bActiveStorageReady = false;
				break;
			}
		}
	}
	if ( !bActiveStorageReady ) {
		if ( !bBoundedCpuLoad ) {
			setRegionSizeInternal( previous_region_size );
			for ( T3DRegion *const region : m_szRegionPtrMap ) {
				if ( region && !region->isDeleted() ) {
					syncActiveRegion( region->getLocation(), region );
				}
			}
		}
		LOG( T3DTerrain::ERROR, "Failed to populate process-static active terrain storage; previous repository restored" );
		return ERR_INVALID_DATA;
	}
	m_regions = staged_regions;
	std::memcpy( m_szRegionPtrMap, pStagedRegionPtrs, sizeof( m_szRegionPtrMap ) );
	std::memcpy( m_szRegionLocations, staged_region_locations,
			( size_t ) ( staged_region_location_count ) * sizeof( Vector2i ) );
	m_nRegionLocationCount = staged_region_location_count;
	std::memcpy( m_szRegionMap, staged_region_map, sizeof( m_szRegionMap ) );
	m_bRegionMapDirty = false;
	m_nFreeRegionSlotCount = 0;
	m_nActiveRegionCount = ( int ) ( pending.size() );
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_pDefaultHeightMap.unref();
	m_pDefaultControlMap.unref();
	m_pDefaultColorMap.unref();
	m_pDefaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_editedArea = AABB();
	m_masterHeightRange = V2_ZERO;
	m_generatedHeightMaps.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();

	// Resource-path takeover is intentionally deferred until the new repository
	// owns the candidate references. Existing active resources retain their path
	// metadata throughout validation and staging.
	for ( t3dpendingRegion_t &entry : pending ) {
		entry.pRegion->take_over_path( entry.path );
	}

	m_nStartupLoadPublishUsecLast = Time::get_singleton()->get_ticks_usec() - publish_started_usec;
	m_nStartupLoadRegionsPublishedLast = m_nActiveRegionCount;
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 3/4 complete: ", m_nActiveRegionCount,
		" active regions published; rebuilding residency and renderer resources" );
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 4/4: cache policy, GPU textures, and sampling state" );
	const uint64_t finalize_started_usec = Time::get_singleton()->get_ticks_usec();
	m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( target_region_size ) );
	configureResidentPool( 64 );
	configureCpuRegionCache( m_pTerrain->isCpuRegionCacheEffective(),
		m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(), m_pTerrain->getCpuRegionLoadBudgetLayers(),
		m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
		m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(), m_pTerrain->getInstanceRegionLoadBudgetPages(),
		m_pTerrain->getInstanceRegionLoadBudgetMb() );
	if ( m_bCpuRegionCacheEnabled ) {
		updateCpuRegionResidency( Vector3(), true );
	}
	updateMaps( T3DRegion::TYPE_MAX, true, false );
	invalidateSamplingCache();
	m_nStartupLoadFinalizeUsecLast = Time::get_singleton()->get_ticks_usec() - finalize_started_usec;
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 4/4 complete: terrain repository is ready for consumers" );
	if ( m_pTerrain->isInitialized() && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, true );
	}
	m_nStartupLoadTotalUsecLast = Time::get_singleton()->get_ticks_usec() - load_started_usec;
	m_bStartupLoadSucceededLast = true;
	const double fElapsedMs = ( double ) ( m_nStartupLoadTotalUsecLast ) / 1000.0;
	LOG( T3DTerrain::INFO, "Loaded ", m_nActiveRegionCount, " terrain regions (", m_nRegionLocationCount, " texture layers) in ",
		vformat( "%.2f", fElapsedMs ), " ms; synchronous Resource deserialize ",
		vformat( "%.2f", ( double ) ( m_nStartupLoadDeserializeUsecLast ) / 1000.0 ), " ms" );
	( void ) fElapsedMs;
	return OK;
}

// TODO have load_directory call load_region, or make a load_file that loads a specific path
void T3DData::loadRegion( const Vector2i &rRegionLoc, const String &rDir, const bool bUpdate ) {
	LOG( T3DTerrain::INFO, "Loading region from location ", rRegionLoc );
	const String path = rDir + String( "/" ) + Util::locationToFilename( rRegionLoc );
	if ( !FileAccess::file_exists( path ) ) {
		LOG( T3DTerrain::ERROR, "File ", path, " doesn't exist" );
		return;
	}
	Ref<T3DRegion> pRegion =
		ResourceLoader::get_singleton()->load( path, "Terrain3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
	if ( pRegion.is_null() ) {
		LOG( T3DTerrain::ERROR, "Cannot load region at ", path );
		return;
	}
	const Ref<Image> pLoadedHeightBeforeUpgrade = pRegion->getHeightMap();
	if ( pLoadedHeightBeforeUpgrade.is_valid() &&
		( pLoadedHeightBeforeUpgrade->get_format() == Image::FORMAT_R16 ||
		 pLoadedHeightBeforeUpgrade->get_format() == Image::FORMAT_RF ) &&
		!Terrain3DRegionCodec::hasValidHeightPayload( pLoadedHeightBeforeUpgrade, pRegion->getRegionSize() ) ) {
		LOG( T3DTerrain::ERROR, "Malformed height payload in file: ", path, ". Region was not published." );
		return;
	}
	if ( m_regions.is_empty() ) {
		m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t )pRegion->getRegionSize() );
	} else {
		if ( m_pTerrain->getRegionSize() != ( T3DTerrain::t3dregionSize_t )pRegion->getRegionSize() ) {
			LOG( T3DTerrain::ERROR, "Region size mismatch. First loaded: ", m_pTerrain->getRegionSize(),
				" next: ", pRegion->getRegionSize(), " in file: ", path );
			return;
		}
	}
	const Ref<Image> pLoadedHeight = pRegion->getHeightMap();
	const bool bStorageUpgrade = pRegion->getVersion() < TERRAIN3D_DATA_CURRENT_VERSION || pLoadedHeight.is_null() ||
		pLoadedHeight->get_format() != Image::FORMAT_R16 ||
		( pRegion->getColorMap().is_valid() && pRegion->getColorMap()->has_mipmaps() );
	pRegion->setModified( false );
	pRegion->markRuntimeMapsClean();
	pRegion->take_over_path( path );
	pRegion->setLocation( rRegionLoc );
	pRegion->setVersion( TERRAIN3D_DATA_CURRENT_VERSION ); // Sends upgrade warning if old version
	pRegion->sanitizeMaps();
	if ( addRegionInternal( pRegion, bUpdate, true ) == OK ) {
		pRegion->setModified( bStorageUpgrade || pRegion->isModified() );
		pRegion->setEdited( false );
		m_bCpuRegionCacheSelectionDirty = true;
	}
}

TypedArray<Image> T3DData::getMaps( const t3dmapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Specified map type out of range" );
		return TypedArray<Image>();
	}
	switch ( eMapType ) {
		case T3DRegion::TYPE_HEIGHT:
			return getHeightMaps();
			break;
		case T3DRegion::TYPE_CONTROL:
			return getControlMaps();
			break;
		case T3DRegion::TYPE_COLOR:
			return getColorMaps();
			break;
		default:
			break;
	}
	return TypedArray<Image>();
}

void T3DData::updateMaps( const t3dmapType_t eMapType, const bool bAllRegions, const bool bGenerateMipmaps ) {
	( void ) bGenerateMipmaps; // GPU publication builds transient mip chains; CPU maps stay base-level only.
	if ( isFarAtlasRequired() ) {
		markAllGpuFarRegionsDirty( eMapType );
	}
	if ( m_bGpuRegionCacheEnabled ) {
		updateMapsBounded( eMapType, bAllRegions );
		return;
	}

	const bool bImplicitStateChanged = refreshImplicitMapState();
	if ( bImplicitStateChanged && m_pResidentPool ) {
		configureResidentPool( 64 );
	}

	// A full rebuild also provisions spare texture-array layers. The spare layers
	// are the key to constant-time add/remove operations between rebuilds.
	const bool bSkipReserve = bAllRegions && m_bSkipSlotReserveOnce;
	const bool bSlotCapacityChanged = bAllRegions && !bSkipReserve && ensureRegionSlotReserve();
	if ( bSkipReserve ) {
		m_bSkipSlotReserveOnce = false;
	}
	if ( bAllRegions ) {
		LOG( T3DTerrain::EXTREME, "Marking dirty maps of type: ", eMapType );
		if ( bSlotCapacityChanged || eMapType == T3DRegion::TYPE_MAX ) {
			m_generatedHeightMaps.clear();
			m_generatedControlMaps.clear();
			m_generatedColorMaps.clear();
			m_generatedDerivedMaps.clear();
		} else {
			switch ( eMapType ) {
				case T3DRegion::TYPE_HEIGHT:
					m_generatedHeightMaps.clear();
					m_generatedDerivedMaps.clear();
					break;
				case T3DRegion::TYPE_CONTROL:
					m_generatedControlMaps.clear();
					break;
				case T3DRegion::TYPE_COLOR:
					m_generatedColorMaps.clear();
					break;
				default:
					break;
			}
		}
	}

	bool bAnyChanged = bImplicitStateChanged;

	if ( m_bRegionMapDirty ) {
		LOG( T3DTerrain::EXTREME, "Regenerating ", V2I( m_nRegionMapSize ), " active region map from fixed slot storage" );
		rebuildRegionMapFromSlots();
		bAnyChanged = true;
		if ( !m_bStartupBulkLoad ) {
			LOG( T3DTerrain::DEBUG, "Emitting region_map_changed" );
			emit_signal( "region_map_changed" );
		}
	}

	const bool bRebuildHeight = m_generatedHeightMaps.isDirty();
	const bool bRebuildControl = m_generatedControlMaps.isDirty();
	const bool bRebuildColor = m_generatedColorMaps.isDirty();
	const bool bRebuildDerived = m_generatedDerivedMaps.isDirty() || bRebuildHeight;

	// Full/startup rebuilds used to traverse the region slots once per map and
	// once again for the height range. Resolve each region once and populate all
	// requested arrays in a single pass; GPU array creation remains separate.
	if ( bRebuildHeight || bRebuildControl || bRebuildColor || bRebuildDerived ) {
		ensureDefaultMaps();
		const int slot_count = m_nRegionLocationCount;
		TypedArray<Image> gpu_control_maps;
		TypedArray<Image> gpu_color_maps;
		if ( bRebuildHeight ) {
			m_heightMaps.resize( slot_count );
			m_masterHeightRange = V2_ZERO;
		}
		if ( bRebuildControl ) {
			m_controlMaps.resize( slot_count );
			gpu_control_maps.resize( slot_count );
		}
		if ( bRebuildColor ) {
			m_colorMaps.resize( slot_count );
			gpu_color_maps.resize( slot_count );
		}
		if ( bRebuildDerived ) {
			m_derivedMaps.resize( slot_count );
			eastl::vector<Vector2i> missing;
			missing.reserve( ( size_t ) ( m_nActiveRegionCount ) );
			for ( int slot = 0; slot < slot_count; ++slot ) {
				const Vector2i location = m_szRegionLocations[slot];
				T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
				if ( pRegion && !pRegion->isDeleted() && !pRegion->hasValidDerivedNormalSlopeMap() ) {
					missing.push_back( location );
				}
			}
			regenerateDerivedMaps( missing );
		}

		for ( int slot = 0; slot < slot_count; ++slot ) {
			const Vector2i location = m_szRegionLocations[slot];
			const T3DRegion *pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
			const bool bValid = pRegion && !pRegion->isDeleted();
			if ( bRebuildHeight ) {
				m_heightMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, T3DRegion::TYPE_HEIGHT );
				if ( bValid ) {
					updateMasterHeights( pRegion->getHeightRange() );
				}
			}
			if ( bRebuildControl ) {
				m_controlMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, T3DRegion::TYPE_CONTROL );
				gpu_control_maps[slot] = bValid ? makeGpuRenderControlImage( location ) : m_pDefaultControlMap;
			}
			if ( bRebuildColor ) {
				m_colorMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, T3DRegion::TYPE_COLOR );
				gpu_color_maps[slot] = makeGpuColorImage( m_colorMaps[slot] );
			}
			if ( bRebuildDerived ) {
				const Ref<Image> pDerived = bValid ? pRegion->getDerivedNormalSlopeMap() : Ref<Image>();
				m_derivedMaps[slot] = pDerived.is_valid() ? pDerived : m_pDefaultDerivedMap;
			}
		}

		if ( bRebuildHeight ) {
			m_generatedHeightMaps.create( m_heightMaps );
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "height_maps_changed" );
			}
		}
		if ( bRebuildControl ) {
			if ( m_bControlMapsImplicit ) {
				const TypedArray<Image> implicit_control = {m_pDefaultControlMap};
				m_generatedControlMaps.create( implicit_control );
			} else {
				m_generatedControlMaps.create( gpu_control_maps );
			}
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "control_maps_changed" );
			}
		}
		if ( bRebuildColor ) {
			if ( m_bColorMapsImplicit ) {
				const TypedArray<Image> implicit_color = {makeGpuColorImage( m_pDefaultColorMap )};
				m_generatedColorMaps.create( implicit_color );
			} else {
				m_generatedColorMaps.create( gpu_color_maps );
			}
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "color_maps_changed" );
			}
		}
		if ( bRebuildDerived ) {
			m_generatedDerivedMaps.create( m_derivedMaps );
			bAnyChanged = true;
			if ( !IS_EDITOR ) {
				for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
					const Vector2i location = m_szRegionLocations[slot];
					T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
					if ( !pRegion || pRegion->isDeleted() ) {
						continue;
					}
					if ( m_pResidentPool ) {
						m_pResidentPool->syncDerived( location, pRegion );
					}
					pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
				}
				m_derivedMaps.clear();
				m_derivedMaps.resize( slot_count );
			}
		}
	}

	// If no map has been rebuilt, update only edited layers. Emit once per map
	// type rather than once per region to avoid multiplying downstream work.
	if ( !bAnyChanged ) {
		bool bHeightChanged = false;
		bool bControlChanged = false;
		bool bColorChanged = false;
		for ( int region_id = 0; region_id < m_nRegionLocationCount; ++region_id ) {
			const Vector2i region_loc = m_szRegionLocations[region_id];
			if ( region_loc == V2I_MAX ) {
				continue;
			}
			const T3DRegion *pRegion = getRegionPtr( region_loc );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->isEdited() ) {
				continue;
			}
			switch ( eMapType ) {
				case T3DRegion::TYPE_HEIGHT:
					m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), region_id );
					bHeightChanged = true;
					break;
				case T3DRegion::TYPE_CONTROL:
					m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
					bControlChanged = true;
					break;
				case T3DRegion::TYPE_COLOR:
					m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ), region_id );
					bColorChanged = true;
					break;
				default:
					m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), region_id );
					m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
					m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ), region_id );
					bHeightChanged = true;
					bControlChanged = true;
					bColorChanged = true;
					break;
			}
		}
		if ( !m_bStartupBulkLoad ) {
			if ( bHeightChanged ) {
				emit_signal( "height_maps_changed" );
			}
			if ( bControlChanged ) {
				emit_signal( "control_maps_changed" );
			}
			if ( bColorChanged ) {
				emit_signal( "color_maps_changed" );
			}
		}
	}

	if ( !m_bGpuRegionCacheEnabled && isFarAtlasRequired() ) {
		ensureFarAtlases();
	}

	if ( bAnyChanged && !m_bStartupBulkLoad ) {
		LOG( T3DTerrain::DEBUG, "Emitting maps_changed" );
		emit_signal( "maps_changed" );
		m_pTerrain->snap();
	}
}

void T3DData::updateMapRegions( const t3dmapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations ) {
	if ( rRegionLocations.empty() ) {
		return;
	}

	if ( refreshImplicitMapState() ) {
		updateMaps( eMapType, true, false );
		return;
	}

	eastl::vector<Vector2i> render_locations = rRegionLocations;
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX ) {
		// The compiled render-control field uses a two-texel source halo. A control
		// edit can therefore change the rendered transition on any immediate
		// neighboring region even though only the authored owner map was modified.
		for ( const Vector2i &source_location : rRegionLocations ) {
			for ( int y = -1; y <= 1; ++y ) {
				for ( int x = -1; x <= 1; ++x ) {
					const Vector2i dependency_location = source_location + Vector2i( x, y );
					if ( dependency_location == source_location ) {
						continue;
					}
					T3DRegion *const pDependencyRegion = getRegionPtr( dependency_location );
					if ( !pDependencyRegion || pDependencyRegion->isDeleted() ) {
						continue;
					}
					bool bAlreadyPresent = false;
					for ( const Vector2i &existing_location : render_locations ) {
						if ( existing_location == dependency_location ) {
							bAlreadyPresent = true;
							break;
						}
					}
					if ( !bAlreadyPresent ) {
						render_locations.push_back( dependency_location );
					}
				}
			}
		}
	}

	bool bHeightChanged = false;
	bool bControlChanged = false;
	bool bColorChanged = false;
	bool bPhysicalMappingChanged = false;
	for ( const Vector2i region_loc : render_locations ) {
		if ( isFarAtlasRequired() ) {
			markGpuFarRegionDirty( region_loc, eMapType );
		}
		T3DRegion *const pRegion = getRegionPtr( region_loc );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const int repository_slot = getRegionId( region_loc );
		if ( repository_slot < 0 ) {
			continue;
		}
		if ( m_bGpuRegionCacheEnabled ) {
			const int render_layer = findGpuRegionSlot( region_loc );
			if ( render_layer < 0 ) {
				m_bGpuRegionCacheSelectionDirty = true;
				continue;
			}
			bHeightChanged = bHeightChanged || eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX;
			if ( republishGpuRegionLocation( region_loc ) ) {
				bPhysicalMappingChanged = true;
			}
			continue;
		}

		switch ( eMapType ) {
			case T3DRegion::TYPE_HEIGHT:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), repository_slot );
				if ( pRegion->getDerivedNormalSlopeMap().is_valid() ) {
					m_derivedMaps[repository_slot] = pRegion->getDerivedNormalSlopeMap();
					m_generatedDerivedMaps.update( pRegion->getDerivedNormalSlopeMap(), repository_slot );
					if ( !IS_EDITOR ) {
						pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
						m_derivedMaps[repository_slot] = Ref<Image>();
					}
				}
				bHeightChanged = true;
				break;
			case T3DRegion::TYPE_CONTROL:
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
				bControlChanged = true;
				break;
			case T3DRegion::TYPE_COLOR:
				m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ),
					repository_slot );
				bColorChanged = true;
				break;
			default:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), repository_slot );
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
				m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ),
					repository_slot );
				if ( pRegion->getDerivedNormalSlopeMap().is_valid() ) {
					m_derivedMaps[repository_slot] = pRegion->getDerivedNormalSlopeMap();
					m_generatedDerivedMaps.update( pRegion->getDerivedNormalSlopeMap(), repository_slot );
					if ( !IS_EDITOR ) {
						pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
						m_derivedMaps[repository_slot] = Ref<Image>();
					}
				}
				bHeightChanged = true;
				bControlChanged = true;
				bColorChanged = true;
				break;
		}
	}

	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}
	if ( bHeightChanged ) {
		emit_signal( "height_maps_changed" );
	}
	if ( bControlChanged ) {
		emit_signal( "control_maps_changed" );
	}
	if ( bColorChanged ) {
		emit_signal( "color_maps_changed" );
	}
	if ( !m_bGpuRegionCacheEnabled && isFarAtlasRequired() ) {
		ensureFarAtlases();
	}
}

void T3DData::regenerateDerivedMaps(
	const eastl::vector<Vector2i> &rRegionLocations,
	const AABB &rEditedArea
) {
	if ( rRegionLocations.empty() || m_nRegionSize <= 0 || m_fVertexSpacing <= 0.0f ) {
		return;
	}


	eastl::vector<t3dderivedMapTask_t> tasks;
	tasks.reserve( rRegionLocations.size() );
	for ( const Vector2i &location : rRegionLocations ) {
		t3dderivedMapTask_t task;
		task.location = location;
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			tasks.push_back( std::move( task ) );
			continue;
		}
		task.available = true;
		task.dirty_rect = Rect2i( V2I_ZERO, V2I( m_nRegionSize ) );
		if ( pRegion->getDerivedNormalSlopeMap().is_valid() && rEditedArea.size.x > 0.0f &&
			rEditedArea.size.z > 0.0f ) {
			const Vector2 region_world_origin = Vector2( location ) * ( real_t ) ( m_nRegionSize ) * m_fVertexSpacing;
			const Vector2 editBegin( rEditedArea.position.x, rEditedArea.position.z );
			const Vector3 edit_end_3d = rEditedArea.get_end();
			const Vector2 editEnd( edit_end_3d.x, edit_end_3d.z );
			const Vector2 local_begin = ( editBegin - region_world_origin ) / m_fVertexSpacing;
			const Vector2 local_end = ( editEnd - region_world_origin ) / m_fVertexSpacing;
			const Vector2i pixel_begin = Vector2i( Math::floor( local_begin.x ), Math::floor( local_begin.y ) ) - V2I( 1 );
			const Vector2i pixel_end = Vector2i( Math::ceil( local_end.x ), Math::ceil( local_end.y ) ) + V2I( 1 );
			task.dirty_rect =
				Rect2i( pixel_begin, pixel_end - pixel_begin ).intersection( Rect2i( V2I_ZERO, V2I( m_nRegionSize ) ) );
		}
		tasks.push_back( std::move( task ) );
	}

	// Keep the established GPU path for a single substantial request. Multi-region
	// batches use the CPU worker/SIMD path so one synchronous GPU readback cannot
	// serialize the whole batch. All Godot compute interaction remains on the
	// calling thread.
	size_t eligible_task_count = 0u;
	size_t eligible_task_index = 0u;
	for ( size_t task_index = 0u; task_index < tasks.size(); ++task_index ) {
		const t3dderivedMapTask_t &task = tasks[task_index];
		if ( task.available && task.dirty_rect.size.x > 0 && task.dirty_rect.size.y > 0 ) {
			eligible_task_index = task_index;
			++eligible_task_count;
		}
	}
	T3DCompute *const pCompute = getComputeBackend();
	if ( pCompute && eligible_task_count == 1u ) {
		t3dderivedMapTask_t &task = tasks[eligible_task_index];
		const int width = task.dirty_rect.size.x;
		const int height = task.dirty_rect.size.y;
		const int halo_stride = width + 2;
		eastl::vector<float> halo( ( size_t ) ( halo_stride ) * ( size_t ) ( height + 2 ) );
		T3DSampler sampler( this );
		const int64_t region_base_x = ( int64_t ) ( task.location.x ) * m_nRegionSize;
		const int64_t region_base_z = ( int64_t ) ( task.location.y ) * m_nRegionSize;
		const int64_t halo_grid_x = region_base_x + task.dirty_rect.position.x - 1;
		const int64_t halo_grid_z = region_base_z + task.dirty_rect.position.y - 1;
		sampler.copyHeightRect( halo_grid_x, halo_grid_z, width + 2, height + 2, halo.data(), halo_stride, false );
		for ( int y = -1; y <= height; ++y ) {
			for ( int x = -1; x <= width; ++x ) {
				float &sample = halo[( size_t ) ( y + 1 ) * ( size_t ) ( halo_stride ) + ( size_t ) ( x + 1 )];
				if ( std::isfinite( sample ) ) {
					continue;
				}
				const int local_x = task.dirty_rect.position.x + x;
				const int local_y = task.dirty_rect.position.y + y;
				const int clamped_x = CLAMP( local_x, 0, m_nRegionSize - 1 );
				const int clamped_y = CLAMP( local_y, 0, m_nRegionSize - 1 );
				sample = sampler.sampleHeightGrid( region_base_x + clamped_x, region_base_z + clamped_y, false );
				sample = std::isfinite( sample ) ? sample : 0.0f;
			}
		}
		PackedByteArray rgba_output;
		if ( pCompute->generateDerivedMap( halo, width, height, ( float ) ( m_fVertexSpacing ), rgba_output ) &&
			rgba_output.size() >= ( int64_t ) ( width ) * ( int64_t ) ( height ) * 4 ) {
			const uint8_t *pRgba = rgba_output.ptr();
			const size_t texel_count = ( size_t ) ( width ) * ( size_t ) ( height );
			task.output.resize( texel_count * 2u );
			for ( size_t texel = 0u; texel < texel_count; ++texel ) {
				task.output[texel * 2u] = pRgba[texel * 4u];
				task.output[texel * 2u + 1u] = pRgba[texel * 4u + 2u];
			}
			task.success = true;
		}
	}

	const uint32_t hardware_threads = MAX( 1u, ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	const T3DParallelExecutor executor( ( int32_t ) ( MIN<size_t>( ( size_t ) ( hardware_threads ), tasks.size() ) ) );
	t3dderivedMapBuildContext_t build_context;
	build_context.data = this;
	build_context.pTasks = &tasks;
	build_context.nRegionSize = m_nRegionSize;
	build_context.fVertexSpacing = ( float ) ( m_fVertexSpacing );
	executor.runStatic( tasks.size(), 1u, terrain3dBuildDerivedMapRange, &build_context );

	for ( t3dderivedMapTask_t &task : tasks ) {
		if ( !task.available || task.dirty_rect.size.x <= 0 || task.dirty_rect.size.y <= 0 ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( task.location );
		if ( !pRegion || pRegion->isDeleted() || !task.success ) {
			if ( pRegion ) {
				pRegion->invalidateDerivedNormalSlopeMap();
			}
			continue;
		}
		PackedByteArray output;
		output.resize( ( int64_t ) ( task.output.size() ) );
		std::memcpy( output.ptrw(), task.output.data(), task.output.size() );
		const int width = task.dirty_rect.size.x;
		const int height = task.dirty_rect.size.y;
		const Ref<Image> pTile = Image::create_from_data( width, height, false, Image::FORMAT_RG8, output );
		if ( pTile.is_null() ) {
			pRegion->invalidateDerivedNormalSlopeMap();
			continue;
		}
		if ( task.dirty_rect.position == V2I_ZERO && task.dirty_rect.size == V2I( m_nRegionSize ) ) {
			pRegion->setDerivedNormalSlopeMapInternal( pTile );
		} else {
			Ref<Image> pDerived = pRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() || pDerived->get_width() != m_nRegionSize || pDerived->get_height() != m_nRegionSize ) {
				pRegion->invalidateDerivedNormalSlopeMap();
				continue;
			}
			pDerived->blit_rect( pTile, Rect2i( V2I_ZERO, task.dirty_rect.size ), task.dirty_rect.position );
			pRegion->setDerivedNormalSlopeMapInternal( pDerived );
		}
		const int repository_slot = getRegionId( task.location );
		const int render_layer = m_bGpuRegionCacheEnabled ? findGpuRegionSlot( task.location ) : repository_slot;
		const Ref<Image> pDerivedMap = pRegion->getDerivedNormalSlopeMap();
		TypedArray<Image> &derived_layers = m_bGpuRegionCacheEnabled ? m_gpuDerivedMaps : m_derivedMaps;
		if ( render_layer >= 0 && pDerivedMap.is_valid() && !m_generatedDerivedMaps.isDirty() &&
			render_layer < derived_layers.size() && render_layer < m_generatedDerivedMaps.size() ) {
			derived_layers[render_layer] = pDerivedMap;
			m_generatedDerivedMaps.update( pDerivedMap, render_layer );
		}
		if ( m_bOwnsActiveStorage ) {
			terrain3dActiveStorageSyncDerived( this, task.location, pRegion, task.dirty_rect );
		}
		if ( m_pResidentPool ) {
			m_pResidentPool->syncDerived( task.location, pRegion, task.dirty_rect );
		}
	}
}

void T3DData::commitMapEdits(
	const t3dmapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations,
	const AABB &rEditedArea, const uint32_t nFlags
) {
	if ( eMapType < T3DRegion::TYPE_HEIGHT || eMapType >= T3DRegion::TYPE_MAX || rRegionLocations.empty() ) {
		return;
	}

	m_activeLocationScratch.clear();
	m_activeLocationScratch.reserve( MIN<size_t>( rRegionLocations.size(), ( size_t ) ( m_nRegionMapSize ) * ( size_t ) ( m_nRegionMapSize ) ) );
	eastl::vector<Vector2i> &active_locations = m_activeLocationScratch;
	AABB editedArea = rEditedArea;
	real_t fEditedMinHeight = std::numeric_limits<real_t>::infinity();
	real_t fEditedMaxHeight = -std::numeric_limits<real_t>::infinity();
	for ( const Vector2i &location : rRegionLocations ) {
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || getRegionId( location ) < 0 ) {
			continue;
		}
		pRegion->markMapEdited( eMapType, true );
		if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_COLOR ) {
			pRegion->compactMapIfDefault( eMapType );
		}
		const Rect2i dirty_rect = getRegionDirtyRect( location, m_nRegionSize, m_fVertexSpacing, editedArea );
		if ( m_bOwnsActiveStorage &&
			!terrain3dActiveStorageSyncMap( this, location, pRegion, eMapType, dirty_rect ) ) {
			LOG( T3DTerrain::ERROR, "Failed to synchronize active terrain map for region ", location, " type ", eMapType );
			continue;
		}
		if ( m_pResidentPool ) {
			m_pResidentPool->syncMap( location, pRegion, eMapType, dirty_rect );
		}

		active_locations.push_back( location );
	}
	if ( active_locations.empty() ) {
		return;
	}
	if ( m_bGpuRegionCacheEnabled ) {
		for ( const Vector2i &location : active_locations ) {
			markGpuFarRegionDirty( location, eMapType );
		}
	}
	if ( eMapType == T3DRegion::TYPE_HEIGHT ) {
		// Derived normals use forward X/Z height differences. An edited height
		// sample therefore affects the sample at the same location plus the
		// immediately preceding sample on each axis. When an edit reaches a
		// region's first column/row, that predecessor lives in the left/top
		// neighboring region. Track only those actual dependencies instead of
		// rebuilding a 3x3 neighborhood for every brush update.
		m_derivedLocationScratch.clear();
		m_derivedLocationScratch.reserve( MIN<size_t>( active_locations.size() * 3u,
			( size_t ) ( m_nRegionMapSize ) * ( size_t ) ( m_nRegionMapSize ) ) );
		eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> derived_location_seen;
		eastl::bitset<TERRAIN3D_MAX_REGION_SLOTS> active_location_seen;
		for ( const Vector2i &location : active_locations ) {
			const int map_index = getRegionMapIndex( location );
			if ( map_index >= 0 ) {
				active_location_seen.set( ( size_t ) ( map_index ) );
			}

			eastl::array<Vector2i, 3> derived_candidates;
			int derived_candidate_count = 1;
			derived_candidates[0] = location;
			const Rect2i dirty_rect = getRegionDirtyRect( location, m_nRegionSize, m_fVertexSpacing, editedArea );
			if ( dirty_rect.position.x == 0 ) {
				derived_candidates[( size_t ) ( derived_candidate_count++ )] = location + Vector2i( -1, 0 );
			}
			if ( dirty_rect.position.y == 0 ) {
				derived_candidates[( size_t ) ( derived_candidate_count++ )] = location + Vector2i( 0, -1 );
			}

			for ( int candidate_index = 0; candidate_index < derived_candidate_count; ++candidate_index ) {
				const Vector2i &candidate = derived_candidates[( size_t ) ( candidate_index )];
				const int candidate_map_index = getRegionMapIndex( candidate );
				if ( candidate_map_index < 0 || derived_location_seen.test( ( size_t ) ( candidate_map_index ) ) ) {
					continue;
				}
				T3DRegion *const pCandidateRegion = getRegionPtr( candidate );
				if ( !pCandidateRegion || pCandidateRegion->isDeleted() ) {
					continue;
				}
				derived_location_seen.set( ( size_t ) ( candidate_map_index ) );
				m_derivedLocationScratch.push_back( candidate );
			}
		}
		regenerateDerivedMaps( m_derivedLocationScratch, editedArea );
		if ( isFarAtlasRequired() && m_pTerrain ) {
			const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
			if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
				// Runtime autoshader far-surface classification consumes both height and
				// derived slope. Dirty the exact derived dependency set so boundary edits
				// also refresh the predecessor region whose forward-difference normal changed.
				for ( const Vector2i &location : m_derivedLocationScratch ) {
					markGpuFarRegionDirty( location, T3DRegion::TYPE_COLOR );
				}
			}
		}
		for ( const Vector2i &location : m_derivedLocationScratch ) {
			const int map_index = getRegionMapIndex( location );
			if ( map_index >= 0 && !active_location_seen.test( ( size_t ) ( map_index ) ) ) {
				publishRegionSamplingGeneration( location );
			}
		}
		for ( const Vector2i &location : active_locations ) {
			if ( T3DRegion *const pRegion = getRegionPtr( location ) ) {
				const Vector2 range = pRegion->getHeightRange();
				fEditedMinHeight = MIN( fEditedMinHeight, range.x );
				fEditedMaxHeight = MAX( fEditedMaxHeight, range.y );
			}
		}
	}
	if ( editedArea.size.x <= 0.f || editedArea.size.z <= 0.f ) {
		bool bFirstRegion = true;
		for ( const Vector2i &location : active_locations ) {
			T3DRegion *const pRegion = getRegionPtr( location );
			if ( !pRegion ) {
				continue;
			}
			const Vector2 range = pRegion->getHeightRange();
			const real_t fWorldSize = pRegion->getRegionSize() * m_fVertexSpacing;
			const AABB regionArea(
				Vector3( location.x * fWorldSize, range.x, location.y * fWorldSize ),
				Vector3( fWorldSize, MAX( range.y - range.x, m_fVertexSpacing ), fWorldSize )
			);
			if ( bFirstRegion ) {
				editedArea = regionArea;
				bFirstRegion = false;
			} else {
				editedArea = editedArea.merge( regionArea );
			}
		}
	}

	// Height-map listeners rebuild mesh bounds synchronously. Publish region and
	// aggregate ranges before emitting the coalesced height-map notification.
	if ( eMapType == T3DRegion::TYPE_HEIGHT ) {
		calcHeightRange( false );
		if ( std::isfinite( fEditedMinHeight ) && std::isfinite( fEditedMaxHeight ) ) {
			editedArea.position.y = fEditedMinHeight;
			editedArea.size.y = MAX( fEditedMaxHeight - fEditedMinHeight, m_fVertexSpacing );
		}
	}
	// Publish only the edited region generations. Fallback PackedByteArray views
	// and resident leases for unrelated regions stay hot. The release publication
	// follows authoritative map writes and resident/derived synchronization.
	for ( const Vector2i &location : active_locations ) {
		publishRegionSamplingGeneration( location );
	}
	updateMapRegions( eMapType, active_locations );

	if ( ( nFlags & MAP_EDIT_KEEP_EDITED ) == 0u ) {
		for ( const Vector2i &location : active_locations ) {
			if ( T3DRegion *const pRegion = getRegionPtr( location ) ) {
				pRegion->setEdited( false );
			}
		}
	}
	if ( editedArea.size.x > 0.f && editedArea.size.z > 0.f ) {
		addEditedArea( editedArea );
	}

	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_INSTANCES ) != 0u && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateTransforms( editedArea );
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_COLLISION ) != 0u ) {
		T3DCollision *const pCollision = m_pTerrain->getCollision();
		if ( pCollision && pCollision->isEnabled() ) {
			if ( pCollision->isDynamicMode() ) {
				pCollision->update( V2I_MAX, true );
			} else {
				for ( const Vector2i &location : active_locations ) {
					pCollision->update( location, true );
				}
			}
		}
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_SNAP ) != 0u ) {
		m_pTerrain->snap();
	}
}

void T3DData::commitMapEdit(
	const t3dmapType_t eMapType, const Vector2i &rRegionLocation,
	const Rect2i &rDirtyRect
) {
	if ( eMapType < T3DRegion::TYPE_HEIGHT || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Invalid map type: ", eMapType );
		return;
	}
	T3DRegion *const pRegion = getRegionPtr( rRegionLocation );
	if ( !pRegion || pRegion->isDeleted() || getRegionId( rRegionLocation ) < 0 ) {
		LOG( T3DTerrain::ERROR, "No active region found at: ", rRegionLocation );
		return;
	}

	const int region_size = pRegion->getRegionSize();
	Rect2i dirty_rect = rDirtyRect;
	if ( dirty_rect.size.x <= 0 || dirty_rect.size.y <= 0 ) {
		dirty_rect = Rect2i( V2I_ZERO, V2I( region_size ) );
	} else {
		dirty_rect = dirty_rect.intersection( Rect2i( V2I_ZERO, V2I( region_size ) ) );
		if ( dirty_rect.size.x <= 0 || dirty_rect.size.y <= 0 ) {
			dirty_rect = Rect2i( V2I_ZERO, V2I( region_size ) );
		}
	}
	const real_t fSpacing = m_fVertexSpacing;
	const Vector2i pixel_origin = rRegionLocation * region_size + dirty_rect.position;
	const Vector2 height_range = pRegion->getHeightRange();
	const AABB editedArea(
		Vector3( pixel_origin.x * fSpacing, height_range.x, pixel_origin.y * fSpacing ),
		Vector3( MAX( dirty_rect.size.x * fSpacing, fSpacing ),
		MAX( height_range.y - height_range.x, fSpacing ),
		MAX( dirty_rect.size.y * fSpacing, fSpacing ) )
	);
	uint32_t flags = MAP_EDIT_SNAP;
	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_CONTROL ) {
		flags |= MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION;
	}
	commitMapEdits( eMapType, {rRegionLocation}, editedArea, flags );
}

Ref<T3DErosionJob> T3DData::createErosionJob( const Ref<T3DErosionSettings> &rSettings ) {
	Ref<T3DErosionJob> pJob;
	pJob.instantiate();
	if ( pJob->setup( this, rSettings ) != OK ) {
		return pJob;
	}
	return pJob;
}

void T3DData::setPixel( const t3dmapType_t eMapType, const Vector3 &rGlobalPosition, const Color &rPixel ) {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Specified map type out of range" );
		return;
	}
	const Vector2i region_loc = getRegionLocation( rGlobalPosition );
	if ( !ensureCpuRegionResident( region_loc ) ) {
		LOG( T3DTerrain::ERROR, "Unable to load CPU map payload for region: ", region_loc );
		return;
	}
	T3DRegion *const pRegion = getRegionPtr( region_loc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "No active region found at: ", rGlobalPosition );
		return;
	}
	if ( pRegion->isDeleted() ) {
		LOG( T3DTerrain::ERROR, "No active region found at: ", rGlobalPosition );
		return;
	}
	const Vector2i global_offset = region_loc * m_nRegionSize;
	const Vector3 descaled_pos = rGlobalPosition / m_fVertexSpacing;
	Vector2i img_pos = Vector2i( descaled_pos.x - global_offset.x, descaled_pos.z - global_offset.y );
	img_pos = img_pos.clamp( V2I_ZERO, V2I( m_nRegionSize - 1 ) );
	Image *const pMap = pRegion->getMapPtrForWrite( eMapType );
	if ( pMap ) {
		if ( eMapType == T3DRegion::TYPE_HEIGHT ) {
			Terrain3DRegionCodec::writeHeightPixel( pMap, img_pos, rPixel.r );
		} else {
			pMap->set_pixelv( img_pos, rPixel );
		}
		commitMapEdit( eMapType, region_loc, Rect2i( img_pos, Vector2i( 1, 1 ) ) );
	}
}

Color T3DData::getPixel( const t3dmapType_t eMapType, const Vector3 &rGlobalPosition ) const {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Specified map type out of range" );
		return COLOR_NAN;
	}
	const Vector2i region_loc = getRegionLocation( rGlobalPosition );
	if ( eMapType != T3DRegion::TYPE_HEIGHT && m_bCpuRegionCacheEnabled &&
		!const_cast<T3DData *>( this )->ensureCpuRegionResident( region_loc ) ) {
		return COLOR_NAN;
	}
	const T3DRegion *pRegion = getRegionPtr( region_loc );
	if ( !pRegion ) {
		return COLOR_NAN;
	}
	if ( pRegion->isDeleted() ) {
		return COLOR_NAN;
	}
	const Vector2i global_offset = region_loc * m_nRegionSize;
	const Vector3 descaled_pos = rGlobalPosition / m_fVertexSpacing;
	Vector2i img_pos = Vector2i( descaled_pos.x - global_offset.x, descaled_pos.z - global_offset.y );
	img_pos = img_pos.clamp( V2I_ZERO, V2I( m_nRegionSize - 1 ) );
	const T3DActiveMapLease active = acquireActiveMap( region_loc );
	if ( active.valid() ) {
		const t3dactiveMapView_t &view = active.view();
		const int index = img_pos.y * m_nRegionSize + img_pos.x;
		switch ( eMapType ) {
				case T3DRegion::TYPE_HEIGHT:
				return Color( Terrain3DRegionCodec::decodeHeightU16( view.pHeight[index] ), 0.0f, 0.0f, 1.0f );
			default:
				break;
		}
	}
	Image *pMap = pRegion->getMapPtr( eMapType );
	if ( !pMap ) {
		if ( eMapType == T3DRegion::TYPE_CONTROL ) {
			return COLOR_CONTROL;
		}
		if ( eMapType == T3DRegion::TYPE_COLOR ) {
			return COLOR_ROUGHNESS;
		}
		return COLOR_NAN;
	}
	if ( eMapType == T3DRegion::TYPE_HEIGHT ) {
		return Color( Terrain3DRegionCodec::readHeightPixel( pMap, img_pos ), 0.0f, 0.0f, 1.0f );
	}
	return pMap->get_pixelv( img_pos );
}

real_t T3DData::getHeight( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return NAN;
	}
	T3DSampler sampler( this );
	return sampler.sampleHeight( rGlobalPosition );
}

uint32_t T3DData::getControl( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 0 ) ) {
		return UINT32_MAX;
	}
	T3DSampler sampler( this );
	return sampler.sampleControl( rGlobalPosition );
}

PackedFloat32Array T3DData::getHeights( const PackedVector3Array &rGlobalPositions ) const {
	PackedFloat32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	T3DSampler sampler( this );
	if ( !m_bCpuRegionCacheEnabled ) {
		sampler.sampleHeights( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	float *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<T3DData *>( this )->ensureCpuSampleArea( position, 1 )
			? sampler.sampleHeight( position )
			: NAN;
	}
	return result;
}

PackedInt32Array T3DData::getControls( const PackedVector3Array &rGlobalPositions ) const {
	PackedInt32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	T3DSampler sampler( this );
	if ( !m_bCpuRegionCacheEnabled ) {
		sampler.sampleControls( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	int32_t *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<T3DData *>( this )->ensureCpuSampleArea( position, 0 )
			? ( int32_t ) ( sampler.sampleControl( position ) )
			: ( int32_t ) ( UINT32_MAX );
	}
	return result;
}

PackedVector3Array T3DData::getNormals( const PackedVector3Array &rGlobalPositions ) const {
	PackedVector3Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	T3DSampler sampler( this );
	if ( !m_bCpuRegionCacheEnabled ) {
		sampler.sampleNormals( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	Vector3 *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<T3DData *>( this )->ensureCpuSampleArea( position, 1 )
			? sampler.sampleNormal( position )
			: V3_NAN;
	}
	return result;
}

Vector3 T3DData::getNormal( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return V3_NAN;
	}
	T3DSampler sampler( this );
	return sampler.sampleNormal( rGlobalPosition );
}

bool T3DData::isInSlope(
	const Vector3 &rGlobalPosition, const Vector2 &rSlopeRange,
	const Vector3 &rNormal
) const {
	const Vector2 slope_range = CLAMP( rSlopeRange, V2_ZERO, V2( 90.f ) );
	if ( slope_range.y - slope_range.x > 89.99f ) {
		return true;
	}

	Vector3 slope_normal = rNormal;
	if ( !slope_normal.is_zero_approx() ) {
		slope_normal.normalize();
	} else {
		if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
			return false;
		}
		T3DSampler sampler( this );
		uint32_t control = UINT32_MAX;
		if ( !sampler.sampleControlAndSlopeNormal( rGlobalPosition, control, slope_normal ) ) {
			return false;
		}
	}

	const real_t fSlopeAngleDegrees = Math::rad_to_deg( Math::acos( CLAMP( slope_normal.dot( V3_UP ), -1.0f, 1.0f ) ) );
	return slope_range.x <= fSlopeAngleDegrees && fSlopeAngleDegrees <= slope_range.y;
}

/**
 * Returns:
 * X = base index
 * Y = overlay index
 * Z = percentage blend between X and Y. Limited to the fixed values in RANGE.
 * Interpretation of this data is up to the gamedev. Unfortunately due to blending, this isn't
 * pixel perfect. I would have your player print this location as you walk around to see how the
 * blending values look, then consider that the overlay texture is visible starting at a blend
 * value of .3-.5, otherwise it's the base texture.
 **/
Vector3 T3DData::getTextureId( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return V3_NAN;
	}
	T3DSampler sampler( this );
	const uint32_t control = sampler.sampleControl( rGlobalPosition );
	if ( control == UINT32_MAX || isHole( control ) ) {
		return V3_NAN;
	}

	// If material available, autoshader enabled, and pixel set to auto
	if ( m_pTerrain ) {
		const Ref<T3DMaterial> pTMaterial = m_pTerrain->getMaterial();
		const bool bControlAuto = isAuto( control );
		if ( pTMaterial.is_valid() && pTMaterial->getAutoShaderEnabled() && bControlAuto ) {
			const real_t fAutoSlope = ( real_t ) ( pTMaterial->getShaderParam( "auto_slope" ) );
			const real_t fAutoHeightReduction = ( real_t ) ( pTMaterial->getShaderParam( "auto_height_reduction" ) );
			float fHeight = NAN;
			Vector3 normal = V3_NAN;
			if ( !sampler.sampleHeightAndNormal( rGlobalPosition, fHeight, normal ) ) {
				return V3_NAN;
			}
			const uint32_t base_id = pTMaterial->getShaderParam( "auto_base_texture" );
			const uint32_t overlay_id = pTMaterial->getShaderParam( "auto_overlay_texture" );
			const real_t fBlend =
				CLAMP( ( fAutoSlope * 2.f * ( normal.y - 1.f ) + 1.f ) - fAutoHeightReduction * .01f * fHeight, 0.f, 1.f );
			return Vector3( ( real_t ) ( base_id ), ( real_t ) ( overlay_id ), fBlend );
		}
	}

	// Else, just get textures from control map
	const uint32_t base_id = getBase( control );
	const uint32_t overlay_id = getOverlay( control );
	const real_t fBlend = ( real_t ) ( getBlend( control ) ) / TERRAIN3D_BYTE_MAX_FLOAT;
	return Vector3( ( real_t ) ( base_id ), ( real_t ) ( overlay_id ), fBlend );
}

/**
 * Returns the location of a terrain vertex at a certain LOD. If there is a hole at the position, it returns
 * NAN in the vector's Y coordinate.
 * p_lod (0-8): Determines how many heights around the given global position will be sampled.
 * p_filter:
 *  HEIGHT_FILTER_NEAREST: Samples the height map at the exact coordinates given.
 *  HEIGHT_FILTER_MINIMUM: Samples (1 << p_lod) ** 2 heights around the given coordinates and returns the lowest.
 * p_global_position: X and Z coordinates of the vertex. Heights will be sampled around these coordinates.
 */
Vector3 T3DData::getMeshVertex(
	const int32_t nLod, const t3dheightFilter_t filter,
	const Vector3 &rGlobalPosition
) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, step / 2 + 1 ) ) {
		return Vector3( rGlobalPosition.x, NAN, rGlobalPosition.z );
	}
	real_t fHeight = 0.0f;
	T3DSampler sampler( this );

	switch ( filter ) {
		case HEIGHT_FILTER_NEAREST: {
			fHeight = sampler.sampleHeight( rGlobalPosition );
		} break;
		case HEIGHT_FILTER_MINIMUM: {
			const double fGridX = ( double ) ( rGlobalPosition.x ) / ( double ) ( m_fVertexSpacing );
			const double fGridZ = ( double ) ( rGlobalPosition.z ) / ( double ) ( m_fVertexSpacing );
			const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
			const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
			const double fWorldDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( m_fVertexSpacing );
			const double fWorldDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( m_fVertexSpacing );
			if ( fWorldDx * fWorldDx + fWorldDz * fWorldDz < 0.0001 ) {
				float fMinimum = NAN;
				fHeight = sampler.minimumHeightRect( rounded_x - step / 2, rounded_z - step / 2, step, step,
					fMinimum, true )
					? fMinimum
					: NAN;
				break;
			}

			// Preserve bilinear sampling semantics for callers that request a minimum
			// filter at a fractional grid position. Mesh and navigation bake callers
			// use the grid-aligned bulk path above.
			fHeight = sampler.sampleHeight( rGlobalPosition );
			bool bInvalid = std::isnan( fHeight );
			for ( int32_t dz = -step / 2; dz < step / 2 && !bInvalid; ++dz ) {
				for ( int32_t dx = -step / 2; dx < step / 2; ++dx ) {
					const Vector3 position = rGlobalPosition + Vector3( dx, 0.f, dz ) * m_fVertexSpacing;
					const real_t fSample = sampler.sampleHeight( position );
					if ( std::isnan( fSample ) ) {
						bInvalid = true;
						break;
					}
					fHeight = MIN( fHeight, fSample );
				}
			}
			if ( bInvalid ) {
				fHeight = NAN;
			}
		} break;
	}
	return Vector3( rGlobalPosition.x, fHeight, rGlobalPosition.z );
}

void T3DData::addEditedArea( const AABB &rArea ) {
	if ( m_editedArea.has_surface() ) {
		m_editedArea = m_editedArea.merge( rArea );
	} else {
		m_editedArea = rArea;
	}
	LOG( T3DTerrain::DEBUG, "Emitting maps_edited" );
	emit_signal( "maps_edited", rArea );
}

// Recalculates master height range from all active regions current height ranges
// Recursive mode has all regions to recalculate from each heightmap pixel
void T3DData::calcHeightRange( const bool bRecursive ) {
	m_masterHeightRange = V2_ZERO;
	for ( int slot = 0; slot < m_nRegionLocationCount; ++slot ) {
		const Vector2i region_loc = m_szRegionLocations[slot];
		T3DRegion *pRegion = getRegionPtr( region_loc );
		if ( !pRegion ) {
			continue;
		}
		if ( bRecursive ) {
			if ( !ensureCpuRegionResident( region_loc ) ) {
				LOG( T3DTerrain::ERROR, "Unable to page region for recursive height-range calculation: ", region_loc );
				continue;
			}
			pRegion = getRegionPtr( region_loc );
			pRegion->calcHeightRange();
		}
		updateMasterHeights( pRegion->getHeightRange() );
	}
	LOG( T3DTerrain::EXTREME, "Accumulated height range for all regions: ", m_masterHeightRange );
}

/**
 * Imports an Image set (Height, Control, Color) into Terrain3DData
 * It does NOT normalize values to 0-1. You must do that using get_min_max() and adjusting scale and offset.
 * Parameters:
 *	p_images - t3dmapType_t.TYPE_MAX sized array of Images for Height, Control, Color. Images can be blank or null
 *	p_global_position - X,0,Z location on the region map. Valid range is ~ (+/-8192, +/-8192)
 *	p_offset - Add this factor to all height values, can be negative
 *	p_scale - Scale all height values by this factor (applied after offset)
 */
void T3DData::importImages(
	const TypedArray<Image> &rImages, const Vector3 &rGlobalPosition,
	const real_t fOffset, const real_t fScale
) {
	IS_INIT_MESG( "Data not initialized", VOID );
	if ( rImages.size() != T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "p_images.size( ) is ", rImages.size(), ". It should be ", T3DRegion::TYPE_MAX,
			" even if some Images are blank or null" );
		return;
	}

	Vector2i img_size = V2I_ZERO;
	for ( int i = 0; i < T3DRegion::TYPE_MAX; i++ ) {
		const Ref<Image> pImg = rImages[i];
		if ( pImg.is_valid() && !pImg->is_empty() ) {
			LOG( T3DTerrain::INFO, "Importing image type ", TERRAIN3D_REGION_MAP_TYPE_NAMES[i], ", size: ", pImg->get_size(),
				", format: ", pImg->get_format() );
			if ( i == T3DRegion::TYPE_HEIGHT ) {
				LOG( T3DTerrain::INFO, "Applying offset: ", fOffset, ", scale: ", fScale );
			}
			if ( img_size == V2I_ZERO ) {
				img_size = pImg->get_size();
			} else if ( img_size != pImg->get_size() ) {
				LOG( T3DTerrain::ERROR, "Included Images in p_images have different dimensions. Aborting import" );
				return;
			}
		}
	}
	if ( img_size == V2I_ZERO ) {
		LOG( T3DTerrain::ERROR, "All images are empty. Nothing to import" );
		return;
	}

	const Vector3 descaled_position = rGlobalPosition / m_fVertexSpacing;
	const int max_dimension = m_nRegionSize * m_nRegionMapSize / 2;
	if ( ( std::abs( descaled_position.x ) > max_dimension ) || ( std::abs( descaled_position.z ) > max_dimension ) ) {
		LOG( T3DTerrain::ERROR, "Specify a position within +/-", Vector3( max_dimension, 0.f, max_dimension ) * m_fVertexSpacing );
		return;
	}
	if ( ( descaled_position.x + img_size.x > max_dimension ) || ( descaled_position.z + img_size.y > max_dimension ) ) {
		LOG( T3DTerrain::ERROR, img_size, " image will not fit at ", rGlobalPosition, ". Try ",
			-( img_size * m_fVertexSpacing ) / 2.f, " to center, or increase region_size" );
		return;
	}

	TypedArray<Image> src_images;
	src_images.resize( T3DRegion::TYPE_MAX );

	for ( int i = 0; i < T3DRegion::TYPE_MAX; i++ ) {
		const Ref<Image> pImg = rImages[i];
		if ( pImg.is_null() ) {
			src_images[i] = pImg;
			continue;
		}
		Ref<Image> pReadable = pImg;
		if ( pReadable->is_compressed() ) {
			pReadable = pReadable->duplicate();
			if ( pReadable->decompress() != OK ) {
				LOG( T3DTerrain::ERROR, "Unable to decompress imported ", TERRAIN3D_REGION_MAP_TYPE_NAMES[i], " image" );
				return;
			}
		}
		src_images[i] = pReadable;

		// Apply scale and offsets to the heightmap and filter out invalid data
		if ( i == T3DRegion::TYPE_HEIGHT ) {
			LOG( T3DTerrain::DEBUG, "Creating new temp image to adjust scale: ", fScale, " offset: ", fOffset );
			Ref<Image> pNewimg = Image::create_empty( pReadable->get_size().x, pReadable->get_size().y, false, Image::FORMAT_RF );
			for ( int y = 0; y < pReadable->get_height(); y++ ) {
				for ( int x = 0; x < pReadable->get_width(); x++ ) {
					// Import sources retain their ordinary normalized/channel semantics.
					// Only region-owned R16 images use the fixed Terrain3D world-height codec.
					const float fSourceHeight = pReadable->get_pixel( x, y ).r;
					const float fHeight = std::isfinite( fSourceHeight ) ? fSourceHeight * fScale + fOffset : fOffset;
					pNewimg->set_pixel( x, y, Color( fHeight, 0.0f, 0.0f, 1.0f ) );
				}
			}
			src_images[i] = pNewimg;
		}
	}

	// Calculate regions this image will span
	const int img_start_x = ( int )Math::floor( descaled_position.x );
	const int img_start_z = ( int )Math::floor( descaled_position.z );
	const int img_end_x = img_start_x + img_size.x - 1;
	const int img_end_z = img_start_z + img_size.y - 1;

	int start_region_x = ( int )Math::floor( ( real_t ) ( img_start_x ) / ( real_t ) ( m_nRegionSize ) );
	int start_region_z = ( int )Math::floor( ( real_t ) ( img_start_z ) / ( real_t ) ( m_nRegionSize ) );
	int end_region_x = ( int )Math::floor( ( real_t ) ( img_end_x ) / ( real_t ) ( m_nRegionSize ) );
	int end_region_z = ( int )Math::floor( ( real_t ) ( img_end_z ) / ( real_t ) ( m_nRegionSize ) );

	// Clamp region indices to valid range
	const int half_region_map = m_nRegionMapSize / 2;
	start_region_x = CLAMP( start_region_x, -half_region_map, half_region_map - 1 );
	start_region_z = CLAMP( start_region_z, -half_region_map, half_region_map - 1 );
	end_region_x = CLAMP( end_region_x, -half_region_map, half_region_map - 1 );
	end_region_z = CLAMP( end_region_z, -half_region_map, half_region_map - 1 );

	LOG( T3DTerrain::DEBUG, "Image spans regions (", start_region_x, ",", start_region_z, ") to (", end_region_x, ",", end_region_z,
		")" );

	bool bGenerateMipmaps = false;
	for ( int rz = start_region_z; rz <= end_region_z; rz++ ) {
		for ( int rx = start_region_x; rx <= end_region_x; rx++ ) {
			const Vector2i region_loc = Vector2i( rx, rz );

			const int region_start_x = rx * m_nRegionSize;
			const int region_start_z = rz * m_nRegionSize;
			const int region_end_x = region_start_x + m_nRegionSize - 1;
			const int region_end_z = region_start_z + m_nRegionSize - 1;

			const int overlap_start_x = MAX( region_start_x, img_start_x );
			const int overlap_start_z = MAX( region_start_z, img_start_z );
			const int overlap_end_x = MIN( region_end_x, img_end_x );
			const int overlap_end_z = MIN( region_end_z, img_end_z );

			// Skip if no overlap
			if ( overlap_end_x < overlap_start_x || overlap_end_z < overlap_start_z ) {
				continue;
			}

			const int copy_width = overlap_end_x - overlap_start_x + 1;
			const int copy_height = overlap_end_z - overlap_start_z + 1;

			const int src_x = overlap_start_x - img_start_x;
			const int src_z = overlap_start_z - img_start_z;
			const int dst_x = overlap_start_x - region_start_x;
			const int dst_z = overlap_start_z - region_start_z;

			LOG( T3DTerrain::DEBUG, "Region ", region_loc, ": copying ", Vector2i( copy_width, copy_height ), " from img(", src_x, ",",
				src_z, ") to region(", dst_x, ",", dst_z, ")" );

			Ref<T3DRegion> pRegion = getRegion( region_loc );
			if ( pRegion.is_null() ) {
				pRegion.instantiate();
				pRegion->setLocation( region_loc );
				pRegion->setRegionSize( m_nRegionSize );
				pRegion->setVertexSpacing( m_fVertexSpacing );
				addRegion( pRegion, false );
			} else if ( !pRegion->isDeleted() && !ensureCpuRegionResident( region_loc ) ) {
				LOG( T3DTerrain::ERROR, "Unable to page existing CPU terrain maps for image import at ", region_loc );
				return;
			} else if ( pRegion->isDeleted() ) {
				pRegion->clear();
				pRegion->setLocation( region_loc );
				pRegion->setRegionSize( m_nRegionSize );
				pRegion->setVertexSpacing( m_fVertexSpacing );
			}
			for ( int i = 0; i < T3DRegion::TYPE_MAX; i++ ) {
				const Ref<Image> pImg = src_images[i];
				if ( pImg.is_valid() && !pImg->is_empty() ) {
					Ref<Image> pRegionMap;

					const Ref<Image> pExistingMap = pRegion->getMap( ( t3dmapType_t ) ( i ) );
					if ( i == T3DRegion::TYPE_HEIGHT ) {
						pRegionMap = Image::create_empty( m_nRegionSize, m_nRegionSize, false, Image::FORMAT_RF );
						if ( pExistingMap.is_valid() && !pExistingMap->is_empty() ) {
							for ( int y = 0; y < m_nRegionSize; ++y ) {
								for ( int x = 0; x < m_nRegionSize; ++x ) {
									const float fHeight = Terrain3DRegionCodec::readHeightPixel( *pExistingMap, Vector2i( x, y ) );
									pRegionMap->set_pixel( x, y, Color( fHeight, 0.0f, 0.0f, 1.0f ) );
								}
							}
						} else {
							pRegionMap->fill( Color( 0.0f, 0.0f, 0.0f, 1.0f ) );
						}
					} else if ( pExistingMap.is_valid() && !pExistingMap->is_empty() ) {
						pRegionMap.instantiate();
						pRegionMap->copy_from( pExistingMap );
						if ( pRegionMap->get_format() != pImg->get_format() ) {
							pRegionMap->convert( pImg->get_format() );
						}
					} else {
						pRegionMap = Util::getFilledImage( m_regionSizev, TERRAIN3D_REGION_MAP_DEFAULT_COLORS[i], false, pImg->get_format() );
					}
					pRegionMap->blit_rect( pImg, Rect2i( src_x, src_z, copy_width, copy_height ), Vector2i( dst_x, dst_z ) );
					pRegion->setMap( ( t3dmapType_t ) ( i ), pRegionMap );
					if ( i == T3DRegion::TYPE_COLOR ) {
						bGenerateMipmaps = true;
					}
				}
			}
			pRegion->setModified( true );
			pRegion->sanitizeMaps();
		}
	}
	updateMaps( T3DRegion::TYPE_MAX, true, bGenerateMipmaps );
}

/** Exports a specified map as one of r16/raw, exr, jpg, png, webp, res, tres
 * r16 or exr are recommended for roundtrip external editing
 * r16 can be edited by Krita, however you must know the dimensions and min/max before reimporting
 * res/tres stores in Godot's native format.
 */
Error T3DData::exportImage( const String &rFileName, const t3dmapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Invalid map type specified: ", eMapType, " max: ", T3DRegion::TYPE_MAX - 1 );
		return FAILED;
	}

	if ( rFileName.is_empty() ) {
		LOG( T3DTerrain::ERROR, "No file specified. Nothing to export" );
		return FAILED;
	}

	if ( getRegionCount() == 0 ) {
		LOG( T3DTerrain::ERROR, "No valid regions. Nothing to export" );
		return FAILED;
	}

	// Simple file name validation
	static const String s_badChars = "?*|%<>\"";
	for ( int i = 0; i < s_badChars.length(); ++i ) {
		for ( int j = 0; j < rFileName.length(); ++j ) {
			if ( s_badChars[i] == rFileName[j] ) {
				LOG( T3DTerrain::ERROR, "Invalid file path '" + rFileName + "'" );
				return FAILED;
			}
		}
	}

	// Update path delimeter
	String file_name = rFileName.replace( "\\", "/" );

	// Check if p_file_name has a path and prepend "res://" if not
	bool bIsSimpleFilename = true;
	for ( int i = 0; i < file_name.length(); ++i ) {
		const char32_t c = file_name[i];
		if ( c == '/' || c == ':' ) {
			bIsSimpleFilename = false;
			break;
		}
	}
	if ( bIsSimpleFilename ) {
		file_name = "res://" + file_name;
	}

	// Check if the file can be opened for writing
	Ref<FileAccess> pFileRef = FileAccess::open( file_name, FileAccess::ModeFlags::WRITE );
	if ( pFileRef.is_null() ) {
		LOG( T3DTerrain::ERROR, "Cannot open file '" + file_name + "' for writing" );
		return FAILED;
	}
	pFileRef->close();

	// Filename is validated. Begin export image generation
	Ref<Image> pImg = layeredToImage( eMapType );
	if ( pImg.is_null() || pImg->is_empty() ) {
		LOG( T3DTerrain::ERROR, "Cannot create an export image for map type: ", TERRAIN3D_REGION_MAP_TYPE_NAMES[eMapType] );
		return FAILED;
	}

	String ext = file_name.get_extension().to_lower();
	LOG( T3DTerrain::MESG, "Saving ", pImg->get_size(), " sized ", TERRAIN3D_REGION_MAP_TYPE_NAMES[eMapType], " map in format ", pImg->get_format(), " as ",
		ext, " to: ", file_name );
	const Vector2i minmax = Util::getMinMax( pImg );
	LOG( T3DTerrain::MESG, "Minimum height: ", minmax.x, ", Maximum height: ", minmax.y );
	if ( ext == "r16" || ext == "raw" ) {
		Ref<FileAccess> pFile = FileAccess::open( file_name, FileAccess::WRITE );
		const real_t fHeightMin = minmax.x;
		const real_t fHeightMax = minmax.y;
		const real_t fHscale = 65535.0 / ( fHeightMax - fHeightMin );
		for ( int y = 0; y < pImg->get_height(); y++ ) {
			for ( int x = 0; x < pImg->get_width(); x++ ) {
				int h = ( int ) ( ( pImg->get_pixel( x, y ).r - fHeightMin ) * fHscale );
				h = CLAMP( h, 0, 65535 );
				pFile->store_16( h );
			}
		}
		return pFile->get_error();
	} else if ( ext == "exr" ) {
		return pImg->save_exr( file_name, ( eMapType == T3DRegion::TYPE_HEIGHT ) ? true : false );
	} else if ( ext == "png" ) {
		return pImg->save_png( file_name );
	} else if ( ext == "jpg" ) {
		return pImg->save_jpg( file_name );
	} else if ( ext == "webp" ) {
		return pImg->save_webp( file_name );
	} else if ( ( ext == "res" ) || ( ext == "tres" ) ) {
		return ResourceSaver::get_singleton()->save( pImg, file_name, ResourceSaver::FLAG_COMPRESS );
	}

	LOG( T3DTerrain::ERROR, "No recognized file type. See docs for valid extensions" );
	return FAILED;
}

Ref<Image> T3DData::layeredToImage( const t3dmapType_t eMapType ) const {
	LOG( T3DTerrain::INFO, "Generating a full sized image for all regions including empty regions" );
	t3dmapType_t map_type = eMapType;
	if ( map_type >= T3DRegion::TYPE_MAX ) {
		map_type = T3DRegion::TYPE_HEIGHT;
	}
	const TypedArray<Vector2i> active_locations = getRegionLocations();
	if ( active_locations.is_empty() ) {
		return Util::getFilledImage(
			m_regionSizev, TERRAIN3D_REGION_MAP_DEFAULT_COLORS[map_type], false,
			map_type == T3DRegion::TYPE_HEIGHT ? Image::FORMAT_RF : TERRAIN3D_REGION_MAP_FORMATS[map_type]
		);
	}

	Vector2i top_left = active_locations[0];
	Vector2i bottom_right = active_locations[0];
	for ( const Vector2i region_loc : active_locations ) {
		LOG( T3DTerrain::DEBUG, "Region location: ", region_loc );
		top_left.x = MIN( top_left.x, region_loc.x );
		top_left.y = MIN( top_left.y, region_loc.y );
		bottom_right.x = MAX( bottom_right.x, region_loc.x );
		bottom_right.y = MAX( bottom_right.y, region_loc.y );
	}

	LOG( T3DTerrain::DEBUG, "Full range to cover all regions: ", top_left, " to ", bottom_right );
	const Vector2i img_size = Vector2i( 1 + bottom_right.x - top_left.x, 1 + bottom_right.y - top_left.y ) * m_nRegionSize;
	LOG( T3DTerrain::DEBUG, "Image size: ", img_size );
	Ref<Image> pImg = Util::getFilledImage( img_size, TERRAIN3D_REGION_MAP_DEFAULT_COLORS[map_type], false,
		map_type == T3DRegion::TYPE_HEIGHT ? Image::FORMAT_RF : TERRAIN3D_REGION_MAP_FORMATS[map_type] );

	for ( const Vector2i region_loc : active_locations ) {
		const Vector2i img_location = ( region_loc - top_left ) * m_nRegionSize;
		LOG( T3DTerrain::DEBUG, "Region to blit: ", region_loc, " Export image coords: ", img_location );
		const T3DRegion *pRegion = getRegionPtr( region_loc );
		if ( !pRegion ) {
			continue;
		}
		if ( m_bCpuRegionCacheEnabled && !const_cast<T3DData *>( this )->ensureCpuRegionResident( region_loc ) ) {
			LOG( T3DTerrain::ERROR, "Unable to page region for image export: ", region_loc );
			continue;
		}
		pRegion = getRegionPtr( region_loc );
		const Ref<Image> pSource = pRegion->getMap( map_type );
		if ( map_type == T3DRegion::TYPE_HEIGHT && pSource.is_valid() ) {
			for ( int y = 0; y < m_nRegionSize; ++y ) {
				for ( int x = 0; x < m_nRegionSize; ++x ) {
					pImg->set_pixel( img_location.x + x, img_location.y + y,
						Color( Terrain3DRegionCodec::readHeightPixel( *pSource, Vector2i( x, y ) ), 0.0f, 0.0f, 1.0f ) );
				}
			}
		} else {
			const Ref<Image> pFallback = getRegionMapOrDefault( pRegion, map_type );
			if ( pFallback.is_valid() ) {
				pImg->blit_rect( pFallback, Rect2i( V2I_ZERO, m_regionSizev ), img_location );
			}
		}
	}
	return pImg;
}

void T3DData::dump( const bool bVerbose ) const {
	const TypedArray<Vector2i> region_slots = getRegionSlots();
	LOG( T3DTerrain::MESG, "_region_locations (", m_nRegionLocationCount, "): ", region_slots );
	const Array keys = m_regions.keys();
	LOG( T3DTerrain::MESG, "_regions (", keys.size(), "):" );
	for ( const Vector2i region_loc : keys ) {
		const T3DRegion *pRegion = getRegionPtr( region_loc );
		if ( !pRegion ) {
			LOG( T3DTerrain::WARN, "No region found at: ", region_loc );
			continue;
		}
		pRegion->dump( bVerbose );
	}
	if ( bVerbose ) {
		for ( int i = 0; i < TERRAIN3D_MAX_REGION_SLOTS; i++ ) {
			if ( m_szRegionMap[i] ) {
				LOG( T3DTerrain::MESG, "Region map array index: ", i, " / ", TERRAIN3D_MAX_REGION_SLOTS - 1,
					", Region id: ", m_szRegionMap[i] );
			}
		}
		Util::dumpMaps( m_heightMaps, "Height maps" );
		Util::dumpGentex( m_generatedHeightMaps, "height" );
		Util::dumpMaps( m_controlMaps, "Control maps" );
		Util::dumpGentex( m_generatedControlMaps, "control" );
		Util::dumpMaps( m_colorMaps, "Color maps" );
		Util::dumpGentex( m_generatedColorMaps, "color" );
	}
}

///////////////////////////
// Protected Functions
///////////////////////////

void T3DData::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_NEAREST );
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_MINIMUM );

	ClassDB::bind_integer_constant( get_class_static(), "", "WORLD_TEXEL_SIZE", TERRAIN3D_ACTIVE_WORLD_TEXELS );
	ClassDB::bind_integer_constant( get_class_static(), "", "REGION_MAP_SIZE", TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE );

	ClassDB::bind_method( D_METHOD( "get_region_count" ), &T3DData::getRegionCount );
	ClassDB::bind_method( D_METHOD( "set_region_locations", "region_locations" ), &T3DData::setRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_region_locations" ), &T3DData::getRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_render_region_slots" ), &T3DData::getRenderRegionSlots );
	ClassDB::bind_method( D_METHOD( "get_regions_active", "copy", "deep" ), &T3DData::getRegionsActive,
						 DEFVAL( false ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_regions_all" ), &T3DData::getRegionsAll );
	ClassDB::bind_method( D_METHOD( "get_region_map" ), &T3DData::getRegionMap );
	ClassDB::bind_method( D_METHOD( "get_region_map_size" ), &T3DData::getRegionMapSize );
	ClassDB::bind_method( D_METHOD( "is_runtime_ready" ), &T3DData::isRuntimeReady );
	ClassDB::bind_method( D_METHOD( "get_active_storage_generation" ), &T3DData::getActiveStorageGeneration );
	ClassDB::bind_method( D_METHOD( "get_runtime_state" ), &T3DData::getRuntimeState );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &T3DData::getGpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_middle_height_cache_stats" ), &T3DData::getMiddleHeightCacheStats );
	ClassDB::bind_method( D_METHOD( "get_startup_load_stats" ), &T3DData::getStartupLoadStats );
	ClassDB::bind_method( D_METHOD( "get_runtime_residency_grid", "radius" ),
		&T3DData::getRuntimeResidencyGrid, DEFVAL( 6 ) );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &T3DData::getCpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ),
		&T3DData::getInstanceRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "ensure_instance_region_resident", "location" ),
		&T3DData::ensureInstanceRegionResident );
	ClassDB::bind_method( D_METHOD( "ensure_cpu_region_resident", "region_location" ),
		&T3DData::ensureCpuRegionResident );
	ClassDB::bind_static_method( "Terrain3DData", D_METHOD( "get_region_map_index", "region_location" ),
								&T3DData::getRegionMapIndex );

	ClassDB::bind_method( D_METHOD( "do_for_regions", "area", "callback" ), &T3DData::doForRegions );
	ClassDB::bind_method( D_METHOD( "change_region_size", "region_size" ), &T3DData::changeRegionSize );

	ClassDB::bind_method( D_METHOD( "get_region_location", "global_position" ), &T3DData::getRegionLocation );
	ClassDB::bind_method( D_METHOD( "get_region_id", "region_location" ), &T3DData::getRegionId );
	ClassDB::bind_method( D_METHOD( "get_region_idp", "global_position" ), &T3DData::getRegionIdp );

	ClassDB::bind_method( D_METHOD( "has_region", "region_location" ), &T3DData::hasRegion );
	ClassDB::bind_method( D_METHOD( "has_regionp", "global_position" ), &T3DData::hasRegionp );
	ClassDB::bind_method( D_METHOD( "get_region", "region_location" ), &T3DData::getRegion );
	ClassDB::bind_method( D_METHOD( "get_regionp", "global_position" ), &T3DData::getRegionp );

	ClassDB::bind_method( D_METHOD( "set_region_modified", "region_location", "modified" ),
						 &T3DData::setRegionModified );
	ClassDB::bind_method( D_METHOD( "is_region_modified", "region_location" ), &T3DData::isRegionModified );
	ClassDB::bind_method( D_METHOD( "set_region_deleted", "region_location", "deleted" ),
						 &T3DData::setRegionDeleted );
	ClassDB::bind_method( D_METHOD( "is_region_deleted", "region_location" ), &T3DData::isRegionDeleted );

	ClassDB::bind_method( D_METHOD( "add_region_blankp", "global_position", "update" ), &T3DData::addRegionBlankp,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region_blank", "region_location", "update" ), &T3DData::addRegionBlank,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region", "region", "update" ), &T3DData::addRegion, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "replace_region_data", "region_location", "data" ),
						 &T3DData::replaceRegionData );
	ClassDB::bind_method( D_METHOD( "remove_regionp", "global_position", "update" ), &T3DData::removeRegionp,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_regionl", "region_location", "update" ), &T3DData::removeRegionl,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_region", "region", "update" ), &T3DData::removeRegion, DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "save_directory", "directory" ), &T3DData::saveDirectory );
	ClassDB::bind_method( D_METHOD( "save_region", "region_location", "directory", "save_16_bit" ),
						 &T3DData::saveRegion, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "inspect_gpu_region_pages" ), &T3DData::inspectGpuRegionPages );
	ClassDB::bind_method( D_METHOD( "cook_gpu_region_pages" ), &T3DData::cookGpuRegionPages );
	ClassDB::bind_method( D_METHOD( "load_directory", "directory" ), &T3DData::loadDirectory );
	ClassDB::bind_method( D_METHOD( "load_region", "region_location", "directory", "update" ), &T3DData::loadRegion,
						 DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "get_height_maps" ), &T3DData::getHeightMaps );
	ClassDB::bind_method( D_METHOD( "get_control_maps" ), &T3DData::getControlMaps );
	ClassDB::bind_method( D_METHOD( "get_color_maps" ), &T3DData::getColorMaps );
	ClassDB::bind_method( D_METHOD( "get_maps", "map_type" ), &T3DData::getMaps );
	ClassDB::bind_method( D_METHOD( "update_maps", "map_type", "all_regions", "generate_mipmaps" ),
						 &T3DData::updateMaps, DEFVAL( T3DRegion::TYPE_MAX ), DEFVAL( true ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "create_erosion_job", "settings" ), &T3DData::createErosionJob );
	ClassDB::bind_method( D_METHOD( "get_height_maps_rid" ), &T3DData::getHeightMapsRid );
	ClassDB::bind_method( D_METHOD( "get_derived_maps_rid" ), &T3DData::getDerivedMapsRid );
	ClassDB::bind_method( D_METHOD( "get_control_maps_rid" ), &T3DData::getControlMapsRid );
	ClassDB::bind_method( D_METHOD( "get_color_maps_rid" ), &T3DData::getColorMapsRid );
	ClassDB::bind_method( D_METHOD( "are_control_maps_implicit" ), &T3DData::areControlMapsImplicit );
	ClassDB::bind_method( D_METHOD( "are_color_maps_implicit" ), &T3DData::areColorMapsImplicit );

	ClassDB::bind_method( D_METHOD( "set_pixel", "map_type", "global_position", "pixel" ), &T3DData::setPixel );
	ClassDB::bind_method( D_METHOD( "commit_map_edit", "map_type", "region_location", "dirty_rect" ),
						 &T3DData::commitMapEdit, DEFVAL( Rect2i() ) );
	ClassDB::bind_method( D_METHOD( "get_pixel", "map_type", "global_position" ), &T3DData::getPixel );
	ClassDB::bind_method( D_METHOD( "set_height", "global_position", "height" ), &T3DData::setHeight );
	ClassDB::bind_method( D_METHOD( "get_height", "global_position" ), &T3DData::getHeight );
	ClassDB::bind_method( D_METHOD( "get_heights", "global_positions" ), &T3DData::getHeights );
	ClassDB::bind_method( D_METHOD( "get_normals", "global_positions" ), &T3DData::getNormals );
	ClassDB::bind_method( D_METHOD( "get_controls", "global_positions" ), &T3DData::getControls );
	ClassDB::bind_method( D_METHOD( "set_color", "global_position", "color" ), &T3DData::setColor );
	ClassDB::bind_method( D_METHOD( "get_color", "global_position" ), &T3DData::getColor );
	ClassDB::bind_method( D_METHOD( "set_control", "global_position", "control" ), &T3DData::setControl );
	ClassDB::bind_method( D_METHOD( "get_control", "global_position" ), &T3DData::getControl );
	ClassDB::bind_method( D_METHOD( "set_roughness", "global_position", "roughness" ), &T3DData::setRoughness );
	ClassDB::bind_method( D_METHOD( "get_roughness", "global_position" ), &T3DData::getRoughness );

	ClassDB::bind_method( D_METHOD( "set_control_base_id", "global_position", "texture_id" ),
						 &T3DData::setControlBaseId );
	ClassDB::bind_method( D_METHOD( "get_control_base_id", "global_position" ), &T3DData::getControlBaseId );
	ClassDB::bind_method( D_METHOD( "set_control_overlay_id", "global_position", "texture_id" ),
						 &T3DData::setControlOverlayId );
	ClassDB::bind_method( D_METHOD( "get_control_overlay_id", "global_position" ), &T3DData::getControlOverlayId );
	ClassDB::bind_method( D_METHOD( "set_control_blend", "global_position", "blend_value" ),
						 &T3DData::setControlBlend );
	ClassDB::bind_method( D_METHOD( "get_control_blend", "global_position" ), &T3DData::getControlBlend );
	ClassDB::bind_method( D_METHOD( "set_control_angle", "global_position", "degrees" ),
						 &T3DData::setControlAngle );
	ClassDB::bind_method( D_METHOD( "get_control_angle", "global_position" ), &T3DData::getControlAngle );
	ClassDB::bind_method( D_METHOD( "set_control_scale", "global_position", "percentage_modifier" ),
						 &T3DData::setControlScale );
	ClassDB::bind_method( D_METHOD( "get_control_scale", "global_position" ), &T3DData::getControlScale );
	ClassDB::bind_method( D_METHOD( "set_control_hole", "global_position", "enable" ), &T3DData::setControlHole );
	ClassDB::bind_method( D_METHOD( "get_control_hole", "global_position" ), &T3DData::getControlHole );
	ClassDB::bind_method( D_METHOD( "set_control_navigation", "global_position", "enable" ),
						 &T3DData::setControlNavigation );
	ClassDB::bind_method( D_METHOD( "get_control_navigation", "global_position" ), &T3DData::getControlNavigation );
	ClassDB::bind_method( D_METHOD( "set_control_auto", "global_position", "enable" ), &T3DData::setControlAuto );
	ClassDB::bind_method( D_METHOD( "get_control_auto", "global_position" ), &T3DData::getControlAuto );

	ClassDB::bind_method( D_METHOD( "get_normal", "global_position" ), &T3DData::getNormal );
	ClassDB::bind_method( D_METHOD( "is_in_slope", "global_position", "slope_range", "normal" ),
						 &T3DData::isInSlope, DEFVAL( V3_ZERO ) );
	ClassDB::bind_method( D_METHOD( "get_texture_id", "global_position" ), &T3DData::getTextureId );
	ClassDB::bind_method( D_METHOD( "get_mesh_vertex", "lod", "filter", "global_position" ),
						 &T3DData::getMeshVertex );

	ClassDB::bind_method( D_METHOD( "get_height_range" ), &T3DData::getHeightRange );
	ClassDB::bind_method( D_METHOD( "calc_height_range", "recursive" ), &T3DData::calcHeightRange, DEFVAL( false ) );

	ClassDB::bind_method( D_METHOD( "import_images", "images", "global_position", "offset", "scale" ),
						 &T3DData::importImages, DEFVAL( V3_ZERO ), DEFVAL( 0.f ), DEFVAL( 1.f ) );
	ClassDB::bind_method( D_METHOD( "export_image", "file_name", "map_type" ), &T3DData::exportImage );
	ClassDB::bind_method( D_METHOD( "layered_to_image", "map_type" ), &T3DData::layeredToImage );
	ClassDB::bind_method( D_METHOD( "dump", "verbose" ), &T3DData::dump, DEFVAL( false ) );

	const int ro_flags = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Runtime Repository Status", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "region_locations", PROPERTY_HINT_ARRAY_TYPE, "Vector2i", ro_flags ),
				 "set_region_locations", "get_region_locations" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "height_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ), "",
				 "get_height_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "control_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ), "",
				 "get_control_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "color_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ), "",
				 "get_color_maps" );

	ADD_SIGNAL( MethodInfo( "maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "gpu_region_page_table_changed" ) );
	ADD_SIGNAL( MethodInfo( "region_map_changed" ) );
	ADD_SIGNAL( MethodInfo( "height_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "control_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "color_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "maps_edited", PropertyInfo( Variant::AABB, "edited_area" ) ) );
}
