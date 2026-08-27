// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/generated_texture.h"

#include "core/logger.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_rd_texture_manager.h"
#include "terrain_3d.h"

#include <atomic>
#include <utility>

static std::atomic_uint64_t s_nGeneratedTextureId{ 1u };

static Terrain3DRdTextureManager *t3dGeneratedTextureManager( void )
{
	Terrain3DRdTextureManager *pManager = Terrain3DRdTextureManager::getSingleton();
	if ( !pManager ) {
		Terrain3DRdTextureManager::initializeSingleton();
		pManager = Terrain3DRdTextureManager::getSingleton();
	}
	return pManager;
}

// -----------------------------------------------------------------------------
// Private functions

bool Terrain3DGeneratedTexture::matchesLayout( const Ref<Image>& rImage ) const
{
	return rImage.is_valid() && !rImage->is_empty() && rImage->get_width() == m_nWidth &&
			rImage->get_height() == m_nHeight && (int)( rImage->get_format() ) == m_nFormat &&
			rImage->has_mipmaps() == m_bMipmaps;
}

String Terrain3DGeneratedTexture::getResourceName( const String& rSuffix ) const
{
	// Names are diagnostics only; the RID registry remains the ownership source of truth.
	const uint64_t id = s_nGeneratedTextureId.fetch_add( 1u, std::memory_order_relaxed );
	return String( "T3D.GeneratedTexture." ) + String::num_uint64( id ) + rSuffix;
}

bool Terrain3DGeneratedTexture::ensureRegionStaging( const Ref<Image>& rImage )
{
	if ( rImage.is_null() || rImage->is_empty() || rImage->has_mipmaps() ) {
		return false;
	}
	if ( m_hRegionStagingRid.is_valid() && m_nRegionStagingWidth == rImage->get_width() &&
			m_nRegionStagingHeight == rImage->get_height() &&
			m_nRegionStagingFormat == (int)( rImage->get_format() ) ) {
		return true;
	}
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	if ( !pManager ) {
		return false;
	}
	if ( m_hRegionStagingRid.is_valid() ) {
		pManager->freeResource( m_hRegionStagingRid, m_nRegionStagingBytes );
		m_hRegionStagingRid = RID();
		m_nRegionStagingBytes = 0u;
	}
	const PackedByteArray data = rImage->get_data();
	t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildLayout( rImage, 1u, false, (uint64_t)( data.size() ), m_semantic, layout ) ) {
		return false;
	}
	TypedArray<PackedByteArray> initial_data;
	initial_data.push_back( data );
	m_hRegionStagingRid = pManager->createTexture(
			layout, initial_data, getResourceName( ".RegionStaging" ), Terrain3DRdContext::MEMORY_SCRATCH );
	if ( !m_hRegionStagingRid.is_valid() ) {
		return false;
	}
	m_nRegionStagingWidth = rImage->get_width();
	m_nRegionStagingHeight = rImage->get_height();
	m_nRegionStagingFormat = (int)( rImage->get_format() );
	m_nRegionStagingBytes = (size_t)( data.size() );
	return true;
}

// -----------------------------------------------------------------------------
// Public functions

void Terrain3DGeneratedTexture::clear( void )
{
	// During module teardown the manager is deliberately destroyed before the
	// shared RD context. Do not resurrect it from a late owner clear; the context
	// remains the deterministic fallback owner for any still-registered RID.
	Terrain3DRdTextureManager *const pManager = Terrain3DRdTextureManager::getSingleton();
	if ( pManager && m_hRid.is_valid() ) {
		LOG( EXTREME, "Terrain3DGeneratedTexture freeing direct RD texture ", m_hRid );
		pManager->freeTexture( m_hRid, m_nPayloadBytes );
	}
	if ( pManager && m_hRegionStagingRid.is_valid() ) {
		pManager->freeResource( m_hRegionStagingRid, m_nRegionStagingBytes );
	}
	if ( m_image.is_valid() ) {
		LOG( EXTREME, "Terrain3DGeneratedTexture unref image", m_image );
		m_image.unref();
	}
	m_layers.clear();
	m_hRid = RID();
	m_hRegionStagingRid = RID();
	m_bDirty = true;
	m_nSize = 0;
	m_nWidth = 0;
	m_nHeight = 0;
	m_nFormat = -1;
	m_nRegionStagingWidth = 0;
	m_nRegionStagingHeight = 0;
	m_nRegionStagingFormat = -1;
	m_bMipmaps = false;
	m_bLayered = false;
	m_nPayloadBytes = 0u;
	m_nRegionStagingBytes = 0u;
}

