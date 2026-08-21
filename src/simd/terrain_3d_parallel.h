// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_PARALLEL_H
#define TERRAIN3D_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <godot_cpp/classes/worker_thread_pool.hpp>
#include "core/terrain_3d_compiler.h"

namespace terrain3d_parallel_detail {
#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using WorkerThreadPool = godot::WorkerThreadPool;
#else
using WorkerThreadPool = ::WorkerThreadPool;
#endif
} // namespace terrain3d_parallel_detail

class T3DParallelExecutor {
public:
	using RangeTask = void ( * )( void *, size_t, size_t );
	using StaticRangeTask = void ( * )( void *, size_t, size_t, int32_t );
	using TileTask = void ( * )( void *, size_t, size_t, size_t, size_t, int32_t );

private:
	int32_t m_nTaskLimit = 1;

	typedef struct t3dDispatch_s {
		std::atomic<size_t> next{0u};
		size_t nCount = 0u;
		size_t grain = 1u;
		RangeTask task = nullptr;
		void *pContext = nullptr;

		void process( void ) {
			for ( ;; ) {
				size_t begin = next.load( std::memory_order_relaxed );
				size_t end = 0u;
				for ( ;; ) {
					if ( begin >= nCount ) {
						return;
					}
					const size_t remaining = nCount - begin;
					end = grain >= remaining ? nCount : begin + grain;
					if ( next.compare_exchange_weak( begin, end, std::memory_order_relaxed,
							std::memory_order_relaxed ) ) {
						break;
					}
				}
				task( pContext, begin, end );
			}
		}

		EA_FORCE_INLINE static void worker( void *const pUserdata, uint32_t ) { ( ( t3dDispatch_s * ) ( pUserdata ) )->process(); }
	} t3dDispatch_t;

	typedef struct t3dStaticDispatch_s {
		size_t nCount = 0u;
		size_t alignment = 1u;
		int32_t nTaskCount = 1;
		StaticRangeTask task = nullptr;
		void *pContext = nullptr;

		EA_FORCE_INLINE static size_t partitionPoint( const size_t nCount, const size_t nPart, const size_t nParts ) { const size_t base = nCount / nParts; const size_t extra = nCount % nParts; return base * nPart + std::min( nPart, extra ); }

		static size_t alignedOffset( const size_t nChunk, const size_t nAlignment, const size_t nLimit ) {
			if ( nChunk >= nLimit / nAlignment + ( size_t ) ( nLimit % nAlignment != 0u ) ) {
				return nLimit;
			}
			return nChunk * nAlignment;
		}

		void process( const int32_t iWorkerIndex ) const {
			const size_t chunk_count = nCount / alignment + ( size_t ) ( nCount % alignment != 0u );
			const size_t worker = ( size_t ) ( std::max( 0, iWorkerIndex ) );
			const size_t workers = ( size_t ) ( std::max( 1, nTaskCount ) );
			const size_t first_chunk = partitionPoint( chunk_count, worker, workers );
			const size_t last_chunk = partitionPoint( chunk_count, worker + 1u, workers );
			const size_t begin = alignedOffset( first_chunk, alignment, nCount );
			const size_t end = alignedOffset( last_chunk, alignment, nCount );
			if ( begin < end ) {
				task( pContext, begin, end, iWorkerIndex );
			}
		}

		EA_FORCE_INLINE static void worker( void *const pUserdata, const uint32_t iIndex ) { ( ( t3dStaticDispatch_s * ) ( pUserdata ) )->process( ( int32_t ) ( iIndex ) ); }
	} t3dStaticDispatch_t;

	typedef struct t3dTileDispatchContext_s {
		size_t nWidth = 0u;
		size_t height = 0u;
		size_t tileWidth = 1u;
		size_t tileHeight = 1u;
		size_t tilesX = 0u;
		TileTask task = nullptr;
		void *pContext = nullptr;
	} t3dTileDispatchContext_t;

	static void runTileRange( void *const pContext, const size_t nBegin, const size_t nEnd, const int32_t nWorker ) {
		t3dTileDispatchContext_t *const dispatch = ( t3dTileDispatchContext_t * ) ( pContext );
		for ( size_t tile = nBegin; tile < nEnd; ++tile ) {
			const size_t tile_y = tile / dispatch->tilesX;
			const size_t tile_x = tile - tile_y * dispatch->tilesX;
			const size_t x0 = tile_x * dispatch->tileWidth;
			const size_t y0 = tile_y * dispatch->tileHeight;
			const size_t x1 = dispatch->tileWidth >= dispatch->nWidth - x0 ? dispatch->nWidth : x0 + dispatch->tileWidth;
			const size_t y1 =
				dispatch->tileHeight >= dispatch->height - y0 ? dispatch->height : y0 + dispatch->tileHeight;
			dispatch->task( dispatch->pContext, x0, y0, x1, y1, nWorker );
		}
	}

public:
	EA_FORCE_INLINE explicit T3DParallelExecutor( const int32_t nTaskLimit ) : m_nTaskLimit( std::max( 1, nTaskLimit ) ) {}

