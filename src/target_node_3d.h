// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TARGET_NODE3D_CLASS_H
#define TARGET_NODE3D_CLASS_H

#include <godot_cpp/classes/node3d.hpp>

#include "Terrain3DConstants.h"

class T3DTargetNode3D {
	CLASS_NAME_STATIC( "Terrain3DTargetNode3D" );

private:
	uint64_t m_instanceId = 0;

public:
	void clear( void ) {
		m_instanceId = 0;
	}

	void setTarget( Node3D *const pNode ) {
		if ( pNode && !pNode->is_queued_for_deletion() ) {
			m_instanceId = pNode->get_instance_id();
		} else {
			clear();
		}
	}

	Node3D *getTarget( void ) const {
		if ( m_instanceId == 0 ) {
			return nullptr;
		}
		Object *obj = ObjectDB::get_instance( m_instanceId );
		return obj ? Object::cast_to<Node3D>( obj ) : nullptr;
	}

	Node3D *ptr( void ) const {
		return getTarget();
	}

	bool isValid( void ) const {
		Node3D *node = getTarget();
		return node && node->is_inside_tree() && !node->is_queued_for_deletion();
	}

	bool isNull( void ) const {
		return !isValid();
	}
};

#endif // TARGET_NODE3D_CLASS_H
