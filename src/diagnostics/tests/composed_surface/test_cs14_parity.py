"""CS14 capture-contract and failure-detection tests (no invented Vulkan data)."""
import json
import struct
import tempfile
from pathlib import Path
from compare_cs14_material_parity import compare

with tempfile.TemporaryDirectory() as location:
    root = Path(location)
    ref = root / "reference"
    cand = root / "candidate"
    ref.mkdir()
    cand.mkdir()
    manifest = {
        "width": 2, "height": 2, "region_key": "0x123", "mip": 0,
        "source_fingerprint": "0x456", "composition_revision": 7,
        "channels": {key: {"file": f"{key}.f32le", "components": n}
                     for key, n in (("albedo", 3), ("normal", 3), ("orm", 4))}
    }
    for folder in (ref, cand):
        (folder / "capture.json").write_text(json.dumps(manifest))
        for name, spec in manifest["channels"].items():
            n = 4 * spec["components"]
            (folder / spec["file"]).write_bytes(struct.pack(f"<{n}f", *([0.25] * n)))
    limits = {key: {"max_abs": 0.01, "mae": 0.003} for key in manifest["channels"]}
    assert compare(ref / "capture.json", cand / "capture.json", limits)["accepted"]
    count = 4 * 4
    values = [0.25] * count
    values[3] = 0.75
    (cand / "orm.f32le").write_bytes(struct.pack(f"<{count}f", *values))
    result = compare(ref / "capture.json", cand / "capture.json", limits)
    assert not result["accepted"]
    assert result["channels"]["orm"]["violations"] == 1
    assert result["channels"]["albedo"]["accepted"]
    manifest["source_fingerprint"] = "different"
    (cand / "capture.json").write_text(json.dumps(manifest))
    try:
        compare(ref / "capture.json", cand / "capture.json", limits)
        assert False, "source fingerprint mismatch was silently accepted"
    except ValueError as error:
        assert "source_fingerprint" in str(error)
print("CS14 parity capture comparator: PASS")
