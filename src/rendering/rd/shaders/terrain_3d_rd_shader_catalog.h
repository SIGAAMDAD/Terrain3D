#ifndef TERRAIN3D_RD_SHADER_CATALOG_H
#define TERRAIN3D_RD_SHADER_CATALOG_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/shaders/terrain_3d_rd_material_vt_contract.h"
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

constexpr t3dRdShaderId_t t3dRdTerrainCompactFeedbackId( uint32_t tier, uint32_t variant ) {
    return ( t3dRdShaderId_t )( T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE +
        tier * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
}

constexpr t3dRdShaderId_t t3dRdTerrainFeedbackId( uint32_t tier, uint32_t variant, bool oneSun ) {
	return ( t3dRdShaderId_t ) ( ( oneSun ? T3D_RD_SHADER_TERRAIN_FEEDBACK_ONE_SUN_BASE : T3D_RD_SHADER_TERRAIN_FEEDBACK_BASE ) +
										  tier * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
}

// SHD-01 canonical route declaration. A route with m_bAvailable == false has
// no legal shader version in the current catalog; SHD-02 must add/merge the
// missing compact+feedback combination before any consumer can select it.
typedef struct t3dRdCanonicalShaderRoute_s
{
    t3dRdShaderId_t m_id;
    uint32_t m_nMaterialVtSet;
    uint32_t m_nFeedbackSet;
    bool m_bAvailable;
} t3dRdCanonicalShaderRoute_t;

constexpr t3dRdCanonicalShaderRoute_t t3dRdCanonicalTerrainRoute(
    uint32_t path, uint32_t tier, uint32_t variant, bool feedback, bool oneSun, bool compact ) {
    if ( path >= T3D_TERRAIN_SHADER_PATH_COUNT || tier >= 3u ||
        variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ||
        ( ( feedback || oneSun || compact ) && variant >= T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) ||
        ( feedback && path != T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL ) ||
        ( compact && oneSun ) ) {
        return { T3D_RD_SHADER_COUNT, T3D_RD_TERRAIN_VT_DESCRIPTOR_SET, 0u, false };
    }
    if ( feedback ) {
        return { compact ? t3dRdTerrainCompactFeedbackId( tier, variant ) :
            t3dRdTerrainFeedbackId( tier, variant, oneSun ),
            T3D_RD_TERRAIN_VT_DESCRIPTOR_SET, T3D_RD_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET, true };
    }
    if ( compact ) {
        return { ( t3dRdShaderId_t )( T3D_RD_SHADER_TERRAIN_COMPACT_BASE +
            ( path * 3u + tier ) * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant ),
            T3D_RD_TERRAIN_VT_DESCRIPTOR_SET, 0u, true };
    }
    return { oneSun ? t3dRdTerrainOneSunId( path, tier, variant ) :
        t3dRdTerrainForwardId( path, tier, variant ),
        T3D_RD_TERRAIN_VT_DESCRIPTOR_SET, 0u, true };
}

// Terrain strips and WORLD mesh share evaluator code but use distinct shader
// entrypoints and set-1 VT bindings. Environment receivers remain separate
// pass/capability variants without changing authored materials.
constexpr t3dRdCanonicalShaderRoute_t t3dRdCanonicalWorldRoute(
    bool terrainStrip, uint32_t variant, bool feedback, bool compact, bool environment ) {
    if ( variant >= T3D_RD_FORWARD_SHADER_VARIANT_MAX ||
        ( ( feedback || compact ) && variant >= T3D_RD_FORWARD_COLOR_VARIANT_COUNT ) ||
        ( feedback && ( compact || terrainStrip ) ) ) {
        return { T3D_RD_SHADER_COUNT, T3D_RD_WORLD_VT_DESCRIPTOR_SET, 0u, false };
    }
    if ( feedback ) {
        return { ( t3dRdShaderId_t )( ( environment ? T3D_RD_SHADER_WORLD_MESH_ENV_FEEDBACK_BASE :
            T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE ) + variant ),
            T3D_RD_WORLD_VT_DESCRIPTOR_SET, T3D_RD_VT_FRAGMENT_FEEDBACK_DESCRIPTOR_SET, true };
    }
    if ( compact ) {
        return { ( t3dRdShaderId_t )( ( terrainStrip ?
            ( environment ? T3D_RD_SHADER_WORLD_STRIP_ENV_COMPACT_BASE : T3D_RD_SHADER_WORLD_STRIP_COMPACT_BASE ) :
            ( environment ? T3D_RD_SHADER_WORLD_MESH_ENV_COMPACT_BASE : T3D_RD_SHADER_WORLD_MESH_COMPACT_BASE ) ) + variant ),
            T3D_RD_WORLD_VT_DESCRIPTOR_SET, 0u, true };
    }
    return { ( t3dRdShaderId_t )( ( terrainStrip ?
        ( environment && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT ? T3D_RD_SHADER_WORLD_STRIP_ENV_BASE :
            T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE ) :
        ( environment ? T3D_RD_SHADER_WORLD_MESH_ENV_BASE : T3D_RD_SHADER_WORLD_RASTER_MESH_BASE ) ) + variant ),
        T3D_RD_WORLD_VT_DESCRIPTOR_SET, 0u, true };
}

constexpr t3dRdShaderId_t t3dRdCanonicalWorldShadowId( bool terrainStrip, bool environment ) {
    return terrainStrip ? T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW :
        ( environment ? T3D_RD_SHADER_WORLD_MESH_ENV_SHADOW : T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW );
}

static_assert( t3dRdCanonicalTerrainRoute( 0u, 0u, T3D_RD_FORWARD_SHADER_COLOR, false, false, false ).m_id ==
    T3D_RD_SHADER_TERRAIN_FORWARD_BASE, "Canonical terrain shader ID changed" );
static_assert( t3dRdCanonicalTerrainRoute( 0u, 0u, T3D_RD_FORWARD_SHADER_COLOR,
    true, false, true ).m_id == T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE,
    "Compact terrain feedback ABI route changed" );
static_assert( t3dRdCanonicalWorldRoute( false, T3D_RD_FORWARD_SHADER_COLOR, true, false, false ).m_id ==
    T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE, "WORLD feedback shader ID changed" );

// The hint shaders use the previously allocated VT variant slots. The
// reserved slots are never selected and do not have shader catalog entries.
constexpr t3dRdShaderId_t t3dRdCanonicalVtColorHintId( uint32_t tier ) {
    return ( t3dRdShaderId_t )( T3D_RD_SHADER_TERRAIN_VT_COLOR_HINT_BASE +
        tier * T3D_RD_VT_COLOR_HINT_ID_STRIDE + 2u );
}
constexpr t3dRdShaderId_t t3dRdCanonicalVtFeedbackHintId( uint32_t tier ) {
    return ( t3dRdShaderId_t )( T3D_RD_SHADER_TERRAIN_VT_FEEDBACK_HINT_BASE +
        tier * T3D_RD_VT_FEEDBACK_HINT_VARIANT_COUNT + 1u );
}

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
