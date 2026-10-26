#include "authoring/foliage/terrain_3d_foliage_generator.h"
#include "terrain_3d.h"
#include "data/model/terrain_3d_data.h"
#include "data/sampling/terrain_3d_sampler.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "rendering/material/terrain_3d_material.h"
#include "core/utility/terrain_3d_util.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/classes/time.hpp>
#include <math.h>
#include <float.h>

uint64_t Terrain3DFoliageGenerator::hash64(uint64_t n) {
    n ^= n >> 30; n *= UINT64_C(0xbf58476d1ce4e5b9);
    n ^= n >> 27; n *= UINT64_C(0x94d049bb133111eb);
    return n ^ (n >> 31);
}
double Terrain3DFoliageGenerator::randomUnit(uint64_t n) {
    return (double)(hash64(n) >> 11) * (1.0 / 9007199254740992.0);
}
uint64_t Terrain3DFoliageGenerator::hashName(const String &rValue) {
    const CharString utf8 = rValue.utf8();
    uint64_t result = UINT64_C(14695981039346656037);
    for (int i = 0; i < utf8.length(); ++i) { result = (result ^ (uint8_t)utf8[i]) * UINT64_C(1099511628211); }
    return result;
}
int Terrain3DFoliageGenerator::cellColor(const Vector2i &rCell) {
    // Unsigned parity is well-defined at negative world coordinates.
    return ((uint32_t)rCell.x & 1u) + 2 * ((uint32_t)rCell.y & 1u);
}
Terrain3DFoliageGenerator::~Terrain3DFoliageGenerator() { cancel(); }

bool Terrain3DFoliageGenerator::start(Terrain3D *pTerrain, const Dictionary &rConfig,
                                     const TypedArray<Vector2i> &rRegions) {
    cancel();
    m_error = "";
    if (!pTerrain || !pTerrain->dataObject() || !pTerrain->getAssets().is_valid() || rRegions.is_empty()) {
        m_error = "No terrain, regions or assets"; return false;
    }
    m_pTerrain = pTerrain;
    m_regions = rRegions.duplicate();
    m_regions.sort();
    const Array rules = rConfig.get("rules", Array());
    Dictionary groups;
    Dictionary requestedRegions;
    for (int i = 0; i < m_regions.size(); ++i) { requestedRegions[m_regions[i]] = true; }
    for (int i = 0; i < rules.size(); ++i) {
        const Dictionary rule = rules[i];
        const String uid = rule.get("uid", "");
        const Vector2i region = rule.get("region", Vector2i(INT32_MAX, INT32_MAX));
        const double density = (double)rule.get("density", 0.0) * (double)rule.get("weight", 1.0);
        const double spacing = rule.get("spacing", 0.0);
        const int mesh = rule.get("mesh_id", -1), material = rule.get("material_id", -1);
        const int64_t owner = rule.get("owner", (int64_t)0);
        if (uid.is_empty() || !requestedRegions.has(region) || !pTerrain->dataObject()->hasRegion(region) ||
            owner <= 0 || owner > INT32_MAX || mesh < 0 || mesh >= pTerrain->getAssets()->getMeshCount() ||
            material < 0 || material >= pTerrain->getAssets()->getTextureCount() ||
            !isfinite(density) || density <= 0.0 || !isfinite(spacing) || spacing < 0.0 || spacing > 50.0) {
            m_error = "Invalid compiled foliage rule/region/asset"; return false;
        }
        if (pTerrain->getAssets()->getMeshAsset(mesh).is_null() || pTerrain->getAssets()->getTextureAsset(material).is_null() ||
            PackedInt32Array(rule.get("excluded_materials", PackedInt32Array())).size() > 128) {
            m_error = "Missing foliage asset or excessive exclusion list"; return false;
        }
        const double grid = CLAMP(1.0 / sqrt(density), 0.05, 128.0);
        if ((spacing > 0.0 && ceil(spacing / grid) + 1 > 32) ||
            ceil(128.0 / grid + 2.0) * ceil(128.0 / grid + 2.0) > 2000000.0) {
            m_error = "Rule exceeds spacing or candidate limits; reduce density/spacing"; return false;
        }
        Dictionary group = groups.get(uid, Dictionary());
        if (group.is_empty()) {
            group["prototype"] = rule.duplicate(true);
            group["rules"] = Dictionary();
            group["regions"] = TypedArray<Vector2i>();
            group["bindings"] = Array();
        } else {
            Dictionary prototype = Dictionary(group["prototype"]).duplicate(true);
            Dictionary comparable = rule.duplicate(true);
            for (const char *key : {"region", "owner", "key"}) { prototype.erase(key); comparable.erase(key); }
            if (prototype != comparable) { m_error = "Inconsistent region rules for " + uid; return false; }
        }
        Dictionary byRegion = group["rules"];
        if (byRegion.has(region)) { m_error = "Duplicate rule identity in region"; return false; }
        byRegion[region] = rule;
        group["rules"] = byRegion;
        TypedArray<Vector2i> regions = group["regions"];
        regions.push_back(region);
        group["regions"] = regions;
        groups[uid] = group;
    }
    Array uids = groups.keys();
    uids.sort();
    for (int i = 0; i < uids.size(); ++i) {
        Dictionary group = groups[uids[i]];
        TypedArray<Vector2i> regions = group["regions"];
        regions.sort();
        group["regions"] = regions;
        m_groups.push_back(group);
        const Dictionary prototype = group["prototype"];
        const int mesh = prototype["mesh_id"], material = prototype["material_id"];
        Array binding;
        binding.push_back(mesh); binding.push_back((int64_t)pTerrain->getAssets()->getMeshAsset(mesh)->get_instance_id());
        binding.push_back(material); binding.push_back((int64_t)pTerrain->getAssets()->getTextureAsset(material)->get_instance_id());
        m_assetBindings.push_back(binding);
    }
    if (m_groups.is_empty()) { m_error = "Preset has no active mesh rules"; return false; }
    m_seed = (uint64_t)(int64_t)rConfig.get("seed", (int64_t)0);
    m_regionCap = CLAMP((int)rConfig.get("region_limit", 50000), 1, 1000000);
    m_jobCap = CLAMP((int)rConfig.get("job_limit", 250000), 1, 2000000);
    m_scratchCap = (int64_t)CLAMP((int)rConfig.get("scratch_mb", 64), 8, 512) * 1048576;
    const Ref<Terrain3DMaterial> material = pTerrain->materialResource();
    m_auto = material.is_valid() && material->getAutoShaderEnabled();
    if (m_auto) {
        m_autoSlope = material->getShaderParam("auto_slope");
        m_autoHeight = material->getShaderParam("auto_height_reduction");
        m_autoBase = material->getShaderParam("auto_base_texture");
        m_autoOverlay = material->getShaderParam("auto_overlay_texture");
    }
    m_frozenSources = pTerrain->dataObject()->captureFoliageSources(m_regions, 4);
    m_sourceStamp = Terrain3DRegion::getFoliageSourceStamp();
    m_pSampler = memnew(Terrain3DSampler(pTerrain->dataObject()->getSamplingSourceInternal()));
    m_running = true;
    return true;
}