void Terrain3DGeneratedTexture::swap( Terrain3DGeneratedTexture& rOther )
{
	using std::swap;
	swap( m_hRid, rOther.m_hRid );
	swap( m_hRegionStagingRid, rOther.m_hRegionStagingRid );
	swap( m_image, rOther.m_image );
	swap( m_layers, rOther.m_layers );
	swap( m_bDirty, rOther.m_bDirty );
	swap( m_nSize, rOther.m_nSize );
	swap( m_nWidth, rOther.m_nWidth );
	swap( m_nHeight, rOther.m_nHeight );
	swap( m_nFormat, rOther.m_nFormat );
	swap( m_nRegionStagingWidth, rOther.m_nRegionStagingWidth );
	swap( m_nRegionStagingHeight, rOther.m_nRegionStagingHeight );
	swap( m_nRegionStagingFormat, rOther.m_nRegionStagingFormat );
	swap( m_bMipmaps, rOther.m_bMipmaps );
	swap( m_bLayered, rOther.m_bLayered );
	swap( m_nPayloadBytes, rOther.m_nPayloadBytes );
	swap( m_nRegionStagingBytes, rOther.m_nRegionStagingBytes );
	swap( m_semantic, rOther.m_semantic );
}

RID Terrain3DGeneratedTexture::create( const TypedArray<Image>& rLayers, const t3dRdTextureSemantic_t semantic )
{
	if ( rLayers.is_empty() ) {
		clear();
		return RID();
	}
	const Ref<Image> pFirst = rLayers[0];
	if ( pFirst.is_null() || pFirst->is_empty() ) {
		LOG( ERROR, "Cannot create Terrain3D direct RD texture array from an empty first layer" );
		return RID();
	}
	const int width = pFirst->get_width();
	const int height = pFirst->get_height();
	const int format = (int)( pFirst->get_format() );
	const bool bMipmaps = pFirst->has_mipmaps();
	TypedArray<PackedByteArray> initial_data;
	initial_data.resize( rLayers.size() );
	size_t payload_bytes = 0u;
	for ( int layer = 0; layer < rLayers.size(); ++layer ) {
		const Ref<Image> pImage = rLayers[layer];
		if ( pImage.is_null() || pImage->is_empty() || pImage->get_width() != width || pImage->get_height() != height ||
				(int)( pImage->get_format() ) != format || pImage->has_mipmaps() != bMipmaps ) {
			LOG( ERROR, "Rejected incompatible Terrain3D direct RD texture-array layer ", layer );
			return RID();
		}
		const PackedByteArray data = pImage->get_data();
		initial_data[layer] = data;
		payload_bytes += (size_t)( data.size() );
	}

	t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildLayout(
				 pFirst, (uint32_t)( rLayers.size() ), true, (uint64_t)( payload_bytes ), semantic, layout ) ) {
		LOG( ERROR, "Unsupported Terrain3D generated texture-array image format: ", format );
		return RID();
	}
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	if ( !pManager ) {
		return RID();
	}
	const RID new_rid = pManager->createTexture( layout, initial_data, getResourceName( ".Array" ) );
	if ( !new_rid.is_valid() ) {
		m_bDirty = true;
		LOG( ERROR, "RenderingDevice rejected Terrain3D generated texture array" );
		return RID();
	}
	if ( m_hRid.is_valid() ) {
		pManager->freeTexture( m_hRid, m_nPayloadBytes );
	}
	if ( m_hRegionStagingRid.is_valid() ) {
		pManager->freeResource( m_hRegionStagingRid, m_nRegionStagingBytes );
	}
	m_image.unref();
	m_layers = rLayers.duplicate();
	m_hRid = new_rid;
	m_hRegionStagingRid = RID();
	m_bDirty = false;
	m_nSize = rLayers.size();
	m_nWidth = width;
	m_nHeight = height;
	m_nFormat = format;
	m_nRegionStagingWidth = 0;
	m_nRegionStagingHeight = 0;
	m_nRegionStagingFormat = -1;
	m_bMipmaps = bMipmaps;
	m_bLayered = true;
	m_nPayloadBytes = payload_bytes;
	m_nRegionStagingBytes = 0u;
	m_semantic = semantic;
	return m_hRid;
}

