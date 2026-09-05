#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    sys.exit(1)


def function_body(path: str, signature: str) -> str:
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.find(signature)
    if start < 0:
        fail(f"missing {signature} in {path}")
    brace = text.find("{", start)
    if brace < 0:
        fail(f"missing function body for {signature}")
    depth = 0
    for index in range(brace, len(text)):
        ch = text[index]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:index]
    fail(f"unterminated function body for {signature}")
    return ""


init = function_body("src/terrain_3d.cpp", "void Terrain3D::initializeInternal( void )")
cold = init.find("Terrain3DColdPhaseScope")
phase = init.find("TERRAIN3D_ALLOCATION_PHASE_INITIALIZATION")
if cold < 0 or phase < 0 or cold > phase:
    fail("initializeInternal must open a cold phase scope before the initialization phase scope")

setter = function_body("src/core/terrain_3d_resources.cpp", "void Terrain3D::setDataDirectory( const String dir )")
if "Terrain3DColdPhaseScope" not in setter:
    fail("setDataDirectory is missing its cold repository-transition phase")
if setter.find("Terrain3DColdPhaseScope") > setter.find("m_pData->loadDirectory( dir )"):
    fail("setDataDirectory opens its cold phase scope after repository loading")

loader = function_body("src/data/terrain_3d_data.cpp", "Error Terrain3DData::loadDirectory( const String &rDir )")
if "Terrain3DColdPhaseScope" not in loader:
    fail("Terrain3DData::loadDirectory is missing its direct-call cold phase")
if loader.find("Terrain3DColdPhaseScope") > loader.find("t3dStagedRepository_t staged"):
    fail("loadDirectory constructs staged repository storage before opening the cold boundary")

rebuild = function_body(
    "src/streaming/terrain_3d_runtime_page_store.cpp",
    "bool Terrain3DRuntimePageStore::rebuildRuntimeSources( const t3dRuntimePageServiceState_t &rState )",
)
fast = rebuild.find("if ( same_config && m_nWorkerThreadCount == m_nEffectivePrepareThreads )")
cold = rebuild.find("Terrain3DColdPhaseScope")
poll = rebuild.find("poll( true )")
if fast < 0 or cold < 0 or poll < 0:
    fail("runtime-page source rebuild phase is incomplete")
if not (fast < cold < poll):
    fail("runtime-page cold phase scope must follow the fast no-rebuild check and precede blocking rebuild work")

print("PASS: READY/data-directory cold phase boundaries")
