// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "core/terrain_3d_eastl_helpers.h"

#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_persistence.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_page_format.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instance_page_codec.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "memory/terrain_3d_memory.h"

#include <EASTL/bitset.h>
#include <limits.h>
#include <string.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static void t3dRuntimePageStoreFnv1a64Append( uint64_t &nHash, const uint8_t *pData, const size_t nSize ) {
	for ( size_t index = 0u; index < nSize; ++index ) {
		nHash ^= ( uint64_t )( pData[index] );
		nHash *= 1099511628211ull;
	}
}

EA_FORCE_INLINE static void t3dRuntimePageStoreFnv1a64AppendU32( uint64_t &nHash, const uint32_t nValue ) {
	t3dRuntimePageStoreFnv1a64Append( nHash, ( const uint8_t * )( &nValue ), sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dRuntimePageStoreFnv1a64AppendU64( uint64_t &nHash, const uint64_t nValue ) {
	t3dRuntimePageStoreFnv1a64Append( nHash, ( const uint8_t * )( &nValue ), sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dRuntimePageStoreFnv1a64AppendF32( uint64_t &nHash, const float fValue ) {
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ) );
	memcpy( &bits, &fValue, sizeof( bits ) );
	t3dRuntimePageStoreFnv1a64AppendU32( nHash, bits );
}

static void t3dRuntimePageStoreFnv1a64AppendString( uint64_t &nHash, const String &rValue ) {
	const CharString utf8 = rValue.utf8();
	const uint32_t length = ( uint32_t )( MAX( 0, utf8.length() ) );
	t3dRuntimePageStoreFnv1a64AppendU32( nHash, length );
	if ( length > 0u ) {
		t3dRuntimePageStoreFnv1a64Append( nHash, ( const uint8_t * )( utf8.get_data() ), ( size_t )( length ) );
	}
}

static bool t3dRuntimePageStoreFnv1a64AppendR16HeightEdge(
        uint64_t &nHash, const Ref<Image> &rHeight, const int nRegionSize, const bool bFirstColumn ) {
	if ( rHeight.is_null() || rHeight->get_width() != nRegionSize || rHeight->get_height() != nRegionSize
	        || rHeight->get_format() != Image::FORMAT_R16 || rHeight->has_mipmaps() ) {
		return false;
	}
	const PackedByteArray bytes = rHeight->get_data();
	const size_t sample_bytes = sizeof( uint16_t );
	const size_t row_bytes = ( size_t )( nRegionSize )*sample_bytes;
	const size_t expected_bytes = row_bytes * ( size_t )( nRegionSize );
	if ( ( size_t )( bytes.size() ) != expected_bytes ) {
		return false;
	}
	if ( !bFirstColumn ) {
		t3dRuntimePageStoreFnv1a64Append( nHash, bytes.ptr(), row_bytes );
		return true;
	}
	for ( int row = 0; row < nRegionSize; ++row ) {
		t3dRuntimePageStoreFnv1a64Append( nHash, bytes.ptr() + ( size_t )( row )*row_bytes, sample_bytes );
	}
	return true;
}

static bool t3dRuntimePageStoreFnv1a64AppendR16HeightCorner( uint64_t &nHash, const Ref<Image> &rHeight, const int nRegionSize ) {
	if ( rHeight.is_null() || rHeight->get_width() != nRegionSize || rHeight->get_height() != nRegionSize
	        || rHeight->get_format() != Image::FORMAT_R16 || rHeight->has_mipmaps() ) {
		return false;
	}
	const PackedByteArray bytes = rHeight->get_data();
	const size_t expected_bytes = ( size_t )( nRegionSize ) * ( size_t )( nRegionSize ) * sizeof( uint16_t );
	if ( ( size_t )( bytes.size() ) != expected_bytes ) {
		return false;
	}
	t3dRuntimePageStoreFnv1a64Append( nHash, bytes.ptr(), sizeof( uint16_t ) );
	return true;
}

EA_FORCE_INLINE static void t3dRuntimePageStoreWriteU32Raw( uint8_t *&pWrite, const uint32_t nValue ) {
	pWrite[0] = ( uint8_t )( nValue & 0xffu );
	pWrite[1] = ( uint8_t )( ( nValue >> 8u ) & 0xffu );
	pWrite[2] = ( uint8_t )( ( nValue >> 16u ) & 0xffu );
	pWrite[3] = ( uint8_t )( ( nValue >> 24u ) & 0xffu );
	pWrite += 4u;
}

EA_FORCE_INLINE static uint32_t t3dRuntimePageStoreReadU32Raw( const uint8_t *&pRead ) {
	const uint32_t value =
	        ( uint32_t )pRead[0] | ( ( uint32_t )pRead[1] << 8u ) | ( ( uint32_t )pRead[2] << 16u ) | ( ( uint32_t )pRead[3] << 24u );
	pRead += 4u;
	return value;
}

static bool t3dRuntimePageStoreEncodeHeader( PackedByteArray &rBytes, const t3dRuntimePageHeader_t &rHeader ) {
	if ( rBytes.size() < s_nRuntimePageHeaderBytes ) {
		return false;
	}
	uint8_t *const pBegin = rBytes.ptrw();
	uint8_t *pWrite = pBegin;
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nMagic );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nVersion );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nHeaderBytes );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nRegionSize );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nColorDivisor );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nDerivedDivisor );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nChannelFlags );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nTotalBytes );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nChannelCount );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_iChannelTableOffset );
	for ( uint32_t channel = 0u; channel < s_nRuntimePageChannelCount; ++channel ) {
		const t3dRuntimePageChannelInfo_t &info = rHeader.m_szChannels[channel];
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nType );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nFormat );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nCompression );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_iOffset );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nCompressedBytes );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nUncompressedBytes );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nWidth );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nHeight );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nMipCount );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nAlignment );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nChecksum );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.m_nSignature );
	}
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nSourceSignatureLo );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nSourceSignatureHi );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nBlendRecipeVersion );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nMaterialMask );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nMiddleHeightDivisor );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nWorldSignatureLo );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nWorldSignatureHi );
	t3dRuntimePageStoreWriteU32Raw( pWrite, ( uint32_t )rHeader.m_nRegionX );
	t3dRuntimePageStoreWriteU32Raw( pWrite, ( uint32_t )rHeader.m_nRegionZ );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nBoundsMinYBits );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.m_nBoundsMaxYBits );
	return ( size_t )( pWrite - pBegin ) == s_nRuntimePageHeaderBytes;
}

static t3dRuntimePageHeader_t t3dRuntimePageStoreDecodeHeaderRaw( const uint8_t *pRead, const size_t nBytes ) {
	t3dRuntimePageHeader_t header;
	if ( !pRead || nBytes < s_nRuntimePageHeaderBytes ) {
		return header;
	}
	header.m_nMagic = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nVersion = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nHeaderBytes = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nRegionSize = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nColorDivisor = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nDerivedDivisor = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nChannelFlags = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nTotalBytes = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nChannelCount = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_iChannelTableOffset = t3dRuntimePageStoreReadU32Raw( pRead );
	for ( uint32_t channel = 0u; channel < s_nRuntimePageChannelCount; ++channel ) {
		t3dRuntimePageChannelInfo_t &info = header.m_szChannels[channel];
		info.m_nType = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nFormat = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nCompression = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_iOffset = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nCompressedBytes = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nUncompressedBytes = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nWidth = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nHeight = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nMipCount = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nAlignment = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nChecksum = t3dRuntimePageStoreReadU32Raw( pRead );
		info.m_nSignature = t3dRuntimePageStoreReadU32Raw( pRead );
	}
	header.m_nSourceSignatureLo = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nSourceSignatureHi = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nBlendRecipeVersion = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nMaterialMask = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nMiddleHeightDivisor = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nWorldSignatureLo = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nWorldSignatureHi = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nRegionX = ( int32_t )t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nRegionZ = ( int32_t )t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nBoundsMinYBits = t3dRuntimePageStoreReadU32Raw( pRead );
	header.m_nBoundsMaxYBits = t3dRuntimePageStoreReadU32Raw( pRead );
	return header;
}

static t3dRuntimePageHeader_t t3dRuntimePageStoreDecodeHeader( const PackedByteArray &rBytes ) {
	return t3dRuntimePageStoreDecodeHeaderRaw( rBytes.ptr(), ( size_t )( rBytes.size() ) );
}

typedef struct t3dStagedPageBuild_s {
	PackedByteArray m_szPayloads[s_nRuntimePageChannelCount];
	uint32_t m_nTotalBytes = s_nRuntimePageHeaderBytes;
} t3dStagedPageBuild_t;

static PackedByteArray t3dRuntimePageStoreEncodeRle( const PackedByteArray &rSource ) {
	PackedByteArray encoded;
	if ( rSource.is_empty() ) {
		return encoded;
	}
	const uint8_t *const pRead = rSource.ptr();
	int64_t read = 0;
	int64_t encoded_bytes = 0;
	while ( read < rSource.size() ) {
		const uint8_t value = pRead[read];
		uint32_t run = 1u;
		while ( read + run < rSource.size() && run < 255u && pRead[read + run] == value ) {
			++run;
		}
		encoded_bytes += 2;
		read += run;
	}
	if ( encoded_bytes + 16 >= rSource.size() ) {
		return encoded;
	}
	encoded.resize( encoded_bytes );
	if ( encoded.size() != encoded_bytes ) {
		return PackedByteArray();
	}
	uint8_t *const pWrite = encoded.ptrw();
	read = 0;
	int64_t write = 0;
	while ( read < rSource.size() ) {
		const uint8_t value = pRead[read];
		uint8_t run = 1u;
		while ( read + run < rSource.size() && run < 255u && pRead[read + run] == value ) {
			++run;
		}
		pWrite[write++] = run;
		pWrite[write++] = value;
		read += run;
	}
	return encoded;
}

static PackedByteArray t3dRuntimePageStoreDecodeRle( const PackedByteArray &rSource, const uint32_t nExpectedBytes ) {
	PackedByteArray decoded;
	if ( rSource.is_empty() || nExpectedBytes == 0u || nExpectedBytes > s_nRuntimePageMaxBytes ) {
		return decoded;
	}
	decoded.resize( nExpectedBytes );
	if ( decoded.size() != ( int64_t )nExpectedBytes ) {
		return PackedByteArray();
	}
	const uint8_t *const pRead = rSource.ptr();
	uint8_t *const pWrite = decoded.ptrw();
	int64_t read = 0;
	uint32_t write = 0u;
	while ( read + 1 < rSource.size() ) {
		const uint32_t run = pRead[read++];
		const uint8_t value = pRead[read++];
		if ( run == 0u || run > nExpectedBytes - write ) {
			decoded.clear();
			return decoded;
		}
		memset( pWrite + write, value, run );
		write += run;
	}
	if ( read != rSource.size() || write != nExpectedBytes ) {
		decoded.clear();
	}
	return decoded;
}

static bool t3dRuntimePageStoreStageImageChannel(
        t3dStagedPageBuild_t &rBuild, t3dRuntimePageHeader_t &rHeader, const t3dRuntimePageChannel_t channel, const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() || rHeader.m_nChannelCount >= s_nRuntimePageChannelCount
	        || t3dRuntimePageFindChannel( rHeader, channel ) ) {
		return false;
	}
	const PackedByteArray image_bytes = rImage->get_data();
	if ( image_bytes.is_empty() || image_bytes.size() > INT32_MAX ) {
		return false;
	}
	const uint32_t aligned_offset = t3dRuntimePageAlign( rBuild.m_nTotalBytes );
	if ( aligned_offset == 0u || aligned_offset > s_nRuntimePageMaxBytes
	        || ( uint32_t )image_bytes.size() > s_nRuntimePageMaxBytes - aligned_offset ) {
		return false;
	}
	const uint32_t channel_index = rHeader.m_nChannelCount++;
	rBuild.m_szPayloads[channel_index] = image_bytes;
	t3dRuntimePageChannelInfo_t &info = rHeader.m_szChannels[channel_index];
	info.m_nType = ( uint32_t )channel;
	info.m_nFormat = ( uint32_t )rImage->get_format();
	info.m_nCompression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	info.m_iOffset = aligned_offset;
	info.m_nCompressedBytes = ( uint32_t )image_bytes.size();
	info.m_nUncompressedBytes = ( uint32_t )image_bytes.size();
	info.m_nChecksum = t3dRuntimePageChecksum( image_bytes.ptr(), ( size_t )image_bytes.size() );
	info.m_nWidth = ( uint32_t )rImage->get_width();
	info.m_nHeight = ( uint32_t )rImage->get_height();
	info.m_nMipCount = rImage->has_mipmaps() ? ( uint32_t )( rImage->get_mipmap_count() + 1 ) : 1u;
	info.m_nAlignment = s_nRuntimePageAlignment;
	info.m_nSignature = info.m_nChecksum;
	rBuild.m_nTotalBytes = aligned_offset + info.m_nCompressedBytes;
	rHeader.m_nTotalBytes = rBuild.m_nTotalBytes;
	return true;
}

static bool t3dRuntimePageStoreStageBlobChannel( t3dStagedPageBuild_t &rBuild,
        t3dRuntimePageHeader_t &rHeader,
        const t3dRuntimePageChannel_t channel,
        const PackedByteArray &rPayload,
        const bool bAllowCompression = true ) {
	if ( rPayload.is_empty() || rHeader.m_nChannelCount >= s_nRuntimePageChannelCount || t3dRuntimePageFindChannel( rHeader, channel ) ) {
		return false;
	}
	PackedByteArray stored = rPayload;
	uint32_t compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	if ( bAllowCompression && rPayload.size() >= 64 ) {
		PackedByteArray compressed = t3dRuntimePageStoreEncodeRle( rPayload );
		if ( !compressed.is_empty() ) {
			stored = compressed;
			compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE;
		}
	}
	const uint32_t aligned_offset = t3dRuntimePageAlign( rBuild.m_nTotalBytes );
	if ( aligned_offset == 0u || ( uint64_t )aligned_offset + ( uint64_t )stored.size() > s_nRuntimePageMaxBytes ) {
		return false;
	}
	const uint32_t channel_index = rHeader.m_nChannelCount++;
	rBuild.m_szPayloads[channel_index] = stored;
	t3dRuntimePageChannelInfo_t &info = rHeader.m_szChannels[channel_index];
	info.m_nType = ( uint32_t )channel;
	info.m_nFormat = UINT32_MAX;
	info.m_nCompression = compression;
	info.m_iOffset = aligned_offset;
	info.m_nCompressedBytes = ( uint32_t )rBuild.m_szPayloads[channel_index].size();
	info.m_nUncompressedBytes = ( uint32_t )rPayload.size();
	info.m_nWidth = ( uint32_t )rPayload.size();
	info.m_nHeight = 1u;
	info.m_nMipCount = 1u;
	info.m_nChecksum = t3dRuntimePageChecksum( rPayload.ptr(), ( size_t )rPayload.size() );
	info.m_nSignature = info.m_nChecksum;
	rBuild.m_nTotalBytes = aligned_offset + info.m_nCompressedBytes;
	rHeader.m_nTotalBytes = rBuild.m_nTotalBytes;
	return true;
}

static bool t3dRuntimePageStoreFinalizePageBytes(
        const t3dStagedPageBuild_t &rBuild, t3dRuntimePageHeader_t &rHeader, PackedByteArray &rFileBytes ) {
	if ( rBuild.m_nTotalBytes < s_nRuntimePageHeaderBytes || rBuild.m_nTotalBytes > s_nRuntimePageMaxBytes ) {
		return false;
	}
	rFileBytes.resize( rBuild.m_nTotalBytes );
	if ( rFileBytes.size() != ( int64_t )rBuild.m_nTotalBytes ) {
		return false;
	}
	uint8_t *const pWrite = rFileBytes.ptrw();
	memset( pWrite, 0, rBuild.m_nTotalBytes );
	for ( uint32_t channel_index = 0u; channel_index < rHeader.m_nChannelCount; ++channel_index ) {
		const t3dRuntimePageChannelInfo_t &info = rHeader.m_szChannels[channel_index];
		const PackedByteArray &payload = rBuild.m_szPayloads[channel_index];
		if ( payload.size() != ( int64_t )info.m_nCompressedBytes || info.m_iOffset > rBuild.m_nTotalBytes
		        || info.m_nCompressedBytes > rBuild.m_nTotalBytes - info.m_iOffset ) {
			return false;
		}
		if ( info.m_nCompressedBytes > 0u ) {
			memcpy( pWrite + info.m_iOffset, payload.ptr(), info.m_nCompressedBytes );
		}
	}
	rHeader.m_nTotalBytes = rBuild.m_nTotalBytes;
	return t3dRuntimePageStoreEncodeHeader( rFileBytes, rHeader );
}

