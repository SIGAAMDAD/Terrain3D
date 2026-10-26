// Copyright (c) 2026 Terrain3D contributors.
#include "authoring/meshes/terrain_3d_meshopt_cook.h"
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <EASTL/vector.h>
#include <math.h>
#include <stdint.h>

#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
#include "meshoptimizer.h"
#if MESHOPTIMIZER_VERSION != 1030
#error "Terrain3D requires the pinned meshoptimizer v1.3 headers"
#endif
#endif

namespace t3dMeshoptCook
{
    bool hasEmbeddedLods( const Ref< ArrayMesh > &rSource )
    {
        if ( rSource.is_null() ) { return false; }
        RenderingServer *const pServer = RenderingServer::get_singleton();
        if ( pServer == NULL ) { return true; } // fail closed
        for ( int s = 0; s < rSource->get_surface_count(); ++s ) {
            const Dictionary surface = pServer->mesh_get_surface( rSource->get_rid(), s );
            if ( surface.is_empty() ) { return true; }
            if ( surface.has( "lods" ) ) {
                const Array lods = surface["lods"];
                if ( !lods.is_empty() ) { return true; }
            }
        }
        return false;
    }

    bool available( void )
    {
#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
        return true;
#else
        return false;
#endif
    }
    uint32_t version( void )
    {
#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
        return MESHOPTIMIZER_VERSION;
#else
        return 0u;
#endif
    }

#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
    static bool supported( const Array &rArrays, PackedVector3Array &rPositions,
        PackedInt32Array &rIndices )
    {
        if ( rArrays.size() != Mesh::ARRAY_MAX ) {
            return false;
        }
        for ( int nSlot = Mesh::ARRAY_CUSTOM0; nSlot <= Mesh::ARRAY_CUSTOM3; ++nSlot ) {
            if ( rArrays[nSlot].get_type() != Variant::NIL ) {
                return false;
            }
        }
        if ( rArrays[Mesh::ARRAY_BONES].get_type() != Variant::NIL ||
            rArrays[Mesh::ARRAY_WEIGHTS].get_type() != Variant::NIL ) {
            return false;
        }
        rPositions = rArrays[Mesh::ARRAY_VERTEX];
        rIndices = rArrays[Mesh::ARRAY_INDEX];
        const int64_t nVertices = rPositions.size();
        const int64_t nIndices = rIndices.is_empty() ? nVertices : rIndices.size();
        if ( nVertices < 3 || nIndices < 3 || nIndices % 3 != 0 || nVertices > INT32_MAX ) {
            return false;
        }
        const PackedVector3Array aNormals = rArrays[Mesh::ARRAY_NORMAL];
        const PackedVector2Array aUvs = rArrays[Mesh::ARRAY_TEX_UV];
        const PackedVector2Array aUv2 = rArrays[Mesh::ARRAY_TEX_UV2];
        const PackedColorArray aColors = rArrays[Mesh::ARRAY_COLOR];
        const PackedFloat32Array aTangents = rArrays[Mesh::ARRAY_TANGENT];
        if ( (!aNormals.is_empty() && aNormals.size() != nVertices) ||
            (!aUvs.is_empty() && aUvs.size() != nVertices) ||
            (!aUv2.is_empty() && aUv2.size() != nVertices) ||
            (!aColors.is_empty() && aColors.size() != nVertices) ||
            (!aTangents.is_empty() && aTangents.size() != nVertices * 4) ) {
            return false;
        }
        for ( int64_t i = 0; i < nIndices; ++i ) {
            int32_t nIndex = rIndices.is_empty() ? (int32_t) i : rIndices[i];
            if ( nIndex < 0 || nIndex >= nVertices ) {
                return false;
            }
        }
        return true;
    }

