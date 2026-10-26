// Copyright (c) 2023-2026 Terrain3D contributors.
#ifndef TERRAIN3D_VT_DEMAND_COOKER_H
#define TERRAIN3D_VT_DEMAND_COOKER_H

#include "Terrain3DTypes.h"
#include <godot_cpp/variant/packed_byte_array.hpp>

class Terrain3DData;
class Terrain3DAssets;

// Owner-thread offline cook. The final (post-MegaVT merge) V3 package is the
// source of stable keys and its content signature. No runtime scene traversal.
bool t3d_vt_demand_cook_asset_sidecar_cold( const Terrain3DData &rData,
    const godot::Ref< Terrain3DAssets > &rAssets,
    const godot::PackedByteArray &rFinalPackage,
    godot::PackedByteArray &rSidecar, godot::String &rPath,
    godot::String &rError,
    godot::PackedByteArray *pV3Sidecar = nullptr,
    godot::String *pV3Path = nullptr );

// Publish two independently validated files as one rollback-capable cook action.
// The package may stay in terrain's data directory; the demand sidecar follows the asset.
bool t3d_vt_demand_publish_pair_cold( const godot::String		   &rPackagePath,
									  const godot::PackedByteArray &rPackage,
									  const godot::String		   &rDemandPath,
									  const godot::PackedByteArray &rDemand,
									  godot::String				   &rError );
// The third artifact is optional. v2 is always published at its canonical
// path, so shipping runtimes can continue using their existing reader.
bool t3d_vt_demand_publish_triplet_cold( const godot::String &rPackagePath,
    const godot::PackedByteArray &rPackage,
    const godot::String &rDemandPath, const godot::PackedByteArray &rDemand,
    const godot::String &rV3Path, const godot::PackedByteArray &rV3,
    godot::String &rError );
#endif
