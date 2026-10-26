// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "instances/storage/terrain_3d_instance_page_codec.h"

#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_checked_math.h"

#include <EASTL/numeric_limits.h>
#include <string.h>

static bool t3dInstancePageCodecSizeAdd( const size_t nA, const size_t nB, size_t &nValue ) {
	return t3dCheckedAdd( nA, nB, nValue );
}

static bool t3dInstancePageCodecSizeMul( const size_t nA, const size_t nB, size_t &nValue ) {
	return t3dCheckedMultiply( nA, nB, nValue );
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
	return ( uint32_t )( pSrc[0] ) | ( ( uint32_t )( pSrc[1] ) << 8u ) | ( ( uint32_t )( pSrc[2] ) << 16u )
	        | ( ( uint32_t )( pSrc[3] ) << 24u );
}

EA_FORCE_INLINE static void t3dInstancePageCodecWriteF32( uint8_t *pDst, const float fValue ) {
	uint32_t bits = 0u;
	memcpy( &bits, &fValue, sizeof( bits ) );
	t3dInstancePageCodecWriteU32( pDst, bits );
}

EA_FORCE_INLINE static float t3dInstancePageCodecReadF32( const uint8_t *pSrc ) {
	const uint32_t bits = t3dInstancePageCodecReadU32( pSrc );
	float fValue = 0.0f;
	memcpy( &fValue, &bits, sizeof( fValue ) );
	return fValue;
}

EA_FORCE_INLINE static bool t3dInstancePageCodecPageHas( const size_t nOffset, const size_t nBytes, const size_t nSize ) {
	return nOffset <= nSize && nBytes <= nSize - nOffset;
}

static void t3dInstancePageCodecClearMeshCounts( uint32_t *pMeshCounts ) {
	if ( pMeshCounts ) {
		memset( pMeshCounts, 0, sizeof( uint32_t ) * ( size_t )( T3D_MAX_MESH_ASSETS ) );
	}
}

#if defined( TERRAIN3D_INSTANCE_PAGE_CODEC_TEST_ACCESS )
static int s_nInstancePageCodecDecodeFailureCountdown = -1;

static bool t3dInstancePageCodecInjectDecodeFailure( void ) {
	if ( s_nInstancePageCodecDecodeFailureCountdown < 0 ) {
		return false;
	}
	if ( s_nInstancePageCodecDecodeFailureCountdown == 0 ) {
		s_nInstancePageCodecDecodeFailureCountdown = -1;
		return true;
	}
	--s_nInstancePageCodecDecodeFailureCountdown;
	return false;
}

void Terrain3DInstancePageCodec::testSetDecodeFailureCountdown( const int nCountdown ) {
	s_nInstancePageCodecDecodeFailureCountdown = nCountdown;
}

void Terrain3DInstancePageCodec::testClearDecodeFailure( void ) {
	s_nInstancePageCodecDecodeFailureCountdown = -1;
}
#else
EA_FORCE_INLINE static bool t3dInstancePageCodecInjectDecodeFailure( void ) {
	return false;
}
#endif

