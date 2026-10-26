// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HLOD_BUILDER_H
#define TERRAIN3D_HLOD_BUILDER_H

#include "Terrain3DConstants.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include "data/model/terrain_3d_region.h"
#include "terrain_3d.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Native geometry workhorse for the editor-only HLOD authoring pipeline.
// GDScript owns orchestration/transactional publishing; this class keeps the
// expensive instance-store traversal, vertex compaction and error measurement
// out of Variant/Dictionary-heavy script loops.
class Terrain3DHLODBuilder : public RefCounted
{
	GDCLASS( Terrain3DHLODBuilder, RefCounted );
	CLASS_NAME();

public:
	PackedInt32Array getInstancerMeshIds( Terrain3DRegion *pRegion ) const;
	Dictionary		 analyzeInstancerSource( Terrain3D *pTerrain, Terrain3DRegion *pRegion, int nMeshId ) const;
	Dictionary        extractInstancerTransforms( Terrain3D *pTerrain, Terrain3DRegion *pRegion, int nMeshId ) const;
	Dictionary		 buildInstancerSource( Terrain3D *pTerrain, Terrain3DRegion *pRegion, int nMeshId,
		bool bPreserveTopology = false, real_t fMaxWorldError = 0.0f ) const;
	Dictionary		 optimizeMesh( const Ref< ArrayMesh > &rMesh ) const;
	Dictionary		 simplifyMesh( const Ref< ArrayMesh > &rMesh, real_t fRatio ) const;
    PackedByteArray encodeSinglePrototypeGroup( const Ref< ArrayMesh > &rMesh, const Array &rTransforms, int64_t nSignature ) const;
    Array decodeSinglePrototypeGroup( const PackedByteArray &rPayload ) const;
    PackedByteArray encodeMultiPrototypeGroup( const Array &rMeshes, const Array &rTransforms,
        const PackedInt32Array &rPrototypeIndices, const Array &rSignatures ) const;
    Dictionary decodeMultiPrototypeGroup( const PackedByteArray &rPayload ) const;
	Dictionary		 simplifyMeshWithError( const Ref< ArrayMesh > &rMesh, real_t fRatio, real_t fMaxWorldError ) const;

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_HLOD_BUILDER_H
