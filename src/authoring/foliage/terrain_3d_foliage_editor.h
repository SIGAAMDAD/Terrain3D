// Editor-only foliage publication and source snapshots. Never linked in templates.
#ifndef TERRAIN3D_FOLIAGE_EDITOR_H
#define TERRAIN3D_FOLIAGE_EDITOR_H
#if defined(T3D_EDITOR_NATIVE)
#include "instances/runtime/terrain_3d_instancer.h"
#include <godot_cpp/classes/ref_counted.hpp>

class Terrain3D;

class Terrain3DFoliageEditor : public RefCounted {
    GDCLASS(Terrain3DFoliageEditor, RefCounted);
    CLASS_NAME();
    struct RestoreState;
    RestoreState *m_restore = nullptr;
    uint64_t m_terrainId = 0;
    eastl::vector<t3dPendingPlacement_t, Terrain3DEastlAllocator> m_placements;
    Terrain3D *getTerrain() const;
    void terrainExiting();
    Dictionary replaceOwnedRegionsInternal(const Array &rBatches, bool bBackup, bool bPublish);
protected:
    static void _bind_methods();
public:
    ~Terrain3DFoliageEditor();
    void setTerrain(Terrain3D *pTerrain);
    Dictionary replaceOwnedRegions(const Array &rBatches, bool bBackup = true);
    Dictionary prepareOwnedRegions(const Array &rBatches);
    Dictionary beginRestore(const String &rId, const Dictionary &rAfter, const Dictionary &rBefore,
        const Dictionary &rAfterGenerations, const Dictionary &rBeforeGenerations,
        const Dictionary &rBeforePersistence, const Dictionary &rExpectedRevisions);
    Dictionary stepRestore(const String &rId, int nPages = 1, int64_t nBytes = 1048576);
    Dictionary publishRestore(const String &rId, bool bAfter);
    void cancelRestore(const String &rId = String());
    Dictionary restoreInstancePages(const Dictionary &rPages, const Dictionary &rGenerations,
        const Dictionary &rPersistenceStates);
    static Dictionary captureSources(Terrain3D *pTerrain, const TypedArray<Vector2i> &rRegions, int nHalo = 4);
    static int64_t sourceStamp();
    static int64_t instanceRevision(const Ref<Terrain3DRegion> &rRegion);
    static Dictionary getOwnerIds(const Ref<Terrain3DRegion> &rRegion);
    static bool validateInstancePage(const PackedByteArray &rPage);
    static Error stageRegionInstances(Terrain3D *pTerrain, const Vector2i &rRegionLoc, const String &rDir,
        const String &rStagePath, const PackedByteArray &rPage, int64_t nGeneration);
};
#endif
#endif