static PackedByteArray t3dRuntimePageStoreReadBlob(
        const Ref<FileAccess> &rFile, const t3dRuntimePageChannelInfo_t &rInfo, uint64_t &nReadUsec, uint64_t &nValidationUsec ) {
	if ( rFile.is_null() || rInfo.m_nFormat != UINT32_MAX || rInfo.m_nCompressedBytes == 0u
	        || ( rInfo.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE
	                && rInfo.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ) ) {
		return PackedByteArray();
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.m_iOffset );
	const PackedByteArray stored = rFile->get_buffer( rInfo.m_nCompressedBytes );
	nReadUsec += ( pTime ? pTime->get_ticks_usec() : read_started ) - read_started;
	const uint64_t validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	PackedByteArray bytes;
	if ( stored.size() == ( int64_t )( rInfo.m_nCompressedBytes ) ) {
		bytes = rInfo.m_nCompression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE
		        ? t3dRuntimePageStoreDecodeRle( stored, rInfo.m_nUncompressedBytes )
		        : stored;
	}
	const bool valid = bytes.size() == ( int64_t )( rInfo.m_nUncompressedBytes )
	        && t3dRuntimePageChecksum( bytes.ptr(), ( size_t )( bytes.size() ) ) == rInfo.m_nChecksum;
	nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	return valid ? bytes : PackedByteArray();
}

static bool t3dRuntimePageStoreExpectedImageBytes( const t3dRuntimePageChannelInfo_t &rInfo, uint64_t &rExpectedBytes ) {
	uint32_t bytes_per_pixel = 0u;
	switch ( Image::Format( rInfo.m_nFormat ) ) {
		case Image::FORMAT_L8:
			bytes_per_pixel = 1u;
			break;
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
	if ( rInfo.m_nWidth == 0u || rInfo.m_nHeight == 0u || rInfo.m_nMipCount == 0u ) {
		return false;
	}
	uint32_t width = rInfo.m_nWidth;
	uint32_t height = rInfo.m_nHeight;
	uint64_t total = 0u;
	for ( uint32_t mip = 0u; mip < rInfo.m_nMipCount; ++mip ) {
		const uint64_t level_bytes = ( uint64_t )( width ) * ( uint64_t )( height )*bytes_per_pixel;
		if ( level_bytes > s_nRuntimePageMaxBytes || total > s_nRuntimePageMaxBytes - level_bytes ) {
			return false;
		}
		total += level_bytes;
		width = MAX( 1u, width >> 1u );
		height = MAX( 1u, height >> 1u );
	}
	rExpectedBytes = total;
	return true;
}

static Ref<Image> t3dRuntimePageStoreReadImage( const Ref<FileAccess> &rFile,
        const t3dRuntimePageChannelInfo_t &rInfo,
        uint64_t &nReadUsec,
        uint64_t &nValidationUsec,
        uint64_t &nMaterializeUsec,
        PackedByteArray *const pValidatedNativeBytes = NULL,
        const bool bMaterialize = true ) {
	if ( rFile.is_null() || rInfo.m_nCompressedBytes == 0u || rInfo.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ) {
		return Ref<Image>();
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.m_iOffset );
	const PackedByteArray image_bytes = rFile->get_buffer( rInfo.m_nCompressedBytes );
	const uint64_t read_finished = pTime ? pTime->get_ticks_usec() : read_started;
	nReadUsec += read_finished - read_started;
	const uint64_t validation_started = read_finished;
	uint64_t expected_bytes = 0u;
	const bool bPayloadShapeValid =
	        t3dRuntimePageStoreExpectedImageBytes( rInfo, expected_bytes ) && expected_bytes == ( uint64_t )( rInfo.m_nUncompressedBytes );
	const bool bPayloadValid = bPayloadShapeValid && image_bytes.size() == ( int64_t )( rInfo.m_nCompressedBytes )
	        && t3dRuntimePageChecksum( image_bytes.ptr(), ( size_t )( image_bytes.size() ) ) == rInfo.m_nChecksum;
	const uint64_t validation_finished = pTime ? pTime->get_ticks_usec() : validation_started;
	nValidationUsec += validation_finished - validation_started;
	if ( !bPayloadValid ) {
		return Ref<Image>();
	}
	if ( pValidatedNativeBytes ) {
		*pValidatedNativeBytes = image_bytes;
	}
	if ( !bMaterialize ) {
		return Ref<Image>();
	}
	const uint64_t materialize_started = validation_finished;
	const Ref<Image> pImage = Image::create_from_data(
	        ( int )( rInfo.m_nWidth ), ( int )( rInfo.m_nHeight ), rInfo.m_nMipCount > 1u, Image::Format( rInfo.m_nFormat ), image_bytes );
	const bool bImageValid =
	        pImage.is_valid() && !pImage->is_empty() && pImage->get_data().size() == ( int64_t )( rInfo.m_nUncompressedBytes );
	const uint64_t materialize_finished = pTime ? pTime->get_ticks_usec() : materialize_started;
	nMaterializeUsec += materialize_finished - materialize_started;
	return bImageValid ? pImage : Ref<Image>();
}


static bool t3dRuntimePageStoreDecodeRleInto(
        const uint8_t *const pRead, const uint32_t nReadBytes, uint8_t *const pWrite, const uint32_t nWriteBytes ) {
	if ( !pRead || !pWrite || nReadBytes == 0u || nWriteBytes == 0u ) {
		return false;
	}
	uint32_t read = 0u;
	uint32_t write = 0u;
	while ( read + 1u < nReadBytes ) {
		const uint32_t run = pRead[read++];
		const uint8_t value = pRead[read++];
		if ( run == 0u || run > nWriteBytes - write ) {
			return false;
		}
		memset( pWrite + write, value, run );
		write += run;
	}
	return read == nReadBytes && write == nWriteBytes;
}

static bool t3dRuntimePageStoreReadBlobFixed( const Ref<FileAccess> &rFile,
        const t3dRuntimePageChannelInfo_t &rInfo,
        PackedByteArray &rTarget,
        uint32_t &rLogicalBytes,
        t3dRuntimePageWorkerScratch_t &rScratch,
        uint64_t &nReadUsec,
        uint64_t &nValidationUsec ) {
	rLogicalBytes = 0u;
	if ( rFile.is_null() || rInfo.m_nFormat != UINT32_MAX || rInfo.m_nCompressedBytes == 0u || rInfo.m_nUncompressedBytes == 0u
	        || rInfo.m_nCompressedBytes > ( uint32_t )( rScratch.m_ioBytes.size() )
	        || rInfo.m_nUncompressedBytes > ( uint32_t )( rTarget.size() )
	        || ( rInfo.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE
	                && rInfo.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ) ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.m_iOffset );
	uint8_t *const pScratch = rScratch.m_ioBytes.ptrw();
	const uint64_t read_bytes = rFile->get_buffer( pScratch, rInfo.m_nCompressedBytes );
	nReadUsec += ( pTime ? pTime->get_ticks_usec() : read_started ) - read_started;
	if ( read_bytes != rInfo.m_nCompressedBytes ) {
		return false;
	}
	uint8_t *const pTarget = rTarget.ptrw();
	const bool decoded = rInfo.m_nCompression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE
	        ? t3dRuntimePageStoreDecodeRleInto( pScratch, rInfo.m_nCompressedBytes, pTarget, rInfo.m_nUncompressedBytes )
	        : ( memcpy( pTarget, pScratch, rInfo.m_nUncompressedBytes ), true );
	const uint64_t validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const bool valid = decoded && t3dRuntimePageChecksum( rTarget.ptr(), ( size_t )( rInfo.m_nUncompressedBytes ) ) == rInfo.m_nChecksum;
	nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	if ( valid ) {
		rLogicalBytes = rInfo.m_nUncompressedBytes;
	}
	return valid;
}

EA_FORCE_INLINE static uint32_t t3dRuntimePageStoreChannelIndex( const t3dRuntimePageChannel_t eChannel );

static bool t3dRuntimePageStoreReadOptionalBlob( const Ref<FileAccess> &rFile,
        const t3dRuntimePageHeader_t &rHeader,
        t3dRuntimePagePayload_t &rPayload,
        t3dRuntimePageWorkerScratch_t &rScratch,
        t3dRuntimePageMeta_t &rMeta,
        const t3dRuntimePageChannel_t eChannel,
        PackedByteArray &rTarget,
        const bool bRequired ) {
	const uint32_t nChannelIndex = t3dRuntimePageStoreChannelIndex( eChannel );
	if ( nChannelIndex == 0u ) {
		return false;
	}
	const t3dRuntimePageChannelInfo_t *const pChannel = t3dRuntimePageFindChannel( rHeader, eChannel );
	if ( !pChannel ) {
		rPayload.m_szLogicalBytes[nChannelIndex] = 0u;
		return !bRequired;
	}
	return t3dRuntimePageStoreReadBlobFixed(
	        rFile, *pChannel, rTarget, rPayload.m_szLogicalBytes[nChannelIndex], rScratch, rMeta.m_nFileReadUsec, rMeta.m_nValidationUsec );
}

static bool t3dRuntimePageStoreReadImageFixed( const Ref<FileAccess> &rFile,
        const t3dRuntimePageChannelInfo_t &rInfo,
        PackedByteArray &rBytes,
        uint32_t &rLogicalBytes,
        Ref<Image> &rImage,
        uint64_t &nReadUsec,
        uint64_t &nValidationUsec,
        uint64_t &nMaterializeUsec,
        const bool bMaterialize ) {
	rLogicalBytes = 0u;
	if ( rFile.is_null() || rInfo.m_nCompressedBytes == 0u || rInfo.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ) {
		return false;
	}
	uint64_t expected_bytes = 0u;
	if ( !t3dRuntimePageStoreExpectedImageBytes( rInfo, expected_bytes ) || expected_bytes != ( uint64_t )( rInfo.m_nUncompressedBytes )
	        || rInfo.m_nCompressedBytes != rInfo.m_nUncompressedBytes || rBytes.size() != ( int64_t )( rInfo.m_nUncompressedBytes ) ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.m_iOffset );
	const uint64_t read_bytes = rFile->get_buffer( rBytes.ptrw(), rInfo.m_nCompressedBytes );
	const uint64_t read_finished = pTime ? pTime->get_ticks_usec() : read_started;
	nReadUsec += read_finished - read_started;
	const uint64_t validation_started = read_finished;
	const bool valid = read_bytes == rInfo.m_nCompressedBytes
	        && t3dRuntimePageChecksum( rBytes.ptr(), ( size_t )( rBytes.size() ) ) == rInfo.m_nChecksum;
	const uint64_t validation_finished = pTime ? pTime->get_ticks_usec() : validation_started;
	nValidationUsec += validation_finished - validation_started;
	if ( !valid ) {
		return false;
	}
	rLogicalBytes = rInfo.m_nUncompressedBytes;
	if ( !bMaterialize ) {
		rImage.unref();
		return true;
	}
	const uint64_t materialize_started = validation_finished;
	rImage = Image::create_from_data(
	        ( int )( rInfo.m_nWidth ), ( int )( rInfo.m_nHeight ), rInfo.m_nMipCount > 1u, Image::Format( rInfo.m_nFormat ), rBytes );
	const bool image_valid =
	        rImage.is_valid() && !rImage->is_empty() && rImage->get_data().size() == ( int64_t )( rInfo.m_nUncompressedBytes );
	nMaterializeUsec += ( pTime ? pTime->get_ticks_usec() : materialize_started ) - materialize_started;
	return image_valid;
}

static uint32_t t3dRuntimePageStoreRequiredScratchBytes( const String &rPagePath ) {
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return s_nRuntimePageHeaderBytes;
	}
	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nRuntimePageHeaderBytes ) {
		return s_nRuntimePageHeaderBytes;
	}
	eastl::array<uint8_t, s_nRuntimePageHeaderBytes> header_bytes;
	if ( pFile->get_buffer( header_bytes.data(), s_nRuntimePageHeaderBytes ) != s_nRuntimePageHeaderBytes ) {
		return s_nRuntimePageHeaderBytes;
	}
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeaderRaw( header_bytes.data(), header_bytes.size() );
	if ( !t3dRuntimePageValidateHeader( header, pFile->get_length() ) ) {
		return s_nRuntimePageHeaderBytes;
	}
	uint32_t scratch_bytes = s_nRuntimePageHeaderBytes;
	for ( uint32_t channel = 0u; channel < s_nRuntimePageChannelCount; ++channel ) {
		scratch_bytes = MAX( scratch_bytes, header.m_szChannels[channel].m_nCompressedBytes );
	}
	return scratch_bytes;
}

EA_FORCE_INLINE static bool t3dRuntimePageStoreValidImage(
        const Ref<Image> &rImage, const int nWidth, const int nHeight, const Image::Format eFormat, const bool bMipmaps ) {
	return rImage.is_valid() && !rImage->is_empty() && rImage->get_width() == nWidth && rImage->get_height() == nHeight
	        && rImage->get_format() == eFormat && rImage->has_mipmaps() == bMipmaps;
}

EA_FORCE_INLINE static uint32_t t3dRuntimePageStoreChannelIndex( const t3dRuntimePageChannel_t eChannel ) {
	const uint32_t channel = ( uint32_t )( eChannel );
	return channel <= s_nRuntimePageChannelCount ? channel : 0u;
}

static PackedByteArray *t3dRuntimePageStorePayloadBuffer( t3dRuntimePagePayload_t &rPayload, const t3dRuntimePageChannel_t eChannel ) {
	switch ( eChannel ) {
		case TERRAIN3D_RUNTIME_PAGE_HEIGHT:
			return &rPayload.m_heightBytes;
		case TERRAIN3D_RUNTIME_PAGE_CONTROL:
			return &rPayload.m_controlBytes;
		case TERRAIN3D_RUNTIME_PAGE_COLOR:
			return &rPayload.m_colorBytes;
		case TERRAIN3D_RUNTIME_PAGE_DERIVED:
			return &rPayload.m_derivedBytes;
		case TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT:
			return &rPayload.m_middleHeightBytes;
		case TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE:
			return &rPayload.m_middleCompositeBytes;
		case TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK:
			return &rPayload.m_terrainShadowMaskBytes;
		case TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES:
			return &rPayload.m_instancePage;
		case TERRAIN3D_RUNTIME_PAGE_HOLE_MASK:
			return &rPayload.m_holeMask;
		case TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT:
			return &rPayload.m_collisionHeight;
		case TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS:
			return &rPayload.m_collisionBounds;
		case TERRAIN3D_RUNTIME_PAGE_NAVIGATION:
			return &rPayload.m_navigation;
		case TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT:
			return &rPayload.m_minMaxHeight;
		case TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS:
			return &rPayload.m_nodeDescriptors;
		case TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA:
			return &rPayload.m_instanceMetadata;
		case TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS:
			return &rPayload.m_lightingReceivers;
		case TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START:
			return &rPayload.m_multimeshFastStart;
		default:
			return NULL;
	}
}

static const PackedByteArray *t3dRuntimePageStorePayloadBuffer(
        const t3dRuntimePagePayload_t &rPayload, const t3dRuntimePageChannel_t eChannel ) {
	switch ( eChannel ) {
		case TERRAIN3D_RUNTIME_PAGE_HEIGHT:
			return &rPayload.m_heightBytes;
		case TERRAIN3D_RUNTIME_PAGE_CONTROL:
			return &rPayload.m_controlBytes;
		case TERRAIN3D_RUNTIME_PAGE_COLOR:
			return &rPayload.m_colorBytes;
		case TERRAIN3D_RUNTIME_PAGE_DERIVED:
			return &rPayload.m_derivedBytes;
		case TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT:
			return &rPayload.m_middleHeightBytes;
		case TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE:
			return &rPayload.m_middleCompositeBytes;
		case TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK:
			return &rPayload.m_terrainShadowMaskBytes;
		case TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES:
			return &rPayload.m_instancePage;
		case TERRAIN3D_RUNTIME_PAGE_HOLE_MASK:
			return &rPayload.m_holeMask;
		case TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT:
			return &rPayload.m_collisionHeight;
		case TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS:
			return &rPayload.m_collisionBounds;
		case TERRAIN3D_RUNTIME_PAGE_NAVIGATION:
			return &rPayload.m_navigation;
		case TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT:
			return &rPayload.m_minMaxHeight;
		case TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS:
			return &rPayload.m_nodeDescriptors;
		case TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA:
			return &rPayload.m_instanceMetadata;
		case TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS:
			return &rPayload.m_lightingReceivers;
		case TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START:
			return &rPayload.m_multimeshFastStart;
		default:
			return NULL;
	}
}

static bool t3dRuntimePageStorePreparePayloadStorage(
        t3dRuntimePagePayload_t &rPayload, const eastl::array<uint32_t, s_nRuntimePageChannelCount + 1u> &rCapacities ) {
	memset( rPayload.m_szLogicalBytes, 0, sizeof( rPayload.m_szLogicalBytes ) );
	for ( uint32_t channel = 1u; channel <= s_nRuntimePageChannelCount; ++channel ) {
		PackedByteArray *const pBuffer = t3dRuntimePageStorePayloadBuffer( rPayload, ( t3dRuntimePageChannel_t )( channel ) );
		if ( !pBuffer ) {
			continue;
		}
		const uint32_t capacity = rCapacities[channel];
		if ( pBuffer->size() != ( int64_t )( capacity ) ) {
			pBuffer->resize( capacity );
		}
		if ( pBuffer->size() != ( int64_t )( capacity ) ) {
			return false;
		}
	}
	return true;
}

static void t3dRuntimePageStoreResetPayloadRuntime( t3dRuntimePagePayload_t &rPayload ) {
	memset( rPayload.m_szLogicalBytes, 0, sizeof( rPayload.m_szLogicalBytes ) );
	rPayload.m_height.unref();
	rPayload.m_control.unref();
	rPayload.m_color.unref();
	rPayload.m_derived.unref();
	rPayload.m_middleHeight.unref();
	rPayload.m_middleComposite.unref();
	rPayload.m_terrainShadowMask.unref();
	rPayload.m_preparedInstanceStore.clearRetainCapacity();
}

