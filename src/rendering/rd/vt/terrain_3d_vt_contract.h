// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_CONTRACT_H
#define TERRAIN3D_VT_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

// Terrain3D Sparse VT V3 is the frozen shipping package ABI.
// Filter/compressor ABI revisions are explicit cache-invalidating contracts.
#define T3D_VT_FILE_MAGIC 0x56543354u
#define T3D_VT_FILE_VERSION_V3 3u
#define T3D_VT_FILE_VERSION T3D_VT_FILE_VERSION_V3
#define T3D_VT_V3_ABI_VERSION 3u
#define T3D_VT_V3_FILTER_ABI_VERSION 1u
#define T3D_VT_V3_COMPRESSOR_ABI_VERSION 1u
static_assert( T3D_VT_FILE_VERSION_V3 == 3u, "Sparse VT V3 file ABI version is frozen" );
static_assert( T3D_VT_V3_ABI_VERSION == 3u, "Sparse VT V3 ABI version is frozen" );
static_assert( T3D_VT_V3_FILTER_ABI_VERSION == 1u, "Sparse VT V3 filter ABI version is frozen" );
static_assert( T3D_VT_V3_COMPRESSOR_ABI_VERSION == 1u, "Sparse VT V3 compressor ABI version is frozen" );
#define T3D_VT_PAGE_INTERIOR 128u
#define T3D_VT_PAGE_GUTTER 4u
#define T3D_VT_PHYSICAL_TILE 136u
#define T3D_VT_MAX_ANISOTROPY 4u
#define T3D_VT_CHANNEL_COUNT 4u
#define T3D_VT_LOGICAL_CHANNEL_COUNT 5u
#define T3D_VT_QUALITY_PROFILE_COUNT 6u
#define T3D_VT_CHANNEL_MASK_ALL ( ( 1u << T3D_VT_CHANNEL_COUNT ) - 1u )
#define T3D_VT_LOGICAL_CHANNEL_MASK_ALL ( ( 1u << T3D_VT_LOGICAL_CHANNEL_COUNT ) - 1u )
#define T3D_VT_INVALID_PAGE 0xffffffffu
#define T3D_VT_INVALID_SLOT 0xffffffffu
#define T3D_VT_INVALID_ALLOCATION 0xffffffffu
#define T3D_VT_INVALID_TAIL 0xffffffffu
#define T3D_VT_MAX_MIPS 16u
#define T3D_VT_ANCESTOR_PROBE_CAP 16u
#define T3D_VT_TAIL_TILE 16u
#define T3D_VT_TAIL_ATLAS_WIDTH 2048u
#define T3D_VT_TAIL_TILES_PER_ROW ( T3D_VT_TAIL_ATLAS_WIDTH / T3D_VT_TAIL_TILE )
// Sparse V3 cooks the compact tail mip chain horizontally inside the 128 texel
// page interior. Runtime preserves that layout verbatim so BC-compressed tail
// payloads never need to be decoded/re-encoded merely to expose lower mips.
#define T3D_VT_PACK_TAIL_SLOT_WIDTH 128u
#define T3D_VT_PACK_TAIL_SLOT_HEIGHT 64u
#define T3D_VT_PACK_TAILS_PER_ROW ( T3D_VT_TAIL_ATLAS_WIDTH / T3D_VT_PACK_TAIL_SLOT_WIDTH )
#define T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY 1024u

// Stable-key namespace reserved for Terrain3D logical terrain material layers.
// Instance rendering-material keys are content hashes and must not occupy this
// namespace; the Godot cooker rejects such a collision when collecting assets.
#define T3D_VT_TERRAIN_MATERIAL_KEY_TAG 0x5433445400000000ULL
#define T3D_VT_TERRAIN_MATERIAL_KEY_MASK 0xffffffff00000000ULL

static inline uint64_t t3d_vt_terrain_material_stable_key( const uint32_t nLogicalMaterialId ) {
	return T3D_VT_TERRAIN_MATERIAL_KEY_TAG | ( uint64_t )( nLogicalMaterialId + 1u );
}

static inline bool t3d_vt_decode_terrain_material_stable_key( const uint64_t nStableKey, uint32_t &rLogicalMaterialId ) {
	if ( ( nStableKey & T3D_VT_TERRAIN_MATERIAL_KEY_MASK ) != T3D_VT_TERRAIN_MATERIAL_KEY_TAG ) {
		return false;
	}
	const uint32_t nEncoded = ( uint32_t )( nStableKey & 0xffffffffULL );
	if ( nEncoded == 0u ) {
		return false;
	}
	rLogicalMaterialId = nEncoded - 1u;
	return true;
}

// SH-B frozen shared shader descriptor contract. These constants mirror the
// literal GLSL layout declarations; the literals stay in GLSL so SH-A's
// descriptor reflection remains authoritative and can detect drift.
#define T3D_VT_SAMPLE_DESCRIPTOR_SET 0u
#define T3D_VT_SAMPLE_ALLOCATION_BINDING 8u
#define T3D_VT_SAMPLE_PAGE_TABLE_BINDING 9u
#define T3D_VT_SAMPLE_ALBEDO_BANK_BINDING_BASE 10u
#define T3D_VT_SAMPLE_NORMAL_BANK_BINDING_BASE 26u
#define T3D_VT_SAMPLE_SURFACE_BANK_BINDING_BASE 42u
#define T3D_VT_SAMPLE_HEIGHT_BANK_BINDING_BASE 58u
#define T3D_VT_SAMPLE_TAIL_METADATA_BINDING 74u
#define T3D_VT_SAMPLE_ALBEDO_TAIL_BINDING 75u
#define T3D_VT_SAMPLE_NORMAL_TAIL_BINDING 76u
#define T3D_VT_SAMPLE_SURFACE_TAIL_BINDING 77u
#define T3D_VT_SAMPLE_HEIGHT_TAIL_BINDING 78u

