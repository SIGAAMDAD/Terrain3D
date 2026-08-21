// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/terrain_3d_instance_page_codec.h"

#include "core/terrain_3d_compiler.h"

#include <cstring>
#include <limits>

static bool t3dInstancePageCodecSizeAdd( const size_t nA, const size_t nB, size_t &nValue ) {
	if ( nB > std::numeric_limits<size_t>::max() - nA ) {
		return false;
	}
	nValue = nA + nB;
	return true;
}

static bool t3dInstancePageCodecSizeMul( const size_t nA, const size_t nB, size_t &nValue ) {
	if ( nA != 0u && nB > std::numeric_limits<size_t>::max() / nA ) {
		return false;
	}
	nValue = nA * nB;
	return true;
}

EA_FORCE_INLINE static void t3dInstancePageCodecWriteU16( uint8_t *pDst, const uint16_t nValue ) {
	pDst[0] = ( uint8_t )( nValue & 0xffu );
	pDst[1] = ( uint8_t )( ( nValue >> 8u ) & 0xffu );
}

EA_FORCE_INLINE static void t3dInstancePageCodecWriteU32( uint8_t *pDst, const uint32_t nValue ) {
	pDst[0] = ( uint8_t )( nValue & 0xffu );
	pDst[1] = ( uint8_t )( ( nValue >> 8u ) & 0xffu );
	pDst[2] = ( uint8_t )( ( nValue >> 16u ) & 0xffu );
	pDst[3] = ( uint8_t )( ( nValue >> 24u ) & 0xffu );
}

EA_FORCE_INLINE static uint16_t t3dInstancePageCodecReadU16( const uint8_t *pSrc ) {
	return ( uint16_t )( ( uint16_t )( pSrc[0] ) | ( ( uint16_t )( pSrc[1] ) << 8u ) );
}

EA_FORCE_INLINE static uint32_t t3dInstancePageCodecReadU32( const uint8_t *pSrc ) {
	return ( uint32_t )( pSrc[0] ) | ( ( uint32_t )( pSrc[1] ) << 8u ) | ( ( uint32_t )( pSrc[2] ) << 16u ) |
			( ( uint32_t )( pSrc[3] ) << 24u );
}

EA_FORCE_INLINE static void t3dInstancePageCodecWriteF32( uint8_t *pDst, const float fValue ) {
	uint32_t bits = 0u;
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	t3dInstancePageCodecWriteU32( pDst, bits );
}

EA_FORCE_INLINE static float t3dInstancePageCodecReadF32( const uint8_t *pSrc ) {
	const uint32_t bits = t3dInstancePageCodecReadU32( pSrc );
	float fValue = 0.0f;
	std::memcpy( &fValue, &bits, sizeof( fValue ) );
	return fValue;
}

EA_FORCE_INLINE static bool
t3dInstancePageCodecPageHas( const size_t nOffset, const size_t nBytes, const size_t nSize ) {
	return nOffset <= nSize && nBytes <= nSize - nOffset;
}

static void t3dInstancePageCodecClearMeshCounts( uint32_t *pMeshCounts ) {
	if ( pMeshCounts ) {
		std::memset( pMeshCounts, 0, sizeof( uint32_t ) * ( size_t )( s_nMaxMeshAssets ) );
	}
}

