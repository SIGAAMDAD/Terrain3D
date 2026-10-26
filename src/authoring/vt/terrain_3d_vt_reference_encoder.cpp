// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/vt/terrain_3d_vt_reference_encoder.h"

#include <string.h>

static uint16_t t3d_vt_reference_rgb565( const uint8_t r, const uint8_t g, const uint8_t b ) {
	return ( uint16_t )( ( ( uint16_t )( r >> 3u ) << 11u ) | ( ( uint16_t )( g >> 2u ) << 5u ) | ( b >> 3u ) );
}

static void t3d_vt_reference_rgb565_decode( const uint16_t nColor, uint8_t *pRgb ) {
	const uint32_t r = ( nColor >> 11u ) & 31u;
	const uint32_t g = ( nColor >> 5u ) & 63u;
	const uint32_t b = nColor & 31u;
	pRgb[0] = ( uint8_t )( ( r << 3u ) | ( r >> 2u ) );
	pRgb[1] = ( uint8_t )( ( g << 2u ) | ( g >> 4u ) );
	pRgb[2] = ( uint8_t )( ( b << 3u ) | ( b >> 2u ) );
}

static uint32_t t3d_vt_reference_rgb_distance( const uint8_t *pPixel, const uint8_t *pColor ) {
	const int32_t dr = ( int32_t )pPixel[0] - pColor[0];
	const int32_t dg = ( int32_t )pPixel[1] - pColor[1];
	const int32_t db = ( int32_t )pPixel[2] - pColor[2];
	return ( uint32_t )( dr * dr + dg * dg + db * db );
}

static void t3d_vt_reference_encode_bc1_block( const uint8_t *pPixels, const uint32_t nStride, uint8_t *pBlock ) {
	uint32_t nMinLuma = UINT32_MAX;
	uint32_t nMaxLuma = 0u;
	uint8_t MinColor[3] = { 0u, 0u, 0u };
	uint8_t MaxColor[3] = { 255u, 255u, 255u };
	bool bHasAlpha = false;
	for ( uint32_t y = 0u; y < 4u; ++y ) {
		for ( uint32_t x = 0u; x < 4u; ++x ) {
			const uint8_t *pPixel = pPixels + y * nStride + x * 4u;
			if ( pPixel[3] < 128u ) {
				bHasAlpha = true;
				continue;
			}
			const uint32_t nLuma = ( uint32_t )pPixel[0] * 77u + ( uint32_t )pPixel[1] * 150u + ( uint32_t )pPixel[2] * 29u;
			if ( nLuma < nMinLuma ) {
				nMinLuma = nLuma;
				MinColor[0] = pPixel[0];
				MinColor[1] = pPixel[1];
				MinColor[2] = pPixel[2];
			}
			if ( nLuma >= nMaxLuma ) {
				nMaxLuma = nLuma;
				MaxColor[0] = pPixel[0];
				MaxColor[1] = pPixel[1];
				MaxColor[2] = pPixel[2];
			}
		}
	}
	if ( nMinLuma == UINT32_MAX ) {
		MinColor[0] = MinColor[1] = MinColor[2] = 0u;
		MaxColor[0] = MaxColor[1] = MaxColor[2] = 0u;
	}

	uint16_t nMin565 = t3d_vt_reference_rgb565( MinColor[0], MinColor[1], MinColor[2] );
	uint16_t nMax565 = t3d_vt_reference_rgb565( MaxColor[0], MaxColor[1], MaxColor[2] );
	uint16_t nColor0 = bHasAlpha ? nMin565 : nMax565;
	uint16_t nColor1 = bHasAlpha ? nMax565 : nMin565;
	if ( bHasAlpha && nColor0 > nColor1 ) {
		const uint16_t nSwap = nColor0;
		nColor0 = nColor1;
		nColor1 = nSwap;
	}
	if ( !bHasAlpha && nColor0 <= nColor1 ) {
		if ( nColor1 < 0xffffu ) {
			nColor0 = nColor1 + 1u;
		} else if ( nColor0 > 0u ) {
			nColor1 = nColor0 - 1u;
		}
	}

	pBlock[0] = ( uint8_t )nColor0;
	pBlock[1] = ( uint8_t )( nColor0 >> 8u );
	pBlock[2] = ( uint8_t )nColor1;
	pBlock[3] = ( uint8_t )( nColor1 >> 8u );
	uint8_t Palette[4][3];
	t3d_vt_reference_rgb565_decode( nColor0, Palette[0] );
	t3d_vt_reference_rgb565_decode( nColor1, Palette[1] );
	if ( nColor0 > nColor1 ) {
		for ( uint32_t c = 0u; c < 3u; ++c ) {
			Palette[2][c] = ( uint8_t )( ( 2u * Palette[0][c] + Palette[1][c] + 1u ) / 3u );
			Palette[3][c] = ( uint8_t )( ( Palette[0][c] + 2u * Palette[1][c] + 1u ) / 3u );
		}
	} else {
		for ( uint32_t c = 0u; c < 3u; ++c ) {
			Palette[2][c] = ( uint8_t )( ( Palette[0][c] + Palette[1][c] + 1u ) >> 1u );
			Palette[3][c] = 0u;
		}
	}

	uint32_t nIndices = 0u;
	for ( uint32_t y = 0u; y < 4u; ++y ) {
		for ( uint32_t x = 0u; x < 4u; ++x ) {
			const uint32_t nPixelIndex = y * 4u + x;
			const uint8_t *pPixel = pPixels + y * nStride + x * 4u;
			uint32_t nBest = 0u;
			if ( bHasAlpha && pPixel[3] < 128u ) {
				nBest = 3u;
			} else {
				uint32_t nBestDistance = UINT32_MAX;
				const uint32_t nPaletteCount = nColor0 > nColor1 ? 4u : 3u;
				for ( uint32_t iPalette = 0u; iPalette < nPaletteCount; ++iPalette ) {
					const uint32_t nDistance = t3d_vt_reference_rgb_distance( pPixel, Palette[iPalette] );
					if ( nDistance < nBestDistance ) {
						nBestDistance = nDistance;
						nBest = iPalette;
					}
				}
			}
			nIndices |= nBest << ( nPixelIndex * 2u );
		}
	}
	pBlock[4] = ( uint8_t )nIndices;
	pBlock[5] = ( uint8_t )( nIndices >> 8u );
	pBlock[6] = ( uint8_t )( nIndices >> 16u );
	pBlock[7] = ( uint8_t )( nIndices >> 24u );
}

