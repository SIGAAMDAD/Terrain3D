// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
#include "instances/terrain_3d_instance_page_codec.h"

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

EA_FORCE_INLINE static void t3dInstancePageCodecWriteU16( uint8_t *pDst, const uint16_t nValue ) { pDst[0] = ( uint8_t ) ( nValue & 0xffu ); pDst[1] = ( uint8_t ) ( ( nValue >> 8u ) & 0xffu ); }

EA_FORCE_INLINE static void t3dInstancePageCodecWriteU32( uint8_t *pDst, const uint32_t nValue ) { pDst[0] = ( uint8_t ) ( nValue & 0xffu ); pDst[1] = ( uint8_t ) ( ( nValue >> 8u ) & 0xffu ); pDst[2] = ( uint8_t ) ( ( nValue >> 16u ) & 0xffu ); pDst[3] = ( uint8_t ) ( ( nValue >> 24u ) & 0xffu ); }

EA_FORCE_INLINE static uint16_t t3dInstancePageCodecReadU16( const uint8_t *pSrc ) { return ( uint16_t ) ( ( uint16_t ) ( pSrc[0] ) | ( ( uint16_t ) ( pSrc[1] ) << 8u ) ); }

EA_FORCE_INLINE static uint32_t t3dInstancePageCodecReadU32( const uint8_t *pSrc ) { return ( uint32_t ) ( pSrc[0] ) | ( ( uint32_t ) ( pSrc[1] ) << 8u ) | ( ( uint32_t ) ( pSrc[2] ) << 16u ) | ( ( uint32_t ) ( pSrc[3] ) << 24u ); }

EA_FORCE_INLINE static void t3dInstancePageCodecWriteF32( uint8_t *pDst, const float fValue ) { uint32_t bits = 0u; std::memcpy( &bits, &fValue, sizeof( bits ) ); t3dInstancePageCodecWriteU32( pDst, bits ); }

EA_FORCE_INLINE static float t3dInstancePageCodecReadF32( const uint8_t *pSrc ) { const uint32_t bits = t3dInstancePageCodecReadU32( pSrc ); float fValue = 0.0f; std::memcpy( &fValue, &bits, sizeof( fValue ) ); return fValue; }

EA_FORCE_INLINE static bool t3dInstancePageCodecPageHas( const size_t nOffset, const size_t nBytes, const size_t nSize ) { return nOffset <= nSize && nBytes <= nSize - nOffset; }

static void t3dInstancePageCodecClearMeshCounts( uint32_t *pMeshCounts ) {
	if ( pMeshCounts ) {
		std::memset( pMeshCounts, 0, sizeof( uint32_t ) * ( size_t ) ( TERRAIN3D_MAX_MESH_ASSETS ) );
	}
}

