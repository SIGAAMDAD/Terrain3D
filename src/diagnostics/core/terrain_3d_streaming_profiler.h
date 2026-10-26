// Copyright (c) 2026 Terrain3D Contributors.
// Opt-in cross-thread streaming stage profiler. No heap allocation or locks in hot scopes.
#ifndef TERRAIN_3D_STREAMING_PROFILER_H
#define TERRAIN_3D_STREAMING_PROFILER_H

#include <atomic>
#include <chrono>
#include <stdint.h>

// GPU stages measure CPU submission/service time, NOT GPU execution time.
enum t3dStreamProfileStage_t : uint32_t
{
	T3D_SP_VT_PREDICTION,
	T3D_SP_VT_REQUEST_ADMISSION,
	T3D_SP_VT_JOB_DISPATCH,
	T3D_SP_VT_IO_READ_DECODE,
	T3D_SP_VT_UPLOAD_SERVICE,
	T3D_SP_VT_PHYSICAL_UPLOAD,
	T3D_SP_VT_PAGE_TABLE_PUBLISH,
	T3D_SP_VT_FEEDBACK_SERVICE,
	T3D_SP_REGION_IO_READ,
	T3D_SP_REGION_WORKER_PREPARE,
	T3D_SP_REGION_UPLOAD_PREPARE,
	T3D_SP_REGION_TEXTURE_UPLOAD,
	T3D_SP_WORLD_PUBLICATION,
	T3D_SP_FORWARD_PASS_PREPARE,
	T3D_SP_BUDGET_ADMISSION,
	T3D_SP_REGION_TEXTURE_UPDATE,
	T3D_SP_REGION_TEXTURE_COPY,
	T3D_SP_VT_FEEDBACK_READBACK_SUBMIT,
	T3D_SP_VT_FEEDBACK_RESET_SUBMIT,
	T3D_SP_WORLD_BUFFER_UPLOAD,
	T3D_SP_FORWARD_PASS_SUBMIT,
	T3D_SP_FORWARD_BRIDGE_PACKET,
	T3D_SP_FORWARD_WORLD_VISIBILITY,
	T3D_SP_FORWARD_HIZ_BUILD,
	T3D_SP_FORWARD_VISIBILITY_DISPATCH,
	T3D_SP_FORWARD_WORLD_RASTER,
	T3D_SP_FORWARD_VT_SERVICE,
	T3D_SP_FORWARD_DIAGNOSTICS,
	// P12: source-inferred H-series measurements; all stages are CPU-side.
	T3D_SP_H01_PACK_RESOLVE,
	T3D_SP_H02_CACHE_SCAN,
	T3D_SP_H02_CACHE_WAIT,
	T3D_SP_H02_MUTEX_WAIT,
	T3D_SP_H03_INSTANCE_PREDICTION,
	T3D_SP_H04_RASTER_COOK,
	T3D_SP_H05_PACK_DEDUP,
	T3D_SP_H05_PACK_OPEN_DUPLICATE,
	T3D_SP_H06_MANIFEST_INTERN,
	T3D_SP_H07_GPU_LAYER_SCAN,
	T3D_SP_H08_DIAGNOSTIC_BUILD,
	T3D_SP_H08_RUNTIME_DIAGNOSTICS,
	T3D_SP_H09_PACK_REMOUNT,
	T3D_SP_H10_MOTION_SORT,
	T3D_SP_H11_ATLAS_REBUILD,
	T3D_SP_H12_BUDGET_LOCK_WAIT,
	T3D_SP_H12_BUDGET_LOCK_HOLD,
	T3D_SP_COUNT
};

class Terrain3DStreamingProfiler
{
public:
	enum
	{
		HISTOGRAM_BUCKETS = 24
	};
	struct Stage
	{
		std::atomic< uint64_t > calls{ 0u };
		std::atomic< uint64_t > samples{ 0u };
		std::atomic< uint64_t > totalNanoseconds{ 0u };
		std::atomic< uint64_t > maxNanoseconds{ 0u };
		std::atomic< uint64_t > bytes{ 0u };
		std::atomic< uint64_t > workUnits{ 0u };
		std::atomic< uint64_t > events{ 0u };
		std::atomic< uint64_t > histogram[HISTOGRAM_BUCKETS]{};
	};
	struct Snapshot
	{
		uint64_t calls = 0u;
		uint64_t samples = 0u;
		uint64_t totalNanoseconds = 0u;
		uint64_t maxNanoseconds = 0u;
		uint64_t bytes = 0u;
		uint64_t workUnits = 0u;
		uint64_t events = 0u;
		uint64_t histogram[HISTOGRAM_BUCKETS]{};
	};