    static bool remapStreams( const Array &rInput, const eastl::vector< unsigned int > &rMapping,
        uint32_t nNewVertices, const eastl::vector< unsigned int > &rIndices, Array &rOutput )
    {
        const int64_t nOldVertices = (int64_t) rMapping.size();
        PackedVector3Array aPositions = rInput[Mesh::ARRAY_VERTEX];
        PackedVector3Array aNormals = rInput[Mesh::ARRAY_NORMAL];
        PackedVector2Array aUvs = rInput[Mesh::ARRAY_TEX_UV];
        PackedVector2Array aUv2 = rInput[Mesh::ARRAY_TEX_UV2];
        PackedColorArray aColors = rInput[Mesh::ARRAY_COLOR];
        PackedFloat32Array aTangents = rInput[Mesh::ARRAY_TANGENT];
        PackedVector3Array bPositions; bPositions.resize( nNewVertices );
        PackedVector3Array bNormals;
        PackedVector2Array bUvs;
        PackedVector2Array bUv2;
        PackedColorArray bColors;
        PackedFloat32Array bTangents;
        if ( !aNormals.is_empty() ) { bNormals.resize( nNewVertices ); }
        if ( !aUvs.is_empty() ) { bUvs.resize( nNewVertices ); }
        if ( !aUv2.is_empty() ) { bUv2.resize( nNewVertices ); }
        if ( !aColors.is_empty() ) { bColors.resize( nNewVertices ); }
        if ( !aTangents.is_empty() ) { bTangents.resize( nNewVertices * 4 ); }
        for ( int64_t v = 0; v < nOldVertices; ++v ) {
            const unsigned int nDst = rMapping[(size_t) v];
            if ( nDst == ~0u ) { continue; }
            if ( nDst >= nNewVertices ) { return false; }
            bPositions[nDst] = aPositions[v];
            if ( !aNormals.is_empty() ) { bNormals[nDst] = aNormals[v]; }
            if ( !aUvs.is_empty() ) { bUvs[nDst] = aUvs[v]; }
            if ( !aUv2.is_empty() ) { bUv2[nDst] = aUv2[v]; }
            if ( !aColors.is_empty() ) { bColors[nDst] = aColors[v]; }
            if ( !aTangents.is_empty() ) {
                for ( int t = 0; t < 4; ++t ) { bTangents[nDst * 4u + t] = aTangents[v * 4 + t]; }
            }
        }
        PackedInt32Array bIndices; bIndices.resize( (int64_t) rIndices.size() );
        for ( size_t i = 0; i < rIndices.size(); ++i ) {
            if ( rIndices[i] >= nNewVertices ) { return false; }
            bIndices[(int64_t) i] = (int32_t) rIndices[i];
        }
        rOutput = rInput.duplicate( true );
        rOutput[Mesh::ARRAY_VERTEX] = bPositions;
        rOutput[Mesh::ARRAY_NORMAL] = bNormals;
        rOutput[Mesh::ARRAY_TEX_UV] = bUvs;
        rOutput[Mesh::ARRAY_TEX_UV2] = bUv2;
        rOutput[Mesh::ARRAY_COLOR] = bColors;
        rOutput[Mesh::ARRAY_TANGENT] = bTangents;
        rOutput[Mesh::ARRAY_INDEX] = bIndices;
        return true;
    }
#endif

    bool optimizeArrays( const Array &rInput, Array &rOutput )
    {
#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
        PackedVector3Array aPositions;
        PackedInt32Array aIndices;
        if ( !supported( rInput, aPositions, aIndices ) ) { return false; }
        const size_t nVertices = (size_t) aPositions.size();
        const size_t nIndices = aIndices.is_empty() ? nVertices : (size_t) aIndices.size();
        eastl::vector< unsigned int > aSource; aSource.resize( nIndices );
        for ( size_t i = 0; i < nIndices; ++i ) {
            aSource[i] = aIndices.is_empty() ? (unsigned int) i : (unsigned int) aIndices[(int64_t) i];
        }
        eastl::vector< unsigned int > aReordered; aReordered.resize( nIndices );
        meshopt_optimizeVertexCache( aReordered.data(), aSource.data(), nIndices, nVertices );
        eastl::vector< unsigned int > aMapping; aMapping.resize( nVertices );
        const size_t nUnique = meshopt_optimizeVertexFetchRemap( aMapping.data(), aReordered.data(), nIndices, nVertices );
        if ( nUnique == 0 || nUnique > nVertices ) { return false; }
        for ( size_t i = 0; i < nIndices; ++i ) {
            if ( aMapping[aReordered[i]] >= nUnique ) { return false; }
            aReordered[i] = aMapping[aReordered[i]];
        }
        return remapStreams( rInput, aMapping, (uint32_t) nUnique, aReordered, rOutput );
#else
        (void) rInput; (void) rOutput;
        return false;
#endif
    }

