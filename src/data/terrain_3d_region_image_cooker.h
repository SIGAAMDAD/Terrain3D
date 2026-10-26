// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_REGION_IMAGE_COOKER_H
#define TERRAIN3D_REGION_IMAGE_COOKER_H

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_assets.h"
#include "assets/terrain_3d_texture_asset.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_control_map_cooker.h"
#include "data/terrain_3d_region.h"
#include "lighting/terrain_3d_region_lighting.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>

EA_FORCE_INLINE Ref<Image> t3dRegionImageResolveMap( const Terrain3DRegion *pRegion,
        const t3dMapType_t eMapType,
        const Ref<Image> &rDefaultHeight,
        const Ref<Image> &rDefaultControl,
        const Ref<Image> &rDefaultColor ) {
	if ( pRegion && !pRegion->isDeleted() ) {
		const Ref<Image> pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
		}
	}
	switch ( eMapType ) {
		case TYPE_HEIGHT:
			return rDefaultHeight;
		case TYPE_CONTROL:
			return rDefaultControl;
		case TYPE_COLOR:
			return rDefaultColor;
		default:
			return Ref<Image>();
	}
}

EA_FORCE_INLINE Ref<Image> t3dRegionImageCookGpuColor(
        const Ref<Image> &rImage, const Ref<Image> &rDefaultImage, const int nResolutionDivisor ) {
	Ref<Image> pSource = rImage.is_valid() ? rImage : rDefaultImage;
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	const int divisor = MAX( 1, nResolutionDivisor );
	if ( divisor <= 1 && pSource->has_mipmaps() ) {
		return pSource;
	}
	Ref<Image> pGpuImage;
	pGpuImage.instantiate();
	pGpuImage->copy_from( pSource );
	if ( pGpuImage->has_mipmaps() ) {
		pGpuImage->clear_mipmaps();
	}
	if ( divisor > 1 ) {
		pGpuImage->resize(
		        MAX( 1, pSource->get_width() / divisor ), MAX( 1, pSource->get_height() / divisor ), Image::INTERPOLATE_BILINEAR );
	}
	if ( pGpuImage->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pGpuImage;
}

EA_FORCE_INLINE Ref<Image> t3dRegionImageCookGpuDerived(
        const Ref<Image> &rImage, const Ref<Image> &rDefaultImage, const int nResolutionDivisor ) {
	Ref<Image> pSource = rImage.is_valid() ? rImage : rDefaultImage;
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	const int divisor = MAX( 1, nResolutionDivisor );
	if ( divisor <= 1 ) {
		return pSource;
	}
	Ref<Image> pGpuImage;
	pGpuImage.instantiate();
	pGpuImage->copy_from( pSource );
	pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ), MAX( 1, pSource->get_height() / divisor ), Image::INTERPOLATE_BILINEAR );
	return pGpuImage;
}

EA_FORCE_INLINE bool t3dRegionImagePrepareMiddleCompositeMaterials( const Ref<Terrain3DAssets> &rAssets,
        PackedColorArray &rColors,
        PackedFloat32Array &rScales,
        Ref<Image> *pImages,
        const size_t nImageCapacity ) {
	rColors.clear();
	rScales.clear();
	for ( size_t i = 0u; i < nImageCapacity; ++i ) {
		pImages[i].unref();
	}
	if ( rAssets.is_null() ) {
		return true;
	}
	rColors = rAssets->getTextureColors();
	rScales = rAssets->getTextureUvScales();
	const int texture_count = MIN( rAssets->getTextureCount(), ( int )( nImageCapacity ) );
	for ( int material = 0; material < texture_count; ++material ) {
		const Ref<Terrain3DTextureAsset> pAsset = rAssets->getTextureAsset( material );
		const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getAlbedoTexture() : Ref<Texture2D>();
		Ref<Image> pMaterialImage = pTexture.is_valid() ? pTexture->get_image() : Ref<Image>();
		if ( pMaterialImage.is_valid() && pMaterialImage->is_compressed() ) {
			pMaterialImage = pMaterialImage->duplicate();
			if ( pMaterialImage->decompress() != OK ) {
				pMaterialImage.unref();
			}
		}
		if ( pMaterialImage.is_valid() && pMaterialImage->is_empty() ) {
			pMaterialImage.unref();
		}
		pImages[( size_t )( material )] = pMaterialImage;
	}
	return true;
}

