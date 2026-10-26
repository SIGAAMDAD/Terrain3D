#include "streaming/residency/terrain_3d_tile_residency_controller.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "streaming/residency/terrain_3d_raster_admission.h"
#include <string.h>

using namespace godot;

void Terrain3DTileResidencyController::setFrameDemand(
	const t3dResidencyFrameState_s &rFrame ) {
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
		const Vector2i location( ( int ) ( i % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF,
								 ( int ) ( i / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF );
		m_szDemandPriority[i] = t3dRasterRegionPriority( rFrame, location );
	}
}

bool Terrain3DTileResidencyController::configureCold(
	const uint16_t nSlots ) {
	if ( nSlots > 256u ) {
		return false;
	}
	reset();
	m_slots.resize( nSlots ); // cold only; no steady-state slot growth
	return true;
}

void Terrain3DTileResidencyController::reset(
	void ) {
	for ( size_t i = 0u; i < m_slots.size(); ++i ) {
		m_slots[i].m_nId = UINT16_MAX;
		m_slots[i].m_nPins = 0u;
		m_slots[i].m_bytes.clear();
		m_slots[i].m_nGeneration = m_slots[i].m_nGeneration == UINT32_MAX ? 1u : m_slots[i].m_nGeneration + 1u;
	}
	m_nResidentBytes = 0u;
	m_nClock = 1u;
}

bool Terrain3DTileResidencyController::acquire(
	const uint16_t					  nId,
	const Terrain3DRasterBundleIndex &rIndex,
	t3dRasterTileLease_t			 &rLease,
	PackedByteArray					 &rData,
	Ref< FileAccess >				 &rReader ) {
	rLease = t3dRasterTileLease_t{};
	if ( !t3dRasterTileIdValid( nId ) || !rIndex.hasTile( nId ) ) {
		return false;
	}
	for ( size_t i = 0u; i < m_slots.size(); ++i ) {
		auto &slot = m_slots[i];
		if ( slot.m_nId == nId ) {
			if ( slot.m_nPins == UINT16_MAX ) {
				return false;
			}
			++slot.m_nPins;
			slot.m_nLastUse = ++m_nClock;
			rLease.m_nSlot = ( uint16_t ) i;
			rLease.m_nGeneration = slot.m_nGeneration;
			rData = slot.m_bytes;
			++m_nHits;
			return true;
		}
	}
	uint16_t chosen = UINT16_MAX;
	uint64_t oldest = UINT64_MAX;
	uint32_t lowestPriority = UINT32_MAX;
	for ( uint16_t i = 0u; i < m_slots.size(); ++i ) {
		const auto &slot = m_slots[i];
		if ( slot.m_nPins != 0u ) {
			continue;
		}
		if ( slot.m_nId == UINT16_MAX ) {
			chosen = i;
			break;
		}
		const uint32_t regionIndex = ( uint32_t ) slot.m_nId / ( REGION_TYPE_MAX * T3D_RASTER_TILES_PER_REGION );
		const uint32_t priority = m_szDemandPriority[regionIndex];
		if ( priority < lowestPriority || ( priority == lowestPriority && slot.m_nLastUse < oldest ) ) {
			chosen = i;
			lowestPriority = priority;
			oldest = slot.m_nLastUse;
		}
	}
	if ( chosen == UINT16_MAX ) {
		return false;
	}
	PackedByteArray decoded;
	if ( !rIndex.readTile( nId, decoded, rReader ) ) {
		return false;
	}
	auto &slot = m_slots[chosen];
	if ( slot.m_nId != UINT16_MAX ) {
		m_nResidentBytes -= ( uint64_t ) slot.m_bytes.size();
		++m_nEvictions;
	}
	slot.m_bytes = decoded;
	slot.m_nId = nId;
	slot.m_nPins = 1u;
	slot.m_nLastUse = ++m_nClock;
	slot.m_nGeneration = slot.m_nGeneration == UINT32_MAX ? 1u : slot.m_nGeneration + 1u;
	m_nResidentBytes += ( uint64_t ) slot.m_bytes.size();
	if ( m_nResidentBytes > m_nHighWaterBytes ) {
		m_nHighWaterBytes = m_nResidentBytes;
	}
	rLease.m_nSlot = chosen;
	rLease.m_nGeneration = slot.m_nGeneration;
	rData = slot.m_bytes;
	++m_nMisses;
	return true;
}

void Terrain3DTileResidencyController::release(
	t3dRasterTileLease_t &rLease ) {
	if ( rLease.m_nSlot < m_slots.size() ) {
		auto &slot = m_slots[rLease.m_nSlot];
		if ( slot.m_nGeneration == rLease.m_nGeneration && slot.m_nPins != 0u ) {
			--slot.m_nPins;
		}
	}
	rLease = t3dRasterTileLease_t{};
}

Ref< Image > Terrain3DTileResidencyController::reconstructRegion(
	const Vector2i					 &rRegion,
	const t3dMapType_t				  eChannel,
	const Terrain3DRasterBundleIndex &rIndex ) {
	const uint32_t pixelBytes = t3dRasterTilePixelBytes( eChannel );
	if ( pixelBytes == 0u || t3dRasterTileId( rRegion, eChannel, 0u, 0u ) == UINT16_MAX ||
		 ( eChannel != TYPE_HEIGHT && !rIndex.hasRegionChannel( rRegion, eChannel ) ) ) {
		return Ref< Image >();
	}
	uint8_t pixel[4] = {};
	if ( eChannel == TYPE_HEIGHT ) {
		const uint16_t height = t3dRegionCodecEncodeHeightU16( 0.0f );
		memcpy( pixel, &height, 2u );
	} else if ( eChannel == TYPE_CONTROL ) {
		const uint32_t control = T3D_REGION_DEFAULT_CONTROL;
		memcpy( pixel, &control, 4u );
	} else {
		pixel[0] = pixel[1] = pixel[2] = 255u;
		pixel[3] = 128u;
	}
	PackedByteArray output;
	output.resize( 128u * 128u * pixelBytes );
	uint8_t *const pOutput = output.ptrw();
	for ( size_t i = 0u; i < 128u * 128u; ++i ) {
		memcpy( pOutput + i * pixelBytes, pixel, pixelBytes );
	}
	// Reuse one cold source handle per region instead of sixteen opens on
	// cache misses. Cache hits still avoid any payload decode or staging.
	Ref< FileAccess > reader; // Lazy: one handle per reconstruction, only on actual source misses.
	for ( uint8_t ty = 0u; ty < 4u; ++ty ) {
		for ( uint8_t tx = 0u; tx < 4u; ++tx ) {
			const uint16_t id = t3dRasterTileId( rRegion, eChannel, tx, ty );
			if ( !rIndex.hasTile( id ) ) {
				continue;
			}
			t3dRasterTileLease_t lease;
			PackedByteArray		 data;
			if ( !acquire( id, rIndex, lease, data, reader ) || data.size() != t3dRasterTileRawBytes( eChannel ) ) {
				release( lease );
				return Ref< Image >();
			}
			for ( uint32_t row = 0u; row < 32u; ++row ) {
				memcpy( pOutput + ( ( size_t ) ( ty * 32u + row ) * 128u + tx * 32u ) * pixelBytes,
						data.ptr() + row * 32u * pixelBytes,
						32u * pixelBytes );
			}
			release( lease );
		}
	}
	const Image::Format format = eChannel == TYPE_HEIGHT	? Image::FORMAT_R16
								 : eChannel == TYPE_CONTROL ? Image::FORMAT_RF
															: Image::FORMAT_RGBA8;
	return Image::create_from_data( 128, 128, false, format, output );
}

Dictionary Terrain3DTileResidencyController::getStats(
	void ) const {
	Dictionary stats;
	uint32_t   occupied = 0u;
	uint32_t   pinned = 0u;
	for ( size_t i = 0u; i < m_slots.size(); ++i ) {
		occupied += m_slots[i].m_nId != UINT16_MAX ? 1u : 0u;
		pinned += m_slots[i].m_nPins != 0u ? 1u : 0u;
	}
	// Capacity ceiling is stable after cold configuration. Do not put the
	// fluctuating committed-byte counter into the memory grant's fixed section:
	// that would recreate the budget/reallocation feedback loop on each edit.
	stats["raster_cpu_tile_slot_metadata_bytes"] =
		( int64_t ) ( m_slots.capacity() * sizeof( t3dRasterTileResident_t ) );
	stats["raster_cpu_tile_payload_capacity_ceiling_bytes"] = ( int64_t ) ( m_slots.size() * 4096u );
	stats["raster_cpu_tile_capacity"] = ( int64_t ) m_slots.size();
	stats["raster_cpu_tile_resident"] = occupied;
	stats["raster_cpu_tile_pinned"] = pinned;
	stats["raster_cpu_tile_bytes"] = ( int64_t ) m_nResidentBytes;
	stats["raster_cpu_tile_peak_bytes"] = ( int64_t ) m_nHighWaterBytes;
	stats["raster_cpu_tile_hits"] = ( int64_t ) m_nHits;
	stats["raster_cpu_tile_misses"] = ( int64_t ) m_nMisses;
	stats["raster_cpu_tile_evictions"] = ( int64_t ) m_nEvictions;
	return stats;
}