#define T3D_VT_FRAGMENT_FEEDBACK_CONTRACT_VERSION 1u
#define T3D_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET 3u
#define T3D_VT_FRAGMENT_FEEDBACK_ALLOCATION_BINDING 0u
#define T3D_VT_FRAGMENT_FEEDBACK_PAGE_TABLE_BINDING 1u
#define T3D_VT_FRAGMENT_FEEDBACK_HASH_BINDING 2u
#define T3D_VT_FRAGMENT_FEEDBACK_COUNTER_BINDING 3u
#define T3D_VT_FRAGMENT_FEEDBACK_PARAMS_BINDING 4u
#define T3D_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_COUNT 5u

#define T3D_VT_MATERIAL_METALLIC_SHIFT 0u
#define T3D_VT_MATERIAL_ROUGHNESS_SHIFT 8u
#define T3D_VT_MATERIAL_AO_SHIFT 16u
#define T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT 24u
#define T3D_VT_MATERIAL_UNORM8_MASK 0xffu
#define T3D_VT_MATERIAL_NORMAL_SCALE_MASK 0xffffu
#define T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT 16u
#define T3D_VT_MATERIAL_SURFACE_LAYOUT_MASK ( 0x7u << T3D_VT_MATERIAL_SURFACE_LAYOUT_SHIFT )
#define T3D_VT_MATERIAL_FILTER_SHIFT 19u
#define T3D_VT_MATERIAL_FILTER_MASK ( 0x7u << T3D_VT_MATERIAL_FILTER_SHIFT )
#define T3D_VT_MATERIAL_EMISSION_FORMAT_RGB9E5 1u

// Missing material channels are cooked to these semantic samples. Scalar
// material parameters remain authoritative for roughness/metallic/AO defaults.
#define T3D_VT_NEUTRAL_ALBEDO_R8 255u
#define T3D_VT_NEUTRAL_ALBEDO_G8 255u
#define T3D_VT_NEUTRAL_ALBEDO_B8 255u
#define T3D_VT_NEUTRAL_ALBEDO_A8 255u
#define T3D_VT_NEUTRAL_NORMAL_X8 128u
#define T3D_VT_NEUTRAL_NORMAL_Y8 128u
#define T3D_VT_NEUTRAL_SURFACE_AO8 255u
#define T3D_VT_NEUTRAL_SURFACE_ROUGHNESS8 255u
#define T3D_VT_NEUTRAL_SURFACE_METALLIC8 255u
#define T3D_VT_NEUTRAL_SURFACE_ALPHA8 255u
#define T3D_VT_NEUTRAL_HEIGHT8 128u

typedef enum t3dVTChannel_e : uint32_t {
	T3D_VT_CHANNEL_ALBEDO = 0u,
	T3D_VT_CHANNEL_NORMAL = 1u,
	T3D_VT_CHANNEL_SURFACE = 2u,
	T3D_VT_CHANNEL_HEIGHT = 3u,
} t3dVTChannel_t;

typedef enum t3dVTChannelFormat_e : uint32_t {
	T3D_VT_FORMAT_ALBEDO_BC1_SRGB = 0u,
	T3D_VT_FORMAT_NORMAL_BC5_UNORM = 1u,
	T3D_VT_FORMAT_SURFACE_BC7_UNORM = 2u,
	T3D_VT_FORMAT_HEIGHT_R8_UNORM = 3u,
} t3dVTChannelFormat_t;

// V3 logical channels are semantic/format pools. Terrain clipmaps consume
// ALBEDO/PBR/(HEIGHT); instances consume ALBEDO/(NORMAL)/(ORM).
typedef enum t3dVTLogicalChannel_e : uint32_t {
	T3D_VT_LOGICAL_CHANNEL_ALBEDO = 0u,
	T3D_VT_LOGICAL_CHANNEL_PBR = 1u,
	T3D_VT_LOGICAL_CHANNEL_HEIGHT = 2u,
	T3D_VT_LOGICAL_CHANNEL_NORMAL = 3u,
	T3D_VT_LOGICAL_CHANNEL_ORM = 4u,
} t3dVTLogicalChannel_t;

typedef enum t3dVTLogicalChannelFormat_e : uint32_t {
	T3D_VT_LOGICAL_FORMAT_ALBEDO_BC1_SRGB = 0u,
	T3D_VT_LOGICAL_FORMAT_PBR_BC7_UNORM = 1u,
	T3D_VT_LOGICAL_FORMAT_HEIGHT_R8_UNORM = 2u,
	T3D_VT_LOGICAL_FORMAT_NORMAL_BC5_UNORM = 3u,
	T3D_VT_LOGICAL_FORMAT_ORM_BC1_UNORM = 4u,
} t3dVTLogicalChannelFormat_t;

// V3 physical residency is format-family based. Logical ALBEDO and ORM both
// occupy the BC1 UNORM family; the shader applies semantic decode from the
// logical channel rather than from the physical bank.
typedef enum t3dVTPhysicalFormatFamily_e : uint32_t {
	T3D_VT_PHYSICAL_FORMAT_BC1_UNORM = 0u,
	T3D_VT_PHYSICAL_FORMAT_BC7_UNORM = 1u,
	T3D_VT_PHYSICAL_FORMAT_BC5_UNORM = 2u,
	T3D_VT_PHYSICAL_FORMAT_R8_UNORM = 3u,
	T3D_VT_PHYSICAL_FORMAT_COUNT = 4u,
} t3dVTPhysicalFormatFamily_t;

typedef enum t3dVTChannelSemantic_e : uint32_t {
	T3D_VT_CHANNEL_SEMANTIC_SRGB_ALBEDO = 0u,
	T3D_VT_CHANNEL_SEMANTIC_TERRAIN_PBR = 1u,
	T3D_VT_CHANNEL_SEMANTIC_LINEAR_HEIGHT = 2u,
	T3D_VT_CHANNEL_SEMANTIC_NORMAL_XY = 3u,
	T3D_VT_CHANNEL_SEMANTIC_LINEAR_ORM = 4u,
} t3dVTChannelSemantic_t;

