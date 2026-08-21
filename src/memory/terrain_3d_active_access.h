// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_ACTIVE_ACCESS_H
#define TERRAIN3D_ACTIVE_ACCESS_H

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

#include <atomic>
#include <cstdint>
#include <thread>


// Per-region reader/writer gate for the process-static terrain bank.
// Writers announce themselves before waiting so later readers cannot starve them.
typedef struct t3dActiveAccessGate_s {
	bool acquireRead( void ) {
		uint32_t state = m_state.load( std::memory_order_acquire );
		for ( ;; ) {
			if ( ( state & TERRAIN3D_ACTIVE_ACCESS_WRITER_BIT ) != 0u ) {
				std::this_thread::yield();
				state = m_state.load( std::memory_order_acquire );
				continue;
			}
			if ( ( state & TERRAIN3D_ACTIVE_ACCESS_READER_MASK ) == TERRAIN3D_ACTIVE_ACCESS_READER_MASK ) {
				return false;
			}
			if ( m_state.compare_exchange_weak( state, state + 1u, std::memory_order_acq_rel,
				std::memory_order_acquire ) ) {
				return true;
			}
		}
	}

	bool releaseRead( void ) {
		uint32_t state = m_state.load( std::memory_order_acquire );
		for ( ;; ) {
			if ( ( state & TERRAIN3D_ACTIVE_ACCESS_READER_MASK ) == 0u ) {
				return false;
			}
			if ( m_state.compare_exchange_weak( state, state - 1u, std::memory_order_acq_rel,
				std::memory_order_acquire ) ) {
				return true;
			}
		}
	}

	void acquireWrite( void ) {
		uint32_t state = m_state.load( std::memory_order_acquire );
		for ( ;; ) {
			if ( ( state & TERRAIN3D_ACTIVE_ACCESS_WRITER_BIT ) != 0u ) {
				std::this_thread::yield();
				state = m_state.load( std::memory_order_acquire );
				continue;
			}
			if ( m_state.compare_exchange_weak( state, state | TERRAIN3D_ACTIVE_ACCESS_WRITER_BIT, std::memory_order_acq_rel,
				std::memory_order_acquire ) ) {
				break;
			}
		}
		while ( ( m_state.load( std::memory_order_acquire ) & TERRAIN3D_ACTIVE_ACCESS_READER_MASK ) != 0u ) {
			std::this_thread::yield();
		}
	}

	EA_FORCE_INLINE void releaseWrite( void ) { m_state.store( 0u, std::memory_order_release ); }

	void waitIdle( void ) const {
		while ( m_state.load( std::memory_order_acquire ) != 0u ) {
			std::this_thread::yield();
		}
	}

	EA_FORCE_INLINE void resetUnsafe( void ) { m_state.store( 0u, std::memory_order_relaxed ); }

	EA_FORCE_INLINE uint32_t load( void ) const { return m_state.load( std::memory_order_acquire ); }

private:
	std::atomic<uint32_t> m_state{0u};
} t3dActiveAccessGate_t;

#endif // TERRAIN3D_ACTIVE_ACCESS_H
