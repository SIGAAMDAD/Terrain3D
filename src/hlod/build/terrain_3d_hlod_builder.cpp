// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"
#include "hlod/build/terrain_3d_hlod_builder.h"
#include "hlod/format/terrain_3d_hlod_instance_codec.h"
#include "authoring/meshes/terrain_3d_meshopt_cook.h"
#include "core/utility/terrain_3d_hash_bytes.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_mesh_asset.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"
#include "instances/storage/terrain_3d_instance_store.h"
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

// t3dHLODTriangle_s centralized in Terrain3DTypes.h.

// t3dHLODBvhNode_s centralized in Terrain3DTypes.h.

EA_FORCE_INLINE static AABB t3dHlodBuilderBoundsFromPoint(
	const Vector3 &p ) {
	return AABB( p, Vector3() );
}

static real_t t3dHlodBuilderPointAabbDistanceSquared(
	const Vector3 &p,
	const AABB	  &b ) {
	const Vector3 end = b.get_end();
	real_t		  dx = 0.0f;
	real_t		  dy = 0.0f;
	real_t		  dz = 0.0f;
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
static real_t t3dHlodBuilderPointTriangleDistanceSquared(
	const Vector3 &p,
	const Vector3 &a,
	const Vector3 &b,
	const Vector3 &c ) {
	const Vector3 ab = b - a;
	const Vector3 ac = c - a;
	const Vector3 ap = p - a;
	const real_t  d1 = ab.dot( ap );
	const real_t  d2 = ac.dot( ap );
	if ( d1 <= 0.0f && d2 <= 0.0f ) {
		return p.distance_squared_to( a );
	}

	const Vector3 bp = p - b;
	const real_t  d3 = ab.dot( bp );
	const real_t  d4 = ac.dot( bp );
	if ( d3 >= 0.0f && d4 <= d3 ) {
		return p.distance_squared_to( b );
	}

	const real_t vc = d1 * d4 - d3 * d2;
	if ( vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f ) {
		const real_t v = d1 / MAX( d1 - d3, ( real_t ) 1e-20 );
		return p.distance_squared_to( a + ab * v );
	}

	const Vector3 cp = p - c;
	const real_t  d5 = ab.dot( cp );
	const real_t  d6 = ac.dot( cp );
	if ( d6 >= 0.0f && d5 <= d6 ) {
		return p.distance_squared_to( c );
	}

	const real_t vb = d5 * d2 - d1 * d6;
	if ( vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f ) {
		const real_t w = d2 / MAX( d2 - d6, ( real_t ) 1e-20 );
		return p.distance_squared_to( a + ac * w );
	}

	const real_t va = d3 * d6 - d5 * d4;
	if ( va <= 0.0f && ( d4 - d3 ) >= 0.0f && ( d5 - d6 ) >= 0.0f ) {
		const real_t denominator = ( d4 - d3 ) + ( d5 - d6 );
		const real_t w = denominator > ( real_t ) 1e-20 ? ( d4 - d3 ) / denominator : 0.0f;
		return p.distance_squared_to( b + ( c - b ) * w );
	}

	const real_t denominator = va + vb + vc;
	if ( Math::abs( denominator ) <= ( real_t ) 1e-20 ) {
		return MIN( p.distance_squared_to( a ), MIN( p.distance_squared_to( b ), p.distance_squared_to( c ) ) );
	}
	const real_t inv = 1.0f / denominator;
	const real_t v = vb * inv;
	const real_t w = vc * inv;
	return p.distance_squared_to( a + ab * v + ac * w );
}

static int t3dHlodBuilderLargestAxis(
	const Vector3 &size ) {
	if ( size.x >= size.y && size.x >= size.z ) {
		return 0;
	}
	return size.y >= size.z ? 1 : 2;
}

EA_FORCE_INLINE static real_t t3dHlodBuilderComponent(
	const Vector3 &v,
	const int	   axis ) {
	return axis == 0 ? v.x : ( axis == 1 ? v.y : v.z );
}

typedef struct t3dHLODTriangleAxisLess_s
{
	const eastl::vector< t3dHLODTriangle_t > *m_pTriangles;
	int										  m_nAxis;

	bool operator()(
		const int nLhs,
		const int nRhs ) const {
		return t3dHlodBuilderComponent( ( *m_pTriangles )[( size_t ) nLhs].m_center, m_nAxis ) <
			   t3dHlodBuilderComponent( ( *m_pTriangles )[( size_t ) nRhs].m_center, m_nAxis );
	}
} t3dHLODTriangleAxisLess_t;

static int t3dHlodBuilderBuildBvhRecursive(
	const eastl::vector< t3dHLODTriangle_t > &triangles,
	eastl::vector< int >					 &order,
	eastl::vector< t3dHLODBvhNode_t >		 &nodes,
	const int								  begin,
	const int								  count ) {
	t3dHLODBvhNode_t node;
	node.m_nBegin = begin;
	node.m_nCount = count;
	AABB bounds;
	AABB center_bounds;
	bool first = true;
	for ( int i = 0; i < count; ++i ) {
		const t3dHLODTriangle_t &triangle = triangles[( size_t ) order[( size_t ) ( begin + i )]];
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
	const int node_index = ( int ) nodes.size();
	nodes.push_back( node );
	if ( count <= 8 ) {
		return node_index;
	}

	const int				  axis = t3dHlodBuilderLargestAxis( center_bounds.size );
	t3dHLODTriangleAxisLess_t less;
	less.m_pTriangles = &triangles;
	less.m_nAxis = axis;
	eastl::sort( order.begin() + begin, order.begin() + begin + count, less );
	const int left_count = count / 2;
	const int right_count = count - left_count;
	nodes[( size_t ) node_index].m_nLeft =
		t3dHlodBuilderBuildBvhRecursive( triangles, order, nodes, begin, left_count );
	nodes[( size_t ) node_index].m_nRight =
		t3dHlodBuilderBuildBvhRecursive( triangles, order, nodes, begin + left_count, right_count );
	nodes[( size_t ) node_index].m_nCount = 0;
	return node_index;
}

static void t3dHlodBuilderQueryBvhRecursive(
	const eastl::vector< t3dHLODTriangle_t > &triangles,
	const eastl::vector< int >				 &order,
	const eastl::vector< t3dHLODBvhNode_t >	 &nodes,
	const int								  node_index,
	const Vector3							 &point,
	real_t									 &best_squared ) {
	if ( node_index < 0 ) {
		return;
	}
	const t3dHLODBvhNode_t &node = nodes[( size_t ) node_index];
	if ( t3dHlodBuilderPointAabbDistanceSquared( point, node.m_bounds ) >= best_squared ) {
		return;
	}
	if ( node.m_nCount > 0 ) {
		for ( int i = 0; i < node.m_nCount; ++i ) {
			const t3dHLODTriangle_t &triangle = triangles[( size_t ) order[( size_t ) ( node.m_nBegin + i )]];
			if ( t3dHlodBuilderPointAabbDistanceSquared( point, triangle.m_bounds ) >= best_squared ) {
				continue;
			}
			best_squared =
				MIN( best_squared,
					 t3dHlodBuilderPointTriangleDistanceSquared( point, triangle.m_a, triangle.m_b, triangle.m_c ) );
		}
		return;
	}

	const t3dHLODBvhNode_t &left = nodes[( size_t ) node.m_nLeft];
	const t3dHLODBvhNode_t &right = nodes[( size_t ) node.m_nRight];
	const real_t			left_distance = t3dHlodBuilderPointAabbDistanceSquared( point, left.m_bounds );
	const real_t			right_distance = t3dHlodBuilderPointAabbDistanceSquared( point, right.m_bounds );
	if ( left_distance <= right_distance ) {
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nLeft, point, best_squared );
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nRight, point, best_squared );
	} else {
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nRight, point, best_squared );
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, node.m_nLeft, point, best_squared );
	}
}

