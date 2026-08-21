// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_CLIPMAP_BOUNDS_H
#define TERRAIN3D_CLIPMAP_BOUNDS_H

typedef struct t3dClipmapVerticalBounds_s {
	double fMinY = -0.1;
	double fSizeY = 0.2;
} t3dClipmapVerticalBounds_t;

inline t3dClipmapVerticalBounds_t terrain3dClipmapVerticalBounds(
	const double fHeightA,
	const double fHeightB, const double fCullMargin
) {
	const double fMinHeight = fHeightA < fHeightB ? fHeightA : fHeightB;
	const double fMaxHeight = fHeightA < fHeightB ? fHeightB : fHeightA;
	const double fMargin = fCullMargin > 0.0 ? fCullMargin : 0.0;
	const double fHeightSpan = fMaxHeight - fMinHeight;
	const double fMinimumSpan = 0.2;
	const double fBoundsSpan = fHeightSpan > fMinimumSpan ? fHeightSpan : fMinimumSpan;
	const double fHalfExtraSpan = ( fBoundsSpan - fHeightSpan ) * 0.5;

	t3dClipmapVerticalBounds_t bounds;
	bounds.fMinY = fMinHeight - fMargin - fHalfExtraSpan;
	bounds.fSizeY = fBoundsSpan + fMargin * 2.0;
	return bounds;
}

#endif // TERRAIN3D_CLIPMAP_BOUNDS_H
