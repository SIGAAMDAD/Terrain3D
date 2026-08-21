// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef GENERATEDTEXTURE_CLASS_H
#define GENERATEDTEXTURE_CLASS_H

#include <cstddef>

#include <godot_cpp/classes/image.hpp>

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"

class T3DGeneratedTexture {
	CLASS_NAME_STATIC( "Terrain3DGenTex" );

private:
	RID m_hRid = RID();
	Ref<Image> m_image;
	bool m_bDirty = false;
	int m_nSize = 0;
	int m_nWidth = 0;
	int m_nHeight = 0;
	int m_format = -1;
	bool m_bMipmaps = false;
	bool m_bLayered = false;
	size_t m_nPayloadBytes = 0u;

	bool matchesLayout( const Ref<Image> &rImage ) const;

public:
	void clear( void );
	void swap( T3DGeneratedTexture &rOther );
	EA_FORCE_INLINE bool isDirty( void ) const { return m_bDirty; }
	RID create( const TypedArray<Image> &rLayers );
	bool update( const Ref<Image> &rImage, const int nLayer );
	RID create( const Ref<Image> &rImage );
	bool update2d( const Ref<Image> &rImage );
	EA_FORCE_INLINE Ref<Image> getImage( void ) const { return m_image; }
	EA_FORCE_INLINE RID getRid( void ) const { return m_hRid; }
	EA_FORCE_INLINE int size( void ) const { return m_nSize; }
	int maxMipmapLevel( void ) const {
		if ( !m_bMipmaps || m_nWidth <= 0 || m_nHeight <= 0 ) {
			return 0;
		}
		int dimension = m_nWidth > m_nHeight ? m_nWidth : m_nHeight;
		int level = 0;
		while ( dimension > 1 ) {
			dimension >>= 1;
			++level;
		}
		return level;
	}
	EA_FORCE_INLINE size_t payloadBytes( void ) const { return m_nPayloadBytes; }
};

#endif // GENERATEDTEXTURE_CLASS_H
