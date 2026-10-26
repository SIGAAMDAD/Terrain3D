#include "streaming/gpu/terrain_3d_gpu_page_pool.h"
#include "core/utility/terrain_3d_generation.h"

bool Terrain3DGpuPagePool::configure(
	const Configuration &config,
	uint16_t			 poolIndex ) {
	if ( m_texture.isValid() || poolIndex >= T3D_GPU_PAGE_CHANNEL_COUNT || !config.maximumBytes )
		return false;
	t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildRawPageLayout( config.format,
														 config.width,
														 config.height,
														 config.layers,
														 config.mipmaps,
														 config.storageWrites,
														 layout ) ||
		 layout.m_nBytes > config.maximumBytes )
		return false;
	m_config = config;
	m_layout = layout;
	m_poolIndex = poolIndex;
	m_configured = true;
	m_defaultInitialized = false;
	m_defaultStaged = false;
	for ( uint32_t i = 0u; i < MAX_LAYERS; ++i ) {
		m_slots[i] = Slot{};
	}
	m_slots[0].state = RESERVED; // Do not expose an uninitialized default texture.
	return true;
}

bool Terrain3DGpuPagePool::createCold(
	Terrain3DRdTextureManager &manager ) {
	if ( !m_configured || m_texture.isValid() )
		return false;
	return manager.createRawPageTexture( m_texture,
										 m_config.format,
										 m_config.width,
										 m_config.height,
										 m_config.layers,
										 m_config.mipmaps,
										 m_config.storageWrites,
										 TypedArray< PackedByteArray >(),
										 String( "T3D.GpuPagePool." ) + String::num_int64( m_poolIndex ),
										 T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE,
										 false );
}

bool Terrain3DGpuPagePool::stageDefault(
	Terrain3DRdTextureManager &manager,
	const PackedByteArray	  &bytes,
	uint64_t				   submissionEpoch ) {
	if ( !m_texture.isValid() || m_defaultStaged || m_defaultInitialized || !submissionEpoch ||
		 ( uint64_t ) bytes.size() != layerBytes() )
		return false;
	if ( !manager.uploadRawPage( m_texture, 0u, bytes ) )
		return false;
	m_slots[0].lastReaderEpoch = submissionEpoch;
	m_defaultStaged = true;
	return true;
}

bool Terrain3DGpuPagePool::publishDefault(
	uint64_t confirmedCompletedEpoch ) {
	if ( !m_defaultStaged || m_defaultInitialized ||
		 !t3dGpuPageCanRetire( m_slots[0].lastReaderEpoch, confirmedCompletedEpoch ) )
		return false;
	m_slots[0].state = DEFAULT_PAGE;
	m_slots[0].references = 1u;
	m_defaultInitialized = true;
	m_defaultStaged = false;
	return true;
}

bool Terrain3DGpuPagePool::valid(
	const t3dGpuPageHandle_t &handle ) const {
	return m_texture.isValid() && handle.m_nPoolIndex == m_poolIndex && handle.m_nDeviceEpoch == m_deviceEpoch &&
		   handle.m_nPhysicalLayer < m_layout.m_nLayers &&
		   m_slots[handle.m_nPhysicalLayer].generation == handle.m_nSlotGeneration &&
		   m_slots[handle.m_nPhysicalLayer].state != FREE;
}

bool Terrain3DGpuPagePool::isPublished(
	const t3dGpuPageHandle_t &handle ) const {
	return valid( handle ) && ( m_slots[handle.m_nPhysicalLayer].state == PUBLISHED ||
								m_slots[handle.m_nPhysicalLayer].state == DEFAULT_PAGE );
}

t3dGpuPageHandle_t Terrain3DGpuPagePool::reserve() {
	t3dGpuPageHandle_t handle;
	if ( !m_texture.isValid() )
		return handle;
	for ( uint32_t i = 1u; i < m_layout.m_nLayers; ++i ) {
		Slot &slot = m_slots[i];
		if ( slot.state != FREE )
			continue;
		slot.state = RESERVED;
		slot.references = 0u;
		slot.lastReaderEpoch = 0u;
		handle.m_nPoolIndex = m_poolIndex;
		handle.m_nPhysicalLayer = ( uint16_t ) i;
		handle.m_nSlotGeneration = slot.generation;
		handle.m_nDeviceEpoch = m_deviceEpoch;
		return handle;
	}
	return handle;
}

