#!/usr/bin/env python
from glob import glob
from pathlib import Path
import json
import os
import re
import shutil
import subprocess

# godot-cpp 10.0.0-stable supports versioned bundled APIs directly. Terrain3D
# targets the dependency's 4.7 API because this fork carries the persistent
# RenderingDevice buffer extensions there. Do not override this with the old
# standalone 4.7.2 JSON: doing so can erase the fork-specific buffer methods.
GODOT_CPP_API_VERSION = "4.7"
GODOT_CPP_API_PATH = Path("godot-cpp/gdextension/extension_api-4-7.json")


def _verify_godot_cpp_api(path):
    if not path.is_file():
        raise RuntimeError(
            "Terrain3D requires godot-cpp 10.0.0-stable with the bundled 4.7 API; missing: %s" % path
        )

    with path.open("r", encoding="utf-8") as handle:
        api = json.load(handle)

    rendering_device = next(
        (entry for entry in api.get("classes", []) if entry.get("name") == "RenderingDevice"),
        None,
    )
    if rendering_device is None:
        raise RuntimeError("godot-cpp 4.7 API does not expose RenderingDevice")

    methods = {method.get("name"): method for method in rendering_device.get("methods", [])}
    required_methods = {
        "buffer_persistent_map_advance",
        "buffer_flush",
        "uniform_buffer_create",
        "storage_buffer_create",
        "vertex_buffer_create",
        "index_buffer_create",
    }
    missing_methods = sorted(required_methods.difference(methods))
    if missing_methods:
        raise RuntimeError(
            "godot-cpp 4.7 API lacks Terrain3D persistent-buffer methods: %s" % ", ".join(missing_methods)
        )

    enum_values = set()
    for enum in rendering_device.get("enums", []):
        if enum.get("name") == "BufferCreationBits":
            enum_values.update(value.get("name") for value in enum.get("values", []))
            break
    if "BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT" not in enum_values:
        raise RuntimeError(
            "godot-cpp 4.7 API lacks RenderingDevice::BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT"
        )

    expected_creation_bits = {
        "uniform_buffer_create": 2,
        "storage_buffer_create": 3,
        "vertex_buffer_create": 2,
        "index_buffer_create": 4,
    }
    incompatible = []
    for method_name, argument_index in expected_creation_bits.items():
        arguments = methods[method_name].get("arguments", [])
        if len(arguments) <= argument_index or arguments[argument_index].get("name") != "creation_bits":
            incompatible.append(method_name)
    if incompatible:
        raise RuntimeError(
            "godot-cpp 4.7 API has incompatible buffer-create signatures: %s" % ", ".join(incompatible)
        )

    map_method = methods["buffer_persistent_map_advance"]
    if map_method.get("return_value", {}).get("meta") != "uint64":
        raise RuntimeError("godot-cpp buffer_persistent_map_advance() no longer returns a uint64 address")

    flush_arguments = methods["buffer_flush"].get("arguments", [])
    if len(flush_arguments) != 1 or flush_arguments[0].get("name") != "buffer" or flush_arguments[0].get("type") != "RID":
        raise RuntimeError("godot-cpp buffer_flush() has an incompatible signature")


_verify_godot_cpp_api(GODOT_CPP_API_PATH)

# godot-cpp validates command-line variables while loading its SConstruct.
# Hide Terrain3D-owned options during that validation and restore them before
# configuring this extension. Keep godot-cpp's own options (platform, target,
# arch, etc.) visible to its Variables parser.
terrain3d_option_names = (
    "deadcode_audit",
    "hygiene_audit",
    "terrain3d_tests",
    "pch",
    "hygiene_ccn",
    "hygiene_length",
    "hygiene_params",
    "hygiene_cognitive",
    "hygiene_nesting",
    "hygiene_branches",
    # Shader build defaults. They must never be forwarded to godot-cpp.
    "vt_resident_ancestor_hints",
    "terrain_shader_cold_classes",
    "vt_sampling_trial",
    "material_shader_trial",
    "instance_alpha_trial",
    "world_alpha_trial",
    "ocean_prepared_fragment",
    "composed_shader_experiment",
    "terrain_gpu_timestamps",
    "vrs_trial",
)
terrain3d_options = {
    name: ARGUMENTS.pop(name)
    for name in terrain3d_option_names
    if name in ARGUMENTS
}
try:
    env = SConscript("godot-cpp/SConstruct", {"api_version": GODOT_CPP_API_VERSION})
