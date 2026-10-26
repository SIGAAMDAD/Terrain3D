# P09–P10 validation

Example:

```sh
godot --headless --path project -s res://addons/terrain_3d/validation/vt_streaming_acceptance.gd -- --scene res://path/to/cooked_terrain_scene.tscn --frames 600 --output user://vt_acceptance.json
```

Run the same scene and identical camera trajectory at least three times for each release configuration and each comparison (predictor enabled/disabled, cache enabled/disabled). Collect a GPU timestamp trace separately (RenderDoc, Nsight or Intel GPA) for GPU p50/p95/p99 and any 100–200 ms spikes. The script reports **CPU frame intervals**, source residency, sidecar status, published transfer admission, deferrals and synchronization counters. It cannot report true GPU execution latency.

For intentional missing/invalid `.t3vd` fallback tests add `--allow-fallback`; the default treats an absent or mismatched predictor sidecar as an acceptance failure.

P11 coverage hardening: inspect `vt_prediction_incomplete_fallbacks` and
`vt_prediction_current_cell_complete` during high-density traversal and teleports.
If the predictor exhausts its per-frame request budget, terrain reactive demand
must remain enabled. `vt_feedback_mask_uploads_saved` should increase during
stationary, fully covered scenes; it is a counter of avoided 32-byte UBO writes,
not measured GPU time. Warm publication with an outdated `.t3vd` must retain
reactive demand until the sidecar matches the active `.t3dvt` signature.

**Acceptance checklist:** valid asset-derived sidecar and matching package signature; metadata-only file package; compact runtime material view after startup; unchanged visual material mappings; no runtime texture-source rehydration; stable transfer budget accounting; no syncs attributed to routine diagnostic collection; error paths for missing/corrupt index, missing `.t3dvt`, cache pressure, scene reload and warm republish. Do not sign off latency stabilization without GPU capture.

**Editor regression:** confirm authoring texture editing, re-baking and sidecar export retain source Texture2D references. For cooked runtime with `free_editor_textures = true`, confirm `authoring_texture_residency.runtime_metadata_only` is true after startup and `logical_material_count` is unchanged. With that setting false, authoring references remain available. These assertions must be checked in a Godot engine build that includes the native extension.

**Native test:** invoke the existing `virtual_texturing` system test suite and check `runtime_material_view_preserves_identity_and_scalar_settings`, alongside the VT metadata/file-loader, predictor V2, and transfer-admission tests. Unit tests use Godot Ref objects and require engine initialization.