static bool t3dInstancePageCodecMeasurePage( const T3DInstanceStore &rStore,
		size_t &nBytes,
		uint32_t *pMeshCounts,
		uint32_t &nInstanceCount,
		uint16_t &nMeshCount ) {
	nBytes = s_nInstancePageHeaderBytes;
	nInstanceCount = 0u;
	nMeshCount = 0u;
	t3dInstancePageCodecClearMeshCounts( pMeshCounts );
	const t3dInstanceLayout_t &layout = rStore.getLayout();
	if ( !layout.isValid() || rStore.meshCount() > ( uint16_t )( -1 ) ) {
		return false;
	}
	bool bSeenMeshes[s_nMaxMeshAssets] = {};
	for ( const t3dInstanceMesh_t &mesh : rStore.meshes() ) {
		if ( mesh.m_nId < 0 || mesh.m_nId >= s_nMaxMeshAssets ||
				mesh.m_cells.size() > ( size_t )( ( uint32_t )( -1 ) ) || bSeenMeshes[mesh.m_nId] ) {
			return false;
		}
		bSeenMeshes[mesh.m_nId] = true;
		size_t next_bytes = 0u;
		if ( !t3dInstancePageCodecSizeAdd( nBytes, s_nInstancePageMeshHeaderBytes, next_bytes ) ) {
			return false;
		}
		nBytes = next_bytes;
		uint32_t mesh_instances = 0u;
		Vector2i previousCell( -1, -1 );
		bool bHavePreviousCell = false;
		for ( const t3dInstanceCell_t &cell : mesh.m_cells ) {
			if ( !layout.contains( cell.m_location.x, cell.m_location.y ) ||
					cell.m_transforms.size() > ( uint32_t )( -1 ) ) {
				return false;
			}
			if ( bHavePreviousCell &&
					( cell.m_location.x < previousCell.x ||
							( cell.m_location.x == previousCell.x && cell.m_location.y <= previousCell.y ) ) ) {
				return false;
			}
			previousCell = cell.m_location;
			bHavePreviousCell = true;
			const size_t count = cell.m_transforms.size();
			size_t transform_bytes = 0u;
			size_t sidecar_bytes = 0u;
			size_t cell_bytes = s_nInstancePageCellHeaderBytes;
			if ( !t3dInstancePageCodecSizeMul( count, sizeof( t3dPackedTransform_t ), transform_bytes ) ||
					!t3dInstancePageCodecSizeAdd( cell_bytes, transform_bytes, cell_bytes ) ) {
				return false;
			}
			if ( cell.usesColors() ) {
				if ( cell.m_colors.size() != count ||
						!t3dInstancePageCodecSizeMul( count, sizeof( uint32_t ), sidecar_bytes ) ||
						!t3dInstancePageCodecSizeAdd( cell_bytes, sidecar_bytes, cell_bytes ) ) {
					return false;
				}
			}
			if ( cell.usesOwners() ) {
				if ( cell.m_owners.size() != count ||
						!t3dInstancePageCodecSizeMul( count, sizeof( uint32_t ), sidecar_bytes ) ||
						!t3dInstancePageCodecSizeAdd( cell_bytes, sidecar_bytes, cell_bytes ) ) {
					return false;
				}
			}
			if ( !t3dInstancePageCodecSizeAdd( nBytes, cell_bytes, next_bytes ) ||
					count > ( size_t )( ( uint32_t )( -1 ) - mesh_instances ) ) {
				return false;
			}
			nBytes = next_bytes;
			mesh_instances += ( uint32_t )( count );
		}
		if ( mesh_instances > ( uint32_t )( -1 ) - nInstanceCount ) {
			return false;
		}
		nInstanceCount += mesh_instances;
		if ( pMeshCounts ) {
			pMeshCounts[mesh.m_nId] = mesh_instances;
		}
		++nMeshCount;
	}
	return nBytes <= ( size_t )( INT32_MAX );
}

