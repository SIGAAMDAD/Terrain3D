#ifndef TERRAIN3D_MEMORY_ACCOUNT_H
#define TERRAIN3D_MEMORY_ACCOUNT_H

#include "core/utility/terrain_3d_sync.h"
#include "memory/policy/terrain_3d_memory_profile.h"
#include <atomic>
#include <new>
#include <stdint.h>

// One account per Terrain3D owner. Reservations precede allocation and survive
// thread/scope changes. The account outlives its node while allocations remain.
class Terrain3DMemoryAccount
{
public:
	enum Kind
	{
		CPU,
		GPU,
		EXTERNAL,
		KIND_COUNT
	};
	// Resource domains are disjoint. Their caps constrain the same total account;
	// they do not reserve additional memory or grant permission to exceed it.
	enum Domain
	{
		GENERAL,
		TERRAIN_RENDER,
		INSTANCE_RENDER,
		VIRTUAL_TEXTURE,
		WORLD_RENDER,
		HLOD_RENDER,
		OCEAN_RENDER,
		FORWARD_LIGHTING,
		COMPOSED_SURFACE,
		DOMAIN_COUNT
	};
	struct DomainStats
	{
		uint64_t limit = 0, committed = 0, peak = 0, denied = 0;
	};
	struct Stats
	{
		uint64_t	limit = 0, committed = 0, peak = 0, denied = 0;
		uint64_t	bytes[KIND_COUNT]{};
		uint64_t	kind_peaks[KIND_COUNT]{};
		DomainStats domains[DOMAIN_COUNT]{};
		uint64_t	cooking_limit = 0;
		uint64_t	editor_cooking_limit = 0;
		bool		editor_authoring = false;
		// Account pressure is distinct from the last calculated budget pressure.
		// Edges are latched across routine page releases and repeated denials.
		bool		pressure_latched = false;
		uint64_t	pressure_events = 0;
		uint64_t	pressure_recovery_events = 0;
		uint64_t	pressure_last_denied_bytes = 0;
		Domain		pressure_last_denied_domain = GENERAL;
	};
	explicit Terrain3DMemoryAccount(
		uint64_t limit ) {
		m_stats.limit = limit;
	}
	void retain() {
		m_references.fetch_add( 1, std::memory_order_relaxed );
	}
	void releaseReference() {
		if ( m_references.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
			delete this;
		}
	}
	bool reserve(
		uint64_t bytes,
		Kind	 kind = CPU,
		Domain	 domain = GENERAL ) {
		Terrain3DScopedLock lock( m_mutex );
		if ( int( kind ) < 0 || kind >= KIND_COUNT || int( domain ) < 0 || domain >= DOMAIN_COUNT ) {
			++m_stats.denied;
			return false;
		}
		auto &bucket = m_stats.domains[domain];
		if ( m_stats.committed > m_stats.limit || bytes > m_stats.limit - m_stats.committed ||
			 ( bucket.limit && ( bucket.committed > bucket.limit || bytes > bucket.limit - bucket.committed ) ) ) {
			++m_stats.denied;
			++bucket.denied;
			notePressureDenialLocked( bytes, domain );
			return false;
		}
		m_stats.committed += bytes;
		m_stats.bytes[kind] += bytes;
		bucket.committed += bytes;
		if ( bucket.committed > bucket.peak ) {
			bucket.peak = bucket.committed;
		}
		if ( m_stats.bytes[kind] > m_stats.kind_peaks[kind] ) {
			m_stats.kind_peaks[kind] = m_stats.bytes[kind];
		}
		if ( m_stats.committed > m_stats.peak ) {
			m_stats.peak = m_stats.committed;
		}
		return true;
	}
	// A bounded operation can reject its working set before requesting any
	// physical allocation. Surface that admission denial for diagnostics/retry.
	void recordDenial(
		Domain domain = GENERAL ) {
		Terrain3DScopedLock lock( m_mutex );
		++m_stats.denied;
		if ( int( domain ) >= 0 && domain < DOMAIN_COUNT ) {
			++m_stats.domains[domain].denied;
			notePressureDenialLocked( 0u, domain );
		}
	}
	void release(
		uint64_t bytes,
		Kind	 kind = CPU,
		Domain	 domain = GENERAL ) {
		Terrain3DScopedLock lock( m_mutex );
		if ( int( kind ) >= 0 && kind < KIND_COUNT && int( domain ) >= 0 && domain < DOMAIN_COUNT &&
			 bytes <= m_stats.bytes[kind] && bytes <= m_stats.domains[domain].committed ) {
			m_stats.bytes[kind] -= bytes;
			m_stats.committed -= bytes;
			m_stats.domains[domain].committed -= bytes;
		}
	}
	// Reject a live reduction that would invalidate resident allocations. The
	// caller can evict/rebuild first; failure leaves both the cap and resources intact.
	bool setDomainLimit(
		Domain	 domain,
		uint64_t limit ) {
		Terrain3DScopedLock lock( m_mutex );
		if ( int( domain ) < 0 || domain >= DOMAIN_COUNT || ( limit && limit < m_stats.domains[domain].committed ) ) {
			return false;
		}
		if ( m_stats.domains[domain].limit != limit ) {
			m_stats.domains[domain].limit = limit;
			resolvePressureIfPossibleLocked();
		}
		return true;
	}
	void setCookingLimit(
		uint64_t limit ) {
		Terrain3DScopedLock lock( m_mutex );
		m_stats.cooking_limit = limit;
	}
	void setEditorCookingLimit(
		uint64_t limit ) {
		Terrain3DScopedLock lock( m_mutex );
		m_stats.editor_cooking_limit = limit;
	}
	// Automatic authoring cooking limits follow the live account profile. Explicit
	// user limits remain authoritative, including during execution-mode switches.
	void setEditorAuthoringPolicy(
		bool enabled ) {
		Terrain3DScopedLock lock( m_mutex );
		m_stats.editor_authoring = enabled;
	}
	bool setLimit(
		uint64_t limit ) {
		Terrain3DScopedLock lock( m_mutex );
		if ( limit < m_stats.committed ) {
			return false;
		}
		if ( m_stats.limit != limit ) {
			m_stats.limit = limit;
			resolvePressureIfPossibleLocked();
		}
		return true;
	}
	// Called only by an owner-cold structural transition. Routine releases do
	// not generate rebalancing requests or continually re-arm pressure events.
	bool acknowledgePressureRecovery() {
		Terrain3DScopedLock lock( m_mutex );
		return resolvePressureIfPossibleLocked();
	}
	Stats stats() const {
		Terrain3DScopedLock lock( m_mutex );
		return m_stats;
	}

private:
	void notePressureDenialLocked( uint64_t bytes, Domain domain ) {
		if ( m_stats.pressure_latched ) {
			return;
		}
		m_stats.pressure_latched = true;
		m_stats.pressure_last_denied_bytes = bytes;
		m_stats.pressure_last_denied_domain = domain;
		++m_stats.pressure_events;
	}
	bool resolvePressureIfPossibleLocked() {
		if ( !m_stats.pressure_latched || m_stats.committed > m_stats.limit ) {
			return false;
		}
		const uint64_t needed = m_stats.pressure_last_denied_bytes;
		// A zero-byte denial did not report its working-set size. Recovery
		// still requires positive headroom, not merely a changed ceiling.
		if ( ( needed == 0u && m_stats.committed == m_stats.limit ) ||
			 needed > m_stats.limit - m_stats.committed ) {
			return false;
		}
		const Domain domain = m_stats.pressure_last_denied_domain;
		const auto &bucket = m_stats.domains[domain];
		if ( bucket.limit && ( bucket.committed > bucket.limit ||
			 ( needed == 0u && bucket.committed == bucket.limit ) ||
			 needed > bucket.limit - bucket.committed ) ) {
			return false;
		}
		m_stats.pressure_latched = false;
		++m_stats.pressure_recovery_events;
		return true;
	}
	std::atomic< uint32_t > m_references{ 1 };
	mutable Terrain3DMutex	m_mutex;
	Stats					m_stats;
};