finally:
    ARGUMENTS.update(terrain3d_options)

# Each invocation produces exactly one of three native libraries. Keep the
# native target name separate from the runtime/editor execution-policy switch.
if env["target"] not in {"editor", "template_debug", "template_release"}:
    raise RuntimeError("Terrain3D requires target=editor, template_debug, or template_release")

SOURCE_ROOT = Path("src")
TEST_ROOT = SOURCE_ROOT / "diagnostics/tests"
GENERATED_DOC = SOURCE_ROOT / "gen/doc_data.gen.cpp"
PCH_SOURCE = SOURCE_ROOT / "core/build/terrain_3d_pch.cpp"
EDITOR_ROOTS = (SOURCE_ROOT / "authoring", SOURCE_ROOT / "hlod/build")
EDITOR_FILES = {SOURCE_ROOT / "integration/godot/terrain_3d_editor_registration.cpp"}
RUNTIME_AUTHORING_FILES = {SOURCE_ROOT / "authoring/erosion/terrain_3d_erosion_settings.cpp"}
STANDALONE_TEST_MAIN = re.compile(r"\bint\s+main\s*\(")


def _within(path, folder):
    return folder in path.parents


def _editor_only(path):
    return path not in RUNTIME_AUTHORING_FILES and (
        path in EDITOR_FILES or any(_within(path, folder) for folder in EDITOR_ROOTS)
    )


def _native_translation_units(include_editor):
    if not SOURCE_ROOT.is_dir():
        raise RuntimeError("Terrain3D native source directory is missing: %s" % SOURCE_ROOT)
    return [
        path for path in sorted(SOURCE_ROOT.rglob("*.cpp"))
        if path not in (GENERATED_DOC, PCH_SOURCE) and not _within(path, TEST_ROOT)
        and (include_editor or not _editor_only(path))
    ]


def _native_test_units():
    return [
        path for path in sorted(TEST_ROOT.rglob("*.cpp"))
        if not STANDALONE_TEST_MAIN.search(path.read_text(encoding="utf-8", errors="replace"))
    ] if TEST_ROOT.is_dir() else []


native_target = env["target"]
if env.get("is_msvc", False):
    if not env.get("use_static_cpp", False) or env.get("debug_crt", False):
        raise RuntimeError("Terrain3D Windows/MSVC requires use_static_cpp=yes and debug_crt=no")
    defines = {str(d[0] if isinstance(d, tuple) else d) for d in env.get("CPPDEFINES", [])}
    if "T3D_EDITOR_NATIVE" in defines or "T3D_RUNTIME_NATIVE" in defines:
        raise RuntimeError("Native flavor is controlled by target=, not command-line CPPDEFINES")

if native_target == "editor":
    env.AppendUnique(CPPDEFINES=["T3D_EDITOR_NATIVE"])
else:
    env.AppendUnique(CPPDEFINES=["T3D_RUNTIME_NATIVE"])
    if native_target == "template_debug":
        env.AppendUnique(CPPDEFINES=["T3D_DEBUG_RUNTIME"])

# Shader feature flags are extension-only. The C++ hint builder is a compile-
# time feature, whereas individual GLSL shader variants are selected by the
# cold shader library. The options below set binary defaults; the existing
# TERRAIN3D_* process environment variables override those defaults at startup.
_SHADER_MATERIAL_TRIALS = (
    "silhouette", "silhouette_only", "control_nearest", "control_bilinear",
    "control_adaptive", "authored_exact", "authored_hardware", "planar",
    "two_key", "active_key", "fixed_taps", "transform_reuse",
    "authored_corner", "megavt_early",
)
_SHADER_ALPHA_TRIALS = (
    "alpha_gather_depth", "alpha_edge_depth", "alpha_gather_dnr", "alpha_edge_dnr",
)
_SHADER_VT_TRIALS = (
    "prepared_mip_bias", "vt_ancestor_hints", "vt_ancestor_hints_prepared_bias",
)


def _shader_boolean_option(name):
    value = str(ARGUMENTS.get(name, "no")).strip().lower()
    if value in ("yes", "true", "1", "on"):
        return True
    if value in ("no", "false", "0", "off"):
        return False
    raise RuntimeError("%s must be yes or no (received %r)" % (name, value))


