// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_CONTRACT_H
#define TERRAIN3D_WORLD_CONTRACT_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include <stddef.h>
#include <stdint.h>

#define T3D_WORLD_TRIANGLE_BACKSIDE (1u << 18u)


// P09 fixed envelope buckets. Every strip cluster is routed to the smallest
// envelope that can contain its actual vertex count. All six terrain commands
// share one TRIANGLE_STRIPS pipeline and are submitted with one multi-draw.


// Terrain bucket commands occupy the prefix of the indirect buffer; the mesh
// command remains the final command.


// t3dWorldGeometryKind_e centralized in Terrain3DTypes.h.


// Static-world authoring tier. Kept in the canonical World contract so authoring,
// material cooking, and World publication share one tier vocabulary.
// t3dWorldTier_e centralized in Terrain3DTypes.h.


// Terrain strip-state packing. Low 16 bits retain patch flags. Tier and stitch
// positions preserve the ABI-v1 bit assignments; ABI v2 consumes the two
// previously unused bits for strip traversal state and places part_code above
// the stitch fields.


// Canonical static world vertex. Position remains float32. Normal/tangent and
// UV pairs are explicitly packed by the cold cooker; no renderer-specific
// vertex layout is part of this ABI.
// t3dWorldVertex_s centralized in Terrain3DTypes.h.


// packedCounts: low16 vertexCount, high16 triangleCount.
// packedLodGeometryKind: low16 lod, high16 t3dWorldGeometryKind_t.
// t3dWorldCluster_s centralized in Terrain3DTypes.h.


// ABI v2 procedural terrain strip cluster. The fixed 48-byte footprint is
// retained, but topology vertex/index offsets are gone from the canonical
// record. Grid coordinates are reconstructed from gl_VertexIndex.
// t3dWorldTerrainCluster_s centralized in Terrain3DTypes.h.


// One affine 3x4 transform. Current and previous history use separate arrays
// of the same ABI so previous-transform state is never renderer-specific.
// t3dWorldTransform_s centralized in Terrain3DTypes.h.


// t3dWorldVisibleItem_s centralized in Terrain3DTypes.h.


// t3dWorldDrawIndirectArgs_s centralized in Terrain3DTypes.h.


// Cold-cook VT metadata kept separate from the 64-byte raster cluster ABI.
// t3dWorldClusterVTMetadata_s centralized in Terrain3DTypes.h.


// t3dWorldMeshClusterSpan_s centralized in Terrain3DTypes.h.


// GPU-visible per-view world frame. Matrices and planes deliberately use only
// 16-byte groups so the C++ layout mirrors std430 without backend-specific
// padding rules.
// t3dWorldFrame_s centralized in Terrain3DTypes.h.


// Cold-established resource limits. Byte budgets are 64-bit CPU policy values;
// this record is not uploaded as a shader block.
// t3dWorldResourceLimits_s centralized in Terrain3DTypes.h.


static inline uint32_t t3dWorldTerrainVisibleBase( void ) {
	return 0u;
}

static inline uint32_t t3dWorldTerrainVisibleCapacity( const uint32_t nTerrainClusterCount, const uint32_t nVisibleCapacity ) {
	return nTerrainClusterCount < nVisibleCapacity ? nTerrainClusterCount : nVisibleCapacity;
}

static inline uint32_t t3dWorldNonTerrainVisibleBase( const uint32_t nTerrainClusterCount, const uint32_t nVisibleCapacity ) {
	( void )nVisibleCapacity;
	return nTerrainClusterCount;
}

static inline uint32_t t3dWorldNonTerrainVisibleCapacity( const uint32_t nTerrainClusterCount, const uint32_t nVisibleCapacity ) {
	const uint32_t nBase = t3dWorldNonTerrainVisibleBase( nTerrainClusterCount, nVisibleCapacity );
	return nVisibleCapacity > nBase ? nVisibleCapacity - nBase : 0u;
}

static inline uint32_t t3dWorldIndirectOffsetBytes( const uint32_t nCommandIndex ) {
	return nCommandIndex * T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES;
}

static inline uint32_t t3dWorldTerrainStripBucketVertexCount( const uint32_t nBucket ) {
	switch ( nBucket ) {
		case 0u:
			return T3D_WORLD_TERRAIN_STRIP_BUCKET_0_VERTEX_COUNT;
		case 1u:
			return T3D_WORLD_TERRAIN_STRIP_BUCKET_1_VERTEX_COUNT;
		case 2u:
			return T3D_WORLD_TERRAIN_STRIP_BUCKET_2_VERTEX_COUNT;
		case 3u:
			return T3D_WORLD_TERRAIN_STRIP_BUCKET_3_VERTEX_COUNT;
		case 4u:
			return T3D_WORLD_TERRAIN_STRIP_BUCKET_4_VERTEX_COUNT;
		default:
			return T3D_WORLD_TERRAIN_STRIP_BUCKET_5_VERTEX_COUNT;
	}
}