    bool generateLodMesh( const Ref< ArrayMesh > &rSource, float fRatio,
        float fMaxWorldError, Ref< ArrayMesh > &rOutput, float &rMeasuredError,
        uint32_t &rSourceTriangles, uint32_t &rCookedTriangles )
    {
#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
        if ( rSource.is_null() || rSource->get_blend_shape_count() != 0 ||
            rSource->get_surface_count() <= 0 || fRatio <= 0.0f || fRatio >= 1.0f ||
            !isfinite(fMaxWorldError) || fMaxWorldError < 0.0f ) {
            return false;
        }
        if ( hasEmbeddedLods( rSource ) ) { return false; }
        Ref< ArrayMesh > result;
        result.instantiate();
        uint64_t nSource = 0u;
        uint64_t nReduced = 0u;
        float fError = 0.0f;
        for ( int s = 0; s < rSource->get_surface_count(); ++s ) {
            if ( rSource->surface_get_primitive_type( s ) != Mesh::PRIMITIVE_TRIANGLES ) {
                return false;
            }
            Array original = rSource->surface_get_arrays( s );
            PackedVector3Array positions;
            PackedInt32Array indices;
            if ( !supported( original, positions, indices ) ) {
                return false;
            }
            if ( indices.is_empty() ) {
                indices.resize( positions.size() );
                for ( int i = 0; i < indices.size(); ++i ) { indices.set(i, i); }
            }
            PackedInt32Array simplified;
            float fSurfaceError = 0.0f;
            if ( !simplifyArrays( original, indices, fRatio, fMaxWorldError,
                    simplified, fSurfaceError ) || simplified.size() >= indices.size() ) {
                return false;
            }
            Array arrays = original.duplicate( true );
            arrays[Mesh::ARRAY_INDEX] = simplified;
            Array optimized;
            if ( !optimizeArrays( arrays, optimized ) ) {
                return false;
            }
            result->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, optimized,
                TypedArray< Array >(), Dictionary(), rSource->surface_get_format( s ) );
            result->surface_set_material( s, rSource->surface_get_material( s ) );
            result->surface_set_name( s, rSource->surface_get_name( s ) );
            nSource += (uint64_t) indices.size() / 3u;
            nReduced += (uint64_t) simplified.size() / 3u;
            if ( fSurfaceError > fError ) { fError = fSurfaceError; }
        }
        if ( nSource == 0u || nReduced >= nSource || nSource > UINT32_MAX ||
            nReduced > UINT32_MAX ) {
            return false;
        }
        result->set_custom_aabb( rSource->get_custom_aabb() );
        rMeasuredError = fError;
        rSourceTriangles = (uint32_t) nSource;
        rCookedTriangles = (uint32_t) nReduced;
        rOutput = result;
        return true;
#else
        (void) rSource; (void) fRatio; (void) fMaxWorldError;
        (void) rOutput; (void) rMeasuredError;
        (void) rSourceTriangles; (void) rCookedTriangles;
        return false;
#endif
    }

    bool simplifyArrays( const Array &rInput, const PackedInt32Array &rIndices,
        float fRatio, float fMaxWorldError, PackedInt32Array &rOutput, float &rWorldError )
    {
#if defined( T3D_MESHOPT_AVAILABLE ) && T3D_MESHOPT_AVAILABLE
        PackedVector3Array aPositions;
        PackedInt32Array aOriginal;
        if ( !supported( rInput, aPositions, aOriginal ) || rIndices.size() < 3 ) { return false; }
        const size_t nVertices = (size_t) aPositions.size();
        const size_t nIndices = (size_t) rIndices.size();
        if ( nIndices % 3 != 0 || fRatio <= 0.0f || fMaxWorldError < 0.0f ) { return false; }
        eastl::vector< unsigned int > aSource; aSource.resize( nIndices );
        eastl::vector< float > aPositionFloats; aPositionFloats.resize( nVertices * 3u );
        for ( size_t i = 0; i < nVertices; ++i ) {
            const Vector3 p = aPositions[(int64_t) i];
            aPositionFloats[i * 3u] = (float) p.x;
            aPositionFloats[i * 3u + 1u] = (float) p.y;
            aPositionFloats[i * 3u + 2u] = (float) p.z;
        }
        for ( size_t i = 0; i < nIndices; ++i ) {
            int32_t nIndex = rIndices[(int64_t) i];
            if ( nIndex < 0 || (size_t) nIndex >= nVertices ) { return false; }
            aSource[i] = (unsigned int) nIndex;
        }
        const PackedVector3Array aNormals = rInput[Mesh::ARRAY_NORMAL];
        const PackedVector2Array aUvs = rInput[Mesh::ARRAY_TEX_UV];
        const PackedVector2Array aUv2 = rInput[Mesh::ARRAY_TEX_UV2];
        const PackedColorArray aColors = rInput[Mesh::ARRAY_COLOR];
        const PackedFloat32Array aTangents = rInput[Mesh::ARRAY_TANGENT];
        eastl::vector< float > aWeights;
        if ( !aNormals.is_empty() ) { aWeights.push_back(0.5f); aWeights.push_back(0.5f); aWeights.push_back(0.5f); }
        if ( !aUvs.is_empty() ) { aWeights.push_back(0.2f); aWeights.push_back(0.2f); }
        if ( !aUv2.is_empty() ) { aWeights.push_back(0.2f); aWeights.push_back(0.2f); }
        if ( !aColors.is_empty() ) { for (int c=0; c<4; ++c) { aWeights.push_back(0.1f); } }
        if ( !aTangents.is_empty() ) { for (int c=0; c<4; ++c) { aWeights.push_back(0.25f); } }
        const size_t nAttrib = aWeights.size();
        eastl::vector< float > aAttributes; aAttributes.resize( nVertices * nAttrib );
        for ( size_t i = 0; i < nVertices; ++i ) {
            size_t o = i * nAttrib;
            if ( !aNormals.is_empty() ) {
                const Vector3 n = aNormals[(int64_t) i];
                aAttributes[o++] = (float) n.x; aAttributes[o++] = (float) n.y; aAttributes[o++] = (float) n.z;
            }
            if ( !aUvs.is_empty() ) {
                const Vector2 u = aUvs[(int64_t) i];
                aAttributes[o++] = (float) u.x; aAttributes[o++] = (float) u.y;
            }
            if ( !aUv2.is_empty() ) {
                const Vector2 u = aUv2[(int64_t) i];
                aAttributes[o++] = (float) u.x; aAttributes[o++] = (float) u.y;
            }
            if ( !aColors.is_empty() ) {
                const Color c = aColors[(int64_t) i];
                aAttributes[o++] = c.r; aAttributes[o++] = c.g;
                aAttributes[o++] = c.b; aAttributes[o++] = c.a;
            }
            if ( !aTangents.is_empty() ) {
                const size_t t = i * 4u;
                aAttributes[o++] = aTangents[(int64_t) t];
                aAttributes[o++] = aTangents[(int64_t) t + 1u];
                aAttributes[o++] = aTangents[(int64_t) t + 2u];
                aAttributes[o++] = aTangents[(int64_t) t + 3u];
            }
        }
        size_t nTarget = (size_t) ( (float) nIndices * fRatio / 3.0f ) * 3u;
        if ( nTarget < 3u ) { nTarget = 3u; }
        if ( nTarget > nIndices ) { nTarget = nIndices; }
        eastl::vector< unsigned int > aOutput; aOutput.resize( nIndices );
        float fActualError = 0.0f;
        const size_t nResult = meshopt_simplifyWithAttributes( aOutput.data(), aSource.data(), nIndices,
            aPositionFloats.data(), nVertices, 3u * sizeof(float),
            nAttrib ? aAttributes.data() : NULL, nAttrib * sizeof(float),
            nAttrib ? aWeights.data() : NULL, nAttrib, NULL, nTarget, fMaxWorldError,
            meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &fActualError );
        if ( nResult < 3u || nResult > nIndices || nResult % 3u != 0 ||
            !isfinite(fActualError) || fActualError > fMaxWorldError + 0.0001f ) { return false; }
        PackedInt32Array bIndices; bIndices.resize( (int64_t) nResult );
        for ( size_t i = 0; i < nResult; ++i ) { bIndices[(int64_t) i] = (int32_t) aOutput[i]; }
        rWorldError = fActualError;
        rOutput = bIndices;
        return true;
#else
        (void) rInput; (void) rIndices; (void) fRatio; (void) fMaxWorldError;
        (void) rOutput; (void) rWorldError;
        return false;
#endif
    }
}
