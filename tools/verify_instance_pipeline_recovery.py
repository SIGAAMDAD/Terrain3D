#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
coordinator = (ROOT / "src/rendering/rd/core/terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8")
coordinator_h = (ROOT / "src/rendering/rd/core/terrain_3d_render_coordinator.h").read_text(encoding="utf-8")
renderer = (ROOT / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8")
renderer_h = (ROOT / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8")
mesh_h = (ROOT / "src/rendering/rd/instances/terrain_3d_rd_mesh_cache.h").read_text(encoding="utf-8")

checks = {
    "terrain initialization no longer early-returns before companion recovery":
        "if ( m_bRendererInitialized.load( T3D_MEMORY_ORDER_ACQUIRE ) ) {\n\t\treturn true;\n\t}" not in coordinator,
    "instance renderer retries independently":
        "const bool instance_retry_allowed" in coordinator
        and "!instance_ready && ( nRenderFrame == UINT64_MAX || m_nLastInstanceInitializationRenderFrame != nRenderFrame )" in coordinator,
    "failed partial instance initialization is cleaned before retry":
        "const String instance_error = m_instanceRenderer.getLastError();" in coordinator
        and "m_instanceRenderer.shutdown();" in coordinator,
    "instance initialization recovery telemetry exists":
        "m_nInstanceInitializationAttempts" in coordinator_h
        and "instance_initialization_attempts" in coordinator
        and "instance_initialization_recoveries" in coordinator,
    "GPU topology fade starts fully visible":
        "T3D_RD_INSTANCE_FADE_ACTIVE | T3D_RD_INSTANCE_FADE_MASK" in renderer,
    "forward fade always writes an explicit encoded visibility":
        "float fade = 1.0f;" in renderer
        and "draw_flags &= ~T3D_RD_INSTANCE_FADE_MASK;" in renderer
        and "fade_unorm << T3D_RD_INSTANCE_FADE_SHIFT" in renderer,
    "production indirect addressing coexists with isolated CPU reference geometry":
        "rd->draw_list_draw_indirect( draw_list" in renderer
        and ( "getSurfaceVertexArray" in renderer or "getDirectSurfaceVertexArray" in renderer )
        and "rd->draw_list_draw( draw_list, true, packet.m_nVisibleCount, 0u )" in renderer,
    "opaque depth equality requires successful instance depth submit":
        "m_nInstanceLastSuccessfulDepthScenePass != rContext.m_nScenePass" in coordinator
        and "instance_context.m_bDepthPrepassActive = false;" in coordinator
        and "m_nInstanceLastSuccessfulDepthScenePass = rContext.m_nScenePass;" in coordinator,
    "instance depth/opaque stage telemetry exists":
        "instance_depth_prepare_attempts" in coordinator
        and "instance_depth_submit_successes" in coordinator
        and "instance_opaque_prepare_attempts" in coordinator
        and "instance_opaque_submit_successes" in coordinator
        and "instance_depth_prepass_fallbacks" in coordinator,
    "publication stage gauges exist":
        "published_instance_records" in coordinator
        and "published_instance_cells" in coordinator
        and "published_mesh_lods" in coordinator
        and "published_mesh_surfaces" in coordinator
        and "published_mesh_vertex_bytes" in coordinator,
    "mesh resolution gauges exist":
        "m_nResolvedMeshCells" in renderer_h
        and "m_nUnresolvedMeshCells" in renderer_h
        and "instance_resolved_mesh_cells" in renderer
        and "instance_unresolved_mesh_cells" in renderer,
    "per-pass submitted draw telemetry exists":
        "m_nDepthSubmittedDraws" in renderer_h
        and "instance_depth_submitted_draws" in renderer
        and "instance_opaque_submitted_draws" in renderer,
    "mesh cache exposes lod count": "getLodCount" in mesh_h,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + name)
if failed:
    print(f"instance pipeline recovery verifier failed: {len(failed)} check(s)", file=sys.stderr)
    sys.exit(1)
print("PASS: instance recovery/fade/depth-fallback/stage-telemetry regression gate")