static inline uint32_t t3dWorldTerrainStripBucketIndex( const uint32_t nActualVertexCount ) {
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_0_VERTEX_COUNT ) { return 0u; }
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_1_VERTEX_COUNT ) { return 1u; }
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_2_VERTEX_COUNT ) { return 2u; }
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_3_VERTEX_COUNT ) { return 3u; }
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_4_VERTEX_COUNT ) { return 4u; }
	return 5u;
}

static inline uint32_t t3dWorldTerrainStripBucketCounter( const uint32_t nBucket ) {
	return T3D_WORLD_VIS_COUNTER_TERRAIN_BUCKET_BASE + nBucket;
}

static inline uint32_t t3dWorldPackU16Pair( const uint16_t nLow, const uint16_t nHigh ) {
	return ( uint32_t )( nLow ) | ( ( uint32_t )( nHigh ) << 16u );
}

static inline uint16_t t3dWorldUnpackU16Low( const uint32_t nPacked ) {
	return ( uint16_t )( nPacked & 0xffffu );
}

static inline uint16_t t3dWorldUnpackU16High( const uint32_t nPacked ) {
	return ( uint16_t )( nPacked >> 16u );
}

static inline uint32_t t3dWorldPackTerrainGridOrigin( const int16_t nGridX, const int16_t nGridZ ) {
	return ( uint32_t )( ( uint16_t )nGridX ) | ( ( uint32_t )( ( uint16_t )nGridZ ) << 16u );
}

static inline int16_t t3dWorldUnpackTerrainGridOriginX( const uint32_t nPacked ) {
	return ( int16_t )( nPacked & 0xffffu );
}

static inline int16_t t3dWorldUnpackTerrainGridOriginZ( const uint32_t nPacked ) {
	return ( int16_t )( nPacked >> 16u );
}

static inline uint32_t t3dWorldPackTerrainGridExtent( const uint16_t nCellWidth, const uint16_t nRowCount ) {
	return t3dWorldPackU16Pair( nCellWidth, nRowCount );
}

static inline uint16_t t3dWorldUnpackTerrainCellWidth( const uint32_t nPacked ) {
	return t3dWorldUnpackU16Low( nPacked );
}

static inline uint16_t t3dWorldUnpackTerrainRowCount( const uint32_t nPacked ) {
	return t3dWorldUnpackU16High( nPacked );
}

static inline uint32_t t3dWorldPackTerrainVertexCountLod( const uint16_t nVertexCount, const uint16_t nLod ) {
	return t3dWorldPackU16Pair( nVertexCount, nLod );
}

static inline uint16_t t3dWorldUnpackTerrainVertexCount( const uint32_t nPacked ) {
	return t3dWorldUnpackU16Low( nPacked );
}

static inline uint16_t t3dWorldUnpackTerrainLod( const uint32_t nPacked ) {
	return t3dWorldUnpackU16High( nPacked );
}

static inline uint32_t t3dWorldPackTerrainStripState( const uint16_t nPatchFlags,
        const uint8_t nTier,
        const uint8_t nStitchX,
        const uint8_t nStitchZ,
        const uint8_t nPartCode,
        const bool bSerpentineRows,
        const bool bSerpentineBaseOdd ) {
	return ( uint32_t )nPatchFlags | ( ( ( uint32_t )nTier & 0x3u ) << T3D_WORLD_TERRAIN_TIER_SHIFT )
	        | ( bSerpentineRows ? ( 1u << T3D_WORLD_TERRAIN_SERPENTINE_ROWS_BIT ) : 0u )
	        | ( bSerpentineBaseOdd ? ( 1u << T3D_WORLD_TERRAIN_SERPENTINE_BASE_ODD_BIT ) : 0u )
	        | ( ( ( uint32_t )nStitchX & 0x3u ) << T3D_WORLD_TERRAIN_STITCH_X_SHIFT )
	        | ( ( ( uint32_t )nStitchZ & 0x3u ) << T3D_WORLD_TERRAIN_STITCH_Z_SHIFT )
	        | ( ( ( uint32_t )nPartCode & 0x7u ) << T3D_WORLD_TERRAIN_PART_CODE_SHIFT );
}

