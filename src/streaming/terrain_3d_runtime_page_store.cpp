// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
#include "streaming/terrain_3d_runtime_page_store.h"

#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_persistence.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_page_format.h"
#include "instances/terrain_3d_instance_page_codec.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "core/terrain_3d_util.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>

#include "eastl_config.h"
#include <EASTL/bitset.h>
#include <algorithm>
#include <climits>
#include <cstring>

static void t3dRuntimePageStoreFnv1a64Append( uint64_t &nHash, const uint8_t *pData, const size_t nSize ) {
	for ( size_t index = 0u; index < nSize; ++index ) {
		nHash ^= ( uint64_t ) ( pData[index] );
		nHash *= 1099511628211ull;
	}
}

EA_FORCE_INLINE static void t3dRuntimePageStoreFnv1a64AppendU32( uint64_t &nHash, const uint32_t nValue ) { t3dRuntimePageStoreFnv1a64Append( nHash, ( const uint8_t * ) ( &nValue ), sizeof( nValue ) ); }

EA_FORCE_INLINE static void t3dRuntimePageStoreFnv1a64AppendU64( uint64_t &nHash, const uint64_t nValue ) { t3dRuntimePageStoreFnv1a64Append( nHash, ( const uint8_t * ) ( &nValue ), sizeof( nValue ) ); }

EA_FORCE_INLINE static void t3dRuntimePageStoreFnv1a64AppendF32( uint64_t &nHash, const float fValue ) { uint32_t bits = 0u; static_assert( sizeof( bits ) == sizeof( fValue ) ); std::memcpy( &bits, &fValue, sizeof( bits ) ); t3dRuntimePageStoreFnv1a64AppendU32( nHash, bits ); }

static void t3dRuntimePageStoreFnv1a64AppendString( uint64_t &nHash, const String &rValue ) {
	const CharString utf8 = rValue.utf8();
	const uint32_t length = ( uint32_t ) ( MAX( 0, utf8.length() ) );
	t3dRuntimePageStoreFnv1a64AppendU32( nHash, length );
	if ( length > 0u ) {
		t3dRuntimePageStoreFnv1a64Append( nHash, ( const uint8_t * ) ( utf8.get_data() ), ( size_t ) ( length ) );
	}
}

static bool t3dRuntimePageStoreFnv1a64AppendR16HeightEdge(
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
		t3dRuntimePageStoreFnv1a64Append( nHash, bytes.ptr(), row_bytes );
		return true;
	}
	for ( int row = 0; row < nRegionSize; ++row ) {
		t3dRuntimePageStoreFnv1a64Append( nHash, bytes.ptr() + ( size_t ) ( row ) * row_bytes, sample_bytes );
	}
	return true;
}

static bool t3dRuntimePageStoreFnv1a64AppendR16HeightCorner(
	uint64_t &nHash, const Ref<Image> &rHeight, const int nRegionSize
) {
	if ( rHeight.is_null() || rHeight->get_width() != nRegionSize || rHeight->get_height() != nRegionSize ||
		rHeight->get_format() != Image::FORMAT_R16 || rHeight->has_mipmaps() ) {
		return false;
	}
	const PackedByteArray bytes = rHeight->get_data();
	const size_t expected_bytes = ( size_t ) ( nRegionSize ) * ( size_t ) ( nRegionSize ) * sizeof( uint16_t );
	if ( ( size_t ) ( bytes.size() ) != expected_bytes ) {
		return false;
	}
	t3dRuntimePageStoreFnv1a64Append( nHash, bytes.ptr(), sizeof( uint16_t ) );
	return true;
}

EA_FORCE_INLINE static void t3dRuntimePageStoreWriteU32Raw( uint8_t *&pWrite, const uint32_t nValue ) { pWrite[0] = ( uint8_t ) ( nValue & 0xffu ); pWrite[1] = ( uint8_t ) ( ( nValue >> 8u ) & 0xffu ); pWrite[2] = ( uint8_t ) ( ( nValue >> 16u ) & 0xffu ); pWrite[3] = ( uint8_t ) ( ( nValue >> 24u ) & 0xffu ); pWrite += 4u; }

EA_FORCE_INLINE static uint32_t t3dRuntimePageStoreReadU32Raw( const uint8_t *&pRead ) { const uint32_t value = ( uint32_t ) pRead[0] | ( ( uint32_t ) pRead[1] << 8u ) | ( ( uint32_t ) pRead[2] << 16u ) | ( ( uint32_t ) pRead[3] << 24u ); pRead += 4u; return value; }

static bool t3dRuntimePageStoreEncodeHeader( PackedByteArray &rBytes, const t3dRuntimePageHeader_t &rHeader ) {
	if ( rBytes.size() < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) return false;
	uint8_t *const pBegin = rBytes.ptrw();
	uint8_t *pWrite = pBegin;
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.magic );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.version );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.nHeaderBytes );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.nRegionSize );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.colorDivisor );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.derivedDivisor );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.channelFlags );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.nTotalBytes );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.channelCount );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.channelTableOffset );
	for ( uint32_t channel = 0u; channel < TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT; ++channel ) {
		const t3dRuntimePageChannelInfo_t &info = rHeader.szChannels[channel];
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.type );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.format );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.compression );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.iOffset );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.compressedBytes );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.uncompressedBytes );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.nWidth );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.height );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.mipCount );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.alignment );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.checksum );
		t3dRuntimePageStoreWriteU32Raw( pWrite, info.signature );
	}
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.sourceSignatureLo );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.sourceSignatureHi );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.blendRecipeVersion );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.materialMask );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.middleHeightDivisor );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.worldSignatureLo );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.worldSignatureHi );
	t3dRuntimePageStoreWriteU32Raw( pWrite, ( uint32_t ) rHeader.regionX );
	t3dRuntimePageStoreWriteU32Raw( pWrite, ( uint32_t ) rHeader.regionZ );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.boundsMinYBits );
	t3dRuntimePageStoreWriteU32Raw( pWrite, rHeader.boundsMaxYBits );
	return ( size_t ) ( pWrite - pBegin ) == TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES;
}

static t3dRuntimePageHeader_t t3dRuntimePageStoreDecodeHeader( const PackedByteArray &rBytes ) {
	t3dRuntimePageHeader_t header;
	if ( rBytes.size() < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) return header;
	const uint8_t *pRead = rBytes.ptr();
	header.magic = t3dRuntimePageStoreReadU32Raw( pRead );
	header.version = t3dRuntimePageStoreReadU32Raw( pRead );
	header.nHeaderBytes = t3dRuntimePageStoreReadU32Raw( pRead );
	header.nRegionSize = t3dRuntimePageStoreReadU32Raw( pRead );
	header.colorDivisor = t3dRuntimePageStoreReadU32Raw( pRead );
	header.derivedDivisor = t3dRuntimePageStoreReadU32Raw( pRead );
	header.channelFlags = t3dRuntimePageStoreReadU32Raw( pRead );
	header.nTotalBytes = t3dRuntimePageStoreReadU32Raw( pRead );
	header.channelCount = t3dRuntimePageStoreReadU32Raw( pRead );
	header.channelTableOffset = t3dRuntimePageStoreReadU32Raw( pRead );
	for ( uint32_t channel = 0u; channel < TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT; ++channel ) {
		t3dRuntimePageChannelInfo_t &info = header.szChannels[channel];
		info.type = t3dRuntimePageStoreReadU32Raw( pRead );
		info.format = t3dRuntimePageStoreReadU32Raw( pRead );
		info.compression = t3dRuntimePageStoreReadU32Raw( pRead );
		info.iOffset = t3dRuntimePageStoreReadU32Raw( pRead );
		info.compressedBytes = t3dRuntimePageStoreReadU32Raw( pRead );
		info.uncompressedBytes = t3dRuntimePageStoreReadU32Raw( pRead );
		info.nWidth = t3dRuntimePageStoreReadU32Raw( pRead );
		info.height = t3dRuntimePageStoreReadU32Raw( pRead );
		info.mipCount = t3dRuntimePageStoreReadU32Raw( pRead );
		info.alignment = t3dRuntimePageStoreReadU32Raw( pRead );
		info.checksum = t3dRuntimePageStoreReadU32Raw( pRead );
		info.signature = t3dRuntimePageStoreReadU32Raw( pRead );
	}
	header.sourceSignatureLo = t3dRuntimePageStoreReadU32Raw( pRead );
	header.sourceSignatureHi = t3dRuntimePageStoreReadU32Raw( pRead );
	header.blendRecipeVersion = t3dRuntimePageStoreReadU32Raw( pRead );
	header.materialMask = t3dRuntimePageStoreReadU32Raw( pRead );
	header.middleHeightDivisor = t3dRuntimePageStoreReadU32Raw( pRead );
	header.worldSignatureLo = t3dRuntimePageStoreReadU32Raw( pRead );
	header.worldSignatureHi = t3dRuntimePageStoreReadU32Raw( pRead );
	header.regionX = ( int32_t ) t3dRuntimePageStoreReadU32Raw( pRead );
	header.regionZ = ( int32_t ) t3dRuntimePageStoreReadU32Raw( pRead );
	header.boundsMinYBits = t3dRuntimePageStoreReadU32Raw( pRead );
	header.boundsMaxYBits = t3dRuntimePageStoreReadU32Raw( pRead );
	return header;
}

typedef struct t3dStagedPageBuild_s {
	PackedByteArray szPayloads[TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT];
	uint32_t nTotalBytes = TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES;
} t3dStagedPageBuild_t;

static PackedByteArray t3dRuntimePageStoreEncodeRle( const PackedByteArray &rSource ) {
	PackedByteArray encoded;
	if ( rSource.is_empty() ) return encoded;
	const uint8_t *const pRead = rSource.ptr();
	int64_t read = 0;
	int64_t encoded_bytes = 0;
	while ( read < rSource.size() ) {
		const uint8_t value = pRead[read];
		uint32_t run = 1u;
		while ( read + run < rSource.size() && run < 255u && pRead[read + run] == value ) ++run;
		encoded_bytes += 2;
		read += run;
	}
	if ( encoded_bytes + 16 >= rSource.size() ) return encoded;
	encoded.resize( encoded_bytes );
	if ( encoded.size() != encoded_bytes ) return PackedByteArray();
	uint8_t *const pWrite = encoded.ptrw();
	read = 0;
	int64_t write = 0;
	while ( read < rSource.size() ) {
		const uint8_t value = pRead[read];
		uint8_t run = 1u;
		while ( read + run < rSource.size() && run < 255u && pRead[read + run] == value ) ++run;
		pWrite[write++] = run;
		pWrite[write++] = value;
		read += run;
	}
	return encoded;
}

