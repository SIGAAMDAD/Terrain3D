#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_memory_domain.h"

#include <EASTL/utility.h>
#include <godot_cpp/variant/utility_functions.hpp>

uint32_t Terrain3DRdTexture::maximumMipCount( const uint32_t nWidth, const uint32_t nHeight ) {
	if ( nWidth == 0u || nHeight == 0u ) { return 0u; }
	uint32_t dimension = nWidth > nHeight ? nWidth : nHeight;
	uint32_t count = 1u;
	while ( dimension > 1u ) {
		dimension >>= 1u;
		++count;
	}
	return count;
}

bool Terrain3DRdTexture::validateLayout( const t3dRdTextureLayout_t &rLayout ) {
	return rLayout.valid() && rLayout.m_nMipmaps <= maximumMipCount( rLayout.m_nWidth, rLayout.m_nHeight );
}

Ref<RDTextureFormat> Terrain3DRdTexture::buildFormat( const t3dRdTextureLayout_t &rLayout ) {
	Ref<RDTextureFormat> format;
	format.instantiate();
	if ( format.is_null() ) { return format; }
	format->set_format( rLayout.m_format );
	format->set_width( rLayout.m_nWidth );
	format->set_height( rLayout.m_nHeight );
	format->set_depth( rLayout.m_nDepth );
	format->set_array_layers( rLayout.m_nLayers );
	format->set_mipmaps( rLayout.m_nMipmaps );
	format->set_texture_type( rLayout.m_type );
	format->set_samples( rLayout.m_samples );
	format->set_usage_bits( BitField<RenderingDevice::TextureUsageBits>( rLayout.m_nUsageBits ) );
	return format;
}

bool Terrain3DRdTexture::isBorrowedValid( RenderingDevice *const pRd, const RID &rRid ) {
	return pRd && rRid.is_valid() && pRd->texture_is_valid( rRid );
}

Ref<RDTextureFormat> Terrain3DRdTexture::borrowedFormat( RenderingDevice *const pRd, const RID &rRid ) {
	return isBorrowedValid( pRd, rRid ) ? pRd->texture_get_format( rRid ) : Ref<RDTextureFormat>();
}

bool Terrain3DRdTexture::supportsFormatUsage( RenderingDevice *const pRd, const RenderingDevice::DataFormat format,
		const BitField<RenderingDevice::TextureUsageBits> usage ) {
	return pRd && pRd->texture_is_format_supported_for_usage( format, usage );
}

bool Terrain3DRdTexture::registerTracked( Terrain3DRdContext &rContext, RenderingDevice *const pRd, RID &rRid,
        const String &rName, const t3dRdResourceOwner_t owner,
        const t3dRdMemoryCategory_t memory, const uint64_t nBytes, const bool bFailureFatal ) {
	if ( rRid.is_valid() && !rName.is_empty()
	        && rContext.registerResource( rRid, rName, T3D_RD_CONTEXT_RESOURCE_TEXTURE, memory, nBytes, owner ) ) {
		return true;
	}
	if ( bFailureFatal ) {
		rContext.recordResourceCreationFailure( rName, T3D_RD_CONTEXT_RESOURCE_TEXTURE );
	}
	if ( pRd && rRid.is_valid() ) pRd->free_rid( rRid );
	rRid = RID();
	return false;
}

