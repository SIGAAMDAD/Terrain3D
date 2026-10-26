// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_CONTRACT_H
#define TERRAIN3D_WORLD_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

static const uint32_t T3D_WORLD_ABI_VERSION = 2u;
static const uint32_t T3D_WORLD_INVALID_ID = 0xffffffffu;
static const uint32_t T3D_WORLD_CLUSTER_MAX_VERTICES = 64u;
static const uint32_t T3D_WORLD_CLUSTER_MAX_TRIANGLES = 124u;
static const uint32_t T3D_WORLD_CLUSTER_RASTER_VERTEX_COUNT = 372u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_RASTER_VERTEX_COUNT = 372u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_MAX_CELL_WIDTH = 184u;

// P09 fixed envelope buckets. Every strip cluster is routed to the smallest
// envelope that can contain its actual vertex count. All six terrain commands
// share one TRIANGLE_STRIPS pipeline and are submitted with one multi-draw.
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT = 6u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_0_VERTEX_COUNT = 64u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_1_VERTEX_COUNT = 128u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_2_VERTEX_COUNT = 192u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_3_VERTEX_COUNT = 256u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_4_VERTEX_COUNT = 320u;
static const uint32_t T3D_WORLD_TERRAIN_STRIP_BUCKET_5_VERTEX_COUNT = 372u;

// Terrain bucket commands occupy the prefix of the indirect buffer; the mesh
// command remains the final command.
static const uint32_t T3D_WORLD_INDIRECT_TERRAIN_STRIP_INDEX = 0u;
static const uint32_t T3D_WORLD_INDIRECT_NONTERRAIN_INDEX = T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT;
static const uint32_t T3D_WORLD_INDIRECT_COMMAND_COUNT = T3D_WORLD_TERRAIN_STRIP_BUCKET_COUNT + 1u;

static const uint32_t T3D_WORLD_FRAME_FLAG_HIZ_VALID = 1u << 0u;

static const uint32_t T3D_WORLD_VIS_COUNTER_VISIBLE = 0u;
static const uint32_t T3D_WORLD_VIS_COUNTER_CANDIDATE = 1u;
static const uint32_t T3D_WORLD_VIS_COUNTER_FRUSTUM_REJECT = 2u;
static const uint32_t T3D_WORLD_VIS_COUNTER_LOD_REJECT = 3u;
static const uint32_t T3D_WORLD_VIS_COUNTER_CONE_REJECT = 4u;
static const uint32_t T3D_WORLD_VIS_COUNTER_HIZ_REJECT = 5u;
static const uint32_t T3D_WORLD_VIS_COUNTER_OVERFLOW = 6u;
static const uint32_t T3D_WORLD_VIS_COUNTER_TERRAIN = 7u;
static const uint32_t T3D_WORLD_VIS_COUNTER_MESH = 8u;
static const uint32_t T3D_WORLD_VIS_COUNTER_HLOD = 9u;
static const uint32_t T3D_WORLD_VIS_COUNTER_STATIC = 10u;
static const uint32_t T3D_WORLD_VIS_COUNTER_NONTERRAIN = 11u;
static const uint32_t T3D_WORLD_VIS_COUNTER_TERRAIN_BUCKET_BASE = 12u;
static const uint32_t T3D_WORLD_VIS_COUNTER_TERRAIN_USEFUL_VERTICES = 18u;
static const uint32_t T3D_WORLD_VIS_COUNTER_TERRAIN_SUBMITTED_VERTICES = 19u;
static const uint32_t T3D_WORLD_VIS_COUNTER_COUNT = 20u;

static const uint32_t T3D_WORLD_VERTEX_SIZE_BYTES = 32u;
static const uint32_t T3D_WORLD_CLUSTER_SIZE_BYTES = 64u;
static const uint32_t T3D_WORLD_TERRAIN_CLUSTER_SIZE_BYTES = 48u;
static const uint32_t T3D_WORLD_TRANSFORM_SIZE_BYTES = 48u;
static const uint32_t T3D_WORLD_VISIBLE_ITEM_SIZE_BYTES = 16u;
static const uint32_t T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES = 16u;
static const uint32_t T3D_WORLD_INDIRECT_BUFFER_SIZE_BYTES = T3D_WORLD_DRAW_INDIRECT_ARGS_SIZE_BYTES * T3D_WORLD_INDIRECT_COMMAND_COUNT;
static const uint32_t T3D_WORLD_FRAME_SIZE_BYTES = 336u;
static const uint32_t T3D_WORLD_RESOURCE_LIMITS_SIZE_BYTES = 96u;