bool Terrain3DFoliageGenerator::sourcesUnchanged() {
    const Ref<Terrain3DMaterial> material = m_pTerrain->materialResource();
    const bool autoEnabled = material.is_valid() && material->getAutoShaderEnabled();
    if (autoEnabled != m_auto || (m_auto &&
        ((double)material->getShaderParam("auto_slope") != m_autoSlope ||
         (double)material->getShaderParam("auto_height_reduction") != m_autoHeight ||
         (int)material->getShaderParam("auto_base_texture") != m_autoBase ||
         (int)material->getShaderParam("auto_overlay_texture") != m_autoOverlay))) {
        m_error = "Terrain automatic material settings changed"; return false;
    }
    const Ref<Terrain3DAssets> assets = m_pTerrain->getAssets();
    for (int i = 0; i < m_assetBindings.size(); ++i) {
        const Array binding = m_assetBindings[i];
        const Ref<Terrain3DMeshAsset> mesh = assets->getMeshAsset(binding[0]);
        const Ref<Terrain3DTextureAsset> texture = assets->getTextureAsset(binding[2]);
        if (mesh.is_null() || texture.is_null() ||
            (int64_t)mesh->get_instance_id() != (int64_t)binding[1] ||
            (int64_t)texture->get_instance_id() != (int64_t)binding[3]) {
            m_error = "Foliage asset slots changed"; return false;
        }
    }
    const uint64_t stamp = Terrain3DRegion::getFoliageSourceStamp();
    // Topology is inexpensive to compare; residency churn does not change this
    // authored source revision. No Image.get_data() or whole-map hashing here.
    const t3dSamplingSource_t source = m_pTerrain->dataObject()->getSamplingSourceInternal();
    if (stamp != m_sourceStamp ||
        (int64_t)source.m_pRepository->getTopologyGeneration() != (int64_t)m_frozenSources["topology"]) {
        const Dictionary current = m_pTerrain->dataObject()->captureFoliageSources(m_regions, 4);
        if (current["regions"] != m_frozenSources["regions"]) {
            m_error = "Terrain source changed during foliage generation"; return false;
        }
        m_frozenSources["topology"] = current["topology"];
        m_sourceStamp = stamp;
    }
    return true;
}