static void t3d_vt_reference_bc4_palette( const uint8_t nEndpoint0, const uint8_t nEndpoint1, uint8_t *pPalette ) {
	pPalette[0] = nEndpoint0;
	pPalette[1] = nEndpoint1;
	if ( nEndpoint0 > nEndpoint1 ) {
		for ( uint32_t i = 1u; i <= 6u; ++i ) {
			pPalette[i + 1u] = ( uint8_t )( ( ( 7u - i ) * nEndpoint0 + i * nEndpoint1 + 3u ) / 7u );
		}
	} else {
		for ( uint32_t i = 1u; i <= 4u; ++i ) {
			pPalette[i + 1u] = ( uint8_t )( ( ( 5u - i ) * nEndpoint0 + i * nEndpoint1 + 2u ) / 5u );
		}
		pPalette[6] = 0u;
		pPalette[7] = 255u;
	}
}

static void t3d_vt_reference_encode_bc4_block(
        const uint8_t *pPixels, const uint32_t nStride, const uint32_t nPixelStride, const uint32_t nComponent, uint8_t *pBlock ) {
	uint8_t nMin = 255u;
	uint8_t nMax = 0u;
	for ( uint32_t y = 0u; y < 4u; ++y ) {
		for ( uint32_t x = 0u; x < 4u; ++x ) {
			const uint8_t nValue = pPixels[y * nStride + x * nPixelStride + nComponent];
			nMin = nValue < nMin ? nValue : nMin;
			nMax = nValue > nMax ? nValue : nMax;
		}
	}
	pBlock[0] = nMax;
	pBlock[1] = nMin;
	uint8_t Palette[8];
	t3d_vt_reference_bc4_palette( nMax, nMin, Palette );
	uint64_t nIndices = 0u;
	for ( uint32_t y = 0u; y < 4u; ++y ) {
		for ( uint32_t x = 0u; x < 4u; ++x ) {
			const uint8_t nValue = pPixels[y * nStride + x * nPixelStride + nComponent];
			uint32_t nBest = 0u;
			uint32_t nBestDistance = UINT32_MAX;
			for ( uint32_t iPalette = 0u; iPalette < 8u; ++iPalette ) {
				const int32_t nDelta = ( int32_t )nValue - Palette[iPalette];
				const uint32_t nDistance = ( uint32_t )( nDelta < 0 ? -nDelta : nDelta );
				if ( nDistance < nBestDistance ) {
					nBestDistance = nDistance;
					nBest = iPalette;
				}
			}
			nIndices |= ( uint64_t )nBest << ( ( y * 4u + x ) * 3u );
		}
	}
	for ( uint32_t iByte = 0u; iByte < 6u; ++iByte ) {
		pBlock[2u + iByte] = ( uint8_t )( nIndices >> ( iByte * 8u ) );
	}
}

typedef struct t3dVTReferenceBitWriter_s {
	uint8_t *m_pBytes;
	uint32_t m_nBit;
} t3dVTReferenceBitWriter_t;

static void t3d_vt_reference_write_bits( t3dVTReferenceBitWriter_t &rWriter, uint32_t nValue, const uint32_t nBitCount ) {
	for ( uint32_t iBit = 0u; iBit < nBitCount; ++iBit ) {
		if ( ( nValue & ( 1u << iBit ) ) != 0u ) {
			rWriter.m_pBytes[rWriter.m_nBit >> 3u] |= ( uint8_t )( 1u << ( rWriter.m_nBit & 7u ) );
		}
		++rWriter.m_nBit;
	}
}