typedef enum t3dWorldGeometryKind_e : uint32_t {
	T3D_WORLD_GEOMETRY_TERRAIN = 0u,
	T3D_WORLD_GEOMETRY_MESH_CLUSTER = 1u,
	T3D_WORLD_GEOMETRY_HLOD_CLUSTER = 2u,
	T3D_WORLD_GEOMETRY_STATIC_CLUSTER = 3u,
	T3D_WORLD_GEOMETRY_KIND_COUNT = 4u,
} t3dWorldGeometryKind_t;

// Static-world authoring tier. Kept in the canonical World contract so authoring,
// material cooking, and World publication share one tier vocabulary.
typedef enum t3dWorldTier_e : uint32_t {
	T3D_WORLD_TIER_WORLD = 0u,
	T3D_WORLD_TIER_CLUTTER = 1u,
	T3D_WORLD_TIER_FOLIAGE = 2u,
	T3D_WORLD_TIER_COUNT = 3u,
} t3dWorldTier_t;

// Terrain strip-state packing. Low 16 bits retain patch flags. Tier and stitch
// positions preserve the ABI-v1 bit assignments; ABI v2 consumes the two
// previously unused bits for strip traversal state and places part_code above
// the stitch fields.
static const uint32_t T3D_WORLD_TERRAIN_PATCH_FLAGS_MASK = 0x0000ffffu;
static const uint32_t T3D_WORLD_TERRAIN_TIER_SHIFT = 16u;
static const uint32_t T3D_WORLD_TERRAIN_TIER_MASK = 0x00030000u;
static const uint32_t T3D_WORLD_TERRAIN_SERPENTINE_ROWS_BIT = 18u;
static const uint32_t T3D_WORLD_TERRAIN_SERPENTINE_BASE_ODD_BIT = 19u;
static const uint32_t T3D_WORLD_TERRAIN_STITCH_X_SHIFT = 20u;
static const uint32_t T3D_WORLD_TERRAIN_STITCH_X_MASK = 0x00300000u;
static const uint32_t T3D_WORLD_TERRAIN_STITCH_Z_SHIFT = 22u;
static const uint32_t T3D_WORLD_TERRAIN_STITCH_Z_MASK = 0x00c00000u;
static const uint32_t T3D_WORLD_TERRAIN_PART_CODE_SHIFT = 24u;
static const uint32_t T3D_WORLD_TERRAIN_PART_CODE_MASK = 0x07000000u;

// Canonical static world vertex. Position remains float32. Normal/tangent and
// UV pairs are explicitly packed by the cold cooker; no renderer-specific
// vertex layout is part of this ABI.
typedef struct alignas( 16 ) t3dWorldVertex_s {
	float m_fPosition[3];
	uint32_t m_nPackedNormal;
	uint32_t m_nPackedTangent;
	uint32_t m_nPackedUv0;
	uint32_t m_nPackedUv1;
	uint32_t m_nColorRgba8;
} t3dWorldVertex_t;

// packedCounts: low16 vertexCount, high16 triangleCount.
// packedLodGeometryKind: low16 lod, high16 t3dWorldGeometryKind_t.
typedef struct alignas( 16 ) t3dWorldCluster_s {
	float m_fCenter[3];
	float m_fRadius;
	float m_fConeAxis[3];
	float m_fConeCutoff;
	uint32_t m_nVertexIndexOffset;
	uint32_t m_nTriangleOffset;
	uint32_t m_nPackedCounts;
	uint32_t m_nPackedLodGeometryKind;
	uint32_t m_nMaterial;
	uint32_t m_nFlags;
	uint32_t m_nSourceId;
	uint32_t m_nReserved;
} t3dWorldCluster_t;

// ABI v2 procedural terrain strip cluster. The fixed 48-byte footprint is
// retained, but topology vertex/index offsets are gone from the canonical
// record. Grid coordinates are reconstructed from gl_VertexIndex.
typedef struct alignas( 16 ) t3dWorldTerrainCluster_s {
	uint32_t m_nPatchId;
	uint32_t m_nPackedGridOrigin;
	uint32_t m_nPackedGridExtent;
	uint32_t m_nPackedVertexCountLod;
	uint32_t m_nFlags;
	float m_fOriginX;
	float m_fOriginZ;
	float m_fScale;
	uint32_t m_nMaterialContext;
	uint32_t m_nReserved0;
	uint32_t m_nReserved1;
	uint32_t m_nReserved2;
} t3dWorldTerrainCluster_t;

