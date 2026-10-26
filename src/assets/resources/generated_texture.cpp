// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#include "assets/resources/generated_texture.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/utility/logger.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "terrain_3d.h"
#include <EASTL/utility.h>

static t3dAtomicU64_t s_nGeneratedTextureId{ 1u };
static Terrain3DRdTextureManager *t3dGeneratedTextureManager( void ) {
	Terrain3DRdTextureManager *pManager = Terrain3DRdTextureManager::getSingleton();
	if ( !pManager ) { Terrain3DRdTextureManager::initializeSingleton(); pManager = Terrain3DRdTextureManager::getSingleton(); }
	return pManager;
}
static Ref<Image> t3dGeneratedTextureCanonicalizeUploadImage( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() || rImage->get_format() != Image::FORMAT_RGB8 ) { return rImage; }
	Ref<Image> pImage = Ref<Image>( rImage->duplicate() );
	if ( pImage.is_null() ) { return Ref<Image>(); }
	pImage->convert( Image::FORMAT_RGBA8 );
	return pImage->get_format() == Image::FORMAT_RGBA8 && pImage->get_width() == rImage->get_width()
	        && pImage->get_height() == rImage->get_height() && pImage->has_mipmaps() == rImage->has_mipmaps() ? pImage : Ref<Image>();
}

bool Terrain3DGeneratedTexture::matchesLayout( const Ref<Image> &rImage ) const {
	if ( !m_texture.isValid() || rImage.is_null() || rImage->is_empty() ) { return false; }
	const t3dRdTextureLayout_t &layout = m_texture.layout();
	return ( uint32_t )rImage->get_width() == layout.m_nWidth && ( uint32_t )rImage->get_height() == layout.m_nHeight
	        && ( int )rImage->get_format() == m_nFormat && Terrain3DRdTextureManager::getImageMipmapCount( rImage ) == layout.m_nMipmaps;
}
String Terrain3DGeneratedTexture::getResourceName( const String &rSuffix ) const {
	return String( "T3D.GeneratedTexture." ) + String::num_uint64( s_nGeneratedTextureId.fetch_add( 1u, T3D_MEMORY_ORDER_RELAXED ) ) + rSuffix;
}
bool Terrain3DGeneratedTexture::ensureRegionStaging( const Ref<Image> &rImage ) {
	const Ref<Image> pImage = t3dGeneratedTextureCanonicalizeUploadImage( rImage );
	if ( pImage.is_null() || pImage->is_empty() || pImage->has_mipmaps() ) { return false; }
	const PackedByteArray data = pImage->get_data();
	t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildLayout( pImage, 1u, false, ( uint64_t )data.size(), m_semantic, layout ) ) { return false; }
	if ( m_regionStaging.isValid() && m_regionStaging.layout().m_nWidth == layout.m_nWidth
	        && m_regionStaging.layout().m_nHeight == layout.m_nHeight && m_regionStaging.layout().m_format == layout.m_format ) return true;
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	if ( !pManager ) { return false; }
	pManager->freeTexture( m_regionStaging );
	TypedArray<PackedByteArray> initialData; initialData.push_back( data );
	return pManager->createTexture( m_regionStaging, layout, initialData, getResourceName( ".RegionStaging" ), T3D_RD_CONTEXT_MEMORY_SCRATCH );
}

void Terrain3DGeneratedTexture::clear( void ) {
	Terrain3DRdTextureManager *const pManager = Terrain3DRdTextureManager::getSingleton();
	if ( pManager ) { pManager->freeTexture( m_texture ); pManager->freeTexture( m_regionStaging ); }
	else { m_texture.abandon(); m_regionStaging.abandon(); }
	m_image.unref(); m_layers.clear(); m_bDirty = true; m_nFormat = -1;
}
void Terrain3DGeneratedTexture::swap( Terrain3DGeneratedTexture &rOther ) {
	using eastl::swap;
	m_texture.swap( rOther.m_texture ); m_regionStaging.swap( rOther.m_regionStaging );
	swap( m_image, rOther.m_image ); swap( m_layers, rOther.m_layers ); swap( m_bDirty, rOther.m_bDirty );
	swap( m_nFormat, rOther.m_nFormat ); swap( m_semantic, rOther.m_semantic ); swap( m_sourceRetentionPolicy, rOther.m_sourceRetentionPolicy );
}
void Terrain3DGeneratedTexture::releaseCpuSources( void ) {
	m_image.unref(); for ( int layer = 0; layer < m_layers.size(); ++layer ) m_layers[layer] = Ref<Image>();
}

