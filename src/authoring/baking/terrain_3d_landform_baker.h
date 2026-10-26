// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Editor-time semantic landform authoring and procedural cliff generation.
#ifndef TERRAIN3D_LANDFORM_BAKER_H
#define TERRAIN3D_LANDFORM_BAKER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include "terrain_3d.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif


// t3dMode_e centralized in Terrain3DTypes.h.


// t3dMorphologyMode_e centralized in Terrain3DTypes.h.


// t3dGeometryMode_e centralized in Terrain3DTypes.h.


class Terrain3DLandformBaker : public RefCounted {
	GDCLASS( Terrain3DLandformBaker, RefCounted );
	CLASS_NAME();

	mutable Dictionary m_materialCache;

public:
	Dictionary apply( Terrain3D *pTerrain, const PackedVector3Array &rPath, const Dictionary &rSettings );
	void restore( Terrain3D *pTerrain, const Array &rSnapshots, const AABB &rEditedArea );
	Ref<ArrayMesh> buildCliffMesh( Terrain3D *pTerrain, const PackedVector3Array &rPath, const Dictionary &rSettings ) const;
	Dictionary buildCliffModules( Terrain3D *pTerrain, const PackedVector3Array &rPath, const Dictionary &rSettings ) const;
	Ref<ArrayMesh> buildCliffCollisionMesh( Terrain3D *pTerrain, const PackedVector3Array &rPath, const Dictionary &rSettings ) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dMode_t );
VARIANT_ENUM_CAST( t3dMorphologyMode_t );
VARIANT_ENUM_CAST( t3dGeometryMode_t );
#endif // TERRAIN3D_LANDFORM_BAKER_H
