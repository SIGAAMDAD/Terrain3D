// Cooked runtime navigation geometry and editor raw-source fallback.
#include "terrain_3d.h"
#include "data/model/terrain_3d_data.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "core/utility/logger.h"
#include <math.h>

EA_FORCE_INLINE static real_t t3dBakeCollisionHeightAt(
	const PackedFloat32Array &rHeights,
	const int				  nWidth,
	const int				  iX,
	const int				  iZ ) {
	return ( real_t ) rHeights[iX * nWidth + ( nWidth - 1 - iZ )];
}

PackedVector3Array Terrain3D::generateNavMeshSourceGeometry(
	const AABB &rGlobalAabb,
	const bool	bRequireNav ) const {
	LOG( INFO, "Generating NavMesh source geometry from terrain" );
	PackedVector3Array faces;
	const bool		   strict_cooked = m_pData && isGpuRegionCookedPagesEnabled() && !isEditorAuthoringModeInternal();
	if ( !strict_cooked ) {
#if defined( T3D_EDITOR_NATIVE )
		generateTriangles( faces, NULL, 0, HEIGHT_FILTER_NEAREST, bRequireNav, rGlobalAabb );
#else
		LOG( ERROR, "Runtime navigation geometry requires a cooked navigation/collision page set" );
#endif
		return faces;
	}

	const int					 region_size = ( int ) T3D_REGION_SIZE;
	const int					 expected_width = region_size + 1;
	const TypedArray< Vector2i > locations = m_pData->getRegionLocations();
	for ( size_t iLocation = 0u; iLocation < ( size_t ) ( ( locations ).size() ); iLocation++ ) {
		const Vector2i location = ( locations )[iLocation];
		const real_t   region_world = ( real_t ) region_size * s_fRegionVertexSpacing;
		const real_t   region_min_x = ( real_t ) location.x * region_world;
		const real_t   region_min_z = ( real_t ) location.y * region_world;
		if ( rGlobalAabb.has_volume() ) {
			if ( region_min_x + region_world < rGlobalAabb.position.x || region_min_x > rGlobalAabb.get_end().x ||
				 region_min_z + region_world < rGlobalAabb.position.z || region_min_z > rGlobalAabb.get_end().z ) {
				continue;
			}
		}
		PackedByteArray		   navigation_blob;
		PackedByteArray		   collision_blob;
		PackedByteArray		   minmax_blob;
		PackedByteArray *const pMinMaxBlob = rGlobalAabb.has_volume() ? &minmax_blob : NULL;
		if ( !m_pData->loadCookedNavigationTileNow( location, navigation_blob, &collision_blob, pMinMaxBlob ) ) {
			LOG( ERROR, "Missing/invalid baked navigation tile for region ", location );
			continue;
		}
		PackedFloat32Array		collision_heights;
		t3dNavigationMaskView_t navigation_view;
		t3dMinMaxLevelView_t	minmax_view;
		int						collision_width = 0;
		const bool				use_minmax = pMinMaxBlob &&
								Terrain3DRuntimeBakeCodec::getMinMaxLevelView( minmax_blob, 16, minmax_view ) &&
								minmax_view.m_nSourceSide == region_size && minmax_view.m_nLevelSide > 0;
		if ( !Terrain3DRuntimeBakeCodec::getNavigationMaskView( navigation_blob, navigation_view ) ||
			 !Terrain3DRuntimeBakeCodec::decodeCollisionHeight( collision_blob, collision_heights, collision_width ) ||
			 navigation_view.m_nWidth != expected_width || collision_width != expected_width ||
			 ( pMinMaxBlob && !use_minmax ) ) {
			LOG( ERROR, "Baked navigation/collision acceleration data does not match region ", location );
			continue;
		}
		const real_t aabb_min_y = rGlobalAabb.position.y;
		const real_t aabb_max_y = rGlobalAabb.get_end().y;
		for ( int z = 0; z < region_size; ++z ) {
			for ( int x = 0; x < region_size; ++x ) {
				if ( use_minmax && x + 1 < region_size && z + 1 < region_size ) {
					const int coarse_x0 = x * minmax_view.m_nLevelSide / minmax_view.m_nSourceSide;
					const int coarse_z0 = z * minmax_view.m_nLevelSide / minmax_view.m_nSourceSide;
					const int coarse_x1 = ( x + 1 ) * minmax_view.m_nLevelSide / minmax_view.m_nSourceSide;
					const int coarse_z1 = ( z + 1 ) * minmax_view.m_nLevelSide / minmax_view.m_nSourceSide;
					if ( coarse_x0 == coarse_x1 && coarse_z0 == coarse_z1 ) {
						const Vector2 range = Terrain3DRuntimeBakeCodec::getMinMaxRange(
							minmax_view,
							( uint32_t ) ( coarse_z0 * minmax_view.m_nLevelSide + coarse_x0 ) );
						if ( range.y < aabb_min_y || range.x > aabb_max_y ) {
							continue;
						}
					}
				}
				const int  nav_width = navigation_view.m_nWidth;
				const int  i1 = z * nav_width + x;
				const int  i2 = i1 + 1;
				const int  i3 = ( z + 1 ) * nav_width + x;
				const int  i4 = i3 + 1;
				const bool valid1 = navigation_view.isValidSample( ( uint32_t ) i1 );
				const bool valid2 = navigation_view.isValidSample( ( uint32_t ) i2 );
				const bool valid3 = navigation_view.isValidSample( ( uint32_t ) i3 );
				const bool valid4 = navigation_view.isValidSample( ( uint32_t ) i4 );
				if ( !valid1 ) {
					continue;
				}
				const bool nav1 = navigation_view.isNavigationSample( ( uint32_t ) i1 );
				const bool nav2 = navigation_view.isNavigationSample( ( uint32_t ) i2 );
				const bool nav3 = navigation_view.isNavigationSample( ( uint32_t ) i3 );
				const bool nav4 = navigation_view.isNavigationSample( ( uint32_t ) i4 );
				const int  global_x = location.x * region_size + x;
				const int  global_z = location.y * region_size + z;
				Vector3	   v1( ( real_t ) global_x * s_fRegionVertexSpacing,
							   t3dBakeCollisionHeightAt( collision_heights, collision_width, x, z ),
							   ( real_t ) global_z * s_fRegionVertexSpacing );
				Vector3	   v2( ( real_t ) ( global_x + 1 ) * s_fRegionVertexSpacing,
							   t3dBakeCollisionHeightAt( collision_heights, collision_width, x + 1, z ),
							   ( real_t ) global_z * s_fRegionVertexSpacing );
				Vector3	   v3( ( real_t ) global_x * s_fRegionVertexSpacing,
							   t3dBakeCollisionHeightAt( collision_heights, collision_width, x, z + 1 ),
							   ( real_t ) ( global_z + 1 ) * s_fRegionVertexSpacing );
				Vector3	   v4( ( real_t ) ( global_x + 1 ) * s_fRegionVertexSpacing,
							   t3dBakeCollisionHeightAt( collision_heights, collision_width, x + 1, z + 1 ),
							   ( real_t ) ( global_z + 1 ) * s_fRegionVertexSpacing );
				if ( rGlobalAabb.has_volume() && ( v1.y < rGlobalAabb.position.y || v1.y > rGlobalAabb.get_end().y ||
												   v1.x < rGlobalAabb.position.x || v1.x > rGlobalAabb.get_end().x ||
												   v1.z < rGlobalAabb.position.z || v1.z > rGlobalAabb.get_end().z ) ) {
					continue;
				}
				if ( isnan( v2.y ) ) {
					v2.y = v1.y;
				}
				if ( isnan( v3.y ) ) {
					v3.y = v1.y;
				}
				if ( isnan( v4.y ) ) {
					v4.y = !isnan( v2.y ) ? v2.y : ( !isnan( v3.y ) ? v3.y : v1.y );
				}
				if ( valid1 && valid4 && valid3 && ( !bRequireNav || ( nav1 && nav4 && nav3 ) ) ) {
					faces.push_back( v1 );
					faces.push_back( v4 );
					faces.push_back( v3 );
				}
				if ( valid1 && valid2 && valid4 && ( !bRequireNav || ( nav1 && nav2 && nav4 ) ) ) {
					faces.push_back( v1 );
					faces.push_back( v2 );
					faces.push_back( v4 );
				}
			}
		}
	}
	return faces;
}