inline thread_local Terrain3DMemoryAccount *g_t3dMemoryAccount = nullptr;

class Terrain3DMemoryAllocationPermit;
inline thread_local Terrain3DMemoryAllocationPermit *g_t3dMemoryPermit = nullptr;

// EASTL cannot recover from a null allocation with exceptions disabled. Reserve
// its next backing allocation atomically before entering the container, then
// transfer that credit to the native allocation header. Other threads cannot
// consume the preflight headroom between the check and allocation.
class Terrain3DMemoryAllocationPermit
{
	Terrain3DMemoryAccount			*m_account = nullptr;
	Terrain3DMemoryAllocationPermit *m_previous = nullptr;
	uint64_t						 m_available = 0;
	bool							 m_valid = true;

public:
	explicit Terrain3DMemoryAllocationPermit(
		uint64_t bytes ) {
		if ( !g_t3dMemoryAccount || !bytes ) {
			return;
		}
		m_valid = g_t3dMemoryAccount->reserve( bytes );
		if ( !m_valid ) {
			return;
		}
		m_account = g_t3dMemoryAccount;
		m_account->retain();
		m_available = bytes;
		m_previous = g_t3dMemoryPermit;
		g_t3dMemoryPermit = this;
	}
	~Terrain3DMemoryAllocationPermit() {
		if ( m_account ) {
			g_t3dMemoryPermit = m_previous;
			m_account->release( m_available );
			m_account->releaseReference();
		}
	}
	Terrain3DMemoryAllocationPermit( const Terrain3DMemoryAllocationPermit & ) = delete;
	Terrain3DMemoryAllocationPermit &operator=( const Terrain3DMemoryAllocationPermit & ) = delete;
	bool							 valid() const {
		return m_valid;
	}
	static bool consume(
		Terrain3DMemoryAccount *account,
		uint64_t				bytes ) {
		for ( auto *permit = g_t3dMemoryPermit; permit; permit = permit->m_previous ) {
			if ( permit->m_account == account && bytes <= permit->m_available ) {
				permit->m_available -= bytes;
				return true;
			}
		}
		return false;
	}
};

