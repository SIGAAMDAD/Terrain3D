// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "hlod/terrain_3d_hlod_builder.h"

#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_mesh_asset.h"
#include "data/terrain_3d_region.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instance_store.h"
#include "terrain_3d.h"

#include <EASTL/algorithm.h>
#include <EASTL/sort.h>
#include <EASTL/vector.h>
#include <math.h>
#include <stdint.h>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <EASTL/utility.h>

typedef struct t3dHlodTriangle_s {
	Vector3 m_a;
	Vector3 m_b;
	Vector3 m_c;
	Vector3 m_center;
	AABB m_bounds;
} t3dHlodTriangle_t;

typedef struct t3dHlodBvhNode_s {
	AABB m_bounds;
	int m_nLeft = -1;
	int m_nRight = -1;
	int m_nBegin = 0;
	int m_nCount = 0;
} t3dHlodBvhNode_t;

EA_FORCE_INLINE static AABB t3dHlodBuilderBoundsFromPoint( const Vector3 &p ) {
	return AABB( p, Vector3() );
}

static real_t t3dHlodBuilderPointAabbDistanceSquared( const Vector3 &p, const AABB &b ) {
	const Vector3 end = b.get_end();
	real_t dx = 0.0f;
	real_t dy = 0.0f;
	real_t dz = 0.0f;
	if ( p.x < b.position.x ) {
		dx = b.position.x - p.x;
	} else if ( p.x > end.x ) {
		dx = p.x - end.x;
	}
	if ( p.y < b.position.y ) {
		dy = b.position.y - p.y;
	} else if ( p.y > end.y ) {
		dy = p.y - end.y;
	}
	if ( p.z < b.position.z ) {
		dz = b.position.z - p.z;
	} else if ( p.z > end.z ) {
		dz = p.z - end.z;
	}
	return dx * dx + dy * dy + dz * dz;
}

// Squared distance from a point to a triangle. This is the region-test form
// from the standard closest-point construction and is branchy only in the
// editor-only bake path.
static real_t t3dHlodBuilderPointTriangleDistanceSquared( const Vector3 &p, const Vector3 &a, const Vector3 &b, const Vector3 &c ) {
	const Vector3 ab = b - a;
	const Vector3 ac = c - a;
	const Vector3 ap = p - a;
	const real_t d1 = ab.dot( ap );
	const real_t d2 = ac.dot( ap );
	if ( d1 <= 0.0f && d2 <= 0.0f ) {
		return p.distance_squared_to( a );
	}

	const Vector3 bp = p - b;
	const real_t d3 = ab.dot( bp );
	const real_t d4 = ac.dot( bp );
	if ( d3 >= 0.0f && d4 <= d3 ) {
		return p.distance_squared_to( b );
	}

	const real_t vc = d1 * d4 - d3 * d2;
	if ( vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f ) {
		const real_t v = d1 / MAX( d1 - d3, ( real_t )1e-20 );
		return p.distance_squared_to( a + ab * v );
	}

	const Vector3 cp = p - c;
	const real_t d5 = ab.dot( cp );
	const real_t d6 = ac.dot( cp );
	if ( d6 >= 0.0f && d5 <= d6 ) {
		return p.distance_squared_to( c );
	}

	const real_t vb = d5 * d2 - d1 * d6;
	if ( vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f ) {
		const real_t w = d2 / MAX( d2 - d6, ( real_t )1e-20 );
		return p.distance_squared_to( a + ac * w );
	}

	const real_t va = d3 * d6 - d5 * d4;
	if ( va <= 0.0f && ( d4 - d3 ) >= 0.0f && ( d5 - d6 ) >= 0.0f ) {
		const real_t denominator = ( d4 - d3 ) + ( d5 - d6 );
		const real_t w = denominator > ( real_t )1e-20 ? ( d4 - d3 ) / denominator : 0.0f;
		return p.distance_squared_to( b + ( c - b ) * w );
	}

	const real_t denominator = va + vb + vc;
	if ( Math::abs( denominator ) <= ( real_t )1e-20 ) {
		return MIN( p.distance_squared_to( a ), MIN( p.distance_squared_to( b ), p.distance_squared_to( c ) ) );
	}
	const real_t inv = 1.0f / denominator;
	const real_t v = vb * inv;
	const real_t w = vc * inv;
	return p.distance_squared_to( a + ab * v + ac * w );
}

