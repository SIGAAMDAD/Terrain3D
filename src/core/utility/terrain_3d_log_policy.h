// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_LOG_POLICY_H
#define TERRAIN3D_LOG_POLICY_H

#include "Terrain3DTypes.h"
#include "core/utility/terrain_3d_sync.h"

// Logging belongs to the process, independently of a scene owner's lifetime.
// The shared atomic also permits worker logging while the editor changes level.
class Terrain3DLogPolicy {
private:
	static t3dAtomicU32_t &levelStorage( void ) {
		static t3dAtomicU32_t level{ (uint32_t)ERROR };
		return level;
	}
public:
	static t3dDebugLevel_t getLevel( void ) {
		return (t3dDebugLevel_t)levelStorage().load( T3D_MEMORY_ORDER_RELAXED );
	}
	static void setLevel( const t3dDebugLevel_t eLevel ) {
		const int nLevel = (int)eLevel;
		const int nClamped = nLevel < (int)ERROR ? (int)ERROR : ( nLevel > (int)EXTREME ? (int)EXTREME : nLevel );
		levelStorage().store( (uint32_t)nClamped, T3D_MEMORY_ORDER_RELAXED );
	}
};

#endif // TERRAIN3D_LOG_POLICY_H
