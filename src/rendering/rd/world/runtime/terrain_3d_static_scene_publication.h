#ifndef TERRAIN3D_STATIC_SCENE_PUBLICATION_H
#define TERRAIN3D_STATIC_SCENE_PUBLICATION_H
#include "core/utility/terrain_3d_sync.h"
#include "core/utility/terrain_3d_generation.h"
#include "rendering/rd/world/runtime/terrain_3d_world_publication.h"

struct t3dStaticScenePublication_t
{
	const Terrain3DWorldPublication *pWorld = NULL;
	uint32_t						 nCellCount = 0u;
	uint32_t						 nInstanceCount = 0u;
	uint32_t						 nMaterialCount = 0u;
	uint64_t						 nGeneration = 0u;
	bool							 bValid = false;
};

// A read lease holds the publication lock while a builder borrows its world.
// The producer cannot replace or release that world until the lease is destroyed.
class Terrain3DStaticSceneReadLease
{
	Terrain3DScopedLock				   m_lock;
	const t3dStaticScenePublication_t &m_publication;
	const uint64_t					  &m_producerId;

public:
	Terrain3DStaticSceneReadLease(
		Terrain3DMutex					  &rMutex,
		const t3dStaticScenePublication_t &rPublication,
		const uint64_t					  &rProducerId ) :
		m_lock( rMutex ),
		m_publication( rPublication ),
		m_producerId( rProducerId ) {
	}
	const t3dStaticScenePublication_t &publication(
		void ) const {
		return m_publication;
	}
	uint64_t producerId(
		void ) const {
		return m_producerId;
	}
};

class Terrain3DStaticScenePublication
{
private:
	mutable Terrain3DMutex		m_staticScenePublicationMutex;
	Terrain3DWorldPublication	m_staticWorldPublicationStorage[2];
	t3dStaticScenePublication_t m_staticScenePublication{};
	// Protected by m_staticScenePublicationMutex. Zero means unclaimed.
	uint64_t m_nStaticSceneProducerId = 0u;
	uint64_t m_nStaticSceneGpuReadyGeneration = 0u;

public:
	Terrain3DStaticSceneReadLease read(
		void ) {
		return Terrain3DStaticSceneReadLease( m_staticScenePublicationMutex,
											  m_staticScenePublication,
											  m_nStaticSceneProducerId );
	}
	bool publishCold(
		const uint64_t					 nProducerId,
		const Terrain3DWorldPublication &rWorld ) {
		if ( nProducerId == 0u ) {
			return false;
		}
		const t3dWorldPublicationCapacity_t required = { rWorld.vertices().size(),
														 rWorld.clusterVertexIndices().size(),
														 rWorld.triangleWords().size(),
														 rWorld.clusters().size(),
														 rWorld.clusterVtMetadata().size(),
														 rWorld.m_terrainStripClusters.size(),
														 rWorld.m_transforms.size(),
														 rWorld.m_instances.size(),
														 rWorld.m_cells.size(),
														 rWorld.m_hlodRecords.size(),
														 rWorld.m_materials.size(),
														 rWorld.m_visibilityCandidates.size() };
		Terrain3DScopedLock					lock( m_staticScenePublicationMutex );
		if ( m_nStaticSceneProducerId != 0u && m_nStaticSceneProducerId != nProducerId ) {
			return false;
		}
		const uint32_t targetIndex = m_staticScenePublication.pWorld == &m_staticWorldPublicationStorage[0] ? 1u : 0u;
		Terrain3DWorldPublication &target = m_staticWorldPublicationStorage[targetIndex];
		if ( !target.hasCapacity( required ) && !target.reserveCold( required ) ) {
			return false;
		}
		if ( !target.copyFrom( rWorld ) ) {
			return false;
		}
		const uint64_t nextGeneration = t3dNextGeneration( m_staticScenePublication.nGeneration );
		m_staticScenePublication = t3dStaticScenePublication_t{};
		m_staticScenePublication.pWorld = &target;
		m_staticScenePublication.nCellCount = ( uint32_t ) target.m_cells.size();
		m_staticScenePublication.nInstanceCount = ( uint32_t ) target.m_instances.size();
		m_staticScenePublication.nMaterialCount = ( uint32_t ) target.m_materials.size();
		m_staticScenePublication.nGeneration = nextGeneration;
		m_staticScenePublication.bValid = true;
		m_nStaticSceneProducerId = nProducerId;
		m_nStaticSceneGpuReadyGeneration = 0u;
		return true;
	}

	void releaseCold(
		const uint64_t nProducerId ) {
		Terrain3DScopedLock lock( m_staticScenePublicationMutex );
		if ( nProducerId == 0u || m_nStaticSceneProducerId != nProducerId ) {
			return;
		}
		const uint64_t nextGeneration = t3dNextGeneration( m_staticScenePublication.nGeneration );
		m_staticScenePublication = t3dStaticScenePublication_t{};
		m_staticScenePublication.nGeneration = nextGeneration;
		m_nStaticSceneProducerId = 0u;
		m_nStaticSceneGpuReadyGeneration = 0u;
	}

	bool isReadyCold(
		const uint64_t nProducerId ) {
		Terrain3DScopedLock lock( m_staticScenePublicationMutex );
		return nProducerId != 0u && m_nStaticSceneProducerId == nProducerId && m_staticScenePublication.bValid &&
			   m_nStaticSceneGpuReadyGeneration == m_staticScenePublication.nGeneration;
	}
	void acknowledge(
		const Terrain3DWorldPublication &rWorld,
		const bool						 bExactReady ) {
		Terrain3DScopedLock lock( m_staticScenePublicationMutex );
		if ( m_staticScenePublication.bValid && m_nStaticSceneProducerId != 0u &&
			 m_nStaticSceneProducerId == rWorld.m_nStaticSceneProducerId &&
			 m_staticScenePublication.nGeneration == rWorld.m_nStaticScenePublicationGeneration ) {
			m_nStaticSceneGpuReadyGeneration = bExactReady ? m_staticScenePublication.nGeneration : 0u;
		}
	}
	size_t getRetainedCpuBytes(
		void ) const {
		Terrain3DScopedLock lock( m_staticScenePublicationMutex );
		return m_staticWorldPublicationStorage[0].getRetainedCpuBytes() +
			   m_staticWorldPublicationStorage[1].getRetainedCpuBytes();
	}
	void reset(
		void ) {
		Terrain3DScopedLock lock( m_staticScenePublicationMutex );
		m_staticScenePublication = t3dStaticScenePublication_t{};
		m_nStaticSceneProducerId = 0u;
		m_nStaticSceneGpuReadyGeneration = 0u;
		for ( auto &world : m_staticWorldPublicationStorage ) {
			world = Terrain3DWorldPublication();
		}
	}
};
#endif // TERRAIN3D_STATIC_SCENE_PUBLICATION_H
