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
// persistent-upload Godot fork. Stock builds fall back to the existing upload paths.
bool t3dUploadBridgeIsAvailable( void );
bool t3dUploadBridgeUploadTextureLayer( RID hTexture, const Ref<Image> &rImage, int iLayer );
RID t3dUploadBridgeCreatePersistentStorageBuffer( uint32_t nSizeBytes );
Dictionary t3dUploadBridgeGetStats( void );

#endif // TERRAIN3D_UPLOAD_BRIDGE_H
