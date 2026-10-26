// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_STARTUP_STATE_H
#define TERRAIN3D_STARTUP_STATE_H

#include "Terrain3DTypes.h"

#include "core/utility/terrain_3d_sync.h"

#include <stdint.h>

// t3dStartupStage_e centralized in Terrain3DTypes.h.


// t3dStartupDomainSnapshot_s centralized in Terrain3DTypes.h.


// Cold/control-plane record. All mutations and cross-thread reads go through the
// helpers below so stage + generation + progress + timing are observed as one
// coherent tuple. This is intentionally a mutex, not an atomic field fan-out.
typedef struct t3dStartupDomainState_s {

	mutable Terrain3DMutex m_mutex;
	t3dStartupStage_t m_eStage = T3D_STARTUP_IDLE;
	uint32_t m_nGeneration = 0u;
	uint32_t m_nCompletedUnits = 0u;
	uint32_t m_nTotalUnits = 0u;
	uint64_t m_nStartUsec = 0u;
	uint64_t m_nEndUsec = 0u;
} t3dStartupDomainState_t;


static inline t3dStartupDomainSnapshot_t t3dStartupDomainSnapshot( const t3dStartupDomainState_t &rState ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	t3dStartupDomainSnapshot_t snapshot;
	snapshot.m_eStage = rState.m_eStage;
	snapshot.m_nGeneration = rState.m_nGeneration;
	snapshot.m_nCompletedUnits = rState.m_nCompletedUnits;
	snapshot.m_nTotalUnits = rState.m_nTotalUnits;
	snapshot.m_nStartUsec = rState.m_nStartUsec;
	snapshot.m_nEndUsec = rState.m_nEndUsec;
	return snapshot;
}

static inline t3dStartupStage_t t3dStartupDomainGetStage( const t3dStartupDomainSnapshot_t &rState ) {
	return rState.m_eStage;
}

static inline t3dStartupStage_t t3dStartupDomainGetStage( const t3dStartupDomainState_t &rState ) {
	return t3dStartupDomainSnapshot( rState ).m_eStage;
}

static inline void t3dStartupDomainSetStage( t3dStartupDomainState_t &rState, const t3dStartupStage_t eStage ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	rState.m_eStage = eStage;
}

static inline void t3dStartupDomainSetCompletedUnits( t3dStartupDomainState_t &rState, const uint32_t nCompletedUnits ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	rState.m_nCompletedUnits = nCompletedUnits;
}

static inline void t3dStartupDomainSetTotalUnits( t3dStartupDomainState_t &rState, const uint32_t nTotalUnits ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	rState.m_nTotalUnits = nTotalUnits;
}

static inline void t3dStartupDomainSetStartUsecIfZero( t3dStartupDomainState_t &rState, const uint64_t nStartUsec ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	if ( rState.m_nStartUsec == 0u ) {
		rState.m_nStartUsec = nStartUsec;
	}
}

static inline void t3dStartupDomainReset( t3dStartupDomainState_t &rState, const uint32_t nGeneration ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	rState.m_nGeneration = nGeneration;
	rState.m_nCompletedUnits = 0u;
	rState.m_nTotalUnits = 0u;
	rState.m_nStartUsec = 0u;
	rState.m_nEndUsec = 0u;
	rState.m_eStage = T3D_STARTUP_IDLE;
}

static inline void t3dStartupDomainRequest( t3dStartupDomainState_t &rState,
        const uint32_t nGeneration,
        const uint64_t nStartUsec,
        const uint32_t nTotalUnits ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	rState.m_nGeneration = nGeneration;
	rState.m_nCompletedUnits = 0u;
	rState.m_nTotalUnits = nTotalUnits;
	rState.m_nStartUsec = nStartUsec;
	rState.m_nEndUsec = 0u;
	rState.m_eStage = T3D_STARTUP_REQUESTED;
}

static inline void t3dStartupDomainFinish( t3dStartupDomainState_t &rState,
        const t3dStartupStage_t eStage,
        const uint64_t nEndUsec ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	rState.m_nEndUsec = nEndUsec;
	rState.m_eStage = eStage;
}

static inline bool t3dStartupStageIsTerminal( const t3dStartupStage_t eStage ) {
	return eStage == T3D_STARTUP_COMMITTED || eStage == T3D_STARTUP_FAILED || eStage == T3D_STARTUP_CANCELLED;
}

static inline bool t3dStartupDomainMatches( const t3dStartupDomainSnapshot_t &rState, const uint32_t nGeneration ) {
	return nGeneration != 0u && rState.m_nGeneration == nGeneration;
}

static inline bool t3dStartupDomainMatches( const t3dStartupDomainState_t &rState, const uint32_t nGeneration ) {
	return t3dStartupDomainMatches( t3dStartupDomainSnapshot( rState ), nGeneration );
}

static inline uint64_t t3dStartupDomainElapsedUsec( const t3dStartupDomainSnapshot_t &rState, const uint64_t nNowUsec ) {
	if ( rState.m_nStartUsec == 0u ) {
		return 0u;
	}
	const uint64_t nEndUsec = rState.m_nEndUsec != 0u ? rState.m_nEndUsec : nNowUsec;
	return nEndUsec >= rState.m_nStartUsec ? nEndUsec - rState.m_nStartUsec : 0u;
}

static inline uint64_t t3dStartupDomainElapsedUsec( const t3dStartupDomainState_t &rState, const uint64_t nNowUsec ) {
	return t3dStartupDomainElapsedUsec( t3dStartupDomainSnapshot( rState ), nNowUsec );
}

static inline bool t3dStartupDomainCancel( t3dStartupDomainState_t &rState,
        const uint32_t nGeneration,
        const uint64_t nEndUsec ) {
	Terrain3DScopedLock lock( rState.m_mutex );
	if ( nGeneration == 0u || rState.m_nGeneration != nGeneration ) {
		return false;
	}
	if ( rState.m_eStage == T3D_STARTUP_FAILED || rState.m_eStage == T3D_STARTUP_CANCELLED ) {
		return false;
	}
	rState.m_nEndUsec = nEndUsec;
	rState.m_eStage = T3D_STARTUP_CANCELLED;
	return true;
}

static inline const char *t3dStartupStageName( const t3dStartupStage_t eStage ) {
	switch ( eStage ) {
		case T3D_STARTUP_IDLE:
			return "IDLE";
		case T3D_STARTUP_REQUESTED:
			return "REQUESTED";
		case T3D_STARTUP_PREPARING:
			return "PREPARING";
		case T3D_STARTUP_CPU_READY:
			return "CPU_READY";
		case T3D_STARTUP_GPU_PENDING:
			return "GPU_PENDING";
		case T3D_STARTUP_GPU_READY:
			return "GPU_READY";
		case T3D_STARTUP_COMMITTED:
			return "COMMITTED";
		case T3D_STARTUP_FAILED:
			return "FAILED";
		case T3D_STARTUP_CANCELLED:
			return "CANCELLED";
		default:
			return "UNKNOWN";
	}
}

#endif // TERRAIN3D_STARTUP_STATE_H