static int t3dHlodBuilderLargestAxis( const Vector3 &size ) {
	if ( size.x >= size.y && size.x >= size.z ) {
		return 0;
	}
	return size.y >= size.z ? 1 : 2;
}

EA_FORCE_INLINE static real_t t3dHlodBuilderComponent( const Vector3 &v, const int axis ) {
	return axis == 0 ? v.x : ( axis == 1 ? v.y : v.z );
}

typedef struct t3dHlodTriangleAxisLess_s {
	const eastl::vector<t3dHlodTriangle_t> *m_pTriangles;
	int m_nAxis;

	bool operator()( const int nLhs, const int nRhs ) const {
		return t3dHlodBuilderComponent( ( *m_pTriangles )[( size_t )nLhs].m_center, m_nAxis )
		        < t3dHlodBuilderComponent( ( *m_pTriangles )[( size_t )nRhs].m_center, m_nAxis );
	}
} t3dHlodTriangleAxisLess_t;

static int t3dHlodBuilderBuildBvhRecursive( const eastl::vector<t3dHlodTriangle_t> &triangles,
        eastl::vector<int> &order,
        eastl::vector<t3dHlodBvhNode_t> &nodes,
        const int begin,
        const int count ) {
	t3dHlodBvhNode_t node;
	node.m_nBegin = begin;
	node.m_nCount = count;
	AABB bounds;
	AABB center_bounds;
	bool first = true;
	for ( int i = 0; i < count; ++i ) {
		const t3dHlodTriangle_t &triangle = triangles[( size_t )order[( size_t )( begin + i )]];
		if ( first ) {
			bounds = triangle.m_bounds;
			center_bounds = t3dHlodBuilderBoundsFromPoint( triangle.m_center );
			first = false;
		} else {
			bounds = bounds.merge( triangle.m_bounds );
			center_bounds = center_bounds.expand( triangle.m_center );
		}
	}
	node.m_bounds = bounds;
	const int node_index = ( int )nodes.size();
	nodes.push_back( node );
	if ( count <= 8 ) {
		return node_index;
	}

	const int axis = t3dHlodBuilderLargestAxis( center_bounds.size );
	t3dHlodTriangleAxisLess_t less;
	less.m_pTriangles = &triangles;
	less.m_nAxis = axis;
	eastl::sort( order.begin() + begin, order.begin() + begin + count, less );
	const int left_count = count / 2;
	const int right_count = count - left_count;
	nodes[( size_t )node_index].m_nLeft = t3dHlodBuilderBuildBvhRecursive( triangles, order, nodes, begin, left_count );
	nodes[( size_t )node_index].m_nRight = t3dHlodBuilderBuildBvhRecursive( triangles, order, nodes, begin + left_count, right_count );
	nodes[( size_t )node_index].m_nCount = 0;
	return node_index;
}

static void t3dHlodBuilderQueryBvhRecursive( const eastl::vector<t3dHlodTriangle_t> &triangles,
        const eastl::vector<int> &order,
        const eastl::vector<t3dHlodBvhNode_t> &nodes,
        const int node_index,
        const Vector3 &point,
        real_t &best_squared ) {
	if ( node_index < 0 ) {
		return;
	}
	const t3dHlodBvhNode_t &node = nodes[( size_t )node_index];
	if ( t3dHlodBuilderPointAabbDistanceSquared( point, node.m_bounds ) >= best_squared ) {
		return;
	}
	if ( node.m_nCount > 0 ) {
		for ( int i = 0; i < node.m_nCount; ++i ) {
			const t3dHlodTriangle_t &triangle = triangles[( size_t )order[( size_t )( node.m_nBegin + i )]];
			if ( t3dHlodBuilderPointAabbDistanceSquared( point, triangle.m_bounds ) >= best_squared ) {
				continue;
			}
			best_squared =
			        MIN( best_squared, t3dHlodBuilderPointTriangleDistanceSquared( point, triangle.m_a, triangle.m_b, triangle.m_c ) );
		}
		return;
	}

	const t3dHlodBvhNode_t &left = nodes[( size_t )node.m_nLeft];
	const t3dHlodBvhNode_t &right = nodes[( size_t )node.m_nRight];
	const real_t left_distance = t3dHlodBuilderPointAabbDistanceSquared( point, left.m_bounds );
	const real_t right_distance = t3dHlodBuilderPointAabbDistanceSquared( point, right.m_bounds );
	if ( left_distance <= right_distance ) {
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nLeft, point, best_squared );
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nRight, point, best_squared );
	} else {
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nRight, point, best_squared );
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nLeft, point, best_squared );
	}
}

