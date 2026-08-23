#!/usr/bin/env python3
"""Regenerate Terrain3D Visual Studio files from the current repository layout."""

from __future__ import annotations

from pathlib import Path
from uuid import NAMESPACE_URL, uuid5
import xml.sax.saxutils as saxutils

ROOT = Path(__file__).resolve().parent.parent
PROJECT_GUID = "{B8850C81-3339-46A9-9668-CAA004E84629}"
SOLUTION_GUID = "{119F9825-B9FE-4F51-9225-7EA96BF5BF7F}"
WORKSPACE_GUID = "{5AFB489B-6E62-5B44-8C5B-5091889BDF90}"
VC_PROJECT_TYPE_GUID = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}"
SOLUTION_FOLDER_TYPE_GUID = "{66A26720-8FB5-11D2-AA7E-00C04F688DDE}"
CS_PROJECT_TYPE_GUID = "{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}"
CS_PROJECT_GUID = "{E3C3EDF1-6587-443E-B932-EBA7704DB53E}"

CONFIGS = (
    ("Debug", "Win32", "template_debug", "x86_32", True),
    ("Release", "Win32", "template_release", "x86_32", False),
    ("Debug", "x64", "template_debug", "x86_64", True),
    ("Release", "x64", "template_release", "x86_64", False),
)


def win(path: Path | str) -> str:
    return str(path).replace("/", "\\")


def esc(text: str) -> str:
    return saxutils.escape(text, {'"': '&quot;'})


def rel(path: Path) -> Path:
    return path.relative_to(ROOT)


def filter_name(path: Path) -> str:
    parent = rel(path).parent
    if str(parent) == ".":
        return "Workspace"
    return win(parent)


def filter_guid(name: str) -> str:
    return "{" + str(uuid5(NAMESPACE_URL, "Terrain3D.VisualStudio.Filter:" + name)).upper() + "}"


def gather_files() -> tuple[list[Path], list[Path], list[Path]]:
    compile_files = sorted(ROOT.glob("src/**/*.cpp"))
    include_files = sorted(
        [*ROOT.glob("src/**/*.h"), *ROOT.glob("src/**/*.inl"), *ROOT.glob("src/**/*.inc")]
    )

    supplemental: set[Path] = set()
    for name in (".clang-format", ".gitattributes", ".gitignore", "SConstruct"):
        path = ROOT / name
        if path.exists():
            supplemental.add(path)

    for pattern in (
        "docs/**/*.md",
        "src/**/*.glsl",
        "project/project.godot",
        "project/Terrain3D.csproj",
        "project/Terrain3D.sln",
        "project/addons/terrain_3d/terrain.gdextension",
        "project/addons/terrain_3d/plugin.cfg",
        "project/addons/terrain_3d/setting_metadata.json",
        "project/addons/terrain_3d/compute/**/*.glsl",
        "project/addons/terrain_3d/shaders/**/*.gdshader",
        "project/addons/terrain_3d/shaders/**/*.gdshaderinc",
        "project/addons/terrain_3d/csharp/**/*.cs",
        "tools/*.py",
    ):
        supplemental.update(p for p in ROOT.glob(pattern) if p.is_file())

    supplemental.difference_update(compile_files)
    supplemental.difference_update(include_files)
    return compile_files, include_files, sorted(supplemental)


