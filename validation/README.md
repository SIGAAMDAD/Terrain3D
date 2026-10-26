# Validation fixture

`terrain3d_00_02.zip` is the supplied 354-file / 176-region Terrain3D streaming dataset. Do not ingest or extract it during normal Codex work. Batch H uses `tools\rd.cmd fixture --verify`; extraction goes to ignored `.rd_tools/validation_data/terrain3d_00_02`.

## Current architecture acceptance (post-P34 R02)

`current_acceptance_manifest.json` is the authoritative descendant manifest for the final P34 architecture. Run:

```text
python tools/verify_current_architecture.py
```

Use `--release` to additionally reject `.git` packaging and merge markers. The manifest keeps superseded verifiers explicitly classified as `historical_only`; those scripts remain available for archaeology but do not define current green/red status.