static bool t3dInstancePageCodecReadPageHeaders( const PackedByteArray &arrPage,
		size_t &nOffset,
		uint32_t *pMeshCounts,
		uint32_t &nInstanceCount,
		uint16_t &nMeshCount,
		bool bDecode,
		T3DInstanceStore *pStore,
		size_t *const pColorCount = nullptr,
		size_t *const pOwnerCount = nullptr ) {
	t3dInstancePageCodecClearMeshCounts( pMeshCounts );
	nInstanceCount = 0u;
	nMeshCount = 0u;
	if ( pColorCount ) {
		*pColorCount = 0u;
	}
	if ( pOwnerCount ) {
		*pOwnerCount = 0u;
	}
	const size_t page_size = ( size_t )( arrPage.size() );
	if ( !t3dInstancePageCodecPageHas( 0u, s_nInstancePageHeaderBytes, page_size ) ) {
		return arrPage.is_empty();
	}
	const uint8_t *pBytes = arrPage.ptr();
	if ( t3dInstancePageCodecReadU32( pBytes ) != s_nInstancePageMagic ) {
		return false;
	}
	const uint16_t version = t3dInstancePageCodecReadU16( pBytes + 4u );
	if ( version != s_nInstancePageVersion && version != s_nInstancePageLegacyVersion ) {
		return false;
	}
	const uint16_t mesh_count = t3dInstancePageCodecReadU16( pBytes + 6u );
	const uint32_t declared_total = t3dInstancePageCodecReadU32( pBytes + 8u );
	t3dInstanceLayout_t layout;
	if ( version == s_nInstancePageLegacyVersion ) {
		if ( t3dInstancePageCodecReadU32( pBytes + 12u ) != 0u ) {
			return false;
		}
	} else {
		layout.m_nCellSize = t3dInstancePageCodecReadU16( pBytes + 12u );
		layout.m_nCellsPerAxis = t3dInstancePageCodecReadU16( pBytes + 14u );
	}
	if ( mesh_count > s_nMaxMeshAssets || !layout.isValid() ) {
		return false;
	}
	nOffset = s_nInstancePageHeaderBytes;
	if ( bDecode && pStore ) {
		pStore->clearRetainCapacity();
		pStore->setLayout( layout );
	}
	uint32_t decoded_total = 0u;
	bool bSeenMeshes[s_nMaxMeshAssets] = {};
	for ( uint16_t mesh_index = 0u; mesh_index < mesh_count; ++mesh_index ) {
		const size_t mesh_header_bytes = version == s_nInstancePageLegacyVersion ? s_nInstancePageLegacyMeshHeaderBytes
																				 : s_nInstancePageMeshHeaderBytes;
		if ( !t3dInstancePageCodecPageHas( nOffset, mesh_header_bytes, page_size ) ) {
			return false;
		}
		const int mesh_id = ( int )( t3dInstancePageCodecReadU16( pBytes + nOffset ) );
		uint32_t cell_count = 0u;
		uint32_t declared_mesh_total = 0u;
		if ( version == s_nInstancePageLegacyVersion ) {
			cell_count = t3dInstancePageCodecReadU16( pBytes + nOffset + 2u );
			declared_mesh_total = t3dInstancePageCodecReadU32( pBytes + nOffset + 4u );
		} else {
			if ( t3dInstancePageCodecReadU16( pBytes + nOffset + 2u ) != 0u ) {
				return false;
			}
			cell_count = t3dInstancePageCodecReadU32( pBytes + nOffset + 4u );
			declared_mesh_total = t3dInstancePageCodecReadU32( pBytes + nOffset + 8u );
		}
		nOffset += mesh_header_bytes;
		if ( mesh_id < 0 || mesh_id >= s_nMaxMeshAssets || bSeenMeshes[mesh_id] ||
				cell_count > layout.maximumCellCount() ) {
			return false;
		}
		bSeenMeshes[mesh_id] = true;
		Vector2i previousCell( -1, -1 );
		bool bHavePreviousCell = false;
		uint32_t decoded_mesh_total = 0u;
		t3dInstanceMesh_t *pDecodedMesh = nullptr;
		if ( bDecode && pStore ) {
			pDecodedMesh = &pStore->getOrCreateMesh( mesh_id, cell_count );
		}
		for ( uint32_t cell_index = 0u; cell_index < cell_count; ++cell_index ) {
			if ( !t3dInstancePageCodecPageHas( nOffset, s_nInstancePageCellHeaderBytes, page_size ) ) {
				return false;
			}
			const int16_t cell_x = ( int16_t )( t3dInstancePageCodecReadU16( pBytes + nOffset ) );
			const int16_t cell_y = ( int16_t )( t3dInstancePageCodecReadU16( pBytes + nOffset + 2u ) );
			const uint16_t flags = t3dInstancePageCodecReadU16( pBytes + nOffset + 4u );
			const uint16_t reserved = t3dInstancePageCodecReadU16( pBytes + nOffset + 6u );
			const uint32_t count = t3dInstancePageCodecReadU32( pBytes + nOffset + 8u );
			nOffset += s_nInstancePageCellHeaderBytes;
			const Vector2i cellLocation( cell_x, cell_y );
			if ( !layout.contains( cell_x, cell_y ) || reserved != 0u || ( flags & ~s_nInstancePageCellFlags ) != 0u ||
					( bHavePreviousCell &&
							( cellLocation.x < previousCell.x ||
									( cellLocation.x == previousCell.x && cellLocation.y <= previousCell.y ) ) ) ) {
				return false;
			}
			previousCell = cellLocation;
			bHavePreviousCell = true;
			size_t transform_bytes = 0u;
			size_t sidecar_bytes = 0u;
			if ( !t3dInstancePageCodecSizeMul( ( size_t )( count ), sizeof( t3dPackedTransform_t ), transform_bytes ) ||
					!t3dInstancePageCodecPageHas( nOffset, transform_bytes, page_size ) ) {
				return false;
			}
			t3dInstanceCell_t *pDecodedCell = nullptr;
			if ( bDecode && pDecodedMesh && pStore ) {
				pDecodedCell = &pStore->getOrCreateCellExact( mesh_id,
						Vector2i( cell_x, cell_y ),
						count,
						( flags & s_nInstancePageCellColors ) != 0u,
						( flags & s_nInstancePageCellOwners ) != 0u );
				pDecodedCell->m_transforms.resize( count );
				t3dPackedTransform_t *pTransforms = pDecodedCell->m_transforms.data();
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dPackedTransform_t &transform = pTransforms[index];
					const uint8_t *pSource = pBytes + nOffset + ( size_t )( index ) * sizeof( t3dPackedTransform_t );
					transform.m_szOrigin[0] = t3dInstancePageCodecReadF32( pSource );
					transform.m_szOrigin[1] = t3dInstancePageCodecReadF32( pSource + 4u );
					transform.m_szOrigin[2] = t3dInstancePageCodecReadF32( pSource + 8u );
					for ( int basis_index = 0; basis_index < 9; ++basis_index ) {
						transform.m_szBasis[basis_index] =
								t3dInstancePageCodecReadU16( pSource + 12u + ( size_t )( basis_index ) * 2u );
					}
					transform.m_nReserved = t3dInstancePageCodecReadU16( pSource + 30u );
				}
			}
			nOffset += transform_bytes;
			if ( ( flags & s_nInstancePageCellColors ) != 0u ) {
				if ( pColorCount ) {
					if ( ( size_t )count > std::numeric_limits<size_t>::max() - *pColorCount ) {
						return false;
					}
					*pColorCount += ( size_t )count;
				}
				if ( !t3dInstancePageCodecSizeMul( ( size_t )( count ), sizeof( uint32_t ), sidecar_bytes ) ||
						!t3dInstancePageCodecPageHas( nOffset, sidecar_bytes, page_size ) ) {
					return false;
				}
				if ( pDecodedCell ) {
					pDecodedCell->m_colors.resize( count );
					for ( uint32_t index = 0u; index < count; ++index ) {
						pDecodedCell->m_colors[index] =
								t3dInstancePageCodecReadU32( pBytes + nOffset + ( size_t )( index ) * 4u );
					}
				}
				nOffset += sidecar_bytes;
			}
			if ( ( flags & s_nInstancePageCellOwners ) != 0u ) {
				if ( pOwnerCount ) {
					if ( ( size_t )count > std::numeric_limits<size_t>::max() - *pOwnerCount ) {
						return false;
					}
					*pOwnerCount += ( size_t )count;
				}
				if ( !t3dInstancePageCodecSizeMul( ( size_t )( count ), sizeof( uint32_t ), sidecar_bytes ) ||
						!t3dInstancePageCodecPageHas( nOffset, sidecar_bytes, page_size ) ) {
					return false;
				}
				if ( pDecodedCell ) {
					pDecodedCell->m_owners.resize( count );
					for ( uint32_t index = 0u; index < count; ++index ) {
						pDecodedCell->m_owners[index] =
								t3dInstancePageCodecReadU32( pBytes + nOffset + ( size_t )( index ) * 4u );
					}
				}
				nOffset += sidecar_bytes;
			}
			if ( count > ( uint32_t )( -1 ) - decoded_mesh_total ) {
				return false;
			}
			decoded_mesh_total += count;
			if ( pDecodedCell ) {
				pDecodedCell->m_bModified = true;
			}
		}
		if ( decoded_mesh_total != declared_mesh_total || decoded_mesh_total > ( uint32_t )( -1 ) - decoded_total ) {
			return false;
		}
		decoded_total += decoded_mesh_total;
		if ( pMeshCounts ) {
			pMeshCounts[mesh_id] = decoded_mesh_total;
		}
	}
	if ( nOffset != page_size || decoded_total != declared_total ) {
		return false;
	}
	nInstanceCount = decoded_total;
	nMeshCount = mesh_count;
	return true;
}