def make_vcxproj() -> str:
    compile_files, include_files, supplemental = gather_files()
    lines: list[str] = []
    a = lines.append
    a('<?xml version="1.0" encoding="utf-8"?>')
    a('<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
    a('  <ItemGroup Label="ProjectConfigurations">')
    for config, platform, _, _, _ in CONFIGS:
        a(f'    <ProjectConfiguration Include="{config}|{platform}">')
        a(f'      <Configuration>{config}</Configuration>')
        a(f'      <Platform>{platform}</Platform>')
        a('    </ProjectConfiguration>')
    a('  </ItemGroup>')
    a('  <PropertyGroup Label="Globals">')
    a('    <VCProjectVersion>17.0</VCProjectVersion>')
    a('    <Keyword>MakeFileProj</Keyword>')
    a(f'    <ProjectGuid>{PROJECT_GUID}</ProjectGuid>')
    a('    <RootNamespace>Terrain3D</RootNamespace>')
    a('    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>')
    a('  </PropertyGroup>')
    a('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />')
    for config, platform, _, _, debug in CONFIGS:
        a(f'  <PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'{config}|{platform}\'" Label="Configuration">')
        a('    <ConfigurationType>Makefile</ConfigurationType>')
        a(f'    <UseDebugLibraries>{str(debug).lower()}</UseDebugLibraries>')
        a('    <PlatformToolset>v143</PlatformToolset>')
        a('    <CharacterSet>Unicode</CharacterSet>')
        a('  </PropertyGroup>')
    a('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />')
    a('  <ImportGroup Label="ExtensionSettings" />')
    a('  <ImportGroup Label="Shared" />')
    for config, platform, _, _, _ in CONFIGS:
        a(f'  <ImportGroup Label="PropertySheets" Condition="\'$(Configuration)|$(Platform)\'==\'{config}|{platform}\'">')
        a('    <Import Project="$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props" Condition="exists(\'$(UserRootDir)\\Microsoft.Cpp.$(Platform).user.props\')" Label="LocalAppDataPlatform" />')
        a('  </ImportGroup>')
    a('  <PropertyGroup Label="UserMacros" />')
    for config, platform, target, arch, debug in CONFIGS:
        dev = ' dev_build=yes' if debug else ''
        command = f'scons platform=windows target={target} arch={arch}{dev}'
        output_kind = 'debug' if target != 'template_release' else 'release'
        output = f'$(ProjectDir)project\\addons\\terrain_3d\\bin\\libterrain.windows.{output_kind}.{arch}.dll'
        a(f'  <PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'{config}|{platform}\'">')
        a(f'    <NMakeBuildCommandLine>{esc(command)}</NMakeBuildCommandLine>')
        a(f'    <NMakeReBuildCommandLine>{esc("scons --clean platform=windows target=" + target + " arch=" + arch + " && " + command)}</NMakeReBuildCommandLine>')
        a(f'    <NMakeCleanCommandLine>scons --clean platform=windows target={target} arch={arch}</NMakeCleanCommandLine>')
        a(f'    <NMakeOutput>{esc(output)}</NMakeOutput>')
        a('    <NMakeIncludeSearchPath>$(ProjectDir)src;$(ProjectDir)thirdparty\\EASTL\\include;$(ProjectDir)thirdparty\\EABase\\include\\Common;$(ProjectDir)godot-cpp\\gdextension;$(ProjectDir)godot-cpp\\gen\\include;$(ProjectDir)godot-cpp\\include</NMakeIncludeSearchPath>')
        a('    <NMakePreprocessorDefinitions>GDEXTENSION;%(NMakePreprocessorDefinitions)</NMakePreprocessorDefinitions>')
        a('  </PropertyGroup>')

    if include_files:
        a('  <ItemGroup>')
        for path in include_files:
            a(f'    <ClInclude Include="{esc(win(rel(path)))}" />')
        a('  </ItemGroup>')
    if compile_files:
        a('  <ItemGroup>')
        for path in compile_files:
            a(f'    <ClCompile Include="{esc(win(rel(path)))}" />')
        a('  </ItemGroup>')
    if supplemental:
        a('  <ItemGroup>')
        for path in supplemental:
            a(f'    <None Include="{esc(win(rel(path)))}" />')
        a('  </ItemGroup>')

    a('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />')
    a('  <ImportGroup Label="ExtensionTargets" />')
    a('</Project>')
    return "\n".join(lines) + "\n"


def make_filters() -> str:
    compile_files, include_files, supplemental = gather_files()
    all_files = [*compile_files, *include_files, *supplemental]
    filters = sorted({filter_name(path) for path in all_files})
    lines: list[str] = []
    a = lines.append
    a('<?xml version="1.0" encoding="utf-8"?>')
    a('<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')
    a('  <ItemGroup>')
    for name in filters:
        a(f'    <Filter Include="{esc(name)}">')
        a(f'      <UniqueIdentifier>{filter_guid(name)}</UniqueIdentifier>')
        a('    </Filter>')
    a('  </ItemGroup>')

    for tag, files in (("ClInclude", include_files), ("ClCompile", compile_files), ("None", supplemental)):
        if not files:
            continue
        a('  <ItemGroup>')
        for path in files:
            a(f'    <{tag} Include="{esc(win(rel(path)))}">')
            a(f'      <Filter>{esc(filter_name(path))}</Filter>')
            a(f'    </{tag}>')
        a('  </ItemGroup>')
    a('</Project>')
    return "\n".join(lines) + "\n"


def make_sln() -> str:
    workspace_items = [
        ".clang-format",
        ".gitattributes",
        ".gitignore",
        "SConstruct",
        "docs\\rendering_device_conversion_patch.md",
        "project\\project.godot",
        "project\\addons\\terrain_3d\\terrain.gdextension",
        "tools\\generate_msvc_project.py",
    ]
    items = [item for item in workspace_items if (ROOT / item.replace("\\", "/")).exists()]
    lines: list[str] = [
        "Microsoft Visual Studio Solution File, Format Version 12.00",
        "# Visual Studio Version 17",
        "VisualStudioVersion = 17.7.34024.191",
        "MinimumVisualStudioVersion = 10.0.40219.1",
        f'Project("{VC_PROJECT_TYPE_GUID}") = "Terrain3D", "Terrain3D.vcxproj", "{PROJECT_GUID}"',
        "EndProject",
        f'Project("{CS_PROJECT_TYPE_GUID}") = "Terrain3D.Managed", "project\\Terrain3D.csproj", "{CS_PROJECT_GUID}"',
        "EndProject",
        f'Project("{SOLUTION_FOLDER_TYPE_GUID}") = "Workspace", "Workspace", "{WORKSPACE_GUID}"',
        "\tProjectSection(SolutionItems) = preProject",
    ]
    for item in items:
        lines.append(f"\t\t{item} = {item}")
    lines.extend([
        "\tEndProjectSection",
        "EndProject",
        "Global",
        "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution",
    ])
    for config, platform, _, _, _ in CONFIGS:
        solution_platform = "x86" if platform == "Win32" else platform
        lines.append(f"\t\t{config}|{solution_platform} = {config}|{solution_platform}")
    lines.extend([
        "\tEndGlobalSection",
        "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution",
    ])
    for config, platform, _, _, _ in CONFIGS:
        solution_platform = "x86" if platform == "Win32" else platform
        lines.append(f"\t\t{PROJECT_GUID}.{config}|{solution_platform}.ActiveCfg = {config}|{platform}")
        lines.append(f"\t\t{PROJECT_GUID}.{config}|{solution_platform}.Build.0 = {config}|{platform}")
        managed_config = "Debug" if config == "Debug" else "ExportRelease"
        lines.append(f"\t\t{CS_PROJECT_GUID}.{config}|{solution_platform}.ActiveCfg = {managed_config}|Any CPU")
        lines.append(f"\t\t{CS_PROJECT_GUID}.{config}|{solution_platform}.Build.0 = {managed_config}|Any CPU")
    lines.extend([
        "\tEndGlobalSection",
        "\tGlobalSection(SolutionProperties) = preSolution",
        "\t\tHideSolutionNode = FALSE",
        "\tEndGlobalSection",
        "\tGlobalSection(ExtensibilityGlobals) = postSolution",
        f"\t\tSolutionGuid = {SOLUTION_GUID}",
        "\tEndGlobalSection",
        "EndGlobal",
    ])
    return "\n".join(lines) + "\n"


def main() -> None:
    (ROOT / "Terrain3D.vcxproj").write_text(make_vcxproj(), encoding="utf-8")
    (ROOT / "Terrain3D.vcxproj.filters").write_text(make_filters(), encoding="utf-8")
    (ROOT / "Terrain3D.sln").write_text(make_sln(), encoding="utf-8-sig")


if __name__ == "__main__":
    main()
