// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_FORWARD_PASS_SERVICES_H
#define TERRAIN3D_FORWARD_PASS_SERVICES_H

#include "rendering/rd/core/terrain_3d_render_publication_builder.h"
#include "rendering/rd/clipmap/terrain_3d_rd_clipmap_renderer.h"
#include "rendering/rd/instances/terrain_3d_rd_instance_renderer.h"
#include "rendering/rd/ocean/terrain_3d_rd_ocean_renderer.h"
#include "rendering/rd/world/runtime/terrain_3d_world_runtime.h"
#include "rendering/rd/vt/runtime/terrain_3d_virtual_texture.h"

// Lifecycle operations remain with the coordinator. The preparer cannot reach
// owner-thread scene state or control-plane storage through this contract.
class Terrain3DForwardPassHost {
public:
	virtual bool initializeForwardRenderer( uint64_t nFrame ) = 0;
	virtual const t3dRdFrameSnapshotSlot_t *acquireForwardSnapshot( void ) = 0;
	virtual bool ensureForwardSharedVt( void ) = 0;
	virtual bool snapshotForwardSharedVt( t3dSharedVTBindings_t &rBindings ) = 0;
	virtual void publishForwardWorldReadiness( const Terrain3DWorldPublication &rPublication, bool bReady ) = 0;

protected:
	~Terrain3DForwardPassHost( void ) = default;
};

// Borrowed render-thread services for one callback. Policy is captured once;
// renderer lifetime and mutable status stay with the composition root.
typedef struct t3dForwardPassServices_s {
	Terrain3DRenderPublicationBuilder &m_publicationBuilder;
	Terrain3DRdClipmapRenderer &m_renderer;
	Terrain3DRdInstanceRenderer &m_instanceRenderer;
	Terrain3DRdOceanRenderer &m_oceanRenderer;
	Terrain3DWorldRuntime &m_worldRuntime;
	Terrain3DVirtualTexture &m_virtualTexture;
	t3dAtomicBool_t &m_bRenderEnabled;
	t3dAtomicBool_t &m_bRendererInitialized;
	t3dAtomicBool_t &m_bInstanceRendererInitialized;
	t3dAtomicBool_t &m_bOceanRendererInitialized;
	t3dAtomicBool_t &m_bRendererFailed;
	t3dAtomicU32_t &m_nConsecutiveRenderFailures;
	uint32_t m_nWorldUploadBudgetKbPolicy;
	uint32_t m_nWorldClusterBudgetPolicy;
	bool m_bWorldBoundaryDeferredPolicy;
	uint32_t m_nVtRequestBudgetPolicy;
	uint32_t m_nVtAdmissionBudgetPolicy;
} t3dForwardPassServices_t;

#endif // TERRAIN3D_FORWARD_PASS_SERVICES_H