static bool t3dRuntimePageStoreReadHeaderCold( const String &rPagePath, t3dRuntimePageHeader_t &rHeader ) {
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return false;
	}
	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nRuntimePageHeaderBytes ) {
		return false;
	}
	eastl::array<uint8_t, s_nRuntimePageHeaderBytes> header_bytes;
	if ( pFile->get_buffer( header_bytes.data(), s_nRuntimePageHeaderBytes ) != s_nRuntimePageHeaderBytes ) {
		return false;
	}
	rHeader = t3dRuntimePageStoreDecodeHeaderRaw( header_bytes.data(), header_bytes.size() );
	return t3dRuntimePageValidateHeader( rHeader, pFile->get_length() );
}

static bool t3dRuntimePageStorePreparePayloadForSourceCold(
        t3dRuntimePagePayload_t &rPayload, t3dRuntimePageWorkerScratch_t &rScratch, const String &rPagePath ) {
	t3dRuntimePageHeader_t header;
	if ( !t3dRuntimePageStoreReadHeaderCold( rPagePath, header ) ) {
		return false;
	}
	eastl::array<uint32_t, s_nRuntimePageChannelCount + 1u> capacities{};
	uint32_t scratch_bytes = s_nRuntimePageHeaderBytes;
	for ( uint32_t channel = 0u; channel < header.m_nChannelCount; ++channel ) {
		const t3dRuntimePageChannelInfo_t &info = header.m_szChannels[channel];
		if ( info.m_nType == 0u || info.m_nType > s_nRuntimePageChannelCount
		        || !t3dRuntimePageStorePayloadBuffer( rPayload, ( t3dRuntimePageChannel_t )( info.m_nType ) ) ) {
			continue;
		}
		capacities[info.m_nType] = info.m_nUncompressedBytes;
		scratch_bytes = MAX( scratch_bytes, info.m_nCompressedBytes );
	}
	if ( !t3dRuntimePageStorePreparePayloadStorage( rPayload, capacities ) ) {
		return false;
	}
	rScratch.m_ioBytes.resize( scratch_bytes );
	return rScratch.m_ioBytes.size() == ( int64_t )( scratch_bytes );
}

static void t3dRuntimePageStoreResetMeta( t3dRuntimePageMeta_t &rMeta ) {
	rMeta.m_location = s_v2iMax;
	rMeta.m_iSourceIndex = -1;
	rMeta.m_iPayloadSlot = -1;
	rMeta.m_nExpectedChannelFlags = 0u;
	rMeta.m_nExpectedSourceSignature = 0u;
	rMeta.m_nRegionSize = 0;
	rMeta.m_nColorDivisor = 1;
	rMeta.m_nDerivedDivisor = 1;
	rMeta.m_nMiddleHeightDivisor = 1;
	rMeta.m_nMaterialMask = 1u;
	rMeta.m_nConsumerMask = 0u;
	rMeta.m_nAttemptedConsumerMask = 0u;
	rMeta.m_nMaterializedConsumerMask = 0u;
	rMeta.m_nLoadedConsumerMask = 0u;
	rMeta.m_nRequestedUsec = 0u;
	rMeta.m_nWorkerStartedUsec = 0u;
	rMeta.m_nFileOpenUsec = 0u;
	rMeta.m_nFileReadUsec = 0u;
	rMeta.m_nValidationUsec = 0u;
	rMeta.m_nMaterializeUsec = 0u;
	rMeta.m_nWorkerFinishedUsec = 0u;
	rMeta.m_bAttempted = false;
	rMeta.m_bLoaded = false;
	rMeta.m_bInstanceAttempted = false;
	rMeta.m_bInstanceLoaded = false;
	rMeta.m_bLoadMinMax = false;
	rMeta.m_bEditorAuthoring = false;
}

static void t3dRuntimePageStoreResetSource( t3dRuntimePageSource_t &rSource ) {
	rSource.m_pagePath = String();
	rSource.m_sourcePath = String();
	rSource.m_nExpectedChannelFlags = 0u;
	rSource.m_nExpectedSourceSignature = 0u;
	rSource.m_bPresent = false;
}

static void t3dRuntimePageStoreReleasePayload( t3dRuntimePagePayload_t &rPayload ) {
	memset( rPayload.m_szLogicalBytes, 0, sizeof( rPayload.m_szLogicalBytes ) );
	rPayload.m_heightBytes.clear();
	rPayload.m_controlBytes.clear();
	rPayload.m_colorBytes.clear();
	rPayload.m_derivedBytes.clear();
	rPayload.m_middleHeightBytes.clear();
	rPayload.m_middleCompositeBytes.clear();
	rPayload.m_terrainShadowMaskBytes.clear();
	rPayload.m_height.unref();
	rPayload.m_control.unref();
	rPayload.m_color.unref();
	rPayload.m_derived.unref();
	rPayload.m_middleHeight.unref();
	rPayload.m_middleComposite.unref();
	rPayload.m_terrainShadowMask.unref();
	rPayload.m_instancePage.clear();
	rPayload.m_holeMask.clear();
	rPayload.m_collisionHeight.clear();
	rPayload.m_collisionBounds.clear();
	rPayload.m_navigation.clear();
	rPayload.m_minMaxHeight.clear();
	rPayload.m_nodeDescriptors.clear();
	rPayload.m_instanceMetadata.clear();
	rPayload.m_lightingReceivers.clear();
	rPayload.m_multimeshFastStart.clear();
	rPayload.m_preparedInstanceStore.releaseStorage();
}

static void t3dRuntimePageStoreUpdateEditorFile( const String &rPath ) {
	if ( !IS_EDITOR || !rPath.begins_with( "res://" ) ) {
		return;
	}
	EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
	if ( pFilesystem && !pFilesystem->is_scanning() ) {
		pFilesystem->update_file( rPath );
	}
}

Terrain3DRuntimePageStore::Terrain3DRuntimePageStore(
        Terrain3DRegionRepository *const pRepository, const Terrain3DRegionPersistence *const pPersistence ) :
        m_pRepository( pRepository ), m_pPersistence( pPersistence ) {
	for ( int index = 0; index < s_nMaxRegionSlots; ++index ) {
		m_preparedByRegion[( size_t )( index )] = ( int16_t )-1;
		m_workByRegion[( size_t )( index )] = ( int16_t )-1;
		m_queuedNext[( size_t )( index )] = ( int16_t )-1;
		m_queuedPrev[( size_t )( index )] = ( int16_t )-1;
	}
	for ( int priority = 0; priority < 256; ++priority ) {
		m_priorityHeads[( size_t )( priority )] = ( int16_t )-1;
		m_priorityTails[( size_t )( priority )] = ( int16_t )-1;
	}
	m_nFreePayloadCount = s_nGpuCacheMaxLayers;
	for ( int slot = 0; slot < s_nGpuCacheMaxLayers; ++slot ) {
		m_freePayloadSlots[( size_t )( slot )] = ( int16_t )( s_nGpuCacheMaxLayers - slot - 1 );
	}
}

Terrain3DRuntimePageStore::~Terrain3DRuntimePageStore( void ) {
	poll( true );
	stopWorkers();
	reset();
}

void Terrain3DRuntimePageStore::reset( void ) {
	poll( true );
	for ( int index = 0; index < m_nPreparedPageCount; ++index ) {
		const int payload_slot = m_preparedPages[( size_t )( index )].m_iPayloadSlot;
		if ( payload_slot >= 0 && payload_slot < s_nGpuCacheMaxLayers ) {
			t3dRuntimePageStoreReleasePayload( m_payloadSlots[( size_t )( payload_slot )] );
		}
		t3dRuntimePageStoreResetMeta( m_preparedPages[( size_t )( index )] );
	}
	for ( int index = 0; index < m_nPrepareWorkPageCount; ++index ) {
		const int payload_slot = m_workPages[( size_t )( index )].m_iPayloadSlot;
		if ( payload_slot >= 0 && payload_slot < s_nGpuCacheMaxLayers ) {
			t3dRuntimePageStoreReleasePayload( m_payloadSlots[( size_t )( payload_slot )] );
		}
		t3dRuntimePageStoreResetMeta( m_workPages[( size_t )( index )] );
	}
	m_nPreparedPageCount = 0;
	m_nPrepareWorkPageCount = 0;
	m_nQueuedCount = 0;
	m_nStreamingGenerationLast = 0u;
	m_nSourceTopologyGeneration = 0u;
	m_sourceDataDirectory = String();
	m_bRuntimeSourcesDirty = true;
	memset( m_runtimeChannelCapacities.data(), 0, sizeof( m_runtimeChannelCapacities ) );
	memset( m_queuedConsumers.data(), 0, sizeof( m_queuedConsumers ) );
	memset( m_queuedPriorities.data(), 0, sizeof( m_queuedPriorities ) );
	for ( int index = 0; index < s_nMaxRegionSlots; ++index ) {
		m_preparedByRegion[( size_t )( index )] = ( int16_t )-1;
		m_workByRegion[( size_t )( index )] = ( int16_t )-1;
		m_queuedNext[( size_t )( index )] = ( int16_t )-1;
		m_queuedPrev[( size_t )( index )] = ( int16_t )-1;
		t3dRuntimePageStoreResetSource( m_pageSources[( size_t )( index )] );
	}
	for ( int priority = 0; priority < 256; ++priority ) {
		m_priorityHeads[( size_t )( priority )] = ( int16_t )-1;
		m_priorityTails[( size_t )( priority )] = ( int16_t )-1;
	}
	m_nFreePayloadCount = s_nGpuCacheMaxLayers;
	for ( int slot = 0; slot < s_nGpuCacheMaxLayers; ++slot ) {
		m_freePayloadSlots[( size_t )( slot )] = ( int16_t )( s_nGpuCacheMaxLayers - slot - 1 );
	}
	{
		t3dScopedLock_t lock( m_workerMutex );
		m_iWorkRead = 0;
		m_iWorkWrite = 0;
		m_nWorkRingCount = 0;
		m_iCompletionRead = 0;
		m_iCompletionWrite = 0;
		m_nCompletionRingCount = 0;
	}
	m_bPrepareJobActive = false;
	m_nPrepareJobWorkers = 0;
	resetFrameStats();
}

void Terrain3DRuntimePageStore::resetFrameStats( void ) {
	m_nPrepareLatencyUsecLast = 0u;
	m_nPrepareQueueWaitUsecLast = 0u;
	m_nPrepareFileOpenUsecLast = 0u;
	m_nPrepareFileReadUsecLast = 0u;
	m_nPrepareValidationUsecLast = 0u;
	m_nPrepareMaterializeUsecLast = 0u;
	m_nPrepareWorkerUsecLast = 0u;
	m_nPrepareReadyWaitUsecLast = 0u;
}

bool Terrain3DRuntimePageStore::loadSourceRegion( const Vector2i &rLocation, Ref<Terrain3DRegion> &rSource ) const {
	rSource.unref();
	if ( !m_pRepository ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayloadInternal() ) {
		rSource = Ref<Terrain3DRegion>( pRegion );
		return true;
	}
	return m_pPersistence && m_pPersistence->loadRuntimeRegionSource( pRegion->get_path(), m_pRepository->getRegionSize(), rSource );
}

String Terrain3DRuntimePageStore::getPagePath( const Terrain3DRegion *const pRegion, const String &rDataDirectory ) const {
	if ( !pRegion ) {
		return String();
	}
	if ( !rDataDirectory.is_empty() ) {
		const String source_path =
		        rDataDirectory.trim_suffix( "/" ) + String( "/" ) + Terrain3DUtil::locationToFilename( pRegion->getLocation() );
		return source_path.get_basename() + String( ".t3dpage" );
	}
	return pRegion->get_path().is_empty() ? String() : pRegion->get_path().get_basename() + String( ".t3dpage" );
}

uint64_t Terrain3DRuntimePageStore::computeSourceSignature(
        const Terrain3DRegion *const pRegion, const t3dRuntimePageConfig_t &rConfig ) const {
	if ( !pRegion || !m_pRepository ) {
		return 0u;
	}
	const int region_size = m_pRepository->getRegionSize();
	uint64_t hash = 1469598103934665603ull;
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( region_size ) );
	t3dRuntimePageStoreFnv1a64AppendF32( hash, ( float )( m_pRepository->getVertexSpacing() ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( rConfig.m_nColorDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( rConfig.m_nDerivedDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( rConfig.m_nMiddleHeightDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, s_nRuntimePageBlendRecipeVersion );
	t3dRuntimePageStoreFnv1a64AppendU64( hash, rConfig.m_nMaterialSourceSignature );
	t3dRuntimePageStoreFnv1a64AppendU64( hash, rConfig.m_nCollisionSurfaceSignature );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( pRegion->getLocation().x ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( pRegion->getLocation().y ) );

	Ref<Terrain3DRegion> pLoadedOwner;
	const Terrain3DRegion *pOwner = pRegion;
	if ( !pOwner->hasRuntimeMapPayloadInternal() && loadSourceRegion( pOwner->getLocation(), pLoadedOwner ) ) {
		pOwner = pLoadedOwner.ptr();
	}
	if ( !pOwner ) {
		return 0u;
	}
	const Ref<Image> pHeight = pOwner->getHeightMap();
	if ( pHeight.is_valid() ) {
		const PackedByteArray bytes = pHeight->get_data();
		t3dRuntimePageStoreFnv1a64Append( hash, bytes.ptr(), ( size_t )( bytes.size() ) );
	}
	const Ref<Image> pColor = pOwner->getColorMap();
	if ( pColor.is_valid() ) {
		const PackedByteArray bytes = pColor->get_data();
		t3dRuntimePageStoreFnv1a64Append( hash, bytes.ptr(), ( size_t )( bytes.size() ) );
	}
	const PackedByteArray instance_page = pOwner->getInstancePage();
	if ( !instance_page.is_empty() ) {
		t3dRuntimePageStoreFnv1a64Append( hash, instance_page.ptr(), ( size_t )( instance_page.size() ) );
	}

	const Ref<Terrain3DRegionLighting> lighting = pOwner->getLighting();
	if ( lighting.is_valid() ) {
		t3dRuntimePageStoreFnv1a64AppendU64( hash, ( uint64_t )( lighting->getBakeSignature() ) );
		t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( lighting->getShadowmaskMode() ) );
		t3dRuntimePageStoreFnv1a64AppendString( hash, lighting->getDirectionalLightSignature() );
		t3dRuntimePageStoreFnv1a64AppendString( hash, lighting->getTerrainShadowMaskPath() );
	} else {
		t3dRuntimePageStoreFnv1a64AppendU64( hash, 0u );
	}

	const eastl::array<Vector2i, 3> height_dependency_offsets = { Vector2i( 1, 0 ), Vector2i( 0, 1 ), Vector2i( 1, 1 ) };
	for ( size_t dependency = 0u; dependency < height_dependency_offsets.size(); ++dependency ) {
		const Vector2i location = pRegion->getLocation() + height_dependency_offsets[dependency];
		Terrain3DRegion *region = m_pRepository->getRegionPtr( location );
		Ref<Terrain3DRegion> pLoadedSource;
		if ( ( !region || region->getHeightMap().is_null() ) && loadSourceRegion( location, pLoadedSource ) ) {
			region = pLoadedSource.ptr();
		}
		t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( dependency ) );
		const bool dependency_valid = region && !region->isDeleted()
		        && ( dependency < 2 ? t3dRuntimePageStoreFnv1a64AppendR16HeightEdge(
		                                      hash, region->getHeightMap(), region_size, dependency == 0 )
		                            : t3dRuntimePageStoreFnv1a64AppendR16HeightCorner( hash, region->getHeightMap(), region_size ) );
		if ( !dependency_valid ) {
			t3dRuntimePageStoreFnv1a64AppendU32( hash, 0u );
			continue;
		}
		t3dRuntimePageStoreFnv1a64AppendU32( hash, 1u );
	}

	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = pRegion->getLocation() + Vector2i( dx, dz );
			Terrain3DRegion *region = m_pRepository->getRegionPtr( location );
			Ref<Terrain3DRegion> pLoadedSource;
			if ( ( !region || region->getControlMap().is_null() ) && loadSourceRegion( location, pLoadedSource ) ) {
				region = pLoadedSource.ptr();
			}
			t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( dx + 1 ) );
			t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t )( dz + 1 ) );
			if ( !region || region->isDeleted() ) {
				t3dRuntimePageStoreFnv1a64AppendU32( hash, 0u );
				continue;
			}
			t3dRuntimePageStoreFnv1a64AppendU32( hash, 1u );
			const Ref<Image> pControl = region->getControlMap();
			if ( pControl.is_valid() ) {
				const PackedByteArray bytes = pControl->get_data();
				t3dRuntimePageStoreFnv1a64Append( hash, bytes.ptr(), ( size_t )( bytes.size() ) );
			} else {
				t3dRuntimePageStoreFnv1a64AppendU32( hash, s_nRegionDefaultControl );
			}
		}
	}
	return hash;
}