static PackedByteArray t3dRuntimePageStoreDecodeRle( const PackedByteArray &rSource, const uint32_t nExpectedBytes ) {
	PackedByteArray decoded;
	if ( rSource.is_empty() || nExpectedBytes == 0u || nExpectedBytes > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ) {
		return decoded;
	}
	decoded.resize( nExpectedBytes );
	if ( decoded.size() != ( int64_t ) nExpectedBytes ) return PackedByteArray();
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
		std::memset( pWrite + write, value, run );
		write += run;
	}
	if ( read != rSource.size() || write != nExpectedBytes ) decoded.clear();
	return decoded;
}

static bool t3dRuntimePageStoreStageImageChannel(
	t3dStagedPageBuild_t &rBuild, t3dRuntimePageHeader_t &rHeader,
	const t3dRuntimePageChannel_t channel, const Ref<Image> &rImage
) {
	if ( rImage.is_null() || rImage->is_empty() || rHeader.channelCount >= TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT ||
		terrain3dRuntimePageFindChannel( rHeader, channel ) ) return false;
	const PackedByteArray image_bytes = rImage->get_data();
	if ( image_bytes.is_empty() || image_bytes.size() > INT32_MAX ) return false;
	const uint32_t aligned_offset = terrain3dRuntimePageAlign( rBuild.nTotalBytes );
	if ( aligned_offset == 0u || aligned_offset > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ||
		( uint32_t ) image_bytes.size() > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES - aligned_offset ) return false;
	const uint32_t channel_index = rHeader.channelCount++;
	rBuild.szPayloads[channel_index] = image_bytes;
	t3dRuntimePageChannelInfo_t &info = rHeader.szChannels[channel_index];
	info.type = ( uint32_t ) channel;
	info.format = ( uint32_t ) rImage->get_format();
	info.compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	info.iOffset = aligned_offset;
	info.compressedBytes = ( uint32_t ) image_bytes.size();
	info.uncompressedBytes = ( uint32_t ) image_bytes.size();
	info.checksum = terrain3dRuntimePageChecksum( image_bytes.ptr(), ( size_t ) image_bytes.size() );
	info.nWidth = ( uint32_t ) rImage->get_width();
	info.height = ( uint32_t ) rImage->get_height();
	info.mipCount = rImage->has_mipmaps() ? ( uint32_t ) ( rImage->get_mipmap_count() + 1 ) : 1u;
	info.alignment = TERRAIN3D_RUNTIME_PAGE_ALIGNMENT;
	info.signature = info.checksum;
	rBuild.nTotalBytes = aligned_offset + info.compressedBytes;
	rHeader.nTotalBytes = rBuild.nTotalBytes;
	return true;
}

static bool t3dRuntimePageStoreStageBlobChannel(
	t3dStagedPageBuild_t &rBuild, t3dRuntimePageHeader_t &rHeader,
	const t3dRuntimePageChannel_t channel, const PackedByteArray &rPayload,
	const bool bAllowCompression = true
) {
	if ( rPayload.is_empty() || rHeader.channelCount >= TERRAIN3D_RUNTIME_PAGE_CHANNEL_COUNT ||
		terrain3dRuntimePageFindChannel( rHeader, channel ) ) return false;
	PackedByteArray stored = rPayload;
	uint32_t compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE;
	if ( bAllowCompression && rPayload.size() >= 64 ) {
		PackedByteArray compressed = t3dRuntimePageStoreEncodeRle( rPayload );
		if ( !compressed.is_empty() ) {
			stored = compressed;
			compression = TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE;
		}
	}
	const uint32_t aligned_offset = terrain3dRuntimePageAlign( rBuild.nTotalBytes );
	if ( aligned_offset == 0u || ( uint64_t ) aligned_offset + ( uint64_t ) stored.size() > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ) return false;
	const uint32_t channel_index = rHeader.channelCount++;
	rBuild.szPayloads[channel_index] = stored;
	t3dRuntimePageChannelInfo_t &info = rHeader.szChannels[channel_index];
	info.type = ( uint32_t ) channel;
	info.format = UINT32_MAX;
	info.compression = compression;
	info.iOffset = aligned_offset;
	info.compressedBytes = ( uint32_t ) rBuild.szPayloads[channel_index].size();
	info.uncompressedBytes = ( uint32_t ) rPayload.size();
	info.nWidth = ( uint32_t ) rPayload.size();
	info.height = 1u;
	info.mipCount = 1u;
	info.checksum = terrain3dRuntimePageChecksum( rPayload.ptr(), ( size_t ) rPayload.size() );
	info.signature = info.checksum;
	rBuild.nTotalBytes = aligned_offset + info.compressedBytes;
	rHeader.nTotalBytes = rBuild.nTotalBytes;
	return true;
}

static bool t3dRuntimePageStoreFinalizePageBytes(
	const t3dStagedPageBuild_t &rBuild, t3dRuntimePageHeader_t &rHeader, PackedByteArray &rFileBytes
) {
	if ( rBuild.nTotalBytes < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES || rBuild.nTotalBytes > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ) return false;
	rFileBytes.resize( rBuild.nTotalBytes );
	if ( rFileBytes.size() != ( int64_t ) rBuild.nTotalBytes ) return false;
	uint8_t *const pWrite = rFileBytes.ptrw();
	std::memset( pWrite, 0, rBuild.nTotalBytes );
	for ( uint32_t channel_index = 0u; channel_index < rHeader.channelCount; ++channel_index ) {
		const t3dRuntimePageChannelInfo_t &info = rHeader.szChannels[channel_index];
		const PackedByteArray &payload = rBuild.szPayloads[channel_index];
		if ( payload.size() != ( int64_t ) info.compressedBytes || info.iOffset > rBuild.nTotalBytes ||
			info.compressedBytes > rBuild.nTotalBytes - info.iOffset ) return false;
		if ( info.compressedBytes > 0u ) std::memcpy( pWrite + info.iOffset, payload.ptr(), info.compressedBytes );
	}
	rHeader.nTotalBytes = rBuild.nTotalBytes;
	return t3dRuntimePageStoreEncodeHeader( rFileBytes, rHeader );
}

static PackedByteArray t3dRuntimePageStoreReadBlob(
	const Ref<FileAccess> &rFile, const t3dRuntimePageChannelInfo_t &rInfo,
	uint64_t &nReadUsec, uint64_t &nValidationUsec
) {
	if ( rFile.is_null() || rInfo.format != UINT32_MAX || rInfo.compressedBytes == 0u ||
		( rInfo.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE &&
			rInfo.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ) ) {
		return PackedByteArray();
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.iOffset );
	const PackedByteArray stored = rFile->get_buffer( rInfo.compressedBytes );
	nReadUsec += ( pTime ? pTime->get_ticks_usec() : read_started ) - read_started;
	const uint64_t validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	PackedByteArray bytes;
	if ( stored.size() == ( int64_t ) ( rInfo.compressedBytes ) ) {
		bytes = rInfo.compression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE ?
			t3dRuntimePageStoreDecodeRle( stored, rInfo.uncompressedBytes ) : stored;
	}
	const bool valid = bytes.size() == ( int64_t ) ( rInfo.uncompressedBytes ) &&
		terrain3dRuntimePageChecksum( bytes.ptr(), ( size_t ) ( bytes.size() ) ) == rInfo.checksum;
	nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	return valid ? bytes : PackedByteArray();
}

