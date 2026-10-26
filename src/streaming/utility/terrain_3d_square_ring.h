// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_SQUARE_RING_H
#define TERRAIN3D_SQUARE_RING_H

template <typename TVector2i, typename TCallback>
static bool t3dForEachSquareRing(
        const TVector2i &rCenter, const int nMaxRadius, TCallback pCallback, void *const pContext ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !pCallback( pContext, rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !pCallback( pContext, rCenter + TVector2i( x, -radius ) )
			        || !pCallback( pContext, rCenter + TVector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !pCallback( pContext, rCenter + TVector2i( -radius, y ) )
			        || !pCallback( pContext, rCenter + TVector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

#endif // TERRAIN3D_SQUARE_RING_H
