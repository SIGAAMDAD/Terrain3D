// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef GENERATEDTEXTURE_CLASS_H
#define GENERATEDTEXTURE_CLASS_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"
#include "rendering/rd/terrain_3d_rd_texture_manager.h"

#include <cstddef>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/rect2i.hpp>

class Terrain3DGeneratedTexture
{
	CLASS_NAME_STATIC( "Terrain3DGenTex" );

private:
	RID m_hRid = RID();
	RID m_hRegionStagingRid = RID();
	Ref<Image> m_image;
	TypedArray<Image> m_layers;
	bool m_bDirty = false;
	int m_nSize = 0;
	int m_nWidth = 0;
	int m_nHeight = 0;
	int m_nFormat = -1;
	int m_nRegionStagingWidth = 0;
	int m_nRegionStagingHeight = 0;
	int m_nRegionStagingFormat = -1;
	bool m_bMipmaps = false;
	bool m_bLayered = false;
	size_t m_nPayloadBytes = 0u;
	size_t m_nRegionStagingBytes = 0u;
	t3dRdTextureSemantic_t m_semantic = T3D_RD_TEXTURE_LINEAR_DATA;

	bool matchesLayout( const Ref<Image>& rImage ) const;
	bool ensureRegionStaging( const Ref<Image>& rImage );
	String getResourceName( const String& rSuffix = String() ) const;

public:
	void clear( void );
	void swap( Terrain3DGeneratedTexture& rOther );
	EA_FORCE_INLINE bool isDirty( void ) const
	{
		return m_bDirty;
	}
	RID create( const TypedArray<Image>& rLayers, t3dRdTextureSemantic_t semantic );
	bool update( const Ref<Image>& rImage, int nLayer );
	EA_FORCE_INLINE bool canUpdateLayer( const Ref<Image> &rImage, const int nLayer ) const {
		return m_hRid.is_valid() && !m_bDirty && m_bLayered && nLayer >= 0 && nLayer < m_nSize &&
				matchesLayout( rImage );
	}
	EA_FORCE_INLINE void commitLayerImage( const Ref<Image> &rImage, const int nLayer ) {
		if ( canUpdateLayer( rImage, nLayer ) ) {
			m_layers[nLayer] = rImage;
		}
	}
	RID create( const Ref<Image>& rImage, t3dRdTextureSemantic_t semantic );
	bool update2d( const Ref<Image>& rImage );
	bool updateRegion2d( const Ref<Image>& rImage, const Rect2i& rDestination );
	EA_FORCE_INLINE Ref<Image> getImage( void ) const
	{
		return m_image;
	}
	EA_FORCE_INLINE Ref<Image> getLayerImage( const int nLayer ) const
	{
		if ( !m_bLayered || nLayer < 0 || nLayer >= m_layers.size() ) {
			return Ref<Image>();
		}
		return m_layers[nLayer];
	}
	EA_FORCE_INLINE RID getRid( void ) const
	{
		return m_hRid;
	}
	EA_FORCE_INLINE int size( void ) const
	{
		return m_nSize;
	}
	int maxMipmapLevel( void ) const
	{
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
	EA_FORCE_INLINE size_t payloadBytes( void ) const
	{
		return m_nPayloadBytes;
	}
};

#endif // GENERATEDTEXTURE_CLASS_H
