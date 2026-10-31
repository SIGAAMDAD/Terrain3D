#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
COORD_H = ROOT / "src/rendering/rd/core/terrain_3d_render_coordinator.h"
COORD_CPP = ROOT / "src/rendering/rd/core/terrain_3d_render_coordinator.cpp"
BRIDGE_CPP = ROOT / "src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.cpp"
INSTANCE_H = ROOT / "src/rendering/rd/instances/terrain_3d_rd_instance_renderer.h"

coord_h = COORD_H.read_text(encoding="utf-8")
coord_cpp = COORD_CPP.read_text(encoding="utf-8")
bridge_cpp = BRIDGE_CPP.read_text(encoding="utf-8")
instance_h = INSTANCE_H.read_text(encoding="utf-8")

failures = []

def require(condition, label):
    if not condition:
        failures.append(label)

for token in (
    "T3D_RD_FORWARD_LANE_TERRAIN",
    "T3D_RD_FORWARD_LANE_INSTANCES",
    "T3D_RD_FORWARD_LANE_OCEAN",
    "T3D_RD_FORWARD_LANE_RENDER_WORLD",
    "m_nForwardPrepareRequestedLaneMask",
    "m_nForwardPrepareReadyLaneMask",
    "m_nForwardPrepareFailedLaneMask",
    "m_nForwardSubmitAttemptedLaneMask",
    "m_nForwardSubmitSucceededLaneMask",
    "m_nForwardSubmitFailedLaneMask",
    "m_bForwardInstanceLaneExpected",
    "m_bForwardInstancePreparedHasDraws",
    "m_nForwardInstanceSubmitDraws",
):
    require(token in coord_h, f"coordinator declares lane diagnostic state: {token}")

require("getForwardLaneDiagnostics( void ) const" in coord_h,
        "coordinator exposes non-recursive Forward+ lane diagnostics")
require("hasForwardPreparedDraws( void ) const" in instance_h,
        "instance renderer exposes PREPARE draw evidence without allocating")
require("getForwardSubmittedDrawCount" in instance_h,
        "instance renderer exposes per-pass submitted draw counters")

prepare_start = coord_cpp.find("if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE )")
submit_start = coord_cpp.find("if ( rContext.m_phase != T3D_RD_FORWARD_PHASE_SUBMIT", prepare_start)
prepare_block = coord_cpp[prepare_start:submit_start] if prepare_start >= 0 and submit_start >= 0 else ""
require(bool(prepare_block), "Forward+ PREPARE block exists")
require("m_nForwardPublishedInstanceCount = pScene" in prepare_block,
        "PREPARE records published instance count")
require("m_bForwardInstanceLaneExpected = m_nForwardPublishedInstanceCount > 0u" in prepare_block,
        "published instances explicitly mark the instance lane expected")
require("m_bForwardInstanceLaneExpected && !instance_lane_ready" in prepare_block,
        "expected but uninitialized instance lane is recorded as unavailable")
require("m_nForwardPrepareFailedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES" in prepare_block,
        "instance PREPARE failure is represented in the lane mask")
require("m_instanceRenderer.hasForwardPreparedDraws()" in prepare_block,
        "instance PREPARE records whether draw packets actually exist")
require("m_nInstancePreparedWithoutDraws" in prepare_block,
        "successful PREPARE with no instance draws is counted separately")

submit_end = coord_cpp.find("static size_t t3dRenderCoordinatorMeshSnapshotBytes", submit_start)
submit_block = coord_cpp[submit_start:submit_end] if submit_start >= 0 and submit_end >= 0 else ""
require(bool(submit_block), "Forward+ SUBMIT block exists")
require("submitted_draws_before = m_instanceRenderer.getForwardSubmittedDrawCount" in submit_block,
        "SUBMIT snapshots instance draw count before rendering")
require("submitted_draws_after = m_instanceRenderer.getForwardSubmittedDrawCount" in submit_block,
        "SUBMIT snapshots instance draw count after rendering")
require("m_nForwardInstanceSubmitDraws =" in submit_block
        and "submitted_draws_after >= submitted_draws_before ? submitted_draws_after - submitted_draws_before : 0u;" in submit_block,
        "SUBMIT stores actual instance draw delta")
require("m_nForwardSubmitSucceededLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES" in submit_block,
        "successful instance SUBMIT is represented independently")
require("m_nForwardSubmitFailedLaneMask |= T3D_RD_FORWARD_LANE_INSTANCES" in submit_block,
        "failed instance SUBMIT is represented independently")

lane_diag_start = coord_cpp.find("Dictionary Terrain3DRenderCoordinator::getForwardLaneDiagnostics")
lane_diag_end = coord_cpp.find("Dictionary Terrain3DRenderCoordinator::getDiagnostics", lane_diag_start)
lane_diag = coord_cpp[lane_diag_start:lane_diag_end] if lane_diag_start >= 0 and lane_diag_end >= 0 else ""
require(bool(lane_diag), "lane diagnostic dictionary implementation exists")
for key in (
    '"prepare_requested_mask"',
    '"prepare_ready_mask"',
    '"prepare_failed_mask"',
    '"submit_attempted_mask"',
    '"submit_succeeded_mask"',
    '"submit_failed_mask"',
    '"instance_published_records"',
    '"instance_renderer_initialized"',
    '"instance_prepare_attempted"',
    '"instance_prepare_succeeded"',
    '"instance_prepared_has_draws"',
    '"instance_submit_attempted"',
    '"instance_submit_succeeded"',
    '"instance_submit_draws"',
):
    require(key in lane_diag, f"lane diagnostics expose {key}")

bridge_diag_start = bridge_cpp.find("Dictionary Terrain3DRdForwardPlusBridgeAdapter::getDiagnostics")
bridge_diag = bridge_cpp[bridge_diag_start:] if bridge_diag_start >= 0 else ""
require("Array coordinator_lanes" in bridge_diag,
        "bridge diagnostics expose attached coordinator lane states")
require("coordinator->getForwardLaneDiagnostics()" in bridge_diag,
        "bridge diagnostics read lane state without recursive full diagnostics")
require('result["coordinator_lanes"] = coordinator_lanes' in bridge_diag,
        "bridge diagnostic payload publishes coordinator lane array")

# Preserve failure isolation: an instance-lane failure must be observable without
# suppressing a valid terrain pass. Patch 1 changes evidence, not submission policy.
require("if ( terrain_ok )" in prepare_block and "return true;" in prepare_block,
        "terrain remains authoritative for bridge PREPARE acceptance")
require("if ( terrain_ok )" in submit_block and "return true;" in submit_block,
        "terrain remains authoritative for bridge SUBMIT acceptance")

if failures:
    print("P1 Forward+ lane diagnostics verification FAIL")
    for failure in failures:
        print(" -", failure)
    sys.exit(1)

print("P1 Forward+ lane diagnostics verification PASS")
