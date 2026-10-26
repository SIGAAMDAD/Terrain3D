#ifndef TERRAIN3D_RD_SHADER_CATALOG_H
#define TERRAIN3D_RD_SHADER_CATALOG_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/material/terrain_3d_shader_specializer.h"

// Stable IDs describe shader source and version independently of any device.
// A physical shader RID belongs to exactly one RenderingDevice generation.

constexpr t3dRdShaderId_t t3dRdTerrainForwardId(
	uint32_t path,
	uint32_t tier,
	uint32_t variant ) {
	return ( t3dRdShaderId_t ) ( T3D_RD_SHADER_TERRAIN_FORWARD_BASE +
								 ( path * 3u + tier ) * T3D_RD_FORWARD_SHADER_VARIANT_MAX + variant );
}

constexpr t3dRdShaderId_t t3dRdTerrainOneSunId(
	uint32_t path,
	uint32_t tier,
	uint32_t variant ) {
	return ( t3dRdShaderId_t ) ( T3D_RD_SHADER_TERRAIN_ONE_SUN_BASE +
								 ( path * 3u + tier ) * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
}

constexpr t3dRdShaderId_t t3dRdTerrainFeedbackId( uint32_t tier, uint32_t variant, bool oneSun ) {
	return ( t3dRdShaderId_t ) ( ( oneSun ? T3D_RD_SHADER_TERRAIN_FEEDBACK_ONE_SUN_BASE : T3D_RD_SHADER_TERRAIN_FEEDBACK_BASE ) +
										  tier * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
}

// Material shader trial index is one-based; zero selects the original shader.
// Index 255 requests the bounded automatic terrain trial selection.
constexpr t3dRdShaderId_t t3dRdMaterialTrialId( uint32_t tier, uint32_t trial ) {
	return ( t3dRdShaderId_t ) ( T3D_RD_SHADER_MATERIAL_TRIAL_BASE + tier * T3D_RD_MATERIAL_SHADER_TRIAL_COUNT + trial - 1u );
}
constexpr t3dRdShaderId_t t3dRdAlphaSamplingTrialId( bool worldMesh, uint32_t trial ) {
	return ( t3dRdShaderId_t ) ( ( worldMesh ? T3D_RD_SHADER_WORLD_ALPHA_TRIAL_BASE : T3D_RD_SHADER_INSTANCE_ALPHA_TRIAL_BASE ) + trial - 1u );
}
constexpr t3dRdShaderId_t t3dRdVtSamplingTrialId( uint32_t tier, uint32_t trial ) {
	return ( t3dRdShaderId_t ) ( T3D_RD_SHADER_VT_SAMPLING_TRIAL_BASE + tier * T3D_RD_VT_SAMPLING_TRIAL_COUNT + trial - 1u );
}
constexpr t3dRdShaderId_t t3dRdTerrainVtFeedbackHintId( uint32_t tier, uint32_t trial ) {
	// Only VT trials 2 (resident hints) and 3 (hints + prepared mip bias).
	return ( t3dRdShaderId_t ) ( T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE +
		tier * T3D_RD_VT_FEEDBACK_HINT_VARIANT_COUNT + trial - 2u );
}
const char *t3dRdVtSamplingTrialName( uint32_t trial );
const char *t3dRdMaterialTrialName( uint32_t trial );
const char *t3dRdAlphaSamplingTrialName( uint32_t trial );

t3dRdShaderCatalogEntry_t t3dRdShaderCatalogGet( t3dRdShaderId_t id );

// Pass-specific raster variants are acquired during PREPARE. Startup only needs
// the shared raster aliases, compute programs and fixed shadow programs.
constexpr bool t3dRdShaderNeedsStartupPhysical(
	const t3dRdShaderId_t id ) {
	return id >= T3D_RD_SHADER_NEAR_ALIAS && id < T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE &&
		   !( id >= T3D_RD_SHADER_INSTANCE_FORWARD_BASE && id < T3D_RD_SHADER_INSTANCE_ALIAS ) &&
		   !( id >= T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE && id < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW ) &&
		   id != T3D_RD_SHADER_BRUSH_COMPUTE && id != T3D_RD_SHADER_DERIVED_COMPUTE;
}

uint32_t	t3dRdShaderCatalogGlobalSourceCount();
const char *t3dRdShaderCatalogGlobalSourcePath( uint32_t index );
const char *t3dRdVisibilityBakeSourcePath();
uint32_t	t3dRdShaderCatalogSourceCount();
const char *t3dRdShaderCatalogSourcePath( uint32_t index );

#endif // TERRAIN3D_RD_SHADER_CATALOG_H
