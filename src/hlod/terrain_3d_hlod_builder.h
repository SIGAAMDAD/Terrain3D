// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once
#ifndef TERRAIN3D_HLOD_BUILDER_H
#define TERRAIN3D_HLOD_BUILDER_H

#include "Terrain3DConstants.h"
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DTerrain;
class T3DRegion;

// Native geometry workhorse for the editor-only HLOD authoring pipeline.
// GDScript owns orchestration/transactional publishing; this class keeps the
// expensive instance-store traversal, vertex compaction and error measurement
// out of Variant/Dictionary-heavy script loops.
class T3DHLODBuilder : public RefCounted {
	GDCLASS( T3DHLODBuilder, RefCounted );
	CLASS_NAME();

public:
	PackedInt32Array getInstancerMeshIds( T3DRegion *pRegion ) const;
	Dictionary buildInstancerSource( T3DTerrain *pTerrain, T3DRegion *pRegion, int nMeshId ) const;
	Dictionary simplifyMesh( const Ref<ArrayMesh> &rMesh, real_t fRatio ) const;

protected:
	static void _bind_methods( void );
};
#endif