static void t3d_vt_reference_encode_bc7_mode6_block( const uint8_t *pPixels, const uint32_t nStride, uint8_t *pBlock ) {
	uint32_t Sums[4] = { 0u, 0u, 0u, 0u };
	for ( uint32_t y = 0u; y < 4u; ++y ) {
		for ( uint32_t x = 0u; x < 4u; ++x ) {
			const uint8_t *pPixel = pPixels + y * nStride + x * 4u;
			for ( uint32_t c = 0u; c < 4u; ++c ) {
				Sums[c] += pPixel[c];
			}
		}
	}
	uint8_t Average[4];
	uint32_t nOddCount = 0u;
	for ( uint32_t c = 0u; c < 4u; ++c ) {
		Average[c] = ( uint8_t )( ( Sums[c] + 8u ) >> 4u );
		nOddCount += Average[c] & 1u;
	}
	const uint32_t nPBit = nOddCount >= 2u ? 1u : 0u;

	memset( pBlock, 0, 16u );
	t3dVTReferenceBitWriter_t Writer;
	Writer.m_pBytes = pBlock;
	Writer.m_nBit = 0u;
	t3d_vt_reference_write_bits( Writer, 0x40u, 7u );
	for ( uint32_t c = 0u; c < 4u; ++c ) {
		t3d_vt_reference_write_bits( Writer, Average[c] >> 1u, 7u );
		t3d_vt_reference_write_bits( Writer, Average[c] >> 1u, 7u );
	}
	t3d_vt_reference_write_bits( Writer, nPBit, 1u );
	t3d_vt_reference_write_bits( Writer, nPBit, 1u );
	t3d_vt_reference_write_bits( Writer, 0u, 3u );
	for ( uint32_t iTexel = 1u; iTexel < 16u; ++iTexel ) {
		t3d_vt_reference_write_bits( Writer, 0u, 4u );
	}
}

static bool t3d_vt_reference_encode_tile( void *pUser, const t3dVTEncodeTileRequest_t &rRequest, uint32_t &rWrittenBytes ) {
	( void )pUser;
	rWrittenBytes = 0u;
	const uint32_t nBlocksWide = ( rRequest.m_nWidth + 3u ) >> 2u;
	const uint32_t nBlocksHigh = ( rRequest.m_nHeight + 3u ) >> 2u;
	const uint32_t nSourceStride = rRequest.m_nWidth * rRequest.m_nComponents;
	uint8_t *pOut = rRequest.m_pDestination;

	const bool bV3 = rRequest.m_nContractVersion == T3D_VT_FILE_VERSION_V3;
	const uint32_t nHeightFormat = bV3 ? ( uint32_t )T3D_VT_PHYSICAL_FORMAT_R8_UNORM : ( uint32_t )T3D_VT_FORMAT_HEIGHT_R8_UNORM;
	if ( rRequest.m_nFormat == nHeightFormat ) {
		memcpy( pOut, rRequest.m_pSourcePixels, rRequest.m_nSourceBytes );
		rWrittenBytes = rRequest.m_nSourceBytes;
		return true;
	}

	for ( uint32_t yBlock = 0u; yBlock < nBlocksHigh; ++yBlock ) {
		for ( uint32_t xBlock = 0u; xBlock < nBlocksWide; ++xBlock ) {
			const uint8_t *pBlockPixels =
			        rRequest.m_pSourcePixels + ( ( uint64_t )yBlock * 4u * rRequest.m_nWidth + xBlock * 4u ) * rRequest.m_nComponents;
			const uint32_t nBc1Format = bV3 ? ( uint32_t )T3D_VT_PHYSICAL_FORMAT_BC1_UNORM : ( uint32_t )T3D_VT_FORMAT_ALBEDO_BC1_SRGB;
			const uint32_t nBc5Format = bV3 ? ( uint32_t )T3D_VT_PHYSICAL_FORMAT_BC5_UNORM : ( uint32_t )T3D_VT_FORMAT_NORMAL_BC5_UNORM;
			const uint32_t nBc7Format = bV3 ? ( uint32_t )T3D_VT_PHYSICAL_FORMAT_BC7_UNORM : ( uint32_t )T3D_VT_FORMAT_SURFACE_BC7_UNORM;
			if ( rRequest.m_nFormat == nBc1Format ) {
				t3d_vt_reference_encode_bc1_block( pBlockPixels, nSourceStride, pOut );
				pOut += 8u;
			} else if ( rRequest.m_nFormat == nBc5Format ) {
				t3d_vt_reference_encode_bc4_block( pBlockPixels, nSourceStride, 2u, 0u, pOut );
				t3d_vt_reference_encode_bc4_block( pBlockPixels, nSourceStride, 2u, 1u, pOut + 8u );
				pOut += 16u;
			} else if ( rRequest.m_nFormat == nBc7Format ) {
				t3d_vt_reference_encode_bc7_mode6_block( pBlockPixels, nSourceStride, pOut );
				pOut += 16u;
			} else {
				return false;
			}
		}
	}
	rWrittenBytes = ( uint32_t )( pOut - rRequest.m_pDestination );
	return true;
}

void t3d_vt_reference_encoder_make_interface( t3dVTEncoderInterface_t &rEncoder ) {
	rEncoder.m_pUser = NULL;
	rEncoder.m_pEncodeTile = t3d_vt_reference_encode_tile;
}