static bool t3dInstancePageCodecMeasurePage(
	const T3DInstanceStore &rStore, size_t &nBytes, uint32_t *pMeshCounts,
	uint32_t &nInstanceCount, uint16_t &nMeshCount
) {
	nBytes = TERRAIN3D_INSTANCE_PAGE_HEADER_BYTES;
	nInstanceCount = 0u;
	nMeshCount = 0u;
	t3dInstancePageCodecClearMeshCounts( pMeshCounts );
	const t3dInstanceLayout_t &layout = rStore.getLayout();
	if ( !layout.isValid() || rStore.meshCount() > ( uint16_t ) ( -1 ) ) {
		return false;
	}
	bool bSeenMeshes[TERRAIN3D_MAX_MESH_ASSETS] = {};
	for ( const t3dInstanceMesh_t &mesh : rStore.meshes() ) {
		if ( mesh.id < 0 || mesh.id >= TERRAIN3D_MAX_MESH_ASSETS ||
			mesh.cells.size() > ( size_t ) ( ( uint32_t ) ( -1 ) ) || bSeenMeshes[mesh.id] ) {
			return false;
		}
		bSeenMeshes[mesh.id] = true;
		size_t next_bytes = 0u;
		if ( !t3dInstancePageCodecSizeAdd( nBytes, TERRAIN3D_INSTANCE_PAGE_MESH_HEADER_BYTES, next_bytes ) ) {
			return false;
		}
		nBytes = next_bytes;
		uint32_t mesh_instances = 0u;
		Vector2i previousCell( -1, -1 );
		bool bHavePreviousCell = false;
		for ( const t3dInstanceCell_t &cell : mesh.cells ) {
			if ( !layout.contains( cell.location.x, cell.location.y ) || cell.transforms.size() > ( uint32_t ) ( -1 ) ) {
				return false;
			}
			if ( bHavePreviousCell &&
				( cell.location.x < previousCell.x ||
					( cell.location.x == previousCell.x && cell.location.y <= previousCell.y ) ) ) {
				return false;
			}
			previousCell = cell.location;
			bHavePreviousCell = true;
			const size_t count = cell.transforms.size();
			size_t transform_bytes = 0u;
			size_t sidecar_bytes = 0u;
			size_t cell_bytes = TERRAIN3D_INSTANCE_PAGE_CELL_HEADER_BYTES;
			if ( !t3dInstancePageCodecSizeMul( count, sizeof( t3dPackedTransform_t ), transform_bytes ) ||
				!t3dInstancePageCodecSizeAdd( cell_bytes, transform_bytes, cell_bytes ) ) {
				return false;
			}
			if ( cell.usesColors() ) {
				if ( cell.colors.size() != count || !t3dInstancePageCodecSizeMul( count, sizeof( uint32_t ), sidecar_bytes ) ||
					!t3dInstancePageCodecSizeAdd( cell_bytes, sidecar_bytes, cell_bytes ) ) {
					return false;
				}
			}
			if ( cell.usesOwners() ) {
				if ( cell.owners.size() != count || !t3dInstancePageCodecSizeMul( count, sizeof( uint32_t ), sidecar_bytes ) ||
					!t3dInstancePageCodecSizeAdd( cell_bytes, sidecar_bytes, cell_bytes ) ) {
					return false;
				}
			}
			if ( !t3dInstancePageCodecSizeAdd( nBytes, cell_bytes, next_bytes ) ||
				count > ( size_t ) ( ( uint32_t ) ( -1 ) - mesh_instances ) ) {
				return false;
			}
			nBytes = next_bytes;
			mesh_instances += ( uint32_t ) ( count );
		}
		if ( mesh_instances > ( uint32_t ) ( -1 ) - nInstanceCount ) {
			return false;
		}
		nInstanceCount += mesh_instances;
		if ( pMeshCounts ) {
			pMeshCounts[mesh.id] = mesh_instances;
		}
		++nMeshCount;
	}
	return nBytes <= ( size_t ) ( INT32_MAX );
}

