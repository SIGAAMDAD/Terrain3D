// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef LOGGER_CLASS_H
#define LOGGER_CLASS_H

#include <godot_cpp/variant/utility_functions.hpp>

#include "Terrain3DConstants.h"
#include "terrain_3d.h"

/**
 * Prints warnings, errors, and messages to the console.
 * Regular messages are filtered based on the user specified debug level.
 * Warnings and errors always print except in release builds.
 * EXTREME is for continuously called prints like inside snapping.
 * See Terrain3D::t3dDebugLevel_t and Terrain3D::debug_level.
 *
 * DEBUG logging is compiled out of non-DEBUG_ENABLED builds.
 */

#ifdef DEBUG_ENABLED
#define LOG( level, ... )                                                                                 \
	do {                                                                                                \
		if ( level == T3DTerrain::ERROR ) {                                                                           \
			UtilityFunctions::push_error( __class__, ":", __func__, ":", __LINE__, ": ", __VA_ARGS__ );   \
		} else if ( level == T3DTerrain::WARN ) {                                                                     \
			UtilityFunctions::push_warning( __class__, ":", __func__, ":", __LINE__, ": ", __VA_ARGS__ ); \
		} else if ( level <= T3DTerrain::s_debugLevel ) {                                                   \
			UtilityFunctions::print( __class__, ":", __func__, ":", __LINE__, ": ", __VA_ARGS__ );        \
		}                                                                                               \
	} while ( false ); // Macro safety
#else
#define LOG( ... )
#endif

#endif // LOGGER_CLASS_H