	static Terrain3DStreamingProfiler &get() {
		static Terrain3DStreamingProfiler profiler;
		return profiler;
	}
	static const char *name(
		t3dStreamProfileStage_t stage ) {
		static const char *names[T3D_SP_COUNT] = {
			"vt_prediction",
			"vt_request_admission",
			"vt_job_dispatch",
			"vt_io_read_decode",
			"vt_upload_service",
			"vt_physical_texture_upload",
			"vt_page_table_publication",
			"vt_feedback_service",
			"region_io_read",
			"region_worker_preparation",
			"region_upload_preparation",
			"region_texture_upload",
			"world_publication",
			"forward_pass_preparation",
			"gpu_budget_admission",
			"region_texture_update",
			"region_texture_copy",
			"vt_feedback_readback_submit",
			"vt_feedback_reset_submit",
			"world_buffer_upload",
			"forward_pass_submit",
			"forward_bridge_packet",
			"forward_world_visibility",
			"forward_hiz_build",
			"forward_visibility_dispatch",
			"forward_world_raster_prepare",
			"forward_vt_service",
			"forward_diagnostics",
			"h01_pack_resolve",
			"h02_read_cache_scan",
			"h02_read_cache_wait",
			"h02_read_cache_mutex_wait",
			"h03_instance_vt_prediction",
			"h04_raster_hierarchy_cook",
			"h05_pack_build_duplicate_check",
			"h05_pack_open_duplicate_check",
			"h06_manifest_dependency_intern",
			"h07_gpu_region_layer_scan",
			"h08_diagnostic_dictionary_build",
			"h08_runtime_diagnostic_dictionary",
			"h09_pack_remount",
			"h10_instance_motion_sort",
			"h11_atlas_rebuild",
			"h12_budget_mutex_wait",
			"h12_budget_mutex_hold"
		};
		return stage < T3D_SP_COUNT ? names[stage] : "invalid";
	}
	// Counted units are stage-specific and intentionally not extrapolated from sampled timing.
	static const char *workUnitName( t3dStreamProfileStage_t stage ) {
		switch ( stage ) {
			case T3D_SP_H01_PACK_RESOLVE: return "entries_compared";
			case T3D_SP_H02_CACHE_SCAN: return "slots_examined";
			case T3D_SP_H03_INSTANCE_PREDICTION: return "transforms_sampled";
			case T3D_SP_H04_RASTER_COOK: return "category_comparisons";
			case T3D_SP_H05_PACK_DEDUP:
			case T3D_SP_H05_PACK_OPEN_DUPLICATE: return "pairs_checked";
			case T3D_SP_H06_MANIFEST_INTERN: return "paths_compared";
			case T3D_SP_H07_GPU_LAYER_SCAN: return "layers_examined";
			case T3D_SP_H08_DIAGNOSTIC_BUILD:
			case T3D_SP_H08_RUNTIME_DIAGNOSTICS: return "dictionary_keys";
			case T3D_SP_H09_PACK_REMOUNT: return "package_open_attempts";
			case T3D_SP_H10_MOTION_SORT: return "cells_sorted";
			default: return "none";
		}
	}
	static const char *eventName( t3dStreamProfileStage_t stage ) {
		switch ( stage ) {
			case T3D_SP_H01_PACK_RESOLVE: return "filesystem_metadata_queries";
			case T3D_SP_H06_MANIFEST_INTERN: return "unique_dependencies_inserted";
			default: return "none";
		}
	}
	void setEnabled(
		bool	 enabled,
		uint32_t sampleEvery = 8u ) {
		if ( sampleEvery == 0u ) {
			sampleEvery = 1u;
		}
		if ( sampleEvery > 1024u ) {
			sampleEvery = 1024u;
		}
		m_sampleEvery.store( sampleEvery, std::memory_order_release );
		m_enabled.store( enabled, std::memory_order_release );
	}
	bool enabled() const {
		return m_enabled.load( std::memory_order_relaxed );
	}
	uint32_t sampleEvery() const {
		return m_sampleEvery.load( std::memory_order_relaxed );
	}
	void reset() {
		for ( uint32_t i = 0u; i < T3D_SP_COUNT; ++i ) {
			Stage &stage = m_stages[i];
			stage.calls.store( 0u, std::memory_order_relaxed );
			stage.samples.store( 0u, std::memory_order_relaxed );
			stage.totalNanoseconds.store( 0u, std::memory_order_relaxed );
			stage.maxNanoseconds.store( 0u, std::memory_order_relaxed );
			stage.bytes.store( 0u, std::memory_order_relaxed );
			stage.workUnits.store( 0u, std::memory_order_relaxed );
			stage.events.store( 0u, std::memory_order_relaxed );
			for ( auto &bucket : stage.histogram ) {
				bucket.store( 0u, std::memory_order_relaxed );
			}
		}
	}
	bool enter(
		t3dStreamProfileStage_t id ) {
		if ( !enabled() || id >= T3D_SP_COUNT ) {
			return false;
		}
		const uint64_t call = m_stages[id].calls.fetch_add( 1u, std::memory_order_relaxed );
		return call % sampleEvery() == 0u;
	}
	void record(
		t3dStreamProfileStage_t id,
		uint64_t				elapsedNs ) {
		Stage &stage = m_stages[id];
		stage.samples.fetch_add( 1u, std::memory_order_relaxed );
		stage.totalNanoseconds.fetch_add( elapsedNs, std::memory_order_relaxed );
		uint64_t highest = stage.maxNanoseconds.load( std::memory_order_relaxed );
		while ( elapsedNs > highest &&
				!stage.maxNanoseconds.compare_exchange_weak( highest, elapsedNs, std::memory_order_relaxed ) ) {
		}
		// Log2 buckets in microseconds: [0..1], (1..2], (2..4] ...
		uint64_t us = ( elapsedNs + 999u ) / 1000u;
		uint32_t bucket = 0u;
		while ( us > ( 1ull << bucket ) && bucket + 1u < HISTOGRAM_BUCKETS ) {
			++bucket;
		}
		stage.histogram[bucket].fetch_add( 1u, std::memory_order_relaxed );
	}
	void addBytes(
		t3dStreamProfileStage_t id,
		uint64_t				bytes ) {
		if ( enabled() && id < T3D_SP_COUNT ) {
			m_stages[id].bytes.fetch_add( bytes, std::memory_order_relaxed );
		}
	}
	void addWork( t3dStreamProfileStage_t id, uint64_t count ) {
		if ( count && enabled() && id < T3D_SP_COUNT ) {
			m_stages[id].workUnits.fetch_add( count, std::memory_order_relaxed );
		}
	}
	void addEvents( t3dStreamProfileStage_t id, uint64_t count ) {
		if ( count && enabled() && id < T3D_SP_COUNT ) {
			m_stages[id].events.fetch_add( count, std::memory_order_relaxed );
		}
	}
	Snapshot snapshot(
		t3dStreamProfileStage_t id ) const {
		Snapshot result;
		if ( id >= T3D_SP_COUNT ) {
			return result;
		}
		const Stage &stage = m_stages[id];
		result.calls = stage.calls.load( std::memory_order_relaxed );
		result.samples = stage.samples.load( std::memory_order_relaxed );
		result.totalNanoseconds = stage.totalNanoseconds.load( std::memory_order_relaxed );
		result.maxNanoseconds = stage.maxNanoseconds.load( std::memory_order_relaxed );
		result.bytes = stage.bytes.load( std::memory_order_relaxed );
		result.workUnits = stage.workUnits.load( std::memory_order_relaxed );
		result.events = stage.events.load( std::memory_order_relaxed );
		for ( uint32_t i = 0u; i < HISTOGRAM_BUCKETS; ++i ) {
			result.histogram[i] = stage.histogram[i].load( std::memory_order_relaxed );
		}
		return result;
	}

private:
	Terrain3DStreamingProfiler() = default;
	Stage					m_stages[T3D_SP_COUNT];
	std::atomic< bool >		m_enabled{ false };
	std::atomic< uint32_t > m_sampleEvery{ 8u };
};

