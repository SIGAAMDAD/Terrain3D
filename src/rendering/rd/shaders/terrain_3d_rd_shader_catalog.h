#ifndef TERRAIN3D_RD_SHADER_CATALOG_H
#define TERRAIN3D_RD_SHADER_CATALOG_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/material/terrain_3d_shader_specializer.h"


// Stable IDs describe shader source and version independently of any device.
// A physical shader RID belongs to exactly one RenderingDevice generation.
// t3dRdShaderId_e centralized in Terrain3DTypes.h.


// t3dRdShaderDomain_e centralized in Terrain3DTypes.h.


// t3dRdShaderCategory_e centralized in Terrain3DTypes.h.


// t3dRdShaderCatalogEntry_s centralized in Terrain3DTypes.h.


constexpr t3dRdShaderId_t t3dRdTerrainForwardId( uint32_t path, uint32_t tier, uint32_t variant ) {
	return ( t3dRdShaderId_t )( T3D_RD_SHADER_TERRAIN_FORWARD_BASE
	        + ( path * 3u + tier ) * T3D_RD_FORWARD_SHADER_VARIANT_MAX + variant );
}

constexpr t3dRdShaderId_t t3dRdTerrainOneSunId( uint32_t path, uint32_t tier, uint32_t variant ) {
	return ( t3dRdShaderId_t )( T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE
	        + ( path * 3u + tier ) * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
}

t3dRdShaderCatalogEntry_t t3dRdShaderCatalogGet( t3dRdShaderId_t id );

// Pass-specific raster variants are acquired during PREPARE. Startup only needs
// the shared raster aliases, compute programs and fixed shadow programs.
constexpr bool t3dRdShaderNeedsStartupPhysical( const t3dRdShaderId_t id )
{
	return id >= T3D_RD_SHADER_NEAR_ALIAS && id < T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE && !( id >= T3D_RD_SHADER_INSTANCE_FORWARD_BASE && id < T3D_RD_SHADER_INSTANCE_ALIAS ) && !( id >= T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE && id < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ) && id != T3D_RD_SHADER_BRUSH_COMPUTE && id != T3D_RD_SHADER_DERIVED_COMPUTE;
}

uint32_t t3dRdShaderCatalogGlobalSourceCount();
const char *t3dRdShaderCatalogGlobalSourcePath( uint32_t index );
uint32_t t3dRdShaderCatalogSourceCount();
const char *t3dRdShaderCatalogSourcePath( uint32_t index );

#endif // TERRAIN3D_RD_SHADER_CATALOG_H