uint64_t Terrain3DRuntimePageStore::resolveExpectedSourceSignature(
        const Terrain3DRegion *const pRegion, const t3dRuntimePageConfig_t &rConfig ) const {
	if ( !pRegion ) {
		return 0u;
	}

	// A world manifest may already provide an authoritative cook signature. In
	// editor authoring, however, the live region resource is the authority: a
	// copied/restored .t3dpage can have a newer filesystem timestamp while still
	// containing stale height data. Always derive the expected signature from the
	// current authored source there instead of treating a missing manifest
	// signature as a wildcard.
	if ( rConfig.m_bEditorAuthoring ) {
		return computeSourceSignature( pRegion, rConfig );
	}
	return pRegion->getCookedPageSourceSignatureInternal();
}

bool Terrain3DRuntimePageStore::probePage( const String &rPagePath, t3dRuntimePageProbe_t &rProbe ) const {
	rProbe = t3dRuntimePageProbe_t{};
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_MISSING;
		return true;
	}

	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}
	rProbe.m_nFileBytes = ( uint64_t )( pFile->get_length() );
	// Only magic/version/header-size are treated as the cross-version discovery
	// prefix. Legacy payload/header layouts are never interpreted by migration.

	if ( rProbe.m_nFileBytes < s_nDiscoveryBytes ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}
	const PackedByteArray discovery = pFile->get_buffer( s_nDiscoveryBytes );
	if ( discovery.size() != s_nDiscoveryBytes ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}
	const uint8_t *pRead = discovery.ptr();
	const uint32_t magic = t3dRuntimePageStoreReadU32Raw( pRead );
	rProbe.m_nVersion = t3dRuntimePageStoreReadU32Raw( pRead );
	rProbe.m_nHeaderBytes = t3dRuntimePageStoreReadU32Raw( pRead );
	if ( magic != s_nRuntimePageMagic || rProbe.m_nVersion == 0u || rProbe.m_nHeaderBytes < s_nDiscoveryBytes
	        || rProbe.m_nHeaderBytes > s_nRuntimePageMaxBytes ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}

	if ( rProbe.m_nVersion < s_nRuntimePageVersion ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_LEGACY;
		return true;
	}
	if ( rProbe.m_nVersion > s_nRuntimePageVersion ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_FUTURE;
		return true;
	}

	if ( rProbe.m_nHeaderBytes != s_nRuntimePageHeaderBytes || rProbe.m_nFileBytes < s_nRuntimePageHeaderBytes ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}
	pFile->seek( 0u );
	const PackedByteArray header_bytes = pFile->get_buffer( s_nRuntimePageHeaderBytes );
	if ( header_bytes.size() != s_nRuntimePageHeaderBytes ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	if ( !t3dRuntimePageValidateHeader( header, rProbe.m_nFileBytes ) ) {
		rProbe.m_state = RUNTIME_PAGE_PROBE_INVALID;
		return false;
	}
	rProbe.m_nRegionSize = header.m_nRegionSize;
	rProbe.m_nTotalBytes = header.m_nTotalBytes;
	rProbe.m_nChannelCount = header.m_nChannelCount;
	rProbe.m_state = RUNTIME_PAGE_PROBE_CURRENT;
	return true;
}

bool Terrain3DRuntimePageStore::readSourceSignature( const String &rPagePath, uint64_t &nSignature ) const {
	nSignature = 0u;
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return false;
	}
	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nRuntimePageHeaderBytes ) {
		return false;
	}
	const PackedByteArray header_bytes = pFile->get_buffer( s_nRuntimePageHeaderBytes );
	if ( header_bytes.size() != s_nRuntimePageHeaderBytes ) {
		return false;
	}
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	if ( !t3dRuntimePageValidateHeader( header, pFile->get_length() ) ) {
		return false;
	}
	nSignature = ( uint64_t )( header.m_nSourceSignatureLo ) | ( ( uint64_t )( header.m_nSourceSignatureHi ) << 32u );
	return nSignature != 0u;
}

uint32_t Terrain3DRuntimePageStore::readMaterialMask( const String &rPagePath ) const {
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return 0u;
	}
	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nRuntimePageHeaderBytes ) {
		return 0u;
	}
	const PackedByteArray header_bytes = pFile->get_buffer( s_nRuntimePageHeaderBytes );
	if ( header_bytes.size() != s_nRuntimePageHeaderBytes ) {
		return 0u;
	}
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	return t3dRuntimePageValidateHeader( header, pFile->get_length() ) ? header.m_nMaterialMask : 0u;
}

bool Terrain3DRuntimePageStore::removePageForSource( const String &rSourcePath ) const {
	return removePage( rSourcePath.is_empty() ? String() : rSourcePath.get_basename() + String( ".t3dpage" ) );
}

bool Terrain3DRuntimePageStore::removePage( const String &rPagePath ) const {
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return true;
	}
	const bool removed = DirAccess::remove_absolute( rPagePath ) == OK;
	if ( removed ) {
		t3dRuntimePageStoreUpdateEditorFile( rPagePath );
	}
	return removed;
}

bool Terrain3DRuntimePageStore::loadSnapshot( t3dRuntimePageMeta_t &rMeta,
        t3dRuntimePagePayload_t &rPayload,
        t3dRuntimePageWorkerScratch_t &rScratch,
        const t3dRuntimePageSource_t &source,
        const bool bPreserveMaterialized ) const {
	if ( !bPreserveMaterialized ) {
		t3dRuntimePageStoreResetPayloadRuntime( rPayload );
	}
	rMeta.m_nMaterialMask = 1u;
	rMeta.m_nFileOpenUsec = 0u;
	rMeta.m_nFileReadUsec = 0u;
	rMeta.m_nValidationUsec = 0u;
	rMeta.m_nMaterializeUsec = 0u;
	if ( !source.m_bPresent ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	uint64_t validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	if ( source.m_pagePath.is_empty() || !FileAccess::file_exists( source.m_pagePath ) ) {
		rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	const uint64_t page_modified = rMeta.m_bEditorAuthoring ? FileAccess::get_modified_time( source.m_pagePath ) : 0u;
	const uint64_t source_modified =
	        rMeta.m_bEditorAuthoring && !source.m_sourcePath.is_empty() ? FileAccess::get_modified_time( source.m_sourcePath ) : 0u;
	if ( rMeta.m_bEditorAuthoring && source_modified > 0u && page_modified > 0u && page_modified < source_modified ) {
		rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;

	const uint64_t open_started = pTime ? pTime->get_ticks_usec() : 0u;
	const Ref<FileAccess> pFile = FileAccess::open( source.m_pagePath, FileAccess::READ );
	const uint64_t open_finished = pTime ? pTime->get_ticks_usec() : open_started;
	rMeta.m_nFileOpenUsec += open_finished - open_started;
	if ( pFile.is_null() ) {
		return false;
	}
	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const uint64_t file_length = pFile->get_length();
	if ( file_length < s_nRuntimePageHeaderBytes || file_length > s_nRuntimePageMaxBytes || file_length > ( uint64_t )( INT32_MAX ) ) {
		rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;

	const uint64_t header_read_started = pTime ? pTime->get_ticks_usec() : 0u;
	if ( rScratch.m_ioBytes.size() < s_nRuntimePageHeaderBytes ) {
		return false;
	}
	const uint64_t header_bytes = pFile->get_buffer( rScratch.m_ioBytes.ptrw(), s_nRuntimePageHeaderBytes );
	const uint64_t header_read_finished = pTime ? pTime->get_ticks_usec() : header_read_started;
	rMeta.m_nFileReadUsec += header_read_finished - header_read_started;
	if ( header_bytes != s_nRuntimePageHeaderBytes ) {
		return false;
	}
	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeaderRaw( rScratch.m_ioBytes.ptr(), s_nRuntimePageHeaderBytes );
	const uint64_t header_source_signature = ( uint64_t )header.m_nSourceSignatureLo | ( ( uint64_t )header.m_nSourceSignatureHi << 32u );
	if ( !t3dRuntimePageValidateHeader( header, file_length ) || header.m_nRegionSize != ( uint32_t )( rMeta.m_nRegionSize )
	        || header.m_nColorDivisor != ( uint32_t )( rMeta.m_nColorDivisor )
	        || header.m_nDerivedDivisor != ( uint32_t )( rMeta.m_nDerivedDivisor )
	        || header.m_nMiddleHeightDivisor != ( uint32_t )( rMeta.m_nMiddleHeightDivisor ) || header.m_nRegionX != rMeta.m_location.x
	        || header.m_nRegionZ != rMeta.m_location.y || header.m_nBlendRecipeVersion != s_nRuntimePageBlendRecipeVersion
	        || header_source_signature == 0u || ( rMeta.m_bEditorAuthoring && rMeta.m_nExpectedSourceSignature == 0u )
	        || ( rMeta.m_nExpectedSourceSignature != 0u && header_source_signature != rMeta.m_nExpectedSourceSignature ) ) {
		rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	const uint32_t optional_mask = ( uint32_t )( REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR );
	const uint32_t expected_flags = rMeta.m_nExpectedChannelFlags & optional_mask;
	if ( ( header.m_nChannelFlags & optional_mask ) != expected_flags
	        || ( header.m_nChannelFlags & ( uint32_t )( REGION_CHANNEL_DERIVED ) ) == 0u
	        || !t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_HOLE_MASK )
	        || !t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT )
	        || !t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS )
	        || !t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_NAVIGATION )
	        || !t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT ) ) {
		rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;

	const t3dRuntimePageChannelInfo_t *const pHeightChannel = t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_HEIGHT );
	if ( !pHeightChannel ) {
		return false;
	}
	const bool bNeedGpu = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) != 0u;
	const bool bNeedCpu = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_CPU_MAPS ) != 0u;
	const bool bNeedActiveHeight = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT ) != 0u;
	const bool bNeedMiddle = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT ) != 0u;
	const bool bNeedTerrainShadow = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW ) != 0u;
	const bool bNeedHeight = bNeedGpu || bNeedCpu || bNeedActiveHeight;
	const bool bMaterializeHeight = bNeedGpu || bNeedCpu;
	if ( bNeedHeight ) {
		if ( !bPreserveMaterialized || rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_HEIGHT] == 0u ) {
			if ( !t3dRuntimePageStoreReadImageFixed( pFile,
			             *pHeightChannel,
			             rPayload.m_heightBytes,
			             rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_HEIGHT],
			             rPayload.m_height,
			             rMeta.m_nFileReadUsec,
			             rMeta.m_nValidationUsec,
			             rMeta.m_nMaterializeUsec,
			             bMaterializeHeight ) ) {
				return false;
			}
		} else if ( bMaterializeHeight && rPayload.m_height.is_null() ) {
			const uint64_t materialize_started = pTime ? pTime->get_ticks_usec() : 0u;
			rPayload.m_height = Image::create_from_data( ( int )pHeightChannel->m_nWidth,
			        ( int )pHeightChannel->m_nHeight,
			        pHeightChannel->m_nMipCount > 1u,
			        Image::Format( pHeightChannel->m_nFormat ),
			        rPayload.m_heightBytes );
			const uint64_t materialize_finished = pTime ? pTime->get_ticks_usec() : materialize_started;
			rMeta.m_nMaterializeUsec += materialize_finished - materialize_started;
		}
		if ( rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_HEIGHT] == 0u ) {
			return false;
		}
	}
	rMeta.m_nMaterialMask = header.m_nMaterialMask;

	if ( bNeedGpu || bNeedCpu ) {
		const t3dRuntimePageChannelInfo_t *const pDerivedChannel = t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_DERIVED );
		if ( !pDerivedChannel ) {
			return false;
		}
		if ( ( !bPreserveMaterialized || rPayload.m_derived.is_null() )
		        && !t3dRuntimePageStoreReadImageFixed( pFile,
		                *pDerivedChannel,
		                rPayload.m_derivedBytes,
		                rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_DERIVED],
		                rPayload.m_derived,
		                rMeta.m_nFileReadUsec,
		                rMeta.m_nValidationUsec,
		                rMeta.m_nMaterializeUsec,
		                true ) ) {
			return false;
		}
	}
	if ( bNeedMiddle ) {
		const t3dRuntimePageChannelInfo_t *const pMiddleHeightChannel =
		        t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT );
		if ( !pMiddleHeightChannel ) {
			return false;
		}
		if ( ( !bPreserveMaterialized || rPayload.m_middleHeight.is_null() )
		        && !t3dRuntimePageStoreReadImageFixed( pFile,
		                *pMiddleHeightChannel,
		                rPayload.m_middleHeightBytes,
		                rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT],
		                rPayload.m_middleHeight,
		                rMeta.m_nFileReadUsec,
		                rMeta.m_nValidationUsec,
		                rMeta.m_nMaterializeUsec,
		                true ) ) {
			return false;
		}
	}
	if ( bNeedGpu ) {
		const t3dRuntimePageChannelInfo_t *const pMiddleCompositeChannel =
		        t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE );
		if ( !pMiddleCompositeChannel ) {
			return false;
		}
		if ( ( !bPreserveMaterialized || rPayload.m_middleComposite.is_null() )
		        && !t3dRuntimePageStoreReadImageFixed( pFile,
		                *pMiddleCompositeChannel,
		                rPayload.m_middleCompositeBytes,
		                rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE],
		                rPayload.m_middleComposite,
		                rMeta.m_nFileReadUsec,
		                rMeta.m_nValidationUsec,
		                rMeta.m_nMaterializeUsec,
		                true ) ) {
			return false;
		}
	}
	if ( bNeedTerrainShadow ) {
		const t3dRuntimePageChannelInfo_t *const pShadowChannel =
		        t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK );
		if ( !pShadowChannel ) {
			return false;
		}
		if ( ( !bPreserveMaterialized || rPayload.m_terrainShadowMask.is_null() )
		        && !t3dRuntimePageStoreReadImageFixed( pFile,
		                *pShadowChannel,
		                rPayload.m_terrainShadowMaskBytes,
		                rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK],
		                rPayload.m_terrainShadowMask,
		                rMeta.m_nFileReadUsec,
		                rMeta.m_nValidationUsec,
		                rMeta.m_nMaterializeUsec,
		                true ) ) {
			return false;
		}
	}
	if ( bNeedGpu || bNeedCpu ) {
		if ( ( expected_flags & ( uint32_t )( REGION_CHANNEL_CONTROL ) ) != 0u ) {
			const t3dRuntimePageChannelInfo_t *const pControlChannel = t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_CONTROL );
			if ( !pControlChannel ) {
				return false;
			}
			if ( ( !bPreserveMaterialized || rPayload.m_control.is_null() )
			        && !t3dRuntimePageStoreReadImageFixed( pFile,
			                *pControlChannel,
			                rPayload.m_controlBytes,
			                rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_CONTROL],
			                rPayload.m_control,
			                rMeta.m_nFileReadUsec,
			                rMeta.m_nValidationUsec,
			                rMeta.m_nMaterializeUsec,
			                true ) ) {
				return false;
			}
		}
		if ( ( expected_flags & ( uint32_t )( REGION_CHANNEL_COLOR ) ) != 0u ) {
			const t3dRuntimePageChannelInfo_t *const pColorChannel = t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLOR );
			if ( !pColorChannel ) {
				return false;
			}
			if ( ( !bPreserveMaterialized || rPayload.m_color.is_null() )
			        && !t3dRuntimePageStoreReadImageFixed( pFile,
			                *pColorChannel,
			                rPayload.m_colorBytes,
			                rPayload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_COLOR],
			                rPayload.m_color,
			                rMeta.m_nFileReadUsec,
			                rMeta.m_nValidationUsec,
			                rMeta.m_nMaterializeUsec,
			                true ) ) {
				return false;
			}
		}
	}

	const bool bNeedInstances = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u;
	const bool bNeedNodes = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS ) != 0u;
	const bool bNeedCollision = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_COLLISION ) != 0u;
	const bool bNeedNavigation = ( rMeta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_NAVIGATION ) != 0u;
	if ( bNeedInstances
	        && !t3dRuntimePageStoreReadOptionalBlob(
	                pFile, header, rPayload, rScratch, rMeta, TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES, rPayload.m_instancePage, false ) ) {
		return false;
	}
	if ( bNeedNodes
	        && !t3dRuntimePageStoreReadOptionalBlob( pFile,
	                header,
	                rPayload,
	                rScratch,
	                rMeta,
	                TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS,
	                rPayload.m_nodeDescriptors,
	                false ) ) {
		return false;
	}
	if ( bNeedCollision ) {
		if ( !t3dRuntimePageStoreReadOptionalBlob(
		             pFile, header, rPayload, rScratch, rMeta, TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT, rPayload.m_collisionHeight, true )
		        || !t3dRuntimePageStoreReadOptionalBlob( pFile,
		                header,
		                rPayload,
		                rScratch,
		                rMeta,
		                TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS,
		                rPayload.m_collisionBounds,
		                true )
		        || !t3dRuntimePageStoreReadOptionalBlob(
		                pFile, header, rPayload, rScratch, rMeta, TERRAIN3D_RUNTIME_PAGE_HOLE_MASK, rPayload.m_holeMask, true ) ) {
			return false;
		}
	}
	if ( bNeedNavigation
	        && !t3dRuntimePageStoreReadOptionalBlob(
	                pFile, header, rPayload, rScratch, rMeta, TERRAIN3D_RUNTIME_PAGE_NAVIGATION, rPayload.m_navigation, true ) ) {
		return false;
	}
	// Auxiliary acceleration blobs are materialized only for consumers that can use them.
	if ( rMeta.m_bLoadMinMax
	        && !t3dRuntimePageStoreReadOptionalBlob(
	                pFile, header, rPayload, rScratch, rMeta, TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT, rPayload.m_minMaxHeight, true ) ) {
		return false;
	}
	if ( bNeedInstances ) {
		t3dRuntimePageStoreReadOptionalBlob(
		        pFile, header, rPayload, rScratch, rMeta, TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA, rPayload.m_instanceMetadata, false );
		t3dRuntimePageStoreReadOptionalBlob( pFile,
		        header,
		        rPayload,
		        rScratch,
		        rMeta,
		        TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START,
		        rPayload.m_multimeshFastStart,
		        false );
	}

	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const int color_size = MAX( 1, rMeta.m_nRegionSize / rMeta.m_nColorDivisor );
	const int derived_size = MAX( 1, rMeta.m_nRegionSize / rMeta.m_nDerivedDivisor );
	const int middle_height_size = MAX( 1, ( rMeta.m_nRegionSize + rMeta.m_nMiddleHeightDivisor - 1 ) / rMeta.m_nMiddleHeightDivisor );
	if ( ( bMaterializeHeight
	             && !t3dRuntimePageStoreValidImage(
	                     rPayload.m_height, rMeta.m_nRegionSize, rMeta.m_nRegionSize, Image::FORMAT_R16, false ) )
	        || ( ( bNeedGpu || bNeedCpu )
	                && !t3dRuntimePageStoreValidImage( rPayload.m_derived, derived_size, derived_size, Image::FORMAT_RG8, false ) )
	        || ( bNeedMiddle
	                && !t3dRuntimePageStoreValidImage(
	                        rPayload.m_middleHeight, middle_height_size, middle_height_size, Image::FORMAT_R16, false ) )
	        || ( bNeedGpu
	                && !t3dRuntimePageStoreValidImage( rPayload.m_middleComposite, color_size, color_size, Image::FORMAT_RGBA8, true ) )
	        || ( bNeedTerrainShadow
	                && ( rPayload.m_terrainShadowMask.is_null() || rPayload.m_terrainShadowMask->is_empty()
	                        || rPayload.m_terrainShadowMask->get_format() != Image::FORMAT_L8
	                        || !rPayload.m_terrainShadowMask->has_mipmaps()
	                        || rPayload.m_terrainShadowMask->get_width() != rPayload.m_terrainShadowMask->get_height() ) )
	        || ( rPayload.m_control.is_valid()
	                && !t3dRuntimePageStoreValidImage(
	                        rPayload.m_control, rMeta.m_nRegionSize, rMeta.m_nRegionSize, Image::FORMAT_RF, false ) )
	        || ( rPayload.m_color.is_valid()
	                && !t3dRuntimePageStoreValidImage( rPayload.m_color, color_size, color_size, Image::FORMAT_RGBA8, true ) ) ) {
		rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rMeta.m_nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	return true;
}

