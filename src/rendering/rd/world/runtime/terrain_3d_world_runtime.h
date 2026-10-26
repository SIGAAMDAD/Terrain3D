// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_RUNTIME_H
#define TERRAIN3D_WORLD_RUNTIME_H

#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/world/visibility/terrain_3d_world_hiz.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include "rendering/rd/world/visibility/terrain_3d_world_raster.h"
#include "rendering/rd/world/visibility/terrain_3d_world_visibility_gpu.h"

class Terrain3DWorldRuntime
{
private:
	Terrain3DWorldResources		m_resources;
	Terrain3DWorldHiZ			m_hiZ;
	Terrain3DWorldVisibilityGpu m_visibility;
	Terrain3DWorldRaster		m_raster;
	t3dWorldResourceLimits_t	m_limits{};
	uint64_t					m_nPreparedPublicationGeneration = 0u;
	uint64_t m_nPreparedGeometryCacheEpoch = 0u;
	uint64_t					m_nResolvedVtPackagePublicationGeneration = UINT64_MAX;
	uint64_t					m_nGeometryGeneration = 0u;
	uint64_t					m_nMaterialGeneration = 0u;
	uint64_t					m_nCurrentVisibilityPublicationGeneration = 0u;
	uint64_t					m_nPreviousVisibilityPublicationGeneration = 0u;
	uint64_t					m_nCurrentVisibilityTerrainGeneration = 0u;
	uint64_t					m_nPreviousVisibilityTerrainGeneration = 0u;
	uint64_t					m_nCurrentVisibilityRenderFrame = UINT64_MAX;
	uint64_t					m_nPreviousVisibilityRenderFrame = UINT64_MAX;
	uint64_t					m_nHiZBuildRenderFrame = UINT64_MAX;
	uint64_t					m_nHiZBuildViewportKey = 0u;
	uint64_t					m_nPrepareCount = 0u;
	uint64_t					m_nUploadCount = 0u;
	uint64_t					m_nPrepareFailureCount = 0u;
	uint64_t					m_nVisibilityPrepareCount = 0u;
	uint64_t					m_nHiZInvalidHistoryFrames = 0u;
	uint64_t					m_nPublicationUploadBudgetBytes = 4u * 1024u * 1024u;
	uint64_t					m_nPublicationUploadCreditBytes = 0u;
	uint64_t					m_nPublicationBudgetFrame = UINT64_MAX;
	uint64_t					m_nBudgetCheckedPublicationGeneration = 0u;
	uint64_t m_nBudgetCheckedGeometryCacheEpoch = 0u;
	uint64_t					m_nBudgetCheckedVtGeneration = UINT64_MAX;
	uint64_t					m_nLastRequiredUploadBytes = 0u;
	uint32_t					m_nLastRequiredClusterCredit = 0u;
	uint64_t					m_nPublicationBudgetDeferralCount = 0u;
	uint64_t					m_nPublicationBoundaryDeferralCount = 0u;
	uint32_t					m_nPublicationClusterBudget = 8192u;
	uint32_t					m_nPublicationClusterCredit = 0u;
	bool						m_bPublicationBoundaryDeferred = false;
	uint32_t					m_nClusterCount = 0u;
	uint32_t					m_nTerrainStripClusterCount = 0u;
	uint32_t					m_nTransformCount = 0u;
	uint32_t					m_nMaterialCount = 0u;
	uint32_t					m_nVisibilityCandidateCount = 0u;
	bool						m_bInitialized = false;
	bool						m_bReady = false;
	// Static stage name, retained through failure cleanup for startup reporting.
	const char *m_pszLastInitializationFailure = nullptr;

	void buildFrame( const t3dRdForwardPassContext_t &rContext, bool bHiZValid, t3dWorldFrame_t &rFrame ) const;
	bool temporalHiZSafe( const t3dRdForwardPassContext_t &rContext ) const;

public:
	Terrain3DWorldRuntime();
    void discardSuperTerrainCandidateCold( void );
    bool uploadSuperTerrainCandidateCold( const Terrain3DStPublicationImage &rCandidate,
                                           uint32_t nFinePageSlots );
	bool configureComposedExperimentalCold( bool bEnable );
	bool isComposedExperimentalEnabled() const { return m_raster.isComposedExperimentalEnabled(); }
	bool		initializeCold( bool bPersistentFrameBuffer, bool bPersistentCpuBuffers );
	const char *getLastInitializationFailure() const {
		return m_pszLastInitializationFailure;
	}
	bool	 configureLimitsCold( const t3dWorldResourceLimits_t &rLimits );
	void	 setFramePolicyHot( uint64_t nUploadBudgetBytes, uint32_t nClusterBudget, bool bBoundaryDeferred );
	bool	 preparePublicationCold( const Terrain3DWorldPublication &rPublication,
									 uint64_t						  nRenderFrame,
									 const Terrain3DVirtualTexture	 *pVirtualTexture = NULL );
	bool	 prepareVisibilityPass( const t3dRdForwardPassContext_t &rContext,
									const t3dRdFrameSnapshot_t		&rSnapshot,
									uint32_t						 nVisibilityMask );
	bool	 prepareRasterPass( const t3dRdForwardPassContext_t			&rContext,
								const t3dRdWorldTerrainRasterBindings_t &rTerrain,
								const t3dSharedVTBindings_t				&rVT,
								Terrain3DVTGpu							*pFeedbackOwner = NULL,
	const t3dComposedSurfaceGpuBindings_t *pComposed = NULL,
    const t3dRdEnvironmentBindings_t *pEnvironment = NULL );
	bool	 submitRasterPass( const t3dRdForwardPassContext_t &rContext );
	void	 shutdownCold( void );
	void releaseArenaSpansFinal( void );
	bool	 isReady( void ) const;
	uint64_t getPreparedPublicationGeneration( void ) const;
	uint64_t getPrepareCount( void ) const;
	uint64_t getUploadCount( void ) const;
	uint64_t getPrepareFailureCount( void ) const;
	uint64_t getVisibilityPrepareCount( void ) const;
	uint64_t getHiZInvalidHistoryFrames( void ) const;
	uint64_t getPublicationBudgetDeferralCount( void ) const;
	uint64_t getPublicationBoundaryDeferralCount( void ) const;
	uint64_t getPublicationUploadBudgetBytes( void ) const;
	uint64_t getLastRequiredUploadBytes(
		void ) const {
		return m_nLastRequiredUploadBytes;
	}
	uint32_t getLastRequiredClusterCredit(
		void ) const {
		return m_nLastRequiredClusterCredit;
	}
	uint32_t						   getPublicationClusterBudget( void ) const;
	const t3dWorldResourceLimits_t	  &getLimits( void ) const;
	Terrain3DWorldResources			  &getResources( void );
	const Terrain3DWorldResources	  &getResources( void ) const;
	const Terrain3DWorldHiZ			  &getHiZ( void ) const;
	const Terrain3DWorldVisibilityGpu &getVisibility( void ) const;
	const Terrain3DWorldRaster		  &getRaster( void ) const;
	size_t							   getRetainedCpuBytes( void ) const;
};

#endif // TERRAIN3D_WORLD_RUNTIME_H
