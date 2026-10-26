// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_PARALLEL_H
#define TERRAIN3D_PARALLEL_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_sync.h"

#include <EASTL/numeric_limits.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/worker_thread_pool.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
typedef godot::WorkerThreadPool t3dWorkerThreadPool_t;
#else
typedef ::WorkerThreadPool t3dWorkerThreadPool_t;
#endif

typedef void ( *t3dParallelRangeTask_t )( void *, size_t, size_t );
typedef void ( *t3dParallelStaticRangeTask_t )( void *, size_t, size_t, int32_t );
typedef void ( *t3dParallelTileTask_t )( void *, size_t, size_t, size_t, size_t, int32_t );

typedef struct t3dParallelDispatch_s {

	Terrain3DAtomic<size_t> m_iNext{ 0u };
	size_t m_nCount = 0u;
	size_t m_nGrain = 1u;
	t3dParallelRangeTask_t m_task = NULL;
	void *m_pContext = NULL;

	void process( void ) {
		for ( ;; ) {
			size_t iBegin = m_iNext.load( T3D_MEMORY_ORDER_RELAXED );
			size_t iEnd = 0u;
			for ( ;; ) {
				if ( iBegin >= m_nCount ) {
					return;
				}
				const size_t nRemaining = m_nCount - iBegin;
				iEnd = m_nGrain >= nRemaining ? m_nCount : iBegin + m_nGrain;
				if ( m_iNext.compare_exchange_weak( iBegin, iEnd, T3D_MEMORY_ORDER_RELAXED, T3D_MEMORY_ORDER_RELAXED ) ) {
					break;
				}
			}
			m_task( m_pContext, iBegin, iEnd );
		}
	}

	EA_FORCE_INLINE static void worker( void *const pUserdata, uint32_t ) {
		( ( t3dParallelDispatch_s * )( pUserdata ) )->process();
	}
} t3dParallelDispatch_t;


typedef struct t3dParallelStaticDispatch_s {

	size_t m_nCount = 0u;
	size_t m_nAlignment = 1u;
	int32_t m_nTaskCount = 1;
	t3dParallelStaticRangeTask_t m_task = NULL;
	void *m_pContext = NULL;

	EA_FORCE_INLINE static size_t partitionPoint( const size_t nCount, const size_t nPart, const size_t nParts ) {
		const size_t nBase = nCount / nParts;
		const size_t nExtra = nCount % nParts;
		return nBase * nPart + MIN( nPart, nExtra );
	}

	static size_t alignedOffset( const size_t nChunk, const size_t nAlignment, const size_t nLimit ) {
		if ( nChunk >= nLimit / nAlignment + ( size_t )( nLimit % nAlignment != 0u ) ) {
			return nLimit;
		}
		return nChunk * nAlignment;
	}

	void process( const int32_t iWorker ) const {
		const size_t nChunkCount = m_nCount / m_nAlignment + ( size_t )( m_nCount % m_nAlignment != 0u );
		const size_t iWorkerClamped = ( size_t )( MAX( 0, iWorker ) );
		const size_t nWorkers = ( size_t )( MAX( 1, m_nTaskCount ) );
		const size_t iFirstChunk = partitionPoint( nChunkCount, iWorkerClamped, nWorkers );
		const size_t iLastChunk = partitionPoint( nChunkCount, iWorkerClamped + 1u, nWorkers );
		const size_t iBegin = alignedOffset( iFirstChunk, m_nAlignment, m_nCount );
		const size_t iEnd = alignedOffset( iLastChunk, m_nAlignment, m_nCount );
		if ( iBegin < iEnd ) {
			m_task( m_pContext, iBegin, iEnd, iWorker );
		}
	}

	EA_FORCE_INLINE static void worker( void *const pUserdata, const uint32_t iIndex ) {
		( ( t3dParallelStaticDispatch_s * )( pUserdata ) )->process( ( int32_t )( iIndex ) );
	}
} t3dParallelStaticDispatch_t;


// t3dParallelTileDispatchContext_s centralized in Terrain3DTypes.h.


class Terrain3DParallelExecutor {
private:
	int32_t m_nTaskLimit = 1;

	static void runTileRange( void *const pContext, const size_t iBegin, const size_t iEnd, const int32_t iWorker ) {
		t3dParallelTileDispatchContext_t *const pDispatch = ( t3dParallelTileDispatchContext_t * )( pContext );
		for ( size_t iTile = iBegin; iTile < iEnd; ++iTile ) {
			const size_t iTileY = iTile / pDispatch->m_nTilesX;
			const size_t iTileX = iTile - iTileY * pDispatch->m_nTilesX;
			const size_t iX0 = iTileX * pDispatch->m_nTileWidth;
			const size_t iY0 = iTileY * pDispatch->m_nTileHeight;
			const size_t iX1 = pDispatch->m_nTileWidth >= pDispatch->m_nWidth - iX0 ? pDispatch->m_nWidth : iX0 + pDispatch->m_nTileWidth;
			const size_t iY1 =
			        pDispatch->m_nTileHeight >= pDispatch->m_nHeight - iY0 ? pDispatch->m_nHeight : iY0 + pDispatch->m_nTileHeight;
			pDispatch->m_task( pDispatch->m_pContext, iX0, iY0, iX1, iY1, iWorker );
		}
	}

public:
	EA_FORCE_INLINE explicit Terrain3DParallelExecutor( const int32_t nTaskLimit ) : m_nTaskLimit( MAX( 1, nTaskLimit ) ) {
	}

