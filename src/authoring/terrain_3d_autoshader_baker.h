// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Editor-time procedural material classification baked into Terrain3D control maps.
#pragma once

#ifndef TERRAIN3D_AUTOSHADER_BAKER_H
#define TERRAIN3D_AUTOSHADER_BAKER_H

#include "Terrain3DConstants.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class Terrain3D;

class Terrain3DAutoShaderBaker : public RefCounted {
	GDCLASS( Terrain3DAutoShaderBaker, RefCounted );
	CLASS_NAME();

protected:
	static void _bind_methods( void );

public:
	Dictionary
	bakeRegions( Terrain3D *pTerrain, const Dictionary &rProfile, const TypedArray<Vector2i> &rRegionLocations );
	Dictionary computeRegionSignatures( Terrain3D *pTerrain,
			const Dictionary &rProfile,
			const TypedArray<Vector2i> &rRegionLocations );
	Dictionary clearBakedRegions( Terrain3D *pTerrain,
			const TypedArray<Vector2i> &rRegionLocations,
			bool bPreserveAutoMask = true );
};
#endif // TERRAIN3D_AUTOSHADER_BAKER_H
