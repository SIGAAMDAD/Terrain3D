# Windows Codex/Godot stability lane

Target machine: Windows x86-64, Core Ultra 7 258V / Arc 140V, Godot 4.7.1 Mono at `D:\Godot_v4.7.1-stable_mono_win64`.

Use `tools\rd.cmd`; do not repeatedly launch the full interactive editor from Codex.

## Routine work

- Native compile: `tools\rd.cmd fast`.
- Resource import: `tools\rd.cmd import`.
- Editor/plugin smoke: `tools\rd.cmd editor-smoke`.
- C#: `tools\rd.cmd csharp` uses `dotnet build`, not a Godot editor launch.
- Successful command output is suppressed into `.rd_logs`; only failure tails are printed.

Headless Godot runs use `--headless` and `--render-thread safe`. The headless mode removes the display/window path and uses Dummy audio. This is appropriate for import/parse/editor initialization checks, not for validating RenderingDevice raster output.

## GPU work

Use Vulkan first during iteration:

```bat
tools\rd.cmd gpu-smoke vulkan
```

Use D3D12 at the assigned batch gates and final validation:

```bat
tools\rd.cmd gpu-smoke d3d12
```

GPU smoke uses Forward+, safe render-thread mode, one small window, XR off, Dummy audio, 60 FPS cap, finite iteration count and captured logs. Final performance tests must remove these conservative automation settings and use the intended production configuration.

## If Godot crashes

Run:

```bat
tools\rd.cmd diagnose-godot
```

The sequence isolates: recovery-mode editor startup, headless import, Vulkan Forward+ runtime, then D3D12 Forward+ runtime. Recovery mode intentionally disables tool scripts, editor plugins and GDExtensions, so it is diagnostic only.

If recovery/headless pass but D3D12 fails, keep compiler/static work moving and use Vulkan for ordinary GPU iteration. Do not increase Windows TDR values as a routine workaround. Preserve the failing `.rd_logs` output and retest D3D12 after the relevant renderer change or Intel driver update.

Do not automatically kill all Godot processes: the user may have an interactive editor open. Each automated process has a finite timeout and is terminated by the wrapper if it does not exit.
