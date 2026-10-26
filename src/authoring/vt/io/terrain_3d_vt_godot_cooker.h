// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_GODOT_COOKER_H
#define TERRAIN3D_VT_GODOT_COOKER_H
#include "memory/policy/terrain_3d_memory_account.h"

#include "Terrain3DTypes.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_rendering_material.h"
#include "authoring/vt/build/terrain_3d_vt_cooker.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "rendering/rd/vt/runtime/terrain_3d_vt_fixed_profile.h"

#include <godot_cpp/variant/packed_byte_array.hpp>

typedef struct t3dVTGodotSourceV1_s
{
	godot::PackedByteArray		m_ChannelBytes[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTCookImageView_t		m_Channels[T3D_VT_LOGICAL_CHANNEL_COUNT];
	t3dVTGodotSourceChannelV1_t m_ChannelDesc[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint64_t					m_nStableKey;
	uint32_t					m_nAuthoredChannelMask;
	uint32_t					m_nSourceFlags;
	uint32_t					m_nOrmSourceLayout;
	uint32_t					m_nConsumer;
	t3dVTFixedProfile_t			m_eHighestAvailableProfile;
} t3dVTGodotSourceV1_t;

typedef struct t3dVTEditorChannelCacheEntryV1_s
{
	uint64_t						 m_nKey;
	uint32_t						 m_nTargetExtent;
	uint32_t						 m_nLogicalChannel;
	t3dVTMipChainLayout_t			 m_Layout;
	godot::PackedByteArray			 m_Bytes;
	Terrain3DSharedMemoryReservation m_memory;
} t3dVTEditorChannelCacheEntryV1_t;

typedef struct t3dVTEditorAllocationCacheEntryV1_s
{
	uint64_t						 m_nStableKey;
	uint64_t						 m_nKey;
	godot::PackedByteArray			 m_PackageBytes;
	Terrain3DSharedMemoryReservation m_memory;
	bool							 m_bPendingDiskWrite = false;
} t3dVTEditorAllocationCacheEntryV1_t;

typedef struct t3dVTEditorCookCacheV1_s
{
	eastl::vector< t3dVTEditorChannelCacheEntryV1_t, Terrain3DEastlAllocator >	  m_ChannelEntries;
	eastl::vector< t3dVTEditorAllocationCacheEntryV1_t, Terrain3DEastlAllocator > m_AllocationEntries;
	godot::String																  m_CacheDirectory;
	uint32_t m_nFilterAbi = T3D_VT_V1_FILTER_ABI_VERSION;
	uint32_t m_nCompressorAbi = T3D_VT_V1_COMPRESSOR_ABI_VERSION;
	uint64_t m_nLifetimeHits = 0u;
	uint64_t m_nLifetimeMisses = 0u;
} t3dVTEditorCookCacheV1_t;

// Cold material-only authority shared by editor and runtime package cooks.
typedef struct t3dVTGodotAssetMaterials_s
{
	eastl::vector< uint64_t, Terrain3DEastlAllocator >					m_stableKeys;
	eastl::vector< t3dRenderingMaterialGpu_t, Terrain3DEastlAllocator > m_records;
} t3dVTGodotAssetMaterials_t;

// Imports a parity-approved composed .t3sc sidecar through the ordinary V1
// cooker. The expected source fingerprint must come from the authoring owner;
// its value is never inferred from the sidecar itself. This is a cold operation.
// The result is one normal V1 package fragment with COMPOSED stable keys.
// Imports indexed .t3avt region files as ordinary FAR consumer allocations.
// Empty region directories return an empty fragment and success; malformed files fail.
bool t3d_vt_godot_import_far_region_directory_v1(
    const godot::String &rDataDirectory, uint32_t nGeneration,
    godot::PackedByteArray &rFragment, godot::String *pError );

bool t3d_vt_godot_import_composed_sidecar_v1(
    const uint8_t *pSidecar, uint64_t nSidecarBytes,
    uint64_t nExpectedSourceFingerprint, uint32_t nPackageGeneration,
    godot::PackedByteArray &rFragment, godot::String *pError );

bool t3d_vt_godot_cook_source_fragment_v1( const t3dVTCookSparseSourceV1_t &rSource,
										   godot::PackedByteArray		   &rBytes,
										   uint64_t nWorkingByteLimit = 512ull * 1024ull * 1024ull );

// Cold composition is admitted before allocating its bounded work tables.
static const uint64_t T3D_VT_COOK_WORKING_BYTE_LIMIT = 512ull * 1024ull * 1024ull;
struct t3dVTMergeStatistics_t
{
	uint64_t m_nWorkingBytes = 0u;
	uint64_t m_nPayloadComparisons = 0u;
	bool	 m_bWorkingLimitExceeded = false;
};
bool t3d_vt_godot_merge_packages_v1( const godot::PackedByteArray *pPackages,
									 uint32_t					   nCount,
									 uint32_t					   nGeneration,
									 godot::PackedByteArray		  &rPackageBytes,
									 uint64_t					   nWorkingByteLimit = T3D_VT_COOK_WORKING_BYTE_LIMIT,
									 t3dVTMergeStatistics_t		  *pStatistics = NULL,
									 Terrain3DSharedMemoryReservation *pPackageMemory = NULL );

bool t3d_vt_godot_collect_asset_materials( const godot::Ref< Terrain3DAssets > &rAssets,
										   t3dVTGodotAssetMaterials_t		   &rMaterials,
										   godot::String					   *pError );

bool t3d_vt_godot_extract_instance_source_v1( const godot::Ref< Terrain3DRenderingMaterial > &rMaterial,
											  uint64_t										  nStableKey,
											  t3dVTGodotSourceV1_t							 &rSource );
bool t3d_vt_godot_extract_terrain_source_v1( const godot::Ref< Terrain3DAssets > &rAssets,
											 uint32_t							  nLogicalMaterialId,
											 uint64_t							  nStableKey,
											 t3dVTGodotSourceV1_t				 &rSource );
bool t3d_vt_godot_extract_source_by_stable_key_v1( const godot::Ref< Terrain3DAssets > &rAssets,
												   uint64_t								nStableKey,
												   t3dVTGodotSourceV1_t				   &rSource,
												   bool								   &rAuthored );
bool t3d_vt_godot_make_sparse_cook_source_v1( const t3dVTGodotSourceV1_t &rSource,
											  t3dVTCookSparseSourceV1_t	 &rCookSource );
void t3d_vt_godot_make_encoder( t3dVTEncoderInterface_t &rEncoder );
void t3d_vt_godot_editor_cache_reset_v1( t3dVTEditorCookCacheV1_t &rCache );
void t3d_vt_godot_editor_cache_set_directory_v1( t3dVTEditorCookCacheV1_t &rCache, const godot::String &rDirectory );
bool t3d_vt_godot_cook_editor_package_incremental_v1( const godot::Ref< Terrain3DAssets > &rAssets,
													  const uint64_t					  *pStableKeys,
													  const t3dRenderingMaterialGpu_t	  *pMaterialRecords,
													  uint32_t							   nMaterialCount,
													  t3dVTEditorCookCacheV1_t			  &rCache,
													  godot::PackedByteArray			  &rPackageBytes,
													  uint64_t							  &rFingerprint,
													  uint32_t							  &rPackageGeneration,
													  t3dVTEditorCookTelemetryV1_t		  &rTelemetry,
													  godot::String						  *pError,
													  Terrain3DSharedMemoryReservation	  *pPackageMemory = NULL,
													  bool								   bForceRebuild = false );
bool t3d_vt_godot_cook_runtime_package( const godot::Ref< Terrain3DAssets > &rAssets,
										const uint64_t						*pStableKeys,
										const t3dRenderingMaterialGpu_t		*pMaterialRecords,
										uint32_t							 nMaterialCount,
										godot::PackedByteArray				&rPackageBytes,
										uint32_t							&rPackageGeneration,
										godot::String						*pError );
bool t3d_vt_godot_cook_runtime_assets_package( const godot::Ref< Terrain3DAssets > &rAssets,
											   godot::PackedByteArray			   &rPackageBytes,
											   uint32_t							   &rPackageGeneration,
											   uint32_t							   &rMaterialCount,
											   godot::String					   *pError );

bool t3d_vt_godot_compile_material_record( const godot::Ref< Terrain3DRenderingMaterial > &rMaterial,
										   uint32_t										   nAllocationId,
										   t3dRenderingMaterialGpu_t					  &rRecord );

#endif // TERRAIN3D_VT_GODOT_COOKER_H
