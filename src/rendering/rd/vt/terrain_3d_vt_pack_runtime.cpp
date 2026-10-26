// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/vt/terrain_3d_vt_pack_runtime.h"

#include <limits.h>
#include <string.h>

static bool t3d_vt_pack_runtime_tail_block_layout(
        const uint32_t nChannel, uint32_t &rBlockWidth, uint32_t &rBlockHeight, uint32_t &rBlockBytes ) {
	rBlockWidth = 0u;
	rBlockHeight = 0u;
	rBlockBytes = 0u;
	switch ( nChannel ) {
		case T3D_VT_CHANNEL_ALBEDO:
			rBlockWidth = 4u;
			rBlockHeight = 4u;
			rBlockBytes = 8u;
			return true;
		case T3D_VT_CHANNEL_NORMAL:
		case T3D_VT_CHANNEL_SURFACE:
			rBlockWidth = 4u;
			rBlockHeight = 4u;
			rBlockBytes = 16u;
			return true;
		case T3D_VT_CHANNEL_HEIGHT:
			rBlockWidth = 1u;
			rBlockHeight = 1u;
			rBlockBytes = 1u;
			return true;
		default:
			return false;
	}
}

static uint64_t t3d_vt_runtime_align64( const uint64_t nValue ) {
	return ( nValue + 63u ) & ~63ull;
}

void t3d_vt_pack_runtime_clear( T3DVTPackRuntimeStorage &rStorage ) {
	memset( &rStorage, 0, sizeof( rStorage ) );
}

static bool t3d_vt_pack_runtime_raw_clusters_only_v3( const t3dVTPackViewV3_t &rView ) {
	if ( rView.m_pHeader == NULL || rView.m_pClusters == NULL ) {
		return false;
	}
	for ( uint32_t iCluster = 0u; iCluster < rView.m_pHeader->m_nClusterCount; ++iCluster ) {
		if ( rView.m_pClusters[iCluster].m_nCodec != T3D_VT_PACK_CODEC_NONE ) {
			return false;
		}
	}
	return true;
}

