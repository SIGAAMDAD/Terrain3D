// Copyright (c) 2026 Terrain3D contributors.
#ifndef TERRAIN3D_MESHOPT_COOK_H
#define TERRAIN3D_MESHOPT_COOK_H

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Cold editor-only adapter. Returns false without changing output if meshoptimizer
// is unavailable or an unsupported vertex format is encountered.
namespace t3dMeshoptCook
{
    bool optimizeArrays( const Array &rInput, Array &rOutput );
    bool simplifyArrays( const Array &rInput, const PackedInt32Array &rIndices,
        float fRatio, float fMaxWorldError, PackedInt32Array &rOutput,
        float &rWorldError );
    // Editor-only conventional LOD cook. Preserves surface/material identities and
    // returns false without publishing anything when unsupported or not reduced.
    bool generateLodMesh( const Ref< ArrayMesh > &rSource, float fRatio,
        float fMaxWorldError, Ref< ArrayMesh > &rOutput, float &rMeasuredError,
        uint32_t &rSourceTriangles, uint32_t &rCookedTriangles );
    bool hasEmbeddedLods( const Ref< ArrayMesh > &rSource );
    bool available( void );
    uint32_t version( void );
}

#endif
