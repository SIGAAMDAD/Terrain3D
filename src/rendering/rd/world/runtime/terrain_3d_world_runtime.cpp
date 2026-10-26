// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "rendering/rd/world/runtime/terrain_3d_world_runtime.h"
#include "rendering/rd/instances/terrain_3d_rd_frustum.h"

#include <math.h>
#include <string.h>

using namespace godot;

static void t3dWorldCopyProjection( const Projection &rProjection, float *pDestination ) {
	for ( uint32_t column = 0u; column < 4u; ++column ) {
		const Vector4 v = rProjection[column];
		pDestination[column * 4u + 0u] = ( float )v.x;
		pDestination[column * 4u + 1u] = ( float )v.y;
		pDestination[column * 4u + 2u] = ( float )v.z;
		pDestination[column * 4u + 3u] = ( float )v.w;
	}
}

static bool t3dWorldProjectionNearEqual( const Projection &a, const Projection &b ) {
	for ( uint32_t column = 0u; column < 4u; ++column ) {
		const Vector4 av = a[column], bv = b[column];
		if ( fabs( av.x - bv.x ) > 1.0e-5 || fabs( av.y - bv.y ) > 1.0e-5 || fabs( av.z - bv.z ) > 1.0e-5
		        || fabs( av.w - bv.w ) > 1.0e-5 ) {
			return false;
		}
	}
	return true;
}

Terrain3DWorldRuntime::Terrain3DWorldRuntime() :
        m_hiZ( Terrain3DRdContext::getShared() ),
        m_visibility( Terrain3DRdContext::getShared() ),
        m_raster( Terrain3DRdContext::getShared() ) {
	m_limits.m_nHiZMaxWidth = 2048u;
	m_limits.m_nHiZMaxHeight = 2048u;
	m_limits.m_nHiZMaxMipCount = 12u;
}

bool Terrain3DWorldRuntime::configureLimitsCold( const t3dWorldResourceLimits_t &rLimits ) {
	if ( m_bInitialized || rLimits.m_nMaxClusters == 0u || rLimits.m_nMaxTerrainClusters == 0u || rLimits.m_nMaxTransforms == 0u
	        || rLimits.m_nMaxVisibleItems == 0u || rLimits.m_nMaxMaterials == 0u || rLimits.m_nHiZMaxWidth == 0u
	        || rLimits.m_nHiZMaxHeight == 0u || rLimits.m_nHiZMaxMipCount == 0u ) {
		return false;
	}
	m_limits = rLimits;
	return true;
}

void Terrain3DWorldRuntime::setFramePolicyHot(
        const uint64_t nUploadBudgetBytes, const uint32_t nClusterBudget, const bool bBoundaryDeferred ) {
	m_nPublicationUploadBudgetBytes = MAX( 1ull, nUploadBudgetBytes );
	m_nPublicationClusterBudget = MAX( 1u, nClusterBudget );
	m_bPublicationBoundaryDeferred = bBoundaryDeferred;
}


static bool t3dWorldPublicationMaterialReferencesValid( const Terrain3DWorldPublication &rPublication ) {
	const uint32_t nMaterialCount = ( uint32_t )rPublication.m_materials.size();
	for ( size_t i = 0u; i < rPublication.m_clusters.size(); ++i ) {
		const uint32_t nMaterial = rPublication.m_clusters[i].m_nMaterial;
		if ( nMaterial != T3D_WORLD_INVALID_ID && nMaterial >= nMaterialCount ) {
			return false;
		}
	}
	for ( size_t i = 0u; i < rPublication.m_instances.size(); ++i ) {
		const uint32_t nMaterial = rPublication.m_instances[i].m_nMaterialOverride;
		if ( nMaterial != T3D_WORLD_INVALID_ID && nMaterial >= nMaterialCount ) {
			return false;
		}
	}
	return true;
}

