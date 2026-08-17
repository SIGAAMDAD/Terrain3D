// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_OWNER_H
#define TERRAIN3D_OWNER_H

namespace Terrain3DOwnership {

template <typename T>
inline T *transfer( T *&pOwner, T *pNextOwner ) {
	T *previous_owner = pOwner;
	pOwner = pNextOwner;
	return previous_owner;
}

template <typename T>
inline bool release( T *&pOwner, T *pReleasingOwner ) {
	if ( !pReleasingOwner || pOwner != pReleasingOwner ) {
		return false;
	}
	pOwner = nullptr;
	return true;
}

} // namespace Terrain3DOwnership

#endif // TERRAIN3D_OWNER_H
