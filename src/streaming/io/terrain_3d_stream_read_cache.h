#include "diagnostics/core/terrain_3d_streaming_profiler.h"
// Copyright (c) 2026 Terrain3D Contributors.
// Immutable cooked source bytes. This cache never owns decoded terrain images or GPU pages.
#ifndef TERRAIN3D_STREAM_READ_CACHE_H
#define TERRAIN3D_STREAM_READ_CACHE_H

#include "core/utility/terrain_3d_sync.h"
#include "streaming/io/terrain_3d_stream_read_scheduler.h"
#include "streaming/io/terrain_3d_stream_range_key.h"
#include "streaming/io/terrain_3d_cold_chunk_format.h"
#include "streaming/io/terrain_3d_shared_stream_pack.h"
#include "data/runtime/terrain_3d_runtime_page_format.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <EASTL/array.h>
#include <stdint.h>
#include <string.h>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

struct t3dStreamIoStats_t
{
	uint64_t m_nOpenCalls = 0u;
	uint64_t m_nRangeReadCalls = 0u;
	uint64_t m_nRangeReadBytes = 0u;
	uint64_t m_nDistinctRangeBytes = 0u;
	uint64_t m_nDuplicateRangeReads = 0u;
	uint64_t m_nDuplicateRangeBytes = 0u;
	uint64_t m_nCacheHits = 0u;
	uint64_t m_nInFlightJoins = 0u;
	uint64_t m_nCacheHitBytes = 0u;
	uint64_t m_nFailedReads = 0u;
	uint64_t m_nEvictions = 0u;
	uint64_t m_nMetadataHits = 0u;
	uint64_t m_nMetadataReads = 0u;
	uint64_t m_nResidentBytes = 0u;
	uint64_t m_nPackHits = 0u;
	uint64_t m_nPackOpens = 0u;
	uint64_t m_nReadChunks = 0u;
	uint64_t m_nReadWaits = 0u;
	uint64_t m_nReadPromotions = 0u;
};

// Per-page-store bounded cache. One owner reads a missing range; joiners wait for
// validation, receive immutable PackedByteArray leases, and never issue a second read.
// Reader threads may not call clear() until the page store has stopped workers.
class Terrain3DStreamReadCache
{
	enum
	{
		ENTRY_COUNT = 128
	};
	struct Entry
	{
		t3dStreamRangeKey_t m_key;
		PackedByteArray		m_stored;
		uint64_t			m_nLastUse = 0u;
		int					m_nSchedulerTicket = -1;
		bool				m_bReading = false;
		bool				m_bValid = false;
		bool				m_bFailed = false;
		uint64_t			m_nRetryAfter = 0u;
	};
	mutable Terrain3DMutex					 m_mutex;
	mutable Terrain3DStreamReadScheduler	 m_scheduler;
	Terrain3DSharedStreamPack				 m_sharedPack;
	bool									 m_bPackRejected = false;
	Terrain3DCondition						 m_ready;
	eastl::array< Entry, ENTRY_COUNT >		 m_entries{};
	eastl::array< t3dStreamRangeKey_t, 256 > m_seen{};
	size_t									 m_nSeenCount = 0u;
	size_t									 m_nSeenNext = 0u;
	uint64_t								 m_nClock = 0u;
	uint64_t								 m_nEpoch = 1u;
	uint64_t								 m_nBudget = 8u * 1024u * 1024u;
	struct MetadataEntry
	{
		String				   m_path;
		uint64_t			   m_nModified = 0u;
		uint64_t			   m_nLength = 0u;
		t3dRuntimePageHeader_t m_header{};
		uint64_t			   m_nUse = 0u;
	};
	eastl::array< MetadataEntry, 64 > m_metadata{};
	t3dStreamIoStats_t				  m_stats;

	static bool verify(
		const PackedByteArray	  &rStored,
		const t3dStreamRangeKey_t &rKey ) {
		if ( rStored.size() != ( int64_t ) rKey.m_nStoredBytes ) {
			return false;
		}
		if ( rKey.m_nCompression == TERRAIN3D_RUNTIME_PAGE_COMPRESSION_NONE ) {
			return rKey.m_nStoredBytes == rKey.m_nDecodedBytes &&
				   t3dRuntimePageChecksum( rStored.ptr(), rStored.size() ) == rKey.m_nChecksum;
		}
		if ( rKey.m_nCompression != TERRAIN3D_RUNTIME_PAGE_COMPRESSION_RLE || rKey.m_nDecodedBytes == 0u ) {
			return false;
		}
		PackedByteArray decoded;
		decoded.resize( rKey.m_nDecodedBytes );
		if ( decoded.size() != ( int64_t ) rKey.m_nDecodedBytes ) {
			return false;
		}
		return t3dColdChunkDecodeRle( rStored.ptr(), rKey.m_nStoredBytes,
			decoded.ptrw(), rKey.m_nDecodedBytes ) &&
			t3dRuntimePageChecksum( decoded.ptr(), rKey.m_nDecodedBytes ) == rKey.m_nChecksum;
	}

public:
	Terrain3DStreamReadCache( void ) = default;
	Terrain3DStreamReadCache( const Terrain3DStreamReadCache & ) = delete;
	Terrain3DStreamReadCache &operator=( const Terrain3DStreamReadCache & ) = delete;