static inline uint16_t t3dWorldUnpackTerrainPatchFlags( const uint32_t nFlags ) {
	return ( uint16_t )( nFlags & T3D_WORLD_TERRAIN_PATCH_FLAGS_MASK );
}

static inline uint8_t t3dWorldUnpackTerrainTier( const uint32_t nFlags ) {
	return ( uint8_t )( ( nFlags & T3D_WORLD_TERRAIN_TIER_MASK ) >> T3D_WORLD_TERRAIN_TIER_SHIFT );
}

static inline uint8_t t3dWorldUnpackTerrainStitchX( const uint32_t nFlags ) {
	return ( uint8_t )( ( nFlags & T3D_WORLD_TERRAIN_STITCH_X_MASK ) >> T3D_WORLD_TERRAIN_STITCH_X_SHIFT );
}

static inline uint8_t t3dWorldUnpackTerrainStitchZ( const uint32_t nFlags ) {
	return ( uint8_t )( ( nFlags & T3D_WORLD_TERRAIN_STITCH_Z_MASK ) >> T3D_WORLD_TERRAIN_STITCH_Z_SHIFT );
}

static inline uint8_t t3dWorldUnpackTerrainPartCode( const uint32_t nFlags ) {
	return ( uint8_t )( ( nFlags & T3D_WORLD_TERRAIN_PART_CODE_MASK ) >> T3D_WORLD_TERRAIN_PART_CODE_SHIFT );
}

static inline bool t3dWorldTerrainUsesSerpentineRows( const uint32_t nFlags ) {
	return ( nFlags & ( 1u << T3D_WORLD_TERRAIN_SERPENTINE_ROWS_BIT ) ) != 0u;
}

static inline bool t3dWorldTerrainSerpentineBaseOdd( const uint32_t nFlags ) {
	return ( nFlags & ( 1u << T3D_WORLD_TERRAIN_SERPENTINE_BASE_ODD_BIT ) ) != 0u;
}

static_assert( T3D_WORLD_CLUSTER_MAX_VERTICES == 64u, "World cluster vertex limit changed" );
static_assert( T3D_WORLD_CLUSTER_MAX_TRIANGLES == 124u, "World cluster triangle limit changed" );
static_assert( T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT == 372u, "World cluster raster envelope changed" );
static_assert( T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT == 372u, "Terrain strip raster envelope changed" );
static_assert( T3D_WORLD_TERRAIN_STRIP_MAX_CELL_WIDTH == 184u, "Terrain strip max width changed" );
static_assert( T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT == 6u, "Terrain strip bucket count changed" );
static_assert( T3D_WORLD_INDIRECT_COMMAND_COUNT == 7u, "World indirect command count changed" );
static_assert( T3D_WORLD_INDIRECT_BUFFER_SIZE_BYTES == 112u, "World indirect buffer ABI changed" );
static_assert( T3D_WORLD_VIS_COUNTER_TERRAIN_SUBMITTED_VERTICES < T3D_WORLD_VIS_COUNTER_COUNT, "World visibility counter ABI overflow" );

static_assert( sizeof( t3dWorldVertex_t ) == T3D_WORLD_VERTEX_SIZE_BYTES, "World vertex ABI changed" );
static_assert( alignof( t3dWorldVertex_t ) == 16u, "World vertex alignment changed" );
static_assert( offsetof( t3dWorldVertex_t, m_nPackedNormal ) == 12u, "World vertex normal offset changed" );
static_assert( offsetof( t3dWorldVertex_t, m_nPackedTangent ) == 16u, "World vertex tangent offset changed" );
static_assert( offsetof( t3dWorldVertex_t, m_nPackedUv0 ) == 20u, "World vertex UV0 offset changed" );
static_assert( offsetof( t3dWorldVertex_t, m_nPackedUv1 ) == 24u, "World vertex UV1 offset changed" );
static_assert( offsetof( t3dWorldVertex_t, m_nColorRgba8 ) == 28u, "World vertex color offset changed" );

static_assert( sizeof( t3dWorldCluster_t ) == T3D_WORLD_CLUSTER_SIZE_BYTES, "World cluster ABI changed" );
static_assert( alignof( t3dWorldCluster_t ) == 16u, "World cluster alignment changed" );
static_assert( offsetof( t3dWorldCluster_t, m_fConeAxis ) == 16u, "World cluster cone offset changed" );
static_assert( offsetof( t3dWorldCluster_t, m_nVertexIndexOffset ) == 32u, "World cluster vertex-index offset changed" );
static_assert( offsetof( t3dWorldCluster_t, m_nPackedCounts ) == 40u, "World cluster count offset changed" );
static_assert( offsetof( t3dWorldCluster_t, m_nMaterial ) == 48u, "World cluster material offset changed" );
static_assert( offsetof( t3dWorldCluster_t, m_nSourceId ) == 56u, "World cluster source offset changed" );

