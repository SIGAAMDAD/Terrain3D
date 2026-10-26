#ifndef TERRAIN_3D_HOT_DIRTY_IMAGE_H
#define TERRAIN_3D_HOT_DIRTY_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "terrain_3d_hot_range_abi.h"

// Render-thread owned. All allocation happens at initialize(), never per pass.
// A physical frame slot is independent of the logical HOT record index.
class Terrain3DHotDirtyImage
{
public:
	static constexpr uint32_t BLOCK_BYTES = 16u;
	using Range = t3dHotRange_t;

private:
	uint8_t **m_committed = nullptr;
	uint8_t **m_known = nullptr;
	uint32_t *m_touchedEpoch = nullptr;
	uint16_t *m_touchedMask = nullptr;
	Range	 *m_ranges = nullptr;
	uint8_t	 *m_pending = nullptr;
	uint32_t  m_capacity = 0u, m_recordBytes = 0u, m_records = 0u, m_blocks = 0u, m_slots = 0u;
	uint32_t  m_slot = UINT32_MAX, m_epoch = 0u, m_used = 0u, m_rangeCount = 0u;
	uint32_t  m_initializedBlocks = 0u;
	bool	  m_open = false, m_finalized = false;

public:
	Terrain3DHotDirtyImage() = default;
	~Terrain3DHotDirtyImage() {
		release();
	}
	Terrain3DHotDirtyImage( const Terrain3DHotDirtyImage & ) = delete;
	Terrain3DHotDirtyImage &operator=( const Terrain3DHotDirtyImage & ) = delete;

	void release() {
		if ( m_committed ) {
			for ( uint32_t i = 0u; i < m_slots; ++i ) {
				free( m_committed[i] );
			}
			free( m_committed );
		}
		if ( m_known ) {
			for ( uint32_t i = 0u; i < m_slots; ++i ) {
				free( m_known[i] );
			}
			free( m_known );
		}
		free( m_touchedEpoch );
		free( m_touchedMask );
		free( m_ranges );
		m_committed = nullptr;
		m_known = nullptr;
		m_touchedEpoch = nullptr;
		m_touchedMask = nullptr;
		m_ranges = nullptr;
		m_pending = nullptr;
		m_capacity = m_recordBytes = m_records = m_blocks = m_slots = 0u;
		m_slot = UINT32_MAX;
		m_epoch = m_used = m_rangeCount = m_initializedBlocks = 0u;
		m_open = m_finalized = false;
	}

	bool initialize(
		uint32_t record_bytes,
		uint32_t records,
		uint32_t slots,
		uint8_t *staging ) {
		release();
		if ( !record_bytes || !records || !slots || !staging || record_bytes % BLOCK_BYTES ||
			 uint64_t( record_bytes ) * records > UINT32_MAX ) {
			return false;
		}
		m_recordBytes = record_bytes;
		m_records = records;
		m_slots = slots;
		m_capacity = record_bytes * records;
		m_blocks = m_capacity / BLOCK_BYTES;
		m_pending = staging;
		m_committed = static_cast< uint8_t ** >( calloc( slots, sizeof( uint8_t * ) ) );
		m_known = static_cast< uint8_t ** >( calloc( slots, sizeof( uint8_t * ) ) );
		m_touchedEpoch = static_cast< uint32_t * >( calloc( m_blocks, sizeof( uint32_t ) ) );
		m_touchedMask = static_cast< uint16_t * >( calloc( m_blocks, sizeof( uint16_t ) ) );
		m_ranges = static_cast< Range * >( calloc( m_blocks, sizeof( Range ) ) );
		if ( !m_committed || !m_known || !m_touchedEpoch || !m_touchedMask || !m_ranges ) {
			release();
			return false;
		}
		for ( uint32_t i = 0u; i < slots; ++i ) {
			m_committed[i] = static_cast< uint8_t * >( calloc( m_capacity, 1u ) );
			m_known[i] = static_cast< uint8_t * >( calloc( m_blocks, 1u ) );
			if ( !m_committed[i] || !m_known[i] ) {
				release();
				return false;
			}
		}
		return true;
	}

	bool begin(
		uint32_t physical_slot ) {
		if ( !m_pending || physical_slot >= m_slots || m_open ) {
			return false;
		}
		m_slot = physical_slot;
		m_used = m_rangeCount = m_initializedBlocks = 0u;
		m_open = true;
		m_finalized = false;
		if ( ++m_epoch == 0u ) {
			memset( m_touchedEpoch, 0, size_t( m_blocks ) * sizeof( uint32_t ) );
			m_epoch = 1u;
		}
		return true;
	}

	bool reserveRecord() {
		if ( !m_open || m_finalized || m_used >= m_records ) {
			return false;
		}
		++m_used;
		return true;
	}

