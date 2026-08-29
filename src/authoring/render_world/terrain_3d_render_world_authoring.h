// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_AUTHORING_H
#define TERRAIN3D_RENDER_WORLD_AUTHORING_H

#include "rendering/rd/render_world/terrain_3d_render_world_geometry.h"

#include <stdint.h>

static const uint32_t T3D_RW_AUTHORING_INVALID_SLOT = 0xffffffffu;

enum T3DRenderWorldMode : uint32_t {
	T3D_RW_MODE_DISABLED = 0,
	T3D_RW_MODE_EXPLICIT,
	T3D_RW_MODE_ELIGIBLE_SCENE,
	T3D_RW_MODE_FULL_STATIC_SCENE
};

enum T3DRenderWorldOwnership : uint32_t {
	T3D_RW_OWNERSHIP_NATIVE_GODOT = 0,
	T3D_RW_OWNERSHIP_RENDERWORLD,
	T3D_RW_OWNERSHIP_COMPATIBILITY,
	T3D_RW_OWNERSHIP_EDITOR_PROXY
};

enum T3DRenderWorldCandidateKind : uint32_t {
	T3D_RW_CANDIDATE_UNKNOWN = 0,
	T3D_RW_CANDIDATE_MESH_INSTANCE,
	T3D_RW_CANDIDATE_RENDERWORLD_INSTANCE,
	T3D_RW_CANDIDATE_MULTIMESH_INSTANCE,
	T3D_RW_CANDIDATE_SKINNED_MESH
};

enum T3DRenderWorldConversionReason : uint32_t {
	T3D_RW_CONVERSION_OK = 0,
	T3D_RW_CONVERSION_DISABLED,
	T3D_RW_CONVERSION_NOT_EXPLICIT,
	T3D_RW_CONVERSION_EXCLUDED,
	T3D_RW_CONVERSION_HIDDEN,
	T3D_RW_CONVERSION_DYNAMIC_DISABLED,
	T3D_RW_CONVERSION_UNSUPPORTED_KIND,
	T3D_RW_CONVERSION_MISSING_MESH,
	T3D_RW_CONVERSION_UNSUPPORTED_PRIMITIVE,
	T3D_RW_CONVERSION_UNSUPPORTED_MATERIAL,
	T3D_RW_CONVERSION_SORTED_TRANSPARENCY,
	T3D_RW_CONVERSION_TWO_SIDED_NON_FOLIAGE,
	T3D_RW_CONVERSION_CAPACITY_EXCEEDED,
	T3D_RW_CONVERSION_COOK_FAILED,
	T3D_RW_CONVERSION_DUPLICATE_SOURCE
};

enum T3DRenderWorldImportCategory : uint32_t {
	T3D_RW_IMPORT_AUTO = 0,
	T3D_RW_IMPORT_WORLD,
	T3D_RW_IMPORT_CLUTTER,
	T3D_RW_IMPORT_FOLIAGE
};

enum T3DRenderWorldMaterialPolicy : uint32_t {
	T3D_RW_MATERIAL_AUTO_CONVERT = 0,
	T3D_RW_MATERIAL_REQUIRE_RENDERWORLD,
	T3D_RW_MATERIAL_NATIVE_ON_UNSUPPORTED
};

enum T3DRenderWorldTransparencyPolicy : uint32_t {
	T3D_RW_TRANSPARENCY_NATIVE_SORTED = 0,
	T3D_RW_TRANSPARENCY_ALPHA_SCISSOR_ONLY
};

struct T3DRenderWorldConversionPolicy {
	T3DRenderWorldMode eMode;
	uint32_t nDefaultTier;
	T3DRenderWorldMaterialPolicy eMaterialPolicy;
	T3DRenderWorldTransparencyPolicy eTransparencyPolicy;
	bool bConvertMeshInstances;
	bool bConvertMultiMeshInstances;
	bool bConvertDynamicObjects;
	bool bConvertHiddenObjects;
	float fCellSize;
};

struct T3DRenderWorldCandidateDesc {
	uint64_t nSourceId;
	T3DRenderWorldCandidateKind eKind;
	uint32_t nTier;
	bool bExplicitInclude;
	bool bExplicitExclude;
	bool bVisible;
	bool bStatic;
	bool bHasMesh;
	bool bMaterialSupported;
	bool bSortedTransparency;
	bool bTwoSided;
};

struct T3DRenderWorldClassification {
	T3DRenderWorldOwnership eOwnership;
	T3DRenderWorldConversionReason eReason;
	uint32_t nTier;
};

struct T3DRenderWorldOwnershipEntry {
	uint64_t nSourceId;
	uint32_t nPrototypeId;
	uint32_t nInstanceId;
	uint32_t nGeneration;
	T3DRenderWorldOwnership eOwnership;
	T3DRenderWorldConversionReason eReason;
};

struct T3DRenderWorldOwnershipRegistry {
	T3DRenderWorldOwnershipEntry *pEntries;
	uint32_t nCapacity;
	uint32_t nCount;
	uint32_t nGeneration;
};

T3DRenderWorldConversionPolicy t3d_render_world_authoring_default_policy();
T3DRenderWorldClassification t3d_render_world_authoring_classify_reference_debug(
        const T3DRenderWorldConversionPolicy &pPolicy, const T3DRenderWorldCandidateDesc &pCandidate );

bool t3d_render_world_ownership_registry_initialize_cold(
        T3DRenderWorldOwnershipRegistry &rRegistry, T3DRenderWorldOwnershipEntry *pEntries, uint32_t pCapacity );
void t3d_render_world_ownership_registry_clear_cold( T3DRenderWorldOwnershipRegistry &rRegistry );
bool t3d_render_world_ownership_registry_publish_cold(
        T3DRenderWorldOwnershipRegistry &rRegistry, const T3DRenderWorldOwnershipEntry &pEntry );
const T3DRenderWorldOwnershipEntry *t3d_render_world_ownership_registry_find_reference_debug(
        const T3DRenderWorldOwnershipRegistry &pRegistry, uint64_t pSourceId );

#endif // TERRAIN3D_RENDER_WORLD_AUTHORING_H