// V3 runtime page-table address. The top two bits select one of the four
// global physical format-family arrays and the low 30 bits select its layer.
#define T3D_VT_V3_PHYSICAL_FAMILY_SHIFT 30u
#define T3D_VT_V3_PHYSICAL_FAMILY_MASK 0xc0000000u
#define T3D_VT_V3_PHYSICAL_SLOT_MASK 0x3fffffffu

static inline uint32_t t3d_vt_pack_physical_address_v3( const t3dVTPhysicalFormatFamily_t Family, const uint32_t nSlot ) {
	if ( ( uint32_t )Family >= T3D_VT_PHYSICAL_FORMAT_COUNT || nSlot > T3D_VT_V3_PHYSICAL_SLOT_MASK ) {
		return T3D_VT_INVALID_SLOT;
	}
	return ( ( uint32_t )Family << T3D_VT_V3_PHYSICAL_FAMILY_SHIFT ) | nSlot;
}

static inline t3dVTPhysicalFormatFamily_t t3d_vt_physical_address_family_v3( const uint32_t nAddress ) {
	return ( t3dVTPhysicalFormatFamily_t )( ( nAddress & T3D_VT_V3_PHYSICAL_FAMILY_MASK ) >> T3D_VT_V3_PHYSICAL_FAMILY_SHIFT );
}

static inline uint32_t t3d_vt_physical_address_slot_v3( const uint32_t nAddress ) {
	return nAddress & T3D_VT_V3_PHYSICAL_SLOT_MASK;
}

// The current RenderWorld owns four physical arrays already. P04 reinterprets
// them by format family without increasing descriptor count: BC1=albedo array,
// BC7=surface array, BC5=normal array, R8=height array.
static inline uint32_t t3d_vt_physical_family_legacy_resource_channel( const t3dVTPhysicalFormatFamily_t Family ) {
	switch ( Family ) {
		case T3D_VT_PHYSICAL_FORMAT_BC1_UNORM:
			return T3D_VT_CHANNEL_ALBEDO;
		case T3D_VT_PHYSICAL_FORMAT_BC7_UNORM:
			return T3D_VT_CHANNEL_SURFACE;
		case T3D_VT_PHYSICAL_FORMAT_BC5_UNORM:
			return T3D_VT_CHANNEL_NORMAL;
		case T3D_VT_PHYSICAL_FORMAT_R8_UNORM:
			return T3D_VT_CHANNEL_HEIGHT;
		default:
			return T3D_VT_INVALID_SLOT;
	}
}

static inline t3dVTPhysicalFormatFamily_t t3d_vt_logical_channel_physical_family( const t3dVTLogicalChannel_t Channel ) {
	switch ( Channel ) {
		case T3D_VT_LOGICAL_CHANNEL_ALBEDO:
		case T3D_VT_LOGICAL_CHANNEL_ORM:
			return T3D_VT_PHYSICAL_FORMAT_BC1_UNORM;
		case T3D_VT_LOGICAL_CHANNEL_PBR:
			return T3D_VT_PHYSICAL_FORMAT_BC7_UNORM;
		case T3D_VT_LOGICAL_CHANNEL_NORMAL:
			return T3D_VT_PHYSICAL_FORMAT_BC5_UNORM;
		case T3D_VT_LOGICAL_CHANNEL_HEIGHT:
		default:
			return T3D_VT_PHYSICAL_FORMAT_R8_UNORM;
	}
}

static inline t3dVTChannelSemantic_t t3d_vt_logical_channel_semantic( const t3dVTLogicalChannel_t Channel ) {
	switch ( Channel ) {
		case T3D_VT_LOGICAL_CHANNEL_ALBEDO:
			return T3D_VT_CHANNEL_SEMANTIC_SRGB_ALBEDO;
		case T3D_VT_LOGICAL_CHANNEL_PBR:
			return T3D_VT_CHANNEL_SEMANTIC_TERRAIN_PBR;
		case T3D_VT_LOGICAL_CHANNEL_HEIGHT:
			return T3D_VT_CHANNEL_SEMANTIC_LINEAR_HEIGHT;
		case T3D_VT_LOGICAL_CHANNEL_NORMAL:
			return T3D_VT_CHANNEL_SEMANTIC_NORMAL_XY;
		case T3D_VT_LOGICAL_CHANNEL_ORM:
		default:
			return T3D_VT_CHANNEL_SEMANTIC_LINEAR_ORM;
	}
}

typedef enum t3dVTConsumer_e : uint32_t {
	T3D_VT_CONSUMER_CLIPMAP = 0u,
	T3D_VT_CONSUMER_INSTANCE = 1u,
} t3dVTConsumer_t;

// VT-P06 runtime adapter: one logical V3 material allocation is flattened to
// three independent residency allocations. The meaning of slots 1/2 depends
// on the consumer; slot 0 is ALBEDO for both.
#define T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION 3u

static inline bool t3d_vt_v3_runtime_slot_for_channel( const t3dVTConsumer_t Consumer, const t3dVTLogicalChannel_t Channel, uint32_t &rSlot ) {
	if ( Channel == T3D_VT_LOGICAL_CHANNEL_ALBEDO ) { rSlot = 0u; return true; }
	if ( Consumer == T3D_VT_CONSUMER_CLIPMAP ) {
		if ( Channel == T3D_VT_LOGICAL_CHANNEL_PBR ) { rSlot = 1u; return true; }
		if ( Channel == T3D_VT_LOGICAL_CHANNEL_HEIGHT ) { rSlot = 2u; return true; }
	} else if ( Consumer == T3D_VT_CONSUMER_INSTANCE ) {
		if ( Channel == T3D_VT_LOGICAL_CHANNEL_NORMAL ) { rSlot = 1u; return true; }
		if ( Channel == T3D_VT_LOGICAL_CHANNEL_ORM ) { rSlot = 2u; return true; }
	}
	rSlot = T3D_VT_INVALID_SLOT;
	return false;
}