bool Terrain3DFoliageGenerator::prepareGroup() {
    const Dictionary group = m_groups[m_group];
    m_rule = group["prototype"];
    m_groupRules = group["rules"];
    m_domain = group["regions"];
    m_gridSize = CLAMP(1.0 / sqrt((double)m_rule["density"] * (double)m_rule["weight"]), 0.05, 128.0);
    m_spacing = m_rule.get("spacing", 0.0);
    // A small world-coordinate rounding margin preserves spacing after float
    // transform storage even when same-color cells are two bins apart.
    m_macroSize = MAX(m_gridSize, m_spacing > 0.0 ? m_spacing + 4.0 * T3D_ACTIVE_WORLD_TEXELS * FLT_EPSILON : m_gridSize);
    m_cells.clear(); m_outputs.clear(); m_accepted.clear();
    m_scratchBytes = (int64_t)m_added * 128;
    m_region = 0; m_phase = 0; m_finish = 0;
    m_domainReady = false; m_cellReady = false; m_pending = false;
    m_groupReady = true;
    return true;
}
void Terrain3DFoliageGenerator::prepareDomain() {
    const Vector2i region = m_domain[m_region];
    const int halo = m_spacing > 0.0 ? 3 : 0;
    m_minX = (int)floor((double)region.x * 128.0 / m_macroSize) - halo;
    m_minZ = (int)floor((double)region.y * 128.0 / m_macroSize) - halo;
    m_maxX = (int)ceil(((double)region.x + 1.0) * 128.0 / m_macroSize) - 1 + halo;
    m_maxZ = (int)ceil(((double)region.y + 1.0) * 128.0 / m_macroSize) - 1 + halo;
    m_scanX = m_minX; m_scanZ = m_minZ;
    m_domainReady = true;
}
void Terrain3DFoliageGenerator::advanceCell() {
    m_cellReady = false; m_pending = false;
    if (++m_scanX > m_maxX) { m_scanX = m_minX; ++m_scanZ; }
    if (m_scanZ > m_maxZ) { ++m_region; m_domainReady = false; }
}
bool Terrain3DFoliageGenerator::prepareCell() {
    m_cell = Vector2i(m_scanX, m_scanZ);
    if ((m_spacing > 0.0 && cellColor(m_cell) != m_phase) || m_cells.has(m_cell)) {
        advanceCell(); return true;
    }
    m_gridX = (int)floor(m_cell.x * m_macroSize / m_gridSize);
    m_gridZ = (int)floor(m_cell.y * m_macroSize / m_gridSize);
    m_gridWidth = (int)ceil((m_cell.x + 1.0) * m_macroSize / m_gridSize) - m_gridX;
    m_gridHeight = (int)ceil((m_cell.y + 1.0) * m_macroSize / m_gridSize) - m_gridZ;
    m_candidate = 0; m_candidateCount = (int64_t)m_gridWidth * m_gridHeight;
    m_accepted.clear(); m_neighbor = 0; m_neighborPoint = 0;
    m_cellReady = true;
    m_scratchBytes += 96;
    if (m_scratchBytes > m_scratchCap) { m_error = "Foliage occupancy exceeds scratch budget"; return false; }
    return true;
}