RID Terrain3DGeneratedTexture::create( const TypedArray<Image> &rLayers, const t3dRdTextureSemantic_t semantic ) {
	if ( rLayers.is_empty() ) { clear(); return RID(); }
	TypedArray<Image> layers; layers.resize( rLayers.size() );
	TypedArray<PackedByteArray> data; data.resize( rLayers.size() );
	uint64_t bytes = 0u; Ref<Image> first;
	for ( int i = 0; i < rLayers.size(); ++i ) {
		const Ref<Image> image = t3dGeneratedTextureCanonicalizeUploadImage( rLayers[i] );
		if ( image.is_null() || image->is_empty() ) { return RID(); }
		if ( i == 0 ) first = image;
		else if ( image->get_width() != first->get_width() || image->get_height() != first->get_height()
		        || image->get_format() != first->get_format() || image->has_mipmaps() != first->has_mipmaps() ) return RID();
		layers[i] = image; const PackedByteArray layerData = image->get_data(); data[i] = layerData; bytes += ( uint64_t )layerData.size();
	}
	t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildLayout( first, ( uint32_t )layers.size(), true, bytes, semantic, layout ) ) { return RID(); }
	Terrain3DRdTextureManager *const manager = t3dGeneratedTextureManager(); Terrain3DRdTexture replacement;
	if ( !manager || !manager->createTexture( replacement, layout, data, getResourceName( ".Array" ) ) ) { m_bDirty = true; return RID(); }
	manager->freeTexture( m_texture ); manager->freeTexture( m_regionStaging ); m_texture.swap( replacement );
	m_image.unref(); m_layers = layers.duplicate(); m_bDirty = false; m_nFormat = ( int )first->get_format(); m_semantic = semantic;
	if ( m_sourceRetentionPolicy == T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD ) releaseCpuSources();
	return m_texture.rid();
}
bool Terrain3DGeneratedTexture::update( const Ref<Image> &rImage, const int nLayer ) {
	const Ref<Image> image = t3dGeneratedTextureCanonicalizeUploadImage( rImage );
	if ( !canUpdateLayer( image, nLayer ) ) { return false; }
	Terrain3DRdTextureManager *const manager = t3dGeneratedTextureManager();
	if ( !manager || !manager->updateTexture( m_texture, ( uint32_t )nLayer, image->get_data() ) ) { return false; }
	if ( m_sourceRetentionPolicy == T3D_RETAIN_CPU_SOURCE ) m_layers[nLayer] = image;
	return true;
}
bool Terrain3DGeneratedTexture::buildTrackedLayerBatchUpdate(
		const Ref<Image> &rImage, const int nLayer, t3dRdTextureBatchUpdate_t &rUpdate ) {
	rUpdate = t3dRdTextureBatchUpdate_t{};
	if ( !canUpdateLayer( rImage, nLayer ) ) { return false; }
	rUpdate.m_pTexture = &m_texture;
	rUpdate.m_nTextureGeneration = m_texture.generation();
	rUpdate.m_data = rImage->get_data();
	if ( rUpdate.m_data.is_empty() ) {
		rUpdate = t3dRdTextureBatchUpdate_t{};
		return false;
	}
	return true;
}
RID Terrain3DGeneratedTexture::create( const Ref<Image> &rImage, const t3dRdTextureSemantic_t semantic ) {
	const Ref<Image> image = t3dGeneratedTextureCanonicalizeUploadImage( rImage );
	if ( image.is_null() || image->is_empty() ) { m_bDirty = true; return RID(); }
	const PackedByteArray bytes = image->get_data(); t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildLayout( image, 1u, false, ( uint64_t )bytes.size(), semantic, layout ) ) { m_bDirty = true; return RID(); }
	TypedArray<PackedByteArray> initialData; initialData.push_back( bytes );
	Terrain3DRdTextureManager *const manager = t3dGeneratedTextureManager(); Terrain3DRdTexture replacement;
	if ( !manager || !manager->createTexture( replacement, layout, initialData, getResourceName( ".2D" ) ) ) { m_bDirty = true; return RID(); }
	manager->freeTexture( m_texture ); manager->freeTexture( m_regionStaging ); m_texture.swap( replacement );
	m_image = image; m_layers.clear(); m_bDirty = false; m_nFormat = ( int )image->get_format(); m_semantic = semantic;
	if ( m_sourceRetentionPolicy == T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD ) releaseCpuSources();
	return m_texture.rid();
}
bool Terrain3DGeneratedTexture::update2d( const Ref<Image> &rImage ) {
	const Ref<Image> image = t3dGeneratedTextureCanonicalizeUploadImage( rImage );
	if ( !m_texture.isValid() || m_bDirty || isLayered() || m_texture.layout().m_nLayers != 1u ) { return false; }
	if ( !matchesLayout( image ) ) { return create( image, m_semantic ).is_valid(); }
	Terrain3DRdTextureManager *const manager = t3dGeneratedTextureManager();
	if ( !manager || !manager->updateTexture( m_texture, 0u, image->get_data() ) ) { return false; }
	if ( m_sourceRetentionPolicy == T3D_RETAIN_CPU_SOURCE ) m_image = image;
	return true;
}
bool Terrain3DGeneratedTexture::updateRegion2d( const Ref<Image> &rImage, const Rect2i &rDestination ) {
	const Ref<Image> image = t3dGeneratedTextureCanonicalizeUploadImage( rImage );
	if ( !m_texture.isValid() || m_bDirty || isLayered() || m_texture.layout().m_nMipmaps != 1u || image.is_null() || image->is_empty()
	        || image->has_mipmaps() || ( int )image->get_format() != m_nFormat || rDestination.position.x < 0 || rDestination.position.y < 0
	        || rDestination.size.x <= 0 || rDestination.size.y <= 0 || rDestination.size.x != image->get_width() || rDestination.size.y != image->get_height()
	        || rDestination.get_end().x > ( int )m_texture.layout().m_nWidth || rDestination.get_end().y > ( int )m_texture.layout().m_nHeight ) return false;
	if ( !ensureRegionStaging( image ) ) { return false; }
	Terrain3DRdTextureManager *const manager = t3dGeneratedTextureManager();
	return manager && manager->updateTextureRegion( m_texture, m_regionStaging, ( uint32_t )rDestination.position.x,
	        ( uint32_t )rDestination.position.y, ( uint32_t )rDestination.size.x, ( uint32_t )rDestination.size.y, image->get_data() );
}
