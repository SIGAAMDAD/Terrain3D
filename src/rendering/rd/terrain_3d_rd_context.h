// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_CONTEXT_H
#define TERRAIN3D_RD_CONTEXT_H

#include "core/terrain_3d_compiler.h"

#include <array>
#include <atomic>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <mutex>
#include <vector>

using namespace godot;

// Terrain3D's sole owner/coordinator for RenderingDevice objects. Resource
// creation stays in the subsystem which has the construction data, but every
// owned RID must be transferred to this registry immediately after creation.
class Terrain3DRdContext {
public:
	enum ResourceType : uint8_t {
		RESOURCE_UNKNOWN = 0,
		RESOURCE_SAMPLER,
		RESOURCE_SHADER,
		RESOURCE_RASTER_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
		RESOURCE_VERTEX_ARRAY,
		RESOURCE_INDEX_ARRAY,
		RESOURCE_VERTEX_BUFFER,
		RESOURCE_INDEX_BUFFER,
		RESOURCE_STORAGE_BUFFER,
		RESOURCE_UNIFORM_BUFFER,
		RESOURCE_UNIFORM_SET,
		RESOURCE_INDIRECT_BUFFER,
		RESOURCE_CULLING_BUFFER,
		RESOURCE_SHADOW,
		RESOURCE_FRAMEBUFFER,
		RESOURCE_TEXTURE,
		RESOURCE_TYPE_COUNT,
	};

	enum MemoryCategory : uint8_t {
		MEMORY_NONE = 0,
		MEMORY_TOPOLOGY_VERTEX,
		MEMORY_TOPOLOGY_INDEX,
		MEMORY_MESH_VERTEX_ARENA,
		MEMORY_MESH_INDEX_ARENA,
		MEMORY_MATERIAL_TABLE,
		MEMORY_INSTANCE,
		MEMORY_HLOD,
		MEMORY_PATCH,
		MEMORY_INDIRECT,
		MEMORY_UNIFORM,
		MEMORY_STORAGE,
		MEMORY_SCRATCH,
		MEMORY_TERRAIN_TEXTURE,
		MEMORY_SHADOW,
		MEMORY_CATEGORY_COUNT,
	};

	enum ContextState : uint8_t {
		STATE_UNINITIALIZED = 0,
		STATE_READY,
		STATE_FAILED,
		STATE_REBUILD_REQUIRED,
		STATE_REBUILDING,
		STATE_SHUTTING_DOWN,
	};

	enum FailureCode : uint8_t {
		FAILURE_NONE = 0,
		FAILURE_DEVICE_UNAVAILABLE,
		FAILURE_RESOURCE_CREATION,
		FAILURE_DUPLICATE_RESOURCE,
		FAILURE_UNTRACKED_RESOURCE,
		FAILURE_ACCOUNTING,
		FAILURE_DEVICE_INVALIDATED,
		FAILURE_REBUILD,
		FAILURE_RENDER_PASS,
	};

