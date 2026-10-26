// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_CLIPMAP_STRIP_CONTRACT_H
#define TERRAIN3D_CLIPMAP_STRIP_CONTRACT_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>

// Canonical procedural terrain part codes. These values intentionally preserve
// the legacy vertex metadata consumed by t3d_terrain_vertex_evaluate().

// Descriptor-only role/layout flags. Dynamic stitch_x/stitch_z remain patch
// state and are interpreted by the existing terrain vertex evaluator; they are
// deliberately not duplicated in this cold topology description.

#define T3D_CLIPMAP_STRIP_ROLE_MASK                                                                                    \
	( T3D_CLIPMAP_STRIP_FLAG_ROLE_TILE | T3D_CLIPMAP_STRIP_FLAG_ROLE_TRIM_A | T3D_CLIPMAP_STRIP_FLAG_ROLE_TRIM_B |     \
	  T3D_CLIPMAP_STRIP_FLAG_ROLE_FILL_A | T3D_CLIPMAP_STRIP_FLAG_ROLE_FILL_B | T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_X |   \
	  T3D_CLIPMAP_STRIP_FLAG_ROLE_EDGE_Z )
#define T3D_CLIPMAP_STRIP_LAYOUT_MASK ( T3D_CLIPMAP_STRIP_FLAG_LAYOUT_LOD0 | T3D_CLIPMAP_STRIP_FLAG_LAYOUT_OUTER_RING )
#define T3D_CLIPMAP_STRIP_KNOWN_FLAG_MASK                                                                              \
	( T3D_CLIPMAP_STRIP_FLAG_SERPENTINE_ROWS | T3D_CLIPMAP_STRIP_LAYOUT_MASK | T3D_CLIPMAP_STRIP_ROLE_MASK )
#define T3D_CLIPMAP_STRIP_LOD0_PART_COUNT		( 24u )
#define T3D_CLIPMAP_STRIP_OUTER_RING_PART_COUNT ( 20u )
#define T3D_CLIPMAP_STRIP_PART_WORD_COUNT		( 3u )
#define T3D_CLIPMAP_STRIP_VERTEX_ID_SHIFT		( 18u )
#define T3D_CLIPMAP_STRIP_VERTEX_ID_STRIDE		( 1u << T3D_CLIPMAP_STRIP_VERTEX_ID_SHIFT )
#define T3D_CLIPMAP_STRIP_VERTEX_ID_MASK		( T3D_CLIPMAP_STRIP_VERTEX_ID_STRIDE - 1u )

static_assert( sizeof( t3dClipmapStripPart_t ) == 12u, "Clipmap strip-part descriptor ABI changed" );
static_assert( offsetof( t3dClipmapStripPart_t, m_nOriginX ) == 0u, "Clipmap strip origin-X offset changed" );
static_assert( offsetof( t3dClipmapStripPart_t, m_nCellWidth ) == 4u, "Clipmap strip width offset changed" );
static_assert( offsetof( t3dClipmapStripPart_t, m_nPartCode ) == 8u, "Clipmap strip part-code offset changed" );
static_assert( offsetof( t3dClipmapStripPart_t, m_nFlags ) == 10u, "Clipmap strip flags offset changed" );

EA_FORCE_INLINE uint32_t t3dClipmapStripVerticesPerRun(
	const uint32_t nCellWidth ) {
	return nCellWidth * 2u + 4u;
}

EA_FORCE_INLINE uint32_t t3dClipmapStripActualVertexCount(
	const t3dClipmapStripPart_t &rPart ) {
	return t3dClipmapStripVerticesPerRun( rPart.m_nCellWidth ) * rPart.m_nCellHeight;
}

EA_FORCE_INLINE uint32_t t3dClipmapStripVirtualFirstVertex(
	const uint32_t nPartIndex ) {
	return nPartIndex << T3D_CLIPMAP_STRIP_VERTEX_ID_SHIFT;
}

EA_FORCE_INLINE bool t3dClipmapStripPartFitsVirtualVertexRange(
	const t3dClipmapStripPart_t &rPart ) {
	const uint32_t nVertexCount = t3dClipmapStripActualVertexCount( rPart );
	return rPart.m_nCellWidth > 0u && rPart.m_nCellHeight > 0u && nVertexCount > 0u &&
		   nVertexCount < T3D_CLIPMAP_STRIP_VERTEX_ID_STRIDE;
}

#endif // TERRAIN3D_CLIPMAP_STRIP_CONTRACT_H
