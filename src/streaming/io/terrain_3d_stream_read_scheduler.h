// Copyright (c) 2026 Terrain3D Contributors.
// Chunk-boundary source-I/O admission. Does not own threads or Godot file handles.
#ifndef TERRAIN3D_STREAM_READ_SCHEDULER_H
#define TERRAIN3D_STREAM_READ_SCHEDULER_H

#include "core/utility/terrain_3d_sync.h"
#include <EASTL/array.h>
#include <stdint.h>

class Terrain3DStreamReadScheduler
{
	enum
	{
		MAX_JOBS = 64,
		MAX_CONCURRENT = 2,
		CHUNK_BYTES = 64 * 1024
	};
	struct Job
	{
		uint64_t m_nSequence = 0u;
		uint64_t m_nEnqueuedAtSlice = 0u;
		uint8_t	 m_nPriority = 3u;
		bool	 m_bOccupied = false;
		bool	 m_bReadyForSlice = false;
		bool	 m_bInSlice = false;
	};
	Terrain3DMutex				  m_mutex;
	Terrain3DCondition			  m_changed;
	eastl::array< Job, MAX_JOBS > m_jobs{};
	uint64_t					  m_nSequence = 0u;
	uint64_t					  m_nSlices = 0u;
	uint32_t					  m_nInFlight = 0u;
	uint32_t					  m_nLimit = MAX_CONCURRENT;
	uint32_t					  m_nChunkBytes = CHUNK_BYTES;

public:
	struct Counters
	{
		uint64_t m_nReadChunks = 0u;
		uint64_t m_nWaits = 0u;
		uint64_t m_nPromotions = 0u;
		uint64_t m_nReadBytes = 0u;
	};

private:
	Counters m_counters;
	bool	 isBest(
			const int nIndex ) const {
		const Job &self = m_jobs[( size_t ) nIndex];
		// Aging prevents long-running background reads from being starved by new arrivals.
		const uint64_t age = ( m_nSlices - self.m_nEnqueuedAtSlice ) / 8u;
		const uint32_t effective = age >= self.m_nPriority ? 0u : ( uint32_t ) self.m_nPriority - ( uint32_t ) age;
		for ( int i = 0; i < MAX_JOBS; ++i ) {
			if ( i == nIndex ) {
				continue;
			}
			const Job &other = m_jobs[( size_t ) i];
			if ( !other.m_bOccupied || !other.m_bReadyForSlice || other.m_bInSlice ) {
				continue;
			}
			const uint64_t otherAge = ( m_nSlices - other.m_nEnqueuedAtSlice ) / 8u;
			const uint32_t otherEffective =
				otherAge >= other.m_nPriority ? 0u : ( uint32_t ) other.m_nPriority - ( uint32_t ) otherAge;
			if ( otherEffective < effective ||
				 ( otherEffective == effective && other.m_nSequence < self.m_nSequence ) ) {
				return false;
			}
		}
		return true;
	}

public:
	void configure(
		const uint32_t nConcurrent,
		const uint32_t nChunkBytes ) {
		Terrain3DScopedLock lock( m_mutex );
		m_nLimit = nConcurrent < 1u ? 1u : ( nConcurrent > MAX_CONCURRENT ? MAX_CONCURRENT : nConcurrent );
		m_nChunkBytes = nChunkBytes < 4096u ? 4096u : ( nChunkBytes > 256u * 1024u ? 256u * 1024u : nChunkBytes );
		m_changed.notifyAll();
	}
	uint32_t chunkBytes(
		void ) {
		Terrain3DScopedLock lock( m_mutex );
		return m_nChunkBytes;
	}
	Counters getCounters(
		void ) {
		Terrain3DScopedLock lock( m_mutex );
		return m_counters;
	}
	int begin(
		const uint8_t nPriority ) {
		Terrain3DUniqueLock lock( m_mutex );
		for ( ;; ) {
			for ( int i = 0; i < MAX_JOBS; ++i ) {
				if ( m_jobs[( size_t ) i].m_bOccupied ) {
					continue;
				}
				Job &job = m_jobs[( size_t ) i];
				job = Job();
				job.m_bOccupied = true;
				job.m_nPriority = nPriority > 3u ? 3u : nPriority;
				job.m_nSequence = ++m_nSequence;
				job.m_nEnqueuedAtSlice = m_nSlices;
				m_changed.notifyAll();
				return i;
			}
			++m_counters.m_nWaits;
			m_changed.wait( lock );
		}
	}
	void promote(
		const int	  nJob,
		const uint8_t nPriority ) {
		if ( nJob < 0 || nJob >= MAX_JOBS ) {
			return;
		}
		Terrain3DScopedLock lock( m_mutex );
		Job				   &job = m_jobs[( size_t ) nJob];
		if ( job.m_bOccupied && nPriority < job.m_nPriority ) {
			job.m_nPriority = nPriority;
			++m_counters.m_nPromotions;
			m_changed.notifyAll();
		}
	}
	void acquireSlice(
		const int nJob ) {
		Terrain3DUniqueLock lock( m_mutex );
		Job				   &job = m_jobs[( size_t ) nJob];
		job.m_bReadyForSlice = true;
		for ( ;; ) {
			if ( m_nInFlight < m_nLimit && isBest( nJob ) ) {
				++m_nInFlight;
				job.m_bReadyForSlice = false;
				job.m_bInSlice = true;
				return;
			}
			++m_counters.m_nWaits;
			m_changed.wait( lock );
		}
	}
	void releaseSlice(
		const int	   nJob,
		const uint32_t nBytes ) {
		Terrain3DScopedLock lock( m_mutex );
		Job				   &job = m_jobs[( size_t ) nJob];
		job.m_bInSlice = false;
		job.m_nEnqueuedAtSlice = ++m_nSlices;
		job.m_nSequence = ++m_nSequence;
		--m_nInFlight;
		++m_counters.m_nReadChunks;
		m_counters.m_nReadBytes += nBytes;
		m_changed.notifyAll();
	}
	void finish(
		const int nJob ) {
		Terrain3DScopedLock lock( m_mutex );
		m_jobs[( size_t ) nJob] = Job();
		m_changed.notifyAll();
	}
};

#endif // TERRAIN3D_STREAM_READ_SCHEDULER_H
