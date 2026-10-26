// Copyright ? 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_DATA_ACCESS_H
#define TERRAIN3D_DATA_ACCESS_H
#include "assets/catalog/terrain_3d_assets.h"
#include "data/model/terrain_3d_region.h"
#include "rendering/material/terrain_3d_material.h"
#include <godot_cpp/variant/typed_array.hpp>

// Erosion captures authoring inputs through this contract. It cannot inspect
// Data's scene owner, runtime residency state, or private storage layout.
class Terrain3DRegionSource {
protected:
	~Terrain3DRegionSource( void ) = default;
public:
	virtual int getRegionCount( void ) const = 0;
	virtual bool hasRegion( const godot::Vector2i &rLocation ) const = 0;
	virtual godot::TypedArray<godot::Vector2i> getRegionLocations( void ) const = 0;
	virtual godot::Ref<Terrain3DRegion> getRegion( const godot::Vector2i &rRegionLoc ) const = 0;
	virtual bool ensureCpuRegionResident( const godot::Vector2i &vLocation ) = 0;
};

class Terrain3DDataAccess : public Terrain3DRegionSource {
protected:
	~Terrain3DDataAccess( void ) = default;
public:
	virtual godot::Ref<Terrain3DAssets> getErosionAssets( void ) const = 0;
	virtual godot::Ref<Terrain3DMaterial> getErosionMaterial( void ) const = 0;
};
#endif // TERRAIN3D_DATA_ACCESS_H
