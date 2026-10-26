#include "rendering/rd/resources/terrain_3d_rd_texture_manager.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include <stdint.h>

static uint32_t t3dRawPagePixelBytes(
	RenderingDevice::DataFormat format ) {
	switch ( format ) {
	case RenderingDevice::DATA_FORMAT_R8_UNORM:
		return 1u;
	case RenderingDevice::DATA_FORMAT_R8G8_UNORM:
		return 2u;
	case RenderingDevice::DATA_FORMAT_R16_UNORM:
	case RenderingDevice::DATA_FORMAT_R16_UINT:
		return 2u;
	case RenderingDevice::DATA_FORMAT_R32_SFLOAT:
	case RenderingDevice::DATA_FORMAT_R32_UINT:
	case RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM:
		return 4u;
	default:
		return 0u;
	}
}
bool Terrain3DRdTextureManager::buildRawPageLayout(
	RenderingDevice::DataFormat format,
	uint32_t					width,
	uint32_t					height,
	uint32_t					layers,
	uint32_t					mipmaps,
	bool						storageWrites,
	t3dRdTextureLayout_t	   &layout ) {
	layout = t3dRdTextureLayout_t{};
	const uint32_t pixelBytes = t3dRawPagePixelBytes( format );
	if ( !pixelBytes || !width || !height || !layers || !mipmaps || layers > 256u )
		return false;
	uint32_t maxMipCount = 1u;
	uint32_t dimension = width > height ? width : height;
	while ( dimension > 1u ) {
		dimension >>= 1u;
		++maxMipCount;
	}
	if ( mipmaps > maxMipCount )
		return false;
	uint64_t bytesPerLayer = 0u;
	uint32_t mipWidth = width, mipHeight = height;
	for ( uint32_t mip = 0u; mip < mipmaps; ++mip ) {
		bytesPerLayer += ( uint64_t ) mipWidth * ( uint64_t ) mipHeight * pixelBytes;
		mipWidth = mipWidth > 1u ? mipWidth >> 1u : 1u;
		mipHeight = mipHeight > 1u ? mipHeight >> 1u : 1u;
	}
	if ( bytesPerLayer > UINT64_MAX / layers )
		return false;
	layout.m_format = format;
	layout.m_type = layers > 1u ? RenderingDevice::TEXTURE_TYPE_2D_ARRAY : RenderingDevice::TEXTURE_TYPE_2D;
	layout.m_nWidth = width;
	layout.m_nHeight = height;
	layout.m_nLayers = layers;
	layout.m_nMipmaps = mipmaps;
	layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
						  RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
						  RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	if ( storageWrites )
		layout.m_nUsageBits |= RenderingDevice::TEXTURE_USAGE_STORAGE_BIT;
	layout.m_nBytes = bytesPerLayer * layers;
	return layout.valid();
}
bool Terrain3DRdTextureManager::createRawPageTexture(
	Terrain3DRdTexture					&texture,
	RenderingDevice::DataFormat			 format,
	uint32_t							 width,
	uint32_t							 height,
	uint32_t							 layers,
	uint32_t							 mipmaps,
	bool								 storageWrites,
	const TypedArray< PackedByteArray > &initialData,
	const String						&name,
	int									 memoryCategory,
	bool								 failureFatal ) {
	t3dRdTextureLayout_t layout;
	if ( !buildRawPageLayout( format, width, height, layers, mipmaps, storageWrites, layout ) )
		return false;
	const uint64_t layerBytes = layout.m_nBytes / layers;
	if ( !initialData.is_empty() ) {
		if ( initialData.size() != ( int64_t ) layers )
			return false;
		for ( int64_t i = 0; i < initialData.size(); ++i ) {
			const PackedByteArray data = initialData[i];
			if ( ( uint64_t ) data.size() != layerBytes )
				return false;
		}
	}
	return createTexture( texture, layout, initialData, name, memoryCategory, failureFatal );
}
bool Terrain3DRdTextureManager::uploadRawPage(
	Terrain3DRdTexture	  &texture,
	uint32_t			   layer,
	const PackedByteArray &bytes ) {
	if ( !texture.isValid() || layer >= texture.layout().m_nLayers ||
		 bytes.size() != ( int64_t ) ( texture.layout().m_nBytes / texture.layout().m_nLayers ) )
		return false;
	return updateTexture( texture, layer, bytes );
}
bool Terrain3DRdTextureManager::copyRawPage(
	Terrain3DRdContext		 &context,
	const Terrain3DRdTexture &source,
	Terrain3DRdTexture		 &destination,
	uint32_t				  sourceLayer,
	uint32_t				  destLayer ) {
	const t3dRdTextureLayout_t &a = source.layout(), &b = destination.layout();
	if ( a.m_format != b.m_format || a.m_nWidth != b.m_nWidth || a.m_nHeight != b.m_nHeight ||
		 a.m_nMipmaps != b.m_nMipmaps || sourceLayer >= a.m_nLayers || destLayer >= b.m_nLayers )
		return false;
	// Each mip has its own dimensions; never implicitly copy only base mip.
	for ( uint32_t mip = 0; mip < a.m_nMipmaps; ++mip ) {
		uint32_t width = a.m_nWidth >> mip, height = a.m_nHeight >> mip;
		if ( !width )
			width = 1u;
		if ( !height )
			height = 1u;
		if ( !source.copyRegionToTracked( context,
										  destination,
										  Vector3(),
										  Vector3(),
										  Vector3( width, height, 1 ),
										  mip,
										  mip,
										  sourceLayer,
										  destLayer ) )
			return false;
	}
	return true;
}

// Integer storage + UNORM sampled view uses the identical 16-bit pixel payload.
// This requires mutable-format view support on the active RD backend.
bool Terrain3DRdTextureManager::createWritableHeightPage(
	Terrain3DRdTexture					&texture,
	uint32_t							 width,
	uint32_t							 height,
	uint32_t							 layers,
	const TypedArray< PackedByteArray > &data,
	const String						&name ) {
	t3dRdTextureLayout_t layout;
	if ( !buildRawPageLayout( RenderingDevice::DATA_FORMAT_R16_UINT, width, height, layers, 1u, true, layout ) )
		return false;
	layout.m_shareableFormat = RenderingDevice::DATA_FORMAT_R16_UNORM;
	if ( !data.is_empty() ) {
		if ( data.size() != ( int64_t ) layers )
			return false;
		for ( int64_t i = 0; i < data.size(); ++i ) {
			const PackedByteArray bytes = data[i];
			if ( ( uint64_t ) bytes.size() != layout.m_nBytes / layers )
				return false;
		}
	}
	return createTexture( texture, layout, data, name, T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE, false );
}
bool Terrain3DRdTextureManager::createHeightSampleView(
	Terrain3DRdContext		 &context,
	Terrain3DRdTexture		 &view,
	const Terrain3DRdTexture &storage,
	uint32_t				  layer,
	const String			 &name ) {
	if ( storage.layout().m_format != RenderingDevice::DATA_FORMAT_R16_UINT ||
		 storage.layout().m_shareableFormat != RenderingDevice::DATA_FORMAT_R16_UNORM )
		return false;
	Ref< RDTextureView > sampledView;
	sampledView.instantiate();
	if ( sampledView.is_null() )
		return false;
	sampledView->set_format_override( RenderingDevice::DATA_FORMAT_R16_UNORM );
	t3dRdTextureSliceSpec_t slice;
	slice.m_name = name;
	slice.m_owner = T3D_RD_CONTEXT_OWNER_TERRAIN;
	slice.m_nLayer = layer;
	return view.createSharedTracked( context, storage, slice, sampledView );
}
