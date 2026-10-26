#ifndef TERRAIN3D_DIAGNOSTIC_INPUTS_H
#define TERRAIN3D_DIAGNOSTIC_INPUTS_H
#include "core/utility/terrain_3d_owner.h"
#include "rendering/rd/core/terrain_3d_render_lifecycle_state.h"
#include "rendering/rd/forward/terrain_3d_forward_pass_preparer.h"
#include "rendering/rd/preview/terrain_3d_rd_preview_service.h"
#include "rendering/rd/material/terrain_3d_material_state_builder.h"
#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/world/runtime/terrain_3d_world_runtime.h"
// Formatting inputs; subsystem mutation remains with its owner.
struct t3dDiagnosticInputs_t {
	const bool &m_bCompositorDebugEnabled;
	const bool &m_bFailureLatched;
	const bool &m_bForwardBridgeAttached;
	const t3dAtomicBool_t &m_bInstanceRendererInitialized;
	const bool &m_bMotionDebugEnabled;
	const t3dAtomicBool_t &m_bOceanRendererInitialized;
	const t3dAtomicBool_t &m_bRenderEnabled;
	const t3dAtomicBool_t &m_bRendererInitialized;
	Terrain3DMutex &m_diagnosticsMutex;
	const Terrain3DForwardPassPreparer &m_forwardPassPreparer;
	const String &m_initializationError;
	const Terrain3DRdInstanceRenderer &m_instanceRenderer;
	const Terrain3DMaterialStateBuilder &m_materialStateBuilder;
	const t3dAtomicU64_t &m_nCallbackCount;
	const t3dAtomicU32_t &m_nConsecutiveRenderFailures;
	const uint64_t &m_nInstanceInitializationAttempts;
	const uint64_t &m_nInstanceInitializationFailures;
	const uint64_t &m_nInstanceInitializationRecoveries;
	const uint64_t &m_nRendererRecoveryCount;
	const Terrain3DRdOceanRenderer &m_oceanRenderer;
	Terrain3DOwner * const &m_pTerrain;
	const Terrain3DRdPreviewService &m_previewService;
	const Terrain3DRenderPublicationBuilder &m_publicationBuilder;
	const Terrain3DRdClipmapRenderer &m_renderer;
	const Terrain3DRdShaderLibrary &m_shaderLibrary;
	const Terrain3DSharedVTRuntime &m_sharedVtRuntime;
	const Terrain3DTerrainMegaVT &m_terrainMegaVT;
	const Terrain3DVirtualTexture &m_virtualTexture;
	const Terrain3DWorldRuntime &m_worldRuntime;
	t3dRenderLifecycleStatus_t m_lifecycle;
	const t3dSharedVTBindings_t &m_sharedBindings;
	bool m_editorAuthoring;
	bool m_forwardBridgeReady;
};
#endif // TERRAIN3D_DIAGNOSTIC_INPUTS_H