EA_FORCE_INLINE Color t3dRegionImageSampleMiddleCompositeMaterial( const Ref<Image> *pMaterialImages,
        const size_t nMaterialImageCount,
        const PackedFloat32Array &rMaterialScales,
        const int nMaterialId,
        const Color &rTint,
        const float fCosAngle,
        const float fSinAngle,
        const float fWorldX,
        const float fWorldY,
        const float fControlScale ) {
	if ( nMaterialId < 0 || ( size_t )( nMaterialId ) >= nMaterialImageCount || pMaterialImages[( size_t )( nMaterialId )].is_null() ) {
		return rTint;
	}
	const Ref<Image> &pImage = pMaterialImages[( size_t )( nMaterialId )];
	const float fAssetScale = nMaterialId < rMaterialScales.size() ? rMaterialScales[nMaterialId] : 0.1f;
	const float fSourceU = ( fCosAngle * fWorldX + fSinAngle * fWorldY ) * fControlScale * fAssetScale;
	const float fSourceV = ( fCosAngle * fWorldY - fSinAngle * fWorldX ) * fControlScale * fAssetScale;
	const float fWrappedU = fSourceU - Math::floor( fSourceU );
	const float fWrappedV = fSourceV - Math::floor( fSourceV );
	const int nSourceX = CLAMP( ( int )( fWrappedU * pImage->get_width() ), 0, pImage->get_width() - 1 );
	const int nSourceY = CLAMP( ( int )( fWrappedV * pImage->get_height() ), 0, pImage->get_height() - 1 );
	return pImage->get_pixel( nSourceX, nSourceY ) * rTint;
}

EA_FORCE_INLINE Ref<Image> t3dRegionImageCookMiddleComposite( const Vector2i &rLocation,
        const Ref<Image> &rControl,
        const Ref<Image> &rColor,
        const PackedColorArray &rMaterialColors,
        const PackedFloat32Array &rMaterialScales,
        const Ref<Image> *pMaterialImages,
        const size_t nMaterialImageCount,
        PackedByteArray &rOutput ) {
	if ( rControl.is_null() || rColor.is_null() || rControl->get_format() != Image::FORMAT_RF
	        || rColor->get_format() != Image::FORMAT_RGBA8 ) {
		return Ref<Image>();
	}
	const int width = rColor->get_width();
	const int height = rColor->get_height();
	if ( width <= 0 || height <= 0 ) {
		return Ref<Image>();
	}
	const PackedByteArray control_bytes = rControl->get_data();
	const PackedByteArray color_bytes = rColor->get_data();
	const int64_t output_bytes = ( int64_t )( width )*height * 4;
	if ( control_bytes.size() < ( int64_t )( rControl->get_width() ) * rControl->get_height() * 4
	        || color_bytes.size() < output_bytes || rOutput.size() != output_bytes ) {
		return Ref<Image>();
	}
	uint8_t *const pWrite = rOutput.ptrw();
	const uint8_t *const pControl = control_bytes.ptr();
	const uint8_t *const pColor = color_bytes.ptr();
	for ( int y = 0; y < height; ++y ) {
		const int control_y = MIN( rControl->get_height() - 1, y * rControl->get_height() / height );
		for ( int x = 0; x < width; ++x ) {
			const int control_x = MIN( rControl->get_width() - 1, x * rControl->get_width() / width );
			const uint32_t control =
			        t3dControlCookReadU32Unaligned( pControl, ( size_t )( control_y * rControl->get_width() + control_x ) * 4u );
			const int base = ( int )( t3dGetBase( control ) );
			const int overlay = ( int )( t3dGetOverlay( control ) );
			const Color base_color = base < rMaterialColors.size() ? rMaterialColors[base] : s_colorWhite;
			const Color overlay_color = overlay < rMaterialColors.size() ? rMaterialColors[overlay] : base_color;
			const float control_scale = 0.9f - ( float )( ( ( t3dGetUvScale( control ) + 3u ) % 8u ) + 1u ) * 0.1f;
			const float angle = ( float )( t3dGetUvRotation( control ) )*Math::TAU / 16.0f;
			const float cs = Math::cos( angle );
			const float sn = Math::sin( angle );
			const float local_x = ( ( float )( x ) + 0.5f ) * ( float )( s_nRegionSize ) / ( float )( width );
			const float local_y = ( ( float )( y ) + 0.5f ) * ( float )( s_nRegionSize ) / ( float )( height );
			const float world_x = ( ( float )( rLocation.x * s_nRegionSize ) + local_x ) * ( float )( s_fRegionVertexSpacing );
			const float world_y = ( ( float )( rLocation.y * s_nRegionSize ) + local_y ) * ( float )( s_fRegionVertexSpacing );
			const Color base_sample = t3dRegionImageSampleMiddleCompositeMaterial( pMaterialImages,
			        nMaterialImageCount,
			        rMaterialScales,
			        base,
			        base_color,
			        cs,
			        sn,
			        world_x,
			        world_y,
			        control_scale );
			const Color overlay_sample = t3dRegionImageSampleMiddleCompositeMaterial( pMaterialImages,
			        nMaterialImageCount,
			        rMaterialScales,
			        overlay,
			        overlay_color,
			        cs,
			        sn,
			        world_x,
			        world_y,
			        control_scale );
			float blend = ( float )( t3dGetBlend( control ) ) / 255.0f;
			blend = CLAMP( ( blend - 0.12f ) / 0.76f, 0.0f, 1.0f );
			blend = blend * blend * ( 3.0f - 2.0f * blend );
			const Color material_color = base_sample.lerp( overlay_sample, blend );
			const size_t offset = ( size_t )( y * width + x ) * 4u;
			const float authored_r = ( float )( pColor[offset] ) / 255.0f;
			const float authored_g = ( float )( pColor[offset + 1u] ) / 255.0f;
			const float authored_b = ( float )( pColor[offset + 2u] ) / 255.0f;
			pWrite[offset] = t3dControlCookColorToByte( authored_r * material_color.r );
			pWrite[offset + 1u] = t3dControlCookColorToByte( authored_g * material_color.g );
			pWrite[offset + 2u] = t3dControlCookColorToByte( authored_b * material_color.b );
			pWrite[offset + 3u] =
			        t3dIsHole( control ) ? 0u : ( uint8_t )( 0x80u | ( ( uint32_t )( pColor[offset + 3u] ) * 127u + 127u ) / 255u );
		}
	}
	Ref<Image> pComposite = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, rOutput );
	if ( pComposite.is_null() || pComposite->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pComposite;
}