bool Terrain3DRuntimePageStore::validatePageForRegion( const Terrain3DRegion *const pRegion, const t3dRuntimePageConfig_t &rConfig ) const {
	if ( !pRegion || !m_pRepository ) {
		return false;
	}
	t3dRuntimePageMeta_t meta;
	t3dRuntimePagePayload_t payload;
	t3dRuntimePageSource_t source;
	t3dRuntimePageWorkerScratch_t scratch;
	meta.m_location = pRegion->getLocation();
	meta.m_nExpectedChannelFlags = ( uint32_t )( pRegion->getChannelFlags() );
	meta.m_nExpectedSourceSignature = resolveExpectedSourceSignature( pRegion, rConfig );
	meta.m_nRegionSize = m_pRepository->getRegionSize();
	meta.m_nColorDivisor = rConfig.m_nColorDivisor;
	meta.m_nDerivedDivisor = rConfig.m_nDerivedDivisor;
	meta.m_nMiddleHeightDivisor = rConfig.m_nMiddleHeightDivisor;
	meta.m_nConsumerMask = RUNTIME_PAGE_CONSUMER_GPU_DETAIL | RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT;
	const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
	if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
		meta.m_nConsumerMask |= RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW;
	}
	meta.m_bEditorAuthoring = rConfig.m_bEditorAuthoring;
	source.m_pagePath = getPagePath( pRegion, rConfig.m_dataDirectory );
	source.m_sourcePath = pRegion->get_path();
	source.m_nExpectedChannelFlags = meta.m_nExpectedChannelFlags;
	source.m_nExpectedSourceSignature = meta.m_nExpectedSourceSignature;
	source.m_bPresent = true;
	if ( !t3dRuntimePageStorePreparePayloadForSourceCold( payload, scratch, source.m_pagePath ) ) {
		return false;
	}
	return loadSnapshot( meta, payload, scratch, source );
}

bool Terrain3DRuntimePageStore::loadCpuMapsNow( const Vector2i &rLocation,
        const t3dRuntimePageConfig_t &rConfig,
        Ref<Image> &rHeight,
        Ref<Image> &rControl,
        Ref<Image> &rColor,
        Ref<Image> &rDerived ) const {
	rHeight.unref();
	rControl.unref();
	rColor.unref();
	rDerived.unref();
	if ( !m_pRepository || !rConfig.m_bCookedPagesEnabled ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	t3dRuntimePageMeta_t meta;
	t3dRuntimePagePayload_t payload;
	t3dRuntimePageSource_t source;
	t3dRuntimePageWorkerScratch_t scratch;
	meta.m_location = rLocation;
	meta.m_nExpectedChannelFlags = ( uint32_t )pRegion->getChannelFlags();
	meta.m_nExpectedSourceSignature = resolveExpectedSourceSignature( pRegion, rConfig );
	meta.m_nRegionSize = m_pRepository->getRegionSize();
	meta.m_nColorDivisor = rConfig.m_nColorDivisor;
	meta.m_nDerivedDivisor = rConfig.m_nDerivedDivisor;
	meta.m_nMiddleHeightDivisor = rConfig.m_nMiddleHeightDivisor;
	meta.m_nConsumerMask = RUNTIME_PAGE_CONSUMER_CPU_MAPS;
	meta.m_bEditorAuthoring = rConfig.m_bEditorAuthoring;
	source.m_pagePath = getPagePath( pRegion, rConfig.m_dataDirectory );
	source.m_sourcePath = pRegion->get_path();
	source.m_nExpectedChannelFlags = meta.m_nExpectedChannelFlags;
	source.m_nExpectedSourceSignature = meta.m_nExpectedSourceSignature;
	source.m_bPresent = true;
	if ( !t3dRuntimePageStorePreparePayloadForSourceCold( payload, scratch, source.m_pagePath )
	        || !loadSnapshot( meta, payload, scratch, source ) ) {
		return false;
	}
	rHeight = payload.m_height;
	rControl = payload.m_control;
	rColor = payload.m_color;
	rDerived = payload.m_derived;
	return rHeight.is_valid() && rDerived.is_valid();
}

bool Terrain3DRuntimePageStore::loadCollisionNow( const Vector2i &rLocation,
        const t3dRuntimePageConfig_t &rConfig,
        PackedByteArray &rHeight,
        PackedByteArray &rBounds,
        PackedByteArray &rHoleMask ) const {
	rHeight.clear();
	rBounds.clear();
	rHoleMask.clear();
	if ( !m_pRepository || !rConfig.m_bCookedPagesEnabled ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	t3dRuntimePageMeta_t meta;
	t3dRuntimePagePayload_t payload;
	t3dRuntimePageSource_t source;
	t3dRuntimePageWorkerScratch_t scratch;
	meta.m_location = rLocation;
	meta.m_nExpectedChannelFlags = ( uint32_t )pRegion->getChannelFlags();
	meta.m_nExpectedSourceSignature = resolveExpectedSourceSignature( pRegion, rConfig );
	meta.m_nRegionSize = m_pRepository->getRegionSize();
	meta.m_nColorDivisor = rConfig.m_nColorDivisor;
	meta.m_nDerivedDivisor = rConfig.m_nDerivedDivisor;
	meta.m_nMiddleHeightDivisor = rConfig.m_nMiddleHeightDivisor;
	meta.m_nConsumerMask = RUNTIME_PAGE_CONSUMER_COLLISION;
	meta.m_bEditorAuthoring = rConfig.m_bEditorAuthoring;
	source.m_pagePath = getPagePath( pRegion, rConfig.m_dataDirectory );
	source.m_sourcePath = pRegion->get_path();
	source.m_nExpectedChannelFlags = meta.m_nExpectedChannelFlags;
	source.m_nExpectedSourceSignature = meta.m_nExpectedSourceSignature;
	source.m_bPresent = true;
	if ( !t3dRuntimePageStorePreparePayloadForSourceCold( payload, scratch, source.m_pagePath )
	        || !loadSnapshot( meta, payload, scratch, source ) ) {
		return false;
	}
	rHeight = payload.m_collisionHeight;
	rBounds = payload.m_collisionBounds;
	rHoleMask = payload.m_holeMask;
	return !rHeight.is_empty() && !rBounds.is_empty() && !rHoleMask.is_empty();
}

bool Terrain3DRuntimePageStore::loadNavigationNow( const Vector2i &rLocation,
        const t3dRuntimePageConfig_t &rConfig,
        PackedByteArray &rNavigation,
        PackedByteArray *const pCollisionHeight,
        PackedByteArray *const pMinMaxHeight ) const {
	rNavigation.clear();
	if ( pCollisionHeight ) {
		pCollisionHeight->clear();
	}
	if ( pMinMaxHeight ) {
		pMinMaxHeight->clear();
	}
	if ( !m_pRepository || !rConfig.m_bCookedPagesEnabled ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	t3dRuntimePageMeta_t meta;
	t3dRuntimePagePayload_t payload;
	t3dRuntimePageSource_t source;
	t3dRuntimePageWorkerScratch_t scratch;
	meta.m_location = rLocation;
	meta.m_nExpectedChannelFlags = ( uint32_t )pRegion->getChannelFlags();
	meta.m_nExpectedSourceSignature = resolveExpectedSourceSignature( pRegion, rConfig );
	meta.m_nRegionSize = m_pRepository->getRegionSize();
	meta.m_nColorDivisor = rConfig.m_nColorDivisor;
	meta.m_nDerivedDivisor = rConfig.m_nDerivedDivisor;
	meta.m_nMiddleHeightDivisor = rConfig.m_nMiddleHeightDivisor;
	meta.m_nConsumerMask = RUNTIME_PAGE_CONSUMER_NAVIGATION | ( pCollisionHeight ? RUNTIME_PAGE_CONSUMER_COLLISION : 0u );
	meta.m_bLoadMinMax = pMinMaxHeight != NULL;
	meta.m_bEditorAuthoring = rConfig.m_bEditorAuthoring;
	source.m_pagePath = getPagePath( pRegion, rConfig.m_dataDirectory );
	source.m_sourcePath = pRegion->get_path();
	source.m_nExpectedChannelFlags = meta.m_nExpectedChannelFlags;
	source.m_nExpectedSourceSignature = meta.m_nExpectedSourceSignature;
	source.m_bPresent = true;
	if ( !t3dRuntimePageStorePreparePayloadForSourceCold( payload, scratch, source.m_pagePath )
	        || !loadSnapshot( meta, payload, scratch, source ) ) {
		return false;
	}
	rNavigation = payload.m_navigation;
	if ( pCollisionHeight ) {
		*pCollisionHeight = payload.m_collisionHeight;
	}
	if ( pMinMaxHeight ) {
		*pMinMaxHeight = payload.m_minMaxHeight;
	}
	return !rNavigation.is_empty() && ( !pCollisionHeight || !pCollisionHeight->is_empty() )
	        && ( !pMinMaxHeight || !pMinMaxHeight->is_empty() );
}

bool Terrain3DRuntimePageStore::loadLightingReceiversNow(
        const Vector2i &rLocation, const t3dRuntimePageConfig_t &rConfig, PackedByteArray &rReceivers ) const {
	rReceivers.clear();
	if ( !m_pRepository || !rConfig.m_bCookedPagesEnabled ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	const String path = getPagePath( pRegion, rConfig.m_dataDirectory );
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nRuntimePageHeaderBytes ) {
		return false;
	}
	const PackedByteArray header_bytes = pFile->get_buffer( s_nRuntimePageHeaderBytes );
	if ( header_bytes.size() != s_nRuntimePageHeaderBytes ) {
		return false;
	}
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	const uint64_t header_source_signature = ( uint64_t )header.m_nSourceSignatureLo | ( ( uint64_t )header.m_nSourceSignatureHi << 32u );
	const uint64_t expected_source_signature = resolveExpectedSourceSignature( pRegion, rConfig );
	if ( !t3dRuntimePageValidateHeader( header, pFile->get_length() ) || header.m_nRegionSize != ( uint32_t )m_pRepository->getRegionSize()
	        || header.m_nRegionX != rLocation.x || header.m_nRegionZ != rLocation.y
	        || header.m_nBlendRecipeVersion != s_nRuntimePageBlendRecipeVersion || header_source_signature == 0u
	        || ( rConfig.m_bEditorAuthoring && expected_source_signature == 0u )
	        || ( expected_source_signature != 0u && header_source_signature != expected_source_signature ) ) {
		return false;
	}
	const t3dRuntimePageChannelInfo_t *const pChannel = t3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS );
	if ( !pChannel ) {
		return true;
	}
	uint64_t read_usec = 0u, validation_usec = 0u;
	rReceivers = t3dRuntimePageStoreReadBlob( pFile, *pChannel, read_usec, validation_usec );
	return !rReceivers.is_empty();
}

static bool t3dRuntimePageStoreWriteFail( String *const pError, const String &rMessage ) {
	if ( pError ) {
		*pError = rMessage;
	}
	return false;
}

bool Terrain3DRuntimePageStore::writePage( Terrain3DRegion *const pRegion,
        const t3dRuntimePageConfig_t &rConfig,
        const t3dRuntimePageCookPayload_t &rPayload,
        String *const pError ) {
	if ( pError ) {
		*pError = String();
	}
	if ( !pRegion || pRegion->isDeleted() || !m_pRepository ) {
		return t3dRuntimePageStoreWriteFail( pError, "region/repository is unavailable" );
	}
	const int region_size = m_pRepository->getRegionSize();
	const int color_size = MAX( 1, region_size / rConfig.m_nColorDivisor );
	const int derived_size = MAX( 1, region_size / rConfig.m_nDerivedDivisor );
	const int middle_height_size = MAX( 1, ( region_size + rConfig.m_nMiddleHeightDivisor - 1 ) / rConfig.m_nMiddleHeightDivisor );
	if ( !t3dRuntimePageStoreValidImage( rPayload.m_height, region_size, region_size, Image::FORMAT_R16, false ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "height source is missing or has an invalid size/format" );
	}
	if ( rPayload.m_control.is_valid()
	        && !t3dRuntimePageStoreValidImage( rPayload.m_control, region_size, region_size, Image::FORMAT_RF, false ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "render-control page has an invalid size/format" );
	}
	if ( rPayload.m_color.is_valid()
	        && !t3dRuntimePageStoreValidImage( rPayload.m_color, color_size, color_size, Image::FORMAT_RGBA8, true ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "color page has an invalid size/format" );
	}
	if ( !t3dRuntimePageStoreValidImage( rPayload.m_derived, derived_size, derived_size, Image::FORMAT_RG8, false ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "derived normal/slope page has an invalid size/format" );
	}
	if ( !t3dRuntimePageStoreValidImage( rPayload.m_middleHeight, middle_height_size, middle_height_size, Image::FORMAT_R16, false ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "middle-height page has an invalid size/format" );
	}
	if ( !t3dRuntimePageStoreValidImage( rPayload.m_middleComposite, color_size, color_size, Image::FORMAT_RGBA8, true ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "Middle composite page has an invalid size/format" );
	}
	if ( rPayload.m_terrainShadowMask.is_valid()
	        && ( rPayload.m_terrainShadowMask->is_empty() || rPayload.m_terrainShadowMask->get_format() != Image::FORMAT_L8
	                || !rPayload.m_terrainShadowMask->has_mipmaps()
	                || rPayload.m_terrainShadowMask->get_width() != rPayload.m_terrainShadowMask->get_height() ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "terrain shadow mask must be a square mipmapped L8 image" );
	}
	const uint32_t optional_flags =
	        ( uint32_t )( pRegion->getChannelFlags() ) & ( uint32_t )( REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR );
	if ( ( ( optional_flags & REGION_CHANNEL_CONTROL ) != 0u ) != rPayload.m_control.is_valid()
	        || ( ( optional_flags & REGION_CHANNEL_COLOR ) != 0u ) != rPayload.m_color.is_valid() ) {
		return t3dRuntimePageStoreWriteFail( pError, "optional cooked channels do not match region channel metadata" );
	}
	if ( rPayload.m_nMaterialMask == 0u ) {
		return t3dRuntimePageStoreWriteFail( pError, "material mask is empty" );
	}
	// loadSnapshot() rejects any page missing these five channels, so a page written
	// without them would publish, fail its own reload validation, and be deleted with
	// only a generic message. Reject here instead, naming the channel that is absent.
	if ( rPayload.m_holeMask.is_empty() ) {
		return t3dRuntimePageStoreWriteFail( pError, "hole mask is missing; cooked pages require it" );
	}
	if ( rPayload.m_collisionHeight.is_empty() ) {
		return t3dRuntimePageStoreWriteFail( pError, "collision height surface is missing; cooked pages require it" );
	}
	if ( rPayload.m_collisionBounds.is_empty() ) {
		return t3dRuntimePageStoreWriteFail( pError, "collision bounds are missing; cooked pages require them" );
	}
	if ( rPayload.m_navigation.is_empty() ) {
		return t3dRuntimePageStoreWriteFail( pError, "navigation mask is missing; cooked pages require it" );
	}
	if ( rPayload.m_minMaxHeight.is_empty() ) {
		return t3dRuntimePageStoreWriteFail( pError, "min/max height hierarchy is missing; cooked pages require it" );
	}

	const String page_path = getPagePath( pRegion, rConfig.m_dataDirectory );
	if ( page_path.is_empty() ) {
		return t3dRuntimePageStoreWriteFail( pError, "runtime page path is empty" );
	}
	const uint64_t source_signature = computeSourceSignature( pRegion, rConfig );
	if ( source_signature == 0u ) {
		return t3dRuntimePageStoreWriteFail( pError, "source signature generation failed" );
	}

	t3dRuntimePageHeader_t header;
	header.m_nRegionSize = ( uint32_t )( region_size );
	header.m_nColorDivisor = ( uint32_t )( rConfig.m_nColorDivisor );
	header.m_nDerivedDivisor = ( uint32_t )( rConfig.m_nDerivedDivisor );
	header.m_nChannelFlags = ( uint32_t )( pRegion->getChannelFlags() ) | ( uint32_t )( REGION_CHANNEL_DERIVED );
	header.m_nSourceSignatureLo = ( uint32_t )( source_signature & 0xffffffffu );
	header.m_nSourceSignatureHi = ( uint32_t )( source_signature >> 32u );
	header.m_nBlendRecipeVersion = s_nRuntimePageBlendRecipeVersion;
	header.m_nMaterialMask = rPayload.m_nMaterialMask;
	header.m_nMiddleHeightDivisor = ( uint32_t )( rConfig.m_nMiddleHeightDivisor );
	uint64_t world_signature = 1469598103934665603ull;
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, ( uint32_t )( region_size ) );
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, ( uint32_t )( rConfig.m_nColorDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, ( uint32_t )( rConfig.m_nDerivedDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, s_nRuntimePageBlendRecipeVersion );
	header.m_nWorldSignatureLo = ( uint32_t )( world_signature & 0xffffffffu );
	header.m_nWorldSignatureHi = ( uint32_t )( world_signature >> 32u );
	header.m_nRegionX = pRegion->getLocation().x;
	header.m_nRegionZ = pRegion->getLocation().y;
	const Vector2 height_range = pRegion->getHeightRange();
	const float min_height = ( float )( height_range.x );
	const float max_height = ( float )( height_range.y );
	memcpy( &header.m_nBoundsMinYBits, &min_height, sizeof( uint32_t ) );
	memcpy( &header.m_nBoundsMaxYBits, &max_height, sizeof( uint32_t ) );

	t3dStagedPageBuild_t page_build;
	PackedByteArray file_bytes;
	const PackedByteArray instance_page = pRegion->getInstancePage();
	if ( !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_HEIGHT, rPayload.m_height )
	        || ( rPayload.m_control.is_valid()
	                && !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_CONTROL, rPayload.m_control ) )
	        || ( rPayload.m_color.is_valid()
	                && !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_COLOR, rPayload.m_color ) )
	        || !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_DERIVED, rPayload.m_derived )
	        || !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT, rPayload.m_middleHeight )
	        || !t3dRuntimePageStoreStageImageChannel(
	                page_build, header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE, rPayload.m_middleComposite )
	        || ( rPayload.m_terrainShadowMask.is_valid()
	                && !t3dRuntimePageStoreStageImageChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK, rPayload.m_terrainShadowMask ) )
	        || ( !instance_page.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES, instance_page ) )
	        || ( !rPayload.m_holeMask.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_HOLE_MASK, rPayload.m_holeMask ) )
	        || ( !rPayload.m_collisionHeight.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT, rPayload.m_collisionHeight ) )
	        || ( !rPayload.m_collisionBounds.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS, rPayload.m_collisionBounds ) )
	        || ( !rPayload.m_navigation.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_NAVIGATION, rPayload.m_navigation ) )
	        || ( !rPayload.m_minMaxHeight.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT, rPayload.m_minMaxHeight ) )
	        || ( !rPayload.m_nodeDescriptors.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS, rPayload.m_nodeDescriptors ) )
	        || ( !rPayload.m_instanceMetadata.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA, rPayload.m_instanceMetadata ) )
	        || ( !rPayload.m_lightingReceivers.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS, rPayload.m_lightingReceivers ) )
	        || ( !rPayload.m_multimeshFastStart.is_empty()
	                && !t3dRuntimePageStoreStageBlobChannel(
	                        page_build, header, TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START, rPayload.m_multimeshFastStart ) )
	        || !t3dRuntimePageStoreFinalizePageBytes( page_build, header, file_bytes ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "runtime page channel serialization failed" );
	}
	if ( !t3dRuntimePageValidateHeader( header, ( uint64_t )( file_bytes.size() ) ) ) {
		return t3dRuntimePageStoreWriteFail( pError, "generated runtime page header failed validation" );
	}

	const String temporary_path = page_path + String( ".tmp" );
	const String backup_path = page_path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		return t3dRuntimePageStoreWriteFail( pError, "temporary runtime page could not be opened for writing" );
	}
	pFile->store_buffer( file_bytes );
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return t3dRuntimePageStoreWriteFail( pError, "temporary runtime page write failed" );
	}
	const bool bHadPrevious = FileAccess::file_exists( page_path );
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( bHadPrevious && DirAccess::rename_absolute( page_path, backup_path ) != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return t3dRuntimePageStoreWriteFail( pError, "existing runtime page could not be moved to backup" );
	}
	if ( DirAccess::rename_absolute( temporary_path, page_path ) != OK ) {
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, page_path );
		}
		DirAccess::remove_absolute( temporary_path );
		return t3dRuntimePageStoreWriteFail( pError, "temporary runtime page could not be published" );
	}

	if ( !validatePageForRegion( pRegion, rConfig ) ) {
		DirAccess::remove_absolute( page_path );
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, page_path );
		}
		return t3dRuntimePageStoreWriteFail( pError, "published runtime page failed reload validation" );
	}
	if ( bHadPrevious ) {
		DirAccess::remove_absolute( backup_path );
	}
	pRegion->setCookedPageSourceSignatureInternal( source_signature );
	// The source table may have been built before an editor edit/save cycle. Force
	// the next service pass to rebuild it so worker jobs cannot keep validating a
	// freshly cooked page against the pre-edit signature.
	m_bRuntimeSourcesDirty = true;
	t3dRuntimePageStoreUpdateEditorFile( page_path );
	return true;
}

