// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/runtime/terrain_3d_data_residency_adapter.h"

#include "data/model/terrain_3d_data.h"
#include "data/editing/terrain_3d_map_edit_service.h"
#include "data/persistence/terrain_3d_region_persistence.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/runtime/terrain_3d_runtime_artifact_service.h"
#include "data/sampling/terrain_3d_sampling_storage.h"

Terrain3DDataResidencyAdapter::Terrain3DDataResidencyAdapter( Terrain3DRegionRepository &rRepository,
        Terrain3DSamplingStorage &rSamplingStorage,
        Terrain3DMapEditService &rMapEditService,
        Terrain3DRegionPersistence &rPersistence,
        Terrain3DRuntimeArtifactService &rRuntimeArtifacts ) :
        m_cpuRegionCache( &rRepository, &rPersistence, &rSamplingStorage, &rRuntimeArtifacts.pageStore() ),
        m_instanceRegionCache( &rRepository, &rRuntimeArtifacts.pageStore() ),
        m_middleHeightRegionCache( &rRepository, &rSamplingStorage, &rRuntimeArtifacts.pageStore() ),
        m_gpuRegionCache( rRepository, rSamplingStorage, rMapEditService, rRuntimeArtifacts.pageStore(), m_cpuRegionCache ),
        m_farSurfaceAtlasCache( &rRepository, &rSamplingStorage, &m_cpuRegionCache ),
        m_activeHeightResidency( &rRepository, &rSamplingStorage, &rRuntimeArtifacts.pageStore() ),
        m_regionResidency( &rRepository,
                &rSamplingStorage,
                &rRuntimeArtifacts.pageStore(),
                &m_activeHeightResidency,
                &m_cpuRegionCache,
                &m_instanceRegionCache,
                &m_middleHeightRegionCache,
                &m_gpuRegionCache,
                &m_farSurfaceAtlasCache ) {
}

void Terrain3DDataResidencyAdapter::resetCpu( const bool bReleaseCleanMaps ) {
	m_cpuRegionCache.reset( bReleaseCleanMaps );
}

void Terrain3DDataResidencyAdapter::resetMiddleHeight( void ) {
	m_middleHeightRegionCache.reset();
}

void Terrain3DDataResidencyAdapter::resetGpu( const bool bClearTextures ) {
	m_gpuRegionCache.reset( bClearTextures );
}

void Terrain3DDataResidencyAdapter::resetFarSurface( void ) {
	m_farSurfaceAtlasCache.reset();
}

void Terrain3DDataResidencyAdapter::resetRegionResidency( void ) {
	m_regionResidency.reset();
}

void Terrain3DDataResidencyAdapter::deactivateCaches( const bool bClearGpuTextures ) {
	m_regionResidency.reset();
	m_middleHeightRegionCache.reset();
	m_gpuRegionCache.reset( bClearGpuTextures );
	m_cpuRegionCache.reset( false );
}

void Terrain3DDataResidencyAdapter::setBudgetOverrides(
        const int nGpuUploadLayers, const int nCpuLoadLayers, const int nInstanceLoadPages ) {
	m_regionResidency.setBudgetOverrides( nGpuUploadLayers, nCpuLoadLayers, nInstanceLoadPages );
}

void Terrain3DDataResidencyAdapter::clearBudgetOverrides( void ) {
	m_regionResidency.clearBudgetOverrides();
}

int Terrain3DDataResidencyAdapter::effectiveGpuUploadLayers( void ) const {
	return m_regionResidency.effectiveGpuUploadLayers( m_gpuRegionCache.getUploadBudgetLayers() );
}

int Terrain3DDataResidencyAdapter::effectiveCpuLoadLayers( void ) const {
	return m_regionResidency.effectiveCpuLoadLayers( m_cpuRegionCache.getLoadBudgetLayers() );
}

int Terrain3DDataResidencyAdapter::effectiveInstanceLoadPages( void ) const {
	return m_regionResidency.effectiveInstanceLoadPages( m_instanceRegionCache.getLoadBudgetPages() );
}

void Terrain3DDataResidencyAdapter::updateRegionResidency(
        const t3dResidencyFrameState_s &rFrame, const bool bForce, const t3dRegionResidencyUpdateContext_t &rContext ) {
	m_regionResidency.update( rFrame, bForce, rContext );
}

void Terrain3DDataResidencyAdapter::resetRuntimeCaches( const t3dRegionResidencyUpdateContext_t &rContext ) {
	m_regionResidency.resetRuntimeCaches( rContext );
}

void Terrain3DDataResidencyAdapter::appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const {
	m_regionResidency.appendRuntimeProfileSample( rSample );
}