static bool t3dRuntimePageStoreExpectedImageBytes( const t3dRuntimePageChannelInfo_t &rInfo, uint64_t &rExpectedBytes ) {
	uint32_t bytes_per_pixel = 0u;
	switch ( Image::Format( rInfo.format ) ) {
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
	if ( rInfo.nWidth == 0u || rInfo.height == 0u || rInfo.mipCount == 0u ) {
		return false;
	}
	uint32_t width = rInfo.nWidth;
	uint32_t height = rInfo.height;
	uint64_t total = 0u;
	for ( uint32_t mip = 0u; mip < rInfo.mipCount; ++mip ) {
		const uint64_t level_bytes = ( uint64_t ) ( width ) * ( uint64_t ) ( height ) * bytes_per_pixel;
		if ( level_bytes > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES || total > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES - level_bytes ) {
			return false;
		}
		total += level_bytes;
		width = MAX( 1u, width >> 1u );
		height = MAX( 1u, height >> 1u );
	}
	rExpectedBytes = total;
	return true;
}

static Ref<Image> t3dRuntimePageStoreReadImage(
	const Ref<FileAccess> &rFile, const t3dRuntimePageChannelInfo_t &rInfo,
	uint64_t &nReadUsec, uint64_t &nValidationUsec, uint64_t &nMaterializeUsec,
	PackedByteArray *const pValidatedNativeBytes = nullptr, const bool bMaterialize = true
) {
	if ( rFile.is_null() || rInfo.compressedBytes == 0u ||
		rInfo.compression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ) {
		return Ref<Image>();
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t read_started = pTime ? pTime->get_ticks_usec() : 0u;
	rFile->seek( rInfo.iOffset );
	const PackedByteArray image_bytes = rFile->get_buffer( rInfo.compressedBytes );
	const uint64_t read_finished = pTime ? pTime->get_ticks_usec() : read_started;
	nReadUsec += read_finished - read_started;
	const uint64_t validation_started = read_finished;
	uint64_t expected_bytes = 0u;
	const bool bPayloadShapeValid = t3dRuntimePageStoreExpectedImageBytes( rInfo, expected_bytes ) &&
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
	if ( !bMaterialize ) {
		return Ref<Image>();
	}
	const uint64_t materialize_started = validation_finished;
	const Ref<Image> pImage = Image::create_from_data(
		( int ) ( rInfo.nWidth ), ( int ) ( rInfo.height ), rInfo.mipCount > 1u,
		Image::Format( rInfo.format ), image_bytes
	);
	const bool bImageValid = pImage.is_valid() && !pImage->is_empty() &&
		pImage->get_data().size() == ( int64_t ) ( rInfo.uncompressedBytes );
	const uint64_t materialize_finished = pTime ? pTime->get_ticks_usec() : materialize_started;
	nMaterializeUsec += materialize_finished - materialize_started;
	return bImageValid ? pImage : Ref<Image>();
}

EA_FORCE_INLINE static bool t3dRuntimePageStoreValidImage( const Ref<Image> &rImage, const int nWidth, const int nHeight, const Image::Format eFormat, const bool bMipmaps ) { return rImage.is_valid() && !rImage->is_empty() && rImage->get_width() == nWidth && rImage->get_height() == nHeight && rImage->get_format() == eFormat && rImage->has_mipmaps() == bMipmaps; }

static void t3dRuntimePageStoreUpdateEditorFile( const String &rPath ) {
	if ( !IS_EDITOR || !rPath.begins_with( "res://" ) ) {
		return;
	}
	EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
	if ( pFilesystem && !pFilesystem->is_scanning() ) {
		pFilesystem->update_file( rPath );
	}
}

T3DRuntimePageStore::T3DRuntimePageStore(
	T3DRegionRepository *const pRepository,
	const T3DRegionPersistence *const pPersistence
) :
		m_pRepository( pRepository ), m_pPersistence( pPersistence ) {
	m_preparedByRegion.fill( ( int16_t ) -1 );
	m_workByRegion.fill( ( int16_t ) -1 );
}

T3DRuntimePageStore::~T3DRuntimePageStore( void ) {
	reset();
}

void T3DRuntimePageStore::reset( void ) {
	poll( true );
	for ( int index = 0; index < m_nPreparedPageCount; ++index ) {
		m_preparedPages[( size_t ) ( index )] = t3dRuntimePreparedPage_t{};
	}
	for ( int index = 0; index < m_nPrepareWorkPageCount; ++index ) {
		m_workPages[( size_t ) ( index )] = t3dRuntimePreparedPage_t{};
	}
	m_nPreparedPageCount = 0;
	m_nPrepareWorkPageCount = 0;
	m_nQueuedCount = 0;
	m_nStreamingGenerationLast = 0u;
	m_queuedConsumers.fill( 0u );
	m_queuedPriorities.fill( 0u );
	m_preparedByRegion.fill( ( int16_t ) -1 );
	m_workByRegion.fill( ( int16_t ) -1 );
	m_prepareContext = t3dRuntimePrepareJobContext_t{};
	m_nPrepareGroupId = WorkerThreadPool::INVALID_TASK_ID;
	m_bPrepareJobActive = false;
	m_nPrepareJobWorkers = 0;
	resetFrameStats();
}

void T3DRuntimePageStore::resetFrameStats( void ) {
	m_nPrepareLatencyUsecLast = 0u;
	m_nPrepareQueueWaitUsecLast = 0u;
	m_nPrepareFileOpenUsecLast = 0u;
	m_nPrepareFileReadUsecLast = 0u;
	m_nPrepareValidationUsecLast = 0u;
	m_nPrepareMaterializeUsecLast = 0u;
	m_nPrepareWorkerUsecLast = 0u;
	m_nPrepareReadyWaitUsecLast = 0u;
}

bool T3DRuntimePageStore::loadSourceRegion( const Vector2i &rLocation, Ref<T3DRegion> &rSource ) const {
	rSource.unref();
	if ( !m_pRepository ) {
		return false;
	}
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayloadInternal() ) {
		rSource = Ref<T3DRegion>( pRegion );
		return true;
	}
	return m_pPersistence && m_pPersistence->loadRuntimeRegionSource(
		pRegion->get_path(), m_pRepository->getRegionSize(), rSource );
}

String T3DRuntimePageStore::getPagePath(
	const T3DRegion *const pRegion, const String &rDataDirectory
) const {
	if ( !pRegion ) {
		return String();
	}
	if ( !rDataDirectory.is_empty() ) {
		const String source_path = rDataDirectory.trim_suffix( "/" ) + String( "/" ) +
			T3DUtil::locationToFilename( pRegion->getLocation() );
		return source_path.get_basename() + String( ".t3dpage" );
	}
	return pRegion->get_path().is_empty() ? String() :
		pRegion->get_path().get_basename() + String( ".t3dpage" );
}

uint64_t T3DRuntimePageStore::computeSourceSignature(
	const T3DRegion *const pRegion, const T3DRuntimePageConfig &rConfig
) const {
	if ( !pRegion || !m_pRepository ) {
		return 0u;
	}
	const int region_size = m_pRepository->getRegionSize();
	uint64_t hash = 1469598103934665603ull;
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( region_size ) );
	t3dRuntimePageStoreFnv1a64AppendF32( hash, ( float ) ( m_pRepository->getVertexSpacing() ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( rConfig.colorDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( rConfig.derivedDivisor ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( MAX( 1, rConfig.middleHeightDivisor ) ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION );
	t3dRuntimePageStoreFnv1a64AppendU64( hash, rConfig.materialSourceSignature );
	t3dRuntimePageStoreFnv1a64AppendU64( hash, rConfig.collisionSurfaceSignature );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( pRegion->getLocation().x ) );
	t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( pRegion->getLocation().y ) );

	Ref<T3DRegion> pLoadedOwner;
	const T3DRegion *pOwner = pRegion;
	if ( !pOwner->hasRuntimeMapPayloadInternal() && loadSourceRegion( pOwner->getLocation(), pLoadedOwner ) ) {
		pOwner = pLoadedOwner.ptr();
	}
	if ( !pOwner ) {
		return 0u;
	}
	const Ref<Image> pHeight = pOwner->getHeightMap();
	if ( pHeight.is_valid() ) {
		const PackedByteArray bytes = pHeight->get_data();
		t3dRuntimePageStoreFnv1a64Append( hash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
	}
	const Ref<Image> pColor = pOwner->getColorMap();
	if ( pColor.is_valid() ) {
		const PackedByteArray bytes = pColor->get_data();
		t3dRuntimePageStoreFnv1a64Append( hash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
	}
	const PackedByteArray instance_page = pOwner->getInstancePage();
	if ( !instance_page.is_empty() ) {
		t3dRuntimePageStoreFnv1a64Append( hash, instance_page.ptr(), ( size_t ) ( instance_page.size() ) );
	}

	const Ref<T3DRegionLighting> lighting = pOwner->getLighting();
	if ( lighting.is_valid() ) {
		t3dRuntimePageStoreFnv1a64AppendU64( hash, ( uint64_t ) ( lighting->getBakeSignature() ) );
		t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( lighting->getShadowmaskMode() ) );
		t3dRuntimePageStoreFnv1a64AppendString( hash, lighting->getDirectionalLightSignature() );
		t3dRuntimePageStoreFnv1a64AppendString( hash, lighting->getTerrainShadowMaskPath() );
	} else {
		t3dRuntimePageStoreFnv1a64AppendU64( hash, 0u );
	}

	const Vector2i height_dependency_offsets[3] = {
		Vector2i( 1, 0 ), Vector2i( 0, 1 ), Vector2i( 1, 1 )
	};
	for ( int dependency = 0; dependency < 3; ++dependency ) {
		const Vector2i location = pRegion->getLocation() + height_dependency_offsets[dependency];
		T3DRegion *region = m_pRepository->getRegionPtr( location );
		Ref<T3DRegion> pLoadedSource;
		if ( ( !region || region->getHeightMap().is_null() ) && loadSourceRegion( location, pLoadedSource ) ) {
			region = pLoadedSource.ptr();
		}
		t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( dependency ) );
		const bool dependency_valid = region && !region->isDeleted() &&
			( dependency < 2 ? t3dRuntimePageStoreFnv1a64AppendR16HeightEdge(
				hash, region->getHeightMap(), region_size, dependency == 0 ) :
				t3dRuntimePageStoreFnv1a64AppendR16HeightCorner( hash, region->getHeightMap(), region_size ) );
		if ( !dependency_valid ) {
			t3dRuntimePageStoreFnv1a64AppendU32( hash, 0u );
			continue;
		}
		t3dRuntimePageStoreFnv1a64AppendU32( hash, 1u );
	}

	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = pRegion->getLocation() + Vector2i( dx, dz );
			T3DRegion *region = m_pRepository->getRegionPtr( location );
			Ref<T3DRegion> pLoadedSource;
			if ( ( !region || region->getControlMap().is_null() ) && loadSourceRegion( location, pLoadedSource ) ) {
				region = pLoadedSource.ptr();
			}
			t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( dx + 1 ) );
			t3dRuntimePageStoreFnv1a64AppendU32( hash, ( uint32_t ) ( dz + 1 ) );
			if ( !region || region->isDeleted() ) {
				t3dRuntimePageStoreFnv1a64AppendU32( hash, 0u );
				continue;
			}
			t3dRuntimePageStoreFnv1a64AppendU32( hash, 1u );
			const Ref<Image> pControl = region->getControlMap();
			if ( pControl.is_valid() ) {
				const PackedByteArray bytes = pControl->get_data();
				t3dRuntimePageStoreFnv1a64Append( hash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
			} else {
				t3dRuntimePageStoreFnv1a64AppendU32( hash, TERRAIN3D_REGION_DEFAULT_CONTROL );
			}
		}
	}
	return hash;
}

bool T3DRuntimePageStore::readSourceSignature( const String &rPagePath, uint64_t &nSignature ) const {
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
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	if ( !terrain3dRuntimePageValidateHeader( header, pFile->get_length() ) ) {
		return false;
	}
	nSignature = ( uint64_t ) ( header.sourceSignatureLo ) |
		( ( uint64_t ) ( header.sourceSignatureHi ) << 32u );
	return nSignature != 0u;
}

uint32_t T3DRuntimePageStore::readMaterialMask( const String &rPagePath ) const {
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return 0u;
	}
	const Ref<FileAccess> pFile = FileAccess::open( rPagePath, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) {
		return 0u;
	}
	const PackedByteArray header_bytes = pFile->get_buffer( TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES );
	if ( header_bytes.size() != TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) {
		return 0u;
	}
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	return terrain3dRuntimePageValidateHeader( header, pFile->get_length() ) ? header.materialMask : 0u;
}

bool T3DRuntimePageStore::removePageForSource( const String &rSourcePath ) const {
	return removePage( rSourcePath.is_empty() ? String() : rSourcePath.get_basename() + String( ".t3dpage" ) );
}

bool T3DRuntimePageStore::removePage( const String &rPagePath ) const {
	if ( rPagePath.is_empty() || !FileAccess::file_exists( rPagePath ) ) {
		return true;
	}
	const bool removed = DirAccess::remove_absolute( rPagePath ) == OK;
	if ( removed ) {
		t3dRuntimePageStoreUpdateEditorFile( rPagePath );
	}
	return removed;
}

