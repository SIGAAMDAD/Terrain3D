// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MATERIAL_STATE_BUILDER_H
#define TERRAIN3D_MATERIAL_STATE_BUILDER_H

#include "core/utility/terrain_3d_owner.h"
#include "rendering/rd/vt/runtime/terrain_3d_virtual_texture.h"
#include "rendering/rd/vt/runtime/terrain_3d_terrain_megavt.h"

#include "assets/resources/generated_texture.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "data/model/terrain_3d_data.h"
#include "rendering/material/terrain_3d_material.h"


struct t3dMaterialStateSources_t {
	Terrain3DOwner *m_pTerrain;
	const Terrain3DVirtualTexture &m_virtualTexture;
	const Terrain3DTerrainMegaVT &m_terrainMegaVT;
};

class Terrain3DMaterialStateBuilder {
	CLASS_NAME_STATIC( "Terrain3DMaterialStateBuilder" );

private:
	Terrain3DGeneratedTexture m_worldNoiseCache;
	Terrain3DGeneratedTexture m_macroNoiseTexture;
	Terrain3DGeneratedTexture m_editorBrushTexture;
	uint64_t m_nMacroNoiseSourceId = 0u;
	uint64_t m_nEditorBrushSourceId = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nMaterialSourceGeneration = 0u;
	t3dRdMaterialLayer_t m_cachedMaterialLayers[T3D_RD_MATERIAL_LAYER_COUNT]{};
	t3dRdMaterialConfig_t m_cachedMaterialConfig{};
	uint32_t m_nCachedMaterialLayerCount = 1u;
	bool m_bMaterialStateInitialized = false;
	uint64_t m_nMaterialStateBuildCount = 0u;

	bool ensureWorldNoiseCache( const Terrain3DMaterial *pMaterial );
	bool ensureMacroNoiseTexture( const Terrain3DMaterial *pMaterial );
	bool ensureEditorBrushTexture( const Terrain3DMaterial *pMaterial );
	void buildEditorOverlayState(
	        const t3dMaterialStateSources_t &rSources, t3dRdFrameSnapshot_t &rSnapshot, const Terrain3DMaterial *pMaterial );
	void buildMaterialFlags( const t3dMaterialStateSources_t &rSources,
	        t3dRdFrameSnapshot_t &rSnapshot,
	        Terrain3DMaterial *pMaterial,
	        Terrain3DData *pData,
	        const Ref<Terrain3DAssets> &rAssets );
	void buildMaterialParameters( t3dRdMaterialConfig_t &rConfig, Terrain3DMaterial *pMaterial, const Ref<Terrain3DAssets> &rAssets );
	void buildMaterialTierConfig( t3dRdMaterialConfig_t &rConfig, Terrain3DMaterial *pMaterial );
	void buildMaterialLayers(
	        const t3dMaterialStateSources_t &rSources, t3dRdFrameSnapshot_t &rSnapshot, const Ref<Terrain3DAssets> &rAssets );
	void buildMaterialState( const t3dMaterialStateSources_t &rSources, t3dRdFrameSnapshot_t &rSnapshot );

public:
	bool publishSnapshotState( const t3dMaterialStateSources_t &rSources, t3dRdFrameSnapshot_t &rSnapshot );
	void resetPublicationState( void );
	void clear( void );
	RID getWorldNoiseRid( void ) const {
		return m_worldNoiseCache.getRid();
	}
	RID getMacroNoiseRid( void ) const {
		return m_macroNoiseTexture.getRid();
	}
	RID getEditorBrushRid( void ) const {
		return m_editorBrushTexture.getRid();
	}
	int getWorldNoiseMaxMipmapLevel( void ) const {
		return m_worldNoiseCache.maxMipmapLevel();
	}
	uint64_t getBuildCount( void ) const {
		return m_nMaterialStateBuildCount;
	}
};

#endif // TERRAIN3D_MATERIAL_STATE_BUILDER_H