EA_FORCE_INLINE Ref<Image> t3dRegionImageCookTerrainShadowMask( const Terrain3DRegion *pRegion, const int nRegionSize ) {
	if ( nRegionSize <= 0 ) {
		return Ref<Image>();
	}
	Ref<Image> image;
	if ( pRegion && !pRegion->isDeleted() ) {
		const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
		if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
			const String shadow_path = lighting->getTerrainShadowMaskPath();
			if ( FileAccess::file_exists( shadow_path ) ) {
				const Ref<Resource> resource = ResourceLoader::get_singleton()->load( shadow_path );
				image = resource;
			}
		}
	}
	const Color default_color( 1.0f, 1.0f, 1.0f, 1.0f );
	if ( image.is_null() || image->is_empty() ) {
		return Terrain3DUtil::getFilledImage( V2I( nRegionSize ), default_color, true, Image::FORMAT_L8 );
	}
	image = image->duplicate();
	if ( image->is_compressed() && image->decompress() != OK ) {
		return Terrain3DUtil::getFilledImage( V2I( nRegionSize ), default_color, true, Image::FORMAT_L8 );
	}
	if ( image->has_mipmaps() ) {
		image->clear_mipmaps();
	}
	if ( image->get_format() != Image::FORMAT_L8 ) {
		image->convert( Image::FORMAT_L8 );
	}
	if ( image->get_width() != nRegionSize || image->get_height() != nRegionSize ) {
		image->resize( nRegionSize, nRegionSize, Image::INTERPOLATE_BILINEAR );
	}
	if ( image->generate_mipmaps() != OK ) {
		return Terrain3DUtil::getFilledImage( V2I( nRegionSize ), default_color, true, Image::FORMAT_L8 );
	}
	return image;
}

#endif // TERRAIN3D_REGION_IMAGE_COOKER_H
