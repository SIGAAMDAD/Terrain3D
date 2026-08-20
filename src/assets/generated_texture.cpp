// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <godot_cpp/classes/rendering_server.hpp>

#include <utility>

#include "assets/generated_texture.h"
#include "core/logger.h"
#include "terrain_3d.h"
#include "streaming/terrain_3d_upload_bridge.h"

///////////////////////////
// Private Functions
///////////////////////////

bool T3DGeneratedTexture::matchesLayout( const Ref<Image> &rImage ) const {
	return rImage.is_valid() && !rImage->is_empty() && rImage->get_width() == m_nWidth &&
		rImage->get_height() == m_nHeight && ( int ) ( rImage->get_format() ) == m_format &&
		rImage->has_mipmaps() == m_bMipmaps;
}

///////////////////////////
// Public Functions
///////////////////////////

void T3DGeneratedTexture::clear( void ) {
	if ( m_rid.is_valid() ) {
		LOG( Terrain3D::EXTREME, "T3DGeneratedTexture freeing ", m_rid );
		RS->free_rid( m_rid );
	}
	if ( m_pImage.is_valid() ) {
		LOG( Terrain3D::EXTREME, "T3DGeneratedTexture unref image", m_pImage );
		m_pImage.unref();
	}
	m_rid = RID();
	m_bDirty = true;
	m_nSize = 0;
	m_nWidth = 0;
	m_nHeight = 0;
	m_format = -1;
	m_bMipmaps = false;
	m_bLayered = false;
	m_nPayloadBytes = 0u;
}

void T3DGeneratedTexture::swap( T3DGeneratedTexture &rOther ) {
	using std::swap;
	swap( m_rid, rOther.m_rid );
	swap( m_pImage, rOther.m_pImage );
	swap( m_bDirty, rOther.m_bDirty );
	swap( m_nSize, rOther.m_nSize );
	swap( m_nWidth, rOther.m_nWidth );
	swap( m_nHeight, rOther.m_nHeight );
	swap( m_format, rOther.m_format );
	swap( m_bMipmaps, rOther.m_bMipmaps );
	swap( m_bLayered, rOther.m_bLayered );
	swap( m_nPayloadBytes, rOther.m_nPayloadBytes );
}

RID T3DGeneratedTexture::create( const TypedArray<Image> &rLayers ) {
	if ( rLayers.is_empty() ) {
		clear();
		return RID();
	}
	const Ref<Image> pFirst = rLayers[0];
	if ( pFirst.is_null() || pFirst->is_empty() ) {
		LOG( Terrain3D::ERROR, "Cannot create Terrain3D texture array from an empty first layer" );
		return RID();
	}
	const int width = pFirst->get_width();
	const int height = pFirst->get_height();
	const int format = ( int ) ( pFirst->get_format() );
	const bool bMipmaps = pFirst->has_mipmaps();
	size_t payload_bytes = ( size_t ) ( pFirst->get_data().size() );
	for ( int layer = 1; layer < rLayers.size(); ++layer ) {
		const Ref<Image> pImage = rLayers[layer];
		if ( pImage.is_null() || pImage->is_empty() || pImage->get_width() != width ||
			pImage->get_height() != height || ( int ) ( pImage->get_format() ) != format ||
			pImage->has_mipmaps() != bMipmaps ) {
			LOG( Terrain3D::ERROR, "Rejected incompatible Terrain3D texture-array layer ", layer );
			return RID();
		}
		payload_bytes += ( size_t ) ( pImage->get_data().size() );
	}
	if ( Terrain3D::s_debugLevel >= Terrain3D::DEBUG ) {
		LOG( Terrain3D::EXTREME, "RenderingServer creating Texture2DArray, layers size: ", rLayers.size() );
		for ( int i = 0; i < rLayers.size(); i++ ) {
			const Ref<Image> pImg = rLayers[i];
			if ( pImg.is_null() ) {
				LOG( Terrain3D::EXTREME, i, ": null image" );
				continue;
			}
			LOG( Terrain3D::EXTREME, i, ": ", pImg, ", empty: ", pImg->is_empty(), ", size: ", pImg->get_size(),
				", format: ", pImg->get_format() );
		}
	}
	LOG( Terrain3D::DEBUG, "[render-resource] Creating Texture2DArray replacement: layers=", rLayers.size(),
		"; size=", width, "x", height, "; format=", format, "; mipmaps=", bMipmaps,
		"; replacing_old_rid=", m_rid.is_valid() );
	const RID new_rid = RS->texture_2d_layered_create( rLayers, RenderingServer::TEXTURE_LAYERED_2D_ARRAY );
	if ( !new_rid.is_valid() ) {
		m_bDirty = true;
		m_nSize = 0;
		LOG( Terrain3D::ERROR, "RenderingServer rejected Terrain3D texture array with ", rLayers.size(),
			" layers. The active world exceeds the renderer's texture-array capacity or contains incompatible layers." );
		return RID();
	}
	if ( m_rid.is_valid() ) {
		RS->free_rid( m_rid );
	}
	m_pImage.unref();
	m_rid = new_rid;
	m_bDirty = false;
	m_nSize = rLayers.size();
	m_nWidth = width;
	m_nHeight = height;
	m_format = format;
	m_bMipmaps = bMipmaps;
	m_bLayered = true;
	m_nPayloadBytes = payload_bytes;
	return m_rid;
}