def _shader_choice_option(name, choices, aliases=None):
    value = str(ARGUMENTS.get(name, "none")).strip().lower()
    if value in ("none", "off", "disabled", "0"):
        return 0
    if aliases and value in aliases:
        value = aliases[value]
    if value.endswith("_native"):
        value = value[:-7]
    if value in choices:
        return choices.index(value) + 1
    raise RuntimeError("%s=%s is invalid. Choose: none, %s" % (name, value, ", ".join(choices)))


vt_hints_enabled = _shader_boolean_option("vt_resident_ancestor_hints")
vt_trial = _shader_choice_option("vt_sampling_trial", _SHADER_VT_TRIALS)
material_trial_name = str(ARGUMENTS.get("material_shader_trial", "none")).strip().lower()
material_trial = 255 if material_trial_name == "auto" else _shader_choice_option(
    "material_shader_trial", _SHADER_MATERIAL_TRIALS)
instance_alpha_trial = _shader_choice_option("instance_alpha_trial", _SHADER_ALPHA_TRIALS,
                                             {"gather_depth": "alpha_gather_depth",
                                              "edge_depth": "alpha_edge_depth",
                                              "gather_dnr": "alpha_gather_dnr",
                                              "edge_dnr": "alpha_edge_dnr"})
world_alpha_trial = _shader_choice_option("world_alpha_trial", _SHADER_ALPHA_TRIALS,
                                          {"gather_depth": "alpha_gather_depth",
                                           "edge_depth": "alpha_edge_depth",
                                           "gather_dnr": "alpha_gather_dnr",
                                           "edge_dnr": "alpha_edge_dnr"})
if vt_trial in (2, 3) and not vt_hints_enabled:
    raise RuntimeError("vt_sampling_trial=vt_ancestor_hints* requires vt_resident_ancestor_hints=yes")

vrs_trial_name = str(ARGUMENTS.get("vrs_trial", "none")).strip().lower()
if vrs_trial_name not in ("none", "off", "uniform", "conservative"):
    raise RuntimeError("vrs_trial must be none, uniform or conservative")
vrs_trial = {"none": 0, "off": 0, "uniform": 1, "conservative": 2}[vrs_trial_name]

shader_defaults = {
    "T3D_ENABLE_TERRAIN_SHADER_COLD_CLASSES": int(_shader_boolean_option("terrain_shader_cold_classes")),
    "T3D_DEFAULT_VT_SAMPLING_TRIAL": vt_trial,
    "T3D_DEFAULT_MATERIAL_SHADER_TRIAL": material_trial,
    "T3D_DEFAULT_INSTANCE_ALPHA_TRIAL": instance_alpha_trial,
    "T3D_DEFAULT_WORLD_ALPHA_TRIAL": world_alpha_trial,
    "T3D_DEFAULT_OCEAN_PREPARED_FRAGMENT": int(_shader_boolean_option("ocean_prepared_fragment")),
    "T3D_DEFAULT_COMPOSED_SHADER_EXPERIMENT": int(_shader_boolean_option("composed_shader_experiment")),
    "T3D_DEFAULT_TERRAIN_GPU_TIMESTAMPS": int(_shader_boolean_option("terrain_gpu_timestamps")),
    "T3D_DEFAULT_VRS_TRIAL": vrs_trial,
}
# The hint feature is conditionally compiled only when explicitly enabled.
# Keep default builds free of the opt-in definition.
if vt_hints_enabled:
    shader_defaults["T3D_ENABLE_VT_RESIDENT_ANCESTOR_HINTS"] = 1
# Explicit defaults let switching from an experiment to baseline rebuild objects.
env.AppendUnique(CPPDEFINES=[(name, value) for name, value in shader_defaults.items()])
print("Terrain3D shader configuration: VT hints=%s; terrain cold classes=%s; VT trial=%s; "
      "material=%s; instance alpha=%s; world alpha=%s; ocean=%s; composed=%s; GPU timestamps=%s; VRS=%s" % (
          "on" if vt_hints_enabled else "off",
          shader_defaults["T3D_ENABLE_TERRAIN_SHADER_COLD_CLASSES"], vt_trial, material_trial,
          instance_alpha_trial, world_alpha_trial,
          shader_defaults["T3D_DEFAULT_OCEAN_PREPARED_FRAGMENT"],
          shader_defaults["T3D_DEFAULT_COMPOSED_SHADER_EXPERIMENT"],
          shader_defaults["T3D_DEFAULT_TERRAIN_GPU_TIMESTAMPS"], vrs_trial_name))

