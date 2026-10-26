// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/world/terrain_3d_world_godot_converter.h"
#include "data/terrain_3d_half_buffer.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <math.h>
#include <string.h>

using namespace godot;

static uint64_t t3d_world_godot_hash_mix( uint64_t pHash, uint64_t pValue ) {
	pHash ^= pValue + 0x9e3779b97f4a7c15uLL + ( pHash << 6u ) + ( pHash >> 2u );
	return pHash;
}

static uint32_t t3d_world_godot_pack_snorm10( float pValue ) {
	float fValue = pValue;
	if ( fValue < -1.0f ) {
		fValue = -1.0f;
	}
	if ( fValue > 1.0f ) {
		fValue = 1.0f;
	}
	int32_t iValue = ( int32_t )lrintf( fValue * 511.0f );
	return ( uint32_t )iValue & 0x3ffu;
}

static uint32_t t3d_world_godot_pack_normal( const Vector3 &pNormal ) {
	Vector3 N = pNormal;
	if ( N.length_squared() > 0.000001f ) {
		N = N.normalized();
	} else {
		N = Vector3( 0.0f, 1.0f, 0.0f );
	}
	return t3d_world_godot_pack_snorm10( N.x ) | ( t3d_world_godot_pack_snorm10( N.y ) << 10u ) | ( t3d_world_godot_pack_snorm10( N.z ) << 20u );
}

static uint32_t t3d_world_godot_pack_tangent( float pX, float pY, float pZ, float pW ) {
	const uint32_t nHandedness = pW < 0.0f ? 1u : 0u;
	return t3d_world_godot_pack_snorm10( pX ) | ( t3d_world_godot_pack_snorm10( pY ) << 10u ) | ( t3d_world_godot_pack_snorm10( pZ ) << 20u )
	        | ( nHandedness << 30u );
}

static uint32_t t3d_world_godot_pack_color( const Color &pColor ) {
	const float fR = pColor.r < 0.0f ? 0.0f : ( pColor.r > 1.0f ? 1.0f : pColor.r );
	const float fG = pColor.g < 0.0f ? 0.0f : ( pColor.g > 1.0f ? 1.0f : pColor.g );
	const float fB = pColor.b < 0.0f ? 0.0f : ( pColor.b > 1.0f ? 1.0f : pColor.b );
	const float fA = pColor.a < 0.0f ? 0.0f : ( pColor.a > 1.0f ? 1.0f : pColor.a );
	const uint32_t r = ( uint32_t )lrintf( fR * 255.0f );
	const uint32_t g = ( uint32_t )lrintf( fG * 255.0f );
	const uint32_t b = ( uint32_t )lrintf( fB * 255.0f );
	const uint32_t a = ( uint32_t )lrintf( fA * 255.0f );
	return r | ( g << 8u ) | ( b << 16u ) | ( a << 24u );
}

static bool t3d_world_godot_is_two_sided_material(
        const Ref<Material> &pMaterial, const Ref<Terrain3DWorldMaterial> &pExplicitMaterial ) {
	if ( pExplicitMaterial.is_valid() ) {
		return pExplicitMaterial->isTwoSided();
	}
	if ( pMaterial.is_null() ) {
		return false;
	}
	const BaseMaterial3D *const pBase = Object::cast_to<BaseMaterial3D>( pMaterial.ptr() );
	return pBase != NULL && pBase->get_cull_mode() == BaseMaterial3D::CULL_DISABLED;
}

Terrain3DWorldGodotConverter::Terrain3DWorldGodotConverter() :
        m_pBuffers( NULL ), m_bInitialized( false ), m_bNativeOwnershipApplied( false ) {
	memset( &m_policy, 0, sizeof( m_policy ) );
	memset( &m_materialResolver, 0, sizeof( m_materialResolver ) );
}

bool Terrain3DWorldGodotConverter::initializeCold( const T3DWorldConversionPolicy &pPolicy,
        const T3DWorldGodotMaterialResolver &pMaterialResolver,
        T3DWorldSceneBuildBuffers *pBuffers ) {
	if ( pBuffers == NULL || pBuffers->pGeometry == NULL || pBuffers->pMaterials == NULL || pBuffers->pCells == NULL
	        || pBuffers->pInstances == NULL || pBuffers->pTransforms == NULL || pBuffers->pInstanceSourceIds == NULL || pBuffers->pOwnership == NULL
	        || pBuffers->pPrototypeEntries == NULL || pBuffers->pPrototypeBoundsByMeshId == NULL || pBuffers->pMaterialEntries == NULL
	        || pBuffers->pNativeVisuals == NULL || pBuffers->Scratch.pVertices == NULL || pBuffers->Scratch.pIndices == NULL
	        || pBuffers->Scratch.pTriangleMaterialOffsets == NULL || pBuffers->Scratch.pTriangleTiers == NULL ) {
		return false;
	}
	if ( pBuffers->nMaterialCapacity == 0u || pBuffers->nCellCapacity == 0u || pBuffers->nInstanceCapacity == 0u
	        || pBuffers->nPrototypeCapacity == 0u || pBuffers->nPrototypeBoundsCapacity == 0u || pBuffers->nMaterialEntryCapacity == 0u
	        || pBuffers->nNativeVisualCapacity == 0u ) {
		return false;
	}

	m_policy = pPolicy;
	m_materialResolver = pMaterialResolver;
	m_pBuffers = pBuffers;
	m_bInitialized = true;
	m_bNativeOwnershipApplied = false;
	return true;
}