bool Terrain3DRdTexture::createTracked( Terrain3DRdContext &rContext, const t3dRdTextureSpec_t &rSpec,
        const TypedArray<PackedByteArray> &rInitialData ) {
	if ( !validateLayout( rSpec.m_layout ) ) {
		UtilityFunctions::push_error( "[Terrain3D][RDTexture] Invalid texture layout name=", rSpec.m_name,
		        " width=", rSpec.m_layout.m_nWidth, " height=", rSpec.m_layout.m_nHeight,
		        " layers=", rSpec.m_layout.m_nLayers, " mipmaps=", rSpec.m_layout.m_nMipmaps,
		        " max_mipmaps=", maximumMipCount( rSpec.m_layout.m_nWidth, rSpec.m_layout.m_nHeight ) );
		return false;
	}
	if ( isValid() || rSpec.m_name.is_empty()
	        || ( !rInitialData.is_empty() && rInitialData.size() != ( int64_t )rSpec.m_layout.m_nLayers ) ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd ) { return false; }
	const Ref<RDTextureFormat> format = buildFormat( rSpec.m_layout );
	if ( format.is_null() || !supportsFormatUsage( rd, rSpec.m_layout.m_format,
	             BitField<RenderingDevice::TextureUsageBits>( rSpec.m_layout.m_nUsageBits ) ) ) {
		if ( rSpec.m_bFailureFatal ) rContext.recordResourceCreationFailure( rSpec.m_name, T3D_RD_CONTEXT_RESOURCE_TEXTURE );
		return false;
	}
	Ref<RDTextureView> view;
	view.instantiate();
	Terrain3DMemoryReservation reservation;
	if (!reservation.resize(rSpec.m_layout.m_nBytes, Terrain3DMemoryAccount::GPU, g_t3dMemoryAccount, t3dRdMemoryDomain(rSpec.m_owner, rSpec.m_memory))) { return false; }
	RID created = rd->texture_create( format, view, rInitialData );
	if ( !created.is_valid() ) {
		UtilityFunctions::push_error( "[Terrain3D][RDTexture] RenderingDevice texture_create failed name=", rSpec.m_name,
		        " width=", rSpec.m_layout.m_nWidth, " height=", rSpec.m_layout.m_nHeight,
		        " layers=", rSpec.m_layout.m_nLayers, " mipmaps=", rSpec.m_layout.m_nMipmaps,
		        " format=", ( int64_t )rSpec.m_layout.m_format, " usage=", ( int64_t )rSpec.m_layout.m_nUsageBits );
	}
	if ( !registerTracked( rContext, rd, created, rSpec.m_name, rSpec.m_owner, rSpec.m_memory, rSpec.m_layout.m_nBytes, rSpec.m_bFailureFatal ) ) {
		return false;
	}
	m_memoryReservation.swap(reservation);
	m_layout = rSpec.m_layout;
	m_bSharedView = false;
	m_state.publish( created, rSpec.m_owner );
	for (int64_t layer = 0; layer < rInitialData.size(); ++layer) {
		const PackedByteArray bytes = rInitialData[layer];
		rContext.recordTextureUpload((uint64_t)bytes.size(), m_state.owner(), T3D_GPU_PUBLICATION);
	}
	return true;
}

bool Terrain3DRdTexture::createSharedTracked( Terrain3DRdContext &rContext, const Terrain3DRdTexture &rParent,
        const t3dRdTextureSliceSpec_t &rSpec, const Ref<RDTextureView> &rRequestedView ) {
	if ( isValid() || !rParent.isValid() || rSpec.m_name.is_empty() || rSpec.m_nMipmaps == 0u
	        || rSpec.m_nLayer >= rParent.m_layout.m_nLayers || rSpec.m_nMipmap >= rParent.m_layout.m_nMipmaps
	        || rSpec.m_nMipmaps > rParent.m_layout.m_nMipmaps - rSpec.m_nMipmap ) {
		return false;
	}
	RenderingDevice *const rd = rContext.getDevice();
	if ( !rd || !rContext.ownsResource( rParent.rid() ) ) { return false; }
	Ref<RDTextureView> view = rRequestedView;
	if ( view.is_null() ) view.instantiate();
	RID created = rd->texture_create_shared_from_slice( view, rParent.rid(), rSpec.m_nLayer, rSpec.m_nMipmap,
	        rSpec.m_nMipmaps, rSpec.m_sliceType );
	if ( !registerTracked( rContext, rd, created, rSpec.m_name, rSpec.m_owner, rSpec.m_memory, 0u, true ) ) { return false; }
	m_layout = rParent.m_layout;
	m_layout.m_nWidth = rParent.m_layout.m_nWidth >> rSpec.m_nMipmap;
	m_layout.m_nHeight = rParent.m_layout.m_nHeight >> rSpec.m_nMipmap;
	if ( m_layout.m_nWidth == 0u ) m_layout.m_nWidth = 1u;
	if ( m_layout.m_nHeight == 0u ) m_layout.m_nHeight = 1u;
	m_layout.m_nLayers = 1u;
	m_layout.m_nMipmaps = rSpec.m_nMipmaps;
	m_layout.m_nBytes = 0u;
	m_bSharedView = true;
	m_state.publish( created, rSpec.m_owner );
	return true;
}