static inline uint32_t t3d_vt_v3_runtime_allocation_base( const uint32_t nAllocationId ) {
	return nAllocationId * T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION;
}

static inline bool t3d_vt_v3_logical_channel_for_runtime_slot( const t3dVTConsumer_t Consumer, const uint32_t nSlot, t3dVTLogicalChannel_t &rChannel ) {
	if ( nSlot == 0u ) { rChannel = T3D_VT_LOGICAL_CHANNEL_ALBEDO; return true; }
	if ( Consumer == T3D_VT_CONSUMER_CLIPMAP ) {
		if ( nSlot == 1u ) { rChannel = T3D_VT_LOGICAL_CHANNEL_PBR; return true; }
		if ( nSlot == 2u ) { rChannel = T3D_VT_LOGICAL_CHANNEL_HEIGHT; return true; }
	} else if ( Consumer == T3D_VT_CONSUMER_INSTANCE ) {
		if ( nSlot == 1u ) { rChannel = T3D_VT_LOGICAL_CHANNEL_NORMAL; return true; }
		if ( nSlot == 2u ) { rChannel = T3D_VT_LOGICAL_CHANNEL_ORM; return true; }
	}
	return false;
}

#define T3D_VT_LOGICAL_CHANNEL_BIT( channel ) ( 1u << ( uint32_t )( channel ) )
#define T3D_VT_CLIPMAP_ALLOWED_CHANNEL_MASK                                                                                                \
	( T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO ) | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_PBR )               \
	        | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_HEIGHT ) )
#define T3D_VT_INSTANCE_ALLOWED_CHANNEL_MASK                                                                                               \
	( T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO ) | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_NORMAL )            \
	        | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ORM ) )

static inline t3dVTLogicalChannelFormat_t t3d_vt_logical_channel_format( const t3dVTLogicalChannel_t Channel ) {
	switch ( Channel ) {
		case T3D_VT_LOGICAL_CHANNEL_ALBEDO:
			return T3D_VT_LOGICAL_FORMAT_ALBEDO_BC1_SRGB;
		case T3D_VT_LOGICAL_CHANNEL_PBR:
			return T3D_VT_LOGICAL_FORMAT_PBR_BC7_UNORM;
		case T3D_VT_LOGICAL_CHANNEL_HEIGHT:
			return T3D_VT_LOGICAL_FORMAT_HEIGHT_R8_UNORM;
		case T3D_VT_LOGICAL_CHANNEL_NORMAL:
			return T3D_VT_LOGICAL_FORMAT_NORMAL_BC5_UNORM;
		case T3D_VT_LOGICAL_CHANNEL_ORM:
		default:
			return T3D_VT_LOGICAL_FORMAT_ORM_BC1_UNORM;
	}
}


typedef enum t3dVTSamplerClass_e : uint32_t {
	T3D_VT_SAMPLER_LINEAR_REPEAT = 0u,
	T3D_VT_SAMPLER_LINEAR_CLAMP = 1u,
	T3D_VT_SAMPLER_NEAREST_REPEAT = 2u,
	T3D_VT_SAMPLER_NEAREST_CLAMP = 3u,
} t3dVTSamplerClass_t;

typedef enum t3dVTTextureFilter_e : uint32_t {
	T3D_VT_FILTER_NEAREST = 0u,
	T3D_VT_FILTER_LINEAR = 1u,
	T3D_VT_FILTER_NEAREST_MIPMAP = 2u,
	T3D_VT_FILTER_LINEAR_MIPMAP = 3u,
	T3D_VT_FILTER_NEAREST_MIPMAP_ANISOTROPIC = 4u,
	T3D_VT_FILTER_LINEAR_MIPMAP_ANISOTROPIC = 5u,
} t3dVTTextureFilter_t;

typedef enum t3dVTAllocationFlags_e : uint16_t {
	T3D_VT_ALLOCATION_MATERIAL_UV = 1u << 0u,
	T3D_VT_ALLOCATION_MEGATEXTURE = 1u << 1u,
	T3D_VT_ALLOCATION_REPEAT = 1u << 2u,
	T3D_VT_ALLOCATION_CLAMP = 1u << 3u,
	T3D_VT_ALLOCATION_HAS_ALPHA = 1u << 4u,
	T3D_VT_ALLOCATION_HAS_HEIGHT = 1u << 5u,
	T3D_VT_ALLOCATION_PINNED = 1u << 6u,
	T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE = 1u << 7u,
	T3D_VT_ALLOCATION_EDITOR_DYNAMIC = 1u << 8u,
	// VT-P07 runtime-only view selection. Serialized authoring flags keep these bits clear.
	T3D_VT_ALLOCATION_QUALITY_BASE_MIP_SHIFT = 9u,
	T3D_VT_ALLOCATION_QUALITY_BASE_MIP_MASK = 0x0fu << T3D_VT_ALLOCATION_QUALITY_BASE_MIP_SHIFT,
	T3D_VT_ALLOCATION_QUALITY_DISABLED = 1u << 13u,
} t3dVTAllocationFlags_t;

#define T3D_VT_ALLOCATION_RUNTIME_QUALITY_MASK \
	( T3D_VT_ALLOCATION_QUALITY_BASE_MIP_MASK | T3D_VT_ALLOCATION_QUALITY_DISABLED )

static inline uint32_t t3d_vt_allocation_quality_base_mip( const uint16_t nFlags ) {
	return ( ( uint32_t )nFlags & T3D_VT_ALLOCATION_QUALITY_BASE_MIP_MASK ) >> T3D_VT_ALLOCATION_QUALITY_BASE_MIP_SHIFT;
}