bool T3DRuntimePageStore::loadSnapshot( t3dRuntimePreparedPage_t &rPage, const bool bPreserveMaterialized ) {
	if ( !bPreserveMaterialized ) {
		rPage.height.unref();
		rPage.heightBytes.clear();
		rPage.pControl.unref();
		rPage.pColor.unref();
		rPage.pDerived.unref();
		rPage.pMiddleHeight.unref();
		rPage.pMiddleComposite.unref();
		rPage.pTerrainShadowMask.unref();
		rPage.instancePage.clear();
		rPage.holeMask.clear();
		rPage.collisionHeight.clear();
		rPage.collisionBounds.clear();
		rPage.navigation.clear();
		rPage.minMaxHeight.clear();
		rPage.nodeDescriptors.clear();
		rPage.instanceMetadata.clear();
		rPage.lightingReceivers.clear();
		rPage.multimeshFastStart.clear();
		rPage.preparedInstanceStore.clearRetainCapacity();
	}
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
	const uint64_t page_modified = rPage.bEditorAuthoring ? FileAccess::get_modified_time( rPage.pagePath ) : 0u;
	const uint64_t source_modified = rPage.bEditorAuthoring && !rPage.sourcePath.is_empty() ?
		FileAccess::get_modified_time( rPage.sourcePath ) : 0u;
	if ( rPage.bEditorAuthoring && source_modified > 0u && page_modified > 0u && page_modified < source_modified ) {
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
	if ( file_length < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES || file_length > TERRAIN3D_RUNTIME_PAGE_MAX_BYTES ||
		file_length > ( uint64_t ) ( INT32_MAX ) ) {
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
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	const uint64_t header_source_signature = ( uint64_t ) header.sourceSignatureLo |
		( ( uint64_t ) header.sourceSignatureHi << 32u );
	if ( !terrain3dRuntimePageValidateHeader( header, file_length ) ||
		header.nRegionSize != ( uint32_t ) ( rPage.nRegionSize ) ||
		header.colorDivisor != ( uint32_t ) ( rPage.colorDivisor ) ||
		header.derivedDivisor != ( uint32_t ) ( rPage.derivedDivisor ) ||
		header.middleHeightDivisor != ( uint32_t ) ( rPage.middleHeightDivisor ) ||
		header.regionX != rPage.location.x || header.regionZ != rPage.location.y ||
		header.blendRecipeVersion != TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION ||
		header_source_signature == 0u ||
		( rPage.expectedSourceSignature != 0u && header_source_signature != rPage.expectedSourceSignature ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	const uint32_t optional_mask = ( uint32_t ) ( REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR );
	const uint32_t expected_flags = rPage.expectedChannelFlags & optional_mask;
	if ( ( header.channelFlags & optional_mask ) != expected_flags ||
		( header.channelFlags & ( uint32_t ) ( REGION_CHANNEL_DERIVED ) ) == 0u ||
		!terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_HOLE_MASK ) ||
		!terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT ) ||
		!terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS ) ||
		!terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_NAVIGATION ) ||
		!terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;

	const t3dRuntimePageChannelInfo_t *const pHeightChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_HEIGHT );
	if ( !pHeightChannel ) {
		return false;
	}
	const bool bNeedGpu = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) != 0u;
	const bool bNeedCpu = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_CPU_MAPS ) != 0u;
	const bool bNeedActiveHeight = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_ACTIVE_HEIGHT ) != 0u;
	const bool bNeedMiddle = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT ) != 0u;
	const bool bNeedTerrainShadow = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW ) != 0u;
	const bool bNeedHeight = bNeedGpu || bNeedCpu || bNeedActiveHeight;
	const bool bMaterializeHeight = bNeedGpu || bNeedCpu;
	if ( bNeedHeight ) {
		if ( rPage.heightBytes.is_empty() ) {
			rPage.height = t3dRuntimePageStoreReadImage( pFile, *pHeightChannel, rPage.nFileReadUsec,
				rPage.nValidationUsec, rPage.nMaterializeUsec, &rPage.heightBytes, bMaterializeHeight );
		} else if ( bMaterializeHeight && rPage.height.is_null() ) {
			const uint64_t materialize_started = pTime ? pTime->get_ticks_usec() : 0u;
			rPage.height = Image::create_from_data( ( int ) pHeightChannel->nWidth, ( int ) pHeightChannel->height,
				pHeightChannel->mipCount > 1u, Image::Format( pHeightChannel->format ), rPage.heightBytes );
			const uint64_t materialize_finished = pTime ? pTime->get_ticks_usec() : materialize_started;
			rPage.nMaterializeUsec += materialize_finished - materialize_started;
		}
		if ( rPage.heightBytes.is_empty() ) return false;
	}
	rPage.materialMask = header.materialMask;

	if ( bNeedGpu || bNeedCpu ) {
		const t3dRuntimePageChannelInfo_t *const pDerivedChannel =
			terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_DERIVED );
		if ( !pDerivedChannel ) {
			return false;
		}
		if ( rPage.pDerived.is_null() ) rPage.pDerived = t3dRuntimePageStoreReadImage( pFile, *pDerivedChannel, rPage.nFileReadUsec,
			rPage.nValidationUsec, rPage.nMaterializeUsec );
	}
	if ( bNeedMiddle ) {
		const t3dRuntimePageChannelInfo_t *const pMiddleHeightChannel =
			terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT );
		if ( !pMiddleHeightChannel ) {
			return false;
		}
		if ( rPage.pMiddleHeight.is_null() ) rPage.pMiddleHeight = t3dRuntimePageStoreReadImage( pFile, *pMiddleHeightChannel, rPage.nFileReadUsec,
			rPage.nValidationUsec, rPage.nMaterializeUsec );
	}
	if ( bNeedGpu ) {
		const t3dRuntimePageChannelInfo_t *const pMiddleCompositeChannel =
			terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE );
		if ( !pMiddleCompositeChannel ) {
			return false;
		}
		if ( rPage.pMiddleComposite.is_null() ) rPage.pMiddleComposite = t3dRuntimePageStoreReadImage( pFile, *pMiddleCompositeChannel, rPage.nFileReadUsec,
			rPage.nValidationUsec, rPage.nMaterializeUsec );
	}
	if ( bNeedTerrainShadow ) {
		const t3dRuntimePageChannelInfo_t *const pShadowChannel =
			terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK );
		if ( !pShadowChannel ) {
			return false;
		}
		if ( rPage.pTerrainShadowMask.is_null() ) rPage.pTerrainShadowMask = t3dRuntimePageStoreReadImage( pFile, *pShadowChannel, rPage.nFileReadUsec,
			rPage.nValidationUsec, rPage.nMaterializeUsec );
	}
	if ( bNeedGpu || bNeedCpu ) {
		if ( ( expected_flags & ( uint32_t ) ( REGION_CHANNEL_CONTROL ) ) != 0u ) {
			const t3dRuntimePageChannelInfo_t *const pControlChannel =
				terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_CONTROL );
			if ( !pControlChannel ) {
				return false;
			}
			if ( rPage.pControl.is_null() ) rPage.pControl = t3dRuntimePageStoreReadImage( pFile, *pControlChannel, rPage.nFileReadUsec,
				rPage.nValidationUsec, rPage.nMaterializeUsec );
		}
		if ( ( expected_flags & ( uint32_t ) ( REGION_CHANNEL_COLOR ) ) != 0u ) {
			const t3dRuntimePageChannelInfo_t *const pColorChannel =
				terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_COLOR );
			if ( !pColorChannel ) {
				return false;
			}
			if ( rPage.pColor.is_null() ) rPage.pColor = t3dRuntimePageStoreReadImage( pFile, *pColorChannel, rPage.nFileReadUsec,
				rPage.nValidationUsec, rPage.nMaterializeUsec );
		}
	}

	auto read_optional_blob = [&]( const t3dRuntimePageChannel_t eChannel, PackedByteArray &rTarget, const bool bRequired ) -> bool {
		const t3dRuntimePageChannelInfo_t *const pChannel = terrain3dRuntimePageFindChannel( header, eChannel );
		if ( !pChannel ) {
			return !bRequired;
		}
		if ( rTarget.is_empty() ) rTarget = t3dRuntimePageStoreReadBlob( pFile, *pChannel, rPage.nFileReadUsec, rPage.nValidationUsec );
		return !rTarget.is_empty();
	};
	const bool bNeedInstances = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u;
	const bool bNeedNodes = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS ) != 0u;
	const bool bNeedCollision = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_COLLISION ) != 0u;
	const bool bNeedNavigation = ( rPage.consumerMask & RUNTIME_PAGE_CONSUMER_NAVIGATION ) != 0u;
	if ( bNeedInstances && !read_optional_blob( TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES, rPage.instancePage, false ) ) {
		return false;
	}
	if ( bNeedNodes && !read_optional_blob( TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS, rPage.nodeDescriptors, false ) ) {
		return false;
	}
	if ( bNeedCollision ) {
		if ( !read_optional_blob( TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT, rPage.collisionHeight, true ) ||
			!read_optional_blob( TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS, rPage.collisionBounds, true ) ||
			!read_optional_blob( TERRAIN3D_RUNTIME_PAGE_HOLE_MASK, rPage.holeMask, true ) ) {
			return false;
		}
	}
	if ( bNeedNavigation && !read_optional_blob( TERRAIN3D_RUNTIME_PAGE_NAVIGATION, rPage.navigation, true ) ) {
		return false;
	}
	// Auxiliary acceleration blobs are materialized only for consumers that can use them.
	if ( rPage.bLoadMinMax && !read_optional_blob( TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT, rPage.minMaxHeight, true ) ) {
		return false;
	}
	if ( bNeedInstances ) {
		read_optional_blob( TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA, rPage.instanceMetadata, false );
		read_optional_blob( TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START, rPage.multimeshFastStart, false );
	}

	validation_started = pTime ? pTime->get_ticks_usec() : 0u;
	const int color_size = MAX( 1, rPage.nRegionSize / rPage.colorDivisor );
	const int derived_size = MAX( 1, rPage.nRegionSize / rPage.derivedDivisor );
	const int middle_height_size = MAX( 1,
		( rPage.nRegionSize + rPage.middleHeightDivisor - 1 ) / rPage.middleHeightDivisor );
	if ( ( bMaterializeHeight && !t3dRuntimePageStoreValidImage( rPage.height, rPage.nRegionSize, rPage.nRegionSize, Image::FORMAT_R16, false ) ) ||
		( ( bNeedGpu || bNeedCpu ) && !t3dRuntimePageStoreValidImage( rPage.pDerived, derived_size, derived_size, Image::FORMAT_RG8, false ) ) ||
		( bNeedMiddle && !t3dRuntimePageStoreValidImage( rPage.pMiddleHeight, middle_height_size, middle_height_size, Image::FORMAT_R16, false ) ) ||
		( bNeedGpu && !t3dRuntimePageStoreValidImage( rPage.pMiddleComposite, color_size, color_size, Image::FORMAT_RGBA8, true ) ) ||
		( bNeedTerrainShadow && ( rPage.pTerrainShadowMask.is_null() || rPage.pTerrainShadowMask->is_empty() ||
			rPage.pTerrainShadowMask->get_format() != Image::FORMAT_L8 || !rPage.pTerrainShadowMask->has_mipmaps() ||
			rPage.pTerrainShadowMask->get_width() != rPage.pTerrainShadowMask->get_height() ) ) ||
		( rPage.pControl.is_valid() && !t3dRuntimePageStoreValidImage(
			rPage.pControl, rPage.nRegionSize, rPage.nRegionSize, Image::FORMAT_RF, false ) ) ||
		( rPage.pColor.is_valid() && !t3dRuntimePageStoreValidImage(
			rPage.pColor, color_size, color_size, Image::FORMAT_RGBA8, true ) ) ) {
		rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
		return false;
	}
	rPage.nValidationUsec += ( pTime ? pTime->get_ticks_usec() : validation_started ) - validation_started;
	return true;
}