static bool t3d_vt_pack_runtime_open_v3_cold( const void *const pPackage,
        const uint64_t nPackageBytes,
        const T3DVTPackRuntimeAllocator &rAllocator,
        T3DVTPackRuntimeStorage &rStorage ) {
	if ( pPackage == NULL || nPackageBytes == 0u || rAllocator.pAllocate == NULL || rAllocator.pFree == NULL
	        || rStorage.pOwnedBlock != NULL ) {
		return false;
	}
	t3dVTPackViewV3_t SourceView;
	if ( !t3d_vt_pack_open_view_v3( pPackage, nPackageBytes, SourceView ) || !t3d_vt_pack_runtime_raw_clusters_only_v3( SourceView ) ) {
		return false;
	}
	const t3dVTPackHeaderV3_t &Header = *SourceView.m_pHeader;
	if ( Header.m_nAllocationCount == 0u || Header.m_nPayloadCount == 0u || Header.m_nQualityViewCount == 0u
	        || Header.m_nFormatFamilyCount != T3D_VT_LOGICAL_CHANNEL_COUNT
	        || Header.m_nAllocationCount > UINT32_MAX / T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION ) {
		return false;
	}
	const uint32_t nRuntimeAllocationCount = Header.m_nAllocationCount * T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION;

	uint64_t nBytes = t3d_vt_runtime_align64( nPackageBytes );
#define T3D_VT_RUNTIME_ADD( count, type )                                                                                                  \
	do {                                                                                                                                     \
		if ( ( uint64_t )( count ) > ( UINT64_MAX - nBytes ) / sizeof( type ) ) { return false; }                                              \
		nBytes = t3d_vt_runtime_align64( nBytes ) + ( uint64_t )( count ) * sizeof( type );                                                     \
	} while ( 0 )
	T3D_VT_RUNTIME_ADD( 1u, T3DVTFileHeader );
	T3D_VT_RUNTIME_ADD( nRuntimeAllocationCount, T3DVTAllocationRecordV1 );
	T3D_VT_RUNTIME_ADD( Header.m_nPageCount, T3DVTPageFileRecord );
	T3D_VT_RUNTIME_ADD( Header.m_nPayloadCount, T3DVTPayloadRecord );
	T3D_VT_RUNTIME_ADD( nRuntimeAllocationCount, t3dVTAllocation_t );
	T3D_VT_RUNTIME_ADD( Header.m_nPayloadCount, T3DVTRuntimePayloadRecord );
	T3D_VT_RUNTIME_ADD( nRuntimeAllocationCount, t3dVTTailMetadata_t );
	T3D_VT_RUNTIME_ADD( nRuntimeAllocationCount, T3DVTRuntimeTailSourceRecord );
#undef T3D_VT_RUNTIME_ADD
	if ( nBytes == 0u || nBytes > SIZE_MAX ) {
		return false;
	}

	void *const pBlock = rAllocator.pAllocate( rAllocator.pUser, ( size_t )nBytes, 64u );
	if ( pBlock == NULL ) {
		return false;
	}
	memset( pBlock, 0, ( size_t )nBytes );
	T3DVTPackRuntimeStorage Candidate;
	t3d_vt_pack_runtime_clear( Candidate );
	Candidate.pOwnedBlock = pBlock;
	Candidate.nOwnedBytes = nBytes;
	uint8_t *const pBase = ( uint8_t * )pBlock;
	uint64_t nOffset = 0u;
	Candidate.pPackageBytes = pBase;
	Candidate.nPackageBytes = nPackageBytes;
	memcpy( Candidate.pPackageBytes, pPackage, ( size_t )nPackageBytes );
	nOffset = t3d_vt_runtime_align64( nPackageBytes );
	if ( !t3d_vt_pack_open_view_v3( Candidate.pPackageBytes, nPackageBytes, Candidate.PackViewV3 )
	        || !t3d_vt_pack_runtime_raw_clusters_only_v3( Candidate.PackViewV3 ) ) {
		rAllocator.pFree( rAllocator.pUser, pBlock );
		return false;
	}

#define T3D_VT_RUNTIME_TAKE( field, count, type )                                                                                          \
	do {                                                                                                                                     \
		Candidate.field = ( type * )( pBase + nOffset );                                                                                      \
		nOffset = t3d_vt_runtime_align64( nOffset + ( uint64_t )( count ) * sizeof( type ) );                                                   \
	} while ( 0 )
	T3D_VT_RUNTIME_TAKE( pRuntimeHeader, 1u, T3DVTFileHeader );
	T3D_VT_RUNTIME_TAKE( pLegacyAllocations, nRuntimeAllocationCount, T3DVTAllocationRecordV1 );
	T3D_VT_RUNTIME_TAKE( pLegacyPages, Header.m_nPageCount, T3DVTPageFileRecord );
	T3D_VT_RUNTIME_TAKE( pLegacyPayloads, Header.m_nPayloadCount, T3DVTPayloadRecord );
	T3D_VT_RUNTIME_TAKE( pRuntimeAllocations, nRuntimeAllocationCount, t3dVTAllocation_t );
	T3D_VT_RUNTIME_TAKE( pRuntimePayloads, Header.m_nPayloadCount, T3DVTRuntimePayloadRecord );
	T3D_VT_RUNTIME_TAKE( pRuntimeTails, nRuntimeAllocationCount, t3dVTTailMetadata_t );
	T3D_VT_RUNTIME_TAKE( pRuntimeTailSources, nRuntimeAllocationCount, T3DVTRuntimeTailSourceRecord );
#undef T3D_VT_RUNTIME_TAKE

	T3DVTFileHeader &RuntimeHeader = *Candidate.pRuntimeHeader;
	RuntimeHeader.nMagic = T3D_VT_FILE_MAGIC;
	RuntimeHeader.nVersion = T3D_VT_FILE_VERSION_V3;
	RuntimeHeader.nHeaderBytes = sizeof( T3DVTFileHeader );
	RuntimeHeader.nAllocationCount = nRuntimeAllocationCount;
	RuntimeHeader.nPageCount = Header.m_nPageCount;
	RuntimeHeader.nPayloadCount = Header.m_nPayloadCount;
	RuntimeHeader.nChannelCount = T3D_VT_PHYSICAL_FORMAT_COUNT;
	RuntimeHeader.nPayloadDataBytes = Header.m_nPayloadDataBytes;
	RuntimeHeader.nContentSignature = Header.m_nContentSignature;
	RuntimeHeader.nInteriorTexels = T3D_VT_PAGE_INTERIOR;
	RuntimeHeader.nGutterTexels = T3D_VT_PAGE_GUTTER;
	RuntimeHeader.nPhysicalTexels = T3D_VT_PHYSICAL_TILE;

	for ( uint32_t iAllocation = 0u; iAllocation < Header.m_nAllocationCount; ++iAllocation ) {
		const t3dVTPackAllocationRecordV3_t &SourceAllocation = Candidate.PackViewV3.m_pAllocations[iAllocation];
		const t3dVTConsumer_t Consumer = ( t3dVTConsumer_t )SourceAllocation.m_nConsumer;
		const uint32_t nRuntimeBase = t3d_vt_v3_runtime_allocation_base( iAllocation );
		for ( uint32_t iChannel = 0u; iChannel < T3D_VT_LOGICAL_CHANNEL_COUNT; ++iChannel ) {
			uint32_t nSlot = T3D_VT_INVALID_SLOT;
			if ( !t3d_vt_v3_runtime_slot_for_channel( Consumer, ( t3dVTLogicalChannel_t )iChannel, nSlot ) ) {
				continue;
			}
			const uint32_t nRuntimeId = nRuntimeBase + nSlot;
			const t3dVTChannelAllocation_t &SourceChannel =
			        Candidate.PackViewV3.m_pChannelAllocations[iAllocation * T3D_VT_LOGICAL_CHANNEL_COUNT + iChannel];
			T3DVTAllocationRecordV1 &Legacy = Candidate.pLegacyAllocations[nRuntimeId];
			Legacy.nReserved0 = T3D_VT_RESIDENCY_TERRAIN;
			if ( SourceChannel.m_nPageCount == 0u ) {
				Candidate.pRuntimeAllocations[nRuntimeId].m_nTailIndex = T3D_VT_INVALID_TAIL;
				for ( uint32_t c = 0u; c < T3D_VT_CHANNEL_COUNT; ++c ) Candidate.pRuntimeTailSources[nRuntimeId].nPayloadId[c] = T3D_VT_INVALID_PAGE;
				continue;
			}
			if ( SourceChannel.m_nTailIndex >= Header.m_nTailCount ) {
				rAllocator.pFree( rAllocator.pUser, pBlock ); return false;
			}
			const t3dVTPackTailRecordV3_t &SourceTail = Candidate.PackViewV3.m_pTails[SourceChannel.m_nTailIndex];
			if ( SourceTail.m_nChannel != iChannel || SourceTail.m_nPayloadId >= Header.m_nPayloadCount
			        || SourceTail.m_nFirstMipWidth == 0u || SourceTail.m_nFirstMipHeight == 0u
			        || SourceTail.m_nFirstMipWidth > T3D_VT_PACK_TAIL_SLOT_HEIGHT
			        || SourceTail.m_nFirstMipHeight > T3D_VT_PACK_TAIL_SLOT_HEIGHT
			        || SourceTail.m_nAtlasInteriorWidth == 0u || SourceTail.m_nAtlasInteriorHeight == 0u
			        || SourceTail.m_nAtlasInteriorWidth > T3D_VT_PACK_TAIL_SLOT_WIDTH
			        || SourceTail.m_nAtlasInteriorHeight > T3D_VT_PACK_TAIL_SLOT_HEIGHT
			        || SourceTail.m_nLayout != T3D_VT_PACK_TAIL_LAYOUT_HORIZONTAL_V1 ) {
				rAllocator.pFree( rAllocator.pUser, pBlock ); return false;
			}
			t3dVTAllocation_t &RuntimeAllocation = Candidate.pRuntimeAllocations[nRuntimeId];
			RuntimeAllocation.m_nBaseVirtualPage = SourceChannel.m_nBaseVirtualPage;
			RuntimeAllocation.m_nPageCount = SourceChannel.m_nPageCount;
			RuntimeAllocation.m_nMip0WidthPages = SourceChannel.m_nMip0WidthPages;
			RuntimeAllocation.m_nMip0HeightPages = SourceChannel.m_nMip0HeightPages;
			RuntimeAllocation.m_nMipCount = SourceChannel.m_nMipCount;
			RuntimeAllocation.m_nFlags = SourceChannel.m_nFlags;
			RuntimeAllocation.m_nTailIndex = nRuntimeId;
			RuntimeAllocation.m_nGeneration = SourceAllocation.m_nGeneration;
			RuntimeAllocation.m_nContentId = SourceAllocation.m_nContentId;
			RuntimeAllocation.m_nReserved = T3D_VT_RESIDENCY_TERRAIN;

			Legacy.nBaseVirtualPage = SourceChannel.m_nBaseVirtualPage;
			Legacy.nPageCount = SourceChannel.m_nPageCount;
			Legacy.nMip0WidthPages = SourceChannel.m_nMip0WidthPages;
			Legacy.nMip0HeightPages = SourceChannel.m_nMip0HeightPages;
			Legacy.nMipCount = SourceChannel.m_nMipCount;
			Legacy.nChannel = ( uint16_t )t3d_vt_logical_channel_physical_family( ( t3dVTLogicalChannel_t )iChannel );
			Legacy.nFlags = SourceChannel.m_nFlags;
			Legacy.nRootPage = SourceChannel.m_nPageCount != 0u ? SourceChannel.m_nBaseVirtualPage + SourceChannel.m_nPageCount - 1u : 0u;
			Legacy.nReserved1 = SourceAllocation.m_nContentId;

			t3dVTTailMetadata_t &RuntimeTail = Candidate.pRuntimeTails[nRuntimeId];
			RuntimeTail.m_nAtlasX = ( nRuntimeId % T3D_VT_PACK_TAILS_PER_ROW ) * T3D_VT_PACK_TAIL_SLOT_WIDTH;
			RuntimeTail.m_nAtlasY = ( nRuntimeId / T3D_VT_PACK_TAILS_PER_ROW ) * T3D_VT_PACK_TAIL_SLOT_HEIGHT;
			RuntimeTail.m_nWidthTexels = SourceTail.m_nFirstMipWidth;
			RuntimeTail.m_nHeightTexels = SourceTail.m_nFirstMipHeight;
			RuntimeTail.m_nFirstMip = SourceTail.m_nFirstMip;
			RuntimeTail.m_nMipCount = SourceTail.m_nMipCount;
			RuntimeTail.m_nGeneration = SourceAllocation.m_nGeneration;
			const uint32_t nFamily = ( uint32_t )t3d_vt_logical_channel_physical_family( ( t3dVTLogicalChannel_t )iChannel );
			RuntimeTail.m_nChannelMask = 1u << nFamily;
			RuntimeTail.m_nContentId = SourceTail.m_nContentId;
			for ( uint32_t c = 0u; c < T3D_VT_CHANNEL_COUNT; ++c ) Candidate.pRuntimeTailSources[nRuntimeId].nPayloadId[c] = T3D_VT_INVALID_PAGE;
			Candidate.pRuntimeTailSources[nRuntimeId].nPayloadId[nFamily] = SourceTail.m_nPayloadId;
			Candidate.pRuntimeTailSources[nRuntimeId].nAtlasInteriorWidth = SourceTail.m_nAtlasInteriorWidth;
			Candidate.pRuntimeTailSources[nRuntimeId].nAtlasInteriorHeight = SourceTail.m_nAtlasInteriorHeight;
		}
	}

	for ( uint32_t iPage = 0u; iPage < Header.m_nPageCount; ++iPage ) {
		const t3dVTPackPageRecordV3_t &SourcePage = Candidate.PackViewV3.m_pPages[iPage];
		if ( SourcePage.m_nAllocationId >= Header.m_nAllocationCount || SourcePage.m_nChannel >= T3D_VT_LOGICAL_CHANNEL_COUNT ) {
			rAllocator.pFree( rAllocator.pUser, pBlock ); return false;
		}
		const t3dVTConsumer_t Consumer = ( t3dVTConsumer_t )Candidate.PackViewV3.m_pAllocations[SourcePage.m_nAllocationId].m_nConsumer;
		uint32_t nSlot = T3D_VT_INVALID_SLOT;
		if ( !t3d_vt_v3_runtime_slot_for_channel( Consumer, ( t3dVTLogicalChannel_t )SourcePage.m_nChannel, nSlot ) ) {
			rAllocator.pFree( rAllocator.pUser, pBlock ); return false;
		}
		T3DVTPageFileRecord &RuntimePage = Candidate.pLegacyPages[iPage];
		RuntimePage.nVirtualPage = SourcePage.m_nVirtualPage;
		RuntimePage.nParentPage = SourcePage.m_nParentPage;
		RuntimePage.nAllocationId = t3d_vt_v3_runtime_allocation_base( SourcePage.m_nAllocationId ) + nSlot;
		RuntimePage.nPayloadId = SourcePage.m_nPayloadId;
		RuntimePage.nMip = SourcePage.m_nMip;
		RuntimePage.nChannel = ( uint16_t )t3d_vt_logical_channel_physical_family( ( t3dVTLogicalChannel_t )SourcePage.m_nChannel );
		RuntimePage.nFlags = SourcePage.m_nFlags;
		RuntimePage.nGeneration = SourcePage.m_nGeneration;
		RuntimePage.nReserved1 = SourcePage.m_nContentId;
	}

	for ( uint32_t iPayload = 0u; iPayload < Header.m_nPayloadCount; ++iPayload ) {
		const t3dVTPackPayloadRecordV3_t &Payload = Candidate.PackViewV3.m_pPayloads[iPayload];
		const t3dVTPackClusterRecordV3_t &Cluster = Candidate.PackViewV3.m_pClusters[Payload.m_nClusterId];
		if ( Cluster.m_nCodec != T3D_VT_PACK_CODEC_NONE || Payload.m_nClusterOffset > Cluster.m_nDecodedBytes
		        || Payload.m_nDecodedBytes > Cluster.m_nDecodedBytes - Payload.m_nClusterOffset ) {
			rAllocator.pFree( rAllocator.pUser, pBlock ); return false;
		}
		const uint64_t nPayloadOffset = Cluster.m_nPayloadDataOffset + Payload.m_nClusterOffset;
		T3DVTRuntimePayloadRecord &RuntimePayload = Candidate.pRuntimePayloads[iPayload];
		RuntimePayload.nOffset = nPayloadOffset;
		RuntimePayload.nBytes = Payload.m_nDecodedBytes;
		RuntimePayload.nChannel = Payload.m_nFormat;
		RuntimePayload.nContentId = ( uint32_t )( Payload.m_nContentSignature ^ ( Payload.m_nContentSignature >> 32u ) );
		RuntimePayload.nChecksum = Payload.m_nChecksum;
		T3DVTPayloadRecord &LegacyPayload = Candidate.pLegacyPayloads[iPayload];
		LegacyPayload.nOffset = nPayloadOffset;
		LegacyPayload.nCompressedBytes = Payload.m_nDecodedBytes;
		LegacyPayload.nDecodedBytes = Payload.m_nDecodedBytes;
		LegacyPayload.nCodec = T3D_VT_PACK_CODEC_NONE;
		LegacyPayload.nChecksum = Payload.m_nChecksum;
	}

	Candidate.Database.pHeader = Candidate.pRuntimeHeader;
	Candidate.Database.pAllocations = Candidate.pLegacyAllocations;
	Candidate.Database.pPages = Candidate.pLegacyPages;
	Candidate.Database.pPayloads = Candidate.pLegacyPayloads;
	Candidate.Database.pPayloadData = Candidate.PackViewV3.m_pPayloadData;
	Candidate.Database.nFileBytes = nPackageBytes;
	Candidate.Database.pRuntimeAllocations = Candidate.pRuntimeAllocations;
	Candidate.Database.pRuntimeTailMetadata = Candidate.pRuntimeTails;
	Candidate.Database.pRuntimeTailSources = Candidate.pRuntimeTailSources;
	Candidate.Database.nRuntimeTailCount = nRuntimeAllocationCount;
	Candidate.Database.pRuntimePayloads = Candidate.pRuntimePayloads;
	Candidate.Database.nRuntimePayloadCount = Header.m_nPayloadCount;
	Candidate.Database.pV3Allocations = Candidate.PackViewV3.m_pAllocations;
	Candidate.Database.pV3ChannelAllocations = Candidate.PackViewV3.m_pChannelAllocations;
	Candidate.Database.pV3Pages = Candidate.PackViewV3.m_pPages;
	Candidate.Database.pV3Tails = Candidate.PackViewV3.m_pTails;
	Candidate.Database.pV3QualityViews = Candidate.PackViewV3.m_pQualityViews;
	Candidate.Database.pV3FormatFamilies = Candidate.PackViewV3.m_pFormatFamilies;
	Candidate.Database.nV3ChannelAllocationCount = Header.m_nChannelAllocationCount;
	Candidate.Database.nV3PageCount = Header.m_nPageCount;
	Candidate.Database.nV3TailCount = Header.m_nTailCount;
	Candidate.Database.nV3QualityViewCount = Header.m_nQualityViewCount;
	Candidate.Database.nV3FormatFamilyCount = Header.m_nFormatFamilyCount;
	Candidate.Database.nV3FullySupportedProfileMask = Header.m_nFullySupportedProfileMask;
	Candidate.Database.nPackageGeneration = Header.m_nPackageGeneration;
	Candidate.Database.nPackageContentSignature = Header.m_nContentSignature;
	Candidate.Database.nRuntimeKind = T3D_VT_RUNTIME_DATABASE_PACK_V3;

	rStorage = Candidate;
	return true;
}