static bool t3dWorldPublicationFitsLimits( const Terrain3DWorldPublication &rPublication, const t3dWorldResourceLimits_t &rLimits ) {
	const size_t nStripTerrain = rPublication.m_terrainStripClusters.size();
	const size_t nActiveVisibleCapacity = rPublication.m_visibilityCandidates.size();
	return rPublication.m_vertices.size() <= rLimits.m_nMaxSourceVertices && rPublication.m_clusters.size() <= rLimits.m_nMaxClusters
	        && nStripTerrain <= rLimits.m_nMaxTerrainClusters
	        && rPublication.m_transforms.size() <= rLimits.m_nMaxTransforms
	        && nActiveVisibleCapacity <= rLimits.m_nMaxVisibleItems
	        && rPublication.m_materials.size() <= rLimits.m_nMaxMaterials
	        && rPublication.m_clusterVertexIndices.size() <= rLimits.m_nMaxClusterVertexIndices
	        && rPublication.m_triangleWords.size() <= rLimits.m_nMaxTriangleSlots;
}

bool Terrain3DWorldRuntime::initializeCold( const bool bPersistentFrameBuffer, const bool bPersistentCpuBuffers ) {
	if ( m_bInitialized ) { return true; }
	if ( !m_hiZ.initializeCold( m_limits ) || !m_visibility.initializeCold()
	        || !m_raster.initializeCold( bPersistentFrameBuffer, bPersistentCpuBuffers ) ) {
		shutdownCold();
		return false;
	}
	m_bInitialized = true;
	return true;
}

bool Terrain3DWorldRuntime::preparePublicationCold( const Terrain3DWorldPublication &rPublication, const uint64_t nRenderFrame ) {
	++m_nPrepareCount;
	if ( rPublication.m_nGeneration == 0u || !t3dWorldPublicationFitsLimits( rPublication, m_limits ) ) {
		m_bReady = false;
		++m_nPrepareFailureCount;
		return false;
	}
	if ( m_nPreparedPublicationGeneration != rPublication.m_nGeneration ) {
		if ( !t3dWorldPublicationMaterialReferencesValid( rPublication ) ) {
			m_bReady = false;
			++m_nPrepareFailureCount;
			return false;
		}
		const bool bHasStablePublication = m_nPreparedPublicationGeneration != 0u && m_bReady;
		if ( bHasStablePublication && m_bPublicationBoundaryDeferred ) {
			++m_nPublicationBoundaryDeferralCount;
			return true;
		}
		if ( m_nPublicationBudgetFrame != nRenderFrame ) {
			m_nPublicationBudgetFrame = nRenderFrame;
			m_nPublicationUploadCreditBytes =
			        MIN( UINT64_MAX - m_nPublicationUploadCreditBytes, m_nPublicationUploadBudgetBytes ) + m_nPublicationUploadCreditBytes;
			const uint64_t nClusterCredit = ( uint64_t )m_nPublicationClusterCredit + m_nPublicationClusterBudget;
			m_nPublicationClusterCredit = nClusterCredit > UINT32_MAX ? UINT32_MAX : ( uint32_t )nClusterCredit;
		}
		const uint64_t nRequiredBytes = Terrain3DWorldResources::estimatePublicationUploadBytes( rPublication );
		const uint64_t nRequiredClusters =
		        ( uint64_t )rPublication.m_clusters.size() + ( uint64_t )rPublication.m_terrainStripClusters.size()
	               ;
		if ( bHasStablePublication
		        && ( nRequiredBytes > m_nPublicationUploadCreditBytes || nRequiredClusters > m_nPublicationClusterCredit ) ) {
			++m_nPublicationBudgetDeferralCount;
			return true;
		}
		if ( !m_resources.ensureCapacityCold( rPublication ) || !m_resources.uploadPublicationWarm( rPublication ) ) {
			m_bReady = false;
			++m_nPrepareFailureCount;
			return false;
		}
		if ( bHasStablePublication ) {
			m_nPublicationUploadCreditBytes -= MIN( m_nPublicationUploadCreditBytes, nRequiredBytes );
			m_nPublicationClusterCredit -= MIN( m_nPublicationClusterCredit, ( uint32_t )MIN( nRequiredClusters, ( uint64_t )UINT32_MAX ) );
		}
		m_nPreparedPublicationGeneration = rPublication.m_nGeneration;
		m_nGeometryGeneration = rPublication.m_nGeometryGeneration;
		m_nMaterialGeneration = rPublication.m_nMaterialGeneration;
		m_nClusterCount = ( uint32_t )rPublication.m_clusters.size();
		m_nTerrainStripClusterCount = ( uint32_t )rPublication.m_terrainStripClusters.size();
		m_nTransformCount = ( uint32_t )rPublication.m_transforms.size();
		m_nMaterialCount = ( uint32_t )rPublication.m_materials.size();
		m_nVisibilityCandidateCount = ( uint32_t )rPublication.m_visibilityCandidates.size();
		++m_nUploadCount;
	}
	if ( m_bInitialized && !m_visibility.prepareBindingsCold( m_resources, m_hiZ ) ) {
		m_bReady = false;
		++m_nPrepareFailureCount;
		return false;
	}
	m_bReady = true;
	return true;
}