static bool t3dInstancePageCodecMeasurePage(
        const Terrain3DInstanceStore &rStore, size_t &nBytes, uint32_t *pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount ) {
	nBytes = T3D_INSTANCE_PAGE_HEADER_BYTES;
	nInstanceCount = 0u;
	nMeshCount = 0u;
	t3dInstancePageCodecClearMeshCounts( pMeshCounts );
	const t3dInstanceLayout_t &layout = rStore.getLayout();
	if ( !layout.isValid() || rStore.meshCount() > ( uint16_t )( -1 ) ) {
		return false;
	}
	bool bSeenMeshes[T3D_MAX_MESH_ASSETS] = {};
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( rStore.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( rStore.meshes() )[iMesh];
		if ( mesh.m_nId < 0 || mesh.m_nId >= T3D_MAX_MESH_ASSETS || mesh.m_cells.size() > ( size_t )( ( uint32_t )( -1 ) )
		        || bSeenMeshes[mesh.m_nId] ) {
			return false;
		}
		bSeenMeshes[mesh.m_nId] = true;
		size_t next_bytes = 0u;
		if ( !t3dInstancePageCodecSizeAdd( nBytes, T3D_INSTANCE_PAGE_MESH_HEADER_BYTES, next_bytes ) ) {
			return false;
		}
		nBytes = next_bytes;
		uint32_t mesh_instances = 0u;
		Vector2i previousCell( -1, -1 );
		bool bHavePreviousCell = false;
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( mesh.m_cells )[iCell];
			if ( !layout.contains( cell.m_location.x, cell.m_location.y ) || cell.m_transforms.size() > ( uint32_t )( -1 ) ) {
				return false;
			}
			if ( bHavePreviousCell
			        && ( cell.m_location.x < previousCell.x
			                || ( cell.m_location.x == previousCell.x && cell.m_location.y <= previousCell.y ) ) ) {
				return false;
			}
			previousCell = cell.m_location;
			bHavePreviousCell = true;
			const size_t count = cell.m_transforms.size();
			size_t transform_bytes = 0u;
			size_t sidecar_bytes = 0u;
			size_t cell_bytes = T3D_INSTANCE_PAGE_CELL_HEADER_BYTES;
			if ( !t3dInstancePageCodecSizeMul( count, sizeof( t3dPackedTransform_s ), transform_bytes )
			        || !t3dInstancePageCodecSizeAdd( cell_bytes, transform_bytes, cell_bytes ) ) {
				return false;
			}
			if ( cell.usesColors() ) {
				if ( cell.m_colors.size() != count || !t3dInstancePageCodecSizeMul( count, sizeof( uint32_t ), sidecar_bytes )
				        || !t3dInstancePageCodecSizeAdd( cell_bytes, sidecar_bytes, cell_bytes ) ) {
					return false;
				}
			}
			if ( cell.usesOwners() ) {
				if ( cell.m_owners.size() != count || !t3dInstancePageCodecSizeMul( count, sizeof( uint32_t ), sidecar_bytes )
				        || !t3dInstancePageCodecSizeAdd( cell_bytes, sidecar_bytes, cell_bytes ) ) {
					return false;
				}
			}
			if ( !t3dInstancePageCodecSizeAdd( nBytes, cell_bytes, next_bytes )
			        || count > ( size_t )( ( uint32_t )( -1 ) - mesh_instances ) ) {
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
        const size_t nPageBytes,
        size_t &nOffset,
        uint32_t *pMeshCounts,
        uint32_t &nInstanceCount,
        uint16_t &nMeshCount,
        bool bDecode,
        Terrain3DInstanceStore *pStore,
        size_t *const pColorCount = NULL,
        size_t *const pOwnerCount = NULL ) {
	t3dInstancePageCodecClearMeshCounts( pMeshCounts );
	nInstanceCount = 0u;
	nMeshCount = 0u;
	if ( pColorCount ) {
		*pColorCount = 0u;
	}
	if ( pOwnerCount ) {
		*pOwnerCount = 0u;
	}
	const size_t page_size = nPageBytes;
	if ( page_size > ( size_t )( arrPage.size() ) ) {
		return false;
	}
	if ( !t3dInstancePageCodecPageHas( 0u, T3D_INSTANCE_PAGE_HEADER_BYTES, page_size ) ) {
		return page_size == 0u;
	}
	const uint8_t *pBytes = arrPage.ptr();
	if ( t3dInstancePageCodecReadU32( pBytes ) != T3D_INSTANCE_PAGE_MAGIC ) {
		return false;
	}
	const uint16_t version = t3dInstancePageCodecReadU16( pBytes + 4u );
	if ( version != T3D_INSTANCE_PAGE_VERSION && version != T3D_INSTANCE_PAGE_LEGACY_VERSION ) {
		return false;
	}
	const uint16_t mesh_count = t3dInstancePageCodecReadU16( pBytes + 6u );
	const uint32_t declared_total = t3dInstancePageCodecReadU32( pBytes + 8u );
	t3dInstanceLayout_t layout;
	if ( version == T3D_INSTANCE_PAGE_LEGACY_VERSION ) {
		if ( t3dInstancePageCodecReadU32( pBytes + 12u ) != 0u ) {
			return false;
		}
	} else {
		layout.m_nCellSize = t3dInstancePageCodecReadU16( pBytes + 12u );
		layout.m_nCellsPerAxis = t3dInstancePageCodecReadU16( pBytes + 14u );
		layout.m_nCellShift = t3dInstanceCellShift( layout.m_nCellSize );
	}
	if ( mesh_count > T3D_MAX_MESH_ASSETS || !layout.isValid() ) {
		return false;
	}
	nOffset = T3D_INSTANCE_PAGE_HEADER_BYTES;
	if ( bDecode && pStore ) {
		pStore->setLayout( layout );
	}
	uint32_t decoded_total = 0u;
	bool bSeenMeshes[T3D_MAX_MESH_ASSETS] = {};
	for ( uint16_t mesh_index = 0u; mesh_index < mesh_count; ++mesh_index ) {
		const size_t mesh_header_bytes =
		        version == T3D_INSTANCE_PAGE_LEGACY_VERSION ? T3D_INSTANCE_PAGE_LEGACY_MESH_HEADER_BYTES : T3D_INSTANCE_PAGE_MESH_HEADER_BYTES;
		if ( !t3dInstancePageCodecPageHas( nOffset, mesh_header_bytes, page_size ) ) {
			return false;
		}
		const int mesh_id = ( int )( t3dInstancePageCodecReadU16( pBytes + nOffset ) );
		uint32_t cell_count = 0u;
		uint32_t declared_mesh_total = 0u;
		if ( version == T3D_INSTANCE_PAGE_LEGACY_VERSION ) {
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
		if ( mesh_id < 0 || mesh_id >= T3D_MAX_MESH_ASSETS || bSeenMeshes[mesh_id] || cell_count > layout.maximumCellCount() ) {
			return false;
		}
		bSeenMeshes[mesh_id] = true;
		Vector2i previousCell( -1, -1 );
		bool bHavePreviousCell = false;
		uint32_t decoded_mesh_total = 0u;
		t3dInstanceMesh_t *pDecodedMesh = NULL;
		if ( bDecode && pStore ) {
			if ( t3dInstancePageCodecInjectDecodeFailure() ) {
				return false;
			}
			pDecodedMesh = pStore->tryGetOrCreateMesh( mesh_id, cell_count );
			if ( !pDecodedMesh ) {
				return false;
			}
		}
		for ( uint32_t cell_index = 0u; cell_index < cell_count; ++cell_index ) {
			if ( !t3dInstancePageCodecPageHas( nOffset, T3D_INSTANCE_PAGE_CELL_HEADER_BYTES, page_size ) ) {
				return false;
			}
			const int16_t cell_x = ( int16_t )( t3dInstancePageCodecReadU16( pBytes + nOffset ) );
			const int16_t cell_y = ( int16_t )( t3dInstancePageCodecReadU16( pBytes + nOffset + 2u ) );
			const uint16_t flags = t3dInstancePageCodecReadU16( pBytes + nOffset + 4u );
			const uint16_t reserved = t3dInstancePageCodecReadU16( pBytes + nOffset + 6u );
			const uint32_t count = t3dInstancePageCodecReadU32( pBytes + nOffset + 8u );
			nOffset += T3D_INSTANCE_PAGE_CELL_HEADER_BYTES;
			const Vector2i cellLocation( cell_x, cell_y );
			if ( !layout.contains( cell_x, cell_y ) || reserved != 0u || ( flags & ~T3D_INSTANCE_PAGE_CELL_FLAGS ) != 0u
			        || ( bHavePreviousCell
			                && ( cellLocation.x < previousCell.x
			                        || ( cellLocation.x == previousCell.x && cellLocation.y <= previousCell.y ) ) ) ) {
				return false;
			}
			previousCell = cellLocation;
			bHavePreviousCell = true;
			size_t transform_bytes = 0u;
			size_t sidecar_bytes = 0u;
			if ( !t3dInstancePageCodecSizeMul( ( size_t )( count ), sizeof( t3dPackedTransform_s ), transform_bytes )
			        || !t3dInstancePageCodecPageHas( nOffset, transform_bytes, page_size ) ) {
				return false;
			}
			t3dInstanceCell_s *pDecodedCell = NULL;
			if ( bDecode && pDecodedMesh && pStore ) {
				if ( t3dInstancePageCodecInjectDecodeFailure() ) {
					return false;
				}
				pDecodedCell = pStore->tryGetOrCreateCellExact( mesh_id,
				        Vector2i( cell_x, cell_y ),
				        count,
				        ( flags & T3D_INSTANCE_PAGE_CELL_COLORS ) != 0u,
				        ( flags & T3D_INSTANCE_PAGE_CELL_OWNERS ) != 0u );
				if ( !pDecodedCell || t3dInstancePageCodecInjectDecodeFailure() || !pDecodedCell->m_transforms.resize( count ) ) {
					return false;
				}
				t3dPackedTransform_s *pTransforms = pDecodedCell->m_transforms.data();
				for ( uint32_t index = 0u; index < count; ++index ) {
					t3dPackedTransform_s &transform = pTransforms[index];
					const uint8_t *pSource = pBytes + nOffset + ( size_t )( index ) * sizeof( t3dPackedTransform_s );
					transform.m_szOrigin[0] = t3dInstancePageCodecReadF32( pSource );
					transform.m_szOrigin[1] = t3dInstancePageCodecReadF32( pSource + 4u );
					transform.m_szOrigin[2] = t3dInstancePageCodecReadF32( pSource + 8u );
					for ( int basis_index = 0; basis_index < 9; ++basis_index ) {
						transform.m_szBasis[basis_index] = t3dInstancePageCodecReadU16( pSource + 12u + ( size_t )( basis_index ) * 2u );
					}
					transform.m_nReserved = t3dInstancePageCodecReadU16( pSource + 30u );
				}
			}
			nOffset += transform_bytes;
			if ( ( flags & T3D_INSTANCE_PAGE_CELL_COLORS ) != 0u ) {
				if ( pColorCount ) {
					if ( ( size_t )count > eastl::numeric_limits<size_t>::max() - *pColorCount ) {
						return false;
					}
					*pColorCount += ( size_t )count;
				}
				if ( !t3dInstancePageCodecSizeMul( ( size_t )( count ), sizeof( uint32_t ), sidecar_bytes )
				        || !t3dInstancePageCodecPageHas( nOffset, sidecar_bytes, page_size ) ) {
					return false;
				}
				if ( pDecodedCell ) {
					if ( t3dInstancePageCodecInjectDecodeFailure() || !pDecodedCell->m_colors.resize( count ) ) {
						return false;
					}
					for ( uint32_t index = 0u; index < count; ++index ) {
						pDecodedCell->m_colors[index] = t3dInstancePageCodecReadU32( pBytes + nOffset + ( size_t )( index ) * 4u );
					}
				}
				nOffset += sidecar_bytes;
			}
			if ( ( flags & T3D_INSTANCE_PAGE_CELL_OWNERS ) != 0u ) {
				if ( pOwnerCount ) {
					if ( ( size_t )count > eastl::numeric_limits<size_t>::max() - *pOwnerCount ) {
						return false;
					}
					*pOwnerCount += ( size_t )count;
				}
				if ( !t3dInstancePageCodecSizeMul( ( size_t )( count ), sizeof( uint32_t ), sidecar_bytes )
				        || !t3dInstancePageCodecPageHas( nOffset, sidecar_bytes, page_size ) ) {
					return false;
				}
				if ( pDecodedCell ) {
					if ( t3dInstancePageCodecInjectDecodeFailure() || !pDecodedCell->m_owners.resize( count ) ) {
						return false;
					}
					for ( uint32_t index = 0u; index < count; ++index ) {
						pDecodedCell->m_owners[index] = t3dInstancePageCodecReadU32( pBytes + nOffset + ( size_t )( index ) * 4u );
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

static bool t3dInstancePageCodecStoresEquivalent( const Terrain3DInstanceStore &rA, const Terrain3DInstanceStore &rB ) {
	const t3dInstanceLayout_t &layout_a = rA.getLayout();
	const t3dInstanceLayout_t &layout_b = rB.getLayout();
	if ( layout_a.m_nCellSize != layout_b.m_nCellSize || layout_a.m_nCellsPerAxis != layout_b.m_nCellsPerAxis
	        || layout_a.m_nCellShift != layout_b.m_nCellShift || rA.meshCount() != rB.meshCount() ) {
		return false;
	}
	const Terrain3DConstInstanceMeshRange meshes_a = rA.meshes();
	const Terrain3DConstInstanceMeshRange meshes_b = rB.meshes();
	for ( size_t iMesh = 0u; iMesh < meshes_a.size(); ++iMesh ) {
		const t3dInstanceMesh_t &mesh_a = meshes_a[iMesh];
		const t3dInstanceMesh_t &mesh_b = meshes_b[iMesh];
		if ( mesh_a.m_nId != mesh_b.m_nId || mesh_a.m_cells.size() != mesh_b.m_cells.size() ) {
			return false;
		}
		for ( size_t iCell = 0u; iCell < mesh_a.m_cells.size(); ++iCell ) {
			const t3dInstanceCell_s &cell_a = mesh_a.m_cells[iCell];
			const t3dInstanceCell_s &cell_b = mesh_b.m_cells[iCell];
			const size_t count = cell_a.m_transforms.size();
			if ( cell_a.m_location != cell_b.m_location || count != cell_b.m_transforms.size()
			        || cell_a.m_colors.size() != cell_b.m_colors.size() || cell_a.m_owners.size() != cell_b.m_owners.size()
			        || cell_a.m_nRevision != cell_b.m_nRevision ) {
				return false;
			}
			if ( count > 0u
			        && memcmp( cell_a.m_transforms.data(), cell_b.m_transforms.data(), count * sizeof( t3dPackedTransform_s ) ) != 0 ) {
				return false;
			}
			if ( !cell_a.m_colors.empty()
			        && memcmp( cell_a.m_colors.data(), cell_b.m_colors.data(), cell_a.m_colors.size() * sizeof( uint32_t ) ) != 0 ) {
				return false;
			}
			if ( !cell_a.m_owners.empty()
			        && memcmp( cell_a.m_owners.data(), cell_b.m_owners.data(), cell_a.m_owners.size() * sizeof( uint32_t ) ) != 0 ) {
				return false;
			}
		}
	}
	return true;
}

bool Terrain3DInstancePageCodec::encode( const Terrain3DInstanceStore &rStore,
        PackedByteArray &rPage,
        uint32_t *const pMeshCounts,
        uint32_t &nInstanceCount,
        uint16_t &nMeshCount ) {
	size_t page_bytes = 0u;
	if ( !t3dInstancePageCodecMeasurePage( rStore, page_bytes, pMeshCounts, nInstanceCount, nMeshCount ) ) {
		return false;
	}
	if ( page_bytes > ( size_t )( INT32_MAX ) || rPage.resize( ( int64_t )( page_bytes ) ) != 0
	        || rPage.size() != ( int64_t )( page_bytes ) ) {
		rPage.clear();
		return false;
	}
	uint8_t *const pBytes = rPage.ptrw();
	if ( page_bytes > 0u && !pBytes ) {
		rPage.clear();
		return false;
	}
	t3dInstancePageCodecWriteU32( pBytes, T3D_INSTANCE_PAGE_MAGIC );
	t3dInstancePageCodecWriteU16( pBytes + 4u, T3D_INSTANCE_PAGE_VERSION );
	t3dInstancePageCodecWriteU16( pBytes + 6u, nMeshCount );
	t3dInstancePageCodecWriteU32( pBytes + 8u, nInstanceCount );
	const t3dInstanceLayout_t &layout = rStore.getLayout();
	t3dInstancePageCodecWriteU16( pBytes + 12u, layout.m_nCellSize );
	t3dInstancePageCodecWriteU16( pBytes + 14u, layout.m_nCellsPerAxis );
	size_t offset = T3D_INSTANCE_PAGE_HEADER_BYTES;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( rStore.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( rStore.meshes() )[iMesh];
		uint32_t mesh_total = 0u;
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( mesh.m_cells )[iCell];
			mesh_total += ( uint32_t )( cell.m_transforms.size() );
		}
		t3dInstancePageCodecWriteU16( pBytes + offset, ( uint16_t )( mesh.m_nId ) );
		t3dInstancePageCodecWriteU16( pBytes + offset + 2u, 0u );
		t3dInstancePageCodecWriteU32( pBytes + offset + 4u, ( uint32_t )( mesh.m_cells.size() ) );
		t3dInstancePageCodecWriteU32( pBytes + offset + 8u, mesh_total );
		offset += T3D_INSTANCE_PAGE_MESH_HEADER_BYTES;
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( mesh.m_cells )[iCell];
			uint16_t flags = 0u;
			if ( cell.usesColors() ) {
				flags |= T3D_INSTANCE_PAGE_CELL_COLORS;
			}
			if ( cell.usesOwners() ) {
				flags |= T3D_INSTANCE_PAGE_CELL_OWNERS;
			}
			const uint32_t count = ( uint32_t )( cell.m_transforms.size() );
			t3dInstancePageCodecWriteU16( pBytes + offset, ( uint16_t )( ( int16_t )( cell.m_location.x ) ) );
			t3dInstancePageCodecWriteU16( pBytes + offset + 2u, ( uint16_t )( ( int16_t )( cell.m_location.y ) ) );
			t3dInstancePageCodecWriteU16( pBytes + offset + 4u, flags );
			t3dInstancePageCodecWriteU16( pBytes + offset + 6u, 0u );
			t3dInstancePageCodecWriteU32( pBytes + offset + 8u, count );
			offset += T3D_INSTANCE_PAGE_CELL_HEADER_BYTES;
			const t3dPackedTransform_s *pTransforms = cell.m_transforms.data();
			for ( uint32_t index = 0u; index < count; ++index ) {
				const t3dPackedTransform_s &transform = pTransforms[index];
				t3dInstancePageCodecWriteF32( pBytes + offset, transform.m_szOrigin[0] );
				t3dInstancePageCodecWriteF32( pBytes + offset + 4u, transform.m_szOrigin[1] );
				t3dInstancePageCodecWriteF32( pBytes + offset + 8u, transform.m_szOrigin[2] );
				for ( int basis_index = 0; basis_index < 9; ++basis_index ) {
					t3dInstancePageCodecWriteU16(
					        pBytes + offset + 12u + ( size_t )( basis_index ) * 2u, transform.m_szBasis[basis_index] );
				}
				t3dInstancePageCodecWriteU16( pBytes + offset + 30u, transform.m_nReserved );
				offset += sizeof( t3dPackedTransform_s );
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

bool Terrain3DInstancePageCodec::decode( const PackedByteArray &arrPage,
        Terrain3DInstanceStore &rStore,
        uint32_t *const pMeshCounts,
        uint32_t &nInstanceCount,
        uint16_t &nMeshCount ) {
	return decode( arrPage, ( size_t )( arrPage.size() ), rStore, pMeshCounts, nInstanceCount, nMeshCount );
}

bool Terrain3DInstancePageCodec::decode( const PackedByteArray &arrPage,
        const size_t nPageBytes,
        Terrain3DInstanceStore &rStore,
        uint32_t *const pMeshCounts,
        uint32_t &nInstanceCount,
        uint16_t &nMeshCount ) {
	uint32_t decoded_mesh_counts[T3D_MAX_MESH_ASSETS] = {};
	uint32_t decoded_instance_count = 0u;
	uint16_t decoded_mesh_count = 0u;
	Terrain3DInstanceStore decoded;

	if ( nPageBytes == 0u ) {
		decoded.setLayout( rStore.getLayout() );
	} else {
		uint32_t inspected_instances = 0u;
		uint16_t inspected_meshes = 0u;
		size_t inspect_offset = 0u;
		size_t color_count = 0u;
		size_t owner_count = 0u;
		if ( !t3dInstancePageCodecReadPageHeaders( arrPage,
		             nPageBytes,
		             inspect_offset,
		             NULL,
		             inspected_instances,
		             inspected_meshes,
		             false,
		             NULL,
		             &color_count,
		             &owner_count ) ) {
			return false;
		}

		size_t decode_offset = 0u;
		if ( !t3dInstancePageCodecReadPageHeaders(
		             arrPage, nPageBytes, decode_offset, decoded_mesh_counts, decoded_instance_count, decoded_mesh_count, true, &decoded )
		        || decode_offset != nPageBytes || decoded_instance_count != inspected_instances || decoded_mesh_count != inspected_meshes
		        || !decoded.validateDeep() || decoded.instanceCount() != ( size_t )( decoded_instance_count )
		        || decoded.meshCount() != ( size_t )( decoded_mesh_count ) ) {
			return false;
		}
	}

	if ( t3dInstancePageCodecInjectDecodeFailure() ) {
		return false;
	}

	if ( rStore.allocationClass() == TERRAIN3D_INSTANCE_STORE_PERSISTENT_RESOURCE ) {
		rStore = eastl::move( decoded );
	} else {
		rStore = decoded;
		if ( !rStore.validateDeep() || !t3dInstancePageCodecStoresEquivalent( rStore, decoded ) ) {
			return false;
		}
	}

	if ( pMeshCounts ) {
		memcpy( pMeshCounts, decoded_mesh_counts, sizeof( decoded_mesh_counts ) );
	}
	nInstanceCount = decoded_instance_count;
	nMeshCount = decoded_mesh_count;
	return true;
}

bool Terrain3DInstancePageCodec::inspect(
        const PackedByteArray &arrPage, uint32_t *const pMeshCounts, uint32_t &nInstanceCount, uint16_t &nMeshCount ) {
	if ( arrPage.is_empty() ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return true;
	}
	size_t offset = 0u;
	if ( !t3dInstancePageCodecReadPageHeaders(
	             arrPage, ( size_t )( arrPage.size() ), offset, pMeshCounts, nInstanceCount, nMeshCount, false, NULL ) ) {
		t3dInstancePageCodecClearMeshCounts( pMeshCounts );
		nInstanceCount = 0u;
		nMeshCount = 0u;
		return false;
	}
	return true;
}