static const t3dVTPackQualityViewV3_t *t3d_vt_pack_runtime_find_quality_view_v3(
        const T3DVTDatabaseView &rDatabase, const uint32_t nAllocationId, const uint32_t nProfile ) {
	uint32_t nLow = 0u;
	uint32_t nHigh = rDatabase.nV3QualityViewCount;
	while ( nLow < nHigh ) {
		const uint32_t nMid = nLow + ( nHigh - nLow ) / 2u;
		const t3dVTPackQualityViewV3_t &View = rDatabase.pV3QualityViews[nMid];
		const uint64_t nKey = ( ( uint64_t )View.m_nAllocationId << 8u ) | View.m_nProfile;
		const uint64_t nWanted = ( ( uint64_t )nAllocationId << 8u ) | nProfile;
		if ( nKey < nWanted ) nLow = nMid + 1u; else nHigh = nMid;
	}
	if ( nLow >= rDatabase.nV3QualityViewCount ) return NULL;
	const t3dVTPackQualityViewV3_t &View = rDatabase.pV3QualityViews[nLow];
	return View.m_nAllocationId == nAllocationId && View.m_nProfile == nProfile ? &View : NULL;
}

bool t3d_vt_pack_runtime_apply_quality_profile_v3( const T3DVTDatabaseView &rDatabase,
        t3dVTAllocation_t *const pRuntimeAllocations, const uint32_t nRuntimeAllocationCount, const uint32_t nRequestedProfile,
        T3DVTQualityRuntimeStats &rStats ) {
	memset( &rStats, 0, sizeof( rStats ) );
	rStats.nRequestedProfile = nRequestedProfile;
	rStats.nPackageFullySupportedProfileMask = rDatabase.nV3FullySupportedProfileMask;
	if ( rDatabase.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3 || rDatabase.pHeader == NULL || rDatabase.pV3Allocations == NULL
	        || rDatabase.pV3QualityViews == NULL || pRuntimeAllocations == NULL || nRequestedProfile >= T3D_VT_QUALITY_PROFILE_COUNT ) return false;
	const uint32_t nLogicalAllocationCount = rDatabase.pHeader->nAllocationCount / T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION;
	if ( nLogicalAllocationCount == 0u || nRuntimeAllocationCount != nLogicalAllocationCount * T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION ) return false;
	for ( uint32_t iAllocation = 0u; iAllocation < nLogicalAllocationCount; ++iAllocation ) {
		const t3dVTPackAllocationRecordV3_t &Source = rDatabase.pV3Allocations[iAllocation];
		if ( Source.m_nHighestAvailableProfile >= T3D_VT_QUALITY_PROFILE_COUNT ) return false;
		const uint32_t nResolvedProfile = nRequestedProfile < Source.m_nHighestAvailableProfile ? nRequestedProfile : Source.m_nHighestAvailableProfile;
		if ( ( Source.m_nAvailableProfileMask & ( 1u << nResolvedProfile ) ) == 0u ) return false;
		const t3dVTPackQualityViewV3_t *const pView = t3d_vt_pack_runtime_find_quality_view_v3( rDatabase, iAllocation, nResolvedProfile );
		if ( pView == NULL ) return false;
		++rStats.nAllocationsByHighestProfile[Source.m_nHighestAvailableProfile];
		++rStats.nAllocationsByResolvedProfile[nResolvedProfile];
		if ( nResolvedProfile != nRequestedProfile ) ++rStats.nFallbackAllocationCount;
		const t3dVTConsumer_t Consumer = ( t3dVTConsumer_t )Source.m_nConsumer;
		for ( uint32_t nSlot = 0u; nSlot < T3D_VT_V3_RUNTIME_CHANNELS_PER_ALLOCATION; ++nSlot ) {
			t3dVTLogicalChannel_t Channel = T3D_VT_LOGICAL_CHANNEL_ALBEDO;
			if ( !t3d_vt_v3_logical_channel_for_runtime_slot( Consumer, nSlot, Channel ) ) return false;
			t3dVTAllocation_t &Runtime = pRuntimeAllocations[t3d_vt_v3_runtime_allocation_base( iAllocation ) + nSlot];
			Runtime.m_nFlags &= ( uint16_t )~T3D_VT_ALLOCATION_RUNTIME_QUALITY_MASK;
			if ( ( pView->m_nChannelMask & T3D_VT_LOGICAL_CHANNEL_BIT( Channel ) ) == 0u ) {
				Runtime.m_nFlags |= T3D_VT_ALLOCATION_QUALITY_DISABLED;
				continue;
			}
			const uint32_t nBaseMip = pView->m_nBaseMip[Channel];
			const uint32_t nMaxMip = pView->m_nMaxMip[Channel];
			if ( nBaseMip == T3D_VT_PACK_INVALID_MIP || nBaseMip > nMaxMip || nMaxMip >= Runtime.m_nMipCount || nBaseMip > 0x0fu ) return false;
			Runtime.m_nFlags |= ( uint16_t )( nBaseMip << T3D_VT_ALLOCATION_QUALITY_BASE_MIP_SHIFT );
		}
	}
	return true;
}

