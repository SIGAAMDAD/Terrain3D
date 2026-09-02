// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_ACTIVE_ACCESS_H
#define TERRAIN3D_ACTIVE_ACCESS_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_sync.h"
#include "core/terrain_3d_compiler.h"

#include <stdint.h>
// Per-region reader/writer gate for the process-static terrain bank.
// Writers announce themselves before waiting so later readers cannot starve them.
typedef struct t3dActiveAccessGate_s {
	bool acquireRead( void ) {
		uint32_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
		for ( ;; ) {
			if ( ( state & s_nActiveAccessWriterBit ) != 0u ) {
				t3dThreadYield();
				state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
				continue;
			}
			if ( ( state & s_nActiveAccessReaderMask ) == s_nActiveAccessReaderMask ) {
				return false;
			}
			if ( m_state.compare_exchange_weak( state, state + 1u, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
				return true;
			}
		}
	}

	bool releaseRead( void ) {
		uint32_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
		for ( ;; ) {
			if ( ( state & s_nActiveAccessReaderMask ) == 0u ) {
				return false;
			}
			if ( m_state.compare_exchange_weak( state, state - 1u, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
				return true;
			}
		}
	}

	void acquireWrite( void ) {
		uint32_t state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
		for ( ;; ) {
			if ( ( state & s_nActiveAccessWriterBit ) != 0u ) {
				t3dThreadYield();
				state = m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
				continue;
			}
			if ( m_state.compare_exchange_weak(
			             state, state | s_nActiveAccessWriterBit, T3D_MEMORY_ORDER_ACQ_REL, T3D_MEMORY_ORDER_ACQUIRE ) ) {
				break;
			}
		}
		while ( ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) & s_nActiveAccessReaderMask ) != 0u ) {
			t3dThreadYield();
		}
	}

	EA_FORCE_INLINE void releaseWrite( void ) {
		m_state.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	}

	void waitIdle( void ) const {
		while ( m_state.load( T3D_MEMORY_ORDER_ACQUIRE ) != 0u ) {
			t3dThreadYield();
		}
	}

	EA_FORCE_INLINE void resetUnsafe( void ) {
		m_state.store( 0u, T3D_MEMORY_ORDER_RELAXED );
	}

	EA_FORCE_INLINE uint32_t load( void ) const {
		return m_state.load( T3D_MEMORY_ORDER_ACQUIRE );
	}

private:
	t3dAtomic_t<uint32_t> m_state{ 0u };
} t3dActiveAccessGate_t;

#endif // TERRAIN3D_ACTIVE_ACCESS_H
