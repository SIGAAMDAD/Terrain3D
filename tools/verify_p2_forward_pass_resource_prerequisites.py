#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
BRIDGE_CPP = ROOT / "src/rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.cpp"

text = BRIDGE_CPP.read_text(encoding="utf-8")
failures = []

def require(condition, label):
    if not condition:
        failures.append(label)

helper_start = text.find("static bool t3dRdForwardPassRequiresBorrowedResources")
prepare_start = text.find("uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepare(")
helper = text[helper_start:prepare_start] if helper_start >= 0 and prepare_start >= 0 else ""
require(bool(helper), "pass-specific borrowed-resource prerequisite helper exists")
require("T3D_RD_FORWARD_PASS_OPAQUE" in helper, "opaque pass requires borrowed resources")
require("T3D_RD_FORWARD_PASS_MOTION" in helper, "motion pass requires borrowed resources")
require("T3D_RD_FORWARD_PASS_TRANSPARENT" in helper, "transparent pass requires borrowed resources")
require("T3D_RD_FORWARD_PASS_DEPTH" not in helper, "depth pass does not require borrowed resources")
require("T3D_RD_FORWARD_PASS_SHADOW" not in helper, "shadow pass remains on dedicated shadow packet path")

prepare_end = text.find("uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareShadow", prepare_start)
prepare = text[prepare_start:prepare_end] if prepare_start >= 0 and prepare_end >= 0 else ""
require(bool(prepare), "ordinary PREPARE implementation exists")
require("if ( accepted && pResources )" in prepare and "accepted = convertResources( *pResources );" in prepare,
        "supplied borrowed-resource packets are still validated on every ordinary pass")
require("if ( accepted && !pResources && t3dRdForwardPassRequiresBorrowedResources" in prepare,
        "missing borrowed resources are rejected only for passes that require them")
require("accepted = m_nCachedResourceGeneration != 0u;" in prepare,
        "resource-requiring passes may use a previously validated cached resource packet")
require("pResources ? !convertResources" not in prepare,
        "ordinary PREPARE no longer globally couples all passes to borrowed resources")
require("pResources ? !convertResources( *pResources ) : m_nCachedResourceGeneration == 0u" not in prepare,
        "legacy unconditional resource prerequisite is removed")

# Ensure pass conversion happens before the prerequisite test so the current pass kind
# is authoritative when deciding whether borrowed resources are mandatory.
convert_pass_pos = prepare.find("convertPass( *pPass )")
requires_pos = prepare.find("t3dRdForwardPassRequiresBorrowedResources")
require(convert_pass_pos >= 0 and requires_pos > convert_pass_pos,
        "pass packet is validated before pass-specific resource prerequisite is evaluated")

if failures:
    print("P2 Forward+ pass resource prerequisites verification FAIL")
    for failure in failures:
        print(" -", failure)
    sys.exit(1)

print("P2 Forward+ pass resource prerequisites verification PASS")