bool T3DInstancePageCodec::encode( const T3DInstanceStore &rStore,
		PackedByteArray &rPage,
		uint32_t *const pMeshCounts,
		uint32_t &nInstanceCount,
		uint16_t &nMeshCount ) {
	size_t page_bytes = 0u;
	if ( !t3dInstancePageCodecMeasurePage( rStore, page_bytes, pMeshCounts, nInstanceCount, nMeshCount ) ) {
		return false;
	}
	rPage.resize( ( int )( page_bytes ) );
	uint8_t *const pBytes = rPage.ptrw();
	t3dInstancePageCodecWriteU32( pBytes, s_nInstancePageMagic );
	t3dInstancePageCodecWriteU16( pBytes + 4u, s_nInstancePageVersion );
	t3dInstancePageCodecWriteU16( pBytes + 6u, nMeshCount );
	t3dInstancePageCodecWriteU32( pBytes + 8u, nInstanceCount );
	const t3dInstanceLayout_t &layout = rStore.getLayout();
	t3dInstancePageCodecWriteU16( pBytes + 12u, layout.m_nCellSize );
	t3dInstancePageCodecWriteU16( pBytes + 14u, layout.m_nCellsPerAxis );
	size_t offset = s_nInstancePageHeaderBytes;
	for ( const t3dInstanceMesh_t &mesh : rStore.meshes() ) {
		uint32_t mesh_total = 0u;
		for ( const t3dInstanceCell_t &cell : mesh.m_cells ) {
			mesh_total += ( uint32_t )( cell.m_transforms.size() );
		}
		t3dInstancePageCodecWriteU16( pBytes + offset, ( uint16_t )( mesh.m_nId ) );
		t3dInstancePageCodecWriteU16( pBytes + offset + 2u, 0u );
		t3dInstancePageCodecWriteU32( pBytes + offset + 4u, ( uint32_t )( mesh.m_cells.size() ) );
		t3dInstancePageCodecWriteU32( pBytes + offset + 8u, mesh_total );
		offset += s_nInstancePageMeshHeaderBytes;
		for ( const t3dInstanceCell_t &cell : mesh.m_cells ) {
			uint16_t flags = 0u;
			if ( cell.usesColors() ) {
				flags |= s_nInstancePageCellColors;
			}
			if ( cell.usesOwners() ) {
				flags |= s_nInstancePageCellOwners;
			}
			const uint32_t count = ( uint32_t )( cell.m_transforms.size() );
			t3dInstancePageCodecWriteU16( pBytes + offset, ( uint16_t )( ( int16_t )( cell.m_location.x ) ) );
			t3dInstancePageCodecWriteU16( pBytes + offset + 2u, ( uint16_t )( ( int16_t )( cell.m_location.y ) ) );
			t3dInstancePageCodecWriteU16( pBytes + offset + 4u, flags );
			t3dInstancePageCodecWriteU16( pBytes + offset + 6u, 0u );
			t3dInstancePageCodecWriteU32( pBytes + offset + 8u, count );
			offset += s_nInstancePageCellHeaderBytes;
			const t3dPackedTransform_t *pTransforms = cell.m_transforms.data();
			for ( uint32_t index = 0u; index < count; ++index ) {
				const t3dPackedTransform_t &transform = pTransforms[index];
				t3dInstancePageCodecWriteF32( pBytes + offset, transform.m_szOrigin[0] );
				t3dInstancePageCodecWriteF32( pBytes + offset + 4u, transform.m_szOrigin[1] );
				t3dInstancePageCodecWriteF32( pBytes + offset + 8u, transform.m_szOrigin[2] );
				for ( int basis_index = 0; basis_index < 9; ++basis_index ) {
					t3dInstancePageCodecWriteU16(
							pBytes + offset + 12u + ( size_t )( basis_index ) * 2u, transform.m_szBasis[basis_index] );
				}
				t3dInstancePageCodecWriteU16( pBytes + offset + 30u, transform.m_nReserved );
				offset += sizeof( t3dPackedTransform_t );
			}
			if ( cell.usesColors() ) {
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dInstancePageCodecWriteU32( pBytes + offset, cell.m_colors[index] );
					offset += sizeof( uint32_t );
				}
			}
			if ( cell.usesOwners() ) {
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dInstancePageCodecWriteU32( pBytes + offset, cell.m_owners[index] );
					offset += sizeof( uint32_t );
				}
			}
		}
	}
	return offset == page_bytes;
}