static bool t3dHlodBuilderHasUnsupportedStaticAttributes( const Array &arrays ) {
	for ( int channel = Mesh::ARRAY_CUSTOM0; channel <= Mesh::ARRAY_CUSTOM3; ++channel ) {
		if ( channel < arrays.size() && arrays[channel].get_type() != Variant::NIL ) {
			const PackedByteArray data = arrays[channel];
			if ( !data.is_empty() ) {
				return true;
			}
		}
	}
	if ( Mesh::ARRAY_BONES < arrays.size() && arrays[Mesh::ARRAY_BONES].get_type() != Variant::NIL ) {
		const PackedInt32Array bones = arrays[Mesh::ARRAY_BONES];
		if ( !bones.is_empty() ) {
			return true;
		}
	}
	if ( Mesh::ARRAY_WEIGHTS < arrays.size() && arrays[Mesh::ARRAY_WEIGHTS].get_type() != Variant::NIL ) {
		const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
		if ( !weights.is_empty() ) {
			return true;
		}
	}
	return false;
}

static Array t3dHlodBuilderCompactSurfaceArrays( const Array &arrays, const PackedInt32Array &indices ) {
	Array compact;
	compact.resize( Mesh::ARRAY_MAX );
	if ( arrays.size() <= Mesh::ARRAY_VERTEX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY ) {
		return Array();
	}
	const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
	if ( vertices.is_empty() || indices.is_empty() ) {
		return Array();
	}

	eastl::vector<int32_t> remap( ( size_t )vertices.size(), -1 );
	eastl::vector<int32_t> used;
	used.reserve( ( size_t )vertices.size() );
	PackedInt32Array compact_indices;
	compact_indices.resize( indices.size() );
	for ( int i = 0; i < indices.size(); ++i ) {
		const int old_index = indices[i];
		if ( old_index < 0 || old_index >= vertices.size() ) {
			return Array();
		}
		int32_t mapped = remap[( size_t )old_index];
		if ( mapped < 0 ) {
			mapped = ( int32_t )used.size();
			remap[( size_t )old_index] = mapped;
			used.push_back( old_index );
		}
		compact_indices.set( i, mapped );
	}

	PackedVector3Array compact_vertices;
	compact_vertices.resize( ( int )used.size() );
	for ( int i = 0; i < ( int )used.size(); ++i ) {
		compact_vertices.set( i, vertices[used[( size_t )i]] );
	}
	compact[Mesh::ARRAY_VERTEX] = compact_vertices;

	if ( arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY ) {
		const PackedVector3Array source = arrays[Mesh::ARRAY_NORMAL];
		if ( source.size() == vertices.size() ) {
			PackedVector3Array destination;
			destination.resize( ( int )used.size() );
			for ( int i = 0; i < ( int )used.size(); ++i ) {
				destination.set( i, source[used[( size_t )i]] );
			}
			compact[Mesh::ARRAY_NORMAL] = destination;
		}
	}
	if ( arrays[Mesh::ARRAY_TANGENT].get_type() == Variant::PACKED_FLOAT32_ARRAY ) {
		const PackedFloat32Array source = arrays[Mesh::ARRAY_TANGENT];
		if ( source.size() == vertices.size() * 4 ) {
			PackedFloat32Array destination;
			destination.resize( ( int )used.size() * 4 );
			for ( int i = 0; i < ( int )used.size(); ++i ) {
				const int old = used[( size_t )i] * 4;
				const int next = i * 4;
				for ( int component_index = 0; component_index < 4; ++component_index ) {
					destination.set( next + component_index, source[old + component_index] );
				}
			}
			compact[Mesh::ARRAY_TANGENT] = destination;
		}
	}
	if ( arrays[Mesh::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY ) {
		const PackedColorArray source = arrays[Mesh::ARRAY_COLOR];
		if ( source.size() == vertices.size() ) {
			PackedColorArray destination;
			destination.resize( ( int )used.size() );
			for ( int i = 0; i < ( int )used.size(); ++i ) {
				destination.set( i, source[used[( size_t )i]] );
			}
			compact[Mesh::ARRAY_COLOR] = destination;
		}
	}
	const int szUvChannels[] = { Mesh::ARRAY_TEX_UV, Mesh::ARRAY_TEX_UV2 };
	for ( size_t iUvChannel = 0u; iUvChannel < T3D_ARRAY_COUNT( szUvChannels ); iUvChannel++ ) {
		const int channel = szUvChannels[iUvChannel];
		if ( arrays[channel].get_type() != Variant::PACKED_VECTOR2_ARRAY ) {
			continue;
		}
		const PackedVector2Array source = arrays[channel];
		if ( source.size() != vertices.size() ) {
			continue;
		}
		PackedVector2Array destination;
		destination.resize( ( int )used.size() );
		for ( int i = 0; i < ( int )used.size(); ++i ) {
			destination.set( i, source[used[( size_t )i]] );
		}
		compact[channel] = destination;
	}
	compact[Mesh::ARRAY_INDEX] = compact_indices;
	return compact;
}

static int64_t t3dHlodBuilderTriangleCount( const Array &arrays ) {
	if ( arrays.size() <= Mesh::ARRAY_VERTEX ) {
		return 0;
	}
	const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	if ( !indices.is_empty() ) {
		return indices.size() / 3;
	}
	const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
	return vertices.size() / 3;
}

static real_t t3dHlodBuilderMeasureVertexDeviation( const Array &source_arrays, const Array &proxy_arrays ) {
	if ( source_arrays.size() <= Mesh::ARRAY_VERTEX || proxy_arrays.size() <= Mesh::ARRAY_INDEX ) {
		return 0.0f;
	}
	const PackedVector3Array source_vertices = source_arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array proxy_vertices = proxy_arrays[Mesh::ARRAY_VERTEX];
	const PackedInt32Array proxy_indices = proxy_arrays[Mesh::ARRAY_INDEX];
	if ( source_vertices.is_empty() || proxy_vertices.is_empty() || proxy_indices.size() < 3 ) {
		return 0.0f;
	}

	eastl::vector<t3dHlodTriangle_t> triangles;
	triangles.reserve( ( size_t )proxy_indices.size() / 3u );
	for ( int i = 0; i + 2 < proxy_indices.size(); i += 3 ) {
		const int ia = proxy_indices[i];
		const int ib = proxy_indices[i + 1];
		const int ic = proxy_indices[i + 2];
		if ( ia < 0 || ib < 0 || ic < 0 || ia >= proxy_vertices.size() || ib >= proxy_vertices.size() || ic >= proxy_vertices.size() ) {
			continue;
		}
		t3dHlodTriangle_t triangle;
		triangle.m_a = proxy_vertices[ia];
		triangle.m_b = proxy_vertices[ib];
		triangle.m_c = proxy_vertices[ic];
		triangle.m_center = ( triangle.m_a + triangle.m_b + triangle.m_c ) / 3.0f;
		triangle.m_bounds = t3dHlodBuilderBoundsFromPoint( triangle.m_a ).expand( triangle.m_b ).expand( triangle.m_c );
		triangles.push_back( triangle );
	}
	if ( triangles.empty() ) {
		return 0.0f;
	}

	eastl::vector<int> order;
	order.resize( triangles.size() );
	for ( int i = 0; i < ( int )triangles.size(); ++i ) {
		order[( size_t )i] = i;
	}
	eastl::vector<t3dHlodBvhNode_t> nodes;
	nodes.reserve( triangles.size() * 2u );
	const int root = t3dHlodBuilderBuildBvhRecursive( triangles, order, nodes, 0, ( int )triangles.size() );
	real_t max_squared = 0.0f;
	for ( int i = 0; i < source_vertices.size(); ++i ) {
		real_t best_squared = INFINITY;
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, root, source_vertices[i], best_squared );
		if ( isfinite( ( double )best_squared ) ) {
			max_squared = MAX( max_squared, best_squared );
		}
	}
	return Math::sqrt( max_squared );
}

