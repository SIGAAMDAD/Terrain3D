#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
renderer = (ROOT / "src/rendering/rd/terrain_3d_rd_instance_renderer.cpp").read_text(encoding="utf-8")
header = (ROOT / "src/rendering/rd/terrain_3d_rd_instance_renderer.h").read_text(encoding="utf-8")
coordinator = (ROOT / "src/rendering/rd/terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8")
util = (ROOT / "src/core/terrain_3d_util.cpp").read_text(encoding="utf-8")

checks = {
    "forward set0 is shader-variant specific": "uniform_set_create( uniforms, m_hszForwardShaders[variant], 0u )" in renderer,
    "forward material set1 is shader-variant specific": "uniform_set_create( uniforms, m_hszForwardShaders[variant], 1u )" in renderer,
    "forward draw binds variant set0": "m_hszForwardShadowUniformSets[variant] : m_hszForwardUniformSets[variant]" in renderer,
    "forward draw binds variant material set": "getForwardMaterialBinding( packet.m_nMaterial, packet.m_nOverlayMaterial, variant )" in renderer,
    "transparent pass is accepted": "rContext.m_passKind != T3D_RD_FORWARD_PASS_TRANSPARENT" in renderer,
    "transparent pipeline matches Forward+ three-slot blend contract": "blend_count = 3u;" in renderer
        and "const bool blend_color = transparent_pass && i == 0u;" in renderer
        and "a->set_enable_blend( blend_color )" in renderer
        and "BLEND_FACTOR_SRC_ALPHA" in renderer
        and "BLEND_FACTOR_ONE_MINUS_SRC_ALPHA" in renderer,
    "transparent pass disables depth writes": "set_enable_depth_write( !transparent_pass && !key.m_bDepthPrepassActive )" in renderer,
    "transparent packets are submitted only in transparent pass": "if ( !alpha_blend )" in renderer
        and "const bool transparent_pass = rContext.m_passKind == T3D_RD_FORWARD_PASS_TRANSPARENT" in renderer,
    "coordinator prepares instances in transparent pass": "const bool instances_ok = !instance_lane_ready" in coordinator
        and "transparent_pass || !instance_lane_ready" not in coordinator,
    "coordinator does not swallow instance submit": ( "instances_ok = m_instanceRenderer.renderForwardPass( rContext )" in coordinator
        or "instances_ok = m_instanceRenderer.renderForwardPass( instance_context )" in coordinator )
        and "( void )m_instanceRenderer.renderForwardPass( rContext )" not in coordinator
        and "( void )m_instanceRenderer.renderForwardPass( instance_context )" not in coordinator,
    "coordinator exposes instance failures": "instance_forward_prepare_failures" in coordinator
        and "instance_forward_submit_failures" in coordinator
        and "instance_forward_last_error" in coordinator,
    "dictionary debug lookup preserves original Variant key": "const Variant key_variant" in util
        and "rDict.get( key_variant, Variant() )" in util
        and "const Variant var = rDict[key]" not in util,
    "header owns forward uniform set banks": "m_hszForwardUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX]" in header
        and "m_hszForwardShadowUniformSets[T3D_RD_FORWARD_SHADER_VARIANT_MAX]" in header,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + name)
if failed:
    print(f"instance forward submission verifier failed: {len(failed)} check(s)", file=sys.stderr)
    sys.exit(1)
print("PASS: instance Forward+ submission / transparency / diagnostics regression gate")
