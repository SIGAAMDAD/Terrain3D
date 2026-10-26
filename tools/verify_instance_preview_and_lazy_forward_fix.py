#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
renderer_h = (root / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.h").read_text()
renderer = (root / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp").read_text()
coordinator = (root / "src/rendering/rd/core/terrain_3d_render_coordinator.cpp").read_text()

checks = [
    ("preview has dedicated base-raster initializer", "bool initializePreview( const Ref<RDShaderSPIRV> &rShader );" in renderer_h),
    ("coordinator uses preview-only initializer", "m_previewRenderer.initializePreview( m_shaderLibrary.getInstances() )" in coordinator),
    ("preview readiness depends only on its base instance shader", "m_shaderLibrary.getInstances().is_null()" in coordinator and "!m_bPendingPreviewMeshesValid || !m_shaderLibrary.isReady()" not in coordinator),
    ("preview no longer requires Forward/HLOD/runtime cull bundles", "m_previewRenderer.initialize( m_shaderLibrary.getInstances()" not in coordinator),
    ("preview remaps clip Z to RenderingDevice 0..1", "depth_correction.set_depth_correction( false, false, true );" in renderer),
    ("same corrected preview matrix reaches culling and raster", "const Projection view_projection = depth_correction * projection * view;" in renderer),
    ("live initialization caches Forward bundle", "m_forwardShaderBundle = rForwardShaders;" in renderer),
    ("live initialization does not eagerly build all Forward variants", "!ensureForwardShaders( rForwardShaders )" not in renderer),
    ("Forward shader creation is pass-lazy", "ensureForwardShaderVariant( variant )" in renderer),
    ("base instance initialization does not require HLOD compute", "!ensureHlodCullShader( rHlodCullShader )" not in renderer),
    ("HLOD compute is lazy when descriptors exist", "ensureHlodCullShader( m_hlodCullSourceShader )" in renderer),
    ("lazy Forward/HLOD resource creation uses rebuild ownership", renderer.count("beginResourceRebuild()") >= 2 and "m_context.endResourceRebuild();" in renderer),
    ("diagnostics expose instantiated Forward variants", "instance_forward_shader_ready_mask" in renderer),
]

failed = [name for name, ok in checks if not ok]
if failed:
    for name in failed:
        print("FAIL:", name)
    raise SystemExit(1)
for name, _ in checks:
    print("PASS:", name)
print("PASS: instance preview depth and lazy Forward/HLOD capability fix")
