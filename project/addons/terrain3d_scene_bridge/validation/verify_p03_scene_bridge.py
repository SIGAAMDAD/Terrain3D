#!/usr/bin/env python3
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
rules=(root/'bridge_scene_rules.gd').read_text()
index=(root/'bridge_region_index.gd').read_text()
service=(root/'bridge_service.gd').read_text()
plugin=(root/'plugin.cfg').read_text()
checks=[]
def ck(name, cond): checks.append((name, bool(cond)))
ck('category constants', all(x in rules for x in ['INSTANCE_CATEGORY_CLUTTER: int = 0','INSTANCE_CATEGORY_GROUND_COVER: int = 1','INSTANCE_CATEGORY_OCCLUDER: int = 2']))
ck('explicit metadata precedence', 'cursor.has_meta(META_INSTANCE_CATEGORY)' in rules and rules.index('cursor.has_meta(META_INSTANCE_CATEGORY)') < rules.index('_category_from_semantic_name(String(cursor.name))'))
ck('foliage alias', 'token == "foliage"' in rules)
ck('ground cover alias', 'token == "groundcover"' in rules)
ck('occluder semantic', 'token.ends_with("occluders")' in rules)
ck('path fallback', '_category_from_source_path(scene_path)' in rules)
ck('category-qualified key', '::category::' in rules and 'asset_key_with_category' in rules)
ck('placement carries category', index.count('"instance_category":') >= 4)
ck('compound key carries category', 'compound_asset_key(scene_path, instance_category)' in index)
ck('single key carries category', 'mesh_asset_key(scene_path, mesh_path, instance_category)' in index)
ck('new asset category before scene', service.index('mesh_asset.call("set_instance_category", instance_category)') < service.index('mesh_asset.call("set_scene_file", mesh_scene)'))
ck('reused asset current category authority', 'existing_asset.call("get_instance_category")' in service)
ck('single preserves occluder sibling', '_find_matching_occluder(mesh_instance)' in rules and 'occluder_clone.owner = packed_root' in rules)
ck('compound preserves occluders', '_collect_occluder_instances(scene_root, occluder_nodes)' in rules and 'occluder_component.owner = packed_root' in rules)
ck('restore retains category metadata', service.count('set_meta(META_INSTANCE_CATEGORY, instance_category)') >= 2)
ck('visual scope remains ground landmarks', 'GROUND_NODE_NAME' in index and 'LANDMARKS_NODE_NAME' in index and 'WaterSystem' not in index)
ck('plugin version', 'version="1.9.0"' in plugin)
failed=[n for n,v in checks if not v]
for n,v in checks: print(('PASS' if v else 'FAIL')+': '+n)
print(f'{sum(v for _,v in checks)}/{len(checks)} PASS')
sys.exit(1 if failed else 0)
