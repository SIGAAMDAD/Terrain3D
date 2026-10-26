#ifndef TERRAIN3D_TEST_INSTANCE_FIXTURE_H
#define TERRAIN3D_TEST_INSTANCE_FIXTURE_H

#include "instances/storage/terrain_3d_instance_store.h"
#include <initializer_list>

namespace terrain3d_tests {
// Deliberately distinct mesh IDs, cells, owners, colors and transforms expose
// ordering, sidecar and copy errors that identical instances would conceal.
inline bool populate_instance_store(Terrain3DInstanceStore &store) {
	for (const int mesh_id : {3, 1}) {
		for (const Vector2i location : {Vector2i(1, 0), Vector2i(0, 1)}) {
			auto *cell = store.tryGetOrCreateCellExact(mesh_id, location, 3, true, true);
			if (!cell) {
				return false;
			}
			for (uint32_t index = 0; index < 3; ++index) {
				Transform3D transform;
				transform.origin = Vector3(mesh_id + index, location.x * 32, location.y * 32);
				if (!cell->pushTransform(transform, FOLLOW_TERRAIN_HEIGHT | FOLLOW_TERRAIN_NORMAL) ||
				    !cell->m_colors.pushBack(0xff302010u + index) || !cell->m_owners.pushBack(100u + index)) {
					return false;
				}
			}
		}
	}
	return store.validateDeep();
}

inline bool same_instance_store(const Terrain3DInstanceStore &left, const Terrain3DInstanceStore &right) {
	if (left.meshCount() != right.meshCount() || left.instanceCount() != right.instanceCount()) {
		return false;
	}
	for (const auto &mesh : left.meshes()) {
		const auto *other_mesh = right.findMesh(mesh.m_nId);
		if (!other_mesh || mesh.m_cells.size() != other_mesh->m_cells.size()) {
			return false;
		}
		for (const auto &cell : mesh.m_cells) {
			const auto *other = right.findCell(mesh.m_nId, cell.m_location);
			if (!other || cell.m_transforms.size() != other->m_transforms.size() ||
			    cell.m_colors.size() != other->m_colors.size() || cell.m_owners.size() != other->m_owners.size()) {
				return false;
			}
			for (size_t index = 0; index < cell.m_transforms.size(); ++index) {
				if (cell.getTransform(index) != other->getTransform(index) ||
				    cell.getTransformBehaviorFlags(index) != other->getTransformBehaviorFlags(index)) {
					return false;
				}
				if (cell.usesColors() && cell.m_colors[index] != other->m_colors[index]) {
					return false;
				}
				if (cell.usesOwners() && cell.m_owners[index] != other->m_owners[index]) {
					return false;
				}
			}
		}
	}
	return right.validateDeep();
}
} // namespace terrain3d_tests
#endif