// One affine 3x4 transform. Current and previous history use separate arrays
// of the same ABI so previous-transform state is never renderer-specific.
typedef struct alignas( 16 ) t3dWorldTransform_s {
	float m_fRow0[4];
	float m_fRow1[4];
	float m_fRow2[4];
} t3dWorldTransform_t;

typedef struct alignas( 16 ) t3dWorldVisibleItem_s {
	uint32_t m_nClusterOrTerrainId;
	uint32_t m_nInstanceOrPatchId;
	uint32_t m_nGeometryKind;
	uint32_t m_nFlags;
} t3dWorldVisibleItem_t;

typedef struct alignas( 16 ) t3dWorldDrawIndirectArgs_s {
	uint32_t m_nVertexCount;
	uint32_t m_nInstanceCount;
	uint32_t m_nFirstVertex;
	uint32_t m_nFirstInstance;
} t3dWorldDrawIndirectArgs_t;

// Cold-cook VT metadata kept separate from the 64-byte raster cluster ABI.
typedef struct alignas( 16 ) t3dWorldClusterVtMetadata_s {
	uint32_t m_nPackedUvMin;
	uint32_t m_nPackedUvMax;
	uint32_t m_nTexelDensity;
	uint32_t m_nReserved;
} t3dWorldClusterVtMetadata_t;

typedef struct alignas( 16 ) t3dWorldMeshClusterSpan_s {
	uint32_t m_nVertexOffset;
	uint32_t m_nVertexCount;
	uint32_t m_nClusterOffset;
	uint32_t m_nClusterCount;
} t3dWorldMeshClusterSpan_t;

// GPU-visible per-view world frame. Matrices and planes deliberately use only
// 16-byte groups so the C++ layout mirrors std430 without backend-specific
// padding rules.
typedef struct alignas( 16 ) t3dWorldFrame_s {
	float m_fViewProjection[16];
	float m_fPreviousViewProjection[16];
	float m_fFrustumPlanes[24];
	float m_fCameraAndMaxDistance[4];
	float m_fViewportAndInvSize[4];
	float m_fMotionJitter[4];
	uint32_t m_nClusterCount;
	uint32_t m_nTerrainClusterCount;
	uint32_t m_nTransformCount;
	uint32_t m_nMaterialCount;
	uint32_t m_nVisibleCapacity;
	uint32_t m_nPassKind;
	uint32_t m_nViewIndex;
	uint32_t m_nFrameId;
	uint32_t m_nPublicationGeneration;
	uint32_t m_nGeometryGeneration;
	uint32_t m_nMaterialGeneration;
	uint32_t m_nVtGeneration;
	uint32_t m_nHiZWidth;
	uint32_t m_nHiZHeight;
	uint32_t m_nHiZMipCount;
	uint32_t m_nFlags;
} t3dWorldFrame_t;

// Cold-established resource limits. Byte budgets are 64-bit CPU policy values;
// this record is not uploaded as a shader block.
typedef struct alignas( 16 ) t3dWorldResourceLimits_s {
	uint32_t m_nMaxSourceVertices;
	uint32_t m_nMaxClusters;
	uint32_t m_nMaxTerrainClusters;
	uint32_t m_nMaxTransforms;
	uint32_t m_nMaxVisibleItems;
	uint32_t m_nMaxMaterials;
	uint32_t m_nMaxClusterVertexIndices;
	uint32_t m_nMaxTriangleSlots;
	uint64_t m_nGeometryHeapBytes;
	uint64_t m_nTriangleHeapBytes;
	uint64_t m_nVisibilityBytes;
	uint64_t m_nWorldUploadBytesPerFrame;
	uint64_t m_nPublicationScratchBytes;
	uint32_t m_nHiZMaxWidth;
	uint32_t m_nHiZMaxHeight;
	uint32_t m_nHiZMaxMipCount;
	uint32_t m_nVtPageRequestBudget;
	uint32_t m_nVtPageAdmissionBudget;
	uint32_t m_nReserved0;
} t3dWorldResourceLimits_t;

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
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_0_VERTEX_COUNT ) return 0u;
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_1_VERTEX_COUNT ) return 1u;
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_2_VERTEX_COUNT ) return 2u;
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_3_VERTEX_COUNT ) return 3u;
	if ( nActualVertexCount <= T3D_WORLD_TERRAIN_STRIP_BUCKET_4_VERTEX_COUNT ) return 4u;
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

static_assert( sizeof( t3dWorldClusterVtMetadata_t ) == 16u, "World cluster VT metadata ABI changed" );
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