void Terrain3DWorldGodotConverter::setPolicyCold( const T3DWorldConversionPolicy &pPolicy ) {
	m_policy = pPolicy;
}

bool Terrain3DWorldGodotConverter::rebuildSceneCold( Node *pRoot, T3DWorldSceneBuildStats &rStats ) {
	memset( &rStats, 0, sizeof( rStats ) );
	if ( !m_bInitialized || pRoot == NULL ) {
		return false;
	}

	releaseNativeOwnershipCold();
	const T3DWorldGeometryDatabaseBuffers GeometryBuffers = m_pBuffers->pGeometry->Buffers;
	t3d_world_geometry_database_initialize_cold( *m_pBuffers->pGeometry, GeometryBuffers );
	m_pBuffers->nMaterialCount = 0u;
	m_pBuffers->nCellCount = 0u;
	m_pBuffers->nInstanceCount = 0u;
	m_pBuffers->nPrototypeCount = 0u;
	m_pBuffers->nMaterialEntryCount = 0u;
	m_pBuffers->nNativeVisualCount = 0u;
	t3d_world_ownership_registry_clear_cold( *m_pBuffers->pOwnership );
	for ( uint32_t i = 0u; i < m_pBuffers->nPrototypeCapacity; ++i ) {
		memset( &m_pBuffers->pPrototypeEntries[i], 0, sizeof( T3DWorldPrototypeEntry ) );
	}
	for ( uint32_t i = 0u; i < m_pBuffers->nMaterialEntryCapacity; ++i ) {
		memset( &m_pBuffers->pMaterialEntries[i], 0, sizeof( T3DWorldMaterialEntry ) );
	}

	if ( !visitNodeCold( pRoot, rStats ) ) {
		return false;
	}
	if ( !t3d_world_build_scene_cells_cold( m_pBuffers->pInstances,
	             m_pBuffers->pTransforms,
	             m_pBuffers->pInstanceSourceIds,
	             m_pBuffers->nInstanceCount,
	             m_pBuffers->pPrototypeBoundsByMeshId,
	             m_pBuffers->nPrototypeBoundsCapacity,
	             m_policy.fCellSize,
	             m_pBuffers->pCells,
	             m_pBuffers->nCellCapacity,
	             m_pBuffers->CellScratch,
	             *m_pBuffers->pOwnership,
	             m_pBuffers->nCellCount ) ) {
		return false;
	}
	return true;
}

bool Terrain3DWorldGodotConverter::visitNodeCold( Node *pNode, T3DWorldSceneBuildStats &rStats ) {
	if ( pNode == NULL ) {
		return true;
	}
	++rStats.nVisitedNodes;

	MeshInstance3D *pMesh = Object::cast_to<MeshInstance3D>( pNode );
	if ( pMesh != NULL ) {
		++rStats.nMeshCandidates;
		if ( !convertMeshInstanceCold( pMesh, rStats ) ) {
			++rStats.nFailureCount;
		}
	}

	const int32_t nChildren = pNode->get_child_count( false );
	for ( int32_t i = 0; i < nChildren; ++i ) {
		if ( !visitNodeCold( pNode->get_child( i, false ), rStats ) ) {
			return false;
		}
	}
	return true;
}