bool Terrain3DWorldRuntime::temporalHiZSafe( const t3dRdForwardPassContext_t &rContext ) const {
	return rContext.m_bTemporalHistoryValid && !rContext.m_bViewportReconfigured && rContext.m_nViewCount == 1u
	        && rContext.m_hPreviousDepthTexture.is_valid() && m_nPreviousVisibilityPublicationGeneration == m_nPreparedPublicationGeneration
	        && m_nPreviousVisibilityTerrainGeneration == m_nCurrentVisibilityTerrainGeneration
	        && m_nPreviousVisibilityRenderFrame != UINT64_MAX && rContext.m_nRenderFrame == m_nPreviousVisibilityRenderFrame + 1u
	        && t3dWorldProjectionNearEqual( rContext.m_szViewProjection[0], rContext.m_szPreviousViewProjection[0] );
}

void Terrain3DWorldRuntime::buildFrame( const t3dRdForwardPassContext_t &rContext, const bool bHiZValid, t3dWorldFrame_t &rFrame ) const {
	memset( &rFrame, 0, sizeof( rFrame ) );
	t3dWorldCopyProjection( rContext.m_szViewProjection[0], rFrame.m_fViewProjection );
	t3dWorldCopyProjection( rContext.m_bTemporalHistoryValid ? rContext.m_szPreviousViewProjection[0] : rContext.m_szViewProjection[0],
	        rFrame.m_fPreviousViewProjection );
	t3dRdStoreFrustumPlanes( t3dRdExtractFrustum( rContext.m_szViewProjection[0] ), rFrame.m_fFrustumPlanes );
	const Vector3 camera = rContext.m_cameraTransform.origin;
	rFrame.m_fCameraAndMaxDistance[0] = ( float )camera.x;
	rFrame.m_fCameraAndMaxDistance[1] = ( float )camera.y;
	rFrame.m_fCameraAndMaxDistance[2] = ( float )camera.z;
	rFrame.m_fCameraAndMaxDistance[3] = rContext.m_fCameraZFar > 0.0f ? rContext.m_fCameraZFar : 100000.0f;
	rFrame.m_fViewportAndInvSize[0] = ( float )MAX( rContext.m_renderSize.x, 1 );
	rFrame.m_fViewportAndInvSize[1] = ( float )MAX( rContext.m_renderSize.y, 1 );
	rFrame.m_fViewportAndInvSize[2] = 1.0f / rFrame.m_fViewportAndInvSize[0];
	rFrame.m_fViewportAndInvSize[3] = 1.0f / rFrame.m_fViewportAndInvSize[1];
	rFrame.m_fMotionJitter[0] = ( float )rContext.m_taaJitter.x;
	rFrame.m_fMotionJitter[1] = ( float )rContext.m_taaJitter.y;
	rFrame.m_fMotionJitter[2] = ( float )rContext.m_previousTaaJitter.x;
	rFrame.m_fMotionJitter[3] = ( float )rContext.m_previousTaaJitter.y;
	rFrame.m_nClusterCount = m_nClusterCount;
	rFrame.m_nTerrainClusterCount = m_nTerrainStripClusterCount;
	rFrame.m_nTransformCount = m_nTransformCount;
	rFrame.m_nMaterialCount = m_nMaterialCount;
	rFrame.m_nVisibleCapacity =
	        m_resources.getCapacityBytes( WORLD_BUFFER_VISIBLE_ITEMS ) / sizeof( t3dWorldVisibleItem_t );
	rFrame.m_nPassKind = ( uint32_t )rContext.m_passKind;
	rFrame.m_nViewIndex = 0u;
	rFrame.m_nFrameId = ( uint32_t )rContext.m_nRenderFrame;
	rFrame.m_nPublicationGeneration = ( uint32_t )m_nPreparedPublicationGeneration;
	rFrame.m_nGeometryGeneration = ( uint32_t )m_nGeometryGeneration;
	rFrame.m_nMaterialGeneration = ( uint32_t )m_nMaterialGeneration;
	rFrame.m_nVtGeneration = rContext.m_nResourceGeneration;
	rFrame.m_nHiZWidth = bHiZValid ? m_hiZ.getWidth() : 0u;
	rFrame.m_nHiZHeight = bHiZValid ? m_hiZ.getHeight() : 0u;
	rFrame.m_nHiZMipCount = bHiZValid ? m_hiZ.getMipCount() : 0u;
	rFrame.m_nFlags = bHiZValid ? T3D_WORLD_FRAME_FLAG_HIZ_VALID : 0u;
}

