// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
// Editor-time procedural material classification baked into Terrain3D control maps.
#pragma once

#ifndef TERRAIN3D_AUTOSHADER_BAKER_H
#define TERRAIN3D_AUTOSHADER_BAKER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "Terrain3DConstants.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DTerrain;

class T3DAutoShaderBaker : public RefCounted {
	using Terrain3DAutoShaderBaker = T3DAutoShaderBaker;
	GDCLASS( Terrain3DAutoShaderBaker, RefCounted );
	CLASS_NAME();

protected:
	static void _bind_methods( void );

public:
	Dictionary bakeRegions(
		T3DTerrain *pTerrain, const Dictionary &rProfile,
		const TypedArray<Vector2i> &rRegionLocations
	);
	Dictionary computeRegionSignatures(
		T3DTerrain *pTerrain, const Dictionary &rProfile,
		const TypedArray<Vector2i> &rRegionLocations
	);
	Dictionary clearBakedRegions(
		T3DTerrain *pTerrain, const TypedArray<Vector2i> &rRegionLocations,
		bool bPreserveAutoMask = true
	);
};

using Terrain3DAutoShaderBaker = T3DAutoShaderBaker;

#endif // TERRAIN3D_AUTOSHADER_BAKER_H