# Optional MSVC dead-code audit. `yes` runs compiler/static analysis plus linker
# reference elimination; `ltcg` adds cross-translation-unit optimization.
deadcode_audit = ARGUMENTS.get("deadcode_audit", "no").strip().lower()
if deadcode_audit not in {"no", "yes", "ltcg"}:
    raise RuntimeError("deadcode_audit must be no, yes, or ltcg")

if deadcode_audit != "no":
    if not env.get("is_msvc", False):
        raise RuntimeError("deadcode_audit requires the MSVC toolchain")
    if deadcode_audit == "ltcg" and env["target"] != "template_release":
        raise RuntimeError("deadcode_audit=ltcg requires target=template_release")

    audit_dir = Path("build/deadcode")
    audit_dir.mkdir(parents=True, exist_ok=True)
    map_path = (audit_dir / "terrain3d.{}.{}.map".format(env["target"], env["arch"])).as_posix()

    env.AppendUnique(CCFLAGS=["/W4", "/Gy", "/Gw", "/Zi"])
    env.AppendUnique(LINKFLAGS=[
        "/DEBUG:FULL",
        "/INCREMENTAL:NO",
        "/OPT:REF",
        "/VERBOSE:REF",
        "/MAP:" + map_path,
    ])
    if deadcode_audit == "yes":
        env.AppendUnique(CCFLAGS=["/analyze", "/analyze:WX-"])
    else:
        env.AppendUnique(CCFLAGS=["/GL"])
        env.AppendUnique(LINKFLAGS=["/LTCG"])

# Optional project-only MSVC hygiene diagnostics. `yes` enables /W4 and
# /analyze; `core` additionally enables the C++ Core Guidelines checker.
hygiene_audit = ARGUMENTS.get("hygiene_audit", "no").strip().lower()
if hygiene_audit in {"1", "true"}:
    hygiene_audit = "yes"
if hygiene_audit not in {"no", "yes", "core"}:
    raise RuntimeError("hygiene_audit must be no, yes, or core")

if hygiene_audit != "no":
    if not env.get("is_msvc", False):
        raise RuntimeError("hygiene_audit requires the MSVC toolchain")
    if deadcode_audit == "ltcg":
        raise RuntimeError("Run hygiene_audit and deadcode_audit=ltcg in separate builds")

    env.AppendUnique(CCFLAGS=[
        "/W4",
        "/w44061",
        "/w44062",
        "/w44505",
        "/analyze",
        "/analyze:WX-",
        "/external:Ithirdparty",
        "/external:Igodot-cpp",
        "/external:W0",
        "/analyze:external-",
    ])

    if hygiene_audit == "core":
        env["ENV"] = env["ENV"].copy()
        existing_extensions = env["ENV"].get("Esp.Extensions", os.environ.get("Esp.Extensions", ""))
        extensions = [item for item in existing_extensions.split(";") if item]
        if "cppcorecheck.dll" not in {item.lower() for item in extensions}:
            extensions.append("CppCoreCheck.dll")
        env["ENV"]["Esp.Extensions"] = ";".join(extensions)
        env.AppendUnique(CCFLAGS=["/analyze:plugin", "EspXEngine.dll"])

hygiene_compdb_requested = any(
    name in COMMAND_LINE_TARGETS for name in ("hygiene-compdb", "hygiene-tidy")
)
if hygiene_compdb_requested:
    env.Tool("compilation_db")
    env["COMPILATIONDB_USE_ABSPATH"] = True


def _hygiene_limit(name, default):
    try:
        value = int(ARGUMENTS.get(name, str(default)))
    except ValueError:
        raise RuntimeError("%s must be a positive integer" % name)
    if value < 1:
        raise RuntimeError("%s must be a positive integer" % name)
    return value


env.Append(CPPPATH=[
    "src/",
    "thirdparty/EASTL/include",
    "thirdparty/EABase/include/Common",
])
native_cpp_paths = _native_translation_units(native_target == "editor")
if native_target != "editor" and any(_editor_only(path) for path in native_cpp_paths):
    raise RuntimeError("Authoring sources leaked into Terrain3D runtime build")
sources = [env.File(path.as_posix()) for path in native_cpp_paths]

if native_target == "editor":
    env.AppendUnique(CPPDEFINES=["T3D_DOC_DATA_ENABLED"])

