// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_AUTHORING_H
#define TERRAIN3D_WORLD_AUTHORING_H

#include "rendering/rd/world/terrain_3d_world_geometry.h"

#include <stdint.h>

static const uint32_t T3D_WORLD_AUTHORING_INVALID_SLOT = 0xffffffffu;

enum T3DWorldMode : uint32_t {
	T3D_WORLD_MODE_DISABLED = 0,
	T3D_WORLD_MODE_EXPLICIT,
	T3D_WORLD_MODE_ELIGIBLE_SCENE,
	T3D_WORLD_MODE_FULL_STATIC_SCENE
};

enum T3DWorldOwnership : uint32_t {
	T3D_WORLD_OWNERSHIP_NATIVE_GODOT = 0,
	T3D_WORLD_OWNERSHIP_WORLD,
	T3D_WORLD_OWNERSHIP_COMPATIBILITY,
	T3D_WORLD_OWNERSHIP_EDITOR_PROXY
};

enum T3DWorldCandidateKind : uint32_t {
	T3D_WORLD_CANDIDATE_UNKNOWN = 0,
	T3D_WORLD_CANDIDATE_MESH_INSTANCE,
	T3D_WORLD_CANDIDATE_WORLD_INSTANCE,
	T3D_WORLD_CANDIDATE_MULTIMESH_INSTANCE,
	T3D_WORLD_CANDIDATE_SKINNED_MESH
};

enum T3DWorldConversionReason : uint32_t {
	T3D_WORLD_CONVERSION_OK = 0,
	T3D_WORLD_CONVERSION_DISABLED,
	T3D_WORLD_CONVERSION_NOT_EXPLICIT,
	T3D_WORLD_CONVERSION_EXCLUDED,
	T3D_WORLD_CONVERSION_HIDDEN,
	T3D_WORLD_CONVERSION_DYNAMIC_DISABLED,
	T3D_WORLD_CONVERSION_UNSUPPORTED_KIND,
	T3D_WORLD_CONVERSION_MISSING_MESH,
	T3D_WORLD_CONVERSION_UNSUPPORTED_PRIMITIVE,
	T3D_WORLD_CONVERSION_UNSUPPORTED_MATERIAL,
	T3D_WORLD_CONVERSION_SORTED_TRANSPARENCY,
	T3D_WORLD_CONVERSION_TWO_SIDED_NON_FOLIAGE,
	T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED,
	T3D_WORLD_CONVERSION_COOK_FAILED,
	T3D_WORLD_CONVERSION_DUPLICATE_SOURCE
};

enum T3DWorldImportCategory : uint32_t {
	T3D_WORLD_IMPORT_AUTO = 0,
	T3D_WORLD_IMPORT_WORLD,
	T3D_WORLD_IMPORT_CLUTTER,
	T3D_WORLD_IMPORT_FOLIAGE
};

enum T3DWorldMaterialPolicy : uint32_t {
	T3D_WORLD_MATERIAL_AUTO_CONVERT = 0,
	T3D_WORLD_MATERIAL_REQUIRE_WORLD,
	T3D_WORLD_MATERIAL_NATIVE_ON_UNSUPPORTED
};

enum T3DWorldTransparencyPolicy : uint32_t {
	T3D_WORLD_TRANSPARENCY_NATIVE_SORTED = 0,
	T3D_WORLD_TRANSPARENCY_ALPHA_SCISSOR_ONLY
};

struct T3DWorldConversionPolicy {
	T3DWorldMode eMode;
	uint32_t nDefaultTier;
	T3DWorldMaterialPolicy eMaterialPolicy;
	T3DWorldTransparencyPolicy eTransparencyPolicy;
	bool bConvertMeshInstances;
	bool bConvertMultiMeshInstances;
	bool bConvertDynamicObjects;
	bool bConvertHiddenObjects;
	float fCellSize;
};

struct T3DWorldCandidateDesc {
	uint64_t nSourceId;
	T3DWorldCandidateKind eKind;
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

struct T3DWorldClassification {
	T3DWorldOwnership eOwnership;
	T3DWorldConversionReason eReason;
	uint32_t nTier;
};

struct T3DWorldOwnershipEntry {
	uint64_t nSourceId;
	uint32_t nPrototypeId;
	uint32_t nInstanceId;
	uint32_t nGeneration;
	T3DWorldOwnership eOwnership;
	T3DWorldConversionReason eReason;
};

struct T3DWorldOwnershipRegistry {
	T3DWorldOwnershipEntry *pEntries;
	uint32_t nCapacity;
	uint32_t nCount;
	uint32_t nGeneration;
};

T3DWorldConversionPolicy t3d_world_authoring_default_policy();
T3DWorldClassification t3d_world_authoring_classify_reference_debug(
        const T3DWorldConversionPolicy &pPolicy, const T3DWorldCandidateDesc &pCandidate );

bool t3d_world_ownership_registry_initialize_cold(
        T3DWorldOwnershipRegistry &rRegistry, T3DWorldOwnershipEntry *pEntries, uint32_t pCapacity );
void t3d_world_ownership_registry_clear_cold( T3DWorldOwnershipRegistry &rRegistry );
bool t3d_world_ownership_registry_publish_cold(
        T3DWorldOwnershipRegistry &rRegistry, const T3DWorldOwnershipEntry &pEntry );
const T3DWorldOwnershipEntry *t3d_world_ownership_registry_find_reference_debug(
        const T3DWorldOwnershipRegistry &pRegistry, uint64_t pSourceId );

#endif // TERRAIN3D_WORLD_AUTHORING_H
