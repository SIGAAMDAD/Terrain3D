// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_GODOT_COOKER_H
#define TERRAIN3D_VT_GODOT_COOKER_H

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_rendering_material.h"
#include "authoring/vt/terrain_3d_vt_cooker.h"
#include "core/terrain_3d_eastl_helpers.h"
#include "rendering/rd/vt/terrain_3d_vt_fixed_profile.h"

#include <godot_cpp/variant/packed_byte_array.hpp>

typedef enum t3dVTGodotSourceFlagsV3_e : uint32_t {
	T3D_VT_GODOT_SOURCE_V3_DIRECT_ORM = 1u << 0u,
	T3D_VT_GODOT_SOURCE_V3_HAS_ALPHA = 1u << 1u,
} t3dVTGodotSourceFlagsV3_t;

typedef struct t3dVTGodotSourceChannelV3_s {
	uint32_t m_nSourceWidth;
	uint32_t m_nSourceHeight;
	uint32_t m_nSourceExtent;
	uint32_t m_nReserved0;
	uint64_t m_nSourceFingerprint;
	uint8_t m_bAuthored;
	uint8_t m_nLogicalChannel;
	uint8_t m_nConsumer;
	uint8_t m_nWrapMode;
	uint8_t m_bAlphaScissor;
	uint8_t m_nReserved1[3];
} t3dVTGodotSourceChannelV3_t;