bool Terrain3DGpuPagePool::upload(
	Terrain3DRdTextureManager &manager,
	const t3dGpuPageHandle_t  &handle,
	const PackedByteArray	  &bytes ) {
	return valid( handle ) && m_slots[handle.m_nPhysicalLayer].state == RESERVED &&
		   ( uint64_t ) bytes.size() == layerBytes() &&
		   manager.uploadRawPage( m_texture, handle.m_nPhysicalLayer, bytes );
}

bool Terrain3DGpuPagePool::publish(
	const t3dGpuPageHandle_t &handle ) {
	if ( !valid( handle ) || m_slots[handle.m_nPhysicalLayer].state != RESERVED )
		return false;
	Slot &slot = m_slots[handle.m_nPhysicalLayer];
	slot.state = PUBLISHED;
	slot.references = 1u;
	return true;
}

bool Terrain3DGpuPagePool::addReference(
	const t3dGpuPageHandle_t &handle ) {
	if ( !isPublished( handle ) )
		return false;
	Slot &slot = m_slots[handle.m_nPhysicalLayer];
	if ( slot.references == UINT16_MAX )
		return false;
	++slot.references;
	return true;
}

bool Terrain3DGpuPagePool::markReader(
	const t3dGpuPageHandle_t &handle,
	uint64_t				  submissionEpoch ) {
	if ( !isPublished( handle ) )
		return false;
	return pinUntil( handle, submissionEpoch );
}

bool Terrain3DGpuPagePool::pinUntil(
	const t3dGpuPageHandle_t &handle,
	uint64_t				  submissionEpoch ) {
	if ( !valid( handle ) || !submissionEpoch )
		return false;
	Slot &slot = m_slots[handle.m_nPhysicalLayer];
	if ( submissionEpoch > slot.lastReaderEpoch )
		slot.lastReaderEpoch = submissionEpoch;
	return true;
}

bool Terrain3DGpuPagePool::release(
	const t3dGpuPageHandle_t &handle ) {
	if ( !valid( handle ) || handle.m_nPhysicalLayer == 0u )
		return false;
	Slot &slot = m_slots[handle.m_nPhysicalLayer];
	if ( slot.state == RESERVED ) {
		slot.references = 0u;
		slot.state = RETIRED;
		return true;
	}
	if ( slot.state != PUBLISHED || slot.references == 0u )
		return false;
	if ( --slot.references == 0u )
		slot.state = RETIRED;
	return true;
}

uint32_t Terrain3DGpuPagePool::collect(
	uint64_t confirmedCompletedEpoch ) {
	uint32_t count = 0u;
	for ( uint32_t i = 1u; i < m_layout.m_nLayers; ++i ) {
		Slot &slot = m_slots[i];
		if ( slot.state != RETIRED || !t3dGpuPageCanRetire( slot.lastReaderEpoch, confirmedCompletedEpoch ) )
			continue;
		slot.state = FREE;
		slot.generation = t3dNextGeneration( slot.generation );
		slot.lastReaderEpoch = 0u;
		++count;
	}
	return count;
}

void Terrain3DGpuPagePool::destroyCold(
	Terrain3DRdTextureManager &manager ) {
	if ( m_texture.isValid() )
		manager.freeTexture( m_texture );
	abandonDevice();
}
void Terrain3DGpuPagePool::abandonDevice() {
	m_texture.abandon();
	m_deviceEpoch = t3dNextGeneration( m_deviceEpoch );
	for ( uint32_t i = 0u; i < MAX_LAYERS; ++i ) {
		m_slots[i] = Slot{};
		m_slots[i].generation = t3dNextGeneration( m_slots[i].generation );
	}
	m_slots[0].state = RESERVED;
	m_defaultInitialized = false;
	m_defaultStaged = false;
}
uint32_t Terrain3DGpuPagePool::available() const {
	uint32_t count = 0u;
	for ( uint32_t i = 1u; i < m_layout.m_nLayers; ++i )
		count += m_slots[i].state == FREE ? 1u : 0u;
	return count;
}
uint32_t Terrain3DGpuPagePool::resident() const {
	uint32_t count = 0u;
	for ( uint32_t i = 0u; i < m_layout.m_nLayers; ++i )
		count += ( m_slots[i].state == PUBLISHED || m_slots[i].state == DEFAULT_PAGE ) ? 1u : 0u;
	return count;
}
uint32_t Terrain3DGpuPagePool::retired() const {
	uint32_t count = 0u;
	for ( uint32_t i = 1u; i < m_layout.m_nLayers; ++i )
		count += m_slots[i].state == RETIRED ? 1u : 0u;
	return count;
}
