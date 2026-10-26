#ifndef TERRAIN3D_RD_SHADER_CATALOG_H
#define TERRAIN3D_RD_SHADER_CATALOG_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/shaders/terrain_3d_rd_material_vt_contract.h"
#include "rendering/material/terrain_3d_shader_specializer.h"

// Stable IDs describe shader source and version independently of any device.
// A physical shader RID belongs to exactly one RenderingDevice generation.

// UCMP-04/05: one cooked-material raster shader for every clipmap geometry group.
// Variant identity depends on pass, lighting ABI and material path only.
// The existing shader ID ranges remain append-stable for WORLD and legacy consumers.
constexpr t3dRdShaderId_t t3dRdUnifiedTerrainId(
    uint32_t path, uint32_t variant, bool feedback, bool oneSun, bool compact ) {
    if ( feedback ) {
        return ( t3dRdShaderId_t )( ( compact ? T3D_RD_SHADER_UCMP_FEEDBACK_COMPACT_BASE :
            ( oneSun ? T3D_RD_SHADER_UCMP_FEEDBACK_ONE_SUN_BASE : T3D_RD_SHADER_UCMP_FEEDBACK_BASE ) ) + variant );
    }
    if ( oneSun ) {
        return ( t3dRdShaderId_t )( T3D_RD_SHADER_UCMP_ONE_SUN_BASE +
            path * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
    }
    if ( compact ) {
        return ( t3dRdShaderId_t )( T3D_RD_SHADER_UCMP_COMPACT_BASE +
            path * T3D_RD_FORWARD_COLOR_VARIANT_COUNT + variant );
    }
    return ( t3dRdShaderId_t )( T3D_RD_SHADER_UCMP_FORWARD_BASE +
        path * T3D_RD_FORWARD_SHADER_VARIANT_MAX + variant );
}

static_assert( T3D_RD_SHADER_UCMP_ALIAS == T3D_RD_SHADER_TERRAIN_COMPACT_FEEDBACK_BASE +
    3u * T3D_RD_FORWARD_COLOR_VARIANT_COUNT, "UCMP shifted an existing shader ID" );
static_assert( t3dRdUnifiedTerrainId( 0u, T3D_RD_FORWARD_SHADER_COLOR, false, false, false ) ==
    T3D_RD_SHADER_UCMP_FORWARD_BASE, "UCMP material shader root ID mismatch" );
static_assert( t3dRdUnifiedTerrainId( 0u, T3D_RD_FORWARD_SHADER_COLOR, true, false, false ) ==
    T3D_RD_SHADER_UCMP_FEEDBACK_BASE, "UCMP feedback shader ID mismatch" );

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

static_assert( t3dRdCanonicalWorldRoute( false, T3D_RD_FORWARD_SHADER_COLOR, true, false, false ).m_id ==
    T3D_RD_SHADER_WORLD_MESH_FEEDBACK_BASE, "WORLD feedback shader ID changed" );

t3dRdShaderCatalogEntry_t t3dRdShaderCatalogGet( t3dRdShaderId_t id );
// Stable numeric ID values are retained for external ABI safety; only active
// catalog IDs are enumerated and can be acquired by the shader library.
bool t3dRdShaderCatalogIsActive( t3dRdShaderId_t id );
uint32_t t3dRdShaderCatalogActiveCount();
t3dRdShaderId_t t3dRdShaderCatalogActiveIdAt( uint32_t index );

// Pass-specific raster variants are acquired during PREPARE. Startup only needs
// the shared raster aliases, compute programs and fixed shadow programs.
constexpr bool t3dRdShaderNeedsStartupPhysical(
    const t3dRdShaderId_t id ) {
    if ( id == T3D_RD_SHADER_UCMP_ALIAS ) return true;
    if ( id < T3D_RD_SHADER_INSTANCE_FORWARD_BASE ) return false;
    return id < T3D_RD_SHADER_INSTANCE_FEEDBACK_BASE &&
           !( id >= T3D_RD_SHADER_INSTANCE_FORWARD_BASE && id < T3D_RD_SHADER_INSTANCE_ALIAS ) &&
           !( id >= T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE && id < T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW );
}

uint32_t	t3dRdShaderCatalogGlobalSourceCount();
const char *t3dRdShaderCatalogGlobalSourcePath( uint32_t index );
const char *t3dRdVisibilityBakeSourcePath();
uint32_t	t3dRdShaderCatalogSourceCount();
const char *t3dRdShaderCatalogSourcePath( uint32_t index );

#endif // TERRAIN3D_RD_SHADER_CATALOG_H