class Terrain3DStreamingProfileScope
{
	t3dStreamProfileStage_t				  m_stage;
	bool								  m_sampled;
	std::chrono::steady_clock::time_point m_start;

public:
	explicit Terrain3DStreamingProfileScope(
		t3dStreamProfileStage_t stage ) :
		m_stage( stage ),
		m_sampled( Terrain3DStreamingProfiler::get().enter( stage ) ) {
		if ( m_sampled ) {
			m_start = std::chrono::steady_clock::now();
		}
	}
	~Terrain3DStreamingProfileScope() {
		if ( m_sampled ) {
			const auto ns =
				std::chrono::duration_cast< std::chrono::nanoseconds >( std::chrono::steady_clock::now() - m_start )
					.count();
			Terrain3DStreamingProfiler::get().record( m_stage, ns > 0 ? ( uint64_t ) ns : 0u );
		}
	}
	Terrain3DStreamingProfileScope( const Terrain3DStreamingProfileScope & ) = delete;
	Terrain3DStreamingProfileScope &operator=( const Terrain3DStreamingProfileScope & ) = delete;
};

// P27: coalesce per-channel byte/events accounting at an explicit operation
// boundary. The counters remain atomically published when the scope exits;
// no global thread-local registration or change to reset/snapshot ownership.
class Terrain3DStreamingProfileBytesScope
{
	t3dStreamProfileStage_t m_stage;
	bool m_enabled;
	uint64_t m_bytes = 0u;
public:
	explicit Terrain3DStreamingProfileBytesScope( t3dStreamProfileStage_t stage ) :
		m_stage( stage ), m_enabled( Terrain3DStreamingProfiler::get().enabled() ) {}
	~Terrain3DStreamingProfileBytesScope() {
		if ( m_enabled && m_bytes ) {
			Terrain3DStreamingProfiler::get().addBytes( m_stage, m_bytes );
		}
	}
	void add( uint64_t bytes ) { if ( m_enabled ) { m_bytes += bytes; } }
	Terrain3DStreamingProfileBytesScope( const Terrain3DStreamingProfileBytesScope & ) = delete;
	Terrain3DStreamingProfileBytesScope &operator=( const Terrain3DStreamingProfileBytesScope & ) = delete;
};