class Terrain3DMemoryAccountScope
{
public:
	explicit Terrain3DMemoryAccountScope(
		Terrain3DMemoryAccount *account ) :
		m_previous( g_t3dMemoryAccount ),
		m_active( account ? account : m_previous ) {
		if ( m_previous ) {
			m_previous->retain();
		}
		if ( m_active && m_active != m_previous ) {
			m_active->retain();
		}
		g_t3dMemoryAccount = m_active;
	}
	~Terrain3DMemoryAccountScope() {
		g_t3dMemoryAccount = m_previous;
		if ( m_active && m_active != m_previous ) {
			m_active->releaseReference();
		}
		if ( m_previous ) {
			m_previous->releaseReference();
		}
	}
	Terrain3DMemoryAccountScope( const Terrain3DMemoryAccountScope & ) = delete;
	Terrain3DMemoryAccountScope &operator=( const Terrain3DMemoryAccountScope & ) = delete;

private:
	Terrain3DMemoryAccount *m_previous;
	Terrain3DMemoryAccount *m_active;
};

class Terrain3DMemoryReservation
{
public:
	Terrain3DMemoryReservation() = default;
	~Terrain3DMemoryReservation() {
		reset();
	}
	Terrain3DMemoryReservation( const Terrain3DMemoryReservation & ) = delete;
	Terrain3DMemoryReservation &operator=( const Terrain3DMemoryReservation & ) = delete;
	Terrain3DMemoryReservation(
		Terrain3DMemoryReservation &&other ) noexcept {
		swap( other );
	}
	Terrain3DMemoryReservation &operator=(
		Terrain3DMemoryReservation &&other ) noexcept {
		if ( this != &other ) {
			reset();
			swap( other );
		}
		return *this;
	}
	bool resize(
		uint64_t					   bytes,
		Terrain3DMemoryAccount::Kind   kind = Terrain3DMemoryAccount::CPU,
		Terrain3DMemoryAccount		  *account = g_t3dMemoryAccount,
		Terrain3DMemoryAccount::Domain domain = Terrain3DMemoryAccount::GENERAL ) {
		if ( !m_account && account ) {
			if ( !account->reserve( bytes, kind, domain ) ) {
				return false;
			}
			m_account = account;
			m_account->retain();
			m_bytes = bytes;
			m_kind = kind;
			m_domain = domain;
			return true;
		}
		if ( !m_account ) {
			return true;
		}
		if ( bytes > m_bytes && !m_account->reserve( bytes - m_bytes, m_kind, m_domain ) ) {
			return false;
		}
		if ( bytes < m_bytes ) {
			m_account->release( m_bytes - bytes, m_kind, m_domain );
		}
		m_bytes = bytes;
		return true;
	}
	void reset() {
		if ( m_account ) {
			m_account->release( m_bytes, m_kind, m_domain );
			m_account->releaseReference();
		}
		m_account = nullptr;
		m_bytes = 0;
	}
	void swap(
		Terrain3DMemoryReservation &other ) noexcept {
		auto *account = m_account;
		m_account = other.m_account;
		other.m_account = account;
		const auto bytes = m_bytes;
		m_bytes = other.m_bytes;
		other.m_bytes = bytes;
		const auto kind = m_kind;
		m_kind = other.m_kind;
		other.m_kind = kind;
		const auto domain = m_domain;
		m_domain = other.m_domain;
		other.m_domain = domain;
	}
	uint64_t bytes() const {
		return m_bytes;
	}

private:
	Terrain3DMemoryAccount		  *m_account = nullptr;
	uint64_t					   m_bytes = 0;
	Terrain3DMemoryAccount::Kind   m_kind = Terrain3DMemoryAccount::CPU;
	Terrain3DMemoryAccount::Domain m_domain = Terrain3DMemoryAccount::GENERAL;
};

