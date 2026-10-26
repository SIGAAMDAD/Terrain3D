#include "diagnostics/core/terrain_3d_streaming_profiler.h"
// Copyright (c) 2026 Terrain3D contributors.
// P08: bounded admission before immutable streaming upload units are committed.
#ifndef TERRAIN3D_GPU_TRANSFER_BUDGET_H
#define TERRAIN3D_GPU_TRANSFER_BUDGET_H

#include <stdint.h>
#include <chrono>
#include <mutex>

enum t3dGpuTransferLane_t : uint32_t
{
	T3D_GPU_TRANSFER_VT = 0u,
	T3D_GPU_TRANSFER_REGION = 1u,
	T3D_GPU_TRANSFER_WORLD = 2u,
	T3D_GPU_TRANSFER_FEEDBACK = 3u,
	T3D_GPU_TRANSFER_LANE_COUNT = 4u
};

struct t3dGpuTransferTicket_t
{
	t3dGpuTransferTicket_t() = default;
	t3dGpuTransferTicket_t( const t3dGpuTransferTicket_t & ) = delete;
	t3dGpuTransferTicket_t &operator=( const t3dGpuTransferTicket_t & ) = delete;
	t3dGpuTransferTicket_t( t3dGpuTransferTicket_t &&rOther ) noexcept {
		moveFrom( rOther );
	}
	t3dGpuTransferTicket_t &operator=( t3dGpuTransferTicket_t &&rOther ) noexcept {
		if ( this != &rOther ) {
			moveFrom( rOther );
		}
		return *this;
	}
	uint64_t bytes() const { return m_nBytes; }
	uint32_t calls() const { return m_nCalls; }
	bool granted() const { return m_bGranted; }

private:
	friend class Terrain3DGpuTransferBudget;
	void clear() {
		m_nWindow = 0u;
		m_nBytes = 0u;
		m_nCalls = 0u;
		m_nLane = 0u;
		m_bGranted = false;
	}
	void moveFrom( t3dGpuTransferTicket_t &rOther ) {
		m_nWindow = rOther.m_nWindow;
		m_nBytes = rOther.m_nBytes;
		m_nCalls = rOther.m_nCalls;
		m_nLane = rOther.m_nLane;
		m_bGranted = rOther.m_bGranted;
		rOther.clear();
	}
	uint64_t m_nWindow = 0u;
	uint64_t m_nBytes = 0u;
	uint32_t m_nCalls = 0u;
	uint32_t m_nLane = 0u;
	bool m_bGranted = false;
};

struct t3dGpuTransferSnapshot_t
{
	uint64_t nWindow = 0u;
	uint64_t nBytes = 0u;
	uint32_t nCalls = 0u;
	uint64_t nLaneBytes[T3D_GPU_TRANSFER_LANE_COUNT] = {};
	uint64_t nLaneCalls[T3D_GPU_TRANSFER_LANE_COUNT] = {};
	uint64_t nDeferred[T3D_GPU_TRANSFER_LANE_COUNT] = {};
	uint64_t nOversizedBypass[T3D_GPU_TRANSFER_LANE_COUNT] = {};
	uint64_t nAcceptedBytesTotal[T3D_GPU_TRANSFER_LANE_COUNT] = {};
	uint64_t nAcceptedCallsTotal[T3D_GPU_TRANSFER_LANE_COUNT] = {};
	uint64_t nWindowPeakBytes = 0u;
	uint32_t nWindowPeakCalls = 0u;
};

class Terrain3DGpuTransferBudget
{
public:
	static constexpr uint64_t BYTE_LIMIT = 12ull * 1024ull * 1024ull;
	static constexpr uint32_t CALL_LIMIT = 96u;
	static constexpr uint64_t WINDOW_USEC = 16000ull;

