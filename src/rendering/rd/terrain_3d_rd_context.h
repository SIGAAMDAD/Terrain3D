// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_CONTEXT_H
#define TERRAIN3D_RD_CONTEXT_H

#include "core/terrain_3d_compiler.h"

#include <atomic>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

class Terrain3DRdContext {
private:
	RenderingDevice *m_pRd = nullptr;
	std::atomic_uint64_t m_nCreatedResources{ 0u };
	std::atomic_uint64_t m_nFreedResources{ 0u };
	std::atomic_uint64_t m_nLiveBufferBytes{ 0u };
	std::atomic_uint64_t m_nPeakBufferBytes{ 0u };
	std::atomic_uint64_t m_nLiveTextureBytes{ 0u };
	std::atomic_uint64_t m_nPeakTextureBytes{ 0u };
	std::atomic_uint64_t m_nBufferUpdates{ 0u };
	std::atomic_uint64_t m_nBytesUploaded{ 0u };

	void updateBufferPeak( uint64_t nLiveBytes );
	void updateTexturePeak( uint64_t nLiveBytes );

public:
	Terrain3DRdContext( void ) = default;
	~Terrain3DRdContext( void );

	bool initialize( void );
	void shutdown( void );
	void registerResource( const RID &rRid, const String &rName, uint64_t nBufferBytes = 0u );
	void registerTextureResource( const RID &rRid, const String &rName, uint64_t nTextureBytes );
	void freeResource( RID &rRid, uint64_t nBufferBytes = 0u );
	void freeTextureResource( RID &rRid, uint64_t nTextureBytes );
	void recordBufferUpdate( uint64_t nBytes );
	Dictionary getDiagnostics( void ) const;

	EA_FORCE_INLINE RenderingDevice *getDevice( void ) const { return m_pRd; }
	EA_FORCE_INLINE bool isReady( void ) const { return m_pRd != nullptr; }
	EA_FORCE_INLINE uint64_t getLiveBufferBytes( void ) const {
		return m_nLiveBufferBytes.load( std::memory_order_acquire );
	}
};

#endif // TERRAIN3D_RD_CONTEXT_H