bool T3DGeneratedTexture::update( const Ref<Image> &rImage, const int nLayer ) {
	if ( !m_rid.is_valid() || m_bDirty || !m_bLayered || nLayer < 0 || nLayer >= m_nSize ||
		!matchesLayout( rImage ) ) {
		LOG( Terrain3D::ERROR, "Rejected Texture2DArray update. RID valid: ", m_rid.is_valid(), ", dirty: ", m_bDirty,
			", layered: ", m_bLayered, ", layer: ", nLayer, ", layer count: ", m_nSize,
			", compatible image: ", matchesLayout( rImage ) );
		return false;
	}
	LOG( Terrain3D::EXTREME, "RenderingServer updating Texture2DArray at index: ", nLayer );
	if ( !Terrain3DUploadBridge::uploadTextureLayer( m_rid, rImage, nLayer ) ) {
		RS->texture_2d_update( m_rid, rImage, nLayer );
	}
	return true;
}

RID T3DGeneratedTexture::create( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		m_bDirty = true;
		LOG( Terrain3D::ERROR, "Cannot create generated Texture2D from an empty image" );
		return RID();
	}
	LOG( Terrain3D::EXTREME, "RenderingServer creating Texture2D" );
	LOG( Terrain3D::DEBUG, "[render-resource] Creating Texture2D replacement: size=", rImage->get_width(), "x",
		rImage->get_height(), "; format=", ( int ) ( rImage->get_format() ),
		"; replacing_old_rid=", m_rid.is_valid() );
	const RID new_rid = RS->texture_2d_create( rImage );
	if ( !new_rid.is_valid() ) {
		m_bDirty = true;
		LOG( Terrain3D::ERROR, "RenderingServer rejected generated Texture2D" );
		return RID();
	}
	if ( m_rid.is_valid() ) {
		RS->free_rid( m_rid );
	}
	m_pImage = rImage;
	m_rid = new_rid;
	m_bDirty = false;
	m_nSize = 1;
	m_nWidth = rImage->get_width();
	m_nHeight = rImage->get_height();
	m_format = ( int ) ( rImage->get_format() );
	m_bMipmaps = rImage->has_mipmaps();
	m_bLayered = false;
	m_nPayloadBytes = ( size_t ) ( rImage->get_data().size() );
	return m_rid;
}

bool T3DGeneratedTexture::update2d( const Ref<Image> &rImage ) {
	if ( !m_rid.is_valid() || m_bDirty || m_bLayered || m_nSize != 1 || !matchesLayout( rImage ) ) {
		LOG( Terrain3D::ERROR, "Rejected Texture2D update. RID valid: ", m_rid.is_valid(), ", dirty: ", m_bDirty,
			", layered: ", m_bLayered, ", compatible image: ", matchesLayout( rImage ),
			", size: ", m_nSize );
		return false;
	}
	LOG( Terrain3D::EXTREME, "RenderingServer updating Texture2D" );
	if ( !Terrain3DUploadBridge::uploadTextureLayer( m_rid, rImage, 0 ) ) {
		RS->texture_2d_update( m_rid, rImage, 0 );
	}
	m_pImage = rImage;
	return true;
}