bool Terrain3DGeneratedTexture::update( const Ref<Image>& rImage, const int nLayer )
{
	if ( !m_hRid.is_valid() || m_bDirty || !m_bLayered || nLayer < 0 || nLayer >= m_nSize ||
			!matchesLayout( rImage ) ) {
		LOG( ERROR,
				"Rejected direct RD Texture2DArray update. RID valid: ",
				m_hRid.is_valid(),
				", dirty: ",
				m_bDirty,
				", layered: ",
				m_bLayered,
				", layer: ",
				nLayer,
				", layer count: ",
				m_nSize,
				", compatible image: ",
				matchesLayout( rImage ) );
		return false;
	}
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	if ( !pManager || !pManager->updateTexture( m_hRid, (uint32_t)( nLayer ), rImage->get_data() ) ) {
		return false;
	}
	m_layers[nLayer] = rImage;
	return true;
}

RID Terrain3DGeneratedTexture::create( const Ref<Image>& rImage, const t3dRdTextureSemantic_t semantic )
{
	if ( rImage.is_null() || rImage->is_empty() ) {
		m_bDirty = true;
		LOG( ERROR, "Cannot create generated direct RD Texture2D from an empty image" );
		return RID();
	}
	const PackedByteArray data = rImage->get_data();
	t3dRdTextureLayout_t layout;
	if ( !Terrain3DRdTextureManager::buildLayout( rImage, 1u, false, (uint64_t)( data.size() ), semantic, layout ) ) {
		m_bDirty = true;
		LOG( ERROR, "Unsupported Terrain3D generated Texture2D image format: ", (int)( rImage->get_format() ) );
		return RID();
	}
	TypedArray<PackedByteArray> initial_data;
	initial_data.push_back( data );
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	if ( !pManager ) {
		return RID();
	}
	const RID new_rid = pManager->createTexture( layout, initial_data, getResourceName( ".2D" ) );
	if ( !new_rid.is_valid() ) {
		m_bDirty = true;
		LOG( ERROR, "RenderingDevice rejected Terrain3D generated Texture2D" );
		return RID();
	}
	if ( m_hRid.is_valid() ) {
		pManager->freeTexture( m_hRid, m_nPayloadBytes );
	}
	if ( m_hRegionStagingRid.is_valid() ) {
		pManager->freeResource( m_hRegionStagingRid, m_nRegionStagingBytes );
	}
	m_image = rImage;
	m_layers.clear();
	m_hRid = new_rid;
	m_hRegionStagingRid = RID();
	m_bDirty = false;
	m_nSize = 1;
	m_nWidth = rImage->get_width();
	m_nHeight = rImage->get_height();
	m_nFormat = (int)( rImage->get_format() );
	m_nRegionStagingWidth = 0;
	m_nRegionStagingHeight = 0;
	m_nRegionStagingFormat = -1;
	m_bMipmaps = rImage->has_mipmaps();
	m_bLayered = false;
	m_nPayloadBytes = (size_t)( data.size() );
	m_nRegionStagingBytes = 0u;
	m_semantic = semantic;
	return m_hRid;
}

bool Terrain3DGeneratedTexture::update2d( const Ref<Image>& rImage )
{
	if ( !m_hRid.is_valid() || m_bDirty || m_bLayered || m_nSize != 1 ) {
		return false;
	}
	if ( !matchesLayout( rImage ) ) {
		// A 2D generated texture has complete replacement data in this call, so a
		// layout change can be reconstructed deterministically without a native texture bridge.
		return create( rImage, m_semantic ).is_valid();
	}
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	if ( !pManager || !pManager->updateTexture( m_hRid, 0u, rImage->get_data() ) ) {
		return false;
	}
	m_image = rImage;
	return true;
}

bool Terrain3DGeneratedTexture::updateRegion2d( const Ref<Image>& rImage, const Rect2i& rDestination )
{
	if ( !m_hRid.is_valid() || m_bDirty || m_bLayered || m_nSize != 1 || m_bMipmaps || rImage.is_null() ||
			rImage->is_empty() || rImage->has_mipmaps() || (int)( rImage->get_format() ) != m_nFormat ||
			rDestination.position.x < 0 || rDestination.position.y < 0 || rDestination.size.x <= 0 ||
			rDestination.size.y <= 0 || rDestination.size.x != rImage->get_width() ||
			rDestination.size.y != rImage->get_height() || rDestination.get_end().x > m_nWidth ||
			rDestination.get_end().y > m_nHeight ) {
		return false;
	}
	if ( !ensureRegionStaging( rImage ) ) {
		return false;
	}
	Terrain3DRdTextureManager *const pManager = t3dGeneratedTextureManager();
	return pManager &&
			pManager->updateTextureRegion( m_hRid,
					m_hRegionStagingRid,
					(uint32_t)( rDestination.position.x ),
					(uint32_t)( rDestination.position.y ),
					(uint32_t)( rDestination.size.x ),
					(uint32_t)( rDestination.size.y ),
					rImage->get_data() );
}
