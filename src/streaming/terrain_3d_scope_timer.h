#ifndef TERRAIN3D_SCOPE_TIMER_H
#define TERRAIN3D_SCOPE_TIMER_H

#include <stdint.h>
#include <godot_cpp/classes/time.hpp>

// Records the duration of one optional scope without heap allocation.
class Terrain3DLastScopeTimer
{
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;

public:
	Terrain3DLastScopeTimer( uint64_t &rElapsedUsec, const bool bEnabled ) {
		rElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &rElapsedUsec;
		godot::Time *const time = godot::Time::get_singleton();
		m_nStartedUsec = time ? time->get_ticks_usec() : 0u;
	}

	~Terrain3DLastScopeTimer() {
		if ( !m_pElapsedUsec ) {
			return;
		}
		godot::Time *const time = godot::Time::get_singleton();
		const uint64_t finished = time ? time->get_ticks_usec() : m_nStartedUsec;
		*m_pElapsedUsec = finished >= m_nStartedUsec ? finished - m_nStartedUsec : 0u;
	}

	Terrain3DLastScopeTimer( const Terrain3DLastScopeTimer & ) = delete;
	Terrain3DLastScopeTimer &operator=( const Terrain3DLastScopeTimer & ) = delete;
};

// Accumulates the longest observed scope and request-to-completion duration.
class Terrain3DProfileScopeTimer
{
	uint64_t m_nStartedUsec = 0u;
	uint64_t &m_rElapsedUsec;
	const uint64_t &m_rRequestUsec;
	uint64_t &m_rEndToEndUsec;

public:
	Terrain3DProfileScopeTimer( uint64_t &rElapsedUsec, const uint64_t &rRequestUsec, uint64_t &rEndToEndUsec ) :
	        m_rElapsedUsec( rElapsedUsec ), m_rRequestUsec( rRequestUsec ), m_rEndToEndUsec( rEndToEndUsec ) {
		godot::Time *const time = godot::Time::get_singleton();
		m_nStartedUsec = time ? time->get_ticks_usec() : 0u;
	}

	~Terrain3DProfileScopeTimer() {
		godot::Time *const time = godot::Time::get_singleton();
		const uint64_t finished = time ? time->get_ticks_usec() : m_nStartedUsec;
		const uint64_t elapsed = finished >= m_nStartedUsec ? finished - m_nStartedUsec : 0u;
		m_rElapsedUsec = m_rElapsedUsec < elapsed ? elapsed : m_rElapsedUsec;
		if ( m_rRequestUsec > 0u && finished >= m_rRequestUsec ) {
			const uint64_t end_to_end = finished - m_rRequestUsec;
			m_rEndToEndUsec = m_rEndToEndUsec < end_to_end ? end_to_end : m_rEndToEndUsec;
		}
	}

	Terrain3DProfileScopeTimer( const Terrain3DProfileScopeTimer & ) = delete;
	Terrain3DProfileScopeTimer &operator=( const Terrain3DProfileScopeTimer & ) = delete;
};

#endif // TERRAIN3D_SCOPE_TIMER_H