class Terrain3DStreamingProfileEventScope
{
	t3dStreamProfileStage_t m_stage;
	bool m_enabled;
	uint64_t m_events = 0u;
public:
	explicit Terrain3DStreamingProfileEventScope( t3dStreamProfileStage_t stage ) :
		m_stage( stage ), m_enabled( Terrain3DStreamingProfiler::get().enabled() ) {}
	~Terrain3DStreamingProfileEventScope() {
		if ( m_enabled && m_events ) {
			Terrain3DStreamingProfiler::get().addEvents( m_stage, m_events );
		}
	}
	void add( uint64_t events = 1u ) { if ( m_enabled ) { m_events += events; } }
	Terrain3DStreamingProfileEventScope( const Terrain3DStreamingProfileEventScope & ) = delete;
	Terrain3DStreamingProfileEventScope &operator=( const Terrain3DStreamingProfileEventScope & ) = delete;
};

// A scoped accumulation avoids atomic increments inside comparison-heavy loops.
class Terrain3DStreamingProfileWorkScope
{
	t3dStreamProfileStage_t m_stage;
	bool m_enabled;
	uint64_t m_units = 0u;
public:
	explicit Terrain3DStreamingProfileWorkScope( t3dStreamProfileStage_t stage ) :
		m_stage( stage ), m_enabled( Terrain3DStreamingProfiler::get().enabled() ) {}
	~Terrain3DStreamingProfileWorkScope() {
		if ( m_enabled && m_units ) {
			Terrain3DStreamingProfiler::get().addWork( m_stage, m_units );
		}
	}
	void add( uint64_t units = 1u ) { if ( m_enabled ) { m_units += units; } }
	Terrain3DStreamingProfileWorkScope( const Terrain3DStreamingProfileWorkScope & ) = delete;
	Terrain3DStreamingProfileWorkScope &operator=( const Terrain3DStreamingProfileWorkScope & ) = delete;
};

#define T3D_PROFILE_JOIN_INNER( a, b ) a##b
#define T3D_PROFILE_JOIN( a, b ) T3D_PROFILE_JOIN_INNER( a, b )
#define T3D_STREAM_PROFILE_SCOPE( stage ) Terrain3DStreamingProfileScope T3D_PROFILE_JOIN( t3dStreamingScope_, __LINE__ )( stage )
#define T3D_STREAM_PROFILE_WORK_SCOPE( stage ) Terrain3DStreamingProfileWorkScope T3D_PROFILE_JOIN( t3dStreamingWork_, __LINE__ )( stage )
#define T3D_STREAM_PROFILE_EVENTS( stage, amount ) Terrain3DStreamingProfiler::get().addEvents( stage, amount )
#define T3D_STREAM_PROFILE_WORK( stage, units ) Terrain3DStreamingProfiler::get().addWork( stage, units )
#define T3D_STREAM_PROFILE_BYTES( stage, bytes ) Terrain3DStreamingProfiler::get().addBytes( stage, bytes )

#endif
