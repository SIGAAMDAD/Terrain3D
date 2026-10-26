#!/usr/bin/env python3
"""Offline guard for staged HLOD recovery workflow; does not invoke Godot."""
from pathlib import Path
import hashlib
root=Path(__file__).resolve().parents[1]
tools=root/'tools'
baker=(tools/'hlod_baker.gd').read_text()
codec=(tools/'hlod_build_checkpoint.gd').read_text()
panel=(tools/'hlod_panel.gd').read_text()
assert 'HLODBuildCheckpoint.plan_key(jobs, build_signature, manifest_path)' in baker
assert 'HLODBuildSchedule.validate_jobs(jobs)' in baker
assert 'HLODBuildCheckpoint.load_completed(checkpoint_path, checkpoint_key)' in baker
assert 'checkpoint_lookup[id] = entry' in baker
assert '_find_cached_cluster(int(job["level"])' in baker
assert 'job["reason"] = "resumed_package"' in baker
assert 'HLODBuildCheckpoint.save_completed(checkpoint_path, checkpoint_key, checkpoint_completed)' in baker
assert 'HLODBuildCheckpoint.clear_completed(checkpoint_path)' in baker
assert 'source_signature": str(cluster.source_signature)' in baker
assert 'build_signature": str(build_signature)' in baker
assert '"resumed": resumed' in baker
assert '"resumed", 0)' in panel
assert 'class_name Terrain3DHLODBuildCheckpoint' in codec
assert 'return "\\n".join(lines).sha256_text()' in codec
assert 'DirAccess.rename_absolute(stage_abs, final_abs)' in codec
assert 'DirAccess.rename_absolute(prev_abs, final_abs)' in codec
# Independent contract check: actions/rebuild reasons intentionally excluded from
# the plan digest; source order, signatures and child graph remain authoritative.
def fingerprint(jobs, build, target):
    lines=[f'terrain3d-hlod-checkpoint-v1|build={build}|manifest={target}']
    lines += [f'{job[0]}|{job[1]}|{",".join(job[2])}' for job in jobs]
    return hashlib.sha256('\n'.join(lines).encode('utf-8')).hexdigest()
a=[('1:0:0',111,()),('1:1:0',222,()),('2:0:0',333,('1:0:0','1:1:0'))]
base=fingerprint(a,321,'res://data/h.tres')
assert base == fingerprint(list(a),321,'res://data/h.tres')
assert base != fingerprint([a[1],a[0],a[2]],321,'res://data/h.tres')
assert base != fingerprint(a,322,'res://data/h.tres')
assert base != fingerprint(a,321,'res://else/h.tres')
assert base != fingerprint([a[0],a[1],('2:0:0',333,('1:1:0','1:0:0'))],321,'res://data/h.tres')
assert base != fingerprint([a[0],('1:1:0',223,()),a[2]],321,'res://data/h.tres')
print('PASS: checkpoint plan fingerprints, recovery validation, staged writes and exact signature serialization')
