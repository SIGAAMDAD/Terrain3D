// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_GODOT_CONVERTER_H
#define TERRAIN3D_RENDER_WORLD_GODOT_CONVERTER_H

#include "instances/terrain_3d_render_world_instance_3d.h"
#include "authoring/render_world/terrain_3d_render_world_scene_builder.h"

#include <EASTL/type_traits.h>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>

static const uint32_t T3D_RW_GODOT_SOURCE_FLAG_VISIBLE = 1u << 0u;
static const uint32_t T3D_RW_GODOT_SOURCE_FLAG_EXPLICIT = 1u << 1u;
static const uint32_t T3D_RW_GODOT_SOURCE_FLAG_EXCLUDED = 1u << 2u;
static const uint32_t T3D_RW_GODOT_SOURCE_FLAG_DYNAMIC = 1u << 3u;

enum T3DRenderWorldVTMaterialChannel : uint32_t {
	T3D_RW_VT_MATERIAL_ALBEDO = 0,
	T3D_RW_VT_MATERIAL_NORMAL,
	T3D_RW_VT_MATERIAL_SURFACE,
	T3D_RW_VT_MATERIAL_HEIGHT
};

typedef uint32_t ( *T3DRenderWorldResolveTextureVTColdFn )(
        void *pUser, const godot::Ref<godot::Texture2D> &pTexture, T3DRenderWorldVTMaterialChannel pChannel );

struct T3DRenderWorldGodotMaterialResolver {
	T3DRenderWorldResolveTextureVTColdFn pResolveTextureVT;
	void *pUser;
};

struct T3DRenderWorldGodotScratch {
	T3DPrototypeVertex *pVertices;
	uint32_t nVertexCapacity;
	uint32_t *pIndices;
	uint32_t nIndexCapacity;
	uint32_t *pTriangleMaterialOffsets;
	uint8_t *pTriangleTiers;
	uint32_t nTriangleCapacity;
};

struct T3DRenderWorldPrototypeEntry {
	uint64_t nKey;
	uint32_t nMeshId;
	uint32_t nGeneration;
};

struct T3DRenderWorldMaterialEntry {
	uint64_t nKey;
	uint32_t nMaterialId;
	uint32_t nGeneration;
};

struct T3DRenderWorldNativeVisualEntry {
	uint64_t nSourceId;
	T3DRenderWorldOwnership eOwnership;
};
static_assert( eastl::is_trivially_destructible<T3DRenderWorldNativeVisualEntry>::value,
        "RenderWorld native visual records must remain POD-like" );

struct T3DRenderWorldSceneBuildBuffers {
	T3DGeometryDatabase *pGeometry;
	T3DMaterialRecord *pMaterials;
	uint32_t nMaterialCapacity;
	uint32_t nMaterialCount;
	T3DSceneCell *pCells;
	uint32_t nCellCapacity;
	uint32_t nCellCount;
	T3DInstanceRecord *pInstances;
	uint32_t nInstanceCapacity;
	uint32_t nInstanceCount;
	uint64_t *pInstanceSourceIds;
	T3DRenderWorldOwnershipRegistry *pOwnership;
	T3DRenderWorldPrototypeEntry *pPrototypeEntries;
	T3DRenderWorldPrototypeBounds *pPrototypeBoundsByMeshId;
	uint32_t nPrototypeBoundsCapacity;
	uint32_t nPrototypeCapacity;
	uint32_t nPrototypeCount;
	T3DRenderWorldMaterialEntry *pMaterialEntries;
	uint32_t nMaterialEntryCapacity;
	uint32_t nMaterialEntryCount;
	T3DRenderWorldNativeVisualEntry *pNativeVisuals;
	uint32_t nNativeVisualCapacity;
	uint32_t nNativeVisualCount;
	T3DRenderWorldGodotScratch Scratch;
	T3DRenderWorldCellBuildScratch CellScratch;
};

