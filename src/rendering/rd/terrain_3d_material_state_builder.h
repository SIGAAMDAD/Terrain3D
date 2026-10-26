// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_MATERIAL_STATE_BUILDER_H
#define TERRAIN3D_MATERIAL_STATE_BUILDER_H

#include "assets/generated_texture.h"
#include "rendering/rd/terrain_3d_rd_types.h"

class Terrain3DRenderCoordinator;
class Terrain3DDiagnosticsPublisher;
class Terrain3DMaterial;
class Terrain3DData;
class Terrain3DAssets;

class Terrain3DMaterialStateBuilder {
	friend class Terrain3DDiagnosticsPublisher;
	CLASS_NAME_STATIC( "Terrain3DMaterialStateBuilder" );

private:
	Terrain3DGeneratedTexture m_worldNoiseCache;
	Terrain3DGeneratedTexture m_macroNoiseTexture;
	Terrain3DGeneratedTexture m_editorBrushTexture;
	uint64_t m_nMacroNoiseSourceId = 0u;
	uint64_t m_nEditorBrushSourceId = 0u;
	uint64_t m_nMaterialGeneration = 0u;
	uint64_t m_nMaterialSourceGeneration = 0u;
	t3dRdMaterialLayer_t m_cachedMaterialLayers[s_nRdMaterialLayerCount]{};
	t3dRdMaterialConfig_t m_cachedMaterialConfig{};
	uint32_t m_nCachedMaterialLayerCount = 1u;
	bool m_bMaterialStateInitialized = false;
	uint64_t m_nMaterialStateBuildCount = 0u;

	bool ensureWorldNoiseCache( const Terrain3DMaterial *pMaterial );
	bool ensureMacroNoiseTexture( const Terrain3DMaterial *pMaterial );
	bool ensureEditorBrushTexture( const Terrain3DMaterial *pMaterial );
	void buildEditorOverlayState(
	        Terrain3DRenderCoordinator &rCoordinator, t3dRdFrameSnapshot_t &rSnapshot, const Terrain3DMaterial *pMaterial );
	void buildMaterialFlags( Terrain3DRenderCoordinator &rCoordinator,
	        t3dRdFrameSnapshot_t &rSnapshot,
	        Terrain3DMaterial *pMaterial,
	        Terrain3DData *pData,
	        const Ref<Terrain3DAssets> &rAssets );
	void buildMaterialParameters( t3dRdMaterialConfig_t &rConfig, Terrain3DMaterial *pMaterial, const Ref<Terrain3DAssets> &rAssets );
	void buildMaterialTierConfig( t3dRdMaterialConfig_t &rConfig, Terrain3DMaterial *pMaterial );
	void buildMaterialLayers(
	        Terrain3DRenderCoordinator &rCoordinator, t3dRdFrameSnapshot_t &rSnapshot, const Ref<Terrain3DAssets> &rAssets );
	void buildMaterialState( Terrain3DRenderCoordinator &rCoordinator, t3dRdFrameSnapshot_t &rSnapshot );

public:
	bool publishSnapshotState( Terrain3DRenderCoordinator &rCoordinator, t3dRdFrameSnapshot_t &rSnapshot );
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
