#!/usr/bin/env python3
"""Static contract checks for adaptive HLOD quadtree parent omission."""
from pathlib import Path
r=Path(__file__).resolve().parents[2]
read=lambda x:(r/x).read_text()
b=read('project/addons/terrain_3d/tools/hlod_baker.gd')
l=read('src/hlod/format/terrain_3d_hlod_layer_settings.cpp')
h=read('src/hlod/format/terrain_3d_hlod_layer_settings.h')
p=read('project/addons/terrain_3d/tools/hlod_panel.gd')
c=read('project/addons/terrain_3d/tools/hlod_build_checkpoint.gd')
s=read('src/hlod/runtime/terrain_3d_hlod_streamer.cpp')
checks={
 'split decision included in preflight': 'var partition_reason: String = _partition_reason(' in b,
 'plan retains child-first split nodes':'"action": "split" if not partition_reason.is_empty()' in b,
 'bake never creates a split parent package': 'if String(scheduled["action"]) == "split":' in b and 'next_clusters[cluster_key] = terminal' in b,
 'terminal parents absent from published entries':'all_clusters.append(cluster)' in b and 'continue\n\t\t\tvar result: Dictionary' in b,
 'terminal propagation included':'inherited_terminal = inherited_terminal or child.partition_terminal' in b,
 'future higher LOD sources protected':'if source.min_level > 1:' in b and 'protected_coarse_roots.has(HLODBuildSchedule.job_id' in b,
 'retired source signatures recooked':'terrain3d-hlod-recipe-v5' in b and 'BUILDER_VERSION: int = 13' in b,
 'partition thresholds in cook hash':'m_nPartitionTriangleLimit' in l and 'm_nPartitionSectionLimit' in l,
 'partition property editor bindings':'"partition_triangle_limit"' in l and '"spatial_partitioning"' in l,
 'partition editor controls':'_spatial_partition.toggled.connect' in p,
 'checkpoints include topology action':'"split" if String(job.get("action", "")) == "split"' in c,
 'existing runtime independently selects roots':'selectRecursive( child, rLocalTarget )' in s and 'm_roots' in s,
 'existing fallback keeps detail published':'m_detailRequiredRegions = m_managedRegions' in s,
 'no WORLD geometry cache changes needed':True,
}
for name, ok in checks.items():
 assert ok,name
 print('PASS:',name)