bool Terrain3DWorldRuntime::prepareVisibilityPass(
        const t3dRdForwardPassContext_t &rContext, const t3dRdFrameSnapshot_t &rSnapshot, const uint32_t nVisibilityMask ) {
	if ( !m_bInitialized || !m_bReady ) {
		return false;
	}
	++m_nVisibilityPrepareCount;
	if ( m_nCurrentVisibilityRenderFrame != rContext.m_nRenderFrame ) {
		m_nPreviousVisibilityRenderFrame = m_nCurrentVisibilityRenderFrame;
		m_nPreviousVisibilityPublicationGeneration = m_nCurrentVisibilityPublicationGeneration;
		m_nPreviousVisibilityTerrainGeneration = m_nCurrentVisibilityTerrainGeneration;
		m_nCurrentVisibilityRenderFrame = rContext.m_nRenderFrame;
		m_nCurrentVisibilityPublicationGeneration = m_nPreparedPublicationGeneration;
		m_nCurrentVisibilityTerrainGeneration = rSnapshot.m_nGeneration;
	}
	bool hiz_valid = rContext.m_passKind != T3D_RD_FORWARD_PASS_SHADOW && temporalHiZSafe( rContext );
	if ( hiz_valid ) {
		if ( m_nHiZBuildRenderFrame != rContext.m_nRenderFrame || m_nHiZBuildViewportKey != rContext.m_nViewportKey ) {
			hiz_valid = m_hiZ.bindPreviousDepthCold( rContext.m_hPreviousDepthTexture ) && m_hiZ.buildHot( rContext.m_renderSize );
			if ( hiz_valid ) {
				m_nHiZBuildRenderFrame = rContext.m_nRenderFrame;
				m_nHiZBuildViewportKey = rContext.m_nViewportKey;
			}
		} else {
			hiz_valid = m_hiZ.isReady();
		}
	} else {
		++m_nHiZInvalidHistoryFrames;
	}
	if ( !m_visibility.prepareBindingsCold( m_resources, m_hiZ ) ) {
		return false;
	}
	t3dWorldFrame_t frame{};
	buildFrame( rContext, hiz_valid, frame );
	if ( !m_resources.uploadFrameHot( frame )
	        || !m_visibility.dispatchHot(
	                m_resources, m_hiZ, rSnapshot, m_nVisibilityCandidateCount, nVisibilityMask, ( uint32_t )rContext.m_passKind, hiz_valid ) ) {
		return false;
	}
	return true;
}

