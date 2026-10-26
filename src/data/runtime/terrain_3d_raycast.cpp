// Runtime physics raycast query; available to both native build flavors.
#include "terrain_3d.h"
#include "physics/runtime/terrain_3d_collision.h"
#include "core/utility/logger.h"
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/world3d.hpp>

Dictionary Terrain3D::getRaycastResult(
	const Vector3 &rSrcPos,
	const Vector3 &rDirection,
	const uint32_t nColMask,
	const bool	   bExcludeSelf ) const {
	if ( !m_bIsInsideWorld ) {
		return Dictionary();
	}
	PhysicsDirectSpaceState3D *pSpaceState = get_world_3d()->get_direct_space_state();
	if ( !pSpaceState ) {
		LOG( ERROR, "Invalid PhysicsDirectSpaceState3D" );
		return Dictionary();
	}
	Ref< PhysicsRayQueryParameters3D > pQuery =
		PhysicsRayQueryParameters3D::create( rSrcPos, rSrcPos + rDirection, nColMask );
	if ( m_pCollision && bExcludeSelf ) {
		pQuery->set_exclude( TypedArray< RID >( m_pCollision->getRid() ) );
	}
	return pSpaceState->intersect_ray( pQuery );
}
