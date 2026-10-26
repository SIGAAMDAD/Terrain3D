// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGION_CANDIDATE_COLLECTOR_H
#define TERRAIN3D_REGION_CANDIDATE_COLLECTOR_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include <stddef.h>
#include <stdint.h>

#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "streaming/residency/terrain_3d_residency_state.h"
template< typename TVector2i, typename TCallback >
static bool t3dForEachSquareRing(
	const TVector2i &rCenter,
	const int		 nMaxRadius,
	TCallback		 pCallback,
	void *const		 pContext ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !pCallback( pContext, rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !pCallback( pContext, rCenter + TVector2i( x, -radius ) ) ||
				 !pCallback( pContext, rCenter + TVector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !pCallback( pContext, rCenter + TVector2i( -radius, y ) ) ||
				 !pCallback( pContext, rCenter + TVector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

typedef bool ( *t3dRegionCandidatePredicate_t )( const Terrain3DRegion *pRegion,
												 const Vector2i		   &rLocation,
												 void				   *pUserData );

typedef struct t3dRegionCandidateCollectContext_s
{
	const Terrain3DRegionRepository *m_pRepository;
	const t3dResidencyFrameState_s	*m_pFrame;
	Vector2i						*m_pLocations;
	uint8_t							*m_pSelectedMap;
	t3dRegionStreamingState_t		 m_eWanted;
	t3dRegionCandidatePredicate_t	 m_pPredicate;
	void							*m_pPredicateUserData;
	int								 m_nCapacity;
	int								 m_nOutputCount;
} t3dRegionCandidateCollectContext_t;

static bool t3dCollectRegionCandidateVisit(
	void *const		pContext,
	const Vector2i &rLocation ) {
	t3dRegionCandidateCollectContext_t *const pState = ( t3dRegionCandidateCollectContext_t * ) ( pContext );
	if ( pState->m_nOutputCount >= pState->m_nCapacity ) {
		return false;
	}
	if ( !pState->m_pRepository->isLocationValid( rLocation ) ) {
		return true;
	}
	const Terrain3DRegion *const pRegion = pState->m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pState->m_pFrame->streamingState( rLocation ) != pState->m_eWanted ) {
		return true;
	}
	if ( pState->m_pPredicate && !pState->m_pPredicate( pRegion, rLocation, pState->m_pPredicateUserData ) ) {
		return true;
	}
	if ( pState->m_pSelectedMap ) {
		const int nMapIndex = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
		if ( nMapIndex < 0 || pState->m_pSelectedMap[nMapIndex] != 0u ) {
			return true;
		}
		pState->m_pSelectedMap[nMapIndex] = 1u;
	}
	pState->m_pLocations[pState->m_nOutputCount++] = rLocation;
	return pState->m_nOutputCount < pState->m_nCapacity;
}

static int t3dCollectDesiredRegionsForState(
	const Terrain3DRegionRepository	   &rRepository,
	const t3dResidencyFrameState_s	   &rFrame,
	const Vector2i					   &rCenter,
	const int							nMaxRadius,
	const t3dRegionStreamingState_t		eWanted,
	Vector2i *const						pLocations,
	const int							nCapacity,
	const int							nOutputCount,
	uint8_t *const						pSelectedMap = NULL,
	const t3dRegionCandidatePredicate_t pPredicate = NULL,
	void *const							pPredicateUserData = NULL ) {
	if ( !pLocations || nCapacity <= 0 || nOutputCount >= nCapacity ) {
		return nOutputCount;
	}
	t3dRegionCandidateCollectContext_t context;
	context.m_pRepository = &rRepository;
	context.m_pFrame = &rFrame;
	context.m_pLocations = pLocations;
	context.m_pSelectedMap = pSelectedMap;
	context.m_eWanted = eWanted;
	context.m_pPredicate = pPredicate;
	context.m_pPredicateUserData = pPredicateUserData;
	context.m_nCapacity = nCapacity;
	context.m_nOutputCount = nOutputCount;
	t3dForEachSquareRing( rCenter, nMaxRadius, t3dCollectRegionCandidateVisit, &context );
	return context.m_nOutputCount;
}

static int t3dAppendPrefetchRegionsFromRepositorySlots(
	const Terrain3DRegionRepository &rRepository,
	const t3dResidencyFrameState_s	&rFrame,
	Vector2i *const					 pLocations,
	const int						 nCapacity,
	const int						 nOutputCount,
	uint8_t *const					 pSelectedMap ) {
	if ( !pLocations || !pSelectedMap || nCapacity <= 0 || nOutputCount >= nCapacity ) {
		return nOutputCount;
	}
	int outputCount = nOutputCount;
	for ( int slot = 0; slot < rRepository.getSlotCount() && outputCount < nCapacity; ++slot ) {
		const Vector2i location = t3dUnpackRegionCoord( rRepository.getSlotsData()[slot] );
		const int	   mapIndex = location == s_v2iMax ? -1 : Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( mapIndex < 0 || pSelectedMap[mapIndex] != 0u || !rRepository.getRegionPtr( location ) ||
			 !rFrame.isPrefetch( location ) ) {
			continue;
		}
		pLocations[outputCount++] = location;
		pSelectedMap[mapIndex] = 1u;
	}
	return outputCount;
}

#endif // TERRAIN3D_REGION_CANDIDATE_COLLECTOR_H
