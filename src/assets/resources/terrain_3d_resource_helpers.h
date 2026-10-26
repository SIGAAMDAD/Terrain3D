// Shared helpers for Terrain3D Godot resource types.
#ifndef TERRAIN3D_RESOURCE_HELPERS_H
#define TERRAIN3D_RESOURCE_HELPERS_H

// From assets/resources/terrain_3d_surface_format.h
#include "core/config/terrain_3d_compiler.h"
#include <godot_cpp/classes/image.hpp>
EA_FORCE_INLINE bool t3dIsPackedSurfaceFormat(
	const godot::Image::Format format ) {
	return format == godot::Image::FORMAT_RGBA8 || format == godot::Image::FORMAT_DXT5 ||
		   format == godot::Image::FORMAT_BPTC_RGBA;
}

// From assets/resources/terrain_3d_resource_signal.h
#include "core/config/terrain_3d_compiler.h"
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/callable.hpp>

// Avoid a connection-count change when a resource is already wired.
template< typename TResource >
EA_FORCE_INLINE void t3dConnectResourceChanged(
	const godot::Ref< TResource > &rSource,
	const godot::Callable		  &rCallback,
	const bool					   bConnect ) {
	if ( rSource.is_null() ) {
		return;
	}
	const bool connected = rSource->is_connected( "changed", rCallback );
	if ( bConnect && !connected ) {
		rSource->connect( "changed", rCallback );
	} else if ( !bConnect && connected ) {
		rSource->disconnect( "changed", rCallback );
	}
}

// From assets/resources/terrain_3d_vt_scalar_packing.h
#include "Terrain3DTypes.h"
#include "rendering/rd/vt/core/terrain_3d_vt_contract.h"
#include "core/utility/terrain_3d_numeric.h"

EA_FORCE_INLINE uint32_t t3dMaterialPackVtScalars0(
	const float fMetallic,
	const float fRoughness,
	const float fAo,
	const float fAlphaScissor ) {
	return ( t3dPackUnorm8Nearest( fMetallic ) << T3D_VT_MATERIAL_METALLIC_SHIFT ) |
		   ( t3dPackUnorm8Nearest( fRoughness ) << T3D_VT_MATERIAL_ROUGHNESS_SHIFT ) |
		   ( t3dPackUnorm8Nearest( fAo ) << T3D_VT_MATERIAL_AO_SHIFT ) |
		   ( t3dPackUnorm8Nearest( fAlphaScissor ) << T3D_VT_MATERIAL_ALPHA_SCISSOR_SHIFT );
}

#endif // TERRAIN3D_RESOURCE_HELPERS_H