bool T3DRuntimePageStore::validatePageForRegion(
	const T3DRegion *const pRegion, const T3DRuntimePageConfig &rConfig
) const {
	if ( !pRegion || !m_pRepository ) {
		return false;
	}
	t3dRuntimePreparedPage_t page;
	page.location = pRegion->getLocation();
	page.pagePath = getPagePath( pRegion, rConfig.dataDirectory );
	page.sourcePath = pRegion->get_path();
	page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
	page.expectedSourceSignature = pRegion->getCookedPageSourceSignatureInternal();
	page.nRegionSize = m_pRepository->getRegionSize();
	page.colorDivisor = MAX( 1, rConfig.colorDivisor );
	page.derivedDivisor = MAX( 1, rConfig.derivedDivisor );
	page.middleHeightDivisor = MAX( 1, rConfig.middleHeightDivisor );
	page.consumerMask = RUNTIME_PAGE_CONSUMER_GPU_DETAIL | RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT;
	const Ref<T3DRegionLighting> lighting = pRegion->getLighting();
	if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
		page.consumerMask |= RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW;
	}
	page.bEditorAuthoring = rConfig.bEditorAuthoring;
	return loadSnapshot( page );
}

bool T3DRuntimePageStore::loadCpuMapsNow(
	const Vector2i &rLocation, const T3DRuntimePageConfig &rConfig,
	Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived
) const {
	rHeight.unref(); rControl.unref(); rColor.unref(); rDerived.unref();
	if ( !m_pRepository || !rConfig.bCookedPagesEnabled ) return false;
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) return false;
	t3dRuntimePreparedPage_t page;
	page.location = rLocation;
	page.pagePath = getPagePath( pRegion, rConfig.dataDirectory );
	page.sourcePath = pRegion->get_path();
	page.expectedChannelFlags = ( uint32_t ) pRegion->getChannelFlags();
	page.expectedSourceSignature = pRegion->getCookedPageSourceSignatureInternal();
	page.nRegionSize = m_pRepository->getRegionSize();
	page.colorDivisor = MAX( 1, rConfig.colorDivisor );
	page.derivedDivisor = MAX( 1, rConfig.derivedDivisor );
	page.middleHeightDivisor = MAX( 1, rConfig.middleHeightDivisor );
	page.consumerMask = RUNTIME_PAGE_CONSUMER_CPU_MAPS;
	page.bEditorAuthoring = rConfig.bEditorAuthoring;
	if ( !loadSnapshot( page ) ) return false;
	rHeight = page.height; rControl = page.pControl; rColor = page.pColor; rDerived = page.pDerived;
	return rHeight.is_valid() && rDerived.is_valid();
}

bool T3DRuntimePageStore::loadCollisionNow(
	const Vector2i &rLocation, const T3DRuntimePageConfig &rConfig,
	PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask
) const {
	rHeight.clear(); rBounds.clear(); rHoleMask.clear();
	if ( !m_pRepository || !rConfig.bCookedPagesEnabled ) return false;
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) return false;
	t3dRuntimePreparedPage_t page;
	page.location = rLocation;
	page.pagePath = getPagePath( pRegion, rConfig.dataDirectory );
	page.sourcePath = pRegion->get_path();
	page.expectedChannelFlags = ( uint32_t ) pRegion->getChannelFlags();
	page.expectedSourceSignature = pRegion->getCookedPageSourceSignatureInternal();
	page.nRegionSize = m_pRepository->getRegionSize();
	page.colorDivisor = MAX( 1, rConfig.colorDivisor );
	page.derivedDivisor = MAX( 1, rConfig.derivedDivisor );
	page.middleHeightDivisor = MAX( 1, rConfig.middleHeightDivisor );
	page.consumerMask = RUNTIME_PAGE_CONSUMER_COLLISION;
	page.bEditorAuthoring = rConfig.bEditorAuthoring;
	if ( !loadSnapshot( page ) ) return false;
	rHeight = page.collisionHeight; rBounds = page.collisionBounds; rHoleMask = page.holeMask;
	return !rHeight.is_empty() && !rBounds.is_empty() && !rHoleMask.is_empty();
}

bool T3DRuntimePageStore::loadNavigationNow(
	const Vector2i &rLocation, const T3DRuntimePageConfig &rConfig,
	PackedByteArray &rNavigation, PackedByteArray *const pCollisionHeight,
	PackedByteArray *const pMinMaxHeight
) const {
	rNavigation.clear(); if ( pCollisionHeight ) pCollisionHeight->clear(); if ( pMinMaxHeight ) pMinMaxHeight->clear();
	if ( !m_pRepository || !rConfig.bCookedPagesEnabled ) return false;
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) return false;
	t3dRuntimePreparedPage_t page;
	page.location = rLocation;
	page.pagePath = getPagePath( pRegion, rConfig.dataDirectory );
	page.sourcePath = pRegion->get_path();
	page.expectedChannelFlags = ( uint32_t ) pRegion->getChannelFlags();
	page.expectedSourceSignature = pRegion->getCookedPageSourceSignatureInternal();
	page.nRegionSize = m_pRepository->getRegionSize();
	page.colorDivisor = MAX( 1, rConfig.colorDivisor );
	page.derivedDivisor = MAX( 1, rConfig.derivedDivisor );
	page.middleHeightDivisor = MAX( 1, rConfig.middleHeightDivisor );
	page.consumerMask = RUNTIME_PAGE_CONSUMER_NAVIGATION |
		( pCollisionHeight ? RUNTIME_PAGE_CONSUMER_COLLISION : 0u );
	page.bLoadMinMax = pMinMaxHeight != nullptr;
	page.bEditorAuthoring = rConfig.bEditorAuthoring;
	if ( !loadSnapshot( page ) ) return false;
	rNavigation = page.navigation; if ( pCollisionHeight ) *pCollisionHeight = page.collisionHeight;
	if ( pMinMaxHeight ) *pMinMaxHeight = page.minMaxHeight;
	return !rNavigation.is_empty() && ( !pCollisionHeight || !pCollisionHeight->is_empty() ) &&
		( !pMinMaxHeight || !pMinMaxHeight->is_empty() );
}

bool T3DRuntimePageStore::loadLightingReceiversNow(
	const Vector2i &rLocation, const T3DRuntimePageConfig &rConfig, PackedByteArray &rReceivers
) const {
	rReceivers.clear();
	if ( !m_pRepository || !rConfig.bCookedPagesEnabled ) return false;
	T3DRegion *const pRegion = m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) return false;
	const String path = getPagePath( pRegion, rConfig.dataDirectory );
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) return false;
	const PackedByteArray header_bytes = pFile->get_buffer( TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES );
	if ( header_bytes.size() != TERRAIN3D_RUNTIME_PAGE_HEADER_BYTES ) return false;
	const t3dRuntimePageHeader_t header = t3dRuntimePageStoreDecodeHeader( header_bytes );
	if ( !terrain3dRuntimePageValidateHeader( header, pFile->get_length() ) ||
		header.nRegionSize != ( uint32_t ) m_pRepository->getRegionSize() ||
		header.regionX != rLocation.x || header.regionZ != rLocation.y ) return false;
	const t3dRuntimePageChannelInfo_t *const pChannel =
		terrain3dRuntimePageFindChannel( header, TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS );
	if ( !pChannel ) return true;
	uint64_t read_usec = 0u, validation_usec = 0u;
	rReceivers = t3dRuntimePageStoreReadBlob( pFile, *pChannel, read_usec, validation_usec );
	return !rReceivers.is_empty();
}