static bool t3dInstancePageCodecReadPageHeaders(
	const PackedByteArray &arrPage, size_t &nOffset, uint32_t *pMeshCounts,
	uint32_t &nInstanceCount, uint16_t &nMeshCount, bool bDecode, T3DInstanceStore *pStore,
	size_t *const pColorCount = nullptr, size_t *const pOwnerCount = nullptr
) {
	t3dInstancePageCodecClearMeshCounts( pMeshCounts );
	nInstanceCount = 0u;
	nMeshCount = 0u;
	if ( pColorCount ) *pColorCount = 0u;
	if ( pOwnerCount ) *pOwnerCount = 0u;
	const size_t page_size = ( size_t ) ( arrPage.size() );
	if ( !t3dInstancePageCodecPageHas( 0u, TERRAIN3D_INSTANCE_PAGE_HEADER_BYTES, page_size ) ) {
		return arrPage.is_empty();
	}
	const uint8_t *pBytes = arrPage.ptr();
	if ( t3dInstancePageCodecReadU32( pBytes ) != TERRAIN3D_INSTANCE_PAGE_MAGIC ) {
		return false;
	}
	const uint16_t version = t3dInstancePageCodecReadU16( pBytes + 4u );
	if ( version != TERRAIN3D_INSTANCE_PAGE_VERSION && version != TERRAIN3D_INSTANCE_PAGE_LEGACY_VERSION ) {
		return false;
	}
	const uint16_t mesh_count = t3dInstancePageCodecReadU16( pBytes + 6u );
	const uint32_t declared_total = t3dInstancePageCodecReadU32( pBytes + 8u );
	t3dInstanceLayout_t layout;
	if ( version == TERRAIN3D_INSTANCE_PAGE_LEGACY_VERSION ) {
		if ( t3dInstancePageCodecReadU32( pBytes + 12u ) != 0u ) {
			return false;
		}
	} else {
		layout.nCellSize = t3dInstancePageCodecReadU16( pBytes + 12u );
		layout.cellsPerAxis = t3dInstancePageCodecReadU16( pBytes + 14u );
	}
	if ( mesh_count > TERRAIN3D_MAX_MESH_ASSETS || !layout.isValid() ) {
		return false;
	}
	nOffset = TERRAIN3D_INSTANCE_PAGE_HEADER_BYTES;
	if ( bDecode && pStore ) {
		pStore->clearRetainCapacity();
		pStore->setLayout( layout );
	}
	uint32_t decoded_total = 0u;
	bool bSeenMeshes[TERRAIN3D_MAX_MESH_ASSETS] = {};
	for ( uint16_t mesh_index = 0u; mesh_index < mesh_count; ++mesh_index ) {
		const size_t mesh_header_bytes = version == TERRAIN3D_INSTANCE_PAGE_LEGACY_VERSION ?
			TERRAIN3D_INSTANCE_PAGE_LEGACY_MESH_HEADER_BYTES : TERRAIN3D_INSTANCE_PAGE_MESH_HEADER_BYTES;
		if ( !t3dInstancePageCodecPageHas( nOffset, mesh_header_bytes, page_size ) ) {
			return false;
		}
		const int mesh_id = ( int ) ( t3dInstancePageCodecReadU16( pBytes + nOffset ) );
		uint32_t cell_count = 0u;
		uint32_t declared_mesh_total = 0u;
		if ( version == TERRAIN3D_INSTANCE_PAGE_LEGACY_VERSION ) {
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
		if ( mesh_id < 0 || mesh_id >= TERRAIN3D_MAX_MESH_ASSETS || bSeenMeshes[mesh_id] ||
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
			if ( !t3dInstancePageCodecPageHas( nOffset, TERRAIN3D_INSTANCE_PAGE_CELL_HEADER_BYTES, page_size ) ) {
				return false;
			}
			const int16_t cell_x = ( int16_t ) ( t3dInstancePageCodecReadU16( pBytes + nOffset ) );
			const int16_t cell_y = ( int16_t ) ( t3dInstancePageCodecReadU16( pBytes + nOffset + 2u ) );
			const uint16_t flags = t3dInstancePageCodecReadU16( pBytes + nOffset + 4u );
			const uint16_t reserved = t3dInstancePageCodecReadU16( pBytes + nOffset + 6u );
			const uint32_t count = t3dInstancePageCodecReadU32( pBytes + nOffset + 8u );
			nOffset += TERRAIN3D_INSTANCE_PAGE_CELL_HEADER_BYTES;
			const Vector2i cellLocation( cell_x, cell_y );
			if ( !layout.contains( cell_x, cell_y ) || reserved != 0u ||
				( flags & ~TERRAIN3D_INSTANCE_PAGE_CELL_FLAGS ) != 0u ||
				( bHavePreviousCell &&
					( cellLocation.x < previousCell.x ||
						( cellLocation.x == previousCell.x && cellLocation.y <= previousCell.y ) ) ) ) {
				return false;
			}
			previousCell = cellLocation;
			bHavePreviousCell = true;
			size_t transform_bytes = 0u;
			size_t sidecar_bytes = 0u;
			if ( !t3dInstancePageCodecSizeMul( ( size_t ) ( count ), sizeof( t3dPackedTransform_t ), transform_bytes ) ||
				!t3dInstancePageCodecPageHas( nOffset, transform_bytes, page_size ) ) {
				return false;
			}
			t3dInstanceCell_t *pDecodedCell = nullptr;
			if ( bDecode && pDecodedMesh && pStore ) {
				pDecodedCell = &pStore->getOrCreateCellExact( mesh_id, Vector2i( cell_x, cell_y ), count,
					( flags & TERRAIN3D_INSTANCE_PAGE_CELL_COLORS ) != 0u,
					( flags & TERRAIN3D_INSTANCE_PAGE_CELL_OWNERS ) != 0u );
				pDecodedCell->transforms.resize( count );
				t3dPackedTransform_t *pTransforms = pDecodedCell->transforms.data();
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dPackedTransform_t &transform = pTransforms[index];
					const uint8_t *pSource = pBytes + nOffset + ( size_t ) ( index ) * sizeof( t3dPackedTransform_t );
					transform.origin[0] = t3dInstancePageCodecReadF32( pSource );
					transform.origin[1] = t3dInstancePageCodecReadF32( pSource + 4u );
					transform.origin[2] = t3dInstancePageCodecReadF32( pSource + 8u );
					for ( int basis_index = 0; basis_index < 9; ++basis_index ) {
						transform.basis[basis_index] = t3dInstancePageCodecReadU16( pSource + 12u + ( size_t ) ( basis_index ) * 2u );
					}
					transform.reserved = t3dInstancePageCodecReadU16( pSource + 30u );
				}
			}
			nOffset += transform_bytes;
			if ( ( flags & TERRAIN3D_INSTANCE_PAGE_CELL_COLORS ) != 0u ) {
				if ( pColorCount ) { if ( ( size_t ) count > std::numeric_limits<size_t>::max() - *pColorCount ) return false; *pColorCount += ( size_t ) count; }
				if ( !t3dInstancePageCodecSizeMul( ( size_t ) ( count ), sizeof( uint32_t ), sidecar_bytes ) ||
					!t3dInstancePageCodecPageHas( nOffset, sidecar_bytes, page_size ) ) {
					return false;
				}
				if ( pDecodedCell ) {
					pDecodedCell->colors.resize( count );
					for ( uint32_t index = 0u; index < count; ++index ) {
						pDecodedCell->colors[index] = t3dInstancePageCodecReadU32( pBytes + nOffset + ( size_t ) ( index ) * 4u );
					}
				}
				nOffset += sidecar_bytes;
			}
			if ( ( flags & TERRAIN3D_INSTANCE_PAGE_CELL_OWNERS ) != 0u ) {
				if ( pOwnerCount ) { if ( ( size_t ) count > std::numeric_limits<size_t>::max() - *pOwnerCount ) return false; *pOwnerCount += ( size_t ) count; }
				if ( !t3dInstancePageCodecSizeMul( ( size_t ) ( count ), sizeof( uint32_t ), sidecar_bytes ) ||
					!t3dInstancePageCodecPageHas( nOffset, sidecar_bytes, page_size ) ) {
					return false;
				}
				if ( pDecodedCell ) {
					pDecodedCell->owners.resize( count );
					for ( uint32_t index = 0u; index < count; ++index ) {
						pDecodedCell->owners[index] = t3dInstancePageCodecReadU32( pBytes + nOffset + ( size_t ) ( index ) * 4u );
					}
				}
				nOffset += sidecar_bytes;
			}
			if ( count > ( uint32_t ) ( -1 ) - decoded_mesh_total ) {
				return false;
			}
			decoded_mesh_total += count;
			if ( pDecodedCell ) {
				pDecodedCell->bModified = true;
			}
		}
		if ( decoded_mesh_total != declared_mesh_total || decoded_mesh_total > ( uint32_t ) ( -1 ) - decoded_total ) {
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

bool T3DInstancePageCodec::encode(
	const T3DInstanceStore &rStore, PackedByteArray &rPage,
	uint32_t *const pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount
) {
	size_t page_bytes = 0u;
	if ( !t3dInstancePageCodecMeasurePage( rStore, page_bytes, pMeshCounts, nInstanceCount, nMeshCount ) ) {
		return false;
	}
	rPage.resize( ( int ) ( page_bytes ) );
	uint8_t *const pBytes = rPage.ptrw();
	t3dInstancePageCodecWriteU32( pBytes, TERRAIN3D_INSTANCE_PAGE_MAGIC );
	t3dInstancePageCodecWriteU16( pBytes + 4u, TERRAIN3D_INSTANCE_PAGE_VERSION );
	t3dInstancePageCodecWriteU16( pBytes + 6u, nMeshCount );
	t3dInstancePageCodecWriteU32( pBytes + 8u, nInstanceCount );
	const t3dInstanceLayout_t &layout = rStore.getLayout();
	t3dInstancePageCodecWriteU16( pBytes + 12u, layout.nCellSize );
	t3dInstancePageCodecWriteU16( pBytes + 14u, layout.cellsPerAxis );
	size_t offset = TERRAIN3D_INSTANCE_PAGE_HEADER_BYTES;
	for ( const t3dInstanceMesh_t &mesh : rStore.meshes() ) {
		uint32_t mesh_total = 0u;
		for ( const t3dInstanceCell_t &cell : mesh.cells ) {
			mesh_total += ( uint32_t ) ( cell.transforms.size() );
		}
		t3dInstancePageCodecWriteU16( pBytes + offset, ( uint16_t ) ( mesh.id ) );
		t3dInstancePageCodecWriteU16( pBytes + offset + 2u, 0u );
		t3dInstancePageCodecWriteU32( pBytes + offset + 4u, ( uint32_t ) ( mesh.cells.size() ) );
		t3dInstancePageCodecWriteU32( pBytes + offset + 8u, mesh_total );
		offset += TERRAIN3D_INSTANCE_PAGE_MESH_HEADER_BYTES;
		for ( const t3dInstanceCell_t &cell : mesh.cells ) {
			uint16_t flags = 0u;
			if ( cell.usesColors() ) {
				flags |= TERRAIN3D_INSTANCE_PAGE_CELL_COLORS;
			}
			if ( cell.usesOwners() ) {
				flags |= TERRAIN3D_INSTANCE_PAGE_CELL_OWNERS;
			}
			const uint32_t count = ( uint32_t ) ( cell.transforms.size() );
			t3dInstancePageCodecWriteU16( pBytes + offset, ( uint16_t ) ( ( int16_t ) ( cell.location.x ) ) );
			t3dInstancePageCodecWriteU16( pBytes + offset + 2u, ( uint16_t ) ( ( int16_t ) ( cell.location.y ) ) );
			t3dInstancePageCodecWriteU16( pBytes + offset + 4u, flags );
			t3dInstancePageCodecWriteU16( pBytes + offset + 6u, 0u );
			t3dInstancePageCodecWriteU32( pBytes + offset + 8u, count );
			offset += TERRAIN3D_INSTANCE_PAGE_CELL_HEADER_BYTES;
			const t3dPackedTransform_t *pTransforms = cell.transforms.data();
			for ( uint32_t index = 0u; index < count; ++index ) {
				const t3dPackedTransform_t &transform = pTransforms[index];
				t3dInstancePageCodecWriteF32( pBytes + offset, transform.origin[0] );
				t3dInstancePageCodecWriteF32( pBytes + offset + 4u, transform.origin[1] );
				t3dInstancePageCodecWriteF32( pBytes + offset + 8u, transform.origin[2] );
				for ( int basis_index = 0; basis_index < 9; ++basis_index ) {
					t3dInstancePageCodecWriteU16( pBytes + offset + 12u + ( size_t ) ( basis_index ) * 2u,
						transform.basis[basis_index] );
				}
				t3dInstancePageCodecWriteU16( pBytes + offset + 30u, transform.reserved );
				offset += sizeof( t3dPackedTransform_t );
			}
			if ( cell.usesColors() ) {
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dInstancePageCodecWriteU32( pBytes + offset, cell.colors[index] );
					offset += sizeof( uint32_t );
				}
			}
			if ( cell.usesOwners() ) {
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dInstancePageCodecWriteU32( pBytes + offset, cell.owners[index] );
					offset += sizeof( uint32_t );
				}
			}
		}
	}
	return offset == page_bytes;
}

bool T3DInstancePageCodec::decode(
	const PackedByteArray &arrPage, T3DInstanceStore &rStore,
	uint32_t *const pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount
) {
	if ( arrPage.is_empty() ) { rStore.clearRetainCapacity(); t3dInstancePageCodecClearMeshCounts( pMeshCounts ); nInstanceCount = 0u; nMeshCount = 0u; return true; }
	uint32_t inspected_instances = 0u; uint16_t inspected_meshes = 0u; size_t inspect_offset = 0u; size_t color_count = 0u; size_t owner_count = 0u;
	if ( !t3dInstancePageCodecReadPageHeaders( arrPage, inspect_offset, nullptr, inspected_instances, inspected_meshes, false, nullptr, &color_count, &owner_count ) ) { t3dInstancePageCodecClearMeshCounts( pMeshCounts ); nInstanceCount = 0u; nMeshCount = 0u; return false; }
	rStore.reserveMeshCapacity( ( size_t ) inspected_meshes );
	rStore.reservePayloadCapacity( ( size_t ) inspected_instances, color_count, owner_count );
	size_t decode_offset = 0u; if ( !t3dInstancePageCodecReadPageHeaders( arrPage, decode_offset, pMeshCounts, nInstanceCount, nMeshCount, true, &rStore ) ) { t3dInstancePageCodecClearMeshCounts( pMeshCounts ); nInstanceCount = 0u; nMeshCount = 0u; return false; }
	return nInstanceCount == inspected_instances && nMeshCount == inspected_meshes;
}

bool T3DInstancePageCodec::inspect(
	const PackedByteArray &arrPage, uint32_t *const pMeshCounts,
	uint32_t &nInstanceCount, uint16_t &nMeshCount
) {
	if ( arrPage.is_empty() ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return true;
	}
	size_t offset = 0u;
	if ( !t3dInstancePageCodecReadPageHeaders( arrPage, offset, pMeshCounts, nInstanceCount, nMeshCount, false, nullptr ) ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return false;
	}
	return true;
}
