#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src/rendering/rd/terrain_3d_render_coordinator.cpp").read_text(encoding="utf-8")
HDR = (ROOT / "src/rendering/rd/terrain_3d_render_coordinator.h").read_text(encoding="utf-8")

def fail(msg):
    print("FAIL:", msg, file=sys.stderr)
    raise SystemExit(1)

for needle, label in [
    ("t3dAtomicBool_t m_bInstanceRendererInitialized{ false };", "instance companion readiness state"),
    ("t3dAtomicBool_t m_bOceanRendererInitialized{ false };", "ocean companion readiness state"),
    ("bool m_bForwardPreparedHasInstances = false;", "prepared instance companion state"),
    ("const bool terrain_ready = shader_ready", "terrain-primary renderer initialization"),
    ("m_bRendererInitialized.store( terrain_ready", "terrain-only primary initialized state"),
    ("const bool terrain_ok = !bHasTerrain", "compositor terrain result independent of instance result"),
    ("const bool terrain_ok = !has_terrain", "Forward+ terrain prepare result"),
    ("m_bForwardPreparedHasInstances = !transparent_pass && instance_lane_ready && instances_ok;", "isolated Forward+ instance preparation"),
    ("result[\"terrain_renderer_initialized\"]", "terrain renderer diagnostic"),
    ("result[\"instance_renderer_initialized\"]", "instance renderer diagnostic"),
    ("result[\"ocean_renderer_initialized\"]", "ocean renderer diagnostic"),
]:
    hay = HDR if needle.startswith("t3dAtomicBool_t") or needle.startswith("bool m_bForward") else CPP
    if needle not in hay:
        fail("missing " + label)

for forbidden, label in [
    ("instances_ok && m_renderer.render", "compositor instance result gating terrain"),
    ("instance_ready && m_oceanRenderer.initialize", "instance initialization gating ocean"),
]:
    if forbidden in CPP:
        fail("forbidden coupling remains: " + label)

# Primary callback success must follow terrain, not companion lanes.
prepare_tail = CPP[CPP.find("if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE )"):]
if "if ( terrain_ok )" not in prepare_tail:
    fail("Forward+ PREPARE does not use terrain as the primary success condition")
if "if ( m_bForwardPreparedHasInstances )" not in prepare_tail or "( void )m_instanceRenderer.renderForwardPass" not in prepare_tail:
    fail("Forward+ SUBMIT does not isolate companion instance submission")

print("PASS: terrain rendering is the primary lane; instance/HLOD/ocean failures cannot veto a valid terrain pass")