bool T3DRuntimePageStore::writePage(
	T3DRegion *const pRegion, const T3DRuntimePageConfig &rConfig,
	const T3DRuntimePageCookPayload &rPayload, String *const pError
) const {
	if ( pError ) {
		*pError = String();
	}
	auto fail = [pError]( const String &rMessage ) -> bool {
		if ( pError ) {
			*pError = rMessage;
		}
		return false;
	};
	if ( !pRegion || pRegion->isDeleted() || !m_pRepository ) {
		return fail( "region/repository is unavailable" );
	}
	const int region_size = m_pRepository->getRegionSize();
	const int color_size = MAX( 1, region_size / MAX( 1, rConfig.colorDivisor ) );
	const int derived_size = MAX( 1, region_size / MAX( 1, rConfig.derivedDivisor ) );
	const int middle_height_size = MAX( 1,
		( region_size + MAX( 1, rConfig.middleHeightDivisor ) - 1 ) / MAX( 1, rConfig.middleHeightDivisor ) );
	if ( !t3dRuntimePageStoreValidImage( rPayload.height, region_size, region_size, Image::FORMAT_R16, false ) ) {
		return fail( "height source is missing or has an invalid size/format" );
	}
	if ( rPayload.pControl.is_valid() &&
		!t3dRuntimePageStoreValidImage( rPayload.pControl, region_size, region_size, Image::FORMAT_RF, false ) ) {
		return fail( "render-control page has an invalid size/format" );
	}
	if ( rPayload.pColor.is_valid() &&
		!t3dRuntimePageStoreValidImage( rPayload.pColor, color_size, color_size, Image::FORMAT_RGBA8, true ) ) {
		return fail( "color page has an invalid size/format" );
	}
	if ( !t3dRuntimePageStoreValidImage( rPayload.pDerived, derived_size, derived_size, Image::FORMAT_RG8, false ) ) {
		return fail( "derived normal/slope page has an invalid size/format" );
	}
	if ( !t3dRuntimePageStoreValidImage( rPayload.pMiddleHeight, middle_height_size, middle_height_size, Image::FORMAT_R16, false ) ) {
		return fail( "middle-height page has an invalid size/format" );
	}
	if ( !t3dRuntimePageStoreValidImage( rPayload.pMiddleComposite, color_size, color_size, Image::FORMAT_RGBA8, true ) ) {
		return fail( "Middle composite page has an invalid size/format" );
	}
	if ( rPayload.pTerrainShadowMask.is_valid() &&
		( rPayload.pTerrainShadowMask->is_empty() || rPayload.pTerrainShadowMask->get_format() != Image::FORMAT_L8 ||
			!rPayload.pTerrainShadowMask->has_mipmaps() ||
			rPayload.pTerrainShadowMask->get_width() != rPayload.pTerrainShadowMask->get_height() ) ) {
		return fail( "terrain shadow mask must be a square mipmapped L8 image" );
	}
	const uint32_t optional_flags = ( uint32_t ) ( pRegion->getChannelFlags() ) &
		( uint32_t ) ( REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR );
	if ( ( ( optional_flags & REGION_CHANNEL_CONTROL ) != 0u ) != rPayload.pControl.is_valid() ||
		( ( optional_flags & REGION_CHANNEL_COLOR ) != 0u ) != rPayload.pColor.is_valid() ) {
		return fail( "optional cooked channels do not match region channel metadata" );
	}
	if ( rPayload.materialMask == 0u ) {
		return fail( "material mask is empty" );
	}

	const String page_path = getPagePath( pRegion, rConfig.dataDirectory );
	if ( page_path.is_empty() ) {
		return fail( "runtime page path is empty" );
	}
	const uint64_t source_signature = computeSourceSignature( pRegion, rConfig );
	if ( source_signature == 0u ) {
		return fail( "source signature generation failed" );
	}

	t3dRuntimePageHeader_t header;
	header.nRegionSize = ( uint32_t ) ( region_size );
	header.colorDivisor = ( uint32_t ) ( MAX( 1, rConfig.colorDivisor ) );
	header.derivedDivisor = ( uint32_t ) ( MAX( 1, rConfig.derivedDivisor ) );
	header.channelFlags = ( uint32_t ) ( pRegion->getChannelFlags() ) |
		( uint32_t ) ( REGION_CHANNEL_DERIVED );
	header.sourceSignatureLo = ( uint32_t ) ( source_signature & 0xffffffffu );
	header.sourceSignatureHi = ( uint32_t ) ( source_signature >> 32u );
	header.blendRecipeVersion = TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION;
	header.materialMask = rPayload.materialMask;
	header.middleHeightDivisor = ( uint32_t ) ( MAX( 1, rConfig.middleHeightDivisor ) );
	uint64_t world_signature = 1469598103934665603ull;
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, ( uint32_t ) ( region_size ) );
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, ( uint32_t ) ( MAX( 1, rConfig.colorDivisor ) ) );
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, ( uint32_t ) ( MAX( 1, rConfig.derivedDivisor ) ) );
	t3dRuntimePageStoreFnv1a64AppendU32( world_signature, TERRAIN3D_RUNTIME_PAGE_BLEND_RECIPE_VERSION );
	header.worldSignatureLo = ( uint32_t ) ( world_signature & 0xffffffffu );
	header.worldSignatureHi = ( uint32_t ) ( world_signature >> 32u );
	header.regionX = pRegion->getLocation().x;
	header.regionZ = pRegion->getLocation().y;
	const Vector2 height_range = pRegion->getHeightRange();
	const float min_height = ( float ) ( height_range.x );
	const float max_height = ( float ) ( height_range.y );
	std::memcpy( &header.boundsMinYBits, &min_height, sizeof( uint32_t ) );
	std::memcpy( &header.boundsMaxYBits, &max_height, sizeof( uint32_t ) );

	t3dStagedPageBuild_t page_build;
	PackedByteArray file_bytes;
	const PackedByteArray instance_page = pRegion->getInstancePage();
	if ( !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_HEIGHT, rPayload.height ) ||
		( rPayload.pControl.is_valid() && !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_CONTROL, rPayload.pControl ) ) ||
		( rPayload.pColor.is_valid() && !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_COLOR, rPayload.pColor ) ) ||
		!t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_DERIVED, rPayload.pDerived ) ||
		!t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_HEIGHT, rPayload.pMiddleHeight ) ||
		!t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_MIDDLE_COMPOSITE, rPayload.pMiddleComposite ) ||
		( rPayload.pTerrainShadowMask.is_valid() && !t3dRuntimePageStoreStageImageChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_TERRAIN_SHADOW_MASK, rPayload.pTerrainShadowMask ) ) ||
		( !instance_page.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_STATIC_INSTANCES, instance_page ) ) ||
		( !rPayload.holeMask.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_HOLE_MASK, rPayload.holeMask ) ) ||
		( !rPayload.collisionHeight.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_COLLISION_HEIGHT, rPayload.collisionHeight ) ) ||
		( !rPayload.collisionBounds.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_COLLISION_BOUNDS, rPayload.collisionBounds ) ) ||
		( !rPayload.navigation.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_NAVIGATION, rPayload.navigation ) ) ||
		( !rPayload.minMaxHeight.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_MIN_MAX_HEIGHT, rPayload.minMaxHeight ) ) ||
		( !rPayload.nodeDescriptors.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_NODE_DESCRIPTORS, rPayload.nodeDescriptors ) ) ||
		( !rPayload.instanceMetadata.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_INSTANCE_METADATA, rPayload.instanceMetadata ) ) ||
		( !rPayload.lightingReceivers.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_LIGHTING_RECEIVERS, rPayload.lightingReceivers ) ) ||
		( !rPayload.multimeshFastStart.is_empty() && !t3dRuntimePageStoreStageBlobChannel( page_build, header, TERRAIN3D_RUNTIME_PAGE_MULTIMESH_FAST_START, rPayload.multimeshFastStart ) ) ||
		!t3dRuntimePageStoreFinalizePageBytes( page_build, header, file_bytes ) ) {
		return fail( "runtime page channel serialization failed" );
	}
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

	if ( !validatePageForRegion( pRegion, rConfig ) ) {
		DirAccess::remove_absolute( page_path );
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, page_path );
		}
		return fail( "published runtime page failed reload validation" );
	}
	if ( bHadPrevious ) {
		DirAccess::remove_absolute( backup_path );
	}
	t3dRuntimePageStoreUpdateEditorFile( page_path );
	return true;
}

bool T3DRuntimePageStore::collectPageSources(
	const String &rDataDirectory, PackedStringArray &rSourceFiles,
	eastl::vector<Vector2i> &rLocations, PackedStringArray &rErrors
) const {
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

	rSourceFiles = T3DUtil::getFiles( data_directory, "terrain3d*.res" );
	rSourceFiles.sort();
	if ( rSourceFiles.is_empty() ) {
		rErrors.push_back( "No Terrain3D region .res sources were found in " + data_directory );
		return false;
	}

	eastl::bitset<TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE * TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE> source_locations;
	rLocations.reserve( ( size_t ) ( rSourceFiles.size() ) );
	for ( const String &fname : rSourceFiles ) {
		const Vector2i location = T3DUtil::filenameToLocation( fname );
		const int map_index = T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || !m_pRepository->isLocationValid( location ) || T3DUtil::locationToFilename( location ) != fname ) {
			rErrors.push_back( "Malformed or out-of-range Terrain3D region source: " + fname );
			continue;
		}
		if ( source_locations.test( ( size_t ) ( map_index ) ) ) {
			rErrors.push_back( "Duplicate Terrain3D region source location " + String( location ) );
			continue;
		}
		source_locations.set( ( size_t ) ( map_index ) );

		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			rErrors.push_back( "Region source " + fname + " exists in the data directory but is not loaded" );
			continue;
		}
		const String expected_path = data_directory.path_join( fname ).simplify_path();
		const String loaded_path = pRegion->get_path().simplify_path();
		if ( loaded_path != expected_path ) {
			rErrors.push_back( "Region " + String( location ) + " is loaded from " +
				( loaded_path.is_empty() ? String( "<unsaved resource>" ) : loaded_path ) +
				" instead of data-directory source " + expected_path );
			continue;
		}
		rLocations.push_back( location );
	}

	const TypedArray<Vector2i> loaded_locations = m_pRepository->getActiveLocations();
	for ( const Vector2i location : loaded_locations ) {
		const int map_index = T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || !source_locations.test( ( size_t ) ( map_index ) ) ) {
			rErrors.push_back( "Loaded region " + String( location ) +
				" has no matching .res source in " + data_directory );
		}
	}
	return rErrors.is_empty();
}

void T3DRuntimePageStore::prepareTask( void *const pContext, const uint32_t iIndex ) {
	t3dRuntimePrepareJobContext_t *const context = ( t3dRuntimePrepareJobContext_t * ) ( pContext );
	if ( !context || !context->pPages || iIndex >= ( uint32_t ) ( context->nPageCount ) ) {
		return;
	}
	t3dRuntimePreparedPage_t &page = context->pPages[iIndex];
	Time *const pTime = Time::get_singleton();
	page.nWorkerStartedUsec = pTime ? pTime->get_ticks_usec() : page.nRequestedUsec;
	page.bAttempted = true;
	page.bLoaded = loadSnapshot( page );
	page.bInstanceAttempted = !page.instancePage.is_empty();
	if ( page.bInstanceAttempted ) {
		uint32_t mesh_counts[TERRAIN3D_MAX_MESH_ASSETS] = {};
		uint32_t instance_count = 0u;
		uint16_t mesh_count = 0u;
		page.bInstanceLoaded = T3DInstancePageCodec::decode(
			page.instancePage, page.preparedInstanceStore, mesh_counts, instance_count, mesh_count );
	}
	page.nWorkerFinishedUsec = pTime ? pTime->get_ticks_usec() : page.nWorkerStartedUsec;
}