	static Terrain3DGpuTransferBudget &get() {
		static Terrain3DGpuTransferBudget owner;
		return owner;
	}
	bool reserve(
		t3dGpuTransferLane_t	lane,
		uint64_t				bytes,
		uint32_t				calls,
		t3dGpuTransferTicket_t &ticket ) {
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_BUDGET_ADMISSION );
		ticket.clear();
		if ( lane >= T3D_GPU_TRANSFER_LANE_COUNT ) {
			return false;
		}
		const bool measureWait = Terrain3DStreamingProfiler::get().enter( T3D_SP_H12_BUDGET_LOCK_WAIT );
		const auto startedWait = measureWait ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		std::lock_guard< std::mutex > lock( m_mutex );
		if ( measureWait ) {
			const auto ns = std::chrono::duration_cast< std::chrono::nanoseconds >(
				std::chrono::steady_clock::now() - startedWait ).count();
			Terrain3DStreamingProfiler::get().record( T3D_SP_H12_BUDGET_LOCK_WAIT, ns > 0 ? ( uint64_t ) ns : 0u );
		}
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_H12_BUDGET_LOCK_HOLD );
		advance( nowWindow() );
		if ( bytes > BYTE_LIMIT || calls > CALL_LIMIT ) {
			++m_snapshot.nDeferred[lane];
			return false;
		}
		if ( m_snapshot.nBytes > BYTE_LIMIT - bytes || m_snapshot.nCalls > CALL_LIMIT - calls ) {
			++m_snapshot.nDeferred[lane];
			return false;
		}
		m_snapshot.nBytes += bytes;
		m_snapshot.nCalls += calls;
		m_snapshot.nLaneBytes[lane] += bytes;
		m_snapshot.nLaneCalls[lane] += calls;
		m_snapshot.nAcceptedBytesTotal[lane] += bytes;
		m_snapshot.nAcceptedCallsTotal[lane] += calls;
		if ( m_snapshot.nBytes > m_snapshot.nWindowPeakBytes ) {
			m_snapshot.nWindowPeakBytes = m_snapshot.nBytes;
		}
		if ( m_snapshot.nCalls > m_snapshot.nWindowPeakCalls ) {
			m_snapshot.nWindowPeakCalls = m_snapshot.nCalls;
		}
		ticket.m_nWindow = m_snapshot.nWindow;
		ticket.m_nBytes = bytes;
		ticket.m_nCalls = calls;
		ticket.m_nLane = lane;
		ticket.m_bGranted = true;
		return true;
	}
	// Report legacy atomic publication units that exceed the entire budget.
	// Only bootstrap/recovery owners may bypass, never regular VT/region uploads.
	void recordOversizedBypass(
		t3dGpuTransferLane_t lane ) {
		if ( lane >= T3D_GPU_TRANSFER_LANE_COUNT ) {
			return;
		}
		std::lock_guard< std::mutex > lock( m_mutex );
		++m_snapshot.nOversizedBypass[lane];
	}
	// A reservation is a single-use accounting authority. Calling refund again
	// (including after moving the ticket) cannot return credit twice.
	void refund(
		t3dGpuTransferTicket_t &ticket,
		uint64_t unusedBytes,
		uint32_t unusedCalls ) {
		std::lock_guard< std::mutex > lock( m_mutex );
		if ( !ticket.m_bGranted ) {
			return;
		}
		const uint64_t bytes = unusedBytes < ticket.m_nBytes ? unusedBytes : ticket.m_nBytes;
		const uint32_t calls = unusedCalls < ticket.m_nCalls ? unusedCalls : ticket.m_nCalls;
		const uint32_t lane = ticket.m_nLane;
		const bool valid = lane < T3D_GPU_TRANSFER_LANE_COUNT && ticket.m_nWindow == m_snapshot.nWindow &&
			m_snapshot.nBytes >= bytes && m_snapshot.nCalls >= calls &&
			( lane >= T3D_GPU_TRANSFER_LANE_COUNT || ( m_snapshot.nLaneBytes[lane] >= bytes &&
			  m_snapshot.nLaneCalls[lane] >= calls ) );
		ticket.clear();
		if ( !valid ) {
			return;
		}
		m_snapshot.nBytes -= bytes;
		m_snapshot.nCalls -= calls;
		m_snapshot.nLaneBytes[lane] -= bytes;
		m_snapshot.nLaneCalls[lane] -= calls;
	}
	t3dGpuTransferSnapshot_t snapshot() {
		std::lock_guard< std::mutex > lock( m_mutex );
		advance( nowWindow() );
		return m_snapshot;
	}
#ifdef T3D_GPU_BUDGET_TEST_MAIN
	// Fixed synthetic window order for deterministic stale-thread regression.
	t3dGpuTransferSnapshot_t snapshotForWindowForTest( uint64_t window ) {
		std::lock_guard< std::mutex > lock( m_mutex );
		advance( window );
		return m_snapshot;
	}
#endif

private:
	static uint64_t nowWindow() {
		using clock = std::chrono::steady_clock;
		return ( uint64_t ) std::chrono::duration_cast< std::chrono::microseconds >( clock::now().time_since_epoch() )
				   .count() /
			   WINDOW_USEC;
	}
	void advance(
		uint64_t window ) {
		if ( window <= m_snapshot.nWindow ) {
			return;
		}
		const t3dGpuTransferSnapshot_t previous = m_snapshot;
		m_snapshot = t3dGpuTransferSnapshot_t{};
		m_snapshot.nWindow = window;
		m_snapshot.nWindowPeakBytes = previous.nWindowPeakBytes;
		m_snapshot.nWindowPeakCalls = previous.nWindowPeakCalls;
		for ( uint32_t i = 0u; i < T3D_GPU_TRANSFER_LANE_COUNT; ++i ) {
			m_snapshot.nDeferred[i] = previous.nDeferred[i];
			m_snapshot.nOversizedBypass[i] = previous.nOversizedBypass[i];
			m_snapshot.nAcceptedBytesTotal[i] = previous.nAcceptedBytesTotal[i];
			m_snapshot.nAcceptedCallsTotal[i] = previous.nAcceptedCallsTotal[i];
		}
	}
	std::mutex				 m_mutex;
	t3dGpuTransferSnapshot_t m_snapshot;
};
#endif