	void clear(
		void ) {
		Terrain3DScopedLock lock( m_mutex );
		for ( Entry &entry : m_entries ) {
			entry = Entry();
		}
		for ( MetadataEntry &entry : m_metadata ) {
			entry = MetadataEntry();
		}
		m_stats = t3dStreamIoStats_t();
		m_bPackRejected = false;
		m_nClock = 0u;
		m_nSeenCount = 0u;
		m_nSeenNext = 0u;
		++m_nEpoch;
		m_ready.notifyAll();
	}
	void setBudget(
		uint64_t nBytes ) {
		Terrain3DScopedLock lock( m_mutex );
		m_nBudget = nBytes;
		// Evict only ready, unleased cached copies; PackedByteArray leases stay valid.
		while ( m_stats.m_nResidentBytes > m_nBudget ) {
			int victim = -1;
			for ( int i = 0; i < ENTRY_COUNT; ++i ) {
				const Entry &entry = m_entries[( size_t ) i];
				if ( !entry.m_bValid || entry.m_bReading ) {
					continue;
				}
				if ( victim < 0 || entry.m_nLastUse < m_entries[( size_t ) victim].m_nLastUse ) {
					victim = i;
				}
			}
			if ( victim < 0 ) {
				break;
			}
			m_stats.m_nResidentBytes -= ( uint64_t ) m_entries[( size_t ) victim].m_stored.size();
			m_entries[( size_t ) victim] = Entry();
			++m_stats.m_nEvictions;
		}
	}
	// Configure only while page-store reader workers are stopped.
	// Cold-loaded T3PM index; safe for worker reads after publisher quiescence.
	bool getPackPageMetadata(
		const String	&rPath,
		PackedByteArray &rBytes,
		uint64_t		&rLength ) const {
		return m_sharedPack.lookupPage( rPath, rBytes, rLength );
	}
	bool setSharedPackPath(
		const String &rPath ) {
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_H09_PACK_REMOUNT );
		if ( rPath.is_empty() ) {
			m_sharedPack.close();
			clear();
			return true;
		}
		Terrain3DSharedStreamPack candidate;
		T3D_STREAM_PROFILE_WORK( T3D_SP_H09_PACK_REMOUNT, 1u );
		if ( !candidate.open( rPath ) ) {
			return false;
		}
		clear();
		// Candidate has already parsed and verified the chosen immutable
		// generation. Adopt it; no redundant second open/metadata pass.
		m_sharedPack.swapState( candidate );
		return true;
	}
	void configureIoScheduling(
		uint32_t nConcurrent,
		uint32_t nChunkBytes ) {
		m_scheduler.configure( nConcurrent, nChunkBytes );
	}
	Terrain3DStreamReadScheduler::Counters getScheduleStats(
		void ) {
		return m_scheduler.getCounters();
	}
	t3dStreamIoStats_t getStats(
		void ) const {
		Terrain3DScopedLock							 lock( m_mutex );
		t3dStreamIoStats_t							 stats = m_stats;
		const Terrain3DStreamReadScheduler::Counters slices = m_scheduler.getCounters();
		stats.m_nReadChunks = slices.m_nReadChunks;
		stats.m_nReadWaits = slices.m_nWaits;
		stats.m_nReadPromotions = slices.m_nPromotions;
		return stats;
	}
	void recordOpen(
		void ) {
		Terrain3DScopedLock lock( m_mutex );
		++m_stats.m_nOpenCalls;
	}
	uint64_t getEpoch(
		void ) const {
		Terrain3DScopedLock lock( m_mutex );
		return m_nEpoch;
	}
	void recordMetadataHit(
		void ) {
		Terrain3DScopedLock lock( m_mutex );
		++m_stats.m_nMetadataHits;
	}
	void recordMetadataRead(
		void ) {
		Terrain3DScopedLock lock( m_mutex );
		++m_stats.m_nMetadataReads;
	}

	bool getMetadata(
		const String		   &rPath,
		uint64_t				nModified,
		uint64_t				nLength,
		t3dRuntimePageHeader_t &rHeader,
		uint64_t			   *pCachedLength = NULL ) {
		if ( nModified == 0u ) {
			return false;
		}
		Terrain3DScopedLock lock( m_mutex );
		for ( MetadataEntry &entry : m_metadata ) {
			if ( entry.m_nUse && entry.m_path == rPath && entry.m_nModified == nModified &&
				 ( nLength == 0u || entry.m_nLength == nLength ) ) {
				entry.m_nUse = ++m_nClock;
				rHeader = entry.m_header;
				if ( pCachedLength ) {
					*pCachedLength = entry.m_nLength;
				}
				++m_stats.m_nMetadataHits;
				return true;
			}
		}
		return false;
	}
	void putMetadata(
		const String				 &rPath,
		uint64_t					  nModified,
		uint64_t					  nLength,
		const t3dRuntimePageHeader_t &rHeader ) {
		if ( nModified == 0u ) {
			return;
		}
		Terrain3DScopedLock lock( m_mutex );
		size_t				slot = 0;
		for ( size_t i = 0; i < m_metadata.size(); ++i ) {
			if ( m_metadata[i].m_nUse == 0u ||
				 ( m_metadata[i].m_path == rPath && m_metadata[i].m_nModified == nModified ) ) {
				slot = i;
				break;
			}
			if ( m_metadata[i].m_nUse < m_metadata[slot].m_nUse ) {
				slot = i;
			}
		}
		MetadataEntry &entry = m_metadata[slot];
		entry.m_path = rPath;
		entry.m_nModified = nModified;
		entry.m_nLength = nLength;
		entry.m_header = rHeader;
		entry.m_nUse = ++m_nClock;
		++m_stats.m_nMetadataReads;
	}
	bool readStored(
		Ref< FileAccess >		  &rFile,
		const t3dStreamRangeKey_t &rKey,
		PackedByteArray			  &rResult,
		uint64_t				  &rReadUsec,
		uint64_t				  &rValidationUsec ) {
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_REGION_IO_READ );
		rResult = PackedByteArray();
		if ( rKey.m_nStoredBytes == 0u || rKey.m_nDecodedBytes == 0u ||
			 rKey.m_nStoredBytes > T3D_RUNTIME_PAGE_MAX_BYTES || rKey.m_nDecodedBytes > T3D_RUNTIME_PAGE_MAX_BYTES ||
			 rKey.m_nOffset > rKey.m_nFileLength || rKey.m_nStoredBytes > rKey.m_nFileLength - rKey.m_nOffset ) {
			return false;
		}
		// The packed artifact is authoritative: never fall back to an old
		// standalone page after a rejected or missing content entry.
		t3dStreamRangeKey_t sourceKey = rKey;
		Ref< FileAccess >	sourceFile = rFile;
		bool				packAllowed = false;
		{
			Terrain3DScopedLock lock( m_mutex );
			packAllowed = !m_bPackRejected;
		}
		bool usedPack = packAllowed && m_sharedPack.resolve( rKey, sourceKey );
		if ( !usedPack ) {
			return false;
		}
		if ( usedPack ) {
			Terrain3DScopedLock lock( m_mutex );
			++m_stats.m_nPackHits;
		}
		bool cacheable;
		{
			Terrain3DScopedLock lock( m_mutex );
			cacheable = m_nBudget != 0u && sourceKey.m_nStoredBytes <= m_nBudget && sourceKey.m_nModifiedTime != 0u;
		}
		int		 slot = -1;
		uint64_t epoch = 0u;
		{
			Terrain3DScopedLock lock( m_mutex );
			epoch = m_nEpoch;
		}
		if ( cacheable ) {
			T3D_STREAM_PROFILE_SCOPE( T3D_SP_H02_CACHE_SCAN );
			Terrain3DStreamingProfileWorkScope scanned( T3D_SP_H02_CACHE_SCAN );
			const bool measureLockWait = Terrain3DStreamingProfiler::get().enter( T3D_SP_H02_MUTEX_WAIT );
			const auto waitStarted = measureLockWait ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			Terrain3DUniqueLock lock( m_mutex );
			if ( measureLockWait ) {
				const auto ns = std::chrono::duration_cast< std::chrono::nanoseconds >(
					std::chrono::steady_clock::now() - waitStarted ).count();
				Terrain3DStreamingProfiler::get().record( T3D_SP_H02_MUTEX_WAIT, ns > 0 ? ( uint64_t ) ns : 0u );
			}
			epoch = m_nEpoch;
			for ( ;; ) {
				int waiting = -1;
				for ( int i = 0; i < ENTRY_COUNT; ++i ) {
					scanned.add();
					Entry &entry = m_entries[( size_t ) i];
					if ( !( entry.m_bReading || entry.m_bValid || entry.m_bFailed ) || !( entry.m_key == sourceKey ) ) {
						continue;
					}
					if ( entry.m_bFailed ) {
						if ( t3dSteadyClockUsec() < entry.m_nRetryAfter ) {
							return false;
						}
						entry = Entry();
						continue;
					}
					if ( entry.m_bReading ) {
						if ( sourceKey.m_nPriority < entry.m_key.m_nPriority ) {
							entry.m_key.m_nPriority = sourceKey.m_nPriority;
							m_scheduler.promote( entry.m_nSchedulerTicket, sourceKey.m_nPriority );
						}
						waiting = i;
						break;
					}
					entry.m_nLastUse = ++m_nClock;
					rResult = entry.m_stored;
					++m_stats.m_nCacheHits;
					m_stats.m_nCacheHitBytes += sourceKey.m_nStoredBytes;
					return true;
				}
				if ( waiting >= 0 ) {
					++m_stats.m_nInFlightJoins;
					{
						T3D_STREAM_PROFILE_SCOPE( T3D_SP_H02_CACHE_WAIT );
						m_ready.wait( lock );
					}
					continue;
				}
				for ( int i = 0; i < ENTRY_COUNT; ++i ) {
					scanned.add();
					if ( !m_entries[( size_t ) i].m_bReading && !m_entries[( size_t ) i].m_bValid &&
						 !m_entries[( size_t ) i].m_bFailed ) {
						slot = i;
						break;
					}
				}
				if ( slot < 0 ) {
					for ( int i = 0; i < ENTRY_COUNT; ++i ) {
						scanned.add();
						if ( !m_entries[( size_t ) i].m_bReading &&
							 ( slot < 0 ||
							   m_entries[( size_t ) i].m_nLastUse < m_entries[( size_t ) slot].m_nLastUse ) ) {
							slot = i;
						}
					}
				}
				if ( slot >= 0 ) {
					Entry &entry = m_entries[( size_t ) slot];
					if ( entry.m_bValid ) {
						m_stats.m_nResidentBytes -= ( uint64_t ) entry.m_stored.size();
						++m_stats.m_nEvictions;
					}
					entry = Entry();
					entry.m_key = sourceKey;
					entry.m_bReading = true;
					entry.m_nSchedulerTicket = -1;
				}
				break;
			}
		}
		// Delay the physical pack open until the cache has missed. An alias
		// already cached by another logical region performs zero opens.
		if ( usedPack ) {
			sourceFile = FileAccess::open( sourceKey.m_path, FileAccess::READ );
			const bool opened = sourceFile.is_valid() && sourceFile->get_length() == sourceKey.m_nFileLength;
			{
				Terrain3DScopedLock lock( m_mutex );
				++m_stats.m_nPackOpens;
				if ( !opened ) {
					m_bPackRejected = true;
					if ( slot >= 0 && epoch == m_nEpoch && m_entries[( size_t ) slot].m_bReading &&
						 m_entries[( size_t ) slot].m_key == sourceKey ) {
						m_entries[( size_t ) slot] = Entry();
						m_ready.notifyAll();
					}
				}
			}
			if ( !opened ) {
				return false;
			}
		}
		const bool sourceReady =
			sourceFile.is_valid() && ( uint64_t ) sourceFile->get_length() == sourceKey.m_nFileLength;
		const uint64_t readStarted = t3dSteadyClockUsec();
		// At most two filesystem read slices in flight. Each chunk yields to
		// higher-priority jobs; read verification remains per complete payload.
		const int readTicket = m_scheduler.begin( sourceKey.m_nPriority );
		if ( slot >= 0 ) {
			Terrain3DScopedLock lock( m_mutex );
			Entry			   &entry = m_entries[( size_t ) slot];
			if ( epoch == m_nEpoch && entry.m_bReading && entry.m_key == sourceKey ) {
				entry.m_nSchedulerTicket = readTicket;
				m_scheduler.promote( readTicket, entry.m_key.m_nPriority );
			}
		}
		PackedByteArray stored;
		stored.resize( sourceKey.m_nStoredBytes );
		uint32_t completed = 0u;
		if ( sourceReady && stored.size() == ( int64_t ) sourceKey.m_nStoredBytes ) {
			uint8_t *const pBuffer = stored.ptrw();
			while ( completed < sourceKey.m_nStoredBytes ) {
				const uint32_t remaining = sourceKey.m_nStoredBytes - completed;
				const uint32_t chunk = remaining < m_scheduler.chunkBytes() ? remaining : m_scheduler.chunkBytes();
				m_scheduler.acquireSlice( readTicket );
				sourceFile->seek( ( uint64_t ) sourceKey.m_nOffset + completed );
				const uint64_t actual = sourceFile->get_buffer( pBuffer + completed, chunk );
				m_scheduler.releaseSlice( readTicket, ( uint32_t ) actual );
				completed += ( uint32_t ) actual;
				if ( actual != chunk ) {
					break;
				}
			}
		}
		m_scheduler.finish( readTicket );
		if ( completed != sourceKey.m_nStoredBytes ) {
			stored.resize( completed );
		}
		T3D_STREAM_PROFILE_BYTES( T3D_SP_REGION_IO_READ, completed );
		const uint64_t readFinished = t3dSteadyClockUsec();
		bool		   valid = sourceReady && verify( stored, sourceKey ) &&
					 ( sourceKey.m_nModifiedTime == 0u ||
					   FileAccess::get_modified_time( sourceKey.m_path ) == sourceKey.m_nModifiedTime );
		const uint64_t validated = t3dSteadyClockUsec();
		rReadUsec += readFinished - readStarted;
		rValidationUsec += validated - readFinished;
		{
			Terrain3DScopedLock lock( m_mutex );
			// Recook, source removal and world replacement invalidate reads that
			// started before the catalog epoch advanced, even without a cache slot.
			valid = valid && epoch == m_nEpoch;
			++m_stats.m_nRangeReadCalls;
			m_stats.m_nRangeReadBytes += ( uint64_t ) stored.size();
			bool seen = false;
			for ( size_t i = 0u; i < m_nSeenCount; ++i ) {
				if ( m_seen[i] == sourceKey ) {
					seen = true;
					break;
				}
			}
			if ( seen ) {
				++m_stats.m_nDuplicateRangeReads;
				m_stats.m_nDuplicateRangeBytes += ( uint64_t ) stored.size();
			} else {
				m_stats.m_nDistinctRangeBytes += ( uint64_t ) stored.size();
				m_seen[m_nSeenNext] = sourceKey;
				m_nSeenNext = ( m_nSeenNext + 1u ) % m_seen.size();
				if ( m_nSeenCount < m_seen.size() ) {
					++m_nSeenCount;
				}
			}
			if ( !valid ) {
				++m_stats.m_nFailedReads;
			}
			if ( slot >= 0 && epoch == m_nEpoch && m_entries[( size_t ) slot].m_bReading &&
				 m_entries[( size_t ) slot].m_key == sourceKey ) {
				Entry &entry = m_entries[( size_t ) slot];
				if ( valid && sourceKey.m_nStoredBytes <= m_nBudget ) {
					// Trim to budget under the same lock; this entry is still private.
					while ( m_stats.m_nResidentBytes + sourceKey.m_nStoredBytes > m_nBudget ) {
						int victim = -1;
						for ( int i = 0; i < ENTRY_COUNT; ++i ) {
							const Entry &other = m_entries[( size_t ) i];
							if ( i == slot || !other.m_bValid || other.m_bReading ) {
								continue;
							}
							if ( victim < 0 || other.m_nLastUse < m_entries[( size_t ) victim].m_nLastUse ) {
								victim = i;
							}
						}
						if ( victim < 0 ) {
							break;
						}
						m_stats.m_nResidentBytes -= ( uint64_t ) m_entries[( size_t ) victim].m_stored.size();
						m_entries[( size_t ) victim] = Entry();
						++m_stats.m_nEvictions;
					}
					entry.m_stored = stored;
					entry.m_nLastUse = ++m_nClock;
					entry.m_bValid = true;
					m_stats.m_nResidentBytes += sourceKey.m_nStoredBytes;
				} else if ( valid ) {
					entry = Entry();
				} else {
					entry.m_stored = PackedByteArray();
					entry.m_bFailed = true;
					entry.m_nRetryAfter = t3dSteadyClockUsec() + 500000u;
				}
				entry.m_bReading = false;
				m_ready.notifyAll();
			}
		}
		if ( valid ) {
			rResult = stored;
			return true;
		}
		return false;
	}
};

#endif // TERRAIN3D_STREAM_READ_CACHE_H