void T3DRuntimePageStore::poll( const bool bWait ) {
	if ( !m_bPrepareJobActive ) {
		return;
	}
	WorkerThreadPool *const pool = WorkerThreadPool::get_singleton();
	if ( pool && !bWait && !pool->is_group_task_completed( m_nPrepareGroupId ) ) {
		return;
	}
	if ( pool && m_nPrepareGroupId != WorkerThreadPool::INVALID_TASK_ID ) {
		pool->wait_for_group_task_completion( m_nPrepareGroupId );
	}
	m_nPrepareQueueWaitUsecLast = 0u;
	m_nPrepareFileOpenUsecLast = 0u;
	m_nPrepareFileReadUsecLast = 0u;
	m_nPrepareValidationUsecLast = 0u;
	m_nPrepareMaterializeUsecLast = 0u;
	m_nPrepareWorkerUsecLast = 0u;
	for ( int work_index = 0; work_index < m_nPrepareWorkPageCount; ++work_index ) {
		t3dRuntimePreparedPage_t &work_page = m_workPages[( size_t ) ( work_index )];
		if ( work_page.nWorkerStartedUsec >= work_page.nRequestedUsec ) {
			m_nPrepareQueueWaitUsecLast = MAX(
				m_nPrepareQueueWaitUsecLast, work_page.nWorkerStartedUsec - work_page.nRequestedUsec );
		}
		if ( work_page.nWorkerFinishedUsec >= work_page.nWorkerStartedUsec ) {
			m_nPrepareWorkerUsecLast = MAX(
				m_nPrepareWorkerUsecLast, work_page.nWorkerFinishedUsec - work_page.nWorkerStartedUsec );
		}
		m_nPrepareFileOpenUsecLast += work_page.nFileOpenUsec;
		m_nPrepareFileReadUsecLast += work_page.nFileReadUsec;
		m_nPrepareValidationUsecLast += work_page.nValidationUsec;
		m_nPrepareMaterializeUsecLast += work_page.nMaterializeUsec;

		const uint16_t worker_consumer_mask = work_page.consumerMask;
		const int map_index = T3DRegionRepository::getRegionMapIndex( work_page.location );
		if ( map_index >= 0 ) {
			work_page.consumerMask |= m_queuedConsumers[( size_t ) ( map_index )];
			if ( m_queuedConsumers[( size_t ) ( map_index )] != 0u ) {
				m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
			}
			m_queuedConsumers[( size_t ) ( map_index )] = 0u;
			m_queuedPriorities[( size_t ) ( map_index )] = 0u;
		}
		int destination = map_index >= 0 ? m_preparedByRegion[( size_t ) map_index] : -1;
		if ( destination >= 0 && destination < m_nPreparedPageCount ) {
			work_page.consumerMask |= m_preparedPages[( size_t ) destination].consumerMask;
		} else {
			destination = -1;
		}
		// A request can gain consumers while the worker is already reading the page.
		// Re-materialize once with the final mask before publishing the snapshot;
		// otherwise a late CPU/GPU/metadata consumer could observe a valid page whose
		// required payload was intentionally skipped by the original worker read.
		if ( work_page.bLoaded && work_page.consumerMask != worker_consumer_mask ) {
			work_page.bLoaded = loadSnapshot( work_page, true );
			work_page.bInstanceAttempted = false;
			work_page.bInstanceLoaded = false;
			if ( work_page.bLoaded &&
				( work_page.consumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) != 0u &&
				!work_page.instancePage.is_empty() ) {
				work_page.bInstanceAttempted = true;
				uint32_t mesh_counts[TERRAIN3D_MAX_MESH_ASSETS] = {};
				uint32_t instance_count = 0u;
				uint16_t mesh_count = 0u;
				work_page.bInstanceLoaded = T3DInstancePageCodec::decode(
					work_page.instancePage, work_page.preparedInstanceStore, mesh_counts, instance_count, mesh_count );
			}
		}
		if ( destination < 0 && m_nPreparedPageCount < TERRAIN3D_GPU_CACHE_MAX_LAYERS ) {
			destination = m_nPreparedPageCount++;
		}
		if ( destination >= 0 ) {
			m_preparedPages[( size_t ) destination] = std::move( work_page );
			if ( map_index >= 0 ) m_preparedByRegion[( size_t ) map_index] = ( int16_t ) destination;
		}
		if ( map_index >= 0 ) m_workByRegion[( size_t ) map_index] = ( int16_t ) -1;
		work_page = t3dRuntimePreparedPage_t{};
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t finished = pTime ? pTime->get_ticks_usec() : m_nPrepareJobStartedUsec;
	m_nPrepareLatencyUsecLast = finished >= m_nPrepareJobStartedUsec ? finished - m_nPrepareJobStartedUsec : 0u;
	m_nPrepareWorkPageCount = 0;
	m_workByRegion.fill( ( int16_t ) -1 );
	m_prepareContext = t3dRuntimePrepareJobContext_t{};
	m_nPrepareGroupId = WorkerThreadPool::INVALID_TASK_ID;
	m_bPrepareJobActive = false;
	m_nPrepareJobWorkers = 0;
}

int T3DRuntimePageStore::getPreparedIndex( const Vector2i &rLocation ) const {
	const int map_index = T3DRegionRepository::getRegionMapIndex( rLocation ); if ( map_index < 0 ) return -1; const int index = m_preparedByRegion[( size_t ) map_index]; return index >= 0 && index < m_nPreparedPageCount && m_preparedPages[( size_t ) index].location == rLocation ? index : -1;
}

int T3DRuntimePageStore::getWorkIndex( const Vector2i &rLocation ) const {
	const int map_index = T3DRegionRepository::getRegionMapIndex( rLocation ); if ( map_index < 0 ) return -1; const int index = m_workByRegion[( size_t ) map_index]; return index >= 0 && index < m_nPrepareWorkPageCount && m_workPages[( size_t ) index].location == rLocation ? index : -1;
}

bool T3DRuntimePageStore::isPending( const Vector2i &rLocation ) const {
	return m_bPrepareJobActive && getWorkIndex( rLocation ) >= 0;
}

bool T3DRuntimePageStore::isPrepared( const Vector2i &rLocation ) const {
	return getPreparedIndex( rLocation ) >= 0;
}

void T3DRuntimePageStore::request(
	const Vector2i &rLocation, const uint16_t nConsumers,
	const uint8_t nPriority, const bool bEnabled
) {
	if ( nConsumers == 0u || !bEnabled || !m_pRepository ||
		!m_pRepository->isLocationValid( rLocation ) || !m_pRepository->getRegionPtr( rLocation ) ) {
		return;
	}
	const int ready_index = getPreparedIndex( rLocation );
	if ( ready_index >= 0 ) { m_preparedPages[( size_t ) ready_index].consumerMask |= nConsumers; return; }
	const int map_index = T3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 ) {
		return;
	}
	if ( m_queuedConsumers[( size_t ) ( map_index )] == 0u ) {
		++m_nQueuedCount;
	}
	m_queuedConsumers[( size_t ) ( map_index )] |= nConsumers;
	const uint8_t priority = MAX( ( uint8_t ) ( 1u ), nPriority );
	if ( m_queuedPriorities[( size_t ) ( map_index )] == 0u ||
		priority < m_queuedPriorities[( size_t ) ( map_index )] ) {
		m_queuedPriorities[( size_t ) ( map_index )] = priority;
	}
}

void T3DRuntimePageStore::service( const T3DRuntimePageServiceState &rState ) {
	poll( false );
	if ( !rState.config.bCookedPagesEnabled || !m_pRepository ) {
		return;
	}

	if ( !rState.config.bEditorAuthoring && rState.streamingGeneration != m_nStreamingGenerationLast ) {
		for ( int map_index = 0; map_index < TERRAIN3D_MAX_REGION_SLOTS; ++map_index ) {
			if ( m_queuedConsumers[( size_t ) ( map_index )] == 0u ) {
				continue;
			}
			const int map_x = map_index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
			const int map_y = map_index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
			const Vector2i location(
				map_x - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
				map_y - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
			);
			const bool wanted = m_pRepository->getRegionPtr( location ) &&
				( !rState.pIsWanted || rState.pIsWanted( rState.pWantedContext, location ) );
			if ( !wanted ) {
				m_queuedConsumers[( size_t ) ( map_index )] = 0u;
				m_queuedPriorities[( size_t ) ( map_index )] = 0u;
				m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
			}
		}
		for ( int ready_index = m_nPreparedPageCount - 1; ready_index >= 0; --ready_index ) {
			const Vector2i location = m_preparedPages[( size_t ) ( ready_index )].location;
			const bool wanted = m_pRepository->getRegionPtr( location ) &&
				( !rState.pIsWanted || rState.pIsWanted( rState.pWantedContext, location ) );
			if ( !wanted ) {
				consumePreparedIndex( ready_index );
			}
		}
	}
	m_nStreamingGenerationLast = rState.streamingGeneration;
	if ( m_bPrepareJobActive || m_nQueuedCount <= 0 ) {
		return;
	}

	const int free_ready_slots = TERRAIN3D_GPU_CACHE_MAX_LAYERS - m_nPreparedPageCount;
	if ( free_ready_slots <= 0 ) {
		return;
	}
	const int batch_limit = MIN(
		free_ready_slots,
		MIN( TERRAIN3D_GPU_CACHE_MAX_LAYERS, MAX( 2, MAX( 1, rState.config.prepareThreads ) * 2 ) )
	);
	m_nPrepareWorkPageCount = 0;
	while ( m_nPrepareWorkPageCount < batch_limit && m_nQueuedCount > 0 ) {
		int best_slot = -1;
		int best_map_index = -1;
		uint8_t best_priority = UINT8_MAX;
		int best_distance = INT32_MAX;
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			if ( location == V2I_MAX ) {
				continue;
			}
			const int map_index = T3DRegionRepository::getRegionMapIndex( location );
			if ( map_index < 0 || m_queuedConsumers[( size_t ) ( map_index )] == 0u || isPending( location ) ) {
				continue;
			}
			T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->runtimeMapsEvictableInternal() ) {
				m_queuedConsumers[( size_t ) ( map_index )] = 0u;
				m_queuedPriorities[( size_t ) ( map_index )] = 0u;
				m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
				continue;
			}
			const uint8_t priority = m_queuedPriorities[( size_t ) ( map_index )];
			const int distance = MAX(
				Math::abs( location.x - rState.predictedCenter.x ),
				Math::abs( location.y - rState.predictedCenter.y )
			);
			if ( priority < best_priority ||
				( priority == best_priority && distance < best_distance ) ||
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

		const Vector2i location = m_pRepository->getSlotsData()[best_slot];
		T3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		const int work_index = m_nPrepareWorkPageCount++;
		t3dRuntimePreparedPage_t &page = m_workPages[( size_t ) work_index];
		page = t3dRuntimePreparedPage_t{};
		page.location = location;
		m_workByRegion[( size_t ) best_map_index] = ( int16_t ) work_index;
		page.pagePath = getPagePath( pRegion, rState.config.dataDirectory );
		page.sourcePath = pRegion->get_path();
		page.expectedChannelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
		page.expectedSourceSignature = pRegion->getCookedPageSourceSignatureInternal();
		page.nRegionSize = m_pRepository->getRegionSize();
		page.colorDivisor = MAX( 1, rState.config.colorDivisor );
		page.derivedDivisor = MAX( 1, rState.config.derivedDivisor );
		page.middleHeightDivisor = MAX( 1, rState.config.middleHeightDivisor );
		page.consumerMask = m_queuedConsumers[( size_t ) ( best_map_index )];
		page.bEditorAuthoring = rState.config.bEditorAuthoring;
		Time *const pTime = Time::get_singleton();
		page.nRequestedUsec = pTime ? pTime->get_ticks_usec() : 0u;
		m_queuedConsumers[( size_t ) ( best_map_index )] = 0u;
		m_queuedPriorities[( size_t ) ( best_map_index )] = 0u;
		m_nQueuedCount = MAX( 0, m_nQueuedCount - 1 );
	}
	if ( m_nPrepareWorkPageCount <= 0 ) {
		return;
	}

	m_nPrepareJobWorkers = MIN( m_nPrepareWorkPageCount, MAX( 1, rState.config.prepareThreads ) );
	m_prepareContext.pPages = m_workPages.data();
	m_prepareContext.nPageCount = m_nPrepareWorkPageCount;
	Time *const pTime = Time::get_singleton();
	m_nPrepareJobStartedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	WorkerThreadPool *const pool = WorkerThreadPool::get_singleton();
	if ( !pool ) {
		for ( int index = 0; index < m_nPrepareWorkPageCount; ++index ) {
			prepareTask( &m_prepareContext, ( uint32_t ) ( index ) );
		}
		m_bPrepareJobActive = true;
		m_nPrepareGroupId = WorkerThreadPool::INVALID_TASK_ID;
		poll( true );
		return;
	}
	m_nPrepareGroupId = pool->add_native_group_task(
		&prepareTask, &m_prepareContext, m_nPrepareWorkPageCount, m_nPrepareJobWorkers,
		false, "Terrain3D cooked page preparation"
	);
	if ( m_nPrepareGroupId == WorkerThreadPool::INVALID_TASK_ID ) {
		for ( int index = 0; index < m_nPrepareWorkPageCount; ++index ) {
			prepareTask( &m_prepareContext, ( uint32_t ) ( index ) );
		}
		m_bPrepareJobActive = true;
		poll( true );
		return;
	}
	m_bPrepareJobActive = true;
}

