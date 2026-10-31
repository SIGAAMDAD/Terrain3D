#include "diagnostics/tests/systems/test_harness.h"
#include "authoring/vt/visibility/terrain_3d_vt_visibility_baker.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include <string.h>

namespace terrain3d_tests
{
namespace
{
void fillVisibilityScene( t3dVTVisibilityStaticVertex_t *vertices,
                          t3dVTVisibilityStaticTriangle_t *triangles ) {
    // Camera at origin looking +X: a larger near wall conservatively covers
    // a smaller far wall. The far material must not contribute to +X view.
    const float corners[4][2] = { {-1.0f,-1.0f}, {1.0f,-1.0f},
                                   {1.0f,1.0f}, {-1.0f,1.0f} };
    for ( uint32_t wall = 0u; wall < 2u; ++wall ) {
        for ( uint32_t i = 0u; i < 4u; ++i ) {
            const uint32_t id = wall * 4u + i;
            t3dVTVisibilityStaticVertex_t &v = vertices[id];
            v.m_fWorldX = wall == 0u ? 3.0f : 6.0f;
            v.m_fWorldY = corners[i][0] * ( wall == 0u ? 4.0f : 2.0f );
            v.m_fWorldZ = corners[i][1] * ( wall == 0u ? 4.0f : 2.0f );
            v.m_fU = corners[i][0] * 0.5f + 0.5f;
            v.m_fV = corners[i][1] * 0.5f + 0.5f;
        }
        for ( uint32_t face = 0u; face < 2u; ++face ) {
            const uint32_t id = wall * 2u + face;
            auto &t = triangles[id];
            t.m_nMaterialKeyIndex = wall;
            t.m_nFlags = 0u;
            const uint32_t quad[6] = { 0u, 1u, 2u, 0u, 2u, 3u };
            for ( uint32_t v = 0u; v < 3u; ++v )
                t.m_nIndices[v] = wall * 4u + quad[face * 3u + v];
        }
    }
}
} // namespace

void register_vt_visibility_baker_tests( TestRunner &runner ) {
    runner.run( "vt_visibility_baker", "t02_static_surface_probe_selection", []( TestContext &test ) {
        // A 128m flat authored cell yields five deterministic world-space
        // probe locations and correctly handles negative cell coordinates.
        t3dVTVisibilityStaticVertex_t vertices[4]{};
        vertices[0] = { -128.0f, 5.0f, -128.0f, 0.0f, 0.0f };
        vertices[1] = {    0.0f, 5.0f, -128.0f, 1.0f, 0.0f };
        vertices[2] = {    0.0f, 5.0f,    0.0f, 1.0f, 1.0f };
        vertices[3] = { -128.0f, 5.0f,    0.0f, 0.0f, 1.0f };
        t3dVTVisibilityStaticTriangle_t triangles[2]{};
        triangles[0].m_nIndices[0] = 0u;
        triangles[0].m_nIndices[1] = 1u;
        triangles[0].m_nIndices[2] = 2u;
        triangles[1].m_nIndices[0] = 0u;
        triangles[1].m_nIndices[1] = 2u;
        triangles[1].m_nIndices[2] = 3u;
        std::vector< t3dVTVisibilityProbeInput_t > probes;
        T3D_REQUIRE( test, Terrain3DVTVisibilityBaker::selectCellProbes(
            vertices, 4u, triangles, 0u, 2u, -1, -1, 9u, 256u, probes ) );
        T3D_CHECK( test, probes.size() == 5u );
        if ( probes.size() == 5u ) {
            T3D_CHECK( test, near( probes[0].m_fWorldX, -64.0f ) &&
                near( probes[0].m_fWorldY, 6.7f ) && near( probes[0].m_fWorldZ, -64.0f ) );
            T3D_CHECK( test, near( probes[4].m_fWorldX, -32.0f ) &&
                near( probes[4].m_fWorldZ, -32.0f ) );
        }
        triangles[0].m_nFlags = 1u;
        triangles[1].m_nFlags = 1u;
        T3D_CHECK( test, !Terrain3DVTVisibilityBaker::selectCellProbes(
            vertices, 4u, triangles, 0u, 2u, -1, -1, 9u, 256u, probes ) && probes.empty() );
    } );
    runner.run( "vt_visibility_baker", "t02_invalid_keys_indices_and_bounds_rejected", []( TestContext &test ) {
        t3dVTVisibilityStaticVertex_t vertices[8]{};
        t3dVTVisibilityStaticTriangle_t triangles[4]{};
        fillVisibilityScene( vertices, triangles );
        uint64_t keys[2] = { 11u, 22u };
        t3dVTVisibilityProbeInput_t probe;
        probe.m_nSourceRevision = 2u;
        probe.m_nFirstTriangle = 0u;
        probe.m_nTriangleCount = 4u;
        probe.m_nNominalRenderSize = 128u;
        t3dVTVisibilityStaticSceneView_t scene;
        scene.m_pVertices = vertices;
        scene.m_nVertexCount = 8u;
        scene.m_pTriangles = triangles;
        scene.m_nTriangleCount = 4u;
        scene.m_pMaterialKeys = keys;
        scene.m_nMaterialCount = 2u;
        scene.m_pProbes = &probe;
        scene.m_nProbeCount = 1u;
        scene.m_nSourceSignature = 1u;
        t3dVTDemandVisibilityCookConfig_t config;
        config.m_nSourceSignature = 1u;
        config.m_nPackageSignature = 2u;
        config.m_nNominalTextureSize = 1024u;
        Terrain3DVTVisibilityBaker baker;
        std::vector< t3dVTVisibilityProbeResult_t > results;
        t3dVTVisibilityBakeStats_t stats;
        keys[1] = 11u;
        T3D_CHECK( test, !baker.bake( scene, config, results, stats ) );
        keys[1] = 22u;
        triangles[0].m_nIndices[0] = 999u;
        T3D_CHECK( test, !baker.bake( scene, config, results, stats ) );
        triangles[0].m_nIndices[0] = 0u;
        probe.m_fFovDegrees = 180.0f;
        T3D_CHECK( test, !baker.bake( scene, config, results, stats ) );
        probe.m_fFovDegrees = 90.0f;
        config.m_nSourceSignature = 3u;
        T3D_CHECK( test, !baker.bake( scene, config, results, stats ) );
    } );
    runner.run( "vt_visibility_baker", "t02_gpu_occlusion_and_16_bin_histogram", []( TestContext &test ) {
        if ( Terrain3DRdContext::getGlobalDevice() == nullptr ) {
            // A missing Vulkan RD is an explicit skip, never a green result.
            test.skip( "Vulkan RenderingDevice unavailable" );
            return;
        }
        t3dVTVisibilityStaticVertex_t vertices[8]{};
        t3dVTVisibilityStaticTriangle_t triangles[4]{};
        fillVisibilityScene( vertices, triangles );
        const uint64_t keys[2] = { 11u, 22u };
        t3dVTVisibilityProbeInput_t probe;
        probe.m_nCellX = 0;
        probe.m_nCellZ = 0;
        probe.m_fWorldX = 0.1f;
        probe.m_nSourceRevision = 2u;
        probe.m_nFirstTriangle = 0u;
        probe.m_nTriangleCount = 4u;
        probe.m_nNominalRenderSize = 128u;
        t3dVTVisibilityStaticSceneView_t scene;
        scene.m_pVertices = vertices;
        scene.m_nVertexCount = 8u;
        scene.m_pTriangles = triangles;
        scene.m_nTriangleCount = 4u;
        scene.m_pMaterialKeys = keys;
        scene.m_nMaterialCount = 2u;
        scene.m_pProbes = &probe;
        scene.m_nProbeCount = 1u;
        scene.m_nSourceSignature = 123u;
        t3dVTDemandVisibilityCookConfig_t config;
        config.m_nSourceSignature = scene.m_nSourceSignature;
        config.m_nPackageSignature = 987u;
        config.m_nNominalTextureSize = 1024u;
        Terrain3DVTVisibilityBaker baker;
        std::vector< t3dVTVisibilityProbeResult_t > results;
        t3dVTVisibilityBakeStats_t stats;
        T3D_REQUIRE( test, baker.bake( scene, config, results, stats ) );
        T3D_REQUIRE( test, results.size() == 1u );
        const auto &r = results[0];
        T3D_CHECK( test, r.m_counts.size() == 256u && stats.m_nGpuPasses == 20u &&
            r.m_nOpaqueTriangles == 4u && r.m_nUncapturedTriangles == 0u );
        uint64_t visibleNear = 0u, visibleFar = 0u;
        for ( uint32_t mip = 0u; mip < 16u; ++mip ) {
            visibleNear += r.m_counts[mip];
            visibleFar += r.m_counts[128u + mip];
        }
        T3D_CHECK( test, visibleNear > 0u && visibleFar == 0u );
        // Fragment-order-independent, two-pass visibility must be deterministic
        // across identical offline bakes.
        const std::vector< uint32_t > firstCounts = r.m_counts;
        T3D_REQUIRE( test, baker.bake( scene, config, results, stats ) );
        T3D_CHECK( test, results.size() == 1u &&
            results[0].m_counts == firstCounts && stats.m_nGpuPasses == 20u );
        // Cutout/animated triangles are never accepted as opaque visibility;
        // their absence must be propagated as PARTIAL for reactive fallback.
        triangles[2].m_nFlags = 1u;
        T3D_REQUIRE( test, baker.bake( scene, config, results, stats ) );
        T3D_CHECK( test, results[0].m_bPartial &&
            results[0].m_nUncapturedTriangles == 1u );
    } );
}
} // namespace terrain3d_tests