static bool t3dHlodBuilderHasUnsupportedStaticAttributes(
	const Array &arrays ) {
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

static Array t3dHlodBuilderCompactSurfaceArrays(
	const Array			   &arrays,
	const PackedInt32Array &indices ) {
	Array compact;
	compact.resize( Mesh::ARRAY_MAX );
	if ( arrays.size() <= Mesh::ARRAY_VERTEX ||
		 arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY ) {
		return Array();
	}
	const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
	if ( vertices.is_empty() || indices.is_empty() ) {
		return Array();
	}

	eastl::vector< int32_t > remap( ( size_t ) vertices.size(), -1 );
	eastl::vector< int32_t > used;
	used.reserve( ( size_t ) vertices.size() );
	PackedInt32Array compact_indices;
	compact_indices.resize( indices.size() );
	for ( int i = 0; i < indices.size(); ++i ) {
		const int old_index = indices[i];
		if ( old_index < 0 || old_index >= vertices.size() ) {
			return Array();
		}
		int32_t mapped = remap[( size_t ) old_index];
		if ( mapped < 0 ) {
			mapped = ( int32_t ) used.size();
			remap[( size_t ) old_index] = mapped;
			used.push_back( old_index );
		}
		compact_indices.set( i, mapped );
	}

	PackedVector3Array compact_vertices;
	compact_vertices.resize( ( int ) used.size() );
	for ( int i = 0; i < ( int ) used.size(); ++i ) {
		compact_vertices.set( i, vertices[used[( size_t ) i]] );
	}
	compact[Mesh::ARRAY_VERTEX] = compact_vertices;

	if ( arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY ) {
		const PackedVector3Array source = arrays[Mesh::ARRAY_NORMAL];
		if ( source.size() == vertices.size() ) {
			PackedVector3Array destination;
			destination.resize( ( int ) used.size() );
			for ( int i = 0; i < ( int ) used.size(); ++i ) {
				destination.set( i, source[used[( size_t ) i]] );
			}
			compact[Mesh::ARRAY_NORMAL] = destination;
		}
	}
	if ( arrays[Mesh::ARRAY_TANGENT].get_type() == Variant::PACKED_FLOAT32_ARRAY ) {
		const PackedFloat32Array source = arrays[Mesh::ARRAY_TANGENT];
		if ( source.size() == vertices.size() * 4 ) {
			PackedFloat32Array destination;
			destination.resize( ( int ) used.size() * 4 );
			for ( int i = 0; i < ( int ) used.size(); ++i ) {
				const int old = used[( size_t ) i] * 4;
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
			destination.resize( ( int ) used.size() );
			for ( int i = 0; i < ( int ) used.size(); ++i ) {
				destination.set( i, source[used[( size_t ) i]] );
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
		destination.resize( ( int ) used.size() );
		for ( int i = 0; i < ( int ) used.size(); ++i ) {
			destination.set( i, source[used[( size_t ) i]] );
		}
		compact[channel] = destination;
	}
	compact[Mesh::ARRAY_INDEX] = compact_indices;
	return compact;
}

static int64_t t3dHlodBuilderTriangleCount(
	const Array &arrays ) {
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

static real_t t3dHlodBuilderMeasureVertexDeviation(
	const Array &source_arrays,
	const Array &proxy_arrays ) {
	if ( source_arrays.size() <= Mesh::ARRAY_VERTEX || proxy_arrays.size() <= Mesh::ARRAY_INDEX ) {
		return 0.0f;
	}
	const PackedVector3Array source_vertices = source_arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array proxy_vertices = proxy_arrays[Mesh::ARRAY_VERTEX];
	const PackedInt32Array	 proxy_indices = proxy_arrays[Mesh::ARRAY_INDEX];
	if ( source_vertices.is_empty() || proxy_vertices.is_empty() || proxy_indices.size() < 3 ) {
		return 0.0f;
	}

	eastl::vector< t3dHLODTriangle_t > triangles;
	triangles.reserve( ( size_t ) proxy_indices.size() / 3u );
	for ( int i = 0; i + 2 < proxy_indices.size(); i += 3 ) {
		const int ia = proxy_indices[i];
		const int ib = proxy_indices[i + 1];
		const int ic = proxy_indices[i + 2];
		if ( ia < 0 || ib < 0 || ic < 0 || ia >= proxy_vertices.size() || ib >= proxy_vertices.size() ||
			 ic >= proxy_vertices.size() ) {
			continue;
		}
		t3dHLODTriangle_t triangle;
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

	eastl::vector< int > order;
	order.resize( triangles.size() );
	for ( int i = 0; i < ( int ) triangles.size(); ++i ) {
		order[( size_t ) i] = i;
	}
	eastl::vector< t3dHLODBvhNode_t > nodes;
	nodes.reserve( triangles.size() * 2u );
	const int root = t3dHlodBuilderBuildBvhRecursive( triangles, order, nodes, 0, ( int ) triangles.size() );
	real_t	  max_squared = 0.0f;
	for ( int i = 0; i < source_vertices.size(); ++i ) {
		real_t best_squared = INFINITY;
		t3dHlodBuilderQueryBvhRecursive( triangles, order, nodes, root, source_vertices[i], best_squared );
		if ( isfinite( ( double ) best_squared ) ) {
			max_squared = MAX( max_squared, best_squared );
		}
	}
	return Math::sqrt( max_squared );
}

PackedInt32Array Terrain3DHLODBuilder::getInstancerMeshIds(
	Terrain3DRegion *const pRegion ) const {
	PackedInt32Array result;
	if ( !pRegion ) {
		return result;
	}
	const Terrain3DInstanceStore &store = pRegion->getInstanceStore();
	for ( size_t iMesh = 0u; iMesh < ( size_t ) ( ( store.meshes() ).size() ); iMesh++ ) {
		const t3dInstanceMesh_t &mesh = ( store.meshes() )[iMesh];
		bool					 has_instances = false;
		for ( size_t iCell = 0u; iCell < ( size_t ) ( ( mesh.m_cells ).size() ); iCell++ ) {
			const t3dInstanceCell_s &cell = ( mesh.m_cells )[iCell];
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

static AABB t3dHlodBuilderTransformBounds(
	const Transform3D &rTransform,
	const AABB		  &rBounds ) {
	const Vector3 minimum = rBounds.position;
	const Vector3 maximum = rBounds.get_end();
	Vector3		  out_min = rTransform.xform( minimum );
	Vector3		  out_max = out_min;
	for ( int corner = 1; corner < 8; ++corner ) {
		const Vector3 local( ( corner & 1 ) != 0 ? maximum.x : minimum.x,
							 ( corner & 2 ) != 0 ? maximum.y : minimum.y,
							 ( corner & 4 ) != 0 ? maximum.z : minimum.z );
		const Vector3 point = rTransform.xform( local );
		out_min.x = MIN( out_min.x, point.x );
		out_min.y = MIN( out_min.y, point.y );
		out_min.z = MIN( out_min.z, point.z );
		out_max.x = MAX( out_max.x, point.x );
		out_max.y = MAX( out_max.y, point.y );
		out_max.z = MAX( out_max.z, point.z );
	}
	return AABB( out_min, out_max - out_min );
}

Dictionary Terrain3DHLODBuilder::analyzeInstancerSource(
	Terrain3D *const	   pTerrain,
	Terrain3DRegion *const pRegion,
	const int			   nMeshId ) const {
	Dictionary result;
	result["ok"] = false;
	if ( !pTerrain || !pRegion || pTerrain->getAssets().is_null() || nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS ) {
		result["error"] = "invalid Terrain3D instancer source";
		return result;
	}
	const Ref< Terrain3DMeshAsset > asset = pTerrain->getAssets()->getMeshAsset( nMeshId );
	if ( asset.is_null() || !asset->isEnabled() || !asset->isHlodEligible() ) {
		result["error"] = "mesh asset is not HLOD eligible";
		return result;
	}
	if ( asset->getMaterialOverlay().is_valid() || asset->getRenderingMaterialOverlay().is_valid() ) {
		result["error"] = "material_overlay is unsupported for HLOD proxy baking";
		return result;
	}
	const Ref< Mesh > mesh = asset->getMesh( 0 );
	ArrayMesh *const  pArrayMesh = mesh.is_valid() ? Object::cast_to< ArrayMesh >( mesh.ptr() ) : NULL;
	if ( !pArrayMesh || pArrayMesh->get_surface_count() <= 0 ) {
		result["error"] = "HLOD instancer source requires non-empty ArrayMesh LOD0 geometry";
		return result;
	}
	int64_t triangles_per_instance = 0;
	for ( int surface = 0; surface < pArrayMesh->get_surface_count(); ++surface ) {
		if ( !pArrayMesh->surface_get_blend_shape_arrays( surface ).is_empty() ) {
			result["error"] = "blend shapes are unsupported by HLOD proxies";
			return result;
		}
		if ( pArrayMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		const Array arrays = pArrayMesh->surface_get_arrays( surface );
		if ( t3dHlodBuilderHasUnsupportedStaticAttributes( arrays ) ) {
			result["error"] = "mesh uses custom/skinned vertex attributes unsupported by HLOD proxies";
			return result;
		}
		triangles_per_instance += t3dHlodBuilderTriangleCount( arrays );
	}
	if ( triangles_per_instance <= 0 ) {
		result["error"] = "instancer source produced no triangle geometry";
		return result;
	}
	const Terrain3DInstanceStore  &store = pRegion->getInstanceStore();
	const t3dInstanceMesh_t *const source_instances = store.findMesh( nMeshId );
	if ( !source_instances ) {
		result["error"] = "region contains no instances for the mesh asset";
		return result;
	}

	const AABB mesh_bounds = pArrayMesh->get_aabb();
	AABB	   bounds;
	bool	   have_bounds = false;
	int64_t	   instance_count = 0;
	uint64_t   signature = 1469598103934665603ull;
	signature = t3dHashFnv1a64( &nMeshId, sizeof( nMeshId ), signature );
	const uint32_t category = ( uint32_t ) ( asset->getInstanceCategory() );
	const uint32_t hlod_min_level = ( uint32_t ) ( asset->getHlodMinLevel() );
	signature = t3dHashFnv1a64( &category, sizeof( category ), signature );
	signature = t3dHashFnv1a64( &hlod_min_level, sizeof( hlod_min_level ), signature );
	for ( size_t iCell = 0u; iCell < source_instances->m_cells.size(); ++iCell ) {
		const t3dInstanceCell_s &cell = source_instances->m_cells[iCell];
		signature = t3dHashFnv1a64( &cell.m_location, sizeof( cell.m_location ), signature );
		if ( cell.usesColors() ) {
			for ( size_t iColor = 0u; iColor < cell.m_colors.size(); ++iColor ) {
				const uint32_t color = cell.m_colors[iColor];
				if ( color != T3D_INSTANCE_WHITE_RGBA8 ) {
					result["error"] = "per-instance color tinting requires a dedicated HLOD material bake and is "
									  "excluded from geometry-only proxies";
					return result;
				}
			}
		}
		const t3dPackedTransform_s *const pTransforms = cell.m_transforms.data();
		if ( pTransforms && !cell.m_transforms.empty() ) {
			signature =
				t3dHashFnv1a64( pTransforms, cell.m_transforms.size() * sizeof( t3dPackedTransform_s ), signature );
		}
		for ( size_t iTransform = 0u; iTransform < cell.m_transforms.size(); ++iTransform ) {
			const AABB transformed = t3dHlodBuilderTransformBounds( cell.getTransform( iTransform ), mesh_bounds );
			if ( !have_bounds ) {
				bounds = transformed;
				have_bounds = true;
			} else {
				bounds = bounds.merge( transformed );
			}
			++instance_count;
		}
	}
	if ( instance_count <= 0 || !have_bounds ) {
		result["error"] = "region contains no instances for the mesh asset";
		return result;
	}
	result["ok"] = true;
	result["bounds"] = bounds;
	result["instance_count"] = instance_count;
	result["triangles_per_instance"] = triangles_per_instance;
	result["source_triangles"] = triangles_per_instance * instance_count;
	result["instance_signature"] = ( int64_t ) ( signature & 0x7FFFFFFFFFFFFFFFull );
	result["instance_category"] = ( int ) ( asset->getInstanceCategory() );
	return result;
}

static Ref< ArrayMesh > t3dHlodBuilderBuildInstancerChunk(
	const Ref< Mesh >				   &rMesh,
	const Ref< Terrain3DMeshAsset >	   &rAsset,
	const eastl::vector< Transform3D > &rTransforms ) {
	Ref< ArrayMesh > merged;
	ArrayMesh *const pArrayMesh = rMesh.is_valid() ? Object::cast_to< ArrayMesh >( rMesh.ptr() ) : NULL;
	if ( !pArrayMesh ) {
		return merged;
	}
	merged.instantiate();
	for ( int surface = 0; surface < pArrayMesh->get_surface_count(); ++surface ) {
		if ( pArrayMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		Ref< SurfaceTool > tool;
		tool.instantiate();
		tool->begin( Mesh::PRIMITIVE_TRIANGLES );
		for ( size_t iTransform = 0u; iTransform < rTransforms.size(); ++iTransform ) {
			tool->append_from( rMesh, surface, rTransforms[iTransform] );
		}
		tool->index();
		const Ref< Material > material = rAsset->getMaterialOverride().is_valid()
											 ? rAsset->getMaterialOverride()
											 : pArrayMesh->surface_get_material( surface );
		tool->set_material( material );
		merged = tool->commit( merged );
	}
	return merged;
}

static bool t3dHlodBuilderFlushInstancerChunk(
	const Terrain3DHLODBuilder *const pBuilder,
	const Ref< Mesh >				 &rMesh,
	const Ref< Terrain3DMeshAsset >	 &rAsset,
	eastl::vector< Transform3D >	 &rTransforms,
	const real_t					  fPreRatio,
	const real_t					  fMaxWorldError,
	const int64_t					  nTrianglesPerInstance,
	Array							 &rChunks,
	AABB							 &rSourceBounds,
	bool							 &rHaveBounds,
	int64_t							 &rProxyTriangles,
	real_t							 &rPreGeometricError ) {
	if ( rTransforms.empty() ) {
		return true;
	}
	const Ref< ArrayMesh > exact = t3dHlodBuilderBuildInstancerChunk( rMesh, rAsset, rTransforms );
	if ( exact.is_null() || exact->get_surface_count() <= 0 ) {
		return false;
	}
	const AABB exact_bounds = exact->get_aabb();
	if ( !rHaveBounds ) {
		rSourceBounds = exact_bounds;
		rHaveBounds = true;
	} else {
		rSourceBounds = rSourceBounds.merge( exact_bounds );
	}

	Ref< ArrayMesh > chunk_mesh = exact;
	int64_t			 chunk_proxy_triangles = 0;
	real_t			 chunk_error = 0.0f;
	if ( fPreRatio < 0.999f ) {
		const Dictionary simplified = pBuilder->simplifyMeshWithError( exact, fPreRatio, fMaxWorldError );
		if ( !( bool ) simplified.get( "ok", false ) ) {
			return false;
		}
		chunk_mesh = simplified.get( "mesh", Ref< ArrayMesh >() );
		chunk_proxy_triangles = ( int64_t ) simplified.get( "proxy_triangles", 0 );
		chunk_error = ( real_t ) simplified.get( "geometric_error", 0.0f );
	} else {
		for ( int surface = 0; surface < exact->get_surface_count(); ++surface ) {
			chunk_proxy_triangles += t3dHlodBuilderTriangleCount( exact->surface_get_arrays( surface ) );
		}
	}
	if ( chunk_mesh.is_null() || chunk_mesh->get_surface_count() <= 0 ) {
		return false;
	}
	Dictionary chunk;
	chunk["mesh"] = chunk_mesh;
	chunk["bounds"] = exact_bounds;
	chunk["source_triangles"] = nTrianglesPerInstance * ( int64_t ) rTransforms.size();
	chunk["proxy_triangles"] = chunk_proxy_triangles;
	chunk["geometric_error"] = chunk_error;
	rChunks.append( chunk );
	rProxyTriangles += chunk_proxy_triangles;
	rPreGeometricError = MAX( rPreGeometricError, chunk_error );
	rTransforms.clear();
	return true;
}

// Direct instance-source extraction for the INSTANCING HLOD recipe. The
// transformed source geometry is NOT expanded into SurfaceTool buffers.
Dictionary Terrain3DHLODBuilder::extractInstancerTransforms(
    Terrain3D *const pTerrain, Terrain3DRegion *const pRegion, const int nMeshId ) const
{
    Dictionary result;
    result["ok"] = false;
    if ( !pTerrain || !pRegion || pTerrain->getAssets().is_null() ||
         nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS ) { return result; }
    const Ref< Terrain3DMeshAsset > asset = pTerrain->getAssets()->getMeshAsset( nMeshId );
    if ( asset.is_null() || !asset->isEnabled() || !asset->isHlodEligible() ||
         asset->getMaterialOverlay().is_valid() || asset->getRenderingMaterialOverlay().is_valid() ) {
        return result;
    }
    const Ref< Mesh > source = asset->getMesh( 0 );
    ArrayMesh *const pMesh = source.is_valid() ? Object::cast_to< ArrayMesh >( source.ptr() ) : NULL;
    if ( !pMesh || pMesh->get_surface_count() == 0 ) { return result; }
    for ( int surface = 0; surface < pMesh->get_surface_count(); ++surface ) {
        if ( pMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ||
             !pMesh->surface_get_blend_shape_arrays( surface ).is_empty() ||
             t3dHlodBuilderHasUnsupportedStaticAttributes( pMesh->surface_get_arrays( surface ) ) ) {
            return result;
        }
    }
    const t3dInstanceMesh_t *const pInstances = pRegion->getInstanceStore().findMesh( nMeshId );
    if ( !pInstances ) { return result; }
    size_t count = 0u;
    for ( size_t i = 0u; i < pInstances->m_cells.size(); ++i ) {
        const t3dInstanceCell_s &cell = pInstances->m_cells[i];
        count += cell.m_transforms.size();
        if ( count > 1048576u ) { return result; }
        if ( cell.usesColors() ) {
            for ( size_t j = 0u; j < cell.m_colors.size(); ++j ) {
                if ( cell.m_colors[j] != T3D_INSTANCE_WHITE_RGBA8 ) { return result; }
            }
        }
    }
    if ( count == 0u ) { return result; }
    Array transforms;
    transforms.resize( (int64_t) count );
    size_t index = 0u;
    for ( size_t i = 0u; i < pInstances->m_cells.size(); ++i ) {
        const t3dInstanceCell_s &cell = pInstances->m_cells[i];
        for ( size_t j = 0u; j < cell.m_transforms.size(); ++j ) {
            const Transform3D transform = cell.getTransform( j );
            if ( transform.basis.determinant() <= 0.0f ) { return result; }
            transforms[(int64_t) index++] = transform;
        }
    }
    result["ok"] = true;
    result["mesh"] = source;
    result["transforms"] = transforms;
    result["count"] = (int64_t) count;
    return result;
}

Dictionary Terrain3DHLODBuilder::buildInstancerSource(
	Terrain3D *const	   pTerrain,
	Terrain3DRegion *const pRegion,
	const int			   nMeshId,
	const bool			   bPreserveTopology,
	const real_t			   fMaxWorldError ) const {
	Dictionary result;
	result["ok"] = false;
	if ( !pTerrain || !pRegion || pTerrain->getAssets().is_null() || nMeshId < 0 || nMeshId >= T3D_MAX_MESH_ASSETS ) {
		result["error"] = "invalid Terrain3D instancer source";
		return result;
	}
	const Ref< Terrain3DMeshAsset > asset = pTerrain->getAssets()->getMeshAsset( nMeshId );
	if ( asset.is_null() || !asset->isEnabled() || !asset->isHlodEligible() ) {
		result["error"] = "mesh asset is not HLOD eligible";
		return result;
	}
	if ( asset->getMaterialOverlay().is_valid() ) {
		result["error"] = "material_overlay is unsupported for HLOD proxy baking";
		return result;
	}
	const Ref< Mesh > mesh = asset->getMesh( 0 );
	ArrayMesh *const  pArrayMesh = mesh.is_valid() ? Object::cast_to< ArrayMesh >( mesh.ptr() ) : NULL;
	bool			  has_blend_shapes = false;
	if ( pArrayMesh ) {
		for ( int surface = 0; surface < pArrayMesh->get_surface_count(); ++surface ) {
			if ( !pArrayMesh->surface_get_blend_shape_arrays( surface ).is_empty() ) {
				has_blend_shapes = true;
				break;
			}
		}
	}
	if ( !pArrayMesh || pArrayMesh->get_surface_count() <= 0 || has_blend_shapes ) {
		result["error"] = "HLOD instancer source requires non-empty static ArrayMesh LOD0 geometry";
		return result;
	}

	int64_t triangles_per_instance = 0;
	for ( int surface = 0; surface < pArrayMesh->get_surface_count(); ++surface ) {
		if ( pArrayMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		const Array arrays = pArrayMesh->surface_get_arrays( surface );
		if ( t3dHlodBuilderHasUnsupportedStaticAttributes( arrays ) ) {
			result["error"] = "mesh uses custom/skinned vertex attributes unsupported by HLOD proxies";
			return result;
		}
		triangles_per_instance += t3dHlodBuilderTriangleCount( arrays );
	}
	if ( triangles_per_instance <= 0 ) {
		result["error"] = "instancer source produced no triangle geometry";
		return result;
	}

	const Terrain3DInstanceStore  &store = pRegion->getInstanceStore();
	const t3dInstanceMesh_t *const source_instances = store.findMesh( nMeshId );
	if ( !source_instances ) {
		result["error"] = "region contains no instances for the mesh asset";
		return result;
	}

	int64_t instance_count = 0;
	for ( size_t iCell = 0u; iCell < ( size_t ) ( ( source_instances->m_cells ).size() ); iCell++ ) {
		const t3dInstanceCell_s &cell = ( source_instances->m_cells )[iCell];
		instance_count += ( int64_t ) cell.m_transforms.size();
		if ( cell.usesColors() ) {
			for ( size_t iColor = 0u; iColor < ( size_t ) ( ( cell.m_colors ).size() ); iColor++ ) {
				const uint32_t color = ( cell.m_colors )[iColor];
				if ( color != T3D_INSTANCE_WHITE_RGBA8 ) {
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

	// Dense instancer HLOD used to expand every instance in the region into one
	// enormous SurfaceTool before simplification. Bound the temporary image by
	// chunking spatially-adjacent instance-store cells and, for very dense
	// sources, pre-reduce each bounded chunk before GDScript assembles L1.

	const int64_t source_triangles = triangles_per_instance * instance_count;
	// MERGED explicitly preserves input topology. The existing pre-proxy budget
	// becomes a hard fail instead of silently invoking legacy simplification.
	if ( bPreserveTopology && source_triangles > T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET ) {
		result["error"] = "exact instancer HLOD exceeds the pre-proxy triangle budget; use a lower authored LOD or a simplified recipe";
		return result;
	}
	const int	  max_instances_by_triangles =
		( int ) MAX< int64_t >( 1, T3D_HLOD_CHUNK_TRIANGLE_BUDGET / MAX< int64_t >( triangles_per_instance, 1 ) );
	const int	 max_instances_per_chunk = MIN( T3D_HLOD_MAX_INSTANCES_PER_CHUNK, max_instances_by_triangles );
	const real_t pre_ratio =
		!bPreserveTopology && source_triangles > T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET
			? CLAMP( ( real_t ) T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET / ( real_t ) source_triangles, 0.01f, 1.0f )
			: 1.0f;
	if ( pre_ratio < 0.999f && ( !Math::is_finite( fMaxWorldError ) || fMaxWorldError < 0.0f ) ) {
		result["error"] = "dense HLOD pre-simplification requires an explicit nonnegative world-error budget";
		return result;
	}

	Array						 chunks;
	eastl::vector< Transform3D > transforms;
	transforms.reserve( ( size_t ) max_instances_per_chunk );
	AABB	source_bounds;
	bool	have_bounds = false;
	int64_t proxy_triangles = 0;
	real_t	pre_geometric_error = 0.0f;

	for ( size_t iCell = 0u; iCell < ( size_t ) ( ( source_instances->m_cells ).size() ); iCell++ ) {
		const t3dInstanceCell_s &cell = ( source_instances->m_cells )[iCell];
		for ( size_t transform_index = 0u; transform_index < cell.m_transforms.size(); ++transform_index ) {
			transforms.push_back( cell.getTransform( transform_index ) );
			if ( ( int ) transforms.size() >= max_instances_per_chunk &&
				 !t3dHlodBuilderFlushInstancerChunk( this,
													 mesh,
													 asset,
													 transforms,
													 pre_ratio,
													 fMaxWorldError,
													 triangles_per_instance,
													 chunks,
													 source_bounds,
													 have_bounds,
													 proxy_triangles,
													 pre_geometric_error ) ) {
				result["error"] = "instancer HLOD chunk construction/simplification failed";
				return result;
			}
		}
		// Cell boundaries are spatial boundaries. Flush a partially-filled chunk
		// here so unrelated cells are never merged merely to fill a batch.
		if ( !transforms.empty() && !t3dHlodBuilderFlushInstancerChunk( this,
																		mesh,
																		asset,
																		transforms,
																		pre_ratio,
																		fMaxWorldError,
																		triangles_per_instance,
																		chunks,
																		source_bounds,
																		have_bounds,
																		proxy_triangles,
																		pre_geometric_error ) ) {
			result["error"] = "instancer HLOD chunk construction/simplification failed";
			return result;
		}
	}

	if ( chunks.is_empty() || !have_bounds ) {
		result["error"] = "instancer source produced no triangle geometry";
		return result;
	}
	if ( !bPreserveTopology && source_triangles > T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET &&
		 proxy_triangles > T3D_HLOD_PRE_PROXY_TRIANGLE_BUDGET ) {
		result["error"] = "dense instancer cannot meet the pre-proxy triangle budget within the requested world-error ceiling";
		return result;
	}
	result["ok"] = true;
	result["chunks"] = chunks;
	if ( chunks.size() == 1 ) {
		const Dictionary only = chunks[0];
		result["mesh"] = only.get( "mesh", Ref< ArrayMesh >() );
	}
	result["bounds"] = source_bounds;
	result["source_triangles"] = source_triangles;
	result["proxy_triangles"] = proxy_triangles;
	result["instance_count"] = instance_count;
	result["chunk_count"] = chunks.size();
	result["pre_reduction_ratio"] = pre_ratio;
	result["geometric_error"] = pre_geometric_error;
	return result;
}

Dictionary Terrain3DHLODBuilder::optimizeMesh( const Ref< ArrayMesh > &rMesh ) const {
	Dictionary result;
	result["ok"] = false;
	if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) {
		result["error"] = "HLOD mesh optimization requires a non-empty ArrayMesh";
		return result;
	}
	Ref< ArrayMesh > optimized_mesh;
	optimized_mesh.instantiate();
	bool changed = false;
	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		Array arrays = rMesh->surface_get_arrays( surface );
		Array reordered;
		if ( rMesh->surface_get_primitive_type( surface ) == Mesh::PRIMITIVE_TRIANGLES &&
			t3dMeshoptCook::optimizeArrays( arrays, reordered ) ) {
			arrays = reordered;
			changed = true;
		}
		optimized_mesh->add_surface_from_arrays( rMesh->surface_get_primitive_type( surface ), arrays );
		optimized_mesh->surface_set_material( surface, rMesh->surface_get_material( surface ) );
	}
	result["ok"] = true;
	result["mesh"] = changed ? optimized_mesh : rMesh;
	return result;
}

Dictionary Terrain3DHLODBuilder::simplifyMesh(
	const Ref< ArrayMesh > &rMesh,
	const real_t fRatio ) const {
	// Retained native/GDScript entrypoint: a zero-error meshoptimizer cook.
	// All hierarchy/instancer cooking callers pass their explicit world budget.
	return simplifyMeshWithError( rMesh, fRatio, 0.0f );
}

Dictionary Terrain3DHLODBuilder::simplifyMeshWithError(
	const Ref< ArrayMesh > &rMesh,
	const real_t fRatio,
	const real_t fMaxWorldError ) const {
	Dictionary result;
	result["ok"] = false;
	if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) {
		result["error"] = "HLOD simplify requires a non-empty ArrayMesh";
		return result;
	}
	const real_t ratio = CLAMP( fRatio, 0.01f, 1.0f );
	if ( !Math::is_finite( fMaxWorldError ) || fMaxWorldError < 0.0f ) {
		result["error"] = "HLOD simplification requires a finite nonnegative world-error budget";
		return result;
	}
	if ( !t3dMeshoptCook::available() ) {
		result["error"] = "meshoptimizer v1.3 is not vendored in the editor-native build";
		return result;
	}
	Ref< ArrayMesh > output;
	output.instantiate();
	real_t	max_error = 0.0f;
	int64_t source_triangles = 0;
	int64_t proxy_triangles = 0;

	for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
		if ( rMesh->surface_get_primitive_type( surface ) != Mesh::PRIMITIVE_TRIANGLES ) {
			continue;
		}
		Ref< SurfaceTool > source_tool;
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
		float fMeshoptError = 0.0f;
		if ( !t3dMeshoptCook::simplifyArrays( source_arrays, source_indices,
			(float) ratio, (float) fMaxWorldError, simplified_indices, fMeshoptError ) ) {
			result["error"] = "HLOD meshoptimizer rejected input or could not meet the error ceiling";
			return result;
		}
		max_error = MAX( max_error, (real_t) fMeshoptError );
		Array compact_arrays = t3dHlodBuilderCompactSurfaceArrays( source_arrays, simplified_indices );
		if ( compact_arrays.is_empty() ) {
			result["error"] = "HLOD proxy vertex compaction failed";
			return result;
		}
		max_error = MAX( max_error, t3dHlodBuilderMeasureVertexDeviation( source_arrays, compact_arrays ) );
		Array optimized;
		if ( t3dMeshoptCook::optimizeArrays( compact_arrays, optimized ) ) { compact_arrays = optimized; }
		proxy_triangles += simplified_indices.size() / 3;
		output->add_surface_from_arrays( Mesh::PRIMITIVE_TRIANGLES, compact_arrays );
		output->surface_set_material( output->get_surface_count() - 1, rMesh->surface_get_material( surface ) );
	}
	if ( max_error > fMaxWorldError + 0.0001f ) {
		result["error"] = "HLOD simplifier exceeds the independent geometric error ceiling";
		return result;
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

static bool t3dHlodFillPrototype( const Ref< ArrayMesh > &rMesh,
    const uint64_t nSignature, t3dHLODGroupPrototype_t &prototype )
{
    if ( rMesh.is_null() || rMesh->get_surface_count() <= 0 ) { return false; }
    prototype.m_nGeometryKey = nSignature | 1ull;
    prototype.m_nMaterialKey = nSignature | 1ull;
    const AABB bounds = rMesh->get_aabb();
    prototype.m_fBounds[0] = (float) bounds.position.x;
    prototype.m_fBounds[1] = (float) bounds.position.y;
    prototype.m_fBounds[2] = (float) bounds.position.z;
    prototype.m_fBounds[3] = (float) bounds.size.x;
    prototype.m_fBounds[4] = (float) bounds.size.y;
    prototype.m_fBounds[5] = (float) bounds.size.z;
    uint64_t vertexBytes = 0u;
    uint64_t indexBytes = 0u;
    for ( int surface = 0; surface < rMesh->get_surface_count(); ++surface ) {
        const Array arrays = rMesh->surface_get_arrays( surface );
        const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
        const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
        const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
        const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
        const PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
        const PackedVector2Array uv = arrays[Mesh::ARRAY_TEX_UV];
        const PackedVector2Array uv2 = arrays[Mesh::ARRAY_TEX_UV2];
        vertexBytes += (uint64_t) positions.size() * 12u + (uint64_t) normals.size() * 12u;
        vertexBytes += (uint64_t) tangents.size() * 4u + (uint64_t) colors.size() * 16u;
        vertexBytes += (uint64_t) uv.size() * 8u + (uint64_t) uv2.size() * 8u;
        indexBytes += (uint64_t) indices.size() * 4u;
    }
    if ( vertexBytes > UINT32_MAX || indexBytes > UINT32_MAX ||
         vertexBytes == 0u || indexBytes == 0u ) {
        return false;
    }
    prototype.m_nVertexBytes = (uint32_t) vertexBytes;
    prototype.m_nIndexBytes = (uint32_t) indexBytes;
    return true;
}

PackedByteArray Terrain3DHLODBuilder::encodeMultiPrototypeGroup(
    const Array &rMeshes, const Array &rTransforms,
    const PackedInt32Array &rPrototypeIndices, const Array &rSignatures ) const
{
    if ( rMeshes.is_empty() || rMeshes.size() != rSignatures.size() ||
         rTransforms.is_empty() || rTransforms.size() != rPrototypeIndices.size() ) {
        return PackedByteArray();
    }
    t3dHLODGroupData_t group;
    for ( int i = 0; i < rMeshes.size(); ++i ) {
        if ( rMeshes[i].get_type() != Variant::OBJECT ||
             rSignatures[i].get_type() != Variant::INT ) { return PackedByteArray(); }
        const Ref< ArrayMesh > mesh = rMeshes[i];
        t3dHLODGroupPrototype_t prototype;
        if ( !t3dHlodFillPrototype( mesh, (uint64_t) (int64_t) rSignatures[i], prototype ) ) {
            return PackedByteArray();
        }
        group.m_prototypes.push_back( prototype );
    }
    for ( int i = 0; i < rTransforms.size(); ++i ) {
        if ( rTransforms[i].get_type() != Variant::TRANSFORM3D ||
             rPrototypeIndices[i] < 0 || rPrototypeIndices[i] >= rMeshes.size() ) {
            return PackedByteArray();
        }
        const Transform3D transform = rTransforms[i];
        t3dHLODGroupInstance_t instance;
        instance.m_nPrototype = (uint32_t) rPrototypeIndices[i];
        instance.m_nSourceId = (uint32_t) i;
        for ( int row = 0; row < 3; ++row ) {
            for ( int col = 0; col < 3; ++col ) {
                instance.m_fAffine[row * 4 + col] = (float) transform.basis.rows[row][col];
            }
        }
        instance.m_fAffine[3] = (float) transform.origin.x;
        instance.m_fAffine[7] = (float) transform.origin.y;
        instance.m_fAffine[11] = (float) transform.origin.z;
        group.m_instances.push_back( instance );
    }
    return Terrain3DHLODInstanceCodec::encode( group );
}

PackedByteArray Terrain3DHLODBuilder::encodeSinglePrototypeGroup(
    const Ref< ArrayMesh > &rMesh, const Array &rTransforms, const int64_t nSignature ) const
{
    Array meshes;
    meshes.push_back( rMesh );
    Array signatures;
    signatures.push_back( nSignature );
    PackedInt32Array indices;
    indices.resize( rTransforms.size() );
    for ( int i = 0; i < indices.size(); ++i ) { indices.set( i, 0 ); }
    return encodeMultiPrototypeGroup( meshes, rTransforms, indices, signatures );
}

Dictionary Terrain3DHLODBuilder::decodeMultiPrototypeGroup( const PackedByteArray &rPayload ) const
{
    Dictionary result;
    t3dHLODGroupData_t group;
    if ( !Terrain3DHLODInstanceCodec::decode( rPayload, group ) ) { return result; }
    Array transforms;
    PackedInt32Array prototypes;
    prototypes.resize( (int64_t) group.m_instances.size() );
    for ( size_t i = 0u; i < group.m_instances.size(); ++i ) {
        const t3dHLODGroupInstance_t &instance = group.m_instances[i];
        const float *const a = instance.m_fAffine;
        const Basis basis( Vector3( a[0], a[4], a[8] ),
            Vector3( a[1], a[5], a[9] ), Vector3( a[2], a[6], a[10] ) );
        transforms.push_back( Transform3D( basis, Vector3( a[3], a[7], a[11] ) ) );
        prototypes.set( (int64_t) i, (int32_t) instance.m_nPrototype );
    }
    result["transforms"] = transforms;
    result["prototypes"] = prototypes;
    result["prototype_count"] = (int) group.m_prototypes.size();
    return result;
}

Array Terrain3DHLODBuilder::decodeSinglePrototypeGroup(
    const PackedByteArray &rPayload ) const
{
    Array result;
    t3dHLODGroupData_t group;
    if ( !Terrain3DHLODInstanceCodec::decode( rPayload, group ) ||
         group.m_prototypes.size() != 1u ) {
        return result;
    }
    for ( size_t i = 0u; i < group.m_instances.size(); ++i ) {
        const t3dHLODGroupInstance_t &instance = group.m_instances[i];
        const float *const a = instance.m_fAffine;
        const Basis basis( Vector3( a[0], a[4], a[8] ),
            Vector3( a[1], a[5], a[9] ), Vector3( a[2], a[6], a[10] ) );
        result.push_back( Transform3D( basis, Vector3( a[3], a[7], a[11] ) ) );
    }
    return result;
}

void Terrain3DHLODBuilder::_bind_methods(
	void ) {
	ClassDB::bind_method( D_METHOD( "get_instancer_mesh_ids", "region" ), &Terrain3DHLODBuilder::getInstancerMeshIds );
	ClassDB::bind_method( D_METHOD( "analyze_instancer_source", "terrain", "region", "mesh_id" ),
						  &Terrain3DHLODBuilder::analyzeInstancerSource );
    ClassDB::bind_method( D_METHOD( "extract_instancer_transforms", "terrain", "region", "mesh_id" ),
        &Terrain3DHLODBuilder::extractInstancerTransforms );
	ClassDB::bind_method( D_METHOD( "build_instancer_source", "terrain", "region", "mesh_id", "preserve_topology", "max_world_error" ),
						  &Terrain3DHLODBuilder::buildInstancerSource, DEFVAL( false ), DEFVAL( 0.0f ) );
	ClassDB::bind_method( D_METHOD( "optimize_mesh", "mesh" ), &Terrain3DHLODBuilder::optimizeMesh );
    ClassDB::bind_method( D_METHOD( "encode_single_prototype_group", "mesh", "transforms", "signature" ), &Terrain3DHLODBuilder::encodeSinglePrototypeGroup );
    ClassDB::bind_method( D_METHOD( "decode_single_prototype_group", "payload" ), &Terrain3DHLODBuilder::decodeSinglePrototypeGroup );
    ClassDB::bind_method( D_METHOD( "encode_multi_prototype_group", "meshes", "transforms", "prototype_indices", "signatures" ),
        &Terrain3DHLODBuilder::encodeMultiPrototypeGroup );
    ClassDB::bind_method( D_METHOD( "decode_multi_prototype_group", "payload" ), &Terrain3DHLODBuilder::decodeMultiPrototypeGroup );
	ClassDB::bind_method( D_METHOD( "simplify_mesh", "mesh", "ratio" ), &Terrain3DHLODBuilder::simplifyMesh );
	ClassDB::bind_method( D_METHOD( "simplify_mesh_with_error", "mesh", "ratio", "max_world_error" ),
		&Terrain3DHLODBuilder::simplifyMeshWithError );
}