bool Terrain3DWorldRuntime::prepareRasterPass( const t3dRdForwardPassContext_t &rContext,
        const t3dRdWorldTerrainRasterBindings_t &rTerrain,
        const t3dSharedVTBindings_t &rVT ) {
	return m_bReady && m_raster.prepareCold( rContext, m_resources, rTerrain, rVT );
}

bool Terrain3DWorldRuntime::submitRasterPass( const t3dRdForwardPassContext_t &rContext ) {
	return m_bReady && m_raster.submitHot( rContext, m_resources );
}

void Terrain3DWorldRuntime::shutdownCold( void ) {
	m_raster.shutdownCold();
	m_visibility.shutdownCold();
	m_hiZ.shutdownCold();
	m_resources.shutdownCold();
	m_nPreparedPublicationGeneration = 0u;
	m_nCurrentVisibilityPublicationGeneration = 0u;
	m_nPreviousVisibilityPublicationGeneration = 0u;
	m_nCurrentVisibilityTerrainGeneration = 0u;
	m_nPreviousVisibilityTerrainGeneration = 0u;
	m_nCurrentVisibilityRenderFrame = UINT64_MAX;
	m_nPreviousVisibilityRenderFrame = UINT64_MAX;
	m_nHiZBuildRenderFrame = UINT64_MAX;
	m_nTerrainStripClusterCount = 0u;
	m_bInitialized = false;
	m_bReady = false;
}

bool Terrain3DWorldRuntime::isReady( void ) const {
	return m_bReady;
}
uint64_t Terrain3DWorldRuntime::getPreparedPublicationGeneration( void ) const {
	return m_nPreparedPublicationGeneration;
}
uint64_t Terrain3DWorldRuntime::getPrepareCount( void ) const {
	return m_nPrepareCount;
}
uint64_t Terrain3DWorldRuntime::getUploadCount( void ) const {
	return m_nUploadCount;
}
uint64_t Terrain3DWorldRuntime::getPrepareFailureCount( void ) const {
	return m_nPrepareFailureCount;
}
uint64_t Terrain3DWorldRuntime::getVisibilityPrepareCount( void ) const {
	return m_nVisibilityPrepareCount;
}
uint64_t Terrain3DWorldRuntime::getHiZInvalidHistoryFrames( void ) const {
	return m_nHiZInvalidHistoryFrames;
}
uint64_t Terrain3DWorldRuntime::getPublicationBudgetDeferralCount( void ) const {
	return m_nPublicationBudgetDeferralCount;
}
uint64_t Terrain3DWorldRuntime::getPublicationBoundaryDeferralCount( void ) const {
	return m_nPublicationBoundaryDeferralCount;
}
uint64_t Terrain3DWorldRuntime::getPublicationUploadBudgetBytes( void ) const {
	return m_nPublicationUploadBudgetBytes;
}
uint32_t Terrain3DWorldRuntime::getPublicationClusterBudget( void ) const {
	return m_nPublicationClusterBudget;
}
const t3dWorldResourceLimits_t &Terrain3DWorldRuntime::getLimits( void ) const {
	return m_limits;
}
Terrain3DWorldResources &Terrain3DWorldRuntime::getResources( void ) {
	return m_resources;
}
const Terrain3DWorldResources &Terrain3DWorldRuntime::getResources( void ) const {
	return m_resources;
}
const Terrain3DWorldHiZ &Terrain3DWorldRuntime::getHiZ( void ) const {
	return m_hiZ;
}
const Terrain3DWorldVisibilityGpu &Terrain3DWorldRuntime::getVisibility( void ) const {
	return m_visibility;
}
const Terrain3DWorldRaster &Terrain3DWorldRuntime::getRaster( void ) const {
	return m_raster;
}
size_t Terrain3DWorldRuntime::getRetainedCpuBytes( void ) const {
	return m_resources.getRetainedCpuBytes();
}