bool t3d_vt_pack_runtime_open_cold( const void *const pPackage,
        const uint64_t nPackageBytes,
        const T3DVTPackRuntimeAllocator &rAllocator,
        T3DVTPackRuntimeStorage &rStorage ) {
	if ( pPackage == NULL || nPackageBytes < sizeof( uint32_t ) * 2u || rStorage.pOwnedBlock != NULL ) {
		return false;
	}
	uint32_t nPrefix[2];
	memcpy( nPrefix, pPackage, sizeof( nPrefix ) );
	if ( nPrefix[0] != T3D_VT_FILE_MAGIC ) {
		return false;
	}
	if ( nPrefix[1] != T3D_VT_FILE_VERSION_V3 ) {
		return false;
	}
	return t3d_vt_pack_runtime_open_v3_cold( pPackage, nPackageBytes, rAllocator, rStorage );
}

void t3d_vt_pack_runtime_release_cold( const T3DVTPackRuntimeAllocator &rAllocator, T3DVTPackRuntimeStorage &rStorage ) {
	if ( rStorage.pOwnedBlock != NULL && rAllocator.pFree != NULL ) {
		rAllocator.pFree( rAllocator.pUser, rStorage.pOwnedBlock );
	}
	t3d_vt_pack_runtime_clear( rStorage );
}

