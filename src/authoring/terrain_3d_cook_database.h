// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_COOK_DATABASE_H
#define TERRAIN3D_COOK_DATABASE_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <array>
#include <cstdint>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

class Terrain3DCookDatabase {
public:
	void reset( void );
	bool load( const String &rDataDirectory, int nRegionSize, uint64_t nMaterialSignature );
	bool write( const String &rDataDirectory, int nRegionSize, uint64_t nMaterialSignature ) const;
	bool hasCurrentRegion( const Vector2i &rLocation, uint64_t nSourceSignature ) const;
	void setRegion( const Vector2i &rLocation, uint64_t nSourceSignature );
	void clearRegion( const Vector2i &rLocation );
	String getPath( const String &rDataDirectory ) const;
	EA_FORCE_INLINE bool isLoaded( void ) const {
		return m_bLoaded;
	}

private:
	bool m_bLoaded = false;
	std::array<uint64_t, s_nMaxRegionSlots> m_regionSignatures{};
};

#endif // TERRAIN3D_COOK_DATABASE_H
