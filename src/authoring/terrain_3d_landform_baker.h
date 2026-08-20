// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Editor-time semantic landform authoring and procedural cliff generation.
#pragma once

#ifndef TERRAIN3D_LANDFORM_BAKER_H
#define TERRAIN3D_LANDFORM_BAKER_H

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include "Terrain3DConstants.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class Terrain3D;

class Terrain3DLandformBaker : public RefCounted {
	GDCLASS( Terrain3DLandformBaker, RefCounted );
	CLASS_NAME();

	mutable Dictionary m_materialCache;

public:
	typedef enum t3dmode_e {
		RIDGE = 0,
		VALLEY,
		SCARP,
		BASIN,
		PLATEAU,
		TERRACE,
		ROAD,
		PASS,
		TALUS,
		HYDRAULIC_CHANNEL,
		THERMAL_RELAXATION,
		DUNE,
		SHORELINE,
		MORPHOLOGY,
		OCCLUSION,
		MODE_MAX,
	} t3dmode_t;

	typedef enum t3dmorphologyMode_e {
		MORPH_EXPAND_PEAKS = 0,
		MORPH_FILL_PITS,
		MORPH_RIDGE_SHARPEN,
		MORPH_VALLEY_SHARPEN,
		MORPH_PLATEAU_CLEAN,
		MORPH_MAX,
	} t3dmorphologyMode_t;

	typedef enum t3dgeometryMode_e {
		GEOMETRY_TERRAIN_ONLY = 0,
		GEOMETRY_MODULAR_INSTANCED,
		GEOMETRY_PROCEDURAL_UNIQUE,
		GEOMETRY_MAX,
	} t3dgeometryMode_t;

	Dictionary apply( Terrain3D *pTerrain, const PackedVector3Array &rPath, const Dictionary &rSettings );
	void restore( Terrain3D *pTerrain, const Array &rSnapshots, const AABB &rEditedArea );
	Ref<ArrayMesh> buildCliffMesh(
		Terrain3D *pTerrain, const PackedVector3Array &rPath,
		const Dictionary &rSettings
	) const;
	Dictionary buildCliffModules(
		Terrain3D *pTerrain, const PackedVector3Array &rPath,
		const Dictionary &rSettings
	) const;
	Ref<ArrayMesh> buildCliffCollisionMesh(
		Terrain3D *pTerrain, const PackedVector3Array &rPath,
		const Dictionary &rSettings
	) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( Terrain3DLandformBaker::t3dmode_t );
VARIANT_ENUM_CAST( Terrain3DLandformBaker::t3dmorphologyMode_t );
VARIANT_ENUM_CAST( Terrain3DLandformBaker::t3dgeometryMode_t );
#endif // TERRAIN3D_LANDFORM_BAKER_H