bool Terrain3DRdTexture::updateLayerTracked( Terrain3DRdContext &rContext, const uint32_t nLayer, const PackedByteArray &rData,
        const Terrain3DGpuBufferPolicy policy ) const {
	RenderingDevice *const rd = rContext.getDevice();
	if (!rd || !isValid() || nLayer >= m_layout.m_nLayers || rData.is_empty() || !rContext.ownsResource(m_state.rid())
	        || rd->texture_update(m_state.rid(), nLayer, rData) != OK) { return false; }
	rContext.recordTextureUpload((uint64_t)rData.size(), m_state.owner(), policy);
	return true;
}

bool Terrain3DRdTexture::copyRegionToTracked( Terrain3DRdContext &rContext, Terrain3DRdTexture &rDestination,
        const Vector3 &rFrom, const Vector3 &rTo, const Vector3 &rSize,
        const uint32_t nSrcMipmap, const uint32_t nDstMipmap, const uint32_t nSrcLayer, const uint32_t nDstLayer ) const {
	if ( !isValid() || !rDestination.isValid() || nSrcLayer >= m_layout.m_nLayers || nDstLayer >= rDestination.m_layout.m_nLayers
	        || nSrcMipmap >= m_layout.m_nMipmaps || nDstMipmap >= rDestination.m_layout.m_nMipmaps ) return false;
	RenderingDevice *const rd = rContext.getDevice();
	return rd && rContext.ownsResource( m_state.rid() ) && rContext.ownsResource( rDestination.m_state.rid() )
	        && rd->texture_copy( m_state.rid(), rDestination.m_state.rid(), rFrom, rTo, rSize, nSrcMipmap, nDstMipmap, nSrcLayer, nDstLayer ) == OK;
}

PackedByteArray Terrain3DRdTexture::readbackTracked( Terrain3DRdContext &rContext, const uint32_t nLayer ) const {
	RenderingDevice *const rd = rContext.getDevice();
	return rd && isValid() && nLayer < m_layout.m_nLayers && rContext.ownsResource( m_state.rid() )
	        ? rd->texture_get_data( m_state.rid(), nLayer ) : PackedByteArray();
}

Error Terrain3DRdTexture::readbackAsyncTracked( Terrain3DRdContext &rContext, const uint32_t nLayer, const Callable &rCallback ) const {
	RenderingDevice *const rd = rContext.getDevice();
	return rd && isValid() && nLayer < m_layout.m_nLayers && rContext.ownsResource( m_state.rid() )
	        ? rd->texture_get_data_async( m_state.rid(), nLayer, rCallback ) : ERR_INVALID_PARAMETER;
}

void Terrain3DRdTexture::releaseTracked( Terrain3DRdContext &rContext ) {
	if ( m_state.rid().is_valid() ) rContext.freeResource( m_state.rid(), m_layout.m_nBytes );
	abandon();
}

void Terrain3DRdTexture::abandon( void ) {
	m_memoryReservation.reset();
	m_layout = t3dRdTextureLayout_t{};
	m_bSharedView = false;
	m_state.abandon();
}

void Terrain3DRdTexture::swap( Terrain3DRdTexture &rOther ) {
	m_memoryReservation.swap( rOther.m_memoryReservation );
	using eastl::swap;
	m_state.swap( rOther.m_state );
	swap( m_layout, rOther.m_layout );
	swap( m_bSharedView, rOther.m_bSharedView );
}