static inline bool t3d_vt_allocation_quality_enabled( const uint16_t nFlags ) {
	return ( nFlags & T3D_VT_ALLOCATION_QUALITY_DISABLED ) == 0u;
}

typedef enum t3dVTPageFlags_e : uint16_t {
	T3D_VT_PAGE_VALID = 1u << 0u,
	T3D_VT_PAGE_RESIDENT = 1u << 1u,
	T3D_VT_PAGE_PINNED = 1u << 2u,
	T3D_VT_PAGE_NEUTRAL = 1u << 3u,
	T3D_VT_PAGE_ROOT = 1u << 4u,
	T3D_VT_PAGE_TAIL_FALLBACK = 1u << 5u,
} t3dVTPageFlags_t;


typedef enum t3dVTResolveFlags_e : uint16_t {
	T3D_VT_RESOLVE_PHYSICAL = 1u << 0u,
	T3D_VT_RESOLVE_ANCESTOR = 1u << 1u,
	T3D_VT_RESOLVE_TAIL = 1u << 2u,
} t3dVTResolveFlags_t;

typedef enum t3dVTPhysicalSlotFlags_e : uint16_t {
	T3D_VT_SLOT_PINNED = 1u << 0u,
	T3D_VT_SLOT_PENDING = 1u << 1u,
	T3D_VT_SLOT_REFERENCED = 1u << 2u,
	T3D_VT_SLOT_HOT = 1u << 3u,
	T3D_VT_SLOT_RETIRED = 1u << 4u,
} t3dVTPhysicalSlotFlags_t;

typedef enum t3dRenderingMaterialFlags_e : uint32_t {
	T3D_VT_MATERIAL_ALPHA_SCISSOR = 1u << 0u,
	T3D_VT_MATERIAL_FOLIAGE = 1u << 1u,
	T3D_VT_MATERIAL_CAST_SHADOWS = 1u << 2u,
	T3D_VT_MATERIAL_RECEIVE_SHADOWS = 1u << 3u,
	T3D_VT_MATERIAL_TWO_SIDED = 1u << 4u,
	T3D_VT_MATERIAL_UNSHADED = 1u << 5u,
	T3D_VT_MATERIAL_VERTEX_COLOR = 1u << 6u,
	T3D_VT_MATERIAL_VERTEX_COLOR_SRGB = 1u << 7u,
	T3D_VT_MATERIAL_NORMAL_MAPPING = 1u << 8u,
	T3D_VT_MATERIAL_AO_ENABLED = 1u << 9u,
	T3D_VT_MATERIAL_HAS_HEIGHT = 1u << 10u,
	T3D_VT_MATERIAL_HAS_ALPHA = 1u << 11u,
	// Imported custom ShaderMaterial profile: ORM channels are authored as
	// absolute AO/roughness/metallic values rather than scalar multipliers.
	T3D_VT_MATERIAL_DIRECT_ORM = 1u << 12u,
	// m_nReserved low 16 bits hold albedo saturation in [0, 1.5].
	T3D_VT_MATERIAL_ALBEDO_SATURATION = 1u << 13u,
	// Known shared ShaderMaterial profile uses the authored stylized sun loop.
	T3D_VT_MATERIAL_STYLIZED_SUN = 1u << 14u,
	// VT-P06: material allocation is the base of a sparse V3 runtime channel triplet.
	T3D_VT_MATERIAL_V3_LAYOUT = 1u << 15u,
} t3dRenderingMaterialFlags_t;

// CPU fields at offsets 8/10 and 12/14 are consumed by GLSL as two packed
// uints: packedMip0Extent and packedMipCountFlags respectively.
typedef struct t3dVTAllocation_s {
	uint32_t m_nBaseVirtualPage;
	uint32_t m_nPageCount;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nFlags;
	uint32_t m_nTailIndex;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint32_t m_nReserved;
} t3dVTAllocation_t;

// V3 uses one record per allocation/logical-channel pair. Index is
// allocation_id * T3D_VT_LOGICAL_CHANNEL_COUNT + logical_channel.
typedef struct t3dVTChannelAllocation_s {
	uint32_t m_nBaseVirtualPage;
	uint32_t m_nPageCount;
	uint32_t m_nTailIndex;
	uint16_t m_nMip0WidthPages;
	uint16_t m_nMip0HeightPages;
	uint16_t m_nMipCount;
	uint16_t m_nFlags;
} t3dVTChannelAllocation_t;

#define T3D_VT_PACK_INVALID_MIP 0xffu

// Native V3 serialized records shared by the pack parser and runtime. The
// channel-allocation directory remains a fixed allocation x logical-channel
// matrix; absent channels carry a zero record and allocate no pages/tail.
typedef struct t3dVTPackAllocationRecordV3_s {
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint16_t m_nFlags;
	uint16_t m_nChannelMask;
	uint8_t m_nHighestAvailableProfile;
	uint8_t m_nAvailableProfileMask;
	uint8_t m_nConsumer;
	uint8_t m_nReserved0;
} t3dVTPackAllocationRecordV3_t;

typedef struct t3dVTPackPageRecordV3_s {
	uint32_t m_nVirtualPage;
	uint32_t m_nParentPage;
	uint32_t m_nAllocationId;
	uint32_t m_nPayloadId;
	uint32_t m_nGeneration;
	uint32_t m_nContentId;
	uint16_t m_nMip;
	uint16_t m_nFlags;
	uint16_t m_nPageX;
	uint16_t m_nPageY;
	uint16_t m_nChannel;
	uint16_t m_nReserved;
} t3dVTPackPageRecordV3_t;

