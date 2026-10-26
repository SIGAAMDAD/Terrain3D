// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_GODOT_CONVERTER_H
#define TERRAIN3D_WORLD_GODOT_CONVERTER_H

#include "instances/terrain_3d_world_instance_3d.h"
#include "authoring/world/terrain_3d_world_scene_builder.h"

#include <EASTL/type_traits.h>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>

static const uint32_t T3D_WORLD_GODOT_SOURCE_FLAG_VISIBLE = 1u << 0u;
static const uint32_t T3D_WORLD_GODOT_SOURCE_FLAG_EXPLICIT = 1u << 1u;
static const uint32_t T3D_WORLD_GODOT_SOURCE_FLAG_EXCLUDED = 1u << 2u;
static const uint32_t T3D_WORLD_GODOT_SOURCE_FLAG_DYNAMIC = 1u << 3u;

typedef bool ( *T3DWorldResolveMaterialVTColdFn )( void *pUser, uint64_t nStableMaterialKey, uint32_t &rAllocationId );

struct T3DWorldGodotMaterialResolver {
	T3DWorldResolveMaterialVTColdFn pResolveMaterialVT;
	void *pUser;
};

struct T3DWorldGodotScratch {
	t3dWorldVertex_t *pVertices;
	uint32_t nVertexCapacity;
	uint32_t *pIndices;
	uint32_t nIndexCapacity;
	uint32_t *pTriangleMaterialOffsets;
	uint8_t *pTriangleTiers;
	uint32_t nTriangleCapacity;
};

struct T3DWorldPrototypeEntry {
	uint64_t nKey;
	uint32_t nMeshId;
	uint32_t nGeneration;
};

struct T3DWorldMaterialEntry {
	uint64_t nKey;
	uint32_t nMaterialId;
	uint32_t nGeneration;
};

struct T3DWorldNativeVisualEntry {
	uint64_t nSourceId;
	T3DWorldOwnership eOwnership;
};
static_assert( eastl::is_trivially_destructible<T3DWorldNativeVisualEntry>::value,
        "World native visual records must remain POD-like" );

struct T3DWorldSceneBuildBuffers {
	T3DWorldGeometryDatabase *pGeometry;
	t3dRenderingMaterialGpu_t *pMaterials;
	uint32_t nMaterialCapacity;
	uint32_t nMaterialCount;
	t3dWorldCell_t *pCells;
	uint32_t nCellCapacity;
	uint32_t nCellCount;
	t3dWorldInstance_t *pInstances;
	t3dWorldTransform_t *pTransforms;
	uint32_t nInstanceCapacity;
	uint32_t nInstanceCount;
	uint64_t *pInstanceSourceIds;
	T3DWorldOwnershipRegistry *pOwnership;
	T3DWorldPrototypeEntry *pPrototypeEntries;
	T3DWorldPrototypeBounds *pPrototypeBoundsByMeshId;
	uint32_t nPrototypeBoundsCapacity;
	uint32_t nPrototypeCapacity;
	uint32_t nPrototypeCount;
	T3DWorldMaterialEntry *pMaterialEntries;
	uint32_t nMaterialEntryCapacity;
	uint32_t nMaterialEntryCount;
	T3DWorldNativeVisualEntry *pNativeVisuals;
	uint32_t nNativeVisualCapacity;
	uint32_t nNativeVisualCount;
	T3DWorldGodotScratch Scratch;
	T3DWorldCellBuildScratch CellScratch;
};

struct T3DWorldSceneBuildStats {
	uint32_t nVisitedNodes;
	uint32_t nMeshCandidates;
	uint32_t nConvertedInstances;
	uint32_t nUniquePrototypes;
	uint32_t nUniqueMaterials;
	uint32_t nCompatibilityInstances;
	uint32_t nNativeInstances;
	uint32_t nFailureCount;
};

class Terrain3DWorldGodotConverter {
public:
	Terrain3DWorldGodotConverter();