void T3DRuntimePageStore::consumePreparedIndex( const int nIndex ) {
	if ( nIndex < 0 || nIndex >= m_nPreparedPageCount ) return;
	const int removed_map_index = T3DRegionRepository::getRegionMapIndex( m_preparedPages[( size_t ) nIndex].location );
	if ( removed_map_index >= 0 ) m_preparedByRegion[( size_t ) removed_map_index] = ( int16_t ) -1;
	const int last = m_nPreparedPageCount - 1;
	if ( nIndex != last ) {
		m_preparedPages[( size_t ) nIndex] = std::move( m_preparedPages[( size_t ) last] );
		const int moved_map_index = T3DRegionRepository::getRegionMapIndex( m_preparedPages[( size_t ) nIndex].location );
		if ( moved_map_index >= 0 ) m_preparedByRegion[( size_t ) moved_map_index] = ( int16_t ) nIndex;
	}
	m_preparedPages[( size_t ) last] = t3dRuntimePreparedPage_t{};
	--m_nPreparedPageCount;
}

bool T3DRuntimePageStore::takeHeight(
	const Vector2i &rLocation, const uint16_t nConsumer, Ref<Image> &rHeight,
	const bool bMiddle, PackedByteArray *const pNativeHeightBytes
) {
	poll( false );
	rHeight.unref();
	if ( pNativeHeightBytes ) {
		pNativeHeightBytes->clear();
	}
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & nConsumer ) == 0u ) return false;
		if ( page.bLoaded ) {
			rHeight = bMiddle ? page.pMiddleHeight : page.height;
			if ( pNativeHeightBytes && !bMiddle ) {
				*pNativeHeightBytes = page.heightBytes;
			}
		}
		page.consumerMask &= ( uint16_t ) ( ~nConsumer );
		const bool loaded = page.bLoaded &&
			( bMiddle ? rHeight.is_valid() : ( rHeight.is_valid() || ( pNativeHeightBytes && !pNativeHeightBytes->is_empty() ) ) );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeInstance(
	const Vector2i &rLocation, T3DInstanceStore &rStore,
	PackedByteArray *const pInstanceMetadata, PackedByteArray *const pMultimeshFastStart
) {
	poll( false );
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_INSTANCE_STORE ) == 0u ) return false;
		const bool loaded = page.bInstanceAttempted && page.bInstanceLoaded;
		if ( loaded ) {
			rStore = eastl::move( page.preparedInstanceStore );
			if ( pInstanceMetadata ) { *pInstanceMetadata = page.instanceMetadata; }
			if ( pMultimeshFastStart ) { *pMultimeshFastStart = page.multimeshFastStart; }
		}
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_INSTANCE_STORE );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeCpuMaps(
	const Vector2i &rLocation, Ref<Image> &rHeight, Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived
) {
	poll( false );
	rHeight.unref();
	rControl.unref();
	rColor.unref();
	rDerived.unref();
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_CPU_MAPS ) == 0u ) return false;
		if ( page.bLoaded ) {
			rHeight = page.height;
			rControl = page.pControl;
			rColor = page.pColor;
			rDerived = page.pDerived;
		}
		const bool loaded = page.bLoaded && rHeight.is_valid() && rDerived.is_valid();
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_CPU_MAPS );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeNodeDescriptors( const Vector2i &rLocation, PackedByteArray &rDescriptors ) {
	poll( false );
	rDescriptors.clear();
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS ) == 0u ) return false;
		if ( page.bLoaded ) {
			rDescriptors = page.nodeDescriptors;
		}
		const bool loaded = page.bLoaded && ( !rDescriptors.is_empty() || page.nodeDescriptors.is_empty() );
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeCollision(
	const Vector2i &rLocation, PackedByteArray &rHeight, PackedByteArray &rBounds, PackedByteArray &rHoleMask
) {
	poll( false );
	rHeight.clear();
	rBounds.clear();
	rHoleMask.clear();
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_COLLISION ) == 0u ) return false;
		if ( page.bLoaded ) {
			rHeight = page.collisionHeight;
			rBounds = page.collisionBounds;
			rHoleMask = page.holeMask;
		}
		const bool loaded = page.bLoaded && !rHeight.is_empty() && !rBounds.is_empty() && !rHoleMask.is_empty();
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_COLLISION );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeNavigation( const Vector2i &rLocation, PackedByteArray &rNavigation ) {
	poll( false );
	rNavigation.clear();
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_NAVIGATION ) == 0u ) return false;
		if ( page.bLoaded ) {
			rNavigation = page.navigation;
		}
		const bool loaded = page.bLoaded && !rNavigation.is_empty();
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_NAVIGATION );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeTerrainShadowMask( const Vector2i &rLocation, Ref<Image> &rShadowMask ) {
	poll( false );
	rShadowMask.unref();
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW ) == 0u ) return false;
		if ( page.bLoaded ) {
			rShadowMask = page.pTerrainShadowMask;
		}
		const bool loaded = page.bLoaded && rShadowMask.is_valid() && !rShadowMask->is_empty();
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

bool T3DRuntimePageStore::takeGpuPage(
	const Vector2i &rLocation, Ref<Image> &rHeight,
	Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived,
	Ref<Image> &rMiddleComposite, uint32_t &rMaterialMask, bool &bAttempted,
	uint64_t *const pRequestedUsec
) {
	poll( false );
	bAttempted = false;
	rMaterialMask = 1u;
	if ( pRequestedUsec ) {
		*pRequestedUsec = 0u;
	}
	const int index = getPreparedIndex( rLocation ); if ( index < 0 ) return false; t3dRuntimePreparedPage_t &page = m_preparedPages[( size_t ) index];
	if ( ( page.consumerMask & RUNTIME_PAGE_CONSUMER_GPU_DETAIL ) == 0u ) return false;
		bAttempted = page.bAttempted;
		if ( pRequestedUsec ) {
			*pRequestedUsec = page.nRequestedUsec;
		}
		Time *const pTime = Time::get_singleton();
		const uint64_t consumed_usec = pTime ? pTime->get_ticks_usec() : page.nWorkerFinishedUsec;
		if ( page.nWorkerFinishedUsec > 0u && consumed_usec >= page.nWorkerFinishedUsec ) {
			m_nPrepareReadyWaitUsecLast = MAX(
				m_nPrepareReadyWaitUsecLast, consumed_usec - page.nWorkerFinishedUsec );
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
		page.consumerMask &= ( uint16_t ) ( ~RUNTIME_PAGE_CONSUMER_GPU_DETAIL );
		if ( page.consumerMask == 0u ) {
			consumePreparedIndex( index );
		}
	return loaded;
}

T3DRuntimePageStats T3DRuntimePageStore::getStats( void ) const {
	T3DRuntimePageStats stats;
	stats.nPreparedPages = m_nPreparedPageCount;
	stats.nPendingPages = m_bPrepareJobActive ? m_nPrepareWorkPageCount : 0;
	stats.nQueuedPages = m_nQueuedCount;
	stats.nPrepareWorkers = m_bPrepareJobActive ? m_nPrepareJobWorkers : 0;
	stats.bPrepareJobActive = m_bPrepareJobActive;
	for ( int index = 0; index < m_nPreparedPageCount; ++index ) {
		stats.nPreparedLoadedPages += m_preparedPages[( size_t ) ( index )].bLoaded ? 1 : 0;
	}
	stats.nPrepareLatencyUsecLast = m_nPrepareLatencyUsecLast;
	stats.nPrepareQueueWaitUsecLast = m_nPrepareQueueWaitUsecLast;
	stats.nPrepareFileOpenUsecLast = m_nPrepareFileOpenUsecLast;
	stats.nPrepareFileReadUsecLast = m_nPrepareFileReadUsecLast;
	stats.nPrepareValidationUsecLast = m_nPrepareValidationUsecLast;
	stats.nPrepareMaterializeUsecLast = m_nPrepareMaterializeUsecLast;
	stats.nPrepareWorkerUsecLast = m_nPrepareWorkerUsecLast;
	stats.nPrepareReadyWaitUsecLast = m_nPrepareReadyWaitUsecLast;
	return stats;
}
