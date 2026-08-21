// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_OWNER_H
#define TERRAIN3D_OWNER_H

#include "core/terrain_3d_compiler.h"

template <typename T>
EA_FORCE_INLINE T *t3dTransferOwner( T *&pOwner, T *pNextOwner ) {
	T *pPreviousOwner = pOwner;
	pOwner = pNextOwner;
	return pPreviousOwner;
}

template <typename T>
EA_FORCE_INLINE bool t3dReleaseOwner( T *&pOwner, T *pReleasingOwner ) {
	if ( !pReleasingOwner || pOwner != pReleasingOwner ) {
		return false;
	}
	pOwner = nullptr;
	return true;
}

#endif // TERRAIN3D_OWNER_H