	bool write(
		uint32_t	relative_offset,
		const void *source,
		uint32_t	bytes ) {
		if ( !m_open || m_finalized || !m_used || relative_offset > m_recordBytes ||
			 bytes > m_recordBytes - relative_offset || ( bytes && !source ) ) {
			return false;
		}
		if ( !bytes ) {
			return true;
		}
		const uint32_t offset = ( m_used - 1u ) * m_recordBytes + relative_offset;
		memmove( m_pending + offset, source, bytes );
		const uint32_t first = offset / BLOCK_BYTES;
		const uint32_t last = ( offset + bytes - 1u ) / BLOCK_BYTES;
		for ( uint32_t block = first; block <= last; ++block ) {
			if ( m_touchedEpoch[block] != m_epoch ) {
				m_touchedEpoch[block] = m_epoch;
				m_touchedMask[block] = 0u;
			}
			const uint32_t begin_byte = offset > block * BLOCK_BYTES ? offset - block * BLOCK_BYTES : 0u;
			const uint32_t end_byte =
				offset + bytes < ( block + 1u ) * BLOCK_BYTES ? offset + bytes - block * BLOCK_BYTES : BLOCK_BYTES;
			const uint32_t byte_mask = ( ( 1u << ( end_byte - begin_byte ) ) - 1u ) << begin_byte;
			m_touchedMask[block] |= uint16_t( byte_mask );
		}
		return true;
	}

	// Unwritten bytes in an allocated record have explicit zero/default semantics.
	// No full-frame or per-record memset is performed: only differing blocks are materialized.
	bool finalize() {
		if ( !m_open ) {
			return false;
		}
		if ( m_finalized ) {
			return true;
		}
		m_rangeCount = m_initializedBlocks = 0u;
		const uint32_t active_blocks = m_used * m_recordBytes / BLOCK_BYTES;
		for ( uint32_t block = 0u; block < active_blocks; ++block ) {
			const uint32_t offset = block * BLOCK_BYTES;
			const uint16_t mask = m_touchedEpoch[block] == m_epoch ? m_touchedMask[block] : 0u;
			uint8_t		   desired[BLOCK_BYTES];
			if ( mask == 0xffffu ) {
				memcpy( desired, m_pending + offset, BLOCK_BYTES );
			} else {
				for ( uint32_t byte = 0u; byte < BLOCK_BYTES; ++byte ) {
					desired[byte] = ( mask & ( 1u << byte ) ) ? m_pending[offset + byte] : 0u;
				}
			}
			if ( !m_known[m_slot][block] ) {
				++m_initializedBlocks;
			}
			const bool changed =
				!m_known[m_slot][block] || memcmp( desired, m_committed[m_slot] + offset, BLOCK_BYTES ) != 0;
			if ( !changed ) {
				continue;
			}
			memcpy( m_pending + offset, desired, BLOCK_BYTES );
			if ( m_rangeCount &&
				 m_ranges[m_rangeCount - 1u].offset_bytes + m_ranges[m_rangeCount - 1u].size_bytes == offset ) {
				m_ranges[m_rangeCount - 1u].size_bytes += BLOCK_BYTES;
			} else {
				Range &range = m_ranges[m_rangeCount++];
				range.offset_bytes = offset;
				range.size_bytes = BLOCK_BYTES;
				range.source = m_pending + offset;
			}
		}
		m_finalized = true;
		return true;
	}

	uint32_t rangeCount() const {
		return m_finalized ? m_rangeCount : 0u;
	}
	uint32_t initializedBlocks() const {
		return m_finalized ? m_initializedBlocks : 0u;
	}
	uint32_t dirtyBytes() const {
		if ( !m_finalized ) {
			return 0u;
		}
		uint32_t bytes = 0u;
		for ( uint32_t i = 0u; i < m_rangeCount; ++i ) {
			bytes += m_ranges[i].size_bytes;
		}
		return bytes;
	}
	const Range *ranges() const {
		return m_finalized ? m_ranges : nullptr;
	}
	uint32_t activeRecords() const {
		return m_used;
	}
	uint32_t physicalSlot() const {
		return m_slot;
	}
	bool isOpen() const {
		return m_open;
	}

	void abandon() {
		if ( m_open && m_slot < m_slots ) {
			memset( m_known[m_slot], 0, m_blocks );
		}
		m_open = false;
		m_slot = UINT32_MAX;
		m_finalized = false;
		m_rangeCount = m_initializedBlocks = 0u;
	}

	void invalidateAllSlots() {
		for ( uint32_t i = 0u; i < m_slots; ++i ) {
			memset( m_known[i], 0, m_blocks );
		}
	}

	bool finish(
		bool success ) {
		if ( !m_open || !m_finalized ) {
			return false;
		}
		if ( success ) {
			for ( uint32_t i = 0u; i < m_rangeCount; ++i ) {
				const Range &range = m_ranges[i];
				memcpy( m_committed[m_slot] + range.offset_bytes, range.source, range.size_bytes );
				memset( m_known[m_slot] + range.offset_bytes / BLOCK_BYTES, 1u, range.size_bytes / BLOCK_BYTES );
			}
		} else {
			// A failed/partially completed publication is not a usable comparison image.
			memset( m_known[m_slot], 0, m_blocks );
		}
		m_open = false;
		m_slot = UINT32_MAX;
		return success;
	}
};

#endif