// Packed arrays and cache entries share backing through copies. This small
// reference-counted reservation follows that backing without charging aliases.
class Terrain3DSharedMemoryReservation
{
	struct State
	{
		std::atomic< uint32_t >	   references{ 1 };
		Terrain3DMemoryReservation reservation;
	};
	State *m_state = nullptr;

public:
	Terrain3DSharedMemoryReservation() = default;
	~Terrain3DSharedMemoryReservation() {
		reset();
	}
	Terrain3DSharedMemoryReservation(
		const Terrain3DSharedMemoryReservation &other ) :
		m_state( other.m_state ) {
		if ( m_state ) {
			m_state->references.fetch_add( 1, std::memory_order_relaxed );
		}
	}
	Terrain3DSharedMemoryReservation &operator=(
		const Terrain3DSharedMemoryReservation &other ) {
		if ( this != &other ) {
			reset();
			m_state = other.m_state;
			if ( m_state ) {
				m_state->references.fetch_add( 1, std::memory_order_relaxed );
			}
		}
		return *this;
	}
	bool reserve(
		uint64_t bytes ) {
		if ( !g_t3dMemoryAccount || bytes == 0 ) {
			reset();
			return true;
		}
		State *candidate = new ( std::nothrow ) State;
		if ( !candidate ) {
			return false;
		}
		if ( !candidate->reservation.resize( bytes ) ) {
			delete candidate;
			return false;
		}
		reset();
		m_state = candidate;
		return true;
	}
	void reset() {
		if ( m_state && m_state->references.fetch_sub( 1, std::memory_order_acq_rel ) == 1 ) {
			delete m_state;
		}
		m_state = nullptr;
	}
	uint64_t bytes() const {
		return m_state ? m_state->reservation.bytes() : 0;
	}
};

class Terrain3DMemoryAccountOwner
{
public:
	explicit Terrain3DMemoryAccountOwner(
		uint64_t limit ) :
		m_account( new( std::nothrow ) Terrain3DMemoryAccount( limit ) ) {
	}
	~Terrain3DMemoryAccountOwner() {
		if ( m_account ) {
			m_account->releaseReference();
		}
	}
	Terrain3DMemoryAccountOwner( const Terrain3DMemoryAccountOwner & ) = delete;
	Terrain3DMemoryAccountOwner &operator=( const Terrain3DMemoryAccountOwner & ) = delete;
	Terrain3DMemoryAccount		*get() const {
		 return m_account;
	}

private:
	Terrain3DMemoryAccount *m_account;
};
inline uint64_t t3dMemoryCookingLimit(
	Terrain3DMemoryAccount *account ) {
	if ( !account ) {
		return 512ull * 1024ull * 1024ull;
	}
	const auto	   stats = account->stats();
	const uint64_t manual_limit = stats.editor_authoring ? stats.editor_cooking_limit : stats.cooking_limit;
	if ( manual_limit ) {
		return manual_limit < stats.limit ? manual_limit : stats.limit;
	}
	if ( stats.editor_authoring ) {
		const uint64_t editor_limit = uint64_t( T3D_EDITOR_AUTHORING_COOKING_BUDGET_MB ) * 1024ull * 1024ull;
		return stats.limit < editor_limit ? stats.limit : editor_limit;
	}
	const uint64_t limit = stats.limit / 4u;
	return limit < 128ull * 1024ull * 1024ull ? limit : 128ull * 1024ull * 1024ull;
}
// Package composition can borrow shared headroom in automatic mode. Only an
// explicit cooking cap restricts this phase; never reapply the quarter rule.
inline uint64_t t3dMemoryCompositionLimit(
	Terrain3DMemoryAccount *account ) {
	constexpr uint64_t default_limit = 512ull * 1024ull * 1024ull;
	if ( !account ) {
		return default_limit;
	}
	const auto	   stats = account->stats();
	const uint64_t manual_limit = stats.editor_authoring ? stats.editor_cooking_limit : stats.cooking_limit;
	if ( manual_limit ) {
		return manual_limit < stats.limit ? manual_limit : stats.limit;
	}
	const uint64_t editor_limit = uint64_t( T3D_EDITOR_AUTHORING_COOKING_BUDGET_MB ) * 1024ull * 1024ull;
	if ( stats.editor_authoring ) {
		return stats.limit < editor_limit ? stats.limit : editor_limit;
	}
	return default_limit;
}
#endif