bool Terrain3DFoliageGenerator::sampleCandidate() {
    const int gx = m_gridX + (int)(m_candidate % m_gridWidth);
    const int gz = m_gridZ + (int)(m_candidate / m_gridWidth);
    ++m_candidate; ++m_candidatesTested;
    const uint64_t salt = hashName(m_rule.get("uid", "")) ^ m_seed;
    const uint64_t key = hash64((uint64_t)(int64_t)gx * UINT64_C(0x9e3779b185ebca87) ^
                               (uint64_t)(int64_t)gz * UINT64_C(0xc2b2ae3d27d4eb4f) ^ salt);
    const double x = (gx + randomUnit(key)) * m_gridSize;
    const double z = (gz + randomUnit(key ^ UINT64_C(0x632be59bd9b4e019))) * m_gridSize;
    if ((int)floor(x / m_macroSize) != m_cell.x || (int)floor(z / m_macroSize) != m_cell.y) { return true; }
    const double density = (double)m_rule["density"] * (double)m_rule["weight"];
    if (randomUnit(key ^ UINT64_C(0x7cd919a)) > MIN(1.0, density * m_gridSize * m_gridSize)) { return true; }
    const Vector3 location((real_t)x, 0.0f, (real_t)z);
    const Vector2i region((int)floor(x / 128.0), (int)floor(z / 128.0));
    if (!m_pTerrain->dataObject()->hasRegion(region)) { return true; }
    ++m_samples;
    const uint32_t control = m_pSampler->sampleControl(location);
    if (control == UINT32_MAX) { m_error = "Terrain source is unavailable in region " + String::num_int64(region.x) + "," + String::num_int64(region.y); return false; }
    if (t3dIsHole(control)) { return true; }
    float height = NAN; Vector3 normal;
    ++m_samples;
    if (!m_pSampler->sampleHeightAndNormal(location, height, normal)) { return true; }
    if (!normal.is_finite() || normal.length_squared() < 0.1 || !isfinite(height)) { return true; }
    normal.normalize();
    if (height < (double)m_rule.get("min_height", -100000.0) || height > (double)m_rule.get("max_height", 100000.0)) { return true; }
    const double slope = Math::rad_to_deg(Math::acos(CLAMP(normal.y, (real_t)-1.0, (real_t)1.0)));
    if (slope < (double)m_rule.get("min_slope", 0.0) || slope > (double)m_rule.get("max_slope", 90.0)) { return true; }
    int base = t3dGetBase(control), overlay = t3dGetOverlay(control);
    double blend = (double)t3dGetBlend(control) / 255.0;
    if (m_auto && t3dIsAuto(control)) {
        base = m_autoBase; overlay = m_autoOverlay;
        blend = CLAMP((m_autoSlope * 2.0 * (normal.y - 1.0) + 1.0) - m_autoHeight * .01 * height, 0.0, 1.0);
    }
    const int material = m_rule["material_id"];
    const double coverage = (base == material ? 1.0 - blend : 0.0) + (overlay == material ? blend : 0.0);
    const PackedInt32Array exclusions = m_rule.get("excluded_materials", PackedInt32Array());
    for (int i = 0; i < exclusions.size(); ++i) {
        if ((base == exclusions[i] && 1.0 - blend > 0.01) || (overlay == exclusions[i] && blend > 0.01)) { return true; }
    }
    if (randomUnit(key ^ UINT64_C(0xaf17e22a)) >= coverage) { return true; }
    const double cluster = CLAMP((double)m_rule.get("cluster", 0.0), 0.0, 1.0);
    if (cluster > 0.0) {
        const double scale = MAX(0.25, (double)m_rule.get("cluster_scale", 12.0));
        const int cx = (int)floor(x / scale), cz = (int)floor(z / scale);
        const double fx = x / scale - cx, fz = z / scale - cz;
        const double sx = fx * fx * (3.0 - 2.0 * fx), sz = fz * fz * (3.0 - 2.0 * fz);
        const uint64_t seed = salt ^ UINT64_C(0x1842fabbff);
        const auto sample = [&](int dx, int dz) {
            return randomUnit(seed ^ hash64((uint64_t)(int64_t)(cx + dx) * UINT64_C(0x9e3779b185ebca87) ^
                                           (uint64_t)(int64_t)(cz + dz) * UINT64_C(0xc2b2ae3d27d4eb4f)));
        };
        const double a = sample(0,0) * (1-sx) + sample(1,0) * sx;
        const double b = sample(0,1) * (1-sx) + sample(1,1) * sx;
        if (randomUnit(key ^ UINT64_C(0x80e6d4)) > (1-cluster) + cluster * (a * (1-sz) + b * sz)) { return true; }
    }
    m_pendingPosition = Vector3((real_t)x, height, (real_t)z);
    m_pendingNormal = normal; m_pendingKey = key;
    m_neighbor = 0; m_neighborPoint = 0; m_pending = true;
    return true;
}
bool Terrain3DFoliageGenerator::checkSpacing(int &rBudget) {
    if (m_spacing <= 0.0) { return acceptCandidate(); }
    while (m_neighbor < 9 && rBudget > 0) {
        const Vector2i cell = m_cell + Vector2i(m_neighbor % 3 - 1, m_neighbor / 3 - 1);
        TypedArray<Vector3> positions;
        if (cell == m_cell) { positions = m_accepted; }
        else if (cellColor(cell) < m_phase && m_cells.has(cell)) { positions = m_cells[cell]; }
        if (m_neighborPoint >= positions.size()) {
            --rBudget; ++m_neighborTests; ++m_neighbor; m_neighborPoint = 0; continue;
        }
        const Vector3 other = positions[m_neighborPoint++];
        --rBudget; ++m_neighborTests;
        const double dx = (double)other.x - m_pendingPosition.x, dz = (double)other.z - m_pendingPosition.z;
        if (dx * dx + dz * dz < m_spacing * m_spacing) { m_pending = false; return true; }
    }
    if (m_neighbor == 9) { return acceptCandidate(); }
    return true;
}
bool Terrain3DFoliageGenerator::acceptCandidate() {
    m_pending = false;
    if (m_spacing > 0.0) { m_accepted.push_back(m_pendingPosition); m_scratchBytes += 24; }
    const Vector2i region((int)floor((double)m_pendingPosition.x / 128.0),
                         (int)floor((double)m_pendingPosition.z / 128.0));
    if (!m_groupRules.has(region)) { return true; }
    const Dictionary rule = m_groupRules[region];
    const double minScale = rule.get("min_scale", 1.0), maxScale = rule.get("max_scale", 1.0);
    const real_t scale = (real_t)(minScale + (maxScale - minScale) * randomUnit(m_pendingKey ^ UINT64_C(0x917eed)));
    const real_t yaw = (bool)rule.get("random_yaw", true) ? (real_t)(randomUnit(m_pendingKey ^ UINT64_C(0x8c815)) * 6.283185307179586) : 0.0f;
    const real_t alignment = (real_t)CLAMP((double)rule.get("normal_align", 0.0), 0.0, 1.0);
    const Basis basis = Basis(Quaternion().slerp(Quaternion(Vector3(0,1,0), m_pendingNormal), alignment)) * Basis(Vector3(0,1,0), yaw);
    TypedArray<Transform3D> transforms = m_outputs.get(region, TypedArray<Transform3D>());
    transforms.push_back(Transform3D(basis.scaled(Vector3(scale,scale,scale)), m_pendingPosition));
    m_outputs[region] = transforms;
    ++m_added; m_scratchBytes += 128;
    const int count = (int)m_regionCounts.get(region, 0) + 1;
    m_regionCounts[region] = count;
    if (m_added > m_jobCap || count > m_regionCap || m_scratchBytes > m_scratchCap) {
        m_error = "Foliage placement or scratch budget exceeded"; return false;
    }
    return true;
}

