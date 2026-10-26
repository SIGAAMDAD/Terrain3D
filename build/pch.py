"""Opt-in MSVC PCH for Terrain3D's existing per-flavor SCons source sets.

The caller must supply the fully configured editor/runtime SCons environment and
MUST compile only ordinary C++ sources with the returned environment. Generated,
ISA-specialized, registration and test files retain a PCH-disabled environment.
There is deliberately no source glob or modification to the authoritative sets.
"""

from pathlib import Path

PCH_HEADER = "terrain_3d_pch.h"
PCH_SOURCE = "src/core/build/terrain_3d_pch.cpp"
VALID_FLAVORS = ("editor", "runtime")
VALID_MODES = ("off", "stable")

# Directory ownership is declarative: each subsystem keeps its own project
# headers; no project-owned headers are added to the shared PCH.
SUBSYSTEMS = (
    "assets", "authoring", "core", "data", "diagnostics", "hlod",
    "instances", "integration", "lighting", "memory", "physics",
    "rendering", "simd", "streaming",
)


def pch_eligible(src_path):
    """Conservative TU filter; source-set owners may exclude more units."""
    path = Path(str(src_path).replace("\\", "/"))
    s = path.as_posix().lower()
    if not s.endswith(".cpp") or s.endswith("terrain_3d_pch.cpp"):
        return False
    if "/diagnostics/tests/" in "/" + s or "/simd/backends/" in "/" + s:
        return False
    if s.endswith(("/register_types.cpp", "/doc_data.gen.cpp")):
        return False
    if "/generated/" in "/" + s or "/gen/" in "/" + s:
        return False
    return True


def build_flavor_pch(env, flavor, mode="off", build_dir="build/obj", source_root="."):
    """Return (PCH-enabled environment clone, PCH targets).

    Invoke after editor/runtime flags and include paths are configured, before
    creating compile objects. The caller uses the base environment for exempt
    sources. PCH output is flavor-separated, and its compiler flags are inherited
    from the caller; source sets and their compile flags remain authoritative.

    Example (inside the existing SConstruct, AFTER native flavor env setup):
        from build.pch import build_flavor_pch, pch_eligible
        pch_env, pch_nodes = build_flavor_pch(editor_env, "editor", ARGUMENTS.get("pch", "off"))
        for src in editor_source_set:
            use_env = pch_env if pch_eligible(src) else editor_env
            objects.append(use_env.SharedObject(target=object_path(src), source=src))
        # Let SCons track the PCH dependency; do not duplicate PCH .obj at link.
    """
    if flavor not in VALID_FLAVORS:
        raise ValueError("Unsupported Terrain3D native flavor: " + str(flavor))
    if mode not in VALID_MODES:
        raise ValueError("pch must be 'off' or 'stable'")
    cloned = env.Clone()
    if mode == "off":
        cloned.pop("PCH", None)
        cloned.pop("PCHSTOP", None)
        return cloned, ()

    compiler = str(env.subst("$CXX")).replace("\\", "/").rsplit("/", 1)[-1].lower()
    if compiler not in ("cl", "cl.exe"):
        raise RuntimeError("BLD-01 stable PCH requires MSVC cl.exe")
    if "PCH" not in cloned.get("BUILDERS", {}):
        raise RuntimeError("SCons MSVC PCH builder is not installed")

    source_root = str(Path(source_root).resolve())
    pch_dir = Path(build_dir) / flavor / "pch"
    # The C++ header itself must be force-included BEFORE any ordinary source
    # directives so MSVC's /Yu boundary cannot skip user source macros.
    pch_header_dir = str(Path(source_root) / "src" / "core" / "build")
    cloned.AppendUnique(CPPPATH=[pch_header_dir])
    cloned["PCHSTOP"] = PCH_HEADER
    # The PCH-producing env must use precisely the same defines/options.
    producer = cloned.Clone()
    producer.AppendUnique(CXXFLAGS=["/FI" + PCH_HEADER])
    target = str(pch_dir / "terrain_3d_stable.pch")
    nodes = producer.PCH(target=target, source=str(Path(source_root) / PCH_SOURCE))
    # PCH() returns [pch, obj]; use only the first node as the PCH property.
    cloned["PCH"] = nodes[0]
    cloned.AppendUnique(CXXFLAGS=["/FI" + PCH_HEADER])
    return cloned, tuple(nodes)