bool T3DInstancePageCodec::decode( const PackedByteArray &arrPage,
		T3DInstanceStore &rStore,
		uint32_t *const pMeshCounts,
		uint32_t &nInstanceCount,
		uint16_t &nMeshCount ) {
	if ( arrPage.is_empty() ) {
		rStore.clearRetainCapacity();
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return true;
	}
	uint32_t inspected_instances = 0u;
	uint16_t inspected_meshes = 0u;
	size_t inspect_offset = 0u;
	size_t color_count = 0u;
	size_t owner_count = 0u;
	if ( !t3dInstancePageCodecReadPageHeaders( arrPage,
				 inspect_offset,
				 nullptr,
				 inspected_instances,
				 inspected_meshes,
				 false,
				 nullptr,
				 &color_count,
				 &owner_count ) ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return false;
	}
	rStore.reserveMeshCapacity( ( size_t )inspected_meshes );
	rStore.reservePayloadCapacity( ( size_t )inspected_instances, color_count, owner_count );
	size_t decode_offset = 0u;
	if ( !t3dInstancePageCodecReadPageHeaders(
				 arrPage, decode_offset, pMeshCounts, nInstanceCount, nMeshCount, true, &rStore ) ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return false;
	}
	return nInstanceCount == inspected_instances && nMeshCount == inspected_meshes;
}

bool T3DInstancePageCodec::inspect( const PackedByteArray &arrPage,
		uint32_t *const pMeshCounts,
		uint32_t &nInstanceCount,
		uint16_t &nMeshCount ) {
	if ( arrPage.is_empty() ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return true;
	}
	size_t offset = 0u;
	if ( !t3dInstancePageCodecReadPageHeaders(
				 arrPage, offset, pMeshCounts, nInstanceCount, nMeshCount, false, nullptr ) ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return false;
	}
	return true;
}