typedef struct t3dVTGodotSourceV3_s {
	godot::PackedByteArray m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTCookImageView_t m_Channels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTGodotSourceChannelV3_t m_ChannelDesc[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint64_t m_nStableKey;
	uint32_t m_nAuthoredChannelMask;
	uint32_t m_nSourceFlags;
	uint32_t m_nOrmSourceLayout;
	uint32_t m_nConsumer;
	t3dVTFixedProfile_t m_eHighestAvailableProfile;
} t3dVTGodotSourceV3_t;


typedef enum t3dVTEditorCookStageV3_e : uint32_t {
	T3D_VT_EDITOR_COOK_STAGE_IDLE = 0u,
	T3D_VT_EDITOR_COOK_STAGE_EXTRACT = 1u,
	T3D_VT_EDITOR_COOK_STAGE_FILTER = 2u,
	T3D_VT_EDITOR_COOK_STAGE_PAGE = 3u,
	T3D_VT_EDITOR_COOK_STAGE_ENCODE = 4u,
	T3D_VT_EDITOR_COOK_STAGE_PACK = 5u,
	T3D_VT_EDITOR_COOK_STAGE_COMPLETE = 6u,
} t3dVTEditorCookStageV3_t;

typedef struct t3dVTEditorCookTelemetryV3_s {
	uint32_t m_nStage;
	uint32_t m_nDirtyAllocations;
	uint32_t m_nAllocationCacheHits;
	uint32_t m_nAllocationCacheMisses;
	uint32_t m_nChannelCacheHits;
	uint32_t m_nChannelCacheMisses;
	uint64_t m_nExtractUsec;
	uint64_t m_nFilterUsec;
	uint64_t m_nPageUsec;
	uint64_t m_nEncodeUsec;
	uint64_t m_nPackUsec;
} t3dVTEditorCookTelemetryV3_t;

typedef struct t3dVTEditorChannelCacheEntryV3_s {
	uint64_t m_nKey;
	uint32_t m_nTargetExtent;
	uint32_t m_nLogicalChannel;
	t3dVTMipChainLayout_t m_Layout;
	godot::PackedByteArray m_Bytes;
} t3dVTEditorChannelCacheEntryV3_t;

typedef struct t3dVTEditorAllocationCacheEntryV3_s {
	uint64_t m_nStableKey;
	uint64_t m_nKey;
	godot::PackedByteArray m_PackageBytes;
} t3dVTEditorAllocationCacheEntryV3_t;

typedef struct t3dVTEditorCookCacheV3_s {
	eastl::vector<t3dVTEditorChannelCacheEntryV3_t, Terrain3DEastlAllocator> m_ChannelEntries;
	eastl::vector<t3dVTEditorAllocationCacheEntryV3_t, Terrain3DEastlAllocator> m_AllocationEntries;
	godot::String m_CacheDirectory;
	uint32_t m_nFilterAbi = T3D_VT_V3_FILTER_ABI_VERSION;
	uint32_t m_nCompressorAbi = T3D_VT_V3_COMPRESSOR_ABI_VERSION;
	uint64_t m_nLifetimeHits = 0u;
	uint64_t m_nLifetimeMisses = 0u;
} t3dVTEditorCookCacheV3_t;

// Cold material-only authority shared by editor and runtime package cooks.
typedef struct t3dVTGodotAssetMaterials_s {
	eastl::vector<uint64_t, Terrain3DEastlAllocator> m_stableKeys;
	eastl::vector<t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator> m_records;
} t3dVTGodotAssetMaterials_t;

bool t3d_vt_godot_collect_asset_materials(
        const godot::Ref<Terrain3DAssets> &rAssets, t3dVTGodotAssetMaterials_t &rMaterials, godot::String *pError );

bool t3d_vt_godot_extract_instance_source_v3(
        const godot::Ref<Terrain3DRenderingMaterial> &rMaterial, uint64_t nStableKey, t3dVTGodotSourceV3_t &rSource );
bool t3d_vt_godot_extract_terrain_source_v3( const godot::Ref<Terrain3DAssets> &rAssets,
        uint32_t nLogicalMaterialId,
        uint64_t nStableKey,
        t3dVTGodotSourceV3_t &rSource );
bool t3d_vt_godot_extract_source_by_stable_key_v3(
        const godot::Ref<Terrain3DAssets> &rAssets, uint64_t nStableKey, t3dVTGodotSourceV3_t &rSource, bool &rAuthored );
bool t3d_vt_godot_make_sparse_cook_source_v3( const t3dVTGodotSourceV3_t &rSource, t3dVTCookSparseSourceV3_t &rCookSource );
void t3d_vt_godot_make_encoder( t3dVTEncoderInterface_t &rEncoder );
void t3d_vt_godot_editor_cache_reset_v3( t3dVTEditorCookCacheV3_t &rCache );
void t3d_vt_godot_editor_cache_set_directory_v3( t3dVTEditorCookCacheV3_t &rCache, const godot::String &rDirectory );
bool t3d_vt_godot_cook_editor_package_incremental_v3( const godot::Ref<Terrain3DAssets> &rAssets,
        const uint64_t *pStableKeys,
        const t3dRenderingMaterialGpu_t *pMaterialRecords,
        uint32_t nMaterialCount,
        t3dVTEditorCookCacheV3_t &rCache,
        godot::PackedByteArray &rPackageBytes,
        uint64_t &rFingerprint,
        uint32_t &rPackageGeneration,
        t3dVTEditorCookTelemetryV3_t &rTelemetry,
        godot::String *pError );
bool t3d_vt_godot_cook_runtime_package( const godot::Ref<Terrain3DAssets> &rAssets,
        const uint64_t *pStableKeys,
        const t3dRenderingMaterialGpu_t *pMaterialRecords,
        uint32_t nMaterialCount,
        godot::PackedByteArray &rPackageBytes,
        uint32_t &rPackageGeneration,
        godot::String *pError );
bool t3d_vt_godot_cook_runtime_assets_package( const godot::Ref<Terrain3DAssets> &rAssets,
        godot::PackedByteArray &rPackageBytes,
        uint32_t &rPackageGeneration,
        uint32_t &rMaterialCount,
        godot::String *pError );

bool t3d_vt_godot_compile_material_record(
        const godot::Ref<Terrain3DRenderingMaterial> &rMaterial, uint32_t nAllocationId, t3dRenderingMaterialGpu_t &rRecord );

#endif // TERRAIN3D_VT_GODOT_COOKER_H
