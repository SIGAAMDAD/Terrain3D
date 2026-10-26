// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_GLOBAL_DESCRIPTOR_CONTRACT_H
#define TERRAIN3D_RD_GLOBAL_DESCRIPTOR_CONTRACT_H

#include "Terrain3DEnums.h"
#include <godot_cpp/variant/rid.hpp>
#include <stdint.h>
#include <stddef.h>

// Source inventory: the largest current terrain/world sets contain dozens of
// bindings and paired sampler/texture RIDs; the supplied RD GLSL
// variants top out at 13 bindings per set. These are per-recipe *hard* bounds,
// not a preallocated global cache size. No silent truncation is permitted.
#define T3D_RD_DESCRIPTOR_MAX_BINDINGS	   ( 32u )
#define T3D_RD_DESCRIPTOR_MAX_RESOURCES	   ( 64u )
#define T3D_RD_DESCRIPTOR_INVALID_SLOT	   ( UINT32_MAX )
#define T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID ( UINT64_C( 1 ) )

enum t3dRdDescriptorDeviceDomain_t : uint8_t
{
	T3D_RD_DESCRIPTOR_DEVICE_GLOBAL = 0u,
	T3D_RD_DESCRIPTOR_DEVICE_LOCAL = 1u
};

enum t3dRdDescriptorStatus_t : uint8_t
{
	T3D_RD_DESCRIPTOR_OK = 0u,
	T3D_RD_DESCRIPTOR_INVALID_ARGUMENT,
	T3D_RD_DESCRIPTOR_INVALID_RECIPE,
	T3D_RD_DESCRIPTOR_RECIPE_CAPACITY_EXCEEDED,
	T3D_RD_DESCRIPTOR_WRONG_DEVICE,
	T3D_RD_DESCRIPTOR_STALE_DEVICE,
	T3D_RD_DESCRIPTOR_STALE_LEASE,
	T3D_RD_DESCRIPTOR_INVALID_DEPENDENCY,
	T3D_RD_DESCRIPTOR_CACHE_CAPACITY_EXCEEDED,
	T3D_RD_DESCRIPTOR_RESOURCE_REGISTRY_EXHAUSTED,
	T3D_RD_DESCRIPTOR_CREATE_FAILED,
	T3D_RD_DESCRIPTOR_RETIRE_PENDING
};

// Each physical RID belongs to exactly one device. The LOCAL identity must
// distinguish separate local RenderingDevices; the shared GLOBAL device uses
// T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID. A global cache rejects LOCAL recipes.
typedef struct t3dRdDescriptorDeviceKey_s
{
	uint64_t					  m_nDeviceIdentity = 0u;
	uint64_t					  m_nDeviceGeneration = 0u;
	t3dRdDescriptorDeviceDomain_t m_eDomain = T3D_RD_DESCRIPTOR_DEVICE_GLOBAL;
} t3dRdDescriptorDeviceKey_t;

typedef struct t3dRdDescriptorBinding_s
{
	uint16_t m_nBinding = 0u;
	uint16_t m_nType = 0u;
	uint16_t m_nResourceFirst = 0u;
	uint16_t m_nResourceCount = 0u;
} t3dRdDescriptorBinding_t;
static_assert( sizeof( t3dRdDescriptorBinding_t ) == 8u, "Descriptor binding tuple ABI" );

// RID is a Godot value wrapper. Recipes own no physical resources or heap
// memory. Bindings are strictly ascending, and RID lists retain the shader's
// declared order (sampler then texture for combined bindings).
typedef struct t3dRdDescriptorRecipe_s
{
	godot::RID				   m_hShader;
	t3dRdDescriptorDeviceKey_t m_device;
	uint32_t				   m_nSetIndex = 0u;
	uint16_t				   m_nBindingCount = 0u;
	uint16_t				   m_nResourceCount = 0u;
	t3dRdDescriptorBinding_t   m_szBindings[T3D_RD_DESCRIPTOR_MAX_BINDINGS]{};
	godot::RID				   m_szResources[T3D_RD_DESCRIPTOR_MAX_RESOURCES]{};
} t3dRdDescriptorRecipe_t;

// Slot identity is independent of RID identity. A stale lease cannot resolve
// to a recycled slot, even when the physical RID value has been reused.
typedef struct t3dRdDescriptorLease_s
{
	uint32_t m_nSlot = T3D_RD_DESCRIPTOR_INVALID_SLOT;
	uint32_t m_nSlotGeneration = 0u;
	uint64_t m_nDeviceGeneration = 0u;
} t3dRdDescriptorLease_t;
static_assert( sizeof( t3dRdDescriptorLease_t ) == 16u, "Descriptor lease ABI" );

// P04 configures physical slots once during cold initialization using the
// actual free capacity of the existing T3D_RESOURCE_CAPACITY RID registry.
// There is intentionally no default descriptor-slot allocation.
typedef struct t3dRdDescriptorCapacityRequest_s
{
	uint32_t m_nRequestedSlots = 0u;
	uint32_t m_nRegistrySlotsFree = 0u;
	uint32_t m_nNonDescriptorSlotsReserved = 0u;
} t3dRdDescriptorCapacityRequest_t;

// Snapshot schema for the global cache implementation. Per-owner counters
// track logical requests even if multiple owners share a physical RID.
typedef struct t3dRdGlobalDescriptorDiagnostics_s
{
	uint32_t m_nSlotsTotal = 0u;
	uint32_t m_nSlotsLive = 0u;
	uint32_t m_nSlotsPeak = 0u;
	uint32_t m_nRetirePending = 0u;
	uint64_t m_nLeasesLive = 0u;
	uint64_t m_nCacheHits = 0u;
	uint64_t m_nCacheMisses = 0u;
	uint64_t m_nCreates = 0u;
	uint64_t m_nPhysicalReleases = 0u;
	uint64_t m_nCapacityDenials = 0u;
	uint64_t m_nInvalidDependencyRejects = 0u;
	uint64_t m_nStaleLeaseRejects = 0u;
	uint64_t m_nGpuSafeRetirements = 0u;
	uint64_t m_nFenceSubmissions = 0u;
	uint64_t m_nFenceCompletions = 0u;
	uint64_t m_nFenceSubmissionFailures = 0u;
	uint64_t m_nFenceReadbackFailures = 0u;
	uint64_t m_nFenceStaleCallbacks = 0u;
	uint64_t m_nActiveSubmissionEpoch = 0u;
	uint64_t m_nCompletedSubmissionEpoch = 0u;
	bool m_bFencePending = false;
	uint64_t m_szPerOwnerRequests[T3D_RD_CONTEXT_OWNER_COUNT]{};
} t3dRdGlobalDescriptorDiagnostics_t;

#endif // TERRAIN3D_RD_GLOBAL_DESCRIPTOR_CONTRACT_H
