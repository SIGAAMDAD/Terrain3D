// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_GENERATED_TEXTURE_H
#define TERRAIN3D_GENERATED_TEXTURE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/rect2i.hpp>

// t3dGeneratedTextureSourceRetentionPolicy_e centralized in Terrain3DTypes.h.


class Terrain3DGeneratedTexture {
	CLASS_NAME_STATIC( "Terrain3DGenTex" );
private:
	Terrain3DRdTexture m_texture;
	Terrain3DRdTexture m_regionStaging;
	Ref<Image> m_image;
	TypedArray<Image> m_layers;
	bool m_bDirty = false;
	int m_nFormat = -1;
	t3dRdTextureSemantic_t m_semantic = T3D_RD_TEXTURE_LINEAR_DATA;
	t3dGeneratedTextureSourceRetentionPolicy_t m_sourceRetentionPolicy = T3D_RETAIN_CPU_SOURCE;

	bool matchesLayout( const Ref<Image> &rImage ) const;
	bool ensureRegionStaging( const Ref<Image> &rImage );
	String getResourceName( const String &rSuffix = String() ) const;
	EA_FORCE_INLINE bool isLayered( void ) const { return m_texture.isValid() && m_texture.layout().m_type == RenderingDevice::TEXTURE_TYPE_2D_ARRAY; }
public:
	EA_FORCE_INLINE void setSourceRetentionPolicy( t3dGeneratedTextureSourceRetentionPolicy_t policy ) { m_sourceRetentionPolicy = policy; }
	void releaseCpuSources( void );
	void clear( void );
	void swap( Terrain3DGeneratedTexture &rOther );
	EA_FORCE_INLINE bool isDirty( void ) const { return m_bDirty; }
	RID create( const TypedArray<Image> &rLayers, t3dRdTextureSemantic_t semantic );
	bool update( const Ref<Image> &rImage, int nLayer );
	EA_FORCE_INLINE bool canUpdateLayer( const Ref<Image> &rImage, const int nLayer ) const {
		return m_texture.isValid() && !m_bDirty && isLayered() && nLayer >= 0 && nLayer < ( int )m_texture.layout().m_nLayers && matchesLayout( rImage );
	}
	bool buildTrackedLayerBatchUpdate( const Ref<Image> &rImage, int nLayer, t3dRdTextureBatchUpdate_t &rUpdate );
	EA_FORCE_INLINE void commitLayerImage( const Ref<Image> &rImage, const int nLayer ) {
		if ( m_sourceRetentionPolicy == T3D_RETAIN_CPU_SOURCE && canUpdateLayer( rImage, nLayer ) ) m_layers[nLayer] = rImage;
	}
	RID create( const Ref<Image> &rImage, t3dRdTextureSemantic_t semantic );
	bool update2d( const Ref<Image> &rImage );
	bool updateRegion2d( const Ref<Image> &rImage, const Rect2i &rDestination );
	EA_FORCE_INLINE Ref<Image> getImage( void ) const { return m_image; }
	EA_FORCE_INLINE Ref<Image> getLayerImage( const int nLayer ) const {
		return !isLayered() || nLayer < 0 || nLayer >= m_layers.size() ? Ref<Image>() : Ref<Image>( m_layers[nLayer] );
	}
	EA_FORCE_INLINE RID getRid( void ) const { return m_texture.rid(); }
	EA_FORCE_INLINE int size( void ) const { return m_texture.isValid() ? ( int )m_texture.layout().m_nLayers : 0; }
	EA_FORCE_INLINE int maxMipmapLevel( void ) const { return m_texture.isValid() && m_texture.layout().m_nMipmaps > 0u ? ( int )m_texture.layout().m_nMipmaps - 1 : 0; }
	EA_FORCE_INLINE size_t payloadBytes( void ) const { return ( size_t )m_texture.bytes(); }
};
#endif // TERRAIN3D_GENERATED_TEXTURE_H
