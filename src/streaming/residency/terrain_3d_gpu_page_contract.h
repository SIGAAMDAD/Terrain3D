// GPU-authoritative terrain residency: logical identities and transaction ABI.
#ifndef TERRAIN3D_GPU_PAGE_CONTRACT_H
#define TERRAIN3D_GPU_PAGE_CONTRACT_H

#include "Terrain3DConstants.h"
#include <stdint.h>

// Channel IDs are persistent within this migration but not part of cooked-file ABI.
enum t3dGpuPageChannel_t : uint8_t
{
	T3D_GPU_PAGE_HEIGHT = 0u,
	T3D_GPU_PAGE_CONTROL,
	T3D_GPU_PAGE_COLOR,
	T3D_GPU_PAGE_DERIVED,
	T3D_GPU_PAGE_COMPOSITE,
	T3D_GPU_PAGE_SHADOW,
	T3D_GPU_PAGE_CHANNEL_COUNT
};

enum t3dGpuPageRequestState_t : uint8_t
{
	T3D_GPU_PAGE_REQUEST_EMPTY = 0u,
	T3D_GPU_PAGE_REQUEST_QUEUED,
	T3D_GPU_PAGE_REQUEST_PREPARING,
	T3D_GPU_PAGE_REQUEST_READY,
	T3D_GPU_PAGE_REQUEST_FAILED
};

enum t3dGpuPagePriority_t : uint8_t
{
	T3D_GPU_PAGE_PRIORITY_PREFETCH = 1u,
	T3D_GPU_PAGE_PRIORITY_FAR = 2u,
	T3D_GPU_PAGE_PRIORITY_MIDDLE = 3u,
	T3D_GPU_PAGE_PRIORITY_NEAR = 4u,
	T3D_GPU_PAGE_PRIORITY_REQUIRED_HEIGHT = 5u
};

struct t3dGpuPageLogicalKey_t
{
	uint16_t m_nRegionIndex = UINT16_MAX;
	uint8_t	 m_nChannel = T3D_GPU_PAGE_CHANNEL_COUNT;
	uint8_t	 m_nQualityMip = 0u;
	bool	 valid() const {
		return m_nRegionIndex < T3D_REGION_SLOTS && m_nChannel < T3D_GPU_PAGE_CHANNEL_COUNT;
	}
};
static_assert( sizeof( t3dGpuPageLogicalKey_t ) == 4u, "Logical page key must remain compact" );

// Physical equality has no region or source generation component.
struct t3dGpuPageContentKey_t
{
	uint64_t m_nContentHash = 0u;
	uint32_t m_nFormatRecipe = 0u;
	uint32_t m_nPayloadBytes = 0u;
};
static_assert( sizeof( t3dGpuPageContentKey_t ) == 16u, "Physical content key ABI" );

struct t3dGpuPageHandle_t
{
	uint16_t m_nPoolIndex = UINT16_MAX;
	uint16_t m_nPhysicalLayer = UINT16_MAX;
	uint32_t m_nSlotGeneration = 0u;
	uint32_t m_nDeviceEpoch = 0u;
};
static_assert( sizeof( t3dGpuPageHandle_t ) == 12u, "Physical page handle ABI" );

struct t3dGpuPageTicket_t
{
	t3dGpuPageLogicalKey_t m_key;
	uint32_t			   m_nSourceGeneration = 0u;
	uint32_t			   m_nEditGeneration = 0u;
	uint32_t			   m_nDeviceEpoch = 0u;
	uint32_t			   m_nSlotGeneration = 0u;
	uint64_t			   m_nPublicationGeneration = 0u;
};
static_assert( sizeof( t3dGpuPageTicket_t ) == 32u, "Page work ticket ABI" );

inline bool t3dGpuPageTicketCurrent(
	const t3dGpuPageTicket_t &candidate,
	const t3dGpuPageTicket_t &current ) {
	return candidate.m_key.m_nRegionIndex == current.m_key.m_nRegionIndex &&
		   candidate.m_key.m_nChannel == current.m_key.m_nChannel &&
		   candidate.m_key.m_nQualityMip == current.m_key.m_nQualityMip &&
		   candidate.m_nSourceGeneration == current.m_nSourceGeneration &&
		   candidate.m_nEditGeneration == current.m_nEditGeneration && candidate.m_nDeviceEpoch != 0u &&
		   candidate.m_nDeviceEpoch == current.m_nDeviceEpoch &&
		   candidate.m_nSlotGeneration == current.m_nSlotGeneration &&
		   candidate.m_nPublicationGeneration == current.m_nPublicationGeneration;
}

// Last-reader epoch is a SUBMISSION epoch; it is not evidence of GPU completion.
inline bool t3dGpuPageCanRetire(
	uint64_t lastReaderEpoch,
	uint64_t confirmedCompletedEpoch ) {
	return lastReaderEpoch != UINT64_MAX && lastReaderEpoch <= confirmedCompletedEpoch;
}

#endif // TERRAIN3D_GPU_PAGE_CONTRACT_H