bool Terrain3DRuntimePageStore::collectPageSources( const String &rDataDirectory,
        PackedStringArray &rSourceFiles,
        eastl::vector<Vector2i, Terrain3DEastlAllocator> &rLocations,
        PackedStringArray &rErrors ) const {
	rSourceFiles.clear();
	rLocations.clear();
	rErrors.clear();
	const String data_directory = rDataDirectory.trim_suffix( "/" );
	if ( !m_pRepository ) {
		rErrors.push_back( "Region repository is unavailable" );
		return false;
	}
	if ( data_directory.is_empty() ) {
		rErrors.push_back( "Terrain3D data_directory is empty" );
		return false;
	}

	rSourceFiles = Terrain3DUtil::getFiles( data_directory, "terrain3d*.res" );
	rSourceFiles.sort();
	if ( rSourceFiles.is_empty() ) {
		rErrors.push_back( String( "No Terrain3D region .res sources were found in " ) + data_directory );
		return false;
	}

	eastl::bitset<s_nActiveMaxRegionMapSize * s_nActiveMaxRegionMapSize> source_locations;
	rLocations.reserve( ( size_t )( rSourceFiles.size() ) );
	for ( size_t iFile = 0u; iFile < ( size_t )( ( rSourceFiles ).size() ); iFile++ ) {
		const String &fname = ( rSourceFiles )[iFile];
		const Vector2i location = Terrain3DUtil::filenameToLocation( fname );
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || !m_pRepository->isLocationValid( location ) || Terrain3DUtil::locationToFilename( location ) != fname ) {
			rErrors.push_back( String( "Malformed or out-of-range Terrain3D region source: " ) + fname );
			continue;
		}
		if ( source_locations.test( ( size_t )( map_index ) ) ) {
			rErrors.push_back( String( "Duplicate Terrain3D region source location " ) + String( location ) );
			continue;
		}
		source_locations.set( ( size_t )( map_index ) );

		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			rErrors.push_back( String( "Region source " ) + fname + String( " exists in the data directory but is not loaded" ) );
			continue;
		}
		const String expected_path = data_directory.path_join( fname ).simplify_path();
		const String loaded_path = pRegion->get_path().simplify_path();
		if ( loaded_path != expected_path ) {
			rErrors.push_back( String( "Region " ) + String( location ) + String( " is loaded from " )
			        + ( loaded_path.is_empty() ? String( "<unsaved resource>" ) : loaded_path )
			        + String( " instead of data-directory source " ) + expected_path );
			continue;
		}
		rLocations.push_back( location );
	}

	const TypedArray<Vector2i> loaded_locations = m_pRepository->getActiveLocations();
	for ( size_t iLocation = 0u; iLocation < ( size_t )( ( loaded_locations ).size() ); iLocation++ ) {
		const Vector2i location = ( loaded_locations )[iLocation];
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || !source_locations.test( ( size_t )( map_index ) ) ) {
			rErrors.push_back(
			        String( "Loaded region " ) + String( location ) + String( " has no matching .res source in " ) + data_directory );
		}
	}
	return rErrors.is_empty();
}

void Terrain3DRuntimePageStore::queueUnlink( const int nMapIndex ) {
	if ( nMapIndex < 0 || nMapIndex >= s_nMaxRegionSlots ) {
		return;
	}
	const uint8_t priority = m_queuedPriorities[( size_t )( nMapIndex )];
	if ( priority == 0u ) {
		m_queuedNext[( size_t )( nMapIndex )] = ( int16_t )-1;
		m_queuedPrev[( size_t )( nMapIndex )] = ( int16_t )-1;
		return;
	}
	const int previous = m_queuedPrev[( size_t )( nMapIndex )];
	const int next = m_queuedNext[( size_t )( nMapIndex )];
	if ( previous >= 0 ) {
		m_queuedNext[( size_t )( previous )] = ( int16_t )next;
	} else if ( m_priorityHeads[( size_t )( priority )] == nMapIndex ) {
		m_priorityHeads[( size_t )( priority )] = ( int16_t )next;
	}
	if ( next >= 0 ) {
		m_queuedPrev[( size_t )( next )] = ( int16_t )previous;
	} else if ( m_priorityTails[( size_t )( priority )] == nMapIndex ) {
		m_priorityTails[( size_t )( priority )] = ( int16_t )previous;
	}
	m_queuedNext[( size_t )( nMapIndex )] = ( int16_t )-1;
	m_queuedPrev[( size_t )( nMapIndex )] = ( int16_t )-1;
	m_queuedPriorities[( size_t )( nMapIndex )] = 0u;
}

void Terrain3DRuntimePageStore::queueLink( const int nMapIndex, const uint8_t nPriority ) {
	if ( nMapIndex < 0 || nMapIndex >= s_nMaxRegionSlots || m_queuedConsumers[( size_t )( nMapIndex )] == 0u ) {
		return;
	}
	if ( m_queuedPriorities[( size_t )( nMapIndex )] != 0u ) {
		queueUnlink( nMapIndex );
	}
	const uint8_t priority = MAX( ( uint8_t )1u, nPriority );
	const int tail = m_priorityTails[( size_t )( priority )];
	m_queuedPrev[( size_t )( nMapIndex )] = ( int16_t )tail;
	m_queuedNext[( size_t )( nMapIndex )] = ( int16_t )-1;
	if ( tail >= 0 ) {
		m_queuedNext[( size_t )( tail )] = ( int16_t )nMapIndex;
	} else {
		m_priorityHeads[( size_t )( priority )] = ( int16_t )nMapIndex;
	}
	m_priorityTails[( size_t )( priority )] = ( int16_t )nMapIndex;
	m_queuedPriorities[( size_t )( nMapIndex )] = priority;
}

int Terrain3DRuntimePageStore::popBestQueued( const Vector2i &rPredictedCenter ) {
	for ( int priority = 1; priority < 256; ++priority ) {
		int map_index = m_priorityHeads[( size_t )( priority )];
		if ( map_index < 0 ) {
			continue;
		}
		int best_map_index = -1;
		int best_distance = INT32_MAX;
		while ( map_index >= 0 ) {
			const int next = m_queuedNext[( size_t )( map_index )];
			if ( m_queuedConsumers[( size_t )( map_index )] != 0u ) {
				const int map_x = map_index % s_nActiveMaxRegionMapSize;
				const int map_y = map_index / s_nActiveMaxRegionMapSize;
				const Vector2i location( map_x - s_nActiveMaxRegionMapSize / 2, map_y - s_nActiveMaxRegionMapSize / 2 );
				const int distance = MAX( Math::abs( location.x - rPredictedCenter.x ), Math::abs( location.y - rPredictedCenter.y ) );
				if ( distance < best_distance || ( distance == best_distance && ( best_map_index < 0 || map_index < best_map_index ) ) ) {
					best_distance = distance;
					best_map_index = map_index;
				}
			}
			map_index = next;
		}
		if ( best_map_index >= 0 ) {
			queueUnlink( best_map_index );
			return best_map_index;
		}
	}
	return -1;
}

int Terrain3DRuntimePageStore::acquirePayloadSlot( void ) {
	if ( m_nFreePayloadCount <= 0 ) {
		return -1;
	}
	return m_freePayloadSlots[( size_t )( --m_nFreePayloadCount )];
}

void Terrain3DRuntimePageStore::releasePayloadSlot( const int nPayloadSlot ) {
	if ( nPayloadSlot < 0 || nPayloadSlot >= m_nRuntimePayloadLimit || m_nFreePayloadCount >= m_nRuntimePayloadLimit ) {
		return;
	}
	m_freePayloadSlots[( size_t )( m_nFreePayloadCount++ )] = ( int16_t )nPayloadSlot;
}

void Terrain3DRuntimePageStore::startWorkers( const int nWorkerCount ) {
	const int worker_count = CLAMP( nWorkerCount, 1, s_nRuntimeWorkerLimit );
	if ( m_nWorkerThreadCount == worker_count ) {
		return;
	}
	stopWorkers();
	{
		t3dScopedLock_t lock( m_workerMutex );
		m_bWorkersStop = false;
	}
	for ( int worker = 0; worker < worker_count; ++worker ) {
		m_workers[( size_t )( worker )] = t3dThread_t( &Terrain3DRuntimePageStore::workerMain, this, worker );
	}
	m_nWorkerThreadCount = worker_count;
}

void Terrain3DRuntimePageStore::stopWorkers( void ) {
	if ( m_nWorkerThreadCount <= 0 ) {
		return;
	}
	{
		t3dScopedLock_t lock( m_workerMutex );
		m_bWorkersStop = true;
	}
	m_workerWake.notifyAll();
	for ( int worker = 0; worker < m_nWorkerThreadCount; ++worker ) {
		if ( m_workers[( size_t )( worker )].joinable() ) {
			m_workers[( size_t )( worker )].join();
		}
	}
	{
		t3dScopedLock_t lock( m_workerMutex );
		m_iWorkRead = 0;
		m_iWorkWrite = 0;
		m_nWorkRingCount = 0;
		m_iCompletionRead = 0;
		m_iCompletionWrite = 0;
		m_nCompletionRingCount = 0;
		m_bWorkersStop = false;
	}
	m_nWorkerThreadCount = 0;
}

void Terrain3DRuntimePageStore::prepareWorkIndex( const int nWorkIndex, const int nWorkerIndex ) {
	if ( nWorkIndex < 0 || nWorkIndex >= m_nPrepareWorkPageCount || nWorkerIndex < 0 || nWorkerIndex >= m_nWorkerThreadCount ) {
		return;
	}
	t3dRuntimePageMeta_t &meta = m_workPages[( size_t )( nWorkIndex )];
	if ( meta.m_iPayloadSlot < 0 || meta.m_iPayloadSlot >= m_nRuntimePayloadLimit || meta.m_iSourceIndex < 0
	        || meta.m_iSourceIndex >= s_nMaxRegionSlots ) {
		return;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	Time *const pTime = Time::get_singleton();
	meta.m_nWorkerStartedUsec = pTime ? pTime->get_ticks_usec() : meta.m_nRequestedUsec;
	meta.m_bAttempted = true;
	meta.m_nAttemptedConsumerMask = meta.m_nConsumerMask;
	meta.m_bLoaded =
	        loadSnapshot( meta, payload, m_workerScratch[( size_t )( nWorkerIndex )], m_pageSources[( size_t )( meta.m_iSourceIndex )] );
	meta.m_nMaterializedConsumerMask = meta.m_bLoaded ? meta.m_nConsumerMask : 0u;
	meta.m_nLoadedConsumerMask = meta.m_bLoaded ? meta.m_nConsumerMask : 0u;
	meta.m_bInstanceAttempted = meta.m_bLoaded && ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u
	        && payload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES] > 0u;
	if ( meta.m_bInstanceAttempted ) {
		// Keep worker preparation allocation-free. The static-instance byte channel is
		// already materialized into retained payload storage here; decoding into the
		// allocator-backed instance store is deferred to the owner-side take boundary.
		meta.m_bInstanceLoaded = false;
	} else if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u ) {
		meta.m_bInstanceLoaded = meta.m_bLoaded;
	}
	meta.m_nWorkerFinishedUsec = pTime ? pTime->get_ticks_usec() : meta.m_nWorkerStartedUsec;
}