	void run( const size_t nCount, const size_t nGrain, const RangeTask task, void *const pContext ) const {
		if ( nCount == 0u || task == nullptr ) {
			return;
		}

		t3dDispatch_t dispatch;
		dispatch.nCount = nCount;
		dispatch.grain = std::max<size_t>( 1u, nGrain );
		dispatch.pContext = pContext;
		dispatch.task = task;

#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
		dispatch.process();
#else
		using WorkerPool = terrain3d_parallel_detail::WorkerThreadPool;
		WorkerPool *const pool = WorkerPool::get_singleton();
		const size_t chunk_count = nCount / dispatch.grain + ( size_t ) ( nCount % dispatch.grain != 0u );
		const int32_t task_count = ( int32_t ) ( std::min<size_t>( ( size_t ) ( m_nTaskLimit ), chunk_count ) );
		const bool nested_pool_call = pool && ( pool->get_caller_task_id() != WorkerPool::INVALID_TASK_ID ||
			pool->get_caller_group_id() != WorkerPool::INVALID_TASK_ID );
		if ( !pool || task_count <= 1 || nested_pool_call ) {
			dispatch.process();
			return;
		}

		const WorkerPool::GroupID group =
			pool->add_native_group_task( &t3dDispatch_t::worker, &dispatch, task_count, task_count, false );
		if ( group == WorkerPool::INVALID_TASK_ID ) {
			dispatch.process();
			return;
		}
		pool->wait_for_group_task_completion( group );
#endif
	}

	void runStatic( const size_t nCount, const size_t nAlignment, const StaticRangeTask task, void *const pContext ) const {
		if ( nCount == 0u || task == nullptr ) {
			return;
		}

		t3dStaticDispatch_t dispatch;
		dispatch.nCount = nCount;
		dispatch.alignment = std::max<size_t>( 1u, nAlignment );
		dispatch.pContext = pContext;
		dispatch.task = task;

#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
		dispatch.nTaskCount = 1;
		dispatch.process( 0 );
#else
		using WorkerPool = terrain3d_parallel_detail::WorkerThreadPool;
		WorkerPool *const pool = WorkerPool::get_singleton();
		const size_t chunk_count = nCount / dispatch.alignment + ( size_t ) ( nCount % dispatch.alignment != 0u );
		dispatch.nTaskCount = ( int32_t ) ( std::min<size_t>( ( size_t ) ( m_nTaskLimit ), chunk_count ) );
		const bool nested_pool_call = pool && ( pool->get_caller_task_id() != WorkerPool::INVALID_TASK_ID ||
			pool->get_caller_group_id() != WorkerPool::INVALID_TASK_ID );
		if ( !pool || dispatch.nTaskCount <= 1 || nested_pool_call ) {
			dispatch.nTaskCount = 1;
			dispatch.process( 0 );
			return;
		}
		const WorkerPool::GroupID group = pool->add_native_group_task(
			&t3dStaticDispatch_t::worker, &dispatch, dispatch.nTaskCount, dispatch.nTaskCount, false );
		if ( group == WorkerPool::INVALID_TASK_ID ) {
			dispatch.nTaskCount = 1;
			dispatch.process( 0 );
			return;
		}
		pool->wait_for_group_task_completion( group );
#endif
	}

	void runTiles(
		const size_t nWidth, const size_t nHeight, const size_t nTileWidth,
		const size_t nTileHeight, const TileTask task, void *const pContext
	) const {
		if ( nWidth == 0u || nHeight == 0u || task == nullptr ) {
			return;
		}

		t3dTileDispatchContext_t dispatch;
		dispatch.nWidth = nWidth;
		dispatch.height = nHeight;
		dispatch.tileWidth = std::max<size_t>( 1u, nTileWidth );
		dispatch.tileHeight = std::max<size_t>( 1u, nTileHeight );
		dispatch.tilesX = nWidth / dispatch.tileWidth + ( size_t ) ( nWidth % dispatch.tileWidth != 0u );
		const size_t tiles_y = nHeight / dispatch.tileHeight + ( size_t ) ( nHeight % dispatch.tileHeight != 0u );
		if ( dispatch.tilesX == 0u || tiles_y > std::numeric_limits<size_t>::max() / dispatch.tilesX ) {
			return;
		}
		dispatch.task = task;
		dispatch.pContext = pContext;
		runStatic( dispatch.tilesX * tiles_y, 1u, &runTileRange, &dispatch );
	}
};

#endif // TERRAIN3D_PARALLEL_H