	bool initializeCold( const T3DWorldConversionPolicy &pPolicy,
	        const T3DWorldGodotMaterialResolver &pMaterialResolver,
	        T3DWorldSceneBuildBuffers *pBuffers );
	void setPolicyCold( const T3DWorldConversionPolicy &pPolicy );
	bool rebuildSceneCold( godot::Node *pRoot, T3DWorldSceneBuildStats &rStats );
	void releaseNativeOwnershipCold();
	bool applyNativeOwnershipCold( bool pWorldReady );

	bool isInitialized() const;
	const T3DWorldConversionPolicy &getPolicy() const;
	const T3DWorldSceneBuildBuffers *getBuildBuffersCold() const;

private:
	bool visitNodeCold( godot::Node *pNode, T3DWorldSceneBuildStats &rStats );
	bool convertMeshInstanceCold( godot::MeshInstance3D *pMeshInstance, T3DWorldSceneBuildStats &rStats );
	bool buildCandidateCold( godot::MeshInstance3D *pMeshInstance,
	        T3DWorldCandidateDesc &rCandidate,
	        godot::Ref<Terrain3DWorldImportProfile> &rProfile,
	        godot::Ref<Terrain3DWorldMaterial> &rExplicitMaterial ) const;
	bool analyzeMaterialsCold( godot::MeshInstance3D *pMeshInstance,
	        uint32_t pTier,
	        const godot::Ref<Terrain3DWorldMaterial> &pExplicitMaterial,
	        bool &rSupported,
	        bool &rSortedTransparency,
	        bool &rTwoSided ) const;
	bool findOrCookPrototypeCold( godot::MeshInstance3D *pMeshInstance,
	        uint32_t pTier,
	        const godot::Ref<Terrain3DWorldMaterial> &pExplicitMaterial,
	        uint32_t &rMeshId,
	        T3DWorldConversionReason &rReason );
	bool flattenMeshCold( godot::MeshInstance3D *pMeshInstance,
	        uint32_t pTier,
	        const godot::Ref<Terrain3DWorldMaterial> &pExplicitMaterial,
	        T3DWorldGeometrySourceMesh &rSource,
	        T3DWorldGeometrySourceLod &rLod,
	        uint64_t &rPrototypeKey,
	        T3DWorldConversionReason &rReason );
	bool findOrCompileMaterialCold( const godot::Ref<godot::Material> &pMaterial,
	        const godot::Ref<Terrain3DWorldMaterial> &pExplicitMaterial,
	        uint32_t pTier,
	        uint32_t &rMaterialId,
	        T3DWorldConversionReason &rReason );
	bool compileStandardMaterialCold( const godot::Ref<godot::Material> &pMaterial,
	        uint32_t pTier,
	        t3dRenderingMaterialGpu_t &rRecord,
	        bool &rSortedTransparency,
	        bool &rTwoSided ) const;
	uint64_t computePrototypeKeyCold(
	        godot::MeshInstance3D *pMeshInstance, uint32_t pTier, const godot::Ref<Terrain3DWorldMaterial> &pExplicitMaterial ) const;
	bool appendInstanceCold( godot::MeshInstance3D *pMeshInstance, uint32_t pMeshId, uint32_t pMaterialOverride, uint32_t &rInstanceId );
	void publishOwnershipCold( godot::MeshInstance3D *pNode,
	        T3DWorldOwnership pOwnership,
	        T3DWorldConversionReason pReason,
	        uint32_t pPrototypeId,
	        uint32_t pInstanceId );

	T3DWorldConversionPolicy m_policy;
	T3DWorldGodotMaterialResolver m_materialResolver;
	T3DWorldSceneBuildBuffers *m_pBuffers;
	bool m_bInitialized;
	bool m_bNativeOwnershipApplied;
};

#endif // TERRAIN3D_WORLD_GODOT_CONVERTER_H
