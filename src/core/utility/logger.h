// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_LOGGER_H
#define TERRAIN3D_LOGGER_H

#include "Terrain3DConstants.h"
#include "core/utility/logger.h"

#include <godot_cpp/variant/utility_functions.hpp>

/**
 * Prints warnings, errors, and messages to the console.
 * Regular messages are filtered based on the user specified debug level.
 * Warnings and errors always print except in release builds.
 * EXTREME is for continuously called prints like inside snapping.
 * See t3dDebugLevel_t and Terrain3D::debug_level.
 *
 * DEBUG logging is compiled out of non-DEBUG_ENABLED builds.
 */

#ifdef DEBUG_ENABLED
#define LOG( level, ... )                                                                                              \
	do {                                                                                                               \
		if ( level == ERROR ) {                                                                                        \
			UtilityFunctions::push_error( __class__, ":", __func__, ":", __LINE__, ": ", __VA_ARGS__ );                \
		} else if ( level == WARN ) {                                                                                  \
			UtilityFunctions::push_warning( __class__, ":", __func__, ":", __LINE__, ": ", __VA_ARGS__ );              \
		} else if ( level <= Terrain3DLogPolicy::getLevel() ) {                                                        \
			UtilityFunctions::print( __class__, ":", __func__, ":", __LINE__, ": ", __VA_ARGS__ );                     \
		}                                                                                                              \
	} while ( false ); // Macro safety
#else
#define LOG( ... )
#endif

#include "Terrain3DTypes.h"
#include "core/utility/terrain_3d_sync.h"

// Logging belongs to the process, independently of a scene owner's lifetime.
// The shared atomic also permits worker logging while the editor changes level.
class Terrain3DLogPolicy
{
private:
	static t3dAtomicU32_t &levelStorage(
		void ) {
		static t3dAtomicU32_t level{ ( uint32_t ) ERROR };
		return level;
	}

public:
	static t3dDebugLevel_t getLevel(
		void ) {
		return ( t3dDebugLevel_t ) levelStorage().load( T3D_MEMORY_ORDER_RELAXED );
	}
	static void setLevel(
		const t3dDebugLevel_t eLevel ) {
		const int nLevel = ( int ) eLevel;
		const int nClamped =
			nLevel < ( int ) ERROR ? ( int ) ERROR : ( nLevel > ( int ) EXTREME ? ( int ) EXTREME : nLevel );
		levelStorage().store( ( uint32_t ) nClamped, T3D_MEMORY_ORDER_RELAXED );
	}
};

#endif // TERRAIN3D_LOGGER_H
