#!/usr/bin/env python3
"""Static U04 integration guard. Native implementation receives its own compile checks."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
def text(p):
    return (root / p).read_text()
def check(name, condition):
    if not condition:
        raise AssertionError(name)
    print('PASS:', name)

package_h = text('src/hlod/format/terrain_3d_hlod_package.h')
package = text('src/hlod/format/terrain_3d_hlod_package.cpp')
builder = text('src/hlod/build/terrain_3d_hlod_builder.cpp')
streamer = text('src/hlod/runtime/terrain_3d_hlod_streamer.cpp')
streamer_h = text('src/hlod/runtime/terrain_3d_hlod_streamer.h')
publisher = text('src/rendering/rd/core/terrain_3d_render_publication_builder.cpp')
baker = text('project/addons/terrain_3d/tools/hlod_baker.gd')
check('v4 prototype references persisted', 'm_prototypeMeshes' in package_h and '"prototype_meshes"' in package)
check('v3 backward compatibility', 'm_nFormatVersion == 3' in package and 'm_nFormatVersion != 3 && m_nFormatVersion != 4' in package)
check('v4 validates all prototype bounds and metadata', 'prototype.m_nVertexBytes' in package and 'prototype.m_fBounds[k]' in package)
check('v4 validates declared residency', 'm_nEstimatedGpuBytes' in package and 'packageBytes' in package)
check('payload is validated once at cold load', 'validateRuntimeInstanceGroupDecodedCold( decoded )' in streamer and 'entry.m_groupInstances.swap( decoded.m_instances )' in streamer)
check('prototype grouping sorted on cold load', 'eastl::sort( decoded.m_instances.begin()' in streamer)
check('all source keys unique per prototype', 'nPrototype << 32u' in streamer and 'rdMeshSourceKey( i, p )' in streamer)
check('per-prototype material resolver context', 'm_nSurfaceOffset + nSurface' in streamer and 'm_materialContext' in streamer_h)
check('independent source instance cell capacities', all(x in streamer_h for x in ('getRdPublicationSourceCapacity','getRdPublicationInstanceCapacity','getRdPublicationCellCapacity')) and all(x in publisher for x in ('hlod_sources','hlod_instances','hlod_cells')))
check('group ACK still drives visual suppression', 'entry.m_nAwaitedGpuGeneration = m_nRdPublicationGeneration' in streamer and 'entry.m_bGpuReady' in streamer)
check('native codec binds multi-prototype routines', 'encode_multi_prototype_group' in builder and 'decode_multi_prototype_group' in builder)
check('direct instancer extraction avoids SurfaceTool expansion', 'extractInstancerTransforms(' in builder and 'cell.getTransform( j )' in builder and 't3dHlodBuilderBuildInstancerChunk' not in builder[builder.index('Dictionary Terrain3DHLODBuilder::extractInstancerTransforms('):builder.index('Dictionary Terrain3DHLODBuilder::buildInstancerSource(')])
check('editor uses source transforms and flattens parent group', 'source.direct_instance_transforms' in baker and 'decode_multi_prototype_group' in baker and 'group_meshes[prototype_index]' in baker)
check('fallback remains bounded', 'force_merged_instancer' in baker and '_materialize_deferred_instancer_sources(terrain, inputs, 2)' in baker)
check('versioned dirty-only invalidation retains instance-group signature', 'instances=v4|multi-prototype' in baker and 'const BUILDER_VERSION: int = 13' in baker)
