// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_UPLOAD_BRIDGE_H
#define TERRAIN3D_UPLOAD_BRIDGE_H

#include <cstdint>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Optional bridge to the TerrainUploadBridge engine module shipped with the
// persistent-upload Godot fork. Stock Godot builds simply fall back to the
// existing RenderingServer/RenderingDevice upload paths.
namespace Terrain3DUploadBridge {

bool isAvailable( void );
bool uploadTextureLayer( RID texture, const Ref<Image> &rImage, int nLayer );
RID createPersistentStorageBuffer( uint32_t nSizeBytes );
Dictionary getStats( void );

} // namespace Terrain3DUploadBridge

#endif // TERRAIN3D_UPLOAD_BRIDGE_H
