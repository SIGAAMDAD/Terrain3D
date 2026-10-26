// Editor-only deterministic foliage jobs. Runtime instance layout is unchanged.
#ifndef TERRAIN3D_FOLIAGE_GENERATOR_H
#define TERRAIN3D_FOLIAGE_GENERATOR_H
#include "Terrain3DTypes.h"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif
class Terrain3D;
class Terrain3DSampler;

enum t3dFoliageReject_e : uint8_t {
    T3D_FOLIAGE_REJECT_CELL = 0,
    T3D_FOLIAGE_REJECT_DENSITY,
    T3D_FOLIAGE_REJECT_MISSING_REGION,
    T3D_FOLIAGE_REJECT_HOLE,
    T3D_FOLIAGE_REJECT_HEIGHT_SAMPLE,
    T3D_FOLIAGE_REJECT_NORMAL,
    T3D_FOLIAGE_REJECT_HEIGHT_RANGE,
    T3D_FOLIAGE_REJECT_SLOPE,
    T3D_FOLIAGE_REJECT_MATERIAL_EXCLUDED,
    T3D_FOLIAGE_REJECT_MATERIAL_COVERAGE,
    T3D_FOLIAGE_REJECT_CLUSTER,
    T3D_FOLIAGE_REJECT_SPACING,
    T3D_FOLIAGE_REJECT_OUTSIDE_SCOPE,
    T3D_FOLIAGE_REJECT_COUNT
};

class Terrain3DFoliageGenerator : public RefCounted {
    GDCLASS( Terrain3DFoliageGenerator, RefCounted );
    CLASS_NAME();
    Terrain3D *m_pTerrain = nullptr;
    Terrain3DSampler *m_pSampler = nullptr;
    Array m_groups;
    TypedArray<Vector2i> m_regions;
    Array m_batches;
    Dictionary m_rule, m_groupRules, m_cells, m_outputs, m_frozenSources;
    TypedArray<Vector2i> m_domain;
    TypedArray<Vector3> m_accepted;
    Vector2i m_cell;
    Vector3 m_pendingPosition, m_pendingNormal;
    int m_group = 0, m_region = 0, m_phase = 0, m_finish = 0;
    int m_minX = 0, m_maxX = 0, m_minZ = 0, m_maxZ = 0, m_scanX = 0, m_scanZ = 0;
    int m_gridX = 0, m_gridZ = 0, m_gridWidth = 0, m_gridHeight = 0;
    int64_t m_candidate = 0, m_candidateCount = 0;
    int m_neighbor = 0, m_neighborPoint = 0;
    int m_regionCap = 50000, m_jobCap = 250000, m_added = 0;
    int64_t m_scratchCap = 67108864, m_scratchBytes = 0;
    double m_gridSize = 1.0, m_macroSize = 1.0, m_spacing = 0.0;
    uint64_t m_seed = 0, m_pendingKey = 0, m_sourceStamp = 0;
    bool m_running = false, m_failed = false, m_groupReady = false, m_domainReady = false;
    bool m_cellReady = false, m_pending = false;
    String m_error;
    bool m_auto = false;
    double m_autoSlope = 0.0, m_autoHeight = 0.0;
    int m_autoBase = 0, m_autoOverlay = 0;
    Dictionary m_regionCounts;
    Array m_assetBindings;
    int64_t m_candidatesTested = 0, m_neighborTests = 0, m_samples = 0, m_transitions = 0;
    int64_t m_rejections[T3D_FOLIAGE_REJECT_COUNT] = {};
    int64_t m_lastUsec = 0, m_maxUsec = 0;
    bool prepareGroup();
    void prepareDomain();
    bool prepareCell();
    void advanceCell();
    bool sampleCandidate();
    bool acceptCandidate();
    bool checkSpacing(int &rBudget);
    bool sourcesUnchanged();
    static int cellColor(const Vector2i &rCell);
    static uint64_t hash64(uint64_t n);
    static double randomUnit(uint64_t n);
    static uint64_t hashName(const String &rValue);
protected:
    static void _bind_methods();
public:
    ~Terrain3DFoliageGenerator();
    bool start(Terrain3D *pTerrain, const Dictionary &rConfig, const TypedArray<Vector2i> &rRegions);
    Dictionary step(int nBudget = 2048);
    Dictionary stepBudgeted(const Dictionary &rBudgets);
    void cancel();
    Array takeBatches();
    Dictionary status() const;
};
#endif