include_tests = ARGUMENTS.get("terrain3d_tests", "no").lower() in {"1", "yes", "true"}
if include_tests:
    if native_target != "editor":
        raise RuntimeError("terrain3d_tests requires the editor target (authoring codec/bake test fixtures)")
    sources.extend(env.File(path.as_posix()) for path in _native_test_units())

(extension_path,) = glob("project/addons/terrain_3d/*.gdextension")
addon_path = Path(extension_path).parent
project_name = Path(extension_path).stem

scons_cache_path = os.environ.get("SCONS_CACHE")
if scons_cache_path != None:
    CacheDir(scons_cache_path)
    print("Scons cache enabled... (path: '" + scons_cache_path + "')")

if native_target == "editor":
    doc_xml = Glob("doc_classes/*.xml")
    if doc_xml:
        from tools.generate_doc_data import generate_doc_data
        doc_data = env.Command(GENERATED_DOC.as_posix(), doc_xml, generate_doc_data)
        env.Depends(doc_data, "tools/generate_doc_data.py")
        sources.extend(doc_data)
    else:
        if not GENERATED_DOC.is_file():
            raise RuntimeError("Missing pre-generated editor doc source: %s" % GENERATED_DOC)
        sources.append(env.File(GENERATED_DOC.as_posix()))

debug_or_release = "release" if native_target == "template_release" else "debug"
if native_target == "editor" and env["platform"] == "windows" and env["arch"] == "x86_64":
    debug_or_release = "editor"

obj_dir = Path("build/obj") / native_target / env["platform"] / env["arch"]

# Build the stable external-header PCH with the fully configured native flavor.
# Keep the PCH producer out of the ordinary source glob, and never supply the
# PCH to test, generated, registration, or ISA-specialized translation units.
# A separate PCH is produced for each target/platform/architecture directory.
from build.pch import build_flavor_pch, pch_eligible

pch_default = "stable" if env.get("is_msvc", False) else "off"
pch_mode = str(ARGUMENTS.get("pch", pch_default)).strip().lower()
if pch_mode not in ("off", "stable"):
    raise RuntimeError("pch must be off or stable")
if pch_mode == "stable" and not env.get("is_msvc", False):
    raise RuntimeError("pch=stable requires MSVC cl.exe; use pch=off")

pch_env, pch_nodes = build_flavor_pch(
    env,
    "editor" if native_target == "editor" else "runtime",
    mode=pch_mode,
    build_dir=obj_dir,
    source_root=".",
)
objects = []
pch_object_count = 0
for source in sources:
    source_path = Path(str(source))
    # register_types.cpp receives a special test define in test builds; it must
    # never consume a PCH compiled without that define.
    use_pch = pch_mode == "stable" and pch_eligible(source_path)
    if include_tests and source_path.name == "register_types.cpp":
        use_pch = False
    object_env = pch_env if use_pch else env
    if include_tests and source_path.name == "register_types.cpp":
        object_env = env.Clone()
        object_env.AppendUnique(CPPDEFINES=["T3D_TESTS_ENABLED"])
    object_path = obj_dir / source_path.with_suffix("")
    objects.extend(object_env.SharedObject(target=object_path.as_posix(), source=source))
    pch_object_count += int(use_pch)

print("Terrain3D native PCH: mode=%s target=%s eligible=%d/%d" % (
    pch_mode, native_target, pch_object_count, len(sources)))
if pch_nodes:
    # MSVC requires the object emitted by /Yc to be linked with every /Yu
    # consumer. The link environment is the base env (without PCH), so SCons
    # cannot implicitly append that producer object for this SharedLibrary.
    # nodes[0] is the compiler-only .pch; nodes[1] is the linker-required .obj.
    if len(pch_nodes) != 2:
        raise RuntimeError("Terrain3D PCH builder must produce a .pch and companion .obj")
    objects.append(pch_nodes[1])
    env.Alias("terrain3d-pch", pch_nodes[0])

if env.get("is_msvc", False) and env.get("debug_symbols", False):
    pdb_path = addon_path / "bin" / "lib{}.{}.{}.{}.pdb".format(
        project_name, env["platform"], debug_or_release, env["arch"]
    )
    env.AppendUnique(LINKFLAGS=["/DEBUG:FULL", "/PDB:" + pdb_path.as_posix()])
    print("Terrain3D linker symbols: {}".format(pdb_path))
