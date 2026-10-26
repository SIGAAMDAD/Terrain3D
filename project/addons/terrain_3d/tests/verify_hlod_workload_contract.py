#!/usr/bin/env python3
"""Static/source checks + independent quadtree-schedule reference tests.

No Godot parser/runtime is invoked by this script. The native GDScript
contract test is hlod_schedule_test.gd and requires a Godot executable.
"""
from pathlib import Path
import random

root=Path(__file__).resolve().parents[1]
baker=(root/'tools/hlod_baker.gd').read_text()
scheduler=(root/'tools/hlod_build_schedule.gd').read_text()
panel=(root/'tools/hlod_panel.gd').read_text()

assert 'func plan_bake(' in baker
assert 'HLODBuildSchedule.validate_jobs(jobs)' in baker
assert 'func _collect_level_plans(' in baker
assert 'func _source_signature_for_inputs(' in baker
assert baker.count('_source_signature_for_inputs(') >= 3
assert 'func _find_cached_cluster(' in baker
assert baker.count('_find_cached_cluster(') >= 3
assert 'HLODBuildSchedule.sorted_clusters(plans)' in baker
assert '"_reusable_packages": reusable_packages' in baker
assert 'TransactionResourcePublisher.' not in baker
assert 'TransactionalResourcePublisher.publish(' in baker
assert 'func _plan_dirty()' in panel
assert 'plan_button.pressed.connect(_plan_dirty)' in panel
assert 'var _plan_tabs: TabContainer' in panel
assert 'func validate_jobs(' in scheduler
assert 'Vector2i(child_cluster.x >> 1, child_cluster.y >> 1)' in scheduler
print('PASS: offline build-plan hooks, deterministic ordering, shared signatures, staged publication, editor inspection')

# Independent algebra reference: region domain starts at (-32,-32); all
# parent IDs correspond to the correct quadtree cell and child level.
region_min=-32

def key_for(x,z,level):
    span=1<<level
    return ((x-region_min)//span,(z-region_min)//span)

def schedule(sources,max_level=6):
    next_keys={}
    jobs=[]
    for level in range(1,max_level+1):
        plans={}
        if level>1:
            for child in next_keys:
                parent=(child[0]>>1,child[1]>>1)
                plans.setdefault(parent,[]).append((level-1,child))
        for x,z,min_l in sources:
            if min_l==level:
                plans.setdefault(key_for(x,z,level),[])
        next_keys={key:1 for key in plans}
        for key in sorted(plans):
            jobs.append((level,key,sorted(plans[key])))
    return jobs

def valid(jobs):
    seen=set()
    last_level=0
    for level,key,children in jobs:
        assert 1<=level<=6 and level>=last_level
        assert (level,key) not in seen
        for child_level,child in children:
            assert (child_level,child) in seen
            assert child_level==level-1
            assert (child[0]>>1,child[1]>>1)==key
        seen.add((level,key))
        last_level=level

rng=random.Random(1782)
for count in [1,5,20,100,512]:
    sources=[(rng.randint(-32,31),rng.randint(-32,31),rng.randint(1,6)) for _ in range(count)]
    ref=schedule(sources)
    valid(ref)
    for _ in range(5):
        shuffled=sources[:]
        rng.shuffle(shuffled)
        assert schedule(shuffled)==ref
full_grid=[(x,z,1) for x in range(-32,32) for z in range(-32,32)]
full_jobs=schedule(full_grid)
valid(full_jobs)
assert len(full_jobs)==1024+256+64+16+4+1
assert sum(job[0]==6 for job in full_jobs)==1
print('PASS: full 64x64 region map -> 1,365 jobs, single L6 root, valid parent dependencies')
print('PASS: 5 quadtree sizes, deterministic across permutations, child-first dependencies and parent coordinates')

# Detect accidental newline-containing regular strings in the changed .gd files.
for filename in ('tools/hlod_baker.gd','tools/hlod_panel.gd','tools/hlod_build_schedule.gd','tests/hlod_schedule_test.gd'):
    path=root/filename
    for ln,line in enumerate(path.read_text().splitlines(),1):
        if line.lstrip().startswith('#'):
            continue
        count=0
        escaping=False
        for c in line:
            if escaping:
                escaping=False
            elif c=='\\':
                escaping=True
            elif c=='"':
                count+=1
        assert count%2==0,(filename,ln,'unterminated string literal')
print('PASS: quoted-string line balance in edited GDScript')
