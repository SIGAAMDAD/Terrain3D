#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/rendering/rd/core/terrain_3d_render_coordinator.h"
CPP = ROOT / "src/rendering/rd/core/terrain_3d_render_coordinator.cpp"
CLIPMAP = ROOT / "src/rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.cpp"
INSTANCES = ROOT / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.cpp"

header = HEADER.read_text(encoding="utf-8")
cpp = CPP.read_text(encoding="utf-8")
clipmap = CLIPMAP.read_text(encoding="utf-8")
instances = INSTANCES.read_text(encoding="utf-8")

failures = []

def require(condition, label):
    if not condition:
        failures.append(label)

state_start = header.find("typedef struct t3dRdForwardViewportState_s")
state_end = header.find("} t3dRdForwardViewportState_t;", state_start)
state_block = header[state_start:state_end] if state_start >= 0 and state_end >= 0 else ""
require(bool(state_block), "Forward+ viewport lifecycle state exists")
require("m_hFramebuffer" not in state_block, "viewport lifecycle state does not retain framebuffer RID")
require("m_nFramebufferFormat" not in state_block, "viewport lifecycle state does not retain framebuffer format")
for field in (
    "m_nViewportKey",
    "m_renderSize",
    "m_renderRegion",
    "m_nViewCount",
    "m_nSampleCount",
):
    require(field in state_block, f"viewport lifecycle retains stable field {field}")

func_start = cpp.find("bool Terrain3DRenderCoordinator::prepareForwardLifecycle")
func_end = cpp.find("bool Terrain3DRenderCoordinator::renderForwardPass", func_start)
func_block = cpp[func_start:func_end] if func_start >= 0 and func_end >= 0 else ""
require(bool(func_block), "prepareForwardLifecycle implementation exists")
require("slot->m_hFramebuffer" not in func_block, "lifecycle equality ignores framebuffer RID")
require("slot->m_nFramebufferFormat" not in func_block, "lifecycle equality ignores framebuffer format")
require("slot->m_renderSize == rContext.m_renderSize" in func_block, "lifecycle equality keeps render size")
require("slot->m_renderRegion == rContext.m_renderRegion" in func_block, "lifecycle equality keeps render region")
require("slot->m_nViewCount == rContext.m_nViewCount" in func_block, "lifecycle equality keeps view count")
require("slot->m_nSampleCount == rContext.m_nSampleCount" in func_block, "lifecycle equality keeps sample count")
require("rHistoryValid = same && !reconfigured_this_frame && rContext.m_bMotionVectorsEnabled" in func_block,
        "temporal history still depends on stable lifecycle and motion vectors")

# Framebuffer format remains pass/pipeline state. This guards against fixing the
# lifecycle bug by accidentally removing native framebuffer compatibility from
# raster pipeline selection.
require("key.m_nFramebufferFormat = rContext.m_nFramebufferFormat" in clipmap,
        "clipmap pipeline remains keyed by native framebuffer format")
require("key.m_nFramebufferFormat = rContext.m_nFramebufferFormat" in instances,
        "instance pipeline remains keyed by native framebuffer format")

if failures:
    print("P0 Forward+ viewport lifecycle verification FAIL")
    for failure in failures:
        print(" -", failure)
    sys.exit(1)

print("P0 Forward+ viewport lifecycle verification PASS")