typedef struct t3dVTPackTailRecordV3_s {
	uint64_t m_nContentSignature;
	uint32_t m_nCanonicalAllocationId;
	uint32_t m_nPayloadId;
	uint32_t m_nContentId;
	uint16_t m_nFirstMip;
	uint16_t m_nMipCount;
	uint16_t m_nFirstMipWidth;
	uint16_t m_nFirstMipHeight;
	uint16_t m_nAtlasInteriorWidth;
	uint16_t m_nAtlasInteriorHeight;
	uint16_t m_nChannel;
	uint16_t m_nFlags;
	uint32_t m_nLayout;
} t3dVTPackTailRecordV3_t;

typedef struct t3dVTPackQualityViewV3_s {
	uint32_t m_nAllocationId;
	uint8_t m_nProfile;
	uint8_t m_nChannelMask;
	uint8_t m_nBaseMip[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint8_t m_nMaxMip[T3D_VT_LOGICAL_CHANNEL_COUNT];
} t3dVTPackQualityViewV3_t;

typedef struct t3dVTPackFormatFamilyRecordV3_s {
	uint8_t m_nLogicalChannel;
	uint8_t m_nPhysicalFamily;
	uint8_t m_nSemantic;
	uint8_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTPackFormatFamilyRecordV3_t;

// CPU fields at offsets 12/14 are consumed by GLSL as packedResolvedMipFlags.
typedef struct t3dVTPageEntry_s {
	uint32_t m_nPhysicalSlot;
	uint32_t m_nParentPage;
	uint32_t m_nGeneration;
	uint16_t m_nResolvedMip;
	uint16_t m_nFlags;
} t3dVTPageEntry_t;

// GPU-only precomputed allocation mip layout. This is deliberately separate
// from the frozen 32-byte serialized allocation ABI. One allocation owns
// T3D_VT_MAX_MIPS contiguous entries; unused entries are zeroed.
typedef struct t3dVTMipLayout_s {
	uint32_t m_nBaseVirtualPage;
	uint16_t m_nWidthPages;
	uint16_t m_nHeightPages;
} t3dVTMipLayout_t;

// One tail metadata record addresses the same compact atlas rectangle in all
// four channel atlases. ChannelMask identifies authored payloads; absent
// channels resolve to the frozen neutral semantics without allocating pages.
typedef struct t3dVTTailMetadata_s {
	uint32_t m_nAtlasX;
	uint32_t m_nAtlasY;
	uint16_t m_nWidthTexels;
	uint16_t m_nHeightTexels;
	uint16_t m_nFirstMip;
	uint16_t m_nMipCount;
	uint32_t m_nGeneration;
	uint32_t m_nChannelMask;
	uint32_t m_nContentId;
	uint32_t m_nFlags;
} t3dVTTailMetadata_t;

// Debug-visible resolve result. A physical resolution has a valid slot and an
// invalid tail index. A terminal tail resolution has the inverse.
typedef struct t3dVTResolveResult_s {
	uint32_t m_nAllocation;
	uint32_t m_nRequestedPage;
	uint32_t m_nResolvedPage;
	uint32_t m_nPhysicalSlot;
	uint32_t m_nTailIndex;
	uint16_t m_nRequestedMip;
	uint16_t m_nResolvedMip;
	uint16_t m_nFallbackDepth;
	uint16_t m_nFlags;
	uint32_t m_nReserved;
} t3dVTResolveResult_t;

// The slot number selects the same bank/layer for every material channel.
// CPU 16-bit fields at offsets 16..23 are consumed by GLSL as packed words.
typedef struct t3dVTPhysicalSlot_s {
	uint32_t m_nVirtualPage;
	uint32_t m_nGeneration;
	uint32_t m_nLastVisibleFrame;
	uint32_t m_nLastRequestedFrame;
	uint16_t m_nFlags;
	uint16_t m_nClockAge;
	uint16_t m_nPinCount;
	uint16_t m_nRetireFrame;
	uint32_t m_nPayloadId;
	uint32_t m_nReserved;
} t3dVTPhysicalSlot_t;

// CPU wantedMip/priority at offsets 4/6 are consumed by GLSL as one packed uint.
typedef struct t3dVTRequest_s {
	uint32_t m_nVirtualPage;
	uint16_t m_nWantedMip;
	uint16_t m_nPriority;
} t3dVTRequest_t;

typedef struct t3dRenderingMaterialVtBindings_s {
	uint32_t m_nVtAllocation;
	uint32_t m_nOverlayVtAllocation;
} t3dRenderingMaterialVtBindings_t;

// Fixed 64-byte material ABI. Texture identity is represented only by stable
// virtual-texture allocation IDs; source Texture2D/RID identity is cook input.
typedef struct t3dRenderingMaterialGpu_s {
	uint32_t m_nVtAllocation;
	uint32_t m_nOverlayVtAllocation;
	uint32_t m_nFlags;
	uint32_t m_nPackedScalars0;
	float m_vBaseColor[4];
	float m_vUvScaleBias[4];
	uint32_t m_nPackedEmission;
	uint32_t m_nPackedScalars1;
	uint32_t m_nSamplerClass;
	uint32_t m_nReserved;
} t3dRenderingMaterialGpu_t;

static_assert( sizeof( t3dVTAllocation_t ) == 32u, "t3dVTAllocation_t ABI changed" );
static_assert( sizeof( t3dVTChannelAllocation_t ) == 20u, "t3dVTChannelAllocation_t v3 ABI changed" );
static_assert( sizeof( t3dVTPackAllocationRecordV3_t ) == 16u, "t3dVTPackAllocationRecordV3_t ABI changed" );
static_assert( sizeof( t3dVTPackPageRecordV3_t ) == 36u, "t3dVTPackPageRecordV3_t ABI changed" );
static_assert( sizeof( t3dVTPackTailRecordV3_t ) == 40u, "t3dVTPackTailRecordV3_t ABI changed" );
static_assert( sizeof( t3dVTPackQualityViewV3_t ) == 16u, "t3dVTPackQualityViewV3_t ABI changed" );
static_assert( sizeof( t3dVTPackFormatFamilyRecordV3_t ) == 8u, "t3dVTPackFormatFamilyRecordV3_t ABI changed" );
static_assert( offsetof( t3dVTAllocation_t, m_nBaseVirtualPage ) == 0u, "VT allocation base-page ABI changed" );
static_assert( offsetof( t3dVTAllocation_t, m_nMip0WidthPages ) == 8u, "VT allocation extent ABI changed" );
static_assert( offsetof( t3dVTAllocation_t, m_nMipCount ) == 12u, "VT allocation mip ABI changed" );
static_assert( offsetof( t3dVTAllocation_t, m_nTailIndex ) == 16u, "VT allocation tail ABI changed" );
static_assert( offsetof( t3dVTAllocation_t, m_nGeneration ) == 20u, "VT allocation generation ABI changed" );
static_assert( offsetof( t3dVTAllocation_t, m_nContentId ) == 24u, "VT allocation content ABI changed" );

static_assert( sizeof( t3dVTPageEntry_t ) == 16u, "t3dVTPageEntry_t ABI changed" );
static_assert( offsetof( t3dVTPageEntry_t, m_nResolvedMip ) == 12u, "VT page-entry packed ABI changed" );
static_assert( sizeof( t3dVTMipLayout_t ) == 8u, "t3dVTMipLayout_t GPU ABI changed" );
static_assert( offsetof( t3dVTMipLayout_t, m_nWidthPages ) == 4u, "VT mip-layout packed extent ABI changed" );

static_assert( sizeof( t3dVTTailMetadata_t ) == 32u, "t3dVTTailMetadata_t ABI changed" );
static_assert( offsetof( t3dVTTailMetadata_t, m_nWidthTexels ) == 8u, "VT tail extent ABI changed" );
static_assert( offsetof( t3dVTTailMetadata_t, m_nGeneration ) == 16u, "VT tail generation ABI changed" );
static_assert( offsetof( t3dVTTailMetadata_t, m_nChannelMask ) == 20u, "VT tail channel-mask ABI changed" );

static_assert( sizeof( t3dVTResolveResult_t ) == 32u, "t3dVTResolveResult_t ABI changed" );
static_assert( offsetof( t3dVTResolveResult_t, m_nRequestedMip ) == 20u, "VT resolve debug ABI changed" );
static_assert( offsetof( t3dVTResolveResult_t, m_nResolvedMip ) == 22u, "VT resolve packed mip ABI changed" );
static_assert( offsetof( t3dVTResolveResult_t, m_nFallbackDepth ) == 24u, "VT resolve fallback ABI changed" );
static_assert( offsetof( t3dVTResolveResult_t, m_nFlags ) == 26u, "VT resolve flags ABI changed" );
static_assert( offsetof( t3dVTResolveResult_t, m_nReserved ) == 28u, "VT resolve reserved ABI changed" );

static_assert( sizeof( t3dVTPhysicalSlot_t ) == 32u, "t3dVTPhysicalSlot_t ABI changed" );
static_assert( offsetof( t3dVTPhysicalSlot_t, m_nFlags ) == 16u, "VT physical-slot flags ABI changed" );
static_assert( offsetof( t3dVTPhysicalSlot_t, m_nPayloadId ) == 24u, "VT physical-slot payload ABI changed" );

static_assert( sizeof( t3dVTRequest_t ) == 8u, "t3dVTRequest_t ABI changed" );
static_assert( offsetof( t3dVTRequest_t, m_nWantedMip ) == 4u, "VT request packed ABI changed" );

static_assert( sizeof( t3dRenderingMaterialVtBindings_t ) == 8u, "VT material binding ABI changed" );
static_assert( sizeof( t3dRenderingMaterialGpu_t ) == 64u, "VT material GPU ABI changed" );
static_assert( offsetof( t3dRenderingMaterialGpu_t, m_nVtAllocation ) == 0u, "VT material allocation ABI changed" );
static_assert( offsetof( t3dRenderingMaterialGpu_t, m_vBaseColor ) == 16u, "VT material base-color ABI changed" );
static_assert( offsetof( t3dRenderingMaterialGpu_t, m_vUvScaleBias ) == 32u, "VT material UV ABI changed" );
static_assert( offsetof( t3dRenderingMaterialGpu_t, m_nPackedEmission ) == 48u, "VT material emission ABI changed" );
static_assert( offsetof( t3dRenderingMaterialGpu_t, m_nSamplerClass ) == 56u, "VT material sampler ABI changed" );

// P21/P22 shared request-hash contract. The table is fixed-capacity and owner
// controlled. Hash entries never allocate or chain; bounded probing either
// merges the request or records an observable overflow.
#define T3D_VT_REQUEST_HASH_EMPTY_KEY 0xffffffffu
#define T3D_VT_REQUEST_HASH_PROBE_COUNT 8u
#define T3D_VT_REQUEST_HASH_SLOTS_PER_REQUEST 2u
#define T3D_VT_REQUEST_RECT_PAGE_CAP 16u

#define T3D_VT_FRAGMENT_FEEDBACK_ENABLED ( 1u << 0u )
#define T3D_VT_REQUEST_COUNTER_WORDS 16u

#define T3D_VT_REQUEST_PRODUCER_VISIBLE ( 1u << 0u )
#define T3D_VT_REQUEST_PRODUCER_FUTURE ( 1u << 1u )
#define T3D_VT_REQUEST_PRODUCER_FRAGMENT ( 1u << 2u )

#define T3D_VT_PREDICTION_CATEGORY_CLUTTER 0u
#define T3D_VT_PREDICTION_CATEGORY_GROUND_COVER 1u
#define T3D_VT_PREDICTION_CATEGORY_OCCLUDER 2u
#define T3D_VT_PREDICTION_CATEGORY_TERRAIN 3u

// Counter words 12..15 form the generation/frame header consumed with the
// compact request list. Keeping it in the same fixed block makes WG-D readback
// independent from shader-private hash state.
typedef struct t3dVTRequestGpuCounters_s {
	uint32_t m_nCompactedRequestCount;
	uint32_t m_nRawAttemptCount;
	uint32_t m_nUniqueAcceptedCount;
	uint32_t m_nSameKeyMergeCount;
	uint32_t m_nProbeOverflowCount;
	uint32_t m_nRequestListOverflowCount;
	uint32_t m_nVisiblePredictionCount;
	uint32_t m_nFuturePredictionCount;
	uint32_t m_nMipCoarsenCount;
	uint32_t m_nBaseAllocationCount;
	uint32_t m_nOverlayAllocationCount;
	uint32_t m_nFragmentMissCount;
	uint32_t m_nFrameId;
	uint32_t m_nGeneration;
	uint32_t m_nHashCapacity;
	uint32_t m_nRequestCapacity;
} t3dVTRequestGpuCounters_t;

// Descriptor-invariant raster feedback control block bound at set 3/binding 4.
// Capacities are owner-selected cold values; raster code never resizes either
// request structure. flags bit 0 enables the miss-repair producer.
typedef struct t3dVTFragmentFeedbackParams_s {
	uint32_t m_nHashCapacity;
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nFlags;
} t3dVTFragmentFeedbackParams_t;

// 16-byte open-addressing entry. wantedMip and priority intentionally occupy
// separate 32-bit atomic words: same-page merges independently retain the
// finest wanted mip (atomicMin) and highest priority (atomicMax) without 64-bit
// shader atomics. producerMask is diagnostic only.
typedef struct t3dVTRequestHashEntry_s {
	uint32_t m_nVirtualPage;
	uint32_t m_nWantedMip;
	uint32_t m_nPriority;
	uint32_t m_nProducerMask;
} t3dVTRequestHashEntry_t;

// Immutable draw-slot prediction metadata. WG-C can publish this beside its
// final draw-slot table without changing the draw ABI. P21 consumes the table
// by draw slot after GPU visibility has produced the final indirect counts.
typedef struct t3dVTPredictionMetadata_s {
	float m_vWorldSphere[4];
	float m_vUvBounds[4];
	uint32_t m_nBaseVtAllocation;
	uint32_t m_nOverlayVtAllocation;
	uint32_t m_nMaterialId;
	uint32_t m_nDrawSlot;
	float m_fTexelDensity;
	float m_fObjectScale;
	uint16_t m_nGeometryLod;
	uint8_t m_nCategory;
	int8_t m_nMipBias;
	uint32_t m_nFlags;
} t3dVTPredictionMetadata_t;

// Shared 64-byte dispatch packet. Reset/compact consume the first uvec4 only;
// predictors additionally use camera motion and immutable resource counts.
typedef struct t3dVTPredictionDispatch_s {
	uint32_t m_nHashCapacity;
	uint32_t m_nRequestCapacity;
	uint32_t m_nFrameId;
	uint32_t m_nGeneration;
	float m_vCameraLinearVelocity[3];
	float m_fPredictionHorizon;
	float m_fCameraAngularSpeed;
	float m_fViewportWidth;
	float m_fViewportHeight;
	float m_fFuturePriorityScale;
	uint32_t m_nDrawSlotCount;
	uint32_t m_nAllocationCount;
	uint32_t m_nPageCount;
	uint32_t m_nFlags;
} t3dVTPredictionDispatch_t;

static_assert( sizeof( t3dVTRequestHashEntry_t ) == 16u, "VT request hash entry ABI changed" );
static_assert( sizeof( t3dVTRequestGpuCounters_t ) == 64u, "VT request counter/header ABI changed" );
static_assert( sizeof( t3dVTFragmentFeedbackParams_t ) == 16u, "VT fragment feedback parameter ABI changed" );
static_assert( offsetof( t3dVTFragmentFeedbackParams_t, m_nHashCapacity ) == 0u, "VT fragment hash-capacity offset changed" );
static_assert( offsetof( t3dVTFragmentFeedbackParams_t, m_nAllocationCount ) == 4u, "VT fragment allocation-count offset changed" );
static_assert( offsetof( t3dVTFragmentFeedbackParams_t, m_nPageCount ) == 8u, "VT fragment page-count offset changed" );
static_assert( offsetof( t3dVTFragmentFeedbackParams_t, m_nFlags ) == 12u, "VT fragment flags offset changed" );
static_assert( offsetof( t3dVTRequestGpuCounters_t, m_nFrameId ) == 48u, "VT readback frame header offset changed" );
static_assert( sizeof( t3dVTPredictionMetadata_t ) == 64u, "VT prediction metadata ABI changed" );
static_assert( offsetof( t3dVTPredictionMetadata_t, m_nBaseVtAllocation ) == 32u, "VT prediction allocation ABI changed" );
static_assert( offsetof( t3dVTPredictionMetadata_t, m_nDrawSlot ) == 44u, "VT prediction draw-slot ABI changed" );
static_assert( offsetof( t3dVTPredictionMetadata_t, m_fTexelDensity ) == 48u, "VT prediction density ABI changed" );
static_assert( offsetof( t3dVTPredictionMetadata_t, m_nGeometryLod ) == 56u, "VT prediction packed LOD ABI changed" );
static_assert( offsetof( t3dVTPredictionMetadata_t, m_nFlags ) == 60u, "VT prediction flags ABI changed" );
static_assert( sizeof( t3dVTPredictionDispatch_t ) == 64u, "VT prediction dispatch ABI changed" );

static_assert( T3D_VT_SAMPLE_HEIGHT_TAIL_BINDING == 78u, "VT sample descriptor layout changed" );
static_assert( T3D_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET == 3u, "VT fragment feedback descriptor set changed" );
static_assert( T3D_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_COUNT == 5u, "VT fragment feedback descriptor count changed" );

#endif // TERRAIN3D_VT_CONTRACT_H
