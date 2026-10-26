#include "streaming/gpu/terrain_3d_gpu_content_directory.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include <string.h>

static bool t3dSameHandle(
	const t3dGpuPageHandle_t &a,
	const t3dGpuPageHandle_t &b ) {
	return a.m_nPoolIndex == b.m_nPoolIndex && a.m_nPhysicalLayer == b.m_nPhysicalLayer &&
		   a.m_nSlotGeneration == b.m_nSlotGeneration && a.m_nDeviceEpoch == b.m_nDeviceEpoch;
}

t3dGpuPageContentKey_t Terrain3DGpuContentDirectory::fingerprint(
	uint8_t									   channel,
	const Terrain3DGpuPagePool::Configuration &config,
	const PackedByteArray					  &raw ) {
	t3dGpuPageContentKey_t result;
	if ( channel >= T3D_GPU_PAGE_CHANNEL_COUNT || raw.is_empty() || ( uint64_t ) raw.size() > UINT32_MAX )
		return result;
	// Canonical recipe includes channel, full mip chain, dimensions and numeric format.
	uint64_t recipe = 1469598103934665603ULL;
	recipe = t3dHashFnv1a64LittleU32( recipe, channel );
	recipe = t3dHashFnv1a64LittleU32( recipe, ( uint32_t ) config.format );
	recipe = t3dHashFnv1a64LittleU32( recipe, config.width );
	recipe = t3dHashFnv1a64LittleU32( recipe, config.height );
	recipe = t3dHashFnv1a64LittleU32( recipe, config.mipmaps );
	result.m_nFormatRecipe = ( uint32_t ) ( recipe ^ ( recipe >> 32u ) );
	result.m_nPayloadBytes = ( uint32_t ) raw.size();
	result.m_nContentHash = t3dHashStableBytes( raw.ptr(), ( uint64_t ) raw.size() );
	return result;
}

bool Terrain3DGpuContentDirectory::equalBytes(
	const PackedByteArray &a,
	const PackedByteArray &b,
	uint64_t			   expectedBytes ) {
	return expectedBytes != 0u && ( uint64_t ) a.size() == expectedBytes && ( uint64_t ) b.size() == expectedBytes &&
		   memcmp( a.ptr(), b.ptr(), ( size_t ) expectedBytes ) == 0;
}

void Terrain3DGpuContentDirectory::reset() {
	for ( uint32_t i = 0u; i < ENTRY_COUNT; ++i )
		m_entries[i] = Entry{};
}

void Terrain3DGpuContentDirectory::record(
	const t3dGpuPageHandle_t	 &handle,
	const t3dGpuPageContentKey_t &content ) {
	if ( handle.m_nPoolIndex >= T3D_GPU_PAGE_CHANNEL_COUNT ||
		 handle.m_nPhysicalLayer >= Terrain3DGpuPagePool::MAX_LAYERS || !content.m_nPayloadBytes )
		return;
	Entry &e = m_entries[( uint32_t ) handle.m_nPoolIndex * Terrain3DGpuPagePool::MAX_LAYERS + handle.m_nPhysicalLayer];
	e.handle = handle;
	e.content = content;
	e.present = true;
}

void Terrain3DGpuContentDirectory::forget(
	const t3dGpuPageHandle_t &handle ) {
	if ( handle.m_nPoolIndex >= T3D_GPU_PAGE_CHANNEL_COUNT ||
		 handle.m_nPhysicalLayer >= Terrain3DGpuPagePool::MAX_LAYERS )
		return;
	Entry &e = m_entries[( uint32_t ) handle.m_nPoolIndex * Terrain3DGpuPagePool::MAX_LAYERS + handle.m_nPhysicalLayer];
	if ( e.present && t3dSameHandle( e.handle, handle ) )
		e = Entry{};
}

bool Terrain3DGpuContentDirectory::findCandidate(
	uint8_t						  channel,
	const t3dGpuPageContentKey_t &content,
	const t3dGpuPageHandle_t	 &exclude,
	const Terrain3DGpuPagePool	 &pool,
	t3dGpuPageHandle_t			 &candidate,
	uint16_t					  afterPhysicalLayer ) const {
	if ( channel >= T3D_GPU_PAGE_CHANNEL_COUNT || !content.m_nPayloadBytes )
		return false;
	const uint32_t start = ( uint32_t ) channel * Terrain3DGpuPagePool::MAX_LAYERS;
	const uint32_t first = afterPhysicalLayer == UINT16_MAX ? 0u : ( uint32_t ) afterPhysicalLayer + 1u;
	for ( uint32_t i = first; i < Terrain3DGpuPagePool::MAX_LAYERS; ++i ) {
		const Entry &e = m_entries[start + i];
		if ( !e.present || t3dSameHandle( e.handle, exclude ) || !pool.isPublished( e.handle ) )
			continue;
		if ( e.content.m_nContentHash != content.m_nContentHash ||
			 e.content.m_nFormatRecipe != content.m_nFormatRecipe ||
			 e.content.m_nPayloadBytes != content.m_nPayloadBytes )
			continue;
		candidate = e.handle;
		return true;
	}
	return false;
}
uint32_t Terrain3DGpuContentDirectory::entries() const {
	uint32_t count = 0u;
	for ( uint32_t i = 0u; i < ENTRY_COUNT; ++i )
		if ( m_entries[i].present )
			++count;
	return count;
}