	enum SuccessfulPass : uint8_t {
		PASS_NONE = 0,
		PASS_TERRAIN_CLIPMAP,
		PASS_COUNT,
	};

private:
	typedef struct t3dRdResourceRecord_s {
		RID m_hRid;
		String m_name;
		ResourceType m_type = RESOURCE_UNKNOWN;
		MemoryCategory m_memoryCategory = MEMORY_NONE;
		uint64_t m_nBytes = 0u;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdResourceRecord_t;

	typedef struct t3dRdResourceHistory_s {
		String m_name;
		ResourceType m_type = RESOURCE_UNKNOWN;
	} t3dRdResourceHistory_t;

	typedef struct t3dRdFreedResource_s {
		uint64_t m_nRid = 0u;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdFreedResource_t;

	typedef struct t3dRdVertexFormatRecord_s {
		int64_t m_nFormat = RenderingDevice::INVALID_FORMAT_ID;
		String m_name;
		uint64_t m_nDeviceGeneration = 0u;
	} t3dRdVertexFormatRecord_t;

	std::atomic<RenderingDevice *> m_pRd{ nullptr };
	std::atomic<ContextState> m_state{ STATE_UNINITIALIZED };
	std::atomic<FailureCode> m_failureCode{ FAILURE_NONE };
	mutable std::mutex m_resourceMutex;
	std::vector<t3dRdResourceRecord_t> m_resources;
	std::vector<t3dRdResourceHistory_t> m_resourceHistory;
	std::vector<t3dRdFreedResource_t> m_freedResources;
	std::vector<t3dRdVertexFormatRecord_t> m_vertexFormats;
	String m_lastFailure;
	std::atomic<SuccessfulPass> m_lastSuccessfulPass{ PASS_NONE };

	std::atomic_uint64_t m_nDeviceGeneration{ 0u };
	std::atomic_uint64_t m_nDeviceInitializations{ 0u };
	std::atomic_uint64_t m_nDeviceRebuilds{ 0u };
	std::atomic_uint64_t m_nDeviceInvalidations{ 0u };
	std::atomic_uint64_t m_nCreatedResources{ 0u };
	std::atomic_uint64_t m_nFreedResources{ 0u };
	std::atomic_uint64_t m_nAbandonedResources{ 0u };
	std::atomic_uint64_t m_nFailedResourceCreates{ 0u };
	std::atomic_uint64_t m_nDoubleFreeAttempts{ 0u };
	std::atomic_uint64_t m_nUntrackedFreeAttempts{ 0u };
	std::atomic_uint64_t m_nAccountingErrors{ 0u };
	std::atomic_uint64_t m_nLiveBufferBytes{ 0u };
	std::atomic_uint64_t m_nPeakBufferBytes{ 0u };
	std::atomic_uint64_t m_nLiveTextureBytes{ 0u };
	std::atomic_uint64_t m_nPeakTextureBytes{ 0u };
	std::array<std::atomic_uint64_t, RESOURCE_TYPE_COUNT> m_szTypeCreates{};
	std::array<std::atomic_uint64_t, RESOURCE_TYPE_COUNT> m_szTypeFrees{};
	std::array<std::atomic_uint64_t, RESOURCE_TYPE_COUNT> m_szTypeLive{};
	std::array<std::atomic_uint64_t, MEMORY_CATEGORY_COUNT> m_szLiveMemory{};
	std::array<std::atomic_uint64_t, MEMORY_CATEGORY_COUNT> m_szPeakMemory{};
	std::atomic_uint64_t m_nPipelineCreates{ 0u };
	std::atomic_uint64_t m_nPipelineRebuilds{ 0u };
	std::atomic_uint64_t m_nUniformSetCreates{ 0u };
	std::atomic_uint64_t m_nUniformSetRebuilds{ 0u };
	std::atomic_bool m_bWarmupComplete{ false };
	std::atomic_uint32_t m_nExplicitRebuildDepth{ 0u };
	std::atomic_uint64_t m_nSteadyStateAllocationViolations{ 0u };

	std::atomic_uint64_t m_nGpuFrameId{ 0u };
	std::atomic_uint64_t m_nSnapshotGeneration{ 0u };
	std::atomic_uint64_t m_nLastSuccessfulGpuFrame{ 0u };
	std::atomic_uint64_t m_nBufferUpdates{ 0u };
	std::atomic_uint64_t m_nBytesUploaded{ 0u };
	std::atomic_uint64_t m_nFrameBufferUpdates{ 0u };
	std::atomic_uint64_t m_nFrameBytesUploaded{ 0u };
	std::atomic_uint64_t m_nTerrainPatchCount{ 0u };
	std::atomic_uint64_t m_nVisibleTerrainPatches{ 0u };
	std::atomic_uint64_t m_nStaticInstanceCount{ 0u };
	std::atomic_uint64_t m_nVisibleInstances{ 0u };
	std::atomic_uint64_t m_nHlodCount{ 0u };
	std::atomic_uint64_t m_nVisibleHlods{ 0u };
	std::atomic_uint64_t m_nIndirectCommandCount{ 0u };
	std::atomic_uint64_t m_nComputeDispatchCount{ 0u };

	static ResourceType inferResourceType( const String &rName );
	static MemoryCategory inferMemoryCategory( const String &rName, ResourceType type, uint64_t nBytes );
	static bool isPipeline( ResourceType type );
	static bool isSteadyStateAllocation( ResourceType type );
	static void updatePeak( std::atomic_uint64_t &rPeak, uint64_t nLiveBytes );
	static bool subtractSaturating( std::atomic_uint64_t &rValue, uint64_t nAmount );
	void rememberFreedResource( uint64_t nRid, uint64_t nDeviceGeneration );
	void updateBufferPeak( uint64_t nLiveBytes );
	void updateTexturePeak( uint64_t nLiveBytes );
	void accountRegistration( const t3dRdResourceRecord_t &rRecord, bool bRebuild );
	void accountRelease( const t3dRdResourceRecord_t &rRecord );
	void resetLiveAccounting( void );
	void setFailure( FailureCode code, const String &rFailure );
	bool hasResourceHistory( const String &rName, ResourceType type ) const;
	bool registerResourceInternal( const RID &rRid,
			const String &rName,
			ResourceType type,
			MemoryCategory memoryCategory,
			uint64_t nBytes );

public:
	static Terrain3DRdContext &getShared( void );
	static void shutdownShared( void );

	Terrain3DRdContext( void ) = default;
	~Terrain3DRdContext( void );

	Terrain3DRdContext( const Terrain3DRdContext & ) = delete;
	Terrain3DRdContext &operator=( const Terrain3DRdContext & ) = delete;

	bool initialize( void );
	bool validateDevice( void );
	void shutdown( void );
	void invalidateDevice( const String &rReason );
	bool beginRebuild( void );
	bool completeRebuild( bool bSuccess, const String &rFailure = String() );
	void recordFailure( FailureCode code, const String &rFailure );
	void recordResourceCreationFailure( const String &rName, ResourceType type, const String &rFailure = String() );

	// Compatibility entry point. New owners should use the typed overload so
	// accounting never depends on a debug-name convention.
	void registerResource( const RID &rRid, const String &rName, uint64_t nBufferBytes = 0u );
	bool registerResource( const RID &rRid,
			const String &rName,
			ResourceType type,
			MemoryCategory memoryCategory,
			uint64_t nBytes = 0u );
	void registerTextureResource( const RID &rRid, const String &rName, uint64_t nTextureBytes );
	bool registerVertexFormat( int64_t nFormat, const String &rName );
	void releaseVertexFormat( int64_t &rFormat );
	void freeResource( RID &rRid, uint64_t nBufferBytes = 0u );
	void freeTextureResource( RID &rRid, uint64_t nTextureBytes );

	void markWarmupComplete( bool bComplete = true );
	void beginResourceRebuild( void );
	void endResourceRebuild( void );
	void beginFrame( uint64_t nGpuFrameId, uint64_t nSnapshotGeneration );
	void recordBufferUpdate( uint64_t nBytes );
	void recordSuccessfulPass( SuccessfulPass pass, uint64_t nGpuFrameId );
	void recordSceneCounts( uint64_t nTerrainPatches,
			uint64_t nVisibleTerrainPatches,
			uint64_t nStaticInstances,
			uint64_t nVisibleInstances,
			uint64_t nHlods,
			uint64_t nVisibleHlods );
	void recordSubmissionCounts( uint64_t nIndirectCommands, uint64_t nComputeDispatches );
	Dictionary getDiagnostics( void ) const;

	EA_FORCE_INLINE RenderingDevice *getDevice( void ) const {
		const ContextState state = m_state.load( std::memory_order_acquire );
		return ( state == STATE_READY || state == STATE_REBUILDING ) ? m_pRd.load( std::memory_order_acquire ) : nullptr;
	}
	EA_FORCE_INLINE bool isReady( void ) const {
		return m_state.load( std::memory_order_acquire ) == STATE_READY &&
				m_pRd.load( std::memory_order_acquire ) != nullptr;
	}
	EA_FORCE_INLINE bool hasFailure( void ) const {
		return m_failureCode.load( std::memory_order_acquire ) != FAILURE_NONE;
	}
	EA_FORCE_INLINE ContextState getState( void ) const { return m_state.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE FailureCode getFailureCode( void ) const { return m_failureCode.load( std::memory_order_acquire ); }
	EA_FORCE_INLINE uint64_t getDeviceGeneration( void ) const {
		return m_nDeviceGeneration.load( std::memory_order_acquire );
	}
	EA_FORCE_INLINE uint64_t getLiveBufferBytes( void ) const {
		return m_nLiveBufferBytes.load( std::memory_order_acquire );
	}
	EA_FORCE_INLINE uint64_t getLiveBytes( const MemoryCategory category ) const {
		return category < MEMORY_CATEGORY_COUNT ? m_szLiveMemory[category].load( std::memory_order_acquire ) : 0u;
	}
};

#endif // TERRAIN3D_RD_CONTEXT_H
