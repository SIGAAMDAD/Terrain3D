// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_TARGET_NODE_3D_H
#define TERRAIN3D_TARGET_NODE_3D_H

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"

#include <godot_cpp/classes/node3d.hpp>

class Terrain3DTargetNode3D
{
	CLASS_NAME_STATIC( "Terrain3DTargetNode3D" );

private:
	uint64_t m_instanceId = 0;

public:
	EA_FORCE_INLINE void clear(
		void ) {
		m_instanceId = 0;
	}

	void setTarget(
		Node3D *const pNode ) {
		if ( pNode && !pNode->is_queued_for_deletion() ) {
			m_instanceId = pNode->get_instance_id();
		} else {
			clear();
		}
	}

	Node3D *getTarget(
		void ) const {
		if ( m_instanceId == 0 ) {
			return NULL;
		}
		Object *obj = ObjectDB::get_instance( m_instanceId );
		return obj ? Object::cast_to< Node3D >( obj ) : NULL;
	}

	EA_FORCE_INLINE Node3D *ptr(
		void ) const {
		return getTarget();
	}

	EA_FORCE_INLINE bool isValid(
		void ) const {
		Node3D *node = getTarget();
		return node && node->is_inside_tree() && !node->is_queued_for_deletion();
	}

	EA_FORCE_INLINE bool isNull(
		void ) const {
		return !isValid();
	}
};

#endif // TERRAIN3D_TARGET_NODE_3D_H