bool t3d_vt_pack_runtime_find_allocation_by_stable_key(
        const T3DVTPackRuntimeStorage &rStorage, const uint64_t nStableKey, uint32_t &rAllocationId ) {
	rAllocationId = T3D_VT_INVALID_ALLOCATION;
	if ( nStableKey == 0u ) {
		return false;
	}
	const t3dVTPackAllocationKeyRecordV3_t *pKeys = rStorage.PackViewV3.m_pAllocationKeys;
	const uint32_t nAllocationCount = rStorage.PackViewV3.m_pHeader != NULL ? rStorage.PackViewV3.m_pHeader->m_nAllocationCount : 0u;
	if ( rStorage.Database.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3 || pKeys == NULL || nAllocationCount == 0u ) {
		return false;
	}
	uint32_t nLow = 0u;
	uint32_t nHigh = nAllocationCount;
	while ( nLow < nHigh ) {
		const uint32_t nMiddle = nLow + ( ( nHigh - nLow ) >> 1u );
		const uint64_t nKey = pKeys[nMiddle].m_nStableKey;
		if ( nKey < nStableKey ) {
			nLow = nMiddle + 1u;
		} else {
			nHigh = nMiddle;
		}
	}
	if ( nLow >= nAllocationCount || pKeys[nLow].m_nStableKey != nStableKey ) {
		return false;
	}
	rAllocationId = pKeys[nLow].m_nAllocationId;
	return rAllocationId < nAllocationCount;
}