struct T3DRenderWorldSceneBuildStats {
	uint32_t nVisitedNodes;
	uint32_t nMeshCandidates;
	uint32_t nConvertedInstances;
	uint32_t nUniquePrototypes;
	uint32_t nUniqueMaterials;
	uint32_t nCompatibilityInstances;
	uint32_t nNativeInstances;
	uint32_t nFailureCount;
};

class Terrain3DRenderWorldGodotConverter {
public:
	Terrain3DRenderWorldGodotConverter();

	bool initializeCold( const T3DRenderWorldConversionPolicy &pPolicy,
	        const T3DRenderWorldGodotMaterialResolver &pMaterialResolver,
	        T3DRenderWorldSceneBuildBuffers *pBuffers );
	void setPolicyCold( const T3DRenderWorldConversionPolicy &pPolicy );
	bool rebuildSceneCold( godot::Node *pRoot, T3DRenderWorldSceneBuildStats &rStats );
	void releaseNativeOwnershipCold();
	bool applyNativeOwnershipCold( bool pRenderWorldReady );

	bool isInitialized() const;
	const T3DRenderWorldConversionPolicy &getPolicy() const;
	const T3DRenderWorldSceneBuildBuffers *getBuildBuffersCold() const;

private:
	bool visitNodeCold( godot::Node *pNode, T3DRenderWorldSceneBuildStats &rStats );
	bool convertMeshInstanceCold( godot::MeshInstance3D *pMeshInstance, T3DRenderWorldSceneBuildStats &rStats );
	bool buildCandidateCold( godot::MeshInstance3D *pMeshInstance,
	        T3DRenderWorldCandidateDesc &rCandidate,
	        godot::Ref<Terrain3DRenderWorldImportProfile> &rProfile,
	        godot::Ref<Terrain3DRenderWorldMaterial> &rExplicitMaterial ) const;
	bool analyzeMaterialsCold( godot::MeshInstance3D *pMeshInstance,
	        uint32_t pTier,
	        const godot::Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
	        bool &rSupported,
	        bool &rSortedTransparency,
	        bool &rTwoSided ) const;
	bool findOrCookPrototypeCold( godot::MeshInstance3D *pMeshInstance,
	        uint32_t pTier,
	        const godot::Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
	        uint32_t &rMeshId,
	        T3DRenderWorldConversionReason &rReason );
	bool flattenMeshCold( godot::MeshInstance3D *pMeshInstance,
	        uint32_t pTier,
	        const godot::Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
	        T3DGeometrySourceMesh &rSource,
	        T3DGeometrySourceLod &rLod,
	        uint64_t &rPrototypeKey,
	        T3DRenderWorldConversionReason &rReason );
	bool findOrCompileMaterialCold( const godot::Ref<godot::Material> &pMaterial,
	        const godot::Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial,
	        uint32_t pTier,
	        uint32_t &rMaterialId,
	        T3DRenderWorldConversionReason &rReason );
	bool compileStandardMaterialCold( const godot::Ref<godot::Material> &pMaterial,
	        uint32_t pTier,
	        T3DMaterialRecord &rRecord,
	        bool &rSortedTransparency,
	        bool &rTwoSided ) const;
	uint64_t computePrototypeKeyCold(
	        godot::MeshInstance3D *pMeshInstance, uint32_t pTier, const godot::Ref<Terrain3DRenderWorldMaterial> &pExplicitMaterial ) const;
	bool appendInstanceCold( godot::MeshInstance3D *pMeshInstance, uint32_t pMeshId, uint32_t pMaterialOverride, uint32_t &rInstanceId );
	void publishOwnershipCold( godot::MeshInstance3D *pNode,
	        T3DRenderWorldOwnership pOwnership,
	        T3DRenderWorldConversionReason pReason,
	        uint32_t pPrototypeId,
	        uint32_t pInstanceId );

	T3DRenderWorldConversionPolicy m_policy;
	T3DRenderWorldGodotMaterialResolver m_materialResolver;
	T3DRenderWorldSceneBuildBuffers *m_pBuffers;
	bool m_bInitialized;
	bool m_bNativeOwnershipApplied;
};

#endif // TERRAIN3D_RENDER_WORLD_GODOT_CONVERTER_H
