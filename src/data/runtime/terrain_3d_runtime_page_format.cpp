#include "data/runtime/terrain_3d_runtime_page_format.h"

bool t3dRuntimePageValidateHeader( const t3dRuntimePageHeader_t &rHeader, const uint64_t nFileBytes ) {
	if ( rHeader.m_nMagic != T3D_RUNTIME_PAGE_MAGIC || rHeader.m_nVersion != T3D_RUNTIME_PAGE_VERSION
	        || rHeader.m_nHeaderBytes != T3D_RUNTIME_PAGE_HEADER_BYTES || rHeader.m_nReservedGeometry != 0u || rHeader.m_nColorDivisor == 0u
	        || rHeader.m_nDerivedDivisor == 0u || rHeader.m_nTotalBytes < rHeader.m_nHeaderBytes || rHeader.m_nChannelCount == 0u
	        || rHeader.m_nChannelCount > T3D_RUNTIME_PAGE_CHANNEL_COUNT || rHeader.m_iChannelTableOffset != 40u
	        || rHeader.m_nTotalBytes > T3D_RUNTIME_PAGE_MAX_BYTES || ( uint64_t )( rHeader.m_nTotalBytes ) != nFileBytes
	        || ( rHeader.m_nColorDivisor != 1u && rHeader.m_nColorDivisor != 2u && rHeader.m_nColorDivisor != 4u )
	        || ( rHeader.m_nDerivedDivisor != 1u && rHeader.m_nDerivedDivisor != 2u && rHeader.m_nDerivedDivisor != 4u )
	        || ( rHeader.m_nChannelFlags & ~7u ) != 0u ) {
		return false;
	}
	if ( rHeader.m_nBlendRecipeVersion == 0u || rHeader.m_nMaterialMask == 0u
	        || ( rHeader.m_nWorldSignatureLo == 0u && rHeader.m_nWorldSignatureHi == 0u )
	        || ( rHeader.m_nMiddleHeightDivisor != 1u && rHeader.m_nMiddleHeightDivisor != 2u && rHeader.m_nMiddleHeightDivisor != 4u ) ) {
		return false;
	}
	uint32_t previous_end = rHeader.m_nHeaderBytes;
	for ( uint32_t channel = 0u; channel < rHeader.m_nChannelCount; ++channel ) {
		const t3dRuntimePageChannelInfo_t &info = rHeader.m_szChannels[channel];
		if ( info.m_nType == 0u || info.m_nCompressedBytes == 0u || info.m_nUncompressedBytes == 0u
		        || ( info.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE
		                && info.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE )
		        || ( info.m_nCompression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE
		                && info.m_nCompressedBytes != info.m_nUncompressedBytes )
		        || info.m_nAlignment != T3D_RUNTIME_PAGE_ALIGNMENT ) {
			return false;
		}
		for ( uint32_t previous = 0u; previous < channel; ++previous ) {
			if ( rHeader.m_szChannels[previous].m_nType == info.m_nType ) {
				return false;
			}
		}
		if ( info.m_iOffset < rHeader.m_nHeaderBytes || ( info.m_iOffset & ( info.m_nAlignment - 1u ) ) != 0u
		        || info.m_iOffset < previous_end || info.m_nWidth == 0u || info.m_nHeight == 0u || info.m_nMipCount == 0u
		        || info.m_nCompressedBytes > rHeader.m_nTotalBytes - info.m_iOffset ) {
			return false;
		}
		previous_end = info.m_iOffset + info.m_nCompressedBytes;
	}
	return previous_end <= rHeader.m_nTotalBytes;
}