PackedInt32Array Terrain3DHLODBuilder::getInstancerMeshIds( Terrain3DRegion *const pRegion ) const {
	PackedInt32Array result;
	if ( !pRegion ) {
		return result;
	}
	const Terrain3DInstanceStore &store = pRegion->getInstanceStore();
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( store.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( store.meshes() )[iMesh];
		bool has_instances = false;
		for ( size_t iCell = 0u; iCell < ( size_t )( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( mesh.m_cells )[iCell];
			if ( !cell.m_transforms.empty() ) {
				has_instances = true;
				break;
			}
		}
		if ( has_instances ) {
			result.push_back( mesh.m_nId );
		}
	}
	return result;
}

Dictionary Terrain3DHLODBuilder::buildInstancerSource(
        Terrain3D *const pTerrain, Terrain3DRegion *const pRegion, const int nMeshId ) const {
	Dictionary result;
	result["ok"] = false;
	if ( !pTerrain || !pRegion || pTerrain->getAssets().is_null() || nMeshId < 0 || nMeshId >= s_nMaxMeshAssets ) {
		result["error"] = "invalid Terrain3D instancer source";
		return result;
	}
	const Ref<Terrain3DMeshAsset> asset = pTerrain->getAssets()->getMeshAsset( nMeshId );
	if ( asset.is_null() || !asset->isEnabled() || !asset->isHlodEligible() ) {
		result["error"] = "mesh asset is not HLOD eligible";
		return result;
	}
	if ( asset->getMaterialOverlay().is_valid() ) {
		result["error"] = "material_overlay is unsupported for HLOD proxy baking";
		return result;
	}
	const Ref<Mesh> mesh = asset->getMesh( 0 );
	bool has_blend_shapes = false;
	if ( mesh.is_valid() ) {
		for ( int surface = 0; surface < mesh->get_surface_count(); ++surface ) {
			if ( !mesh->surface_get_blend_shape_arrays( surface ).is_empty() ) {
				has_blend_shapes = true;
				break;
			}
		}
	}
	if ( mesh.is_null() || mesh->get_surface_count() <= 0 || has_blend_shapes ) {
		result["error"] = "mesh asset has no supported static LOD0 geometry";
		return result;
	}
	const Terrain3DInstanceStore &store = pRegion->getInstanceStore();
	const t3dInstanceMesh_t *const source_instances = store.findMesh( nMeshId );
	if ( !source_instances ) {
		result["error"] = "region contains no instances for the mesh asset";
		return result;
	}

	Ref<ArrayMesh> merged;
	merged.instantiate();
	int64_t instance_count = 0;
	for ( size_t iCell = 0u; iCell < ( size_t )( ( source_instances->m_cells ).size() ); iCell++ ) {
		const t3dInstanceCell_t &cell = ( source_instances->m_cells )[iCell];
		instance_count += ( int64_t )cell.m_transforms.size();
		if ( cell.usesColors() ) {
			for ( size_t iColor = 0u; iColor < ( size_t )( ( cell.m_colors ).size() ); iColor++ ) {
				const uint32_t color = ( cell.m_colors )[iColor];
				if ( color != s_nInstanceWhiteRgba8 ) {
					result["error"] = "per-instance color tinting requires a dedicated HLOD material bake and is "
					                  "excluded from geometry-only proxies";
					return result;
				}
			}
		}
	}
	if ( instance_count <= 0 ) {
		result["error"] = "region contains no instances for the mesh asset";
		return result;
	}

	int64_t source_triangles = 0;
	for ( int surface = 0; surface < mesh->get_surface_count(); ++surface ) {
		if ( ( Mesh::PrimitiveType )mesh->_surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		const Array arrays = mesh->surface_get_arrays( surface );
		if ( t3dHlodBuilderHasUnsupportedStaticAttributes( arrays ) ) {
			result["error"] = "mesh uses custom/skinned vertex attributes unsupported by HLOD proxies";
			return result;
		}
		Ref<SurfaceTool> tool;
		tool.instantiate();
		tool->begin( Mesh::PRIMITIVE_TRIANGLES );
		for ( size_t iCell = 0u; iCell < ( size_t )( ( source_instances->m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_t &cell = ( source_instances->m_cells )[iCell];
			for ( size_t transform_index = 0; transform_index < cell.m_transforms.size(); ++transform_index ) {
				tool->append_from( mesh, surface, cell.getTransform( transform_index ) );
			}
		}
		tool->index();
		const Ref<Material> material =
		        asset->getMaterialOverride().is_valid() ? asset->getMaterialOverride() : mesh->surface_get_material( surface );
		tool->set_material( material );
		merged = tool->commit( merged );
		source_triangles += t3dHlodBuilderTriangleCount( arrays ) * instance_count;
	}
	if ( merged.is_null() || merged->get_surface_count() <= 0 ) {
		result["error"] = "instancer source produced no triangle geometry";
		return result;
	}
	result["ok"] = true;
	result["mesh"] = merged;
	result["bounds"] = merged->get_aabb();
	result["source_triangles"] = source_triangles;
	result["instance_count"] = instance_count;
	return result;
}

Dictionary Terrain3DHLODBuilder::simplifyMesh( const Ref<ArrayMesh> &rMesh, const real_t fRatio ) const {
	Dictionary result;
	result["ok"] = false;
	if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) {
		result["error"] = "HLOD simplify requires a non-empty ArrayMesh";
		return result;
	}
	const real_t ratio = CLAMP( fRatio, 0.05f, 1.0f );
	Ref<ArrayMesh> output;
	output.instantiate();
	real_t max_error = 0.0f;
	int64_t source_triangles = 0;
	int64_t proxy_triangles = 0;

	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		if ( rMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		Ref<SurfaceTool> source_tool;
		source_tool.instantiate();
		source_tool->create_from( rMesh, surface );
		source_tool->index();
		const Array source_arrays = source_tool->commit_to_arrays();
		if ( t3dHlodBuilderHasUnsupportedStaticAttributes( source_arrays ) ) {
			result["error"] = "HLOD proxy simplification encountered custom/skinned vertex attributes";
			return result;
		}
		const PackedInt32Array source_indices = source_arrays[Mesh::ARRAY_INDEX];
		if ( source_indices.size() < 3 ) {
			continue;
		}
		source_triangles += source_indices.size() / 3;

		PackedInt32Array simplified_indices = source_indices;
		if ( source_indices.size() >= 12 && ratio < 0.999f ) {
			const int target_indices = MAX( 3, ( int )Math::floor( ( real_t )source_indices.size() * ratio / 3.0f ) * 3 );
			const PackedInt32Array generated = source_tool->generate_lod( 1.0f, target_indices );
			if ( generated.size() >= 3 && generated.size() <= source_indices.size() ) {
				simplified_indices = generated;
			}
		}
		const Array compact_arrays = t3dHlodBuilderCompactSurfaceArrays( source_arrays, simplified_indices );
		if ( compact_arrays.is_empty() ) {
			result["error"] = "HLOD proxy vertex compaction failed";
			return result;
		}
		max_error = MAX( max_error, t3dHlodBuilderMeasureVertexDeviation( source_arrays, compact_arrays ) );
		proxy_triangles += simplified_indices.size() / 3;
		output->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, compact_arrays );
		output->surface_set_material( output->get_surface_count() - 1, rMesh->surface_get_material( surface ) );
	}
	if ( output->get_surface_count() <= 0 ) {
		result["error"] = "HLOD simplifier produced no triangle surfaces";
		return result;
	}
	result["ok"] = true;
	result["mesh"] = output;
	result["geometric_error"] = max_error;
	result["source_triangles"] = source_triangles;
	result["proxy_triangles"] = proxy_triangles;
	return result;
}

void Terrain3DHLODBuilder::_bind_methods( void ) {
	ClassDB::bind_method( D_METHOD( "get_instancer_mesh_ids", "region" ), &Terrain3DHLODBuilder::getInstancerMeshIds );
	ClassDB::bind_method(
	        D_METHOD( "build_instancer_source", "terrain", "region", "mesh_id" ), &Terrain3DHLODBuilder::buildInstancerSource );
	ClassDB::bind_method( D_METHOD( "simplify_mesh", "mesh", "ratio" ), &Terrain3DHLODBuilder::simplifyMesh );
}