bool Terrain3DWorldGodotConverter::buildCandidateCold( MeshInstance3D *pMeshInstance,
        T3DWorldCandidateDesc &rCandidate,
        Ref<Terrain3DWorldImportProfile> &rProfile,
        Ref<Terrain3DWorldMaterial> &rExplicitMaterial ) const {
	if ( pMeshInstance == NULL ) {
		return false;
	}

	Terrain3DWorldInstance3D *pExplicit = Object::cast_to<Terrain3DWorldInstance3D>( pMeshInstance );
	memset( &rCandidate, 0, sizeof( rCandidate ) );
	rCandidate.nSourceId = pMeshInstance->get_instance_id();
	rCandidate.eKind = pExplicit != NULL ? T3D_WORLD_CANDIDATE_WORLD_INSTANCE : T3D_WORLD_CANDIDATE_MESH_INSTANCE;
	rCandidate.nTier = m_policy.nDefaultTier;
	rCandidate.bExplicitInclude = pExplicit != NULL || pMeshInstance->is_in_group( StringName( "terrain_3d_world" ) );
	rCandidate.bExplicitExclude = pMeshInstance->is_in_group( StringName( "terrain_3d_world_exclude" ) );
	rCandidate.bVisible = pMeshInstance->is_visible_in_tree();
	rCandidate.bStatic = true;
	rCandidate.bHasMesh = pMeshInstance->get_mesh().is_valid();
	rCandidate.bMaterialSupported = true;
	rCandidate.bSortedTransparency = false;
	rCandidate.bTwoSided = false;

	if ( pExplicit != NULL ) {
		rProfile = pExplicit->getImportProfile();
		rExplicitMaterial = pExplicit->getWorldMaterial();
		rCandidate.bExplicitExclude = rCandidate.bExplicitExclude || pExplicit->isForceNative();
		rCandidate.bStatic = !pExplicit->isDynamicInstance();
		const int32_t iTier = pExplicit->getTierOverride();
		if ( iTier == ( int32_t )T3D_WORLD_IMPORT_WORLD ) {
			rCandidate.nTier = T3D_WORLD_TIER_WORLD;
		} else if ( iTier == ( int32_t )T3D_WORLD_IMPORT_CLUTTER ) {
			rCandidate.nTier = T3D_WORLD_TIER_CLUTTER;
		} else if ( iTier == ( int32_t )T3D_WORLD_IMPORT_FOLIAGE ) {
			rCandidate.nTier = T3D_WORLD_TIER_FOLIAGE;
		}
	}

	if ( rExplicitMaterial.is_valid() ) {
		const int32_t material_tier = rExplicitMaterial->getRenderTier();
		if ( material_tier == ( int32_t )T3D_WORLD_IMPORT_WORLD ) {
			rCandidate.nTier = T3D_WORLD_TIER_WORLD;
		} else if ( material_tier == ( int32_t )T3D_WORLD_IMPORT_CLUTTER ) {
			rCandidate.nTier = T3D_WORLD_TIER_CLUTTER;
		} else if ( material_tier == ( int32_t )T3D_WORLD_IMPORT_FOLIAGE ) {
			rCandidate.nTier = T3D_WORLD_TIER_FOLIAGE;
		}
	}

	if ( rProfile.is_valid() ) {
		if ( !rProfile->isEnabled() ) {
			rCandidate.bExplicitExclude = true;
		}
		rCandidate.nTier = rProfile->resolveTier( rCandidate.nTier );
	}

	bool bSupported = true;
	bool bSorted = false;
	bool bTwoSided = false;
	if ( !analyzeMaterialsCold( pMeshInstance, rCandidate.nTier, rExplicitMaterial, bSupported, bSorted, bTwoSided ) ) {
		bSupported = false;
	}
	rCandidate.bMaterialSupported = bSupported;
	rCandidate.bSortedTransparency = bSorted;
	rCandidate.bTwoSided = bTwoSided;
	return true;
}

uint64_t Terrain3DWorldGodotConverter::computePrototypeKeyCold(
        MeshInstance3D *pMeshInstance, uint32_t pTier, const Ref<Terrain3DWorldMaterial> &pExplicitMaterial ) const {
	Ref<Mesh> Mesh = pMeshInstance->get_mesh();
	if ( Mesh.is_null() ) {
		return 0u;
	}
	uint64_t nHash = 1469598103934665603uLL;
	nHash = t3d_world_godot_hash_mix( nHash, ( uint64_t )Mesh->get_rid().get_id() );
	nHash = t3d_world_godot_hash_mix( nHash, pTier );
	if ( pExplicitMaterial.is_valid() ) {
		return t3d_world_godot_hash_mix( nHash, pExplicitMaterial->get_instance_id() );
	}

	Ref<ArrayMesh> ArrayMeshRef = Mesh;
	if ( ArrayMeshRef.is_null() ) {
		return 0u;
	}
	for ( int32_t s = 0; s < ArrayMeshRef->get_surface_count(); ++s ) {
		Ref<Material> Material = pMeshInstance->get_surface_override_material( s );
		if ( Material.is_null() ) {
			Material = ArrayMeshRef->surface_get_material( s );
		}
		const uint64_t nMaterialKey = Material.is_valid() ? ( uint64_t )Material->get_rid().get_id() : 0u;
		nHash = t3d_world_godot_hash_mix( nHash, nMaterialKey );
	}
	return nHash == 0u ? 1u : nHash;
}