Dictionary Terrain3DFoliageGenerator::step(int nBudget) {
    Dictionary budgets;
    budgets["candidates"] = nBudget;
    return stepBudgeted(budgets);
}
Dictionary Terrain3DFoliageGenerator::stepBudgeted(const Dictionary &rBudgets) {
    if (!m_running) { return status(); }
    const uint64_t begin = Time::get_singleton()->get_ticks_usec();
    const uint64_t deadline = begin + (uint64_t)CLAMP((int)rBudgets.get("usec", 2000), 100, 50000);
    int candidates = CLAMP((int)rBudgets.get("candidates", 2048), 1, 65536);
    int neighbors = CLAMP((int)rBudgets.get("neighbors", 4096), 1, 262144);
    int samples = CLAMP((int)rBudgets.get("samples", 4096), 2, 131072);
    int transitions = CLAMP((int)rBudgets.get("transitions", 256), 1, 65536);
    if (!sourcesUnchanged()) {
        m_failed = true; m_running = false;
        if (m_pSampler) { memdelete(m_pSampler); m_pSampler = nullptr; }
        return status();
    }
    while (m_running && Time::get_singleton()->get_ticks_usec() < deadline) {
        if (m_group >= m_groups.size()) { m_running = false; break; }
        if (m_pending) {
            if (neighbors <= 0) { break; }
            // At most one neighbor distance/cell lookup per iteration, allowing
            // both the independent neighbor quota and wall clock to yield.
            int one = 1;
            if (!checkSpacing(one)) { m_failed = true; break; }
            neighbors -= 1 - one;
            continue;
        }
        if (!m_groupReady) {
            if (transitions-- <= 0) { break; } ++m_transitions;
            if (!prepareGroup()) { m_failed = true; break; }
            continue;
        }
        if (m_region >= m_domain.size()) {
            if (transitions-- <= 0) { break; } ++m_transitions;
            if (m_phase + 1 < (m_spacing > 0.0 ? 4 : 1)) {
                ++m_phase; m_region = 0; m_domainReady = false; continue;
            }
            if (m_finish < m_domain.size()) {
                const Vector2i region = m_domain[m_finish++];
                const Dictionary rule = m_groupRules[region];
                Dictionary batch;
                batch["region"] = region; batch["owner"] = rule["owner"]; batch["mesh_id"] = rule["mesh_id"];
                batch["transforms"] = m_outputs.get(region, TypedArray<Transform3D>());
                m_batches.push_back(batch);
            } else { ++m_group; m_groupReady = false; }
            continue;
        }
        if (!m_domainReady) {
            if (transitions-- <= 0) { break; } ++m_transitions;
            prepareDomain(); continue;
        }
        if (!m_cellReady) {
            if (transitions-- <= 0) { break; } ++m_transitions;
            if (!prepareCell()) { m_failed = true; break; }
            continue;
        }
        if (m_candidate >= m_candidateCount) {
            if (transitions-- <= 0) { break; } ++m_transitions;
            m_cells[m_cell] = m_accepted.duplicate();
            advanceCell(); continue;
        }
        if (candidates <= 0 || samples < 2) { break; }
        --candidates; samples -= 2;
        if (!sampleCandidate()) { m_failed = true; break; }
    }
    if (m_failed) { m_running = false; m_batches.clear(); }
    if (!m_running && m_pSampler) { memdelete(m_pSampler); m_pSampler = nullptr; }
    m_lastUsec = (int64_t)(Time::get_singleton()->get_ticks_usec() - begin);
    m_maxUsec = MAX(m_maxUsec, m_lastUsec);
    return status();
}
void Terrain3DFoliageGenerator::cancel() {
    if (m_pSampler) { memdelete(m_pSampler); m_pSampler = nullptr; }
    m_pTerrain = nullptr; m_groups.clear(); m_regions.clear(); m_batches.clear();
    m_cells.clear(); m_outputs.clear(); m_domain.clear(); m_accepted.clear(); m_assetBindings.clear();
    m_groupRules.clear(); m_rule.clear(); m_frozenSources.clear(); m_regionCounts.clear();
    m_group = 0; m_region = 0; m_added = 0; m_scratchBytes = 0;
    m_running = false; m_failed = false; m_groupReady = false; m_domainReady = false; m_cellReady = false; m_pending = false;
    m_candidatesTested = 0; m_neighborTests = 0; m_samples = 0; m_transitions = 0; m_lastUsec = 0; m_maxUsec = 0;
}
Array Terrain3DFoliageGenerator::takeBatches() {
    if (m_running || m_failed || !m_error.is_empty()) { return Array(); }
    Array result = m_batches; m_batches.clear(); return result;
}
Dictionary Terrain3DFoliageGenerator::status() const {
    Dictionary result;
    result["running"] = m_running;
    result["ready"] = !m_running && !m_failed && m_error.is_empty() && !m_batches.is_empty();
    result["error"] = m_error; result["instances"] = m_added;
    result["region_index"] = m_region; result["region_count"] = m_domain.size();
    result["rule_index"] = m_group; result["rule_uid"] = m_rule.get("uid", "");
    result["progress"] = m_groups.is_empty() ? 0.0 : CLAMP((double)m_group / m_groups.size(), 0.0, 1.0);
    result["candidate_tests"] = m_candidatesTested; result["neighbor_checks"] = m_neighborTests;
    result["samples"] = m_samples; result["transitions"] = m_transitions;
    result["scratch_bytes"] = m_scratchBytes; result["source_copy_bytes"] = 0;
    result["last_step_usec"] = m_lastUsec; result["max_step_usec"] = m_maxUsec;
    return result;
}
void Terrain3DFoliageGenerator::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start", "terrain", "config", "regions"), &Terrain3DFoliageGenerator::start);
    ClassDB::bind_method(D_METHOD("step", "budget"), &Terrain3DFoliageGenerator::step, DEFVAL(2048));
    ClassDB::bind_method(D_METHOD("step_budgeted", "budgets"), &Terrain3DFoliageGenerator::stepBudgeted);
    ClassDB::bind_method(D_METHOD("cancel"), &Terrain3DFoliageGenerator::cancel);
    ClassDB::bind_method(D_METHOD("take_batches"), &Terrain3DFoliageGenerator::takeBatches);
    ClassDB::bind_method(D_METHOD("status"), &Terrain3DFoliageGenerator::status);
}