bool t3d_vt_pack_runtime_copy_tail_mip_chain( const T3DVTDatabaseView &rDatabase,
        const uint32_t nRuntimeTailIndex,
        const uint32_t nChannel,
        uint8_t *const pDestination,
        const uint64_t nDestinationBytes,
        const uint32_t nDestinationWidth,
        const uint32_t nDestinationHeight ) {
	if ( rDatabase.nRuntimeKind != T3D_VT_RUNTIME_DATABASE_PACK_V3 || rDatabase.pHeader == NULL || rDatabase.pRuntimeTailMetadata == NULL
	        || rDatabase.pRuntimeTailSources == NULL || rDatabase.pRuntimePayloads == NULL || rDatabase.pPayloadData == NULL
	        || nRuntimeTailIndex >= rDatabase.nRuntimeTailCount || nChannel >= T3D_VT_CHANNEL_COUNT || pDestination == NULL
	        || nDestinationWidth == 0u || nDestinationHeight == 0u ) {
		return false;
	}
	const t3dVTTailMetadata_t &Tail = rDatabase.pRuntimeTailMetadata[nRuntimeTailIndex];
	const T3DVTRuntimeTailSourceRecord &TailSource = rDatabase.pRuntimeTailSources[nRuntimeTailIndex];
	const uint32_t nPayloadId = TailSource.nPayloadId[nChannel];
	if ( nPayloadId == T3D_VT_INVALID_PAGE ) {
		return true;
	}
	const uint32_t nChainWidth = TailSource.nAtlasInteriorWidth;
	const uint32_t nChainHeight = TailSource.nAtlasInteriorHeight;
	if ( nPayloadId >= rDatabase.nRuntimePayloadCount || Tail.m_nWidthTexels == 0u || Tail.m_nHeightTexels == 0u
	        || nChainWidth == 0u || nChainHeight == 0u || nChainWidth > T3D_VT_PACK_TAIL_SLOT_WIDTH
	        || nChainHeight > T3D_VT_PACK_TAIL_SLOT_HEIGHT || Tail.m_nAtlasX >= nDestinationWidth
	        || nChainWidth > nDestinationWidth - Tail.m_nAtlasX || Tail.m_nAtlasY >= nDestinationHeight
	        || nChainHeight > nDestinationHeight - Tail.m_nAtlasY ) {
		return false;
	}
	const T3DVTRuntimePayloadRecord &Payload = rDatabase.pRuntimePayloads[nPayloadId];
	if ( Payload.nChannel != nChannel || Payload.nOffset > rDatabase.pHeader->nPayloadDataBytes
	        || Payload.nBytes > rDatabase.pHeader->nPayloadDataBytes - Payload.nOffset ) {
		return false;
	}

	uint32_t nBlockWidth = 0u, nBlockHeight = 0u, nBlockBytes = 0u;
	if ( !t3d_vt_pack_runtime_tail_block_layout( nChannel, nBlockWidth, nBlockHeight, nBlockBytes ) || Tail.m_nAtlasX % nBlockWidth != 0u
	        || Tail.m_nAtlasY % nBlockHeight != 0u || T3D_VT_PAGE_GUTTER % nBlockWidth != 0u || T3D_VT_PAGE_GUTTER % nBlockHeight != 0u ) {
		return false;
	}
	const uint32_t nSourceBlocksWide = ( T3D_VT_PHYSICAL_TILE + nBlockWidth - 1u ) / nBlockWidth;
	const uint32_t nSourceBlocksHigh = ( T3D_VT_PHYSICAL_TILE + nBlockHeight - 1u ) / nBlockHeight;
	const uint32_t nDestinationBlocksWide = ( nDestinationWidth + nBlockWidth - 1u ) / nBlockWidth;
	const uint32_t nDestinationBlocksHigh = ( nDestinationHeight + nBlockHeight - 1u ) / nBlockHeight;
	// Copy the complete horizontal compact-tail chain. For BC formats this also
	// intentionally copies the final partially occupied compression block so the
	// 2x2/1x1 tail texels retain their original in-block coordinates.
	const uint32_t nCopyBlocksWide = ( nChainWidth + nBlockWidth - 1u ) / nBlockWidth;
	const uint32_t nCopyBlocksHigh = ( nChainHeight + nBlockHeight - 1u ) / nBlockHeight;
	const uint32_t nSourceBlockX = T3D_VT_PAGE_GUTTER / nBlockWidth;
	const uint32_t nSourceBlockY = T3D_VT_PAGE_GUTTER / nBlockHeight;
	const uint32_t nDestinationBlockX = Tail.m_nAtlasX / nBlockWidth;
	const uint32_t nDestinationBlockY = Tail.m_nAtlasY / nBlockHeight;
	const uint64_t nRequiredSourceBytes = ( uint64_t )nSourceBlocksWide * nSourceBlocksHigh * nBlockBytes;
	const uint64_t nRequiredDestinationBytes = ( uint64_t )nDestinationBlocksWide * nDestinationBlocksHigh * nBlockBytes;
	if ( nRequiredSourceBytes != Payload.nBytes || nRequiredDestinationBytes != nDestinationBytes
	        || nSourceBlockX + nCopyBlocksWide > nSourceBlocksWide || nSourceBlockY + nCopyBlocksHigh > nSourceBlocksHigh
	        || nDestinationBlockX + nCopyBlocksWide > nDestinationBlocksWide
	        || nDestinationBlockY + nCopyBlocksHigh > nDestinationBlocksHigh ) {
		return false;
	}
	const uint8_t *const pSource = rDatabase.pPayloadData + Payload.nOffset;
	const uint64_t nRowBytes = ( uint64_t )nCopyBlocksWide * nBlockBytes;
	for ( uint32_t iRow = 0u; iRow < nCopyBlocksHigh; ++iRow ) {
		const uint64_t nSourceOffset = ( ( uint64_t )( nSourceBlockY + iRow ) * nSourceBlocksWide + nSourceBlockX ) * nBlockBytes;
		const uint64_t nDestinationOffset =
		        ( ( uint64_t )( nDestinationBlockY + iRow ) * nDestinationBlocksWide + nDestinationBlockX ) * nBlockBytes;
		memcpy( pDestination + nDestinationOffset, pSource + nSourceOffset, ( size_t )nRowBytes );
	}
	return true;
}