bool Terrain3DWorldGodotConverter::flattenMeshCold( MeshInstance3D *pMeshInstance,
        uint32_t pTier,
        const Ref<Terrain3DWorldMaterial> &pExplicitMaterial,
        T3DWorldGeometrySourceMesh &rSource,
        T3DWorldGeometrySourceLod &rLod,
        uint64_t &rPrototypeKey,
        T3DWorldConversionReason &rReason ) {
	Ref<ArrayMesh> Mesh = pMeshInstance->get_mesh();
	if ( Mesh.is_null() ) {
		rReason = T3D_WORLD_CONVERSION_UNSUPPORTED_KIND;
		return false;
	}
	if ( Mesh->get_surface_count() > 256 ) {
		rReason = T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED;
		return false;
	}

	bool aSurfaceTwoSided[256];
	memset( aSurfaceTwoSided, 0, sizeof( aSurfaceTwoSided ) );
	uint32_t nVertexTotal = 0u;
	uint32_t nIndexTotal = 0u;
	uint32_t nTriangleTotal = 0u;
	for ( int32_t s = 0; s < Mesh->get_surface_count(); ++s ) {
		if ( Mesh->surface_get_primitive_type( s ) != Mesh::PRIMITIVE_TRIANGLES ) {
			rReason = T3D_WORLD_CONVERSION_UNSUPPORTED_PRIMITIVE;
			return false;
		}
		Array Arrays = Mesh->surface_get_arrays( s );
		PackedVector3Array Positions = Arrays[Mesh::ARRAY_VERTEX];
		PackedInt32Array Indices = Arrays[Mesh::ARRAY_INDEX];
		Ref<Material> Material = pMeshInstance->get_surface_override_material( s );
		if ( Material.is_null() ) {
			Material = Mesh->surface_get_material( s );
		}
		aSurfaceTwoSided[s] = t3d_world_godot_is_two_sided_material( Material, pExplicitMaterial );

		const uint32_t nSurfaceVertices = ( uint32_t )Positions.size();
		const uint32_t nSurfaceIndices = Indices.is_empty() ? nSurfaceVertices : ( uint32_t )Indices.size();
		if ( nSurfaceIndices == 0u || ( nSurfaceIndices % 3u ) != 0u ) {
			rReason = T3D_WORLD_CONVERSION_UNSUPPORTED_PRIMITIVE;
			return false;
		}
		const uint32_t nWindingCount = aSurfaceTwoSided[s] ? 2u : 1u;
		if ( nSurfaceIndices > UINT32_MAX / nWindingCount ) {
			rReason = T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED;
			return false;
		}
		const uint32_t nCookedIndices = nSurfaceIndices * nWindingCount;
		const uint32_t nCookedTriangles = nCookedIndices / 3u;
		if ( UINT32_MAX - nVertexTotal < nSurfaceVertices || UINT32_MAX - nIndexTotal < nCookedIndices
		        || UINT32_MAX - nTriangleTotal < nCookedTriangles ) {
			rReason = T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED;
			return false;
		}
		nVertexTotal += nSurfaceVertices;
		nIndexTotal += nCookedIndices;
		nTriangleTotal += nCookedTriangles;
	}
	if ( nVertexTotal > m_pBuffers->Scratch.nVertexCapacity || nIndexTotal > m_pBuffers->Scratch.nIndexCapacity
	        || nTriangleTotal > m_pBuffers->Scratch.nTriangleCapacity ) {
		rReason = T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED;
		return false;
	}

	uint32_t nVertexCursor = 0u;
	uint32_t nIndexCursor = 0u;
	uint32_t nTriangleCursor = 0u;
	uint32_t nMaterialBase = T3D_WORLD_INVALID_ID;
	uint32_t aSurfaceMaterialIds[256];
	for ( int32_t s = 0; s < Mesh->get_surface_count(); ++s ) {
		Ref<Material> Material = pMeshInstance->get_surface_override_material( s );
		if ( Material.is_null() ) {
			Material = Mesh->surface_get_material( s );
		}
		uint32_t nMaterialId = T3D_WORLD_INVALID_ID;
		if ( !findOrCompileMaterialCold( Material, pExplicitMaterial, pTier, nMaterialId, rReason ) ) {
			return false;
		}
		aSurfaceMaterialIds[s] = nMaterialId;
		if ( nMaterialBase == T3D_WORLD_INVALID_ID || nMaterialId < nMaterialBase ) {
			nMaterialBase = nMaterialId;
		}
	}

	for ( int32_t s = 0; s < Mesh->get_surface_count(); ++s ) {
		Array Arrays = Mesh->surface_get_arrays( s );
		PackedVector3Array Positions = Arrays[Mesh::ARRAY_VERTEX];
		PackedVector3Array Normals = Arrays[Mesh::ARRAY_NORMAL];
		PackedFloat32Array Tangents = Arrays[Mesh::ARRAY_TANGENT];
		PackedVector2Array Uvs = Arrays[Mesh::ARRAY_TEX_UV];
		PackedColorArray Colors = Arrays[Mesh::ARRAY_COLOR];
		PackedInt32Array Indices = Arrays[Mesh::ARRAY_INDEX];
		const uint32_t nSurfaceVertices = ( uint32_t )Positions.size();
		const uint32_t nSurfaceIndices = Indices.is_empty() ? nSurfaceVertices : ( uint32_t )Indices.size();
		const uint32_t nBaseVertex = nVertexCursor;

		for ( uint32_t v = 0u; v < nSurfaceVertices; ++v ) {
			t3dWorldVertex_t &Out = m_pBuffers->Scratch.pVertices[nVertexCursor++];
			const Vector3 P = Positions[( int64_t )v];
			const Vector3 N = v < ( uint32_t )Normals.size() ? Normals[( int64_t )v] : Vector3( 0.0f, 1.0f, 0.0f );
			const Vector2 UV = v < ( uint32_t )Uvs.size() ? Uvs[( int64_t )v] : Vector2( 0.0f, 0.0f );
			const Color C = v < ( uint32_t )Colors.size() ? Colors[( int64_t )v] : Color( 1.0f, 1.0f, 1.0f, 1.0f );
			Out.m_fPosition[0] = P.x;
			Out.m_fPosition[1] = P.y;
			Out.m_fPosition[2] = P.z;
			Out.m_nPackedNormal = t3d_world_godot_pack_normal( N );
			if ( ( uint64_t )v * 4u + 3u < ( uint64_t )Tangents.size() ) {
				const uint32_t t = v * 4u;
				Out.m_nPackedTangent = t3d_world_godot_pack_tangent( Tangents[t], Tangents[t + 1u], Tangents[t + 2u], Tangents[t + 3u] );
			} else {
				Out.m_nPackedTangent = t3d_world_godot_pack_tangent( 1.0f, 0.0f, 0.0f, 1.0f );
			}
			Out.m_nPackedUv0 = ( uint32_t )t3dHalfEncode( ( float )UV.x ) | ( ( uint32_t )t3dHalfEncode( ( float )UV.y ) << 16u );
			Out.m_nPackedUv1 = 0u;
			Out.m_nColorRgba8 = t3d_world_godot_pack_color( C );
		}

		const uint32_t nSurfaceTriangles = nSurfaceIndices / 3u;
		const uint32_t nWindingCount = aSurfaceTwoSided[s] ? 2u : 1u;
		for ( uint32_t t = 0u; t < nSurfaceTriangles; ++t ) {
			uint32_t v[3];
			for ( uint32_t lane = 0u; lane < 3u; ++lane ) {
				const uint32_t nSourceIndex = t * 3u + lane;
				const uint32_t nLocalIndex = Indices.is_empty() ? nSourceIndex : ( uint32_t )Indices[( int64_t )nSourceIndex];
				if ( nLocalIndex >= nSurfaceVertices ) {
					rReason = T3D_WORLD_CONVERSION_COOK_FAILED;
					return false;
				}
				v[lane] = nBaseVertex + nLocalIndex;
			}
			for ( uint32_t winding = 0u; winding < nWindingCount; ++winding ) {
				m_pBuffers->Scratch.pIndices[nIndexCursor++] = v[0];
				m_pBuffers->Scratch.pIndices[nIndexCursor++] = winding == 0u ? v[1] : v[2];
				m_pBuffers->Scratch.pIndices[nIndexCursor++] = winding == 0u ? v[2] : v[1];
				m_pBuffers->Scratch.pTriangleMaterialOffsets[nTriangleCursor] = aSurfaceMaterialIds[s] - nMaterialBase;
				m_pBuffers->Scratch.pTriangleTiers[nTriangleCursor] = ( uint8_t )pTier;
				++nTriangleCursor;
			}
		}
	}

	memset( &rLod, 0, sizeof( rLod ) );
	rLod.pIndices = m_pBuffers->Scratch.pIndices;
	rLod.pTriangleMaterialOffsets = m_pBuffers->Scratch.pTriangleMaterialOffsets;
	rLod.pTriangleTiers = m_pBuffers->Scratch.pTriangleTiers;
	rLod.nIndexCount = nIndexTotal;
	rLod.nDefaultTier = pTier;
	rLod.fMinDistance = 0.0f;
	rLod.fMaxDistance = 1000000.0f;
	rLod.fScreenError = 0.0f;

	memset( &rSource, 0, sizeof( rSource ) );
	rSource.pVertices = m_pBuffers->Scratch.pVertices;
	rSource.pLods = &rLod;
	rSource.nVertexCount = nVertexTotal;
	rSource.nLodCount = 1u;
	rSource.nMaterialBase = nMaterialBase == T3D_WORLD_INVALID_ID ? 0u : nMaterialBase;
	rPrototypeKey = computePrototypeKeyCold( pMeshInstance, pTier, pExplicitMaterial );
	return rPrototypeKey != 0u;
}