static_assert( sizeof( t3dWorldTerrainCluster_t ) == T3D_WORLD_TERRAIN_CLUSTER_SIZE_BYTES, "World terrain cluster ABI changed" );
static_assert( alignof( t3dWorldTerrainCluster_t ) == 16u, "World terrain cluster alignment changed" );
static_assert( offsetof( t3dWorldTerrainCluster_t, m_nPackedGridOrigin ) == 4u, "World terrain cluster grid-origin offset changed" );
static_assert( offsetof( t3dWorldTerrainCluster_t, m_nPackedGridExtent ) == 8u, "World terrain cluster grid-extent offset changed" );
static_assert( offsetof( t3dWorldTerrainCluster_t, m_nPackedVertexCountLod ) == 12u, "World terrain cluster vertex/LOD offset changed" );
static_assert( offsetof( t3dWorldTerrainCluster_t, m_nFlags ) == 16u, "World terrain cluster flags offset changed" );
static_assert( offsetof( t3dWorldTerrainCluster_t, m_nMaterialContext ) == 32u, "World terrain cluster material offset changed" );

static_assert( sizeof( t3dWorldTransform_t ) == T3D_WORLD_TRANSFORM_SIZE_BYTES, "World transform ABI changed" );
static_assert( alignof( t3dWorldTransform_t ) == 16u, "World transform alignment changed" );
static_assert( offsetof( t3dWorldTransform_t, m_fRow1 ) == 16u, "World transform row1 offset changed" );
static_assert( offsetof( t3dWorldTransform_t, m_fRow2 ) == 32u, "World transform row2 offset changed" );

static_assert( sizeof( t3dWorldVisibleItem_t ) == T3D_WORLD_VISIBLE_ITEM_SIZE_BYTES, "World visible-item ABI changed" );
static_assert( alignof( t3dWorldVisibleItem_t ) == 16u, "World visible-item alignment changed" );
static_assert( offsetof( t3dWorldVisibleItem_t, m_nGeometryKind ) == 8u, "World visible-item geometry-kind offset changed" );

static_assert( sizeof( t3dWorldDrawIndirectArgs_t ) == T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES, "World indirect ABI changed" );
static_assert( alignof( t3dWorldDrawIndirectArgs_t ) == 16u, "World indirect alignment changed" );
static_assert( offsetof( t3dWorldDrawIndirectArgs_t, m_nInstanceCount ) == 4u, "World indirect instance-count offset changed" );

static_assert( sizeof( t3dWorldClusterVTMetadata_t ) == 16u, "World cluster VT metadata ABI changed" );
static_assert( sizeof( t3dWorldMeshClusterSpan_t ) == 16u, "World mesh cluster span ABI changed" );

static_assert( sizeof( t3dWorldFrame_t ) == T3D_WORLD_FRAME_SIZE_BYTES, "World frame ABI changed" );
static_assert( alignof( t3dWorldFrame_t ) == 16u, "World frame alignment changed" );
static_assert( offsetof( t3dWorldFrame_t, m_fPreviousViewProjection ) == 64u, "World frame previous matrix offset changed" );
static_assert( offsetof( t3dWorldFrame_t, m_fFrustumPlanes ) == 128u, "World frame frustum offset changed" );
static_assert( offsetof( t3dWorldFrame_t, m_nClusterCount ) == 272u, "World frame count offset changed" );
static_assert( offsetof( t3dWorldFrame_t, m_nPublicationGeneration ) == 304u, "World frame generation offset changed" );
static_assert( offsetof( t3dWorldFrame_t, m_nHiZWidth ) == 320u, "World frame Hi-Z offset changed" );

static_assert( sizeof( t3dWorldResourceLimits_t ) == T3D_WORLD_RESOURCE_LIMITS_SIZE_BYTES, "World resource-limit ABI changed" );
static_assert( alignof( t3dWorldResourceLimits_t ) == 16u, "World resource-limit alignment changed" );
static_assert( offsetof( t3dWorldResourceLimits_t, m_nGeometryHeapBytes ) == 32u, "World resource-limit byte budget offset changed" );
static_assert( offsetof( t3dWorldResourceLimits_t, m_nHiZMaxWidth ) == 72u, "World resource-limit Hi-Z offset changed" );
static_assert( offsetof( t3dWorldResourceLimits_t, m_nVtPageRequestBudget ) == 84u, "World resource-limit VT budget offset changed" );

#endif // TERRAIN3D_WORLD_CONTRACT_H