	void run( const size_t nCount, const size_t nGrain, const t3dParallelRangeTask_t pTask, void *const pContext ) const {
		if ( nCount == 0u || pTask == NULL ) {
			return;
		}

		t3dParallelDispatch_t dispatch;
		dispatch.m_nCount = nCount;
		dispatch.m_nGrain = MAX( 1u, nGrain );
		dispatch.m_pContext = pContext;
		dispatch.m_task = pTask;

#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
		dispatch.process();
#else
		t3dWorkerThreadPool_t *const pPool = t3dWorkerThreadPool_t::get_singleton();
		const size_t nChunkCount = nCount / dispatch.m_nGrain + ( size_t )( nCount % dispatch.m_nGrain != 0u );
		const int32_t nTaskCount = ( int32_t )( MIN( ( size_t )( m_nTaskLimit ), nChunkCount ) );
		const bool bNestedPoolCall = pPool
		        && ( pPool->get_caller_task_id() != t3dWorkerThreadPool_t::INVALID_TASK_ID
		                || pPool->get_caller_group_id() != t3dWorkerThreadPool_t::INVALID_TASK_ID );
		if ( !pPool || nTaskCount <= 1 || bNestedPoolCall ) {
			dispatch.process();
			return;
		}

		const t3dWorkerThreadPool_t::GroupID hGroup =
		        pPool->add_native_group_task( &t3dParallelDispatch_t::worker, &dispatch, nTaskCount, nTaskCount, false );
		if ( hGroup == t3dWorkerThreadPool_t::INVALID_TASK_ID ) {
			dispatch.process();
			return;
		}
		pPool->wait_for_group_task_completion( hGroup );
#endif
	}

	void runStatic( const size_t nCount, const size_t nAlignment, const t3dParallelStaticRangeTask_t pTask, void *const pContext ) const {
		if ( nCount == 0u || pTask == NULL ) {
			return;
		}

		t3dParallelStaticDispatch_t dispatch;
		dispatch.m_nCount = nCount;
		dispatch.m_nAlignment = MAX( 1u, nAlignment );
		dispatch.m_pContext = pContext;
		dispatch.m_task = pTask;

#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
		dispatch.m_nTaskCount = 1;
		dispatch.process( 0 );
#else
		t3dWorkerThreadPool_t *const pPool = t3dWorkerThreadPool_t::get_singleton();
		const size_t nChunkCount = nCount / dispatch.m_nAlignment + ( size_t )( nCount % dispatch.m_nAlignment != 0u );
		dispatch.m_nTaskCount = ( int32_t )( MIN( ( size_t )( m_nTaskLimit ), nChunkCount ) );
		const bool bNestedPoolCall = pPool
		        && ( pPool->get_caller_task_id() != t3dWorkerThreadPool_t::INVALID_TASK_ID
		                || pPool->get_caller_group_id() != t3dWorkerThreadPool_t::INVALID_TASK_ID );
		if ( !pPool || dispatch.m_nTaskCount <= 1 || bNestedPoolCall ) {
			dispatch.m_nTaskCount = 1;
			dispatch.process( 0 );
			return;
		}
		const t3dWorkerThreadPool_t::GroupID hGroup = pPool->add_native_group_task(
		        &t3dParallelStaticDispatch_t::worker, &dispatch, dispatch.m_nTaskCount, dispatch.m_nTaskCount, false );
		if ( hGroup == t3dWorkerThreadPool_t::INVALID_TASK_ID ) {
			dispatch.m_nTaskCount = 1;
			dispatch.process( 0 );
			return;
		}
		pPool->wait_for_group_task_completion( hGroup );
#endif
	}

	void runTiles( const size_t nWidth,
	        const size_t nHeight,
	        const size_t nTileWidth,
	        const size_t nTileHeight,
	        const t3dParallelTileTask_t pTask,
	        void *const pContext ) const {
		if ( nWidth == 0u || nHeight == 0u || pTask == NULL ) {
			return;
		}

		t3dParallelTileDispatchContext_t dispatch;
		dispatch.m_nWidth = nWidth;
		dispatch.m_nHeight = nHeight;
		dispatch.m_nTileWidth = MAX( 1u, nTileWidth );
		dispatch.m_nTileHeight = MAX( 1u, nTileHeight );
		dispatch.m_nTilesX = nWidth / dispatch.m_nTileWidth + ( size_t )( nWidth % dispatch.m_nTileWidth != 0u );
		const size_t nTilesY = nHeight / dispatch.m_nTileHeight + ( size_t )( nHeight % dispatch.m_nTileHeight != 0u );
		if ( dispatch.m_nTilesX == 0u || nTilesY > eastl::numeric_limits<size_t>::max() / dispatch.m_nTilesX ) {
			return;
		}
		dispatch.m_task = pTask;
		dispatch.m_pContext = pContext;
		runStatic( dispatch.m_nTilesX * nTilesY, 1u, &runTileRange, &dispatch );
	}
};

#endif // TERRAIN3D_PARALLEL_H