if env["platform"] == "macos":
    library = env.SharedLibrary(
        "{0}/bin/lib{1}.{2}.{3}.framework/{1}.{2}.{3}".format(
            addon_path,
            project_name,
            env["platform"],
            debug_or_release,
        ),
        source=objects,
    )
else:
    library = env.SharedLibrary(
        "{}/bin/lib{}.{}.{}.{}{}".format(
            addon_path,
            project_name,
            env["platform"],
            debug_or_release,
            env["arch"],
            env["SHLIBSUFFIX"],
        ),
        source=objects,
    )

# Standalone source-hygiene targets; they are never part of Default(library).
if "hygiene-lizard" in COMMAND_LINE_TARGETS or "hygiene-tidy" in COMMAND_LINE_TARGETS:
    audit_sources = [path.as_posix() for path in native_cpp_paths]
    if include_tests:
        audit_sources.extend(path.as_posix() for path in _native_test_units())
    audit_sources = sorted(set(audit_sources))


def _run_hygiene_tool(command, report_path):
    report_path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    report_path.write_text(output, encoding="utf-8")
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    print("Hygiene report: %s" % report_path)
    return result.returncode


if "hygiene-lizard" in COMMAND_LINE_TARGETS:
    def _run_lizard(target, source, action_env):
        lizard = shutil.which("lizard")
        if not lizard:
            print("hygiene-lizard requires: python -m pip install lizard")
            return 1
        command = [
            lizard, "-l", "cpp",
            "-C", str(_hygiene_limit("hygiene_ccn", 15)),
            "-L", str(_hygiene_limit("hygiene_length", 120)),
            "-a", str(_hygiene_limit("hygiene_params", 8)),
            "--warning-msvs",
        ] + audit_sources
        return _run_hygiene_tool(command, Path("build/hygiene/lizard.txt"))

    lizard_alias = env.Alias("hygiene-lizard", [], env.Action(_run_lizard, "Analyzing cyclomatic complexity with Lizard"))
    env.AlwaysBuild(lizard_alias)

if hygiene_compdb_requested:
    compilation_db = env.CompilationDatabase("build/hygiene/compile_commands.json")
    env.Alias("hygiene-compdb", compilation_db)

if "hygiene-tidy" in COMMAND_LINE_TARGETS:
    def _run_tidy(target, source, action_env):
        tidy = shutil.which("clang-tidy")
        if not tidy:
            print("hygiene-tidy requires clang-tidy (LLVM) on PATH")
            return 1
        config = {
            "Checks": ",".join([
                "-*",
                "readability-function-cognitive-complexity",
                "readability-function-size",
                "readability-redundant-declaration",
                "readability-duplicate-include",
                "readability-redundant-control-flow",
                "bugprone-branch-clone",
            ]),
            "CheckOptions": {
                "readability-function-cognitive-complexity.Threshold": str(_hygiene_limit("hygiene_cognitive", 20)),
                "readability-function-size.LineThreshold": str(_hygiene_limit("hygiene_length", 120)),
                "readability-function-size.NestingThreshold": str(_hygiene_limit("hygiene_nesting", 4)),
                "readability-function-size.BranchThreshold": str(_hygiene_limit("hygiene_branches", 20)),
                "readability-function-size.ParameterThreshold": str(_hygiene_limit("hygiene_params", 8)),
            },
        }
        command_base = [
            tidy,
            "-p", str(Path("build/hygiene").resolve()),
            "--config=" + json.dumps(config, separators=(",", ":")),
        ]
        report_path = Path("build/hygiene/clang-tidy.txt")
        report_path.parent.mkdir(parents=True, exist_ok=True)
        outputs = []
        exit_code = 0
        for index in range(0, len(audit_sources), 12):
            result = subprocess.run(
                command_base + audit_sources[index:index + 12],
                capture_output=True, text=True, errors="replace",
            )
            outputs.append(result.stdout + result.stderr)
            if result.returncode:
                exit_code = result.returncode
        output = "".join(outputs)
        report_path.write_text(output, encoding="utf-8")
        if output:
            print(output, end="" if output.endswith("\n") else "\n")
        print("Hygiene report: %s" % report_path)
        return exit_code

    tidy_alias = env.Alias("hygiene-tidy", compilation_db, env.Action(_run_tidy, "Analyzing structure with Clang-Tidy"))
    env.AlwaysBuild(tidy_alias)

Default(library)