void Terrain3DRuntimePageStore::workerMain( const int nWorkerIndex ) {
	for ( ;; ) {
		int work_index = -1;
		{
			t3dUniqueLock_t lock( m_workerMutex );
			while ( !m_bWorkersStop && m_nWorkRingCount <= 0 ) {
				m_workerWake.wait( lock );
			}
			if ( m_bWorkersStop ) {
				return;
			}
			work_index = m_workRing[( size_t )( m_iWorkRead )];
			m_iWorkRead = ( m_iWorkRead + 1 ) % s_nGpuCacheMaxLayers;
			--m_nWorkRingCount;
		}
		{
			Terrain3DAllocationPhaseScope allocation_phase( TERRAIN3D_ALLOCATION_PHASE_STREAMING_WORKER );
			prepareWorkIndex( work_index, nWorkerIndex );
		}
		{
			t3dScopedLock_t lock( m_workerMutex );
			if ( m_nCompletionRingCount < s_nGpuCacheMaxLayers ) {
				m_completionRing[( size_t )( m_iCompletionWrite )] = ( int16_t )work_index;
				m_iCompletionWrite = ( m_iCompletionWrite + 1 ) % s_nGpuCacheMaxLayers;
				++m_nCompletionRingCount;
			}
		}
		m_completionWake.notifyOne();
	}
}

bool Terrain3DRuntimePageStore::rebuildRuntimeSources( const t3dRuntimePageServiceState_t &rState ) {
	if ( !m_pRepository ) {
		return false;
	}
	const t3dRuntimePageNumericConfig_t numeric = t3dCompileRuntimePageNumericConfig( rState.m_config.m_nColorDivisor,
	        rState.m_config.m_nDerivedDivisor,
	        rState.m_config.m_nMiddleHeightDivisor,
	        rState.m_config.m_nPrepareThreads );
	const uint64_t topology_generation = m_pRepository->getTopologyGeneration();
	const bool same_config = !m_bRuntimeSourcesDirty && topology_generation == m_nSourceTopologyGeneration
	        && rState.m_config.m_dataDirectory == m_sourceDataDirectory && numeric.m_nColorDivisor == m_sourceNumericConfig.m_nColorDivisor
	        && numeric.m_nDerivedDivisor == m_sourceNumericConfig.m_nDerivedDivisor
	        && numeric.m_nMiddleHeightDivisor == m_sourceNumericConfig.m_nMiddleHeightDivisor
	        && numeric.m_nPrepareThreads == m_sourceNumericConfig.m_nPrepareThreads
	        && rState.m_config.m_bEditorAuthoring == m_bSourceEditorAuthoring;
	if ( same_config && m_nWorkerThreadCount == numeric.m_nPrepareThreads ) {
		return true;
	}
	poll( true );
	while ( m_nPreparedPageCount > 0 ) {
		consumePreparedIndex( m_nPreparedPageCount - 1 );
	}
	stopWorkers();
	for ( int index = 0; index < s_nMaxRegionSlots; ++index ) {
		t3dRuntimePageStoreResetSource( m_pageSources[( size_t )( index )] );
	}
	memset( m_runtimeChannelCapacities.data(), 0, sizeof( m_runtimeChannelCapacities ) );
	uint32_t scratch_bytes = s_nRuntimePageHeaderBytes;
	for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
		const Vector2i location = m_pRepository->getSlotsData()[slot];
		if ( location == s_v2iMax ) {
			continue;
		}
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( map_index < 0 || !pRegion || pRegion->isDeleted() || !pRegion->runtimeMapsEvictableInternal() ) {
			continue;
		}
		t3dRuntimePageSource_t &source = m_pageSources[( size_t )( map_index )];
		source.m_pagePath = getPagePath( pRegion, rState.m_config.m_dataDirectory );
		source.m_sourcePath = pRegion->get_path();
		source.m_nExpectedChannelFlags = ( uint32_t )pRegion->getChannelFlags();
		source.m_nExpectedSourceSignature = resolveExpectedSourceSignature( pRegion, rState.m_config );
		source.m_bPresent = !source.m_pagePath.is_empty();
		if ( source.m_bPresent ) {
			t3dRuntimePageHeader_t header;
			if ( !t3dRuntimePageStoreReadHeaderCold( source.m_pagePath, header ) ) {
				source.m_bPresent = false;
				continue;
			}
			const uint64_t header_source_signature =
			        ( uint64_t )header.m_nSourceSignatureLo | ( ( uint64_t )header.m_nSourceSignatureHi << 32u );
			if ( header_source_signature == 0u || ( rState.m_config.m_bEditorAuthoring && source.m_nExpectedSourceSignature == 0u )
			        || ( source.m_nExpectedSourceSignature != 0u && header_source_signature != source.m_nExpectedSourceSignature ) ) {
				source.m_bPresent = false;
				continue;
			}
			bool fixed_compatible = true;
			for ( uint32_t channel = 0u; channel < header.m_nChannelCount; ++channel ) {
				const t3dRuntimePageChannelInfo_t &info = header.m_szChannels[channel];
				if ( info.m_nType == 0u || info.m_nType > s_nRuntimePageChannelCount ) {
					continue;
				}
				PackedByteArray *const pBuffer =
				        t3dRuntimePageStorePayloadBuffer( m_payloadSlots[0], ( t3dRuntimePageChannel_t )( info.m_nType ) );
				if ( !pBuffer ) {
					continue;
				}
				uint32_t &capacity = m_runtimeChannelCapacities[info.m_nType];
				if ( info.m_nFormat != UINT32_MAX && capacity != 0u && capacity != info.m_nUncompressedBytes ) {
					fixed_compatible = false;
					break;
				}
				capacity = MAX( capacity, info.m_nUncompressedBytes );
				scratch_bytes = MAX( scratch_bytes, info.m_nCompressedBytes );
			}
			source.m_bPresent = fixed_compatible;
		}
	}
	m_nRuntimePayloadLimit = rState.m_config.m_bEditorAuthoring ? s_nGpuCacheEditorMaxLayers : s_nGpuCacheRuntimeMaxLayers;
	m_nFreePayloadCount = m_nRuntimePayloadLimit;
	for ( int slot = 0; slot < m_nRuntimePayloadLimit; ++slot ) {
		m_freePayloadSlots[( size_t )( slot )] = ( int16_t )( m_nRuntimePayloadLimit - slot - 1 );
		if ( !t3dRuntimePageStorePreparePayloadStorage( m_payloadSlots[( size_t )( slot )], m_runtimeChannelCapacities ) ) {
			return false;
		}
	}
	m_nWorkerScratchBytes = scratch_bytes;
	for ( int worker = 0; worker < numeric.m_nPrepareThreads; ++worker ) {
		t3dRuntimePageWorkerScratch_t &scratch = m_workerScratch[( size_t )( worker )];
		scratch.m_ioBytes.resize( scratch_bytes );
		if ( scratch.m_ioBytes.size() != ( int64_t )( scratch_bytes ) ) {
			return false;
		}
	}
	m_nSourceTopologyGeneration = topology_generation;
	m_sourceDataDirectory = rState.m_config.m_dataDirectory;
	m_sourceNumericConfig = numeric;
	m_bSourceEditorAuthoring = rState.m_config.m_bEditorAuthoring;
	m_bRuntimeSourcesDirty = false;
	startWorkers( numeric.m_nPrepareThreads );
	return true;
}

static void t3dRuntimePageStoreCopyPayloadChannel(
        t3dRuntimePagePayload_t &rDestination, const t3dRuntimePagePayload_t &rSource, const t3dRuntimePageChannel_t eChannel ) {
	const uint32_t channel = t3dRuntimePageStoreChannelIndex( eChannel );
	if ( channel == 0u ) {
		return;
	}
	PackedByteArray *const pDestination = t3dRuntimePageStorePayloadBuffer( rDestination, eChannel );
	const PackedByteArray *const pSource = t3dRuntimePageStorePayloadBuffer( rSource, eChannel );
	if ( !pDestination || !pSource ) {
		return;
	}
	const uint32_t logical_bytes = rSource.m_szLogicalBytes[channel];
	if ( logical_bytes > ( uint32_t )( pDestination->size() ) || logical_bytes > ( uint32_t )( pSource->size() ) ) {
		rDestination.m_szLogicalBytes[channel] = 0u;
		return;
	}
	if ( logical_bytes > 0u ) {
		memcpy( pDestination->ptrw(), pSource->ptr(), logical_bytes );
	}
	rDestination.m_szLogicalBytes[channel] = logical_bytes;
}

void Terrain3DRuntimePageStore::mergePreparedPayload( t3dRuntimePageMeta_t &rDestinationMeta,
        t3dRuntimePagePayload_t &rDestinationPayload,
        const t3dRuntimePageMeta_t &rSourceMeta,
        t3dRuntimePagePayload_t &rSourcePayload ) {
	const uint16_t consumers = rSourceMeta.m_nConsumerMask;
	if ( ( consumers & ( RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT | RUNTIME_PAGE_CONSUMER_CPU_MAPS | RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) )
	        != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_HEIGHT );
		rDestinationPayload.m_height = rSourcePayload.m_height;
	}
	if ( ( consumers & ( RUNTIME_PAGE_CONSUMER_CPU_MAPS | RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_CONTROL );
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_COLOR );
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_DERIVED );
		rDestinationPayload.m_control = rSourcePayload.m_control;
		rDestinationPayload.m_color = rSourcePayload.m_color;
		rDestinationPayload.m_derived = rSourcePayload.m_derived;
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT );
		rDestinationPayload.m_middleHeight = rSourcePayload.m_middleHeight;
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE );
		rDestinationPayload.m_middleComposite = rSourcePayload.m_middleComposite;
		rDestinationMeta.m_nMaterialMask = rSourceMeta.m_nMaterialMask;
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK );
		rDestinationPayload.m_terrainShadowMask = rSourcePayload.m_terrainShadowMask;
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES );
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA );
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START );
		rDestinationPayload.m_preparedInstanceStore = eastl::move( rSourcePayload.m_preparedInstanceStore );
		rDestinationMeta.m_bInstanceAttempted = rSourceMeta.m_bInstanceAttempted;
		rDestinationMeta.m_bInstanceLoaded = rSourceMeta.m_bInstanceLoaded;
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS );
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_COLLISION ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT );
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS );
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_HOLE_MASK );
	}
	if ( ( consumers & RUNTIME_PAGE_CONSUMER_NAVIGATION ) != 0u ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_NAVIGATION );
	}
	if ( rSourceMeta.m_bLoadMinMax ) {
		t3dRuntimePageStoreCopyPayloadChannel( rDestinationPayload, rSourcePayload, TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT );
	}
	rDestinationMeta.m_nConsumerMask |= consumers;
	rDestinationMeta.m_nAttemptedConsumerMask |= rSourceMeta.m_nAttemptedConsumerMask;
	rDestinationMeta.m_nMaterializedConsumerMask |= rSourceMeta.m_nMaterializedConsumerMask;
	rDestinationMeta.m_nLoadedConsumerMask |= rSourceMeta.m_nLoadedConsumerMask;
	rDestinationMeta.m_bAttempted = rDestinationMeta.m_nAttemptedConsumerMask != 0u;
	rDestinationMeta.m_bLoaded = rDestinationMeta.m_nLoadedConsumerMask != 0u;
	rDestinationMeta.m_nRequestedUsec = MIN( rDestinationMeta.m_nRequestedUsec, rSourceMeta.m_nRequestedUsec );
	rDestinationMeta.m_nWorkerFinishedUsec = MAX( rDestinationMeta.m_nWorkerFinishedUsec, rSourceMeta.m_nWorkerFinishedUsec );
}