bool Terrain3DWorldGodotConverter::findOrCookPrototypeCold( MeshInstance3D *pMeshInstance,
        uint32_t pTier,
        const Ref<Terrain3DWorldMaterial> &pExplicitMaterial,
        uint32_t &rMeshId,
        T3DWorldConversionReason &rReason ) {
	const uint64_t nKey = computePrototypeKeyCold( pMeshInstance, pTier, pExplicitMaterial );
	for ( uint32_t i = 0u; i < m_pBuffers->nPrototypeCount; ++i ) {
		if ( m_pBuffers->pPrototypeEntries[i].nKey == nKey ) {
			rMeshId = m_pBuffers->pPrototypeEntries[i].nMeshId;
			return true;
		}
	}
	if ( m_pBuffers->nPrototypeCount >= m_pBuffers->nPrototypeCapacity ) {
		rReason = T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED;
		return false;
	}

	const uint32_t nMaterialCountBefore = m_pBuffers->nMaterialCount;
	const uint32_t nMaterialEntryCountBefore = m_pBuffers->nMaterialEntryCount;
	T3DWorldGeometrySourceMesh Source;
	T3DWorldGeometrySourceLod Lod;
	uint64_t nComputedKey = 0u;
	if ( !flattenMeshCold( pMeshInstance, pTier, pExplicitMaterial, Source, Lod, nComputedKey, rReason ) ) {
		m_pBuffers->nMaterialCount = nMaterialCountBefore;
		m_pBuffers->nMaterialEntryCount = nMaterialEntryCountBefore;
		return false;
	}
	T3DWorldGeometryCookedMeshSpan Span;
	T3DWorldGeometryCookStats Stats;
	const T3DWorldGeometryCookResult eCook = t3d_world_geometry_append_mesh_cold( *m_pBuffers->pGeometry, Source, Span, Stats );
	if ( eCook != T3D_WORLD_GEOMETRY_COOK_OK ) {
		m_pBuffers->nMaterialCount = nMaterialCountBefore;
		m_pBuffers->nMaterialEntryCount = nMaterialEntryCountBefore;
		rReason = eCook == T3D_WORLD_GEOMETRY_COOK_CAPACITY_EXCEEDED ? T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED : T3D_WORLD_CONVERSION_COOK_FAILED;
		return false;
	}

	rMeshId = Span.nMeshId;
	if ( rMeshId >= m_pBuffers->nPrototypeBoundsCapacity ) {
		rReason = T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED;
		return false;
	}
	T3DWorldPrototypeBounds &Bounds = m_pBuffers->pPrototypeBoundsByMeshId[rMeshId];
	if ( Span.nClusterCount == 0u ) {
		memset( &Bounds, 0, sizeof( Bounds ) );
	} else {
		float vMin[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
		float vMax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
		for ( uint32_t i = 0u; i < Span.nClusterCount; ++i ) {
			const T3DWorldClusterRecord &Cluster = m_pBuffers->pGeometry->Buffers.pClusterRecords[Span.nClusterOffset + i];
			for ( uint32_t axis = 0u; axis < 3u; ++axis ) {
				const float fLo = Cluster.vCenter[axis] - Cluster.fRadius;
				const float fHi = Cluster.vCenter[axis] + Cluster.fRadius;
				if ( fLo < vMin[axis] ) {
					vMin[axis] = fLo;
				}
				if ( fHi > vMax[axis] ) {
					vMax[axis] = fHi;
				}
			}
		}
		float fRadiusSq = 0.0f;
		for ( uint32_t axis = 0u; axis < 3u; ++axis ) {
			Bounds.vCenter[axis] = ( vMin[axis] + vMax[axis] ) * 0.5f;
			const float fHalf = ( vMax[axis] - vMin[axis] ) * 0.5f;
			fRadiusSq += fHalf * fHalf;
		}
		Bounds.fRadius = sqrtf( fRadiusSq );
	}
	T3DWorldPrototypeEntry &Entry = m_pBuffers->pPrototypeEntries[m_pBuffers->nPrototypeCount++];
	Entry.nKey = nComputedKey;
	Entry.nMeshId = rMeshId;
	Entry.nGeneration = m_pBuffers->pOwnership->nGeneration;
	return true;
}

bool Terrain3DWorldGodotConverter::appendInstanceCold(
        MeshInstance3D *pMeshInstance, uint32_t pMeshId, uint32_t pMaterialOverride, uint32_t &rInstanceId ) {
	if ( m_pBuffers->nInstanceCount >= m_pBuffers->nInstanceCapacity ) {
		return false;
	}
	if ( pMeshId >= m_pBuffers->pGeometry->nMeshCount ) {
		return false;
	}
	const Transform3D Transform = pMeshInstance->get_global_transform();
	const uint32_t nInstanceIndex = m_pBuffers->nInstanceCount;
	t3dWorldTransform_t &OutTransform = m_pBuffers->pTransforms[nInstanceIndex];
	OutTransform.m_fRow0[0] = Transform.basis[0].x;
	OutTransform.m_fRow0[1] = Transform.basis[0].y;
	OutTransform.m_fRow0[2] = Transform.basis[0].z;
	OutTransform.m_fRow0[3] = Transform.origin.x;
	OutTransform.m_fRow1[0] = Transform.basis[1].x;
	OutTransform.m_fRow1[1] = Transform.basis[1].y;
	OutTransform.m_fRow1[2] = Transform.basis[1].z;
	OutTransform.m_fRow1[3] = Transform.origin.y;
	OutTransform.m_fRow2[0] = Transform.basis[2].x;
	OutTransform.m_fRow2[1] = Transform.basis[2].y;
	OutTransform.m_fRow2[2] = Transform.basis[2].z;
	OutTransform.m_fRow2[3] = Transform.origin.z;
	const T3DWorldMeshRecord &Mesh = m_pBuffers->pGeometry->Buffers.pMeshRecords[pMeshId];
	t3dWorldInstance_t &Out = m_pBuffers->pInstances[nInstanceIndex];
	memset( &Out, 0, sizeof( Out ) );
	Out.m_nClusterOffset = Mesh.nClusterOffset;
	Out.m_nClusterCount = Mesh.nClusterCount;
	Out.m_nTransformIndex = nInstanceIndex;
	Out.m_nPreviousTransformIndex = nInstanceIndex;
	Out.m_nVisibilityLayers = UINT32_MAX;
	Out.m_nFlags = 0u;
	Out.m_nGeometryKind = T3D_WORLD_GEOMETRY_STATIC_CLUSTER;
	Out.m_nSourceId = pMeshId;
	Out.m_nMaterialOverride = pMaterialOverride;
	Out.m_nCellId = T3D_WORLD_INVALID_ID;
	rInstanceId = nInstanceIndex;
	m_pBuffers->pInstanceSourceIds[rInstanceId] = pMeshInstance->get_instance_id();
	++m_pBuffers->nInstanceCount;
	return true;
}

void Terrain3DWorldGodotConverter::publishOwnershipCold( MeshInstance3D *pNode,
        T3DWorldOwnership pOwnership,
        T3DWorldConversionReason pReason,
        uint32_t pPrototypeId,
        uint32_t pInstanceId ) {
	if ( pNode == NULL ) {
		return;
	}
	T3DWorldOwnershipEntry Entry;
	memset( &Entry, 0, sizeof( Entry ) );
	Entry.nSourceId = pNode->get_instance_id();
	Entry.nPrototypeId = pPrototypeId;
	Entry.nInstanceId = pInstanceId;
	Entry.eOwnership = pOwnership;
	Entry.eReason = pReason;
	t3d_world_ownership_registry_publish_cold( *m_pBuffers->pOwnership, Entry );

	if ( m_pBuffers->nNativeVisualCount < m_pBuffers->nNativeVisualCapacity ) {
		T3DWorldNativeVisualEntry &Visual = m_pBuffers->pNativeVisuals[m_pBuffers->nNativeVisualCount++];
		Visual.nSourceId = Entry.nSourceId;
		Visual.eOwnership = pOwnership;
	}
}

bool Terrain3DWorldGodotConverter::convertMeshInstanceCold( MeshInstance3D *pMeshInstance, T3DWorldSceneBuildStats &rStats ) {
	T3DWorldCandidateDesc Candidate;
	Ref<Terrain3DWorldImportProfile> Profile;
	Ref<Terrain3DWorldMaterial> ExplicitMaterial;
	if ( !buildCandidateCold( pMeshInstance, Candidate, Profile, ExplicitMaterial ) ) {
		return false;
	}
	T3DWorldConversionPolicy EffectivePolicy = m_policy;
	if ( Profile.is_valid() ) {
		EffectivePolicy.eMaterialPolicy = ( T3DWorldMaterialPolicy )Profile->getMaterialPolicy();
		EffectivePolicy.eTransparencyPolicy = ( T3DWorldTransparencyPolicy )Profile->getTransparencyPolicy();
	}
	if ( EffectivePolicy.eMaterialPolicy == T3D_WORLD_MATERIAL_REQUIRE_WORLD && ExplicitMaterial.is_null() ) {
		Candidate.bMaterialSupported = false;
	}
	const T3DWorldClassification Classification = t3d_world_authoring_classify_reference_debug( EffectivePolicy, Candidate );
	if ( Classification.eOwnership != T3D_WORLD_OWNERSHIP_WORLD ) {
		publishOwnershipCold( pMeshInstance,
		        Classification.eOwnership,
		        Classification.eReason,
		        T3D_WORLD_AUTHORING_INVALID_SLOT,
		        T3D_WORLD_AUTHORING_INVALID_SLOT );
		if ( Classification.eOwnership == T3D_WORLD_OWNERSHIP_COMPATIBILITY ) {
			++rStats.nCompatibilityInstances;
		} else {
			++rStats.nNativeInstances;
		}
		return true;
	}

	uint32_t nMeshId = T3D_WORLD_AUTHORING_INVALID_SLOT;
	T3DWorldConversionReason eReason = T3D_WORLD_CONVERSION_OK;
	if ( !findOrCookPrototypeCold( pMeshInstance, Classification.nTier, ExplicitMaterial, nMeshId, eReason ) ) {
		publishOwnershipCold(
		        pMeshInstance, T3D_WORLD_OWNERSHIP_COMPATIBILITY, eReason, T3D_WORLD_AUTHORING_INVALID_SLOT, T3D_WORLD_AUTHORING_INVALID_SLOT );
		++rStats.nCompatibilityInstances;
		return eReason == T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED ? false : true;
	}

	uint32_t nMaterialOverride = T3D_WORLD_INVALID_ID;
	if ( ExplicitMaterial.is_valid() ) {
		if ( !findOrCompileMaterialCold( Ref<Material>(), ExplicitMaterial, Classification.nTier, nMaterialOverride, eReason ) ) {
			publishOwnershipCold( pMeshInstance, T3D_WORLD_OWNERSHIP_COMPATIBILITY, eReason, nMeshId, T3D_WORLD_AUTHORING_INVALID_SLOT );
			++rStats.nCompatibilityInstances;
			return true;
		}
	}

	uint32_t nInstanceId = T3D_WORLD_AUTHORING_INVALID_SLOT;
	if ( !appendInstanceCold( pMeshInstance, nMeshId, nMaterialOverride, nInstanceId ) ) {
		publishOwnershipCold( pMeshInstance,
		        T3D_WORLD_OWNERSHIP_COMPATIBILITY,
		        T3D_WORLD_CONVERSION_CAPACITY_EXCEEDED,
		        nMeshId,
		        T3D_WORLD_AUTHORING_INVALID_SLOT );
		++rStats.nCompatibilityInstances;
		return false;
	}
	publishOwnershipCold( pMeshInstance, T3D_WORLD_OWNERSHIP_WORLD, T3D_WORLD_CONVERSION_OK, nMeshId, nInstanceId );
	++rStats.nConvertedInstances;
	rStats.nUniquePrototypes = m_pBuffers->nPrototypeCount;
	rStats.nUniqueMaterials = m_pBuffers->nMaterialCount;
	return true;
}

void Terrain3DWorldGodotConverter::releaseNativeOwnershipCold() {
	if ( !m_bInitialized || !m_bNativeOwnershipApplied || m_pBuffers == NULL ) {
		return;
	}
	RenderingServer *pRS = RenderingServer::get_singleton();
	if ( pRS != NULL ) {
		for ( uint32_t i = 0u; i < m_pBuffers->nNativeVisualCount; ++i ) {
			T3DWorldNativeVisualEntry &Visual = m_pBuffers->pNativeVisuals[i];
			Object *const pObject = ObjectDB::get_instance( Visual.nSourceId );
			MeshInstance3D *const pNode = Object::cast_to<MeshInstance3D>( pObject );
			if ( pNode == NULL ) {
				continue;
			}
			const RID hNativeInstance = pNode->get_instance();
			if ( hNativeInstance.is_valid() ) {
				pRS->instance_set_visible( hNativeInstance, pNode->is_visible_in_tree() );
			}
		}
	}
	m_bNativeOwnershipApplied = false;
}

bool Terrain3DWorldGodotConverter::applyNativeOwnershipCold( bool pWorldReady ) {
	if ( !m_bInitialized || m_pBuffers == NULL ) {
		return false;
	}
	RenderingServer *pRS = RenderingServer::get_singleton();
	if ( pRS == NULL ) {
		return false;
	}

	for ( uint32_t i = 0u; i < m_pBuffers->nNativeVisualCount; ++i ) {
		T3DWorldNativeVisualEntry &Visual = m_pBuffers->pNativeVisuals[i];
		Object *const pObject = ObjectDB::get_instance( Visual.nSourceId );
		MeshInstance3D *const pNode = Object::cast_to<MeshInstance3D>( pObject );
		if ( pNode == NULL ) {
			continue;
		}
		const RID hNativeInstance = pNode->get_instance();
		if ( !hNativeInstance.is_valid() ) {
			continue;
		}
		const bool bWorldOwns = pWorldReady && Visual.eOwnership == T3D_WORLD_OWNERSHIP_WORLD;
		pRS->instance_set_visible( hNativeInstance, bWorldOwns ? false : pNode->is_visible_in_tree() );
	}
	m_bNativeOwnershipApplied = pWorldReady;
	return true;
}

bool Terrain3DWorldGodotConverter::isInitialized() const {
	return m_bInitialized;
}
const T3DWorldConversionPolicy &Terrain3DWorldGodotConverter::getPolicy() const {
	return m_policy;
}
const T3DWorldSceneBuildBuffers *Terrain3DWorldGodotConverter::getBuildBuffersCold() const {
	return m_pBuffers;
}
