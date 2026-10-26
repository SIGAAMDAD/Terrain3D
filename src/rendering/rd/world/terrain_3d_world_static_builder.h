// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_STATIC_BUILDER_H
#define TERRAIN3D_WORLD_STATIC_BUILDER_H

#include "rendering/rd/world/terrain_3d_world_geometry.h"
#include "rendering/rd/world/terrain_3d_world_publication.h"

struct t3dWorldStaticBuildSource_t {
	const T3DWorldGeometryDatabase *m_pGeometry = NULL;
	const t3dWorldTransform_t *m_pTransforms = NULL;
	const t3dWorldInstance_t *m_pInstances = NULL;
	const t3dWorldCell_t *m_pCells = NULL;
	const t3dRenderingMaterialGpu_t *m_pMaterials = NULL;
	uint32_t m_nTransformCount = 0u;
	uint32_t m_nInstanceCount = 0u;
	uint32_t m_nCellCount = 0u;
	uint32_t m_nMaterialCount = 0u;
};

class Terrain3DWorldStaticBuilder {
public:
	static bool buildCold( const t3dWorldStaticBuildSource_t &rSource, Terrain3DWorldPublication &rTarget );
};

#endif // TERRAIN3D_WORLD_STATIC_BUILDER_H