void Terrain3DRuntimePageStore::poll( const bool bWait ) {
	if ( !m_bPrepareJobActive ) {
		return;
	}
	{
		t3dUniqueLock_t lock( m_workerMutex );
		if ( bWait ) {
			while ( m_nCompletionRingCount < m_nPrepareWorkPageCount && !m_bWorkersStop ) {
				m_completionWake.wait( lock );
			}
		} else if ( m_nCompletionRingCount < m_nPrepareWorkPageCount ) {
			return;
		}
		if ( m_nCompletionRingCount < m_nPrepareWorkPageCount ) {
			return;
		}
		for ( int completed = 0; completed < m_nPrepareWorkPageCount; ++completed ) {
			m_iCompletionRead = ( m_iCompletionRead + 1 ) % s_nGpuCacheMaxLayers;
			--m_nCompletionRingCount;
		}
	}

	for ( int work_index = 0; work_index < m_nPrepareWorkPageCount; ++work_index ) {
		t3dRuntimePageMeta_t &work_meta = m_workPages[( size_t )( work_index )];
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( work_meta.m_location );
		m_nPrepareQueueWaitUsecLast = MAX( m_nPrepareQueueWaitUsecLast,
		        work_meta.m_nWorkerStartedUsec >= work_meta.m_nRequestedUsec ? work_meta.m_nWorkerStartedUsec - work_meta.m_nRequestedUsec
		                                                                     : 0u );
		m_nPrepareFileOpenUsecLast = MAX( m_nPrepareFileOpenUsecLast, work_meta.m_nFileOpenUsec );
		m_nPrepareFileReadUsecLast = MAX( m_nPrepareFileReadUsecLast, work_meta.m_nFileReadUsec );
		m_nPrepareValidationUsecLast = MAX( m_nPrepareValidationUsecLast, work_meta.m_nValidationUsec );
		m_nPrepareMaterializeUsecLast = MAX( m_nPrepareMaterializeUsecLast, work_meta.m_nMaterializeUsec );
		m_nPrepareWorkerUsecLast = MAX( m_nPrepareWorkerUsecLast,
		        work_meta.m_nWorkerFinishedUsec >= work_meta.m_nWorkerStartedUsec
		                ? work_meta.m_nWorkerFinishedUsec - work_meta.m_nWorkerStartedUsec
		                : 0u );

		if ( map_index >= 0 && m_queuedConsumers[( size_t )( map_index )] != 0u ) {
			const uint16_t queued = m_queuedConsumers[( size_t )( map_index )];
			const uint16_t already_materialized = queued & work_meta.m_nMaterializedConsumerMask;
			const uint16_t follow_up = queued & ( uint16_t )( ~work_meta.m_nMaterializedConsumerMask );
			work_meta.m_nConsumerMask |= already_materialized;
			if ( follow_up == 0u ) {
				queueUnlink( map_index );
				m_queuedConsumers[( size_t )( map_index )] = 0u;
				m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
			} else {
				const uint8_t priority = MAX( ( uint8_t )1u, m_queuedPriorities[( size_t )( map_index )] );
				m_queuedConsumers[( size_t )( map_index )] = follow_up;
				queueLink( map_index, priority );
			}
		}

		int destination = getPreparedIndex( work_meta.m_location );
		if ( destination >= 0 ) {
			t3dRuntimePageMeta_t &destination_meta = m_preparedPages[( size_t )( destination )];
			t3dRuntimePagePayload_t &destination_payload = m_payloadSlots[( size_t )( destination_meta.m_iPayloadSlot )];
			t3dRuntimePagePayload_t &source_payload = m_payloadSlots[( size_t )( work_meta.m_iPayloadSlot )];
			mergePreparedPayload( destination_meta, destination_payload, work_meta, source_payload );
			releasePayloadSlot( work_meta.m_iPayloadSlot );
		} else if ( m_nPreparedPageCount < m_nRuntimePayloadLimit ) {
			destination = m_nPreparedPageCount++;
			m_preparedPages[( size_t )( destination )] = work_meta;
			if ( map_index >= 0 ) {
				m_preparedByRegion[( size_t )( map_index )] = ( int16_t )destination;
			}
		} else {
			releasePayloadSlot( work_meta.m_iPayloadSlot );
		}
		if ( map_index >= 0 ) {
			m_workByRegion[( size_t )( map_index )] = ( int16_t )-1;
		}
		t3dRuntimePageStoreResetMeta( work_meta );
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t finished = pTime ? pTime->get_ticks_usec() : m_nPrepareJobStartedUsec;
	m_nPrepareLatencyUsecLast = finished >= m_nPrepareJobStartedUsec ? finished - m_nPrepareJobStartedUsec : 0u;
	m_nPrepareWorkPageCount = 0;
	m_bPrepareJobActive = false;
	m_nPrepareJobWorkers = 0;
}

int Terrain3DRuntimePageStore::getPreparedIndex( const Vector2i &rLocation ) const {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int index = m_preparedByRegion[( size_t )( map_index )];
	return index >= 0 && index < m_nPreparedPageCount && m_preparedPages[( size_t )( index )].m_location == rLocation ? index : -1;
}

int Terrain3DRuntimePageStore::getWorkIndex( const Vector2i &rLocation ) const {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int index = m_workByRegion[( size_t )( map_index )];
	return index >= 0 && index < m_nPrepareWorkPageCount && m_workPages[( size_t )( index )].m_location == rLocation ? index : -1;
}

bool Terrain3DRuntimePageStore::isPending( const Vector2i &rLocation ) const {
	return m_bPrepareJobActive && getWorkIndex( rLocation ) >= 0;
}

bool Terrain3DRuntimePageStore::isPrepared( const Vector2i &rLocation ) const {
	return getPreparedIndex( rLocation ) >= 0;
}

void Terrain3DRuntimePageStore::request(
        const Vector2i &rLocation, const uint16_t nConsumers, const uint8_t nPriority, const bool bEnabled ) {
	if ( nConsumers == 0u || !bEnabled || !m_pRepository || !m_pRepository->isLocationValid( rLocation )
	        || !m_pRepository->getRegionPtr( rLocation ) ) {
		return;
	}
	uint16_t consumers = nConsumers;
	const int ready_index = getPreparedIndex( rLocation );
	if ( ready_index >= 0 ) {
		t3dRuntimePageMeta_t &ready = m_preparedPages[( size_t )( ready_index )];
		const uint16_t reusable = consumers & ready.m_nMaterializedConsumerMask;
		ready.m_nConsumerMask |= reusable;
		consumers &= ( uint16_t )( ~reusable );
		if ( consumers == 0u ) {
			return;
		}
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 ) {
		return;
	}
	const bool was_empty = m_queuedConsumers[( size_t )( map_index )] == 0u;
	const uint8_t current_priority = m_queuedPriorities[( size_t )( map_index )];
	m_queuedConsumers[( size_t )( map_index )] |= consumers;
	const uint8_t priority = MAX( ( uint8_t )1u, nPriority );
	if ( was_empty ) {
		++m_nQueuedCount;
		queueLink( map_index, priority );
	} else if ( current_priority == 0u || priority < current_priority ) {
		queueLink( map_index, priority );
	}
}

void Terrain3DRuntimePageStore::service( const t3dRuntimePageServiceState_t &rState ) {
	poll( false );
	if ( !rState.m_config.m_bCookedPagesEnabled || !m_pRepository || !rebuildRuntimeSources( rState ) ) {
		return;
	}

	if ( !rState.m_config.m_bEditorAuthoring && rState.m_nStreamingGeneration != m_nStreamingGenerationLast ) {
		for ( int priority = 1; priority < 256; ++priority ) {
			int map_index = m_priorityHeads[( size_t )( priority )];
			while ( map_index >= 0 ) {
				const int next = m_queuedNext[( size_t )( map_index )];
				const int map_x = map_index % s_nActiveMaxRegionMapSize;
				const int map_y = map_index / s_nActiveMaxRegionMapSize;
				const Vector2i location( map_x - s_nActiveMaxRegionMapSize / 2, map_y - s_nActiveMaxRegionMapSize / 2 );
				const bool wanted = m_pRepository->getRegionPtr( location )
				        && ( !rState.m_isWanted || rState.m_isWanted( rState.m_pWantedContext, location ) );
				if ( !wanted ) {
					queueUnlink( map_index );
					m_queuedConsumers[( size_t )( map_index )] = 0u;
					m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
				}
				map_index = next;
			}
		}
		for ( int ready_index = m_nPreparedPageCount - 1; ready_index >= 0; --ready_index ) {
			const Vector2i location = m_preparedPages[( size_t )( ready_index )].m_location;
			const bool wanted = m_pRepository->getRegionPtr( location )
			        && ( !rState.m_isWanted || rState.m_isWanted( rState.m_pWantedContext, location ) );
			if ( !wanted ) {
				consumePreparedIndex( ready_index );
			}
		}
	}
	m_nStreamingGenerationLast = rState.m_nStreamingGeneration;
	if ( m_bPrepareJobActive || m_nQueuedCount <= 0 || m_nFreePayloadCount <= 0 ) {
		return;
	}

	const int batch_limit =
	        MIN( m_nFreePayloadCount, MIN( m_nRuntimePayloadLimit, MAX( 2, m_sourceNumericConfig.m_nPrepareThreads * 2 ) ) );
	m_nPrepareWorkPageCount = 0;
	while ( m_nPrepareWorkPageCount < batch_limit && m_nQueuedCount > 0 ) {
		const int map_index = popBestQueued( rState.m_predictedCenter );
		if ( map_index < 0 ) {
			break;
		}
		const int map_x = map_index % s_nActiveMaxRegionMapSize;
		const int map_y = map_index / s_nActiveMaxRegionMapSize;
		const Vector2i location( map_x - s_nActiveMaxRegionMapSize / 2, map_y - s_nActiveMaxRegionMapSize / 2 );
		const t3dRuntimePageSource_t &source = m_pageSources[( size_t )( map_index )];
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !source.m_bPresent || !pRegion || pRegion->isDeleted() || !pRegion->runtimeMapsEvictableInternal()
		        || getWorkIndex( location ) >= 0 ) {
			m_queuedConsumers[( size_t )( map_index )] = 0u;
			m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
			continue;
		}
		const int payload_slot = acquirePayloadSlot();
		if ( payload_slot < 0 ) {
			queueLink( map_index, 1u );
			break;
		}
		const int work_index = m_nPrepareWorkPageCount++;
		t3dRuntimePageMeta_t &meta = m_workPages[( size_t )( work_index )];
		t3dRuntimePageStoreResetMeta( meta );
		meta.m_location = location;
		meta.m_iSourceIndex = ( int16_t )map_index;
		meta.m_iPayloadSlot = ( int16_t )payload_slot;
		meta.m_nExpectedChannelFlags = source.m_nExpectedChannelFlags;
		meta.m_nExpectedSourceSignature = source.m_nExpectedSourceSignature;
		meta.m_nRegionSize = m_pRepository->getRegionSize();
		meta.m_nColorDivisor = m_sourceNumericConfig.m_nColorDivisor;
		meta.m_nDerivedDivisor = m_sourceNumericConfig.m_nDerivedDivisor;
		meta.m_nMiddleHeightDivisor = m_sourceNumericConfig.m_nMiddleHeightDivisor;
		meta.m_nConsumerMask = m_queuedConsumers[( size_t )( map_index )];
		meta.m_bEditorAuthoring = rState.m_config.m_bEditorAuthoring;
		Time *const pTime = Time::get_singleton();
		meta.m_nRequestedUsec = pTime ? pTime->get_ticks_usec() : 0u;
		m_workByRegion[( size_t )( map_index )] = ( int16_t )work_index;
		m_queuedConsumers[( size_t )( map_index )] = 0u;
		m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
	}
	if ( m_nPrepareWorkPageCount <= 0 ) {
		return;
	}

	m_nPrepareJobWorkers = MIN( m_nPrepareWorkPageCount, m_nWorkerThreadCount );
	Time *const pTime = Time::get_singleton();
	m_nPrepareJobStartedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	{
		t3dScopedLock_t lock( m_workerMutex );
		for ( int index = 0; index < m_nPrepareWorkPageCount; ++index ) {
			m_workRing[( size_t )( m_iWorkWrite )] = ( int16_t )index;
			m_iWorkWrite = ( m_iWorkWrite + 1 ) % s_nGpuCacheMaxLayers;
			++m_nWorkRingCount;
		}
	}
	m_bPrepareJobActive = true;
	m_workerWake.notifyAll();
}

void Terrain3DRuntimePageStore::consumePreparedIndex( const int nIndex ) {
	if ( nIndex < 0 || nIndex >= m_nPreparedPageCount ) {
		return;
	}
	t3dRuntimePageMeta_t &removed = m_preparedPages[( size_t )( nIndex )];
	const int removed_map_index = Terrain3DRegionRepository::getRegionMapIndex( removed.m_location );
	if ( removed_map_index >= 0 ) {
		m_preparedByRegion[( size_t )( removed_map_index )] = ( int16_t )-1;
	}
	releasePayloadSlot( removed.m_iPayloadSlot );
	const int last = m_nPreparedPageCount - 1;
	if ( nIndex != last ) {
		m_preparedPages[( size_t )( nIndex )] = m_preparedPages[( size_t )( last )];
		const int moved_map_index = Terrain3DRegionRepository::getRegionMapIndex( m_preparedPages[( size_t )( nIndex )].m_location );
		if ( moved_map_index >= 0 ) {
			m_preparedByRegion[( size_t )( moved_map_index )] = ( int16_t )nIndex;
		}
	}
	t3dRuntimePageStoreResetMeta( m_preparedPages[( size_t )( last )] );
	--m_nPreparedPageCount;
}

bool Terrain3DRuntimePageStore::takeHeight( const Vector2i &rLocation,
        const uint16_t nConsumer,
        Ref<Image> &rHeight,
        const bool bMiddle,
        PackedByteArray *const pNativeHeightBytes ) {
	poll( false );
	rHeight.unref();
	if ( pNativeHeightBytes ) {
		pNativeHeightBytes->clear();
	}
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & nConsumer ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool page_loaded = ( meta.m_nLoadedConsumerMask & nConsumer ) != 0u;
	if ( page_loaded ) {
		rHeight = bMiddle ? payload.m_middleHeight : payload.m_height;
		if ( pNativeHeightBytes && !bMiddle ) {
			*pNativeHeightBytes = payload.m_heightBytes;
		}
	}
	meta.m_nConsumerMask &= ( uint16_t )( ~nConsumer );
	const bool loaded = page_loaded
	        && ( bMiddle ? rHeight.is_valid() : ( rHeight.is_valid() || ( pNativeHeightBytes && !pNativeHeightBytes->is_empty() ) ) );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeInstance( const Vector2i &rLocation,
        Terrain3DInstanceStore &rStore,
        PackedByteArray *const pInstanceMetadata,
        PackedByteArray *const pMultimeshFastStart ) {
	poll( false );
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool page_loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u;
	if ( page_loaded && meta.m_bInstanceAttempted && !meta.m_bInstanceLoaded ) {
		// InstanceStore decode may need to establish larger retained slab/vector backing.
		// Keep that allocation out of STREAMING_WORKER and make the cold owner-side
		// materialization boundary explicit. Subsequent reuse retains the capacity.
		Terrain3DColdAllocationScope allocation_scope;
		uint32_t szMeshCounts[s_nMaxMeshAssets];
		memset( szMeshCounts, 0, sizeof( szMeshCounts ) );
		uint32_t instance_count = 0u;
		uint16_t mesh_count = 0u;
		meta.m_bInstanceLoaded = Terrain3DInstancePageCodec::decode( payload.m_instancePage,
		        ( size_t )( payload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES] ),
		        payload.m_preparedInstanceStore,
		        szMeshCounts,
		        instance_count,
		        mesh_count );
		if ( !meta.m_bInstanceLoaded ) {
			meta.m_nLoadedConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_INSTANCE_STORE );
		}
	}
	const bool loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u && meta.m_bInstanceLoaded;
	if ( loaded ) {
		Terrain3DInstanceStore previous = eastl::move( rStore );
		rStore = eastl::move( payload.m_preparedInstanceStore );
		payload.m_preparedInstanceStore = eastl::move( previous );
		if ( pInstanceMetadata ) {
			if ( payload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA] > 0u ) {
				*pInstanceMetadata = payload.m_instanceMetadata;
			} else {
				pInstanceMetadata->clear();
			}
		}
		if ( pMultimeshFastStart ) {
			if ( payload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START] > 0u ) {
				*pMultimeshFastStart = payload.m_multimeshFastStart;
			} else {
				pMultimeshFastStart->clear();
			}
		}
	}
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_INSTANCE_STORE );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeCpuMaps(
        const Vector2i &rLocation, Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived ) {
	poll( false );
	rHeight.unref();
	rControl.unref();
	rColor.unref();
	rDerived.unref();
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_CPU_MAPS ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool page_loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_CPU_MAPS ) != 0u;
	if ( page_loaded ) {
		rHeight = payload.m_height;
		rControl = payload.m_control;
		rColor = payload.m_color;
		rDerived = payload.m_derived;
	}
	const bool loaded = page_loaded && rHeight.is_valid() && rDerived.is_valid();
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_CPU_MAPS );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeNodeDescriptors( const Vector2i &rLocation, PackedByteArray &rDescriptors ) {
	poll( false );
	rDescriptors.clear();
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS ) != 0u;
	if ( loaded && payload.m_szLogicalBytes[TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS] > 0u ) {
		rDescriptors = payload.m_nodeDescriptors;
	}
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeCollision(
        const Vector2i &rLocation, PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask ) {
	poll( false );
	rHeight.clear();
	rBounds.clear();
	rHoleMask.clear();
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_COLLISION ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool page_loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_COLLISION ) != 0u;
	if ( page_loaded ) {
		rHeight = payload.m_collisionHeight;
		rBounds = payload.m_collisionBounds;
		rHoleMask = payload.m_holeMask;
	}
	const bool loaded = page_loaded && !rHeight.is_empty() && !rBounds.is_empty() && !rHoleMask.is_empty();
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_COLLISION );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeNavigation( const Vector2i &rLocation, PackedByteArray &rNavigation ) {
	poll( false );
	rNavigation.clear();
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_NAVIGATION ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool page_loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_NAVIGATION ) != 0u;
	if ( page_loaded ) {
		rNavigation = payload.m_navigation;
	}
	const bool loaded = page_loaded && !rNavigation.is_empty();
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_NAVIGATION );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeTerrainShadowMask( const Vector2i &rLocation, Ref<Image> &rShadowMask ) {
	poll( false );
	rShadowMask.unref();
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	const bool page_loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW ) != 0u;
	if ( page_loaded ) {
		rShadowMask = payload.m_terrainShadowMask;
	}
	const bool loaded = page_loaded && rShadowMask.is_valid() && !rShadowMask->is_empty();
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

bool Terrain3DRuntimePageStore::takeGpuPage( const Vector2i &rLocation,
        Ref<Image> &rHeight,
        Ref<Image> &rControl,
        Ref<Image> &rColor,
        Ref<Image> &rDerived,
        Ref<Image> &rMiddleComposite,
        uint32_t &rMaterialMask,
        bool &bAttempted,
        uint64_t *const pRequestedUsec ) {
	poll( false );
	bAttempted = false;
	rMaterialMask = 1u;
	if ( pRequestedUsec ) {
		*pRequestedUsec = 0u;
	}
	const int index = getPreparedIndex( rLocation );
	if ( index < 0 ) {
		return false;
	}
	t3dRuntimePageMeta_t &meta = m_preparedPages[( size_t )( index )];
	if ( ( meta.m_nConsumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) == 0u || meta.m_iPayloadSlot < 0 ) {
		return false;
	}
	t3dRuntimePagePayload_t &payload = m_payloadSlots[( size_t )( meta.m_iPayloadSlot )];
	bAttempted = ( meta.m_nAttemptedConsumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) != 0u;
	if ( pRequestedUsec ) {
		*pRequestedUsec = meta.m_nRequestedUsec;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t consumed_usec = pTime ? pTime->get_ticks_usec() : meta.m_nWorkerFinishedUsec;
	if ( meta.m_nWorkerFinishedUsec > 0u && consumed_usec >= meta.m_nWorkerFinishedUsec ) {
		m_nPrepareReadyWaitUsecLast = MAX( m_nPrepareReadyWaitUsecLast, consumed_usec - meta.m_nWorkerFinishedUsec );
	}
	const bool loaded = ( meta.m_nLoadedConsumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) != 0u;
	if ( loaded ) {
		rHeight = payload.m_height;
		rControl = payload.m_control;
		rColor = payload.m_color;
		rDerived = payload.m_derived;
		rMiddleComposite = payload.m_middleComposite;
		rMaterialMask = meta.m_nMaterialMask;
	}
	meta.m_nConsumerMask &= ( uint16_t )( ~RUNTIME_PAGE_CONSUMER_GPU_DETAIL );
	if ( meta.m_nConsumerMask == 0u ) {
		consumePreparedIndex( index );
	}
	return loaded;
}

t3dRuntimePageStats_t Terrain3DRuntimePageStore::getStats( void ) const {
	t3dRuntimePageStats_t stats;
	stats.m_nPreparedPages = m_nPreparedPageCount;
	stats.m_nPendingPages = m_bPrepareJobActive ? m_nPrepareWorkPageCount : 0;
	stats.m_nQueuedPages = m_nQueuedCount;
	stats.m_nPrepareWorkers = m_bPrepareJobActive ? m_nPrepareJobWorkers : 0;
	stats.m_bPrepareJobActive = m_bPrepareJobActive;
	for ( int index = 0; index < m_nPreparedPageCount; ++index ) {
		stats.m_nPreparedLoadedPages += m_preparedPages[( size_t )( index )].m_bLoaded ? 1 : 0;
	}
	stats.m_nPrepareLatencyUsecLast = m_nPrepareLatencyUsecLast;
	stats.m_nPrepareQueueWaitUsecLast = m_nPrepareQueueWaitUsecLast;
	stats.m_nPrepareFileOpenUsecLast = m_nPrepareFileOpenUsecLast;
	stats.m_nPrepareFileReadUsecLast = m_nPrepareFileReadUsecLast;
	stats.m_nPrepareValidationUsecLast = m_nPrepareValidationUsecLast;
	stats.m_nPrepareMaterializeUsecLast = m_nPrepareMaterializeUsecLast;
	stats.m_nPrepareWorkerUsecLast = m_nPrepareWorkerUsecLast;
	stats.m_nPrepareReadyWaitUsecLast = m_nPrepareReadyWaitUsecLast;
	return stats;
}
