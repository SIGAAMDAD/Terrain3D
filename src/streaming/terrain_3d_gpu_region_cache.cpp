// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_gpu_region_cache.h"

#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "data/terrain_3d_map_edit_service.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampling_storage.h"
#include "eastl_config.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/terrain_3d_compute.h"
#include "rendering/terrain_3d_material.h"
#include "streaming/terrain_3d_cpu_region_cache.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "terrain_3d.h"

#include <EASTL/array.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <limits>
#include <utility>

static_assert( s_nGpuCacheMaxLayers <= ( int )( Terrain3DRdTextureManager::getTrackedBatchSlotCount() ) );
static_assert( Terrain3DRdTextureManager::getTrackedBatchUpdateCount() >= 6u );

template <typename Callback>
static bool t3dGpuRegionCacheForEachSquareRing( const Vector2i &rCenter, const int nMaxRadius, Callback &&rCallback ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !rCallback( rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !rCallback( rCenter + Vector2i( x, -radius ) ) || !rCallback( rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !rCallback( rCenter + Vector2i( -radius, y ) ) || !rCallback( rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

typedef struct t3dControlCookSource_s {
	PackedByteArray m_bytes;
	bool m_bPresent = false;
	bool m_bImplicit = true;
} t3dControlCookSource_t;

static bool t3dGpuRegionCacheReadCookControl( const eastl::array<t3dControlCookSource_t, 9> &rSources,
											  const int nRegionSize, const int iX, const int iZ, uint32_t &nControl ) {
	int region_dx = 0;
	int region_dz = 0;
	int local_x = iX;
	int local_z = iZ;
	if ( local_x < 0 ) {
		region_dx = -1;
		local_x += nRegionSize;
	} else if ( local_x >= nRegionSize ) {
		region_dx = 1;
		local_x -= nRegionSize;
	}
	if ( local_z < 0 ) {
		region_dz = -1;
		local_z += nRegionSize;
	} else if ( local_z >= nRegionSize ) {
		region_dz = 1;
		local_z -= nRegionSize;
	}
	const t3dControlCookSource_t &source = rSources[( size_t )( ( region_dz + 1 ) * 3 + ( region_dx + 1 ) )];
	if ( !source.m_bPresent ) {
		return false;
	}
	if ( source.m_bImplicit ) {
		nControl = s_nRegionDefaultControl;
		return true;
	}
	const size_t offset = ( size_t )( local_z * nRegionSize + local_x ) * sizeof( uint32_t );
	std::memcpy( &nControl, source.m_bytes.ptr() + offset, sizeof( uint32_t ) );
	return true;
}

static uint32_t t3dGpuRegionCacheReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint32_t t3dGpuRegionCacheMaterialMaskFromControlImage( const Ref<Image> &rControl ) {
	if ( rControl.is_null() || rControl->is_empty() || rControl->get_format() != Image::FORMAT_RF ) {
		return 1u;
	}
	const PackedByteArray bytes = rControl->get_data();
	const uint8_t *const pRead = bytes.ptr();
	if ( !pRead || bytes.size() < 4 ) {
		return 1u;
	}
	uint32_t mask = 0u;
	const size_t texels = ( size_t )( bytes.size() ) / 4u;
	for ( size_t index = 0; index < texels; ++index ) {
		const uint32_t control = t3dGpuRegionCacheReadU32Unaligned( pRead, index * 4u );
		mask |= 1u << ( uint32_t )( t3dGetBase( control ) & 31u );
		mask |= 1u << ( uint32_t )( t3dGetOverlay( control ) & 31u );
	}
	return mask != 0u ? mask : 1u;
}

EA_FORCE_INLINE static uint8_t t3dGpuRegionCacheColorToByte( const float fValue ) {
	return ( uint8_t )( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

static int t3dGpuRegionCacheMaterialIntParam( Terrain3DMaterial *pMaterial, const char *pszName, const int nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int )( value );
}

EA_FORCE_INLINE static size_t t3dGpuRegionCacheGpuImageBytes( const Ref<Image> &rImage ) {
	return rImage.is_valid() ? ( size_t )( rImage->get_data().size() ) : 0u;
}

class Terrain3DGpuRegionCacheLastScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;

public:
	Terrain3DGpuRegionCacheLastScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &nElapsedUsec;
		Time *time = Time::get_singleton();
		m_nStartedUsec = time ? time->get_ticks_usec() : 0u;
	}
	~Terrain3DGpuRegionCacheLastScopeTimer( void ) {
		if ( !m_pElapsedUsec ) {
			return;
		}
		Time *time = Time::get_singleton();
		const uint64_t finished = time ? time->get_ticks_usec() : m_nStartedUsec;
		*m_pElapsedUsec = finished >= m_nStartedUsec ? finished - m_nStartedUsec : 0u;
	}
};

class Terrain3DGpuRegionCacheProfileScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;
	const uint64_t *m_pRequestUsec = nullptr;
	uint64_t *m_pEndToEndUsec = nullptr;

public:
	EA_FORCE_INLINE Terrain3DGpuRegionCacheProfileScopeTimer( uint64_t &nElapsedUsec, const uint64_t &nRequestUsec,
															  uint64_t &nEndToEndUsec ) :
		m_pElapsedUsec( &nElapsedUsec ), m_pRequestUsec( &nRequestUsec ), m_pEndToEndUsec( &nEndToEndUsec ) {
		Time *time = Time::get_singleton();
		m_nStartedUsec = time ? time->get_ticks_usec() : 0u;
	}
	~Terrain3DGpuRegionCacheProfileScopeTimer( void ) {
		Time *time = Time::get_singleton();
		const uint64_t finished = time ? time->get_ticks_usec() : m_nStartedUsec;
		if ( m_pElapsedUsec ) {
			*m_pElapsedUsec = MAX( *m_pElapsedUsec, finished - m_nStartedUsec );
		}
		if ( m_pEndToEndUsec && m_pRequestUsec && *m_pRequestUsec > 0u && finished >= *m_pRequestUsec ) {
			*m_pEndToEndUsec = MAX( *m_pEndToEndUsec, finished - *m_pRequestUsec );
		}
	}
};

Terrain3DGpuRegionCache::Terrain3DGpuRegionCache( Terrain3DRegionRepository &rRepository,
												  Terrain3DSamplingStorage &rSamplingStorage,
												  Terrain3DMapEditService &rMapEditService,
												  Terrain3DRuntimePageStore &rRuntimePageStore,
												  Terrain3DCpuRegionCache &rCpuRegionCache ) :
	m_regionRepository( rRepository ), m_samplingStorage( rSamplingStorage ), m_mapEditService( rMapEditService ),
	m_runtimePageStore( rRuntimePageStore ), m_cpuRegionCache( rCpuRegionCache ) {
	std::fill_n( m_szGpuRegionLocations, s_nGpuCacheMaxLayers, s_v2iMax );
	std::fill_n( m_szGpuRegionPriorityLocations, s_nGpuCacheMaxLayers, s_v2iMax );
}

void Terrain3DGpuRegionCache::setDefaultMaps( const Ref<Image> &rHeight, const Ref<Image> &rControl,
											  const Ref<Image> &rColor, const Ref<Image> &rDerived,
											  const bool bControlImplicit, const bool bColorImplicit ) {
	const bool layoutChanged = bControlImplicit != m_bControlMapsImplicit || bColorImplicit != m_bColorMapsImplicit;
	m_defaultHeightMap = rHeight;
	m_defaultControlMap = rControl;
	m_defaultColorMap = rColor;
	m_defaultDerivedMap = rDerived;
	m_bControlMapsImplicit = bControlImplicit;
	m_bColorMapsImplicit = bColorImplicit;
	if ( layoutChanged && m_bGpuRegionCacheEnabled ) {
		reset( true );
	}
}

Ref<Image> Terrain3DGpuRegionCache::getRegionMapOrDefault( const Terrain3DRegion *pRegion,
														   const t3dMapType_t eMapType ) const {
	if ( pRegion && !pRegion->isDeleted() ) {
		Ref<Image> pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
		}
	}
	switch ( eMapType ) {
		case TYPE_HEIGHT:
			return m_defaultHeightMap;
		case TYPE_CONTROL:
			return m_defaultControlMap;
		case TYPE_COLOR:
			return m_defaultColorMap;
		default:
			return Ref<Image>();
	}
}

Ref<Image> Terrain3DGpuRegionCache::makeGpuColorImage( const Ref<Image> &rImage ) const {
	Ref<Image> pSource = rImage.is_valid() ? rImage : m_defaultColorMap;
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	if ( divisor <= 1 && pSource->has_mipmaps() ) {
		return pSource;
	}
	Ref<Image> pGpuImage;
	pGpuImage.instantiate();
	pGpuImage->copy_from( pSource );
	// Resizing an image that already owns a mip chain and then generating mips again
	// can carry stale lower levels through rehydration. Rebuild from the base level
	// exactly once so the GPU page always contains the canonical chain for its size.
	if ( pGpuImage->has_mipmaps() ) {
		pGpuImage->clear_mipmaps();
	}
	if ( divisor > 1 ) {
		pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ), MAX( 1, pSource->get_height() / divisor ),
						   Image::INTERPOLATE_BILINEAR );
	}
	if ( pGpuImage->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pGpuImage;
}

Ref<Image> Terrain3DGpuRegionCache::makeGpuDerivedImage( const Ref<Image> &rImage ) const {
	Ref<Image> pSource = rImage.is_valid() ? rImage : m_defaultDerivedMap;
	if ( pSource.is_null() ) {
		return Ref<Image>();
	}
	const int divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	if ( divisor <= 1 ) {
		return pSource;
	}
	Ref<Image> pGpuImage;
	pGpuImage.instantiate();
	pGpuImage->copy_from( pSource );
	pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ), MAX( 1, pSource->get_height() / divisor ),
					   Image::INTERPOLATE_BILINEAR );
	return pGpuImage;
}

bool Terrain3DGpuRegionCache::refreshMiddleCompositeSources( const uint64_t nMaterialSignature,
		const int nWidth,
		const int nHeight ) {
	if ( nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	const int64_t scratch_bytes_64 = ( int64_t )( nWidth ) * nHeight * 4;
	if ( scratch_bytes_64 <= 0 || scratch_bytes_64 > INT32_MAX ) {
		return false;
	}
	const int scratch_bytes = ( int )( scratch_bytes_64 );
	const int scratch_layers = CLAMP( MAX( 1, m_nGpuRegionPhysicalLayerCount ), 1, s_nGpuCacheMaxLayers );
	const bool source_changed = nMaterialSignature != m_nMiddleCompositeMaterialSignature;
	if ( source_changed ) {
		m_middleCompositeMaterialColors.clear();
		m_middleCompositeMaterialScales.clear();
		for ( Ref<Image> &pImage : m_middleCompositeMaterialImages ) {
			pImage.unref();
		}
		const Ref<Terrain3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
		if ( pAssets.is_valid() ) {
			m_middleCompositeMaterialColors = pAssets->getTextureColors();
			m_middleCompositeMaterialScales = pAssets->getTextureUvScales();
			const int texture_count = MIN( pAssets->getTextureCount(), s_nMaxTextureAssets );
			for ( int material = 0; material < texture_count; ++material ) {
				const Ref<Terrain3DTextureAsset> pAsset = pAssets->getTextureAsset( material );
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
				m_middleCompositeMaterialImages[( size_t )( material )] = pMaterialImage;
			}
		}
		m_nMiddleCompositeMaterialSignature = nMaterialSignature;
	}
	if ( scratch_bytes != m_nMiddleCompositeScratchBytes || scratch_layers != m_nMiddleCompositeScratchLayerCount ) {
		for ( int layer = 0; layer < scratch_layers; ++layer ) {
			m_middleCompositeScratch[( size_t )( layer )].resize( scratch_bytes );
			if ( m_middleCompositeScratch[( size_t )( layer )].size() != scratch_bytes ) {
				return false;
			}
		}
		m_nMiddleCompositeScratchBytes = scratch_bytes;
		m_nMiddleCompositeScratchLayerCount = scratch_layers;
	}
	return true;
}

Ref<Image> Terrain3DGpuRegionCache::makeMiddleCompositeImage( const Vector2i &rLocation,
		const Ref<Image> &rControl,
		const Ref<Image> &rColor,
		const int nScratchSlot ) {
	if ( rControl.is_null() || rColor.is_null() || rControl->get_format() != Image::FORMAT_RF ||
			rColor->get_format() != Image::FORMAT_RGBA8 ) {
		return Ref<Image>();
	}
	const int width = rColor->get_width();
	const int height = rColor->get_height();
	if ( width <= 0 || height <= 0 ) {
		return Ref<Image>();
	}
	const PackedByteArray control_bytes = rControl->get_data();
	const PackedByteArray color_bytes = rColor->get_data();
	if ( control_bytes.size() < ( int64_t )( rControl->get_width() ) * rControl->get_height() * 4 ||
			color_bytes.size() < ( int64_t )( width ) * height * 4 ) {
		return Ref<Image>();
	}
	const int scratch_slot = CLAMP( nScratchSlot, 0, MAX( 0, m_nMiddleCompositeScratchLayerCount - 1 ) );
	if ( scratch_slot >= m_nMiddleCompositeScratchLayerCount ) {
		return Ref<Image>();
	}
	PackedByteArray &output = m_middleCompositeScratch[( size_t )( scratch_slot )];
	if ( output.size() != ( int64_t )( width ) * height * 4 ) {
		return Ref<Image>();
	}
	uint8_t *const pWrite = output.ptrw();
	const uint8_t *const pControl = control_bytes.ptr();
	const uint8_t *const pColor = color_bytes.ptr();
	for ( int y = 0; y < height; ++y ) {
		const int control_y = MIN( rControl->get_height() - 1, y * rControl->get_height() / height );
		for ( int x = 0; x < width; ++x ) {
			const int control_x = MIN( rControl->get_width() - 1, x * rControl->get_width() / width );
			const uint32_t control = t3dGpuRegionCacheReadU32Unaligned(
					pControl, ( size_t )( control_y * rControl->get_width() + control_x ) * 4u );
			const int base = ( int )( t3dGetBase( control ) );
			const int overlay = ( int )( t3dGetOverlay( control ) );
			const Color base_color = base < m_middleCompositeMaterialColors.size()
					? m_middleCompositeMaterialColors[base]
					: s_colorWhite;
			const Color overlay_color = overlay < m_middleCompositeMaterialColors.size()
					? m_middleCompositeMaterialColors[overlay]
					: base_color;
			const float control_scale = 0.9f - ( float )( ( ( t3dGetUvScale( control ) + 3u ) % 8u ) + 1u ) * 0.1f;
			const float angle = ( float )( t3dGetUvRotation( control ) ) * Math_TAU / 16.0f;
			const float cs = Math::cos( angle );
			const float sn = Math::sin( angle );
			const float local_x =
					( ( float )( x ) + 0.5f ) * ( float )( m_regionRepository.getRegionSize() ) / ( float )( width );
			const float local_y =
					( ( float )( y ) + 0.5f ) * ( float )( m_regionRepository.getRegionSize() ) / ( float )( height );
			const float world_x = ( ( float )( rLocation.x * m_regionRepository.getRegionSize() ) + local_x ) *
					( float )( m_regionRepository.getVertexSpacing() );
			const float world_y = ( ( float )( rLocation.y * m_regionRepository.getRegionSize() ) + local_y ) *
					( float )( m_regionRepository.getVertexSpacing() );
			auto sample_material = [&]( const int material_id, const Color &rTint ) -> Color {
				if ( material_id < 0 || material_id >= s_nMaxTextureAssets ||
						m_middleCompositeMaterialImages[( size_t )( material_id )].is_null() ) {
					return rTint;
				}
				const Ref<Image> &pImage = m_middleCompositeMaterialImages[( size_t )( material_id )];
				const float asset_scale = material_id < m_middleCompositeMaterialScales.size()
						? m_middleCompositeMaterialScales[material_id]
						: 0.1f;
				const float source_u = ( cs * world_x + sn * world_y ) * control_scale * asset_scale;
				const float source_v = ( cs * world_y - sn * world_x ) * control_scale * asset_scale;
				const float wrapped_u = source_u - Math::floor( source_u );
				const float wrapped_v = source_v - Math::floor( source_v );
				const int source_x = CLAMP( ( int )( wrapped_u * pImage->get_width() ), 0, pImage->get_width() - 1 );
				const int source_y = CLAMP( ( int )( wrapped_v * pImage->get_height() ), 0, pImage->get_height() - 1 );
				return pImage->get_pixel( source_x, source_y ) * rTint;
			};
			const Color base_sample = sample_material( base, base_color );
			const Color overlay_sample = sample_material( overlay, overlay_color );
			float blend = ( float )( t3dGetBlend( control ) ) / 255.0f;
			blend = CLAMP( ( blend - 0.12f ) / 0.76f, 0.0f, 1.0f );
			blend = blend * blend * ( 3.0f - 2.0f * blend );
			const Color material_color = base_sample.lerp( overlay_sample, blend );
			const size_t offset = ( size_t )( y * width + x ) * 4u;
			const float authored_r = ( float )( pColor[offset] ) / 255.0f;
			const float authored_g = ( float )( pColor[offset + 1u] ) / 255.0f;
			const float authored_b = ( float )( pColor[offset + 2u] ) / 255.0f;
			pWrite[offset] = t3dGpuRegionCacheColorToByte( authored_r * material_color.r );
			pWrite[offset + 1u] = t3dGpuRegionCacheColorToByte( authored_g * material_color.g );
			pWrite[offset + 2u] = t3dGpuRegionCacheColorToByte( authored_b * material_color.b );
			pWrite[offset + 3u] = t3dIsHole( control )
					? 0u
					: ( uint8_t )( 0x80u | ( ( uint32_t )( pColor[offset + 3u] ) * 127u + 127u ) / 255u );
		}
	}
	Ref<Image> pComposite = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, output );
	if ( pComposite.is_null() || pComposite->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pComposite;
}

Ref<Image> Terrain3DGpuRegionCache::makeTerrainShadowMaskImage( const Terrain3DRegion *const pRegion ) const {
	const int region_size = m_regionRepository.getRegionSize();
	if ( region_size <= 0 ) {
		return Ref<Image>();
	}
	Ref<Image> image;
	if ( pRegion && !pRegion->isDeleted() ) {
		const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
		if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
			const Ref<Resource> resource =
				ResourceLoader::get_singleton()->load( lighting->getTerrainShadowMaskPath() );
			image = resource;
		}
	}
	if ( image.is_null() || image->is_empty() ) {
		return Terrain3DUtil::getFilledImage( V2I( region_size ), Color( 1.0, 1.0, 1.0, 1.0 ), true, Image::FORMAT_L8 );
	}
	image = image->duplicate();
	if ( image->is_compressed() && image->decompress() != OK ) {
		return Ref<Image>();
	}
	if ( image->has_mipmaps() ) {
		image->clear_mipmaps();
	}
	if ( image->get_format() != Image::FORMAT_L8 ) {
		image->convert( Image::FORMAT_L8 );
	}
	if ( image->get_width() != region_size || image->get_height() != region_size ) {
		image->resize( region_size, region_size, Image::INTERPOLATE_BILINEAR );
	}
	if ( image->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return image;
}

Ref<Image> Terrain3DGpuRegionCache::makeGpuRenderControlImage( const Vector2i &vLocation ) const {
	eastl::array<t3dControlCookSource_t, 9> sources;
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = vLocation + Vector2i( dx, dz );
			const int source_index = ( dz + 1 ) * 3 + ( dx + 1 );
			t3dControlCookSource_t &source = sources[( size_t )( source_index )];
			const Terrain3DRegion *pRegion = m_regionRepository.getRegionPtr( location );
			Ref<Terrain3DRegion> pLoadedSource;
			const bool bNeedsSourceLoad = !pRegion || pRegion->isDeleted() ||
										  ( pRegion->hasChannel( TYPE_CONTROL ) && pRegion->getControlMap().is_null() );
			if ( bNeedsSourceLoad && m_cpuRegionCache.loadSource( location, pLoadedSource ) ) {
				pRegion = pLoadedSource.ptr();
			}
			if ( !pRegion || pRegion->isDeleted() ) {
				continue;
			}
			source.m_bPresent = true;
			if ( !pRegion->hasChannel( TYPE_CONTROL ) || pRegion->getControlMap().is_null() ) {
				source.m_bImplicit = true;
				continue;
			}
			const Ref<Image> pControl = pRegion->getControlMap();
			if ( pControl->get_width() != m_regionRepository.getRegionSize() ||
				 pControl->get_height() != m_regionRepository.getRegionSize() ||
				 pControl->get_format() != Image::FORMAT_RF || pControl->has_mipmaps() ) {
				return Ref<Image>();
			}
			source.m_bytes = pControl->get_data();
			if ( source.m_bytes.size() != m_regionRepository.getRegionSize() * m_regionRepository.getRegionSize() *
											  ( int )( sizeof( uint32_t ) ) ) {
				return Ref<Image>();
			}
			source.m_bImplicit = false;
		}
	}

	PackedByteArray output;
	output.resize( m_regionRepository.getRegionSize() * m_regionRepository.getRegionSize() *
				   ( int )( sizeof( uint32_t ) ) );
	uint8_t *const pWrite = output.ptrw();
	for ( int z = 0; z < m_regionRepository.getRegionSize(); ++z ) {
		for ( int x = 0; x < m_regionRepository.getRegionSize(); ++x ) {
			uint32_t center = s_nRegionDefaultControl;
			if ( !t3dGpuRegionCacheReadCookControl( sources, m_regionRepository.getRegionSize(), x, z, center ) ) {
				std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
							 &center, sizeof( uint32_t ) );
				continue;
			}
			if ( t3dIsAuto( center ) || t3dIsHole( center ) ) {
				std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
							 &center, sizeof( uint32_t ) );
				continue;
			}

			eastl::array<uint64_t, s_nMaxTextureAssets> material_weights {};
			for ( int dz = -s_nRenderControlBlendRadius; dz <= s_nRenderControlBlendRadius; ++dz ) {
				for ( int dx = -s_nRenderControlBlendRadius; dx <= s_nRenderControlBlendRadius; ++dx ) {
					uint32_t pControl = s_nRegionDefaultControl;
					if ( !t3dGpuRegionCacheReadCookControl( sources, m_regionRepository.getRegionSize(), x + dx, z + dz,
															pControl ) ||
						 t3dIsAuto( pControl ) || t3dIsHole( pControl ) ) {
						continue;
					}
					const uint64_t spatial_weight =
						( uint64_t )( s_szRenderControlKernel[dx + s_nRenderControlBlendRadius] *
									  s_szRenderControlKernel[dz + s_nRenderControlBlendRadius] );
					const uint8_t base = t3dGetBase( pControl );
					const uint8_t overlay = t3dGetOverlay( pControl );
					const uint8_t blend = t3dGetBlend( pControl );
					if ( base == overlay ) {
						material_weights[( size_t )( base )] += spatial_weight * 255u;
					} else {
						material_weights[( size_t )( base )] += spatial_weight * ( uint64_t )( 255u - blend );
						material_weights[( size_t )( overlay )] += spatial_weight * ( uint64_t )( blend );
					}
				}
			}

			int strongest = -1;
			int second = -1;
			for ( int material = 0; material < s_nMaxTextureAssets; ++material ) {
				if ( strongest < 0 ||
					 material_weights[( size_t )( material )] > material_weights[( size_t )( strongest )] ) {
					second = strongest;
					strongest = material;
				} else if ( second < 0 ||
							material_weights[( size_t )( material )] > material_weights[( size_t )( second )] ) {
					second = material;
				}
			}
			if ( strongest < 0 || material_weights[( size_t )( strongest )] == 0u ) {
				std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
							 &center, sizeof( uint32_t ) );
				continue;
			}
			if ( second < 0 || material_weights[( size_t )( second )] == 0u ) {
				second = strongest;
			}

			// Runtime controls use a canonical pair order so neighboring fragments that
			// contain the same two materials address the same texture-array layers in the
			// same order. This materially improves wave/warp texture-cache coherence.
			// The runtime blend shader is pair-order symmetric, so no authored semantic
			// role is lost by sorting the compiled representation.
			int base = strongest;
			int overlay = second;
			if ( base > overlay ) {
				std::swap( base, overlay );
			}
			const uint64_t base_weight = material_weights[( size_t )( base )];
			const uint64_t overlay_weight = material_weights[( size_t )( overlay )];
			const uint64_t pair_weight = base_weight + overlay_weight;
			uint8_t blend = 0u;
			if ( base != overlay && pair_weight > 0u ) {
				blend = ( uint8_t )( CLAMP( ( int )( ( overlay_weight * 255u + pair_weight / 2u ) / pair_weight ), 0,
											255 ) );
			}
			const uint32_t cooked = ( center & ~s_nRenderControlMaterialMask ) | t3dEncBase( ( uint8_t )( base ) ) |
									t3dEncOverlay( ( uint8_t )( overlay ) ) | t3dEncBlend( blend );
			std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
						 &cooked, sizeof( uint32_t ) );
		}
	}
	return Image::create_from_data( m_regionRepository.getRegionSize(), m_regionRepository.getRegionSize(), false,
									Image::FORMAT_RF, output );
}

int Terrain3DGpuRegionCache::calculateRequiredCorePagesForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int radius = t3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(), detail_lods,
																   ( double )( m_regionRepository.getVertexSpacing() ),
																   m_regionRepository.getRegionSize() );
	const int diameter = radius * 2 + 1;
	return MIN( diameter * diameter, m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
}

int Terrain3DGpuRegionCache::calculateAuthoringGpuRegionBudgetMb( void ) const {
	// m_nGpuRegionCacheBudgetMb is already the effective allocation produced by
	// Terrain3D's aggregate memory-budget coordinator. Authoring may use more
	// physical layers than runtime, but it must fit those layers inside that same
	// coordinated GPU allocation instead of manufacturing extra editor memory.
	return CLAMP( MAX( 1, m_nGpuRegionCacheBudgetMb ), s_nGpuRegionCacheMinBudgetMb, s_nGpuRegionCacheMaxBudgetMb );
}

int Terrain3DGpuRegionCache::calculateCapacityForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int required_core = calculateRequiredCorePagesForLods( detail_lods );
	// Allocation is budget-driven. The unified region policy determines which pages
	// are desirable; legacy GPU guard/prefetch controls must not define a second
	// spatial footprint or reduce the usable physical cache.
	const int requested = MIN( m_regionRepository.getActiveRegionCount(), s_nGpuCacheRuntimeMaxLayers );
	const int capacity = t3dGpuCacheCalculateCapacity(
		m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(), m_nGpuRegionCacheBudgetMb,
		s_nGpuCacheRuntimeMaxLayers, requested, m_bControlMapsImplicit, !isColorChannelEnabled(),
		isDerivedChannelEnabled(), m_pTerrain->getGpuRegionColorResolutionDivisor(),
		m_pTerrain->getGpuRegionDerivedResolutionDivisor(), m_pTerrain->getGpuFarQualityDivisor(),
		m_nGpuRegionSparePages, m_pTerrain->getGpuFarHeightAtlasSide() );
	return MAX( required_core > 0 ? 1 : 0, capacity );
}

int Terrain3DGpuRegionCache::calculateGpuRegionCacheCapacity( void ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		const int page_limit = CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, s_nGpuCacheEditorMaxLayers );
		return t3dGpuCacheCalculateCapacity(
			m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
			calculateAuthoringGpuRegionBudgetMb(), page_limit,
			MIN( m_regionRepository.getActiveRegionCount(), page_limit ), m_bControlMapsImplicit,
			!isColorChannelEnabled(), isDerivedChannelEnabled(), m_pTerrain->getGpuRegionColorResolutionDivisor(),
			m_pTerrain->getGpuRegionDerivedResolutionDivisor(), m_pTerrain->getGpuFarQualityDivisor(),
			m_nGpuRegionSparePages, m_pTerrain->getGpuFarHeightAtlasSide() );
	}
	const int allocation_request = MIN( m_regionRepository.getActiveRegionCount(), s_nGpuCacheRuntimeMaxLayers );
	return t3dGpuCacheCalculateCapacity(
		m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(), m_nGpuRegionCacheBudgetMb,
		s_nGpuCacheRuntimeMaxLayers, allocation_request, m_bControlMapsImplicit, !isColorChannelEnabled(),
		isDerivedChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1, m_nGpuRegionSparePages,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide );
}

int Terrain3DGpuRegionCache::calculateGpuRegionPhysicalLayers( const int nMappedCapacity ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		return t3dGpuCacheCalculatePhysicalLayers(
			nMappedCapacity, m_nGpuRegionSparePages,
			CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, s_nGpuCacheEditorMaxLayers ) );
	}
	return t3dGpuCacheCalculatePhysicalLayers( nMappedCapacity, m_nGpuRegionSparePages, s_nGpuCacheRuntimeMaxLayers );
}

int Terrain3DGpuRegionCache::calculateGpuRegionRequiredRadius( void ) const {
	if ( !m_pTerrain ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	int detail_lods = m_pTerrain->getGpuRegionDetailLods();
	if ( detail_lods <= 0 || detail_lods > total_lods ) {
		detail_lods = total_lods;
	}
	return t3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(), detail_lods,
													   ( double )( m_regionRepository.getVertexSpacing() ),
													   m_regionRepository.getRegionSize() );
}

int Terrain3DGpuRegionCache::calculateGpuRegionRequestedPages( void ) const {
	if ( !m_pTerrain || m_regionRepository.getActiveRegionCount() <= 0 ) {
		return 0;
	}
	if ( m_pTerrain->isEditorAuthoringMode() ) {
		return MIN( m_regionRepository.getActiveRegionCount(), m_pTerrain->getEditorGpuRegionCachePages() );
	}
	const Dictionary streaming = m_pTerrain->getRegionStreamingStats();
	const int active = ( int )( streaming["active"] );
	const int hot = ( int )( streaming["hot"] );
	const int predicted = ( int )( streaming["predicted_promotions"] );
	return MIN( m_regionRepository.getActiveRegionCount(), active + hot + predicted );
}

size_t Terrain3DGpuRegionCache::estimateLayerBytes( void ) const {
	const size_t base = t3dGpuCacheEstimateLayerBytes(
		m_regionRepository.getRegionSize(), m_bControlMapsImplicit, !isColorChannelEnabled(), isDerivedChannelEnabled(),
		m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
	const size_t shadow_texels = t3dGpuCacheTexelCount( m_regionRepository.getRegionSize(), 1 );
	return base + ( shadow_texels * 4u ) / 3u;
}

bool Terrain3DGpuRegionCache::isColorChannelEnabled( void ) const {
	if ( m_bColorMapsImplicit ) {
		return false;
	}
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	const bool near_needed =
		m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		( pMaterial->getNearAuthoredColorContribution() > 0.0f || pMaterial->getNearWetnessContribution() > 0.0f );
	const bool middle_needed =
		m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		( pMaterial->getMiddleAuthoredColorContribution() > 0.0f || pMaterial->getMiddleWetnessContribution() > 0.0f );
	return near_needed || middle_needed;
}

bool Terrain3DGpuRegionCache::isDerivedChannelEnabled( void ) const {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	return ( m_pTerrain->getRuntimeNearShaderLods() > 0 &&
			 pMaterial->getNearTerrainNormalQuality() == TERRAIN_NORMAL_QUALITY_DETAILED ) ||
		   ( m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
			 pMaterial->getMiddleTerrainNormalQuality() == TERRAIN_NORMAL_QUALITY_DETAILED );
}

size_t Terrain3DGpuRegionCache::estimateSharedBytes( void ) const {
	return t3dGpuCacheEstimateSharedBytes(
		m_regionRepository.getRegionSize(), m_bControlMapsImplicit, !isColorChannelEnabled(),
		m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide );
}

int Terrain3DGpuRegionCache::estimateGpuRegionUploadCalls( const Terrain3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0;
	}
	const uint8_t source_flags = ( uint8_t )( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	int calls = 2; // Height and terrain shadowmask are always published.
	if ( isDerivedChannelEnabled() ) {
		++calls;
	}
	if ( !m_bControlMapsImplicit &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t )( REGION_CHANNEL_CONTROL ) ) ) {
		++calls;
	}
	if ( isColorChannelEnabled() &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t )( REGION_CHANNEL_COLOR ) ) ) {
		++calls;
	}
	return calls;
}

size_t Terrain3DGpuRegionCache::estimateGpuRegionUploadBytes( const Terrain3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0u;
	}
	const size_t full_texels = t3dGpuCacheTexelCount( m_regionRepository.getRegionSize(), 1 );
	const size_t derived_texels = t3dGpuCacheTexelCount(
		m_regionRepository.getRegionSize(), m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
	if ( full_texels == 0u || derived_texels == 0u ) {
		return 0u;
	}
	size_t bytes = full_texels * 2u + ( full_texels * 4u ) / 3u;
	if ( isDerivedChannelEnabled() ) {
		bytes += derived_texels * 2u;
	}
	const uint8_t source_flags = ( uint8_t )( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	if ( !m_bControlMapsImplicit &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t )( REGION_CHANNEL_CONTROL ) ) ) {
		bytes += full_texels * 4u;
	}
	if ( isColorChannelEnabled() &&
		 t3dGpuCacheOptionalChannelNeedsUpdate( source_flags, layer_flags, ( uint8_t )( REGION_CHANNEL_COLOR ) ) ) {
		bytes += t3dGpuCacheColorBytes( m_regionRepository.getRegionSize(),
										m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 );
	}
	return bytes;
}

int Terrain3DGpuRegionCache::getGpuRegionTextureUpdateCallsLastTick( void ) const {
	return m_nGpuRegionHeightUpdatesLastTick + m_nGpuRegionControlUpdatesLastTick + m_nGpuRegionColorUpdatesLastTick +
		   m_nGpuRegionDerivedUpdatesLastTick + m_nGpuRegionTerrainShadowUpdatesLastTick +
		   m_nGpuRegionPageTableUpdatesLastTick;
}

void Terrain3DGpuRegionCache::reset( const bool bClearTextures ) {
	std::fill_n( m_szGpuRegionMap, s_nMaxRegionSlots, ( uint16_t )( 0 ) );
	std::fill_n( m_szGpuRegionLocations, s_nGpuCacheMaxLayers, s_v2iMax );
	std::fill_n( m_szGpuRegionLastUseTick, s_nGpuCacheMaxLayers, ( uint64_t )( 0 ) );
	std::fill_n( m_szGpuRegionLoadedTick, s_nGpuCacheMaxLayers, ( uint64_t )( 0 ) );
	std::fill_n( m_szGpuRegionRetireAfterFrame, s_nGpuCacheMaxLayers, ( uint64_t )( 0 ) );
	std::fill_n( m_szGpuRegionSlotStates, s_nGpuCacheMaxLayers, ( uint8_t )( TERRAIN3D_GPU_SLOT_FREE ) );
	std::fill_n( m_szGpuRegionPrefetchOrigin, s_nGpuCacheMaxLayers, ( uint8_t )( 0 ) );
	std::fill_n( m_szGpuRegionLayerChannelFlags, s_nGpuCacheMaxLayers, ( uint8_t )( 0 ) );
	std::fill_n( m_szGpuRegionMaterialMasks, s_nGpuCacheMaxLayers, ( uint32_t )( 0 ) );
	std::memset( m_szGpuRegionVictimReserved, 0, sizeof( m_szGpuRegionVictimReserved ) );
	for ( int layer = 0; layer < s_nGpuCacheMaxLayers; ++layer ) {
		clearPendingGpuRegionUpload( layer );
	}
	m_nGpuRegionPendingUnmappedReservations = 0;
	std::fill_n( m_szGpuRegionPriorityLocations, s_nGpuCacheMaxLayers, s_v2iMax );
	std::fill_n( m_szGpuRegionPriorityClasses, s_nGpuCacheMaxLayers, ( uint8_t )( 0 ) );
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_gpuTerrainShadowMasks.clear();
	m_nGpuRegionCacheCapacity = 0;
	m_nGpuRegionPhysicalLayerCount = 0;
	m_nGpuRegionSpareLayerCount = 0;
	m_nGpuRegionResidentCount = 0;
	m_nGpuRegionRetiredCount = 0;
	m_nGpuRegionDesiredLastTick = 0;
	m_nGpuRegionRequiredLastTick = 0;
	m_nGpuRegionGuardLastTick = 0;
	m_nGpuRegionPrefetchLastTick = 0;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	m_nGpuRegionRequestedPages = 0;
	m_nGpuRegionRequiredRadius = 0;
	m_nGpuRegionUploadsLastTick = 0;
	m_nGpuRegionUploadsSinceReset = 0u;
	m_nGpuRegionEvictionsLastTick = 0;
	m_nGpuRegionCookedHitsLastTick = 0;
	m_nGpuRegionCookedMissesLastTick = 0;
	m_nGpuRegionRuntimePreparesLastTick = 0;
	m_nGpuRegionReusedLayersLastTick = 0;
	m_nGpuRegionPageTableUpdatesLastTick = 0;
	m_nGpuRegionHeightUpdatesLastTick = 0;
	m_nGpuRegionControlUpdatesLastTick = 0;
	m_nGpuRegionColorUpdatesLastTick = 0;
	m_nGpuRegionDerivedUpdatesLastTick = 0;
	m_nGpuRegionTerrainShadowUpdatesLastTick = 0;
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	m_nGpuRegionPreparedPagesLastTick = 0;
	m_nGpuRegionPrepareWorkersLastTick = 0;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionPrefetchUsedTotal = 0u;
	m_nGpuRegionPrefetchDiscardedTotal = 0u;
	m_nGpuRegionResidentTicksTotal = 0u;
	m_nGpuRegionDepartedPagesTotal = 0u;
	m_nGpuRegionTick = 0u;
	m_nGpuRegionLastPublicationTick = 0u;
	m_gpuRegionCacheCenter = s_v2iMax;
	m_gpuRegionPredictionDirection = s_v2iZero;
	m_nGpuRegionStreamingGenerationLast = 0u;
	m_nGpuRegionPriorityCount = 0;
	m_nGpuRegionServiceUsecLast = 0u;
	m_nGpuPreparePublicationUsecLast = 0u;
	m_nGpuPrepareEndToEndUsecLast = 0u;
	m_bPageTableChangedThisTick = false;
	std::memset( m_szGpuRegionRepublishPending, 0, sizeof( m_szGpuRegionRepublishPending ) );
	m_nGpuRegionRepublishPendingCount = 0;
	m_nGpuRegionRepublishCursor = 0;
	m_bGpuRegionCacheTexturesReady = false;
	m_bGpuRegionCacheSelectionDirty = true;
	notePageTableChanged();
	if ( bClearTextures ) {
		m_generatedHeightMaps.clear();
		m_generatedControlMaps.clear();
		m_generatedColorMaps.clear();
		m_generatedDerivedMaps.clear();
		m_generatedMiddleCompositeMaps.clear();
		m_generatedTerrainShadowMasks.clear();
	}
}

bool Terrain3DGpuRegionCache::validatePublishedMappings( void ) const {
	for ( int map_index = 0; map_index < s_nMaxRegionSlots; ++map_index ) {
		const int layer = ( int )( m_szGpuRegionMap[map_index] ) - 1;
		if ( layer < 0 ) {
			continue;
		}
		if ( layer >= m_nGpuRegionPhysicalLayerCount || m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
			 Terrain3DRegionRepository::getRegionMapIndex( m_szGpuRegionLocations[layer] ) != map_index ) {
			return false;
		}
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( m_szGpuRegionLocations[layer] );
		if ( map_index < 0 || m_szGpuRegionMap[map_index] != ( uint16_t )( layer + 1 ) ) {
			return false;
		}
	}
	return true;
}

void Terrain3DGpuRegionCache::notePageTableChanged( void ) {
	m_bPageTableChangedThisTick = true;
	++m_nPhysicalPageTableGeneration;
	if ( m_nPhysicalPageTableGeneration == 0u ) {
		m_nPhysicalPageTableGeneration = 1u;
	}
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	if ( !validatePublishedMappings() ) {
		LOG( ERROR, "GPU physical page table ownership invariant failed" );
	}
#endif
}

bool Terrain3DGpuRegionCache::rebuildTextures( void ) {
	const int capacity = calculateGpuRegionCacheCapacity();
	LOG( INFO, "[gpu-cache] Rebuild requested: mapped_capacity=", capacity,
		 "; region_size=", m_regionRepository.getRegionSize(), "; implicit_control=", m_bControlMapsImplicit,
		 "; implicit_color=", m_bColorMapsImplicit );
	if ( capacity <= 0 || m_defaultHeightMap.is_null() || m_defaultControlMap.is_null() ||
		 m_defaultColorMap.is_null() || m_defaultDerivedMap.is_null() ) {
		return false;
	}

	const Vector2i previous_center = m_gpuRegionCacheCenter;
	reset( true );
	m_nGpuRegionCacheCapacity = capacity;
	m_nGpuRegionPhysicalLayerCount = calculateGpuRegionPhysicalLayers( capacity );
	m_nGpuRegionSpareLayerCount = MAX( 0, m_nGpuRegionPhysicalLayerCount - capacity );
	m_gpuRegionCacheCenter = previous_center;
	m_nGpuRegionColorDivisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	m_nGpuRegionDerivedDivisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const bool bColorChannelEnabled = isColorChannelEnabled();
	const bool bDerivedChannelEnabled = isDerivedChannelEnabled();
	m_bGpuRegionColorChannelAllocated = bColorChannelEnabled;
	m_bGpuRegionDerivedChannelAllocated = bDerivedChannelEnabled;

	m_gpuHeightMaps.resize( m_nGpuRegionPhysicalLayerCount );
	m_gpuDerivedMaps.resize( bDerivedChannelEnabled ? m_nGpuRegionPhysicalLayerCount : 1 );
	const int control_layers = m_bControlMapsImplicit ? 1 : m_nGpuRegionPhysicalLayerCount;
	const int color_layers = bColorChannelEnabled ? m_nGpuRegionPhysicalLayerCount : 1;
	m_gpuControlMaps.resize( control_layers );
	m_gpuColorMaps.resize( color_layers );
	m_gpuMiddleCompositeMaps.resize( m_nGpuRegionPhysicalLayerCount );
	m_gpuTerrainShadowMasks.resize( m_nGpuRegionPhysicalLayerCount );
	const Ref<Image> pDefaultGpuColor = makeGpuColorImage( m_defaultColorMap );
	const Ref<Image> pDefaultGpuDerived = makeGpuDerivedImage( m_defaultDerivedMap );
	if ( pDefaultGpuColor.is_null() || pDefaultGpuDerived.is_null() ) {
		return false;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuHeightMaps[layer] = m_defaultHeightMap;
	}
	for ( int layer = 0; layer < m_gpuDerivedMaps.size(); ++layer ) {
		m_gpuDerivedMaps[layer] = pDefaultGpuDerived;
	}
	for ( int layer = 0; layer < control_layers; ++layer ) {
		m_gpuControlMaps[layer] = m_defaultControlMap;
	}
	for ( int layer = 0; layer < color_layers; ++layer ) {
		m_gpuColorMaps[layer] = pDefaultGpuColor;
	}
	const Ref<Terrain3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	const uint64_t material_signature = pAssets.is_valid() ? pAssets->getRuntimeMaterialSourceSignature() : 0u;
	if ( !refreshMiddleCompositeSources(
			 material_signature, pDefaultGpuColor->get_width(), pDefaultGpuColor->get_height() ) ) {
		return false;
	}
	const Ref<Image> pDefaultMiddleComposite =
			makeMiddleCompositeImage( s_v2iZero, m_defaultControlMap, pDefaultGpuColor, 0 );
	if ( pDefaultMiddleComposite.is_null() ) {
		return false;
	}
	const Ref<Image> pDefaultTerrainShadow = makeTerrainShadowMaskImage( nullptr );
	if ( pDefaultTerrainShadow.is_null() ) {
		return false;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuMiddleCompositeMaps[layer] = pDefaultMiddleComposite;
		m_gpuTerrainShadowMasks[layer] = pDefaultTerrainShadow;
	}

	const bool bHeightOk = m_generatedHeightMaps.create( m_gpuHeightMaps, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	const bool bControlOk = m_generatedControlMaps.create( m_gpuControlMaps, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	const bool bColorOk = m_generatedColorMaps.create( m_gpuColorMaps, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
	const bool bDerivedOk = m_generatedDerivedMaps.create( m_gpuDerivedMaps, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	const bool bMiddleCompositeOk =
		m_generatedMiddleCompositeMaps.create( m_gpuMiddleCompositeMaps, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
	const bool bTerrainShadowOk =
		m_generatedTerrainShadowMasks.create( m_gpuTerrainShadowMasks, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_gpuTerrainShadowMasks.clear();
	m_bGpuRegionCacheTexturesReady =
		bHeightOk && bControlOk && bColorOk && bDerivedOk && bMiddleCompositeOk && bTerrainShadowOk;
	if ( !m_bGpuRegionCacheTexturesReady ) {
		LOG( ERROR, "Failed to create bounded GPU region cache with ", capacity, " physical layers" );
		return false;
	}
	m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	const size_t budget_bytes = ( size_t )( m_nGpuRegionCacheBudgetMb )*s_nGpuCacheMib;
	const size_t layer_bytes = estimateLayerBytes();
	const size_t shared_bytes = estimateSharedBytes();
	const size_t estimated_bytes = shared_bytes + layer_bytes * ( size_t )( m_nGpuRegionPhysicalLayerCount );
	if ( estimated_bytes > budget_bytes ) {
		LOG( WARN, "One terrain region requires approximately ", layer_bytes, " GPU bytes and shared channels require ",
			 shared_bytes, " bytes. The selected cache exceeds the configured budget of ", budget_bytes,
			 " bytes because one physical layer is the minimum legal configuration." );
	}
	m_nGpuRegionRequestedPages = calculateGpuRegionRequestedPages();
	m_nGpuRegionRequiredRadius = calculateGpuRegionRequiredRadius();
	LOG( INFO, "[gpu-cache] Created bounded GPU region cache: ", capacity, " mapped pages, ",
		 m_nGpuRegionSpareLayerCount, " spare/retirement pages, ", m_nGpuRegionPhysicalLayerCount,
		 " physical layers, requested ", m_nGpuRegionRequestedPages, ", required radius ", m_nGpuRegionRequiredRadius,
		 ", estimated bytes ", estimated_bytes, ", budget ", budget_bytes );
	return true;
}

int Terrain3DGpuRegionCache::findGpuRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int layer = ( int )( m_szGpuRegionMap[map_index] ) - 1;
	if ( layer < 0 || layer >= m_nGpuRegionPhysicalLayerCount ||
		 m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED || m_szGpuRegionLocations[layer] != vLocation ) {
		return -1;
	}
	return layer;
}

int Terrain3DGpuRegionCache::findPendingGpuRegionUpload( const Vector2i &vLocation ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		const t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[layer];
		if ( pending.m_nGeneration != 0u && pending.m_location == vLocation ) {
			return layer;
		}
	}
	return -1;
}

int Terrain3DGpuRegionCache::findFreeGpuRegionSlot( void ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionLocations[layer] == s_v2iMax && m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE &&
			 m_szGpuRegionVictimReserved[layer] == 0u ) {
			return layer;
		}
	}
	return -1;
}

void Terrain3DGpuRegionCache::clearPendingGpuRegionUpload( const int nLayer ) {
	if ( nLayer < 0 || nLayer >= s_nGpuCacheMaxLayers ) {
		return;
	}
	t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[nLayer];
	pending.m_nGeneration = 0u;
	pending.m_nLastUseTick = 0u;
	pending.m_nLoadedTick = 0u;
	pending.m_nVictimLastUseTick = 0u;
	pending.m_location = s_v2iMax;
	pending.m_victimLocation = s_v2iMax;
	pending.m_nVictimLayer = -1;
	pending.m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_NONE;
	pending.m_nPrefetchOrigin = 0u;
}

void Terrain3DGpuRegionCache::recordGpuRegionDeparture( const int nLayer, const bool bCountUnusedPrefetch ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return;
	}
	if ( m_szGpuRegionLoadedTick[nLayer] > 0u && m_nGpuRegionTick >= m_szGpuRegionLoadedTick[nLayer] ) {
		m_nGpuRegionResidentTicksTotal += m_nGpuRegionTick - m_szGpuRegionLoadedTick[nLayer];
		++m_nGpuRegionDepartedPagesTotal;
	}
	if ( bCountUnusedPrefetch && m_szGpuRegionPrefetchOrigin[nLayer] != 0u ) {
		++m_nGpuRegionPrefetchDiscardedTotal;
	}
	m_szGpuRegionLoadedTick[nLayer] = 0u;
	m_szGpuRegionPrefetchOrigin[nLayer] = 0u;
}

void Terrain3DGpuRegionCache::retireGpuRegionSlot( const int nLayer ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ||
		 m_szGpuRegionSlotStates[nLayer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
		return;
	}
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_RETIRED;
	m_szGpuRegionRetireAfterFrame[nLayer] = std::numeric_limits<uint64_t>::max();
	++m_nGpuRegionRetiredCount;
}

void Terrain3DGpuRegionCache::armPendingRetirements( void ) {
	Engine *const pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t )( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	const uint64_t release_frame = current_frame + ( uint64_t )( MAX( 1, m_nGpuRegionRetireFrames ) );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED &&
			 m_szGpuRegionRetireAfterFrame[layer] == std::numeric_limits<uint64_t>::max() ) {
			m_szGpuRegionRetireAfterFrame[layer] = release_frame;
		}
	}
}

void Terrain3DGpuRegionCache::reclaimRetiredGpuRegionSlots( void ) {
	Engine *const pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t )( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_RETIRED ||
			 m_szGpuRegionRetireAfterFrame[layer] == std::numeric_limits<uint64_t>::max() ||
			 current_frame < m_szGpuRegionRetireAfterFrame[layer] ) {
			continue;
		}
		m_szGpuRegionLocations[layer] = s_v2iMax;
		m_szGpuRegionLastUseTick[layer] = 0u;
		m_szGpuRegionLoadedTick[layer] = 0u;
		m_szGpuRegionPrefetchOrigin[layer] = 0u;
		m_szGpuRegionMaterialMasks[layer] = 0u;
		m_szGpuRegionRetireAfterFrame[layer] = 0u;
		m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
		m_nGpuRegionRetiredCount = MAX( 0, m_nGpuRegionRetiredCount - 1 );
		++m_nGpuRegionReusedLayersLastTick;
	}
}

void Terrain3DGpuRegionCache::finalizePendingGpuRegionUploads( void ) {
	Terrain3DRdTextureManager *const pTextureManager = Terrain3DRdTextureManager::getSingleton();
	if ( !pTextureManager ) {
		return;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[layer];
		if ( pending.m_nGeneration == 0u ) {
			continue;
		}
		const t3dRdTextureBatchState_t batch_state =
			pTextureManager->getTrackedTextureLayerBatchState( ( uint32_t )( layer ), pending.m_nGeneration );
		if ( batch_state == T3D_RD_TEXTURE_BATCH_PENDING ) {
			continue;
		}
		const uint8_t publication = pending.m_nPublication;
		const int victim_layer = pending.m_nVictimLayer;
		if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_NEW && victim_layer < 0 ) {
			m_nGpuRegionPendingUnmappedReservations = MAX( 0, m_nGpuRegionPendingUnmappedReservations - 1 );
		}
		if ( victim_layer >= 0 && victim_layer < m_nGpuRegionPhysicalLayerCount ) {
			m_szGpuRegionVictimReserved[victim_layer] = 0u;
		}
		if ( batch_state != T3D_RD_TEXTURE_BATCH_SUCCEEDED ) {
			// A render-thread batch can fail after one or more preceding texture updates
			// succeeded. Keep the layer unpublished and force both optional channels to
			// be explicitly overwritten on its next full-page use.
			m_szGpuRegionLayerChannelFlags[layer] |=
				( uint8_t )( REGION_CHANNEL_CONTROL | REGION_CHANNEL_COLOR );
			m_szGpuRegionLocations[layer] = s_v2iMax;
			m_szGpuRegionLastUseTick[layer] = 0u;
			m_szGpuRegionLoadedTick[layer] = 0u;
			m_szGpuRegionPrefetchOrigin[layer] = 0u;
			m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
			if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH && victim_layer >= 0 &&
				 victim_layer < m_nGpuRegionPhysicalLayerCount &&
				 m_szGpuRegionSlotStates[victim_layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
				 m_szGpuRegionLocations[victim_layer] == pending.m_location ) {
				queueRepublish( pending.m_location );
			}
			clearPendingGpuRegionUpload( layer );
			m_bGpuRegionCacheSelectionDirty = true;
			continue;
		}

		bool bPublished = false;
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( pending.m_location );
		Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( pending.m_location );
		if ( map_index >= 0 && pRegion && !pRegion->isDeleted() ) {
			if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_NEW ) {
				bool bCapacityAvailable = false;
				if ( m_szGpuRegionMap[map_index] == 0u && victim_layer >= 0 &&
					 victim_layer < m_nGpuRegionPhysicalLayerCount &&
					 m_szGpuRegionSlotStates[victim_layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
					 m_szGpuRegionLocations[victim_layer] == pending.m_victimLocation &&
					 m_szGpuRegionLastUseTick[victim_layer] == pending.m_nVictimLastUseTick ) {
					const int victim_map_index =
						Terrain3DRegionRepository::getRegionMapIndex( pending.m_victimLocation );
					if ( victim_map_index >= 0 &&
						 m_szGpuRegionMap[victim_map_index] == ( uint16_t )( victim_layer + 1 ) &&
						 m_szGpuRegionRepublishPending[victim_map_index] == 0u ) {
						m_szGpuRegionMap[victim_map_index] = 0u;
						recordGpuRegionDeparture( victim_layer, true );
						retireGpuRegionSlot( victim_layer );
						++m_nGpuRegionEvictionsLastTick;
						bCapacityAvailable = true;
					}
				}
				if ( m_szGpuRegionMap[map_index] == 0u && !bCapacityAvailable &&
					 m_nGpuRegionResidentCount < m_nGpuRegionCacheCapacity ) {
					++m_nGpuRegionResidentCount;
					bCapacityAvailable = true;
				}
				if ( bCapacityAvailable ) {
					m_szGpuRegionLocations[layer] = pending.m_location;
					m_szGpuRegionLastUseTick[layer] = pending.m_nLastUseTick;
					m_szGpuRegionLoadedTick[layer] = pending.m_nLoadedTick;
					m_szGpuRegionPrefetchOrigin[layer] = pending.m_nPrefetchOrigin;
					m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_MAPPED;
					m_szGpuRegionMap[map_index] = ( uint16_t )( layer + 1 );
					bPublished = true;
				}
			} else if ( publication == TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH && victim_layer >= 0 &&
					   victim_layer < m_nGpuRegionPhysicalLayerCount &&
					   m_szGpuRegionSlotStates[victim_layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
					   m_szGpuRegionLocations[victim_layer] == pending.m_location &&
					   m_szGpuRegionMap[map_index] == ( uint16_t )( victim_layer + 1 ) ) {
				m_szGpuRegionLocations[layer] = pending.m_location;
				m_szGpuRegionLastUseTick[layer] = pending.m_nLastUseTick;
				m_szGpuRegionLoadedTick[layer] = pending.m_nLoadedTick;
				m_szGpuRegionPrefetchOrigin[layer] = pending.m_nPrefetchOrigin;
				m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_MAPPED;
				m_szGpuRegionMap[map_index] = ( uint16_t )( layer + 1 );
				m_szGpuRegionLoadedTick[victim_layer] = 0u;
				m_szGpuRegionPrefetchOrigin[victim_layer] = 0u;
				retireGpuRegionSlot( victim_layer );
				bPublished = true;
			}
		}
		if ( bPublished ) {
			++m_nGpuRegionUploadsSinceReset;
			m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
			notePageTableChanged();
		} else {
			m_szGpuRegionLocations[layer] = s_v2iMax;
			m_szGpuRegionLastUseTick[layer] = 0u;
			m_szGpuRegionLoadedTick[layer] = 0u;
			m_szGpuRegionPrefetchOrigin[layer] = 0u;
			m_szGpuRegionSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
			m_bGpuRegionCacheSelectionDirty = true;
		}
		clearPendingGpuRegionUpload( layer );
	}
}

void Terrain3DGpuRegionCache::invalidate( const Vector2i &vLocation ) {
	if ( !m_bGpuRegionCacheEnabled ) {
		return;
	}
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		return;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szGpuRegionMap[map_index] = 0u;
	}
	recordGpuRegionDeparture( layer, true );
	retireGpuRegionSlot( layer );
	m_nGpuRegionResidentCount = MAX( 0, m_nGpuRegionResidentCount - 1 );
	m_bGpuRegionCacheSelectionDirty = true;
	notePageTableChanged();
}

void Terrain3DGpuRegionCache::queueRepublish( const Vector2i &vLocation ) {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 || map_index >= s_nMaxRegionSlots ) {
		return;
	}
	if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
		m_szGpuRegionRepublishPending[map_index] = 1u;
		++m_nGpuRegionRepublishPendingCount;
	}
	// Force the ordinary per-frame residency tick to revisit deferred authoring
	// publications even when the camera and streaming generation are stationary.
	m_bGpuRegionCacheSelectionDirty = true;
}

bool Terrain3DGpuRegionCache::republish( const Vector2i &vLocation, Terrain3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	const int old_layer = findGpuRegionSlot( vLocation );
	if ( old_layer < 0 ) {
		return false;
	}
	if ( findPendingGpuRegionUpload( vLocation ) >= 0 ) {
		return false;
	}
	if ( m_szGpuRegionVictimReserved[old_layer] != 0u ) {
		queueRepublish( vLocation );
		return false;
	}
	reclaimRetiredGpuRegionSlots();
	const int staging_layer = findFreeGpuRegionSlot();
	if ( staging_layer < 0 ) {
		// Authoring can commit several brush stamps before the renderer has advanced
		// enough frames to recycle retired layers. Coalesce the location and publish
		// the newest page later instead of dropping the edit or synchronizing the GPU.
		queueRepublish( vLocation );
		return false;
	}
	// A staging slot starts from default channel contents, so publish the entire
	// page even when only one source channel changed. The old mapped page remains
	// valid until the replacement contains a coherent cross-channel snapshot.
	if ( !uploadGpuRegionLayer( staging_layer, vLocation, REGION_TYPE_MAX ) ) {
		return false;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_FREE;
		clearPendingGpuRegionUpload( staging_layer );
		return false;
	}
	t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[staging_layer];
	if ( pending.m_nGeneration != 0u ) {
		pending.m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_REPUBLISH;
		pending.m_nVictimLayer = old_layer;
		pending.m_victimLocation = vLocation;
		pending.m_nLastUseTick = m_szGpuRegionLastUseTick[old_layer];
		pending.m_nLoadedTick = m_szGpuRegionLoadedTick[old_layer];
		pending.m_nPrefetchOrigin = m_szGpuRegionPrefetchOrigin[old_layer];
		m_szGpuRegionVictimReserved[old_layer] = 1u;
		if ( m_szGpuRegionRepublishPending[map_index] != 0u ) {
			m_szGpuRegionRepublishPending[map_index] = 0u;
			m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
		}
		++m_nGpuRegionUploadsLastTick;
		return false;
	}
	m_szGpuRegionLocations[staging_layer] = vLocation;
	m_szGpuRegionLastUseTick[staging_layer] = m_nGpuRegionTick;
	m_szGpuRegionLoadedTick[staging_layer] = m_szGpuRegionLoadedTick[old_layer];
	m_szGpuRegionPrefetchOrigin[staging_layer] = m_szGpuRegionPrefetchOrigin[old_layer];
	m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
	m_szGpuRegionMap[map_index] = ( uint16_t )( staging_layer + 1 );
	if ( m_szGpuRegionRepublishPending[map_index] != 0u ) {
		m_szGpuRegionRepublishPending[map_index] = 0u;
		m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
	}
	m_szGpuRegionLoadedTick[old_layer] = 0u;
	m_szGpuRegionPrefetchOrigin[old_layer] = 0u;
	retireGpuRegionSlot( old_layer );
	notePageTableChanged();
	return true;
}

int Terrain3DGpuRegionCache::processPendingGpuRegionRepublishes( const int nLimit ) {
	if ( m_nGpuRegionRepublishPendingCount <= 0 || nLimit <= 0 || m_nGpuRegionPhysicalLayerCount <= 0 ) {
		return 0;
	}
	int published = 0;
	int consumed = 0;
	int visited = 0;
	int cursor = m_nGpuRegionRepublishCursor;
	while ( visited < s_nMaxRegionSlots && m_nGpuRegionRepublishPendingCount > 0 && consumed < nLimit ) {
		const int map_index = cursor;
		cursor = ( cursor + 1 ) % s_nMaxRegionSlots;
		++visited;
		if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
			continue;
		}
		const int map_x = map_index % s_nActiveMaxRegionMapSize;
		const int map_y = map_index / s_nActiveMaxRegionMapSize;
		const Vector2i location( map_x - s_nActiveMaxRegionMapSize / 2, map_y - s_nActiveMaxRegionMapSize / 2 );
		if ( findGpuRegionSlot( location ) < 0 ) {
			// An evicted page will be uploaded from current source data if it becomes
			// desirable again, so no separate republish remains necessary.
			m_szGpuRegionRepublishPending[map_index] = 0u;
			m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
			continue;
		}
		if ( findFreeGpuRegionSlot() < 0 ) {
			break;
		}
		if ( republish( location ) ) {
			++published;
			++consumed;
		} else if ( findPendingGpuRegionUpload( location ) >= 0 ) {
			++consumed;
		}
	}
	m_nGpuRegionRepublishCursor = cursor;
	return published;
}

int Terrain3DGpuRegionCache::collectDesiredGpuRegions( const t3dResidencyFrameState_t &rFrame, const Vector2i &rCenter,
													   Vector2i *pLocations, const int nCapacity,
													   t3dGpuCacheSelectionStats_t &rStats ) {
	rStats = t3dGpuCacheSelectionStats_s();
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, s_nGpuCacheMaxLayers );
	int output_count = 0;
	uint8_t selected_map[s_nMaxRegionSlots] = {};
	auto append_location = [&]( const Vector2i &location, const t3dRegionStreamingState_t wanted ) {
		if ( output_count >= capacity ) {
			return false;
		}
		if ( !m_regionRepository.isLocationValid( location ) ||
			 m_regionRepository.getRegionPtr( location ) == nullptr || rFrame.streamingState( location ) != wanted ) {
			return true;
		}
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u ) {
			return true;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
		return output_count < capacity;
	};
	auto append_state = [&]( const t3dRegionStreamingState_t wanted ) {
		t3dGpuRegionCacheForEachSquareRing(
			rCenter, m_regionRepository.getRegionMapSize(),
			[&]( const Vector2i &location ) { return append_location( location, wanted ); } );
	};

	// Full surface pages are mandatory only for ACTIVE. HOT geometry is served by
	// the independent height cache below, so surface pressure can never move vertices.
	append_state( TERRAIN3D_REGION_STREAMING_ACTIVE );
	rStats.m_nRequired = output_count;
	append_state( TERRAIN3D_REGION_STREAMING_HOT );
	rStats.m_nGuard = output_count - rStats.m_nRequired;
	const int prefetch_begin = output_count;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount() && output_count < capacity; ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		const int map_index = location == s_v2iMax ? -1 : Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || m_regionRepository.getRegionPtr( location ) == nullptr ||
			 !rFrame.isPrefetch( location ) ) {
			continue;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
	}
	rStats.m_nPrefetch = output_count - prefetch_begin;
	rStats.m_nWritten = output_count;
	return output_count;
}

int Terrain3DGpuRegionCache::fillMaterialPriorityIds( uint8_t *const pIds, const int nCapacity ) const {
	if ( !pIds || nCapacity <= 0 ) {
		return 0;
	}
	uint32_t emitted_mask = 0u;
	int count = 0;
	auto append_mask = [&]( uint32_t mask ) {
		while ( mask != 0u && count < nCapacity ) {
			int id = 0;
			while ( id < s_nMaxTextureAssets && ( mask & ( 1u << ( uint32_t )( id ) ) ) == 0u ) {
				++id;
			}
			if ( id >= s_nMaxTextureAssets ) {
				break;
			}
			const uint32_t bit = 1u << ( uint32_t )( id );
			mask &= ~bit;
			if ( ( emitted_mask & bit ) != 0u ) {
				continue;
			}
			pIds[count++] = ( uint8_t )( id );
			emitted_mask |= bit;
		}
	};

	// Logical material zero is the stable fallback physical page.
	append_mask( 1u );
	for ( int priority = 0; priority < m_nGpuRegionPriorityCount && count < nCapacity; ++priority ) {
		const int layer = findGpuRegionSlot( m_szGpuRegionPriorityLocations[priority] );
		if ( layer >= 0 ) {
			append_mask( m_szGpuRegionMaterialMasks[layer] );
		}
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && count < nCapacity; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED ) {
			append_mask( m_szGpuRegionMaterialMasks[layer] );
		}
	}
	return count;
}

bool Terrain3DGpuRegionCache::uploadGpuRegionLayer( const int nLayer, const Vector2i &vLocation,
													const t3dMapType_t eMapType ) {
	uint64_t prepared_request_usec = 0u;
	const Terrain3DGpuRegionCacheProfileScopeTimer publicationTimer(
		m_nGpuPreparePublicationUsecLast, prepared_request_usec, m_nGpuPrepareEndToEndUsecLast );
	if ( !m_bGpuRegionCacheEnabled || !m_bGpuRegionCacheTexturesReady || nLayer < 0 ||
		 nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	const uint8_t source_channel_flags = ( uint8_t )( pRegion->getChannelFlags() );
	const uint8_t previous_channel_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	const bool bUploadHeight = eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX;
	// Optional channels are updated only when either the incoming page or the
	// previous physical contents are explicit. Reusing default-on-default layers
	// requires no texture update, while explicit-to-default transitions still clear
	// stale physical contents before publication.
	const bool bUploadControl = ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX ) &&
								!m_bControlMapsImplicit &&
								t3dGpuCacheOptionalChannelNeedsUpdate( source_channel_flags, previous_channel_flags,
																	   ( uint8_t )( REGION_CHANNEL_CONTROL ) );
	const bool bUploadColor = ( eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) && isColorChannelEnabled() &&
							  t3dGpuCacheOptionalChannelNeedsUpdate( source_channel_flags, previous_channel_flags,
																	 ( uint8_t )( REGION_CHANNEL_COLOR ) );
	Ref<Image> pHeight;
	Ref<Image> pDerived;
	Ref<Image> pControl;
	Ref<Image> pColor;
	Ref<Image> pMiddleComposite;
	Ref<Image> pTerrainShadowMask;
	const uint8_t previous_state = m_szGpuRegionSlotStates[nLayer];
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_STAGING;

	bool bCookedPage = false;
	uint32_t cooked_material_mask = 1u;
	if ( eMapType == REGION_TYPE_MAX && pRegion->runtimeMapsEvictableInternal() && m_pTerrain &&
		 m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		bool bPreparedAttempted = false;
		bCookedPage =
			m_runtimePageStore.takeGpuPage( vLocation, pHeight, pControl, pColor, pDerived, pMiddleComposite,
											cooked_material_mask, bPreparedAttempted, &prepared_request_usec );
		if ( !bPreparedAttempted && m_runtimePageStore.isPending( vLocation ) ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PREPARE_PENDING;
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( !bPreparedAttempted ) {
			// Normal residency updates schedule cooked-page work before publication.
			// An unscheduled evictable page is deferred rather than synchronously reading
			// storage on the physics thread.
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_NO_READY_PAGE;
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( bCookedPage ) {
			++m_nGpuRegionCookedHitsLastTick;
		} else {
			++m_nGpuRegionCookedMissesLastTick;
		}
	}
	if ( eMapType == REGION_TYPE_MAX && pRegion->runtimeMapsEvictableInternal() && m_pTerrain &&
		 m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		m_runtimePageStore.takeTerrainShadowMask( vLocation, pTerrainShadowMask );
	}
	if ( !bCookedPage ) {
		if ( m_cpuRegionCache.isEnabled() && !m_cpuRegionCache.ensureResident( vLocation, true ) ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		++m_nGpuRegionRuntimePreparesLastTick;
	}

	// Resolve the complete publication set before overwriting a physical layer.
	// The page table continues to reference the previous logical region until all
	// source images are known to be valid.
	const bool bDerivedChannelEnabled = isDerivedChannelEnabled();
	if ( bUploadHeight && !bCookedPage ) {
		pHeight = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
		if ( pHeight.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( bDerivedChannelEnabled ) {
			pDerived = pRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() ) {
				++m_nGpuRegionRuntimeDerivedBuildsLastTick;
				m_mapEditService.regenerateDerivedMaps( { vLocation }, AABB(), m_pCompute );
				pDerived = pRegion->getDerivedNormalSlopeMap();
			}
			if ( pDerived.is_null() ) {
				pDerived = m_defaultDerivedMap;
			}
			if ( pDerived.is_null() ) {
				m_szGpuRegionSlotStates[nLayer] = previous_state;
				return false;
			}
			pDerived = makeGpuDerivedImage( pDerived );
			if ( pDerived.is_null() ) {
				m_szGpuRegionSlotStates[nLayer] = previous_state;
				return false;
			}
		}
	}
	if ( bUploadControl ) {
		if ( pControl.is_null() ) {
			pControl = makeGpuRenderControlImage( vLocation );
		}
		if ( pControl.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}
	if ( bUploadColor ) {
		if ( pColor.is_null() ) {
			const Ref<Image> pSourceColor = getRegionMapOrDefault( pRegion, TYPE_COLOR );
			if ( pSourceColor.is_valid() &&
				 ( !pSourceColor->has_mipmaps() ||
				   ( m_pTerrain && m_pTerrain->getGpuRegionColorResolutionDivisor() > 1 ) ) ) {
				++m_nGpuRegionRuntimeColorMipsLastTick;
			}
			pColor = makeGpuColorImage( pSourceColor );
		}
		if ( pColor.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}
	if ( eMapType == REGION_TYPE_MAX && pTerrainShadowMask.is_null() ) {
		pTerrainShadowMask = makeTerrainShadowMaskImage( pRegion );
		if ( pTerrainShadowMask.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	} else if ( pTerrainShadowMask.is_valid() ) {
		// Cooked pages preserve source dimensions; normalize every physical layer
		// to the terrain region grid before publication.
		if ( pTerrainShadowMask->get_width() != m_regionRepository.getRegionSize() ||
			 pTerrainShadowMask->get_height() != m_regionRepository.getRegionSize() ) {
			pTerrainShadowMask = pTerrainShadowMask->duplicate();
			if ( pTerrainShadowMask->has_mipmaps() ) {
				pTerrainShadowMask->clear_mipmaps();
			}
			pTerrainShadowMask->resize( m_regionRepository.getRegionSize(), m_regionRepository.getRegionSize(),
										Image::INTERPOLATE_BILINEAR );
			pTerrainShadowMask->convert( Image::FORMAT_L8 );
			if ( pTerrainShadowMask->generate_mipmaps() != OK ) {
				m_szGpuRegionSlotStates[nLayer] = previous_state;
				return false;
			}
		}
	}
	if ( eMapType == REGION_TYPE_MAX && pMiddleComposite.is_null() ) {
		Ref<Image> pCompositeControl = pControl;
		if ( pCompositeControl.is_null() ) {
			pCompositeControl = makeGpuRenderControlImage( vLocation );
		}
		Ref<Image> pCompositeColor = pColor;
		if ( pCompositeColor.is_null() ) {
			pCompositeColor = makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) );
		}
		pMiddleComposite = makeMiddleCompositeImage( vLocation, pCompositeControl, pCompositeColor, nLayer );
		if ( pMiddleComposite.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}
	uint8_t published_flags = previous_channel_flags;
	if ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX ) {
		published_flags &= ( uint8_t )( ~REGION_CHANNEL_CONTROL );
		published_flags |= source_channel_flags & ( uint8_t )( REGION_CHANNEL_CONTROL );
	}
	if ( eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) {
		published_flags &= ( uint8_t )( ~REGION_CHANNEL_COLOR );
		published_flags |= source_channel_flags & ( uint8_t )( REGION_CHANNEL_COLOR );
	}
	uint32_t material_mask =
		bCookedPage ? cooked_material_mask
					: ( pControl.is_valid() ? t3dGpuRegionCacheMaterialMaskFromControlImage( pControl ) : 1u );
	if ( m_pTerrain ) {
		const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
			const int auto_base =
				CLAMP( t3dGpuRegionCacheMaterialIntParam( pMaterial.ptr(), "auto_base_texture", 0 ), 0, 31 );
			const int auto_overlay =
				CLAMP( t3dGpuRegionCacheMaterialIntParam( pMaterial.ptr(), "auto_overlay_texture", 1 ), 0, 31 );
			material_mask |= 1u << ( uint32_t )( auto_base );
			material_mask |= 1u << ( uint32_t )( auto_overlay );
		}
	}

	if ( eMapType == REGION_TYPE_MAX && !Terrain3DRdContext::isOnRenderThread() ) {
		Terrain3DRdTextureManager *const pTextureManager = Terrain3DRdTextureManager::getSingleton();
		if ( !pTextureManager || m_szGpuRegionPendingUploads[nLayer].m_nGeneration != 0u ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		t3dRdTextureBatchUpdate_t updates[6];
		uint32_t update_count = 0u;
		auto append_update = [&]( Terrain3DGeneratedTexture &rTexture, const Ref<Image> &rImage ) -> bool {
			if ( !rTexture.canUpdateLayer( rImage, nLayer ) || update_count >= 6u ) {
				return false;
			}
			updates[update_count].m_hTexture = rTexture.getRid();
			updates[update_count].m_data = rImage->get_data();
			if ( updates[update_count].m_data.is_empty() ) {
				updates[update_count].m_hTexture = RID();
				return false;
			}
			++update_count;
			return true;
		};
		bool bBatchReady = append_update( m_generatedHeightMaps, pHeight );
		if ( bBatchReady && bDerivedChannelEnabled ) {
			bBatchReady = append_update( m_generatedDerivedMaps, pDerived );
		}
		if ( bBatchReady && bUploadControl ) {
			bBatchReady = append_update( m_generatedControlMaps, pControl );
		}
		if ( bBatchReady && bUploadColor ) {
			bBatchReady = append_update( m_generatedColorMaps, pColor );
		}
		if ( bBatchReady ) {
			bBatchReady = append_update( m_generatedMiddleCompositeMaps, pMiddleComposite );
		}
		if ( bBatchReady ) {
			bBatchReady = append_update( m_generatedTerrainShadowMasks, pTerrainShadowMask );
		}
		if ( !bBatchReady ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		const uint64_t upload_generation = pTextureManager->nextTrackedTextureBatchGeneration();
		if ( !pTextureManager->submitTrackedTextureLayerBatch( ( uint32_t )( nLayer ), upload_generation,
				 ( uint32_t )( nLayer ), updates, update_count ) ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[nLayer];
		pending.m_nGeneration = upload_generation;
		pending.m_location = vLocation;
		m_generatedHeightMaps.commitLayerImage( pHeight, nLayer );
		++m_nGpuRegionHeightUpdatesLastTick;
		const size_t height_bytes = t3dGpuRegionCacheGpuImageBytes( pHeight );
		m_nGpuRegionHeightBytesLastTick += height_bytes;
		m_nGpuRegionUploadBytesLastTick += height_bytes;
		if ( bDerivedChannelEnabled ) {
			m_generatedDerivedMaps.commitLayerImage( pDerived, nLayer );
			++m_nGpuRegionDerivedUpdatesLastTick;
			const size_t derived_bytes = t3dGpuRegionCacheGpuImageBytes( pDerived );
			m_nGpuRegionDerivedBytesLastTick += derived_bytes;
			m_nGpuRegionUploadBytesLastTick += derived_bytes;
		}
		if ( bUploadControl ) {
			m_generatedControlMaps.commitLayerImage( pControl, nLayer );
			++m_nGpuRegionControlUpdatesLastTick;
			const size_t control_bytes = t3dGpuRegionCacheGpuImageBytes( pControl );
			m_nGpuRegionControlBytesLastTick += control_bytes;
			m_nGpuRegionUploadBytesLastTick += control_bytes;
		}
		if ( bUploadColor ) {
			m_generatedColorMaps.commitLayerImage( pColor, nLayer );
			++m_nGpuRegionColorUpdatesLastTick;
			const size_t color_bytes = t3dGpuRegionCacheGpuImageBytes( pColor );
			m_nGpuRegionColorBytesLastTick += color_bytes;
			m_nGpuRegionUploadBytesLastTick += color_bytes;
		}
		m_generatedMiddleCompositeMaps.commitLayerImage( pMiddleComposite, nLayer );
		m_nGpuRegionUploadBytesLastTick += t3dGpuRegionCacheGpuImageBytes( pMiddleComposite );
		m_generatedTerrainShadowMasks.commitLayerImage( pTerrainShadowMask, nLayer );
		++m_nGpuRegionTerrainShadowUpdatesLastTick;
		m_nGpuRegionUploadBytesLastTick += t3dGpuRegionCacheGpuImageBytes( pTerrainShadowMask );
		m_szGpuRegionLayerChannelFlags[nLayer] = published_flags;
		m_szGpuRegionMaterialMasks[nLayer] = material_mask;
		if ( bDerivedChannelEnabled && !bCookedPage && !IS_EDITOR && pDerived != m_defaultDerivedMap ) {
			pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
		}
		return true;
	}

	bool bUploadOk = true;
	if ( bUploadHeight ) {
		bUploadOk = m_generatedHeightMaps.update( pHeight, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionHeightUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( pHeight );
			m_nGpuRegionHeightBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
		if ( bUploadOk && bDerivedChannelEnabled ) {
			bUploadOk = m_generatedDerivedMaps.update( pDerived, nLayer );
		}
		if ( bUploadOk && bDerivedChannelEnabled ) {
			++m_nGpuRegionDerivedUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( pDerived );
			m_nGpuRegionDerivedBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
		if ( bDerivedChannelEnabled && !bCookedPage && !IS_EDITOR && pDerived != m_defaultDerivedMap ) {
			pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
		}
	}
	if ( bUploadOk && bUploadControl ) {
		bUploadOk = m_generatedControlMaps.update( pControl, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionControlUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( pControl );
			m_nGpuRegionControlBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
	}
	if ( bUploadOk && bUploadColor ) {
		bUploadOk = m_generatedColorMaps.update( pColor, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionColorUpdatesLastTick;
			const size_t bytes = t3dGpuRegionCacheGpuImageBytes( pColor );
			m_nGpuRegionColorBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
	}
	if ( bUploadOk && eMapType == REGION_TYPE_MAX ) {
		bUploadOk = m_generatedMiddleCompositeMaps.update( pMiddleComposite, nLayer );
		if ( bUploadOk ) {
			m_nGpuRegionUploadBytesLastTick += t3dGpuRegionCacheGpuImageBytes( pMiddleComposite );
		}
	}
	if ( bUploadOk && eMapType == REGION_TYPE_MAX ) {
		bUploadOk = m_generatedTerrainShadowMasks.update( pTerrainShadowMask, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionTerrainShadowUpdatesLastTick;
			m_nGpuRegionUploadBytesLastTick += t3dGpuRegionCacheGpuImageBytes( pTerrainShadowMask );
		}
	}
	if ( !bUploadOk ) {
		m_szGpuRegionSlotStates[nLayer] = previous_state;
		return false;
	}
	m_szGpuRegionLayerChannelFlags[nLayer] = published_flags;
	m_szGpuRegionMaterialMasks[nLayer] = material_mask;
	m_szGpuRegionSlotStates[nLayer] =
		m_szGpuRegionLocations[nLayer] == vLocation ? TERRAIN3D_GPU_SLOT_MAPPED : TERRAIN3D_GPU_SLOT_READY;
	return true;
}

bool Terrain3DGpuRegionCache::isRuntimePageWanted( void *const pContext, const Vector2i &rLocation ) {
	const t3dResidencyFrameState_t *const pFrame = static_cast<const t3dResidencyFrameState_t *>( pContext );
	return pFrame &&
		   ( pFrame->streamingState( rLocation ) >= TERRAIN3D_REGION_STREAMING_HOT || pFrame->isPrefetch( rLocation ) );
}

bool Terrain3DGpuRegionCache::serviceRuntimePages( const t3dResidencyFrameState_t &rFrame ) {
	const int color_size = MAX( 1,
			m_regionRepository.getRegionSize() / MAX( 1, rFrame.m_runtimePageConfig.m_nColorDivisor ) );
	if ( !refreshMiddleCompositeSources(
			rFrame.m_runtimePageConfig.m_nMaterialSourceSignature, color_size, color_size ) ) {
		return false;
	}
	t3dRuntimePageServiceState_t state;
	state.m_config = rFrame.m_runtimePageConfig;
	state.m_predictedCenter = rFrame.m_predictedCenter;
	state.m_nStreamingGeneration = rFrame.m_nStreamingGeneration;
	state.m_isWanted = &Terrain3DGpuRegionCache::isRuntimePageWanted;
	state.m_pWantedContext = const_cast<t3dResidencyFrameState_t *>( &rFrame );
	m_runtimePageStore.service( state );
	return true;
}

bool Terrain3DGpuRegionCache::configure( const bool bEnabled, const int nBudgetMb, const int nUploadLayers,
										 const int nUploadBudgetMb, const int nSparePages, const int nRetireFrames ) {
	const bool bOldEnabled = m_bGpuRegionCacheEnabled;
	const int old_budget = m_nGpuRegionCacheBudgetMb;
	const int old_upload_layers = m_nGpuRegionUploadBudgetLayers;
	const int old_upload_budget = m_nGpuRegionUploadBudgetMb;
	const int old_spare_pages = m_nGpuRegionSparePages;
	const int old_retire_frames = m_nGpuRegionRetireFrames;
	m_bGpuRegionCacheEnabled = bEnabled;
	m_nGpuRegionCacheBudgetMb = CLAMP( nBudgetMb, s_nGpuRegionCacheMinBudgetMb, s_nGpuRegionCacheMaxBudgetMb );
	m_nGpuRegionUploadBudgetLayers = CLAMP( nUploadLayers, 1, s_nGpuRegionUploadMaxLayers );
	m_nGpuRegionUploadBudgetMb = CLAMP( nUploadBudgetMb, 1, s_nGpuRegionUploadMaxBudgetMb );
	m_nGpuRegionSparePages = CLAMP( nSparePages, s_nGpuCacheMinSpareLayers, s_nGpuCacheRuntimeMaxLayers - 1 );
	m_nGpuRegionRetireFrames = CLAMP( nRetireFrames, 1, 16 );

	const int requested_pages = calculateGpuRegionRequestedPages();
	const int required_radius = calculateGpuRegionRequiredRadius();
	const int color_divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	const int derived_divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const int required_capacity = m_bGpuRegionCacheEnabled ? calculateGpuRegionCacheCapacity() : 0;
	const int required_physical_layers =
		m_bGpuRegionCacheEnabled ? calculateGpuRegionPhysicalLayers( required_capacity ) : 0;
	const bool color_channel_enabled = isColorChannelEnabled();
	const bool derived_channel_enabled = isDerivedChannelEnabled();
	const bool bLayoutChanged =
		m_bGpuRegionCacheEnabled && m_bGpuRegionCacheTexturesReady &&
		( required_capacity != m_nGpuRegionCacheCapacity || color_divisor != m_nGpuRegionColorDivisor ||
		  derived_divisor != m_nGpuRegionDerivedDivisor || color_channel_enabled != m_bGpuRegionColorChannelAllocated ||
		  derived_channel_enabled != m_bGpuRegionDerivedChannelAllocated ||
		  required_physical_layers != m_nGpuRegionPhysicalLayerCount );
	const bool bSelectionChanged =
		requested_pages != m_nGpuRegionRequestedPages || required_radius != m_nGpuRegionRequiredRadius;
	const bool bStorageChanged = bOldEnabled != m_bGpuRegionCacheEnabled ||
								 ( m_bGpuRegionCacheEnabled && old_budget != m_nGpuRegionCacheBudgetMb ) ||
								 bLayoutChanged;
	if ( !bStorageChanged && !bSelectionChanged && old_upload_layers == m_nGpuRegionUploadBudgetLayers &&
		 old_upload_budget == m_nGpuRegionUploadBudgetMb && old_spare_pages == m_nGpuRegionSparePages &&
		 old_retire_frames == m_nGpuRegionRetireFrames ) {
		return false;
	}
	m_nGpuRegionRequestedPages = requested_pages;
	m_nGpuRegionRequiredRadius = required_radius;
	m_bGpuRegionCacheSelectionDirty = m_bGpuRegionCacheSelectionDirty || bSelectionChanged;
	if ( bStorageChanged && ( bOldEnabled || m_bGpuRegionCacheEnabled ) ) {
		reset( true );
	}
	return bStorageChanged;
}

t3dGpuRegionCacheUpdateResult_t Terrain3DGpuRegionCache::update( const t3dResidencyFrameState_t &rFrame,
																 const bool bForce, const int nEffectiveUploadLayers,
																 Terrain3DCompute *pCompute ) {
	const Terrain3DGpuRegionCacheLastScopeTimer serviceTimer( m_nGpuRegionServiceUsecLast, rFrame.m_bProfilingEnabled );
	m_nGpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_runtimePageStore.resetFrameStats();
	m_pCompute = pCompute;
	m_bPageTableChangedThisTick = false;
	m_nGpuPreparePublicationUsecLast = 0u;
	m_nGpuPrepareEndToEndUsecLast = 0u;
	m_runtimePageStore.poll( false );
	m_nGpuRegionUploadsLastTick = 0;
	m_nGpuRegionEvictionsLastTick = 0;
	m_nGpuRegionCookedHitsLastTick = 0;
	m_nGpuRegionCookedMissesLastTick = 0;
	m_nGpuRegionRuntimePreparesLastTick = 0;
	m_nGpuRegionReusedLayersLastTick = 0;
	m_nGpuRegionPageTableUpdatesLastTick = 0;
	m_nGpuRegionHeightUpdatesLastTick = 0;
	m_nGpuRegionControlUpdatesLastTick = 0;
	m_nGpuRegionColorUpdatesLastTick = 0;
	m_nGpuRegionDerivedUpdatesLastTick = 0;
	m_nGpuRegionTerrainShadowUpdatesLastTick = 0;
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	const t3dRuntimePageStats_t runtime_page_stats = m_runtimePageStore.getStats();
	m_nGpuRegionPreparedPagesLastTick = runtime_page_stats.m_nPreparedPages;
	m_nGpuRegionPrepareWorkersLastTick = runtime_page_stats.m_nPrepareWorkers;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	if ( !m_bGpuRegionCacheEnabled || m_regionRepository.getRegionSize() <= 0 ||
		 m_regionRepository.getVertexSpacing() <= 0.0f ) {
		return t3dGpuRegionCacheUpdateResult_t {};
	}
	++m_nGpuRegionTick;
	finalizePendingGpuRegionUploads();
	reclaimRetiredGpuRegionSlots();
	const uint64_t streaming_generation = rFrame.m_nStreamingGeneration;
	const Vector2i prediction_delta = rFrame.m_predictedCenter - rFrame.m_streamingCenter;
	m_gpuRegionPredictionDirection = Vector2i( SIGN( prediction_delta.x ), SIGN( prediction_delta.y ) );
	if ( !m_bGpuRegionCacheTexturesReady && !rebuildTextures() ) {
		return t3dGpuRegionCacheUpdateResult_t {};
	}
	const Vector2i center = rFrame.m_residencyCenter;
	m_bGpuRegionCameraMovedLastTick = m_gpuRegionCacheCenter != s_v2iMax && center != m_gpuRegionCacheCenter;
	const int runtime_upload_budget = nEffectiveUploadLayers;
	const int effective_upload_limit = MAX( 1, runtime_upload_budget );
	m_nGpuRegionEffectiveUploadLimitLastTick = effective_upload_limit;
	const int deferred_republish_count = processPendingGpuRegionRepublishes( effective_upload_limit );
	if ( deferred_republish_count > 0 ) {
		m_nGpuRegionUploadsLastTick += deferred_republish_count;
		m_nGpuRegionUploadsSinceReset += ( uint64_t )( deferred_republish_count );
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	if ( !bForce && center == m_gpuRegionCacheCenter && !m_bGpuRegionCacheSelectionDirty &&
		 streaming_generation == m_nGpuRegionStreamingGenerationLast ) {
		return t3dGpuRegionCacheUpdateResult_t { m_bPageTableChangedThisTick };
	}

	Vector2i desired[s_nGpuCacheMaxLayers];
	t3dGpuCacheSelectionStats_t selection_stats;
	int desired_count = collectDesiredGpuRegions( rFrame, center, desired, m_nGpuRegionCacheCapacity, selection_stats );
	const int footprint_count = desired_count;
	m_nGpuRegionRequiredLastTick = selection_stats.m_nRequired;
	m_nGpuRegionGuardLastTick = selection_stats.m_nGuard;
	m_nGpuRegionPrefetchLastTick = selection_stats.m_nPrefetch;
	uint8_t desired_map[s_nMaxRegionSlots] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
		const int layer = findGpuRegionSlot( desired[index] );
		if ( layer >= 0 ) {
			m_szGpuRegionLastUseTick[layer] = m_nGpuRegionTick;
			if ( index < selection_stats.m_nRequired && m_szGpuRegionPrefetchOrigin[layer] != 0u ) {
				++m_nGpuRegionPrefetchUsedTotal;
				m_szGpuRegionPrefetchOrigin[layer] = 0u;
			}
		}
	}

	const uint64_t hysteresis_ticks = ( uint64_t )( MAX( 0, rFrame.m_nGpuHysteresisTicks ) );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && desired_count < m_nGpuRegionCacheCapacity;
		  ++layer ) {
		const Vector2i location = m_szGpuRegionLocations[layer];
		if ( location == s_v2iMax || m_szGpuRegionLastUseTick[layer] == 0u ||
			 m_nGpuRegionTick - m_szGpuRegionLastUseTick[layer] > hysteresis_ticks ) {
			continue;
		}
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || desired_map[map_index] != 0u ) {
			continue;
		}
		desired[desired_count++] = location;
		desired_map[map_index] = 1u;
		++m_nGpuRegionHysteresisLastTick;
	}
	m_nGpuRegionDesiredLastTick = desired_count;
	m_nGpuRegionPriorityCount = MIN( desired_count, s_nGpuCacheMaxLayers );
	const int guard_end = selection_stats.m_nRequired + selection_stats.m_nGuard;
	const int priority_prefetch_end = guard_end + selection_stats.m_nPrefetch;
	for ( int index = 0; index < m_nGpuRegionPriorityCount; ++index ) {
		m_szGpuRegionPriorityLocations[index] = desired[index];
		if ( index < selection_stats.m_nRequired ) {
			m_szGpuRegionPriorityClasses[index] = 1u;
		} else if ( index < guard_end ) {
			m_szGpuRegionPriorityClasses[index] = 2u;
		} else if ( index < priority_prefetch_end ) {
			m_szGpuRegionPriorityClasses[index] = 3u;
		} else {
			m_szGpuRegionPriorityClasses[index] = 4u;
		}
	}
	for ( int index = m_nGpuRegionPriorityCount; index < s_nGpuCacheMaxLayers; ++index ) {
		m_szGpuRegionPriorityLocations[index] = s_v2iMax;
		m_szGpuRegionPriorityClasses[index] = 0u;
	}

	if ( rFrame.m_bCookedPagesEnabled ) {
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) >= 0 || findPendingGpuRegionUpload( desired[index] ) >= 0 ) {
				continue;
			}
			Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( desired[index] );
			if ( pRegion && !pRegion->isDeleted() && pRegion->runtimeMapsEvictableInternal() ) {
				const uint8_t priority = index < m_nGpuRegionPriorityCount ? m_szGpuRegionPriorityClasses[index] : 4u;
				uint16_t consumers = RUNTIME_PAGE_CONSUMER_GPU_DETAIL;
				const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
				if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
					consumers |= RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW;
				}
				m_runtimePageStore.request( desired[index], consumers, priority, rFrame.m_bCookedPagesEnabled );
			}
		}
		serviceRuntimePages( rFrame );
	}

	bool bMapChanged = false;
	bool bAllDesiredResident = true;
	const size_t byte_budget = ( size_t )( m_nGpuRegionUploadBudgetMb )*s_nGpuCacheMib;
	const int update_call_budget = MAX( 7, effective_upload_limit * 6 + 1 );
	for ( int desired_index = 0; desired_index < desired_count; ++desired_index ) {
		const Vector2i location = desired[desired_index];
		const int existing_layer = findGpuRegionSlot( location );
		if ( existing_layer >= 0 ) {
			m_szGpuRegionLastUseTick[existing_layer] = m_nGpuRegionTick;
			continue;
		}
		bAllDesiredResident = false;
		if ( findPendingGpuRegionUpload( location ) >= 0 ) {
			continue;
		}
		if ( m_nGpuRegionUploadsLastTick >= effective_upload_limit ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_LAYER_BUDGET;
			break;
		}

		const int target_layer = findFreeGpuRegionSlot();
		if ( target_layer < 0 ) {
			// Spare pages can be temporarily exhausted while old mappings remain
			// retired for in-flight rendering. Continue on a later frame rather than
			// synchronizing or overwriting a page still visible to the renderer.
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_RETIREMENT_CAPACITY;
			break;
		}
		Terrain3DRegion *const pUploadRegion = m_regionRepository.getRegionPtr( location );
		if ( !pUploadRegion || pUploadRegion->isDeleted() ) {
			continue;
		}
		const size_t expected_bytes = estimateGpuRegionUploadBytes( pUploadRegion, target_layer );
		const int expected_calls = estimateGpuRegionUploadCalls( pUploadRegion, target_layer );
		const int page_table_reserve = bMapChanged ? 0 : 1;
		if ( m_nGpuRegionUploadsLastTick > 0 && m_nGpuRegionUploadBytesLastTick + expected_bytes > byte_budget ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET;
			break;
		}
		if ( m_nGpuRegionUploadsLastTick > 0 &&
			 getGpuRegionTextureUpdateCallsLastTick() + expected_calls + page_table_reserve > update_call_budget ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET;
			break;
		}

		int victim_layer = -1;
		if ( m_nGpuRegionResidentCount + m_nGpuRegionPendingUnmappedReservations >= m_nGpuRegionCacheCapacity ) {
			uint64_t oldest_tick = std::numeric_limits<uint64_t>::max();
			int farthest_distance = -1;
			for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
				const Vector2i old_location = m_szGpuRegionLocations[layer];
				if ( old_location == s_v2iMax || m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
					continue;
				}
				if ( m_szGpuRegionVictimReserved[layer] != 0u ) {
					continue;
				}
				const int old_map_index = Terrain3DRegionRepository::getRegionMapIndex( old_location );
				if ( old_map_index >= 0 && desired_map[old_map_index] != 0u ) {
					continue;
				}
				const int distance =
					MAX( std::abs( old_location.x - center.x ), std::abs( old_location.y - center.y ) );
				const uint64_t last_use = m_szGpuRegionLastUseTick[layer];
				if ( last_use < oldest_tick || ( last_use == oldest_tick && distance > farthest_distance ) ) {
					oldest_tick = last_use;
					farthest_distance = distance;
					victim_layer = layer;
				}
			}
			if ( victim_layer < 0 ) {
				m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_PAGE_CAPACITY;
				continue;
			}
		}

		// The staging layer is not referenced by the page table. All channel
		// updates therefore complete before any shader can sample the new page.
		if ( !uploadGpuRegionLayer( target_layer, location, REGION_TYPE_MAX ) ) {
			continue;
		}
		t3dGpuRegionPendingUpload_t &pending = m_szGpuRegionPendingUploads[target_layer];
		if ( pending.m_nGeneration != 0u ) {
			pending.m_nPublication = TERRAIN3D_GPU_PENDING_PUBLICATION_NEW;
			pending.m_nVictimLayer = victim_layer;
			pending.m_nLastUseTick = m_nGpuRegionTick;
			pending.m_nLoadedTick = m_nGpuRegionTick;
			const int prefetch_begin = selection_stats.m_nRequired + selection_stats.m_nGuard;
			const int prefetch_end = prefetch_begin + selection_stats.m_nPrefetch;
			pending.m_nPrefetchOrigin =
				desired_index >= prefetch_begin && desired_index < prefetch_end ? 1u : 0u;
			if ( victim_layer >= 0 ) {
				pending.m_victimLocation = m_szGpuRegionLocations[victim_layer];
				pending.m_nVictimLastUseTick = m_szGpuRegionLastUseTick[victim_layer];
				m_szGpuRegionVictimReserved[victim_layer] = 1u;
			} else {
				++m_nGpuRegionPendingUnmappedReservations;
			}
			++m_nGpuRegionUploadsLastTick;
			continue;
		}

		if ( victim_layer >= 0 ) {
			const Vector2i old_location = m_szGpuRegionLocations[victim_layer];
			const int old_map_index = Terrain3DRegionRepository::getRegionMapIndex( old_location );
			if ( old_map_index >= 0 ) {
				m_szGpuRegionMap[old_map_index] = 0u;
			}
			recordGpuRegionDeparture( victim_layer, true );
			retireGpuRegionSlot( victim_layer );
			++m_nGpuRegionEvictionsLastTick;
		} else {
			++m_nGpuRegionResidentCount;
		}
		m_szGpuRegionLocations[target_layer] = location;
		m_szGpuRegionLastUseTick[target_layer] = m_nGpuRegionTick;
		m_szGpuRegionLoadedTick[target_layer] = m_nGpuRegionTick;
		const int prefetch_begin = selection_stats.m_nRequired + selection_stats.m_nGuard;
		const int prefetch_end = prefetch_begin + selection_stats.m_nPrefetch;
		m_szGpuRegionPrefetchOrigin[target_layer] =
			desired_index >= prefetch_begin && desired_index < prefetch_end ? 1u : 0u;
		m_szGpuRegionSlotStates[target_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int new_map_index = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( new_map_index >= 0 ) {
			m_szGpuRegionMap[new_map_index] = ( uint16_t )( target_layer + 1 );
		}
		++m_nGpuRegionUploadsLastTick;
		++m_nGpuRegionUploadsSinceReset;
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
		bMapChanged = true;
	}

	int resident_required = 0;
	const int written_required = MIN( selection_stats.m_nRequired, footprint_count );
	for ( int index = 0; index < written_required; ++index ) {
		if ( findGpuRegionSlot( desired[index] ) >= 0 ) {
			++resident_required;
		}
	}
	m_nGpuRegionMissingRequiredLastTick = selection_stats.m_nRequired - resident_required;
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) < 0 ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	m_gpuRegionCacheCenter = center;
	m_nGpuRegionStreamingGenerationLast = streaming_generation;
	m_bGpuRegionCacheSelectionDirty =
		!bAllDesiredResident || m_nGpuRegionMissingRequiredLastTick > 0 || m_nGpuRegionRepublishPendingCount > 0;
	if ( bMapChanged ) {
		notePageTableChanged();
	}
	return t3dGpuRegionCacheUpdateResult_t { m_bPageTableChangedThisTick };
}

bool Terrain3DGpuRegionCache::refreshTerrainShadowMasks( void ) {
	if ( !m_bGpuRegionCacheEnabled || !m_bGpuRegionCacheTexturesReady ) {
		return true;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szGpuRegionLocations[layer];
		Terrain3DRegion *const pRegion = m_regionRepository.getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const Ref<Image> pShadowMask = makeTerrainShadowMaskImage( pRegion );
		if ( pShadowMask.is_null() || !m_generatedTerrainShadowMasks.update( pShadowMask, layer ) ) {
			return false;
		}
		++m_nGpuRegionTerrainShadowUpdatesLastTick;
		m_nGpuRegionUploadBytesLastTick += t3dGpuRegionCacheGpuImageBytes( pShadowMask );
	}
	return true;
}

bool Terrain3DGpuRegionCache::updateResidentLocation( const Vector2i &vLocation, const t3dMapType_t eMapType,
													  Terrain3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		m_bGpuRegionCacheSelectionDirty = true;
		return false;
	}
	if ( eMapType == REGION_TYPE_MAX ) {
		return republish( vLocation, m_pCompute );
	}
	const bool updated = uploadGpuRegionLayer( layer, vLocation, eMapType );
	if ( updated ) {
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	return updated;
}

bool Terrain3DGpuRegionCache::republishAllResidents( Terrain3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	Vector2i locations[s_nGpuCacheMaxLayers];
	int count = 0;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
			 m_szGpuRegionLocations[layer] != s_v2iMax ) {
			locations[count++] = m_szGpuRegionLocations[layer];
		}
	}
	bool changed = false;
	for ( int index = 0; index < count; ++index ) {
		changed = republish( locations[index], m_pCompute ) || changed;
	}
	return changed;
}

TypedArray<Vector2i> Terrain3DGpuRegionCache::getRenderRegionSlots( void ) const {
	TypedArray<Vector2i> slots;
	slots.resize( m_nGpuRegionPhysicalLayerCount );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		slots[layer] = m_szGpuRegionLocations[layer];
	}
	return slots;
}

int Terrain3DGpuRegionCache::getSlotStateForLocation( const Vector2i &vLocation ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionLocations[layer] == vLocation ) {
			return ( int )( m_szGpuRegionSlotStates[layer] );
		}
	}
	return ( int )( TERRAIN3D_GPU_SLOT_FREE );
}

bool Terrain3DGpuRegionCache::isAuthoringFullResidencyPossible( void ) const {
	return !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ||
		   calculateGpuRegionCacheCapacity() >= m_regionRepository.getActiveRegionCount();
}

bool Terrain3DGpuRegionCache::isAuthoringReadbackSafe( void ) const {
	if ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() || !m_bGpuRegionCacheEnabled ) {
		return true;
	}
	if ( !m_bGpuRegionCacheTexturesReady || m_runtimePageStore.isPrepareJobActive() ||
		 m_bGpuRegionCacheSelectionDirty || m_nGpuRegionUploadsLastTick > 0 ) {
		return false;
	}
	return m_nGpuRegionTick >= m_nGpuRegionLastPublicationTick + 2u;
}

size_t Terrain3DGpuRegionCache::getReservedBytes( void ) const {
	const size_t far_height_side =
		( size_t )( m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide );
	const size_t far_surface_side =
		( size_t )( m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : s_nGpuCacheFarHeightSide );
	const size_t fixed_far_bytes = far_height_side * far_height_side * 2u + far_surface_side * far_surface_side * 4u;
	const size_t shared_bytes = estimateSharedBytes();
	return ( shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u ) +
		   estimateLayerBytes() * ( size_t )( MAX( 0, m_nGpuRegionPhysicalLayerCount ) );
}

size_t Terrain3DGpuRegionCache::getMinimumBytes( void ) const {
	const size_t far_height_side =
		( size_t )( m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide );
	const size_t far_surface_side =
		( size_t )( m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : s_nGpuCacheFarHeightSide );
	const size_t fixed_far_bytes = far_height_side * far_height_side * 2u + far_surface_side * far_surface_side * 4u;
	const size_t shared_bytes = estimateSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateLayerBytes() * ( size_t )( s_nGpuCacheMinSpareLayers + 1 );
}

size_t Terrain3DGpuRegionCache::getHardCapBytes( void ) const {
	const size_t far_height_side =
		( size_t )( m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide );
	const size_t far_surface_side =
		( size_t )( m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : s_nGpuCacheFarHeightSide );
	const size_t fixed_far_bytes = far_height_side * far_height_side * 2u + far_surface_side * far_surface_side * 4u;
	const size_t shared_bytes = estimateSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateLayerBytes() * ( size_t )( s_nGpuCacheMaxLayers );
}

t3dGpuRegionCacheStats_t Terrain3DGpuRegionCache::getStats( void ) const {
	t3dGpuRegionCacheStats_t stats;
	stats.m_bEnabled = m_bGpuRegionCacheEnabled;
	stats.m_bTexturesReady = m_bGpuRegionCacheTexturesReady;
	stats.m_bSelectionPending = m_bGpuRegionCacheSelectionDirty;
	stats.m_bCameraMovedLastTick = m_bGpuRegionCameraMovedLastTick;
	stats.m_nBudgetMb = m_nGpuRegionCacheBudgetMb;
	stats.m_nCapacity = m_nGpuRegionCacheCapacity;
	stats.m_nPhysicalLayers = m_nGpuRegionPhysicalLayerCount;
	stats.m_nSpareLayers = m_nGpuRegionSpareLayerCount;
	stats.m_nResident = m_nGpuRegionResidentCount;
	stats.m_nRetired = m_nGpuRegionRetiredCount;
	stats.m_nDesiredLastTick = m_nGpuRegionDesiredLastTick;
	stats.m_nRequiredLastTick = m_nGpuRegionRequiredLastTick;
	stats.m_nGuardLastTick = m_nGpuRegionGuardLastTick;
	stats.m_nPrefetchLastTick = m_nGpuRegionPrefetchLastTick;
	stats.m_nHysteresisLastTick = m_nGpuRegionHysteresisLastTick;
	stats.m_nMissingRequiredLastTick = m_nGpuRegionMissingRequiredLastTick;
	stats.m_nRequestedPages = m_nGpuRegionRequestedPages;
	stats.m_nRequiredRadius = m_nGpuRegionRequiredRadius;
	stats.m_nUploadsLastTick = m_nGpuRegionUploadsLastTick;
	stats.m_nEvictionsLastTick = m_nGpuRegionEvictionsLastTick;
	stats.m_nCookedHitsLastTick = m_nGpuRegionCookedHitsLastTick;
	stats.m_nCookedMissesLastTick = m_nGpuRegionCookedMissesLastTick;
	stats.m_nRuntimePreparesLastTick = m_nGpuRegionRuntimePreparesLastTick;
	stats.m_nReusedLayersLastTick = m_nGpuRegionReusedLayersLastTick;
	stats.m_nPageTableUpdatesLastTick = m_nGpuRegionPageTableUpdatesLastTick;
	stats.m_nHeightUpdatesLastTick = m_nGpuRegionHeightUpdatesLastTick;
	stats.m_nControlUpdatesLastTick = m_nGpuRegionControlUpdatesLastTick;
	stats.m_nColorUpdatesLastTick = m_nGpuRegionColorUpdatesLastTick;
	stats.m_nDerivedUpdatesLastTick = m_nGpuRegionDerivedUpdatesLastTick;
	stats.m_nTerrainShadowUpdatesLastTick = m_nGpuRegionTerrainShadowUpdatesLastTick;
	stats.m_nRuntimeColorMipsLastTick = m_nGpuRegionRuntimeColorMipsLastTick;
	stats.m_nRuntimeDerivedBuildsLastTick = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats.m_nPreparedPagesLastTick = m_nGpuRegionPreparedPagesLastTick;
	stats.m_nPrepareWorkersLastTick = m_nGpuRegionPrepareWorkersLastTick;
	stats.m_nEffectiveUploadLimitLastTick = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats.m_nStallFlagsLastTick = m_nGpuRegionStallFlagsLastTick;
	stats.m_nUploadBytesLastTick = m_nGpuRegionUploadBytesLastTick;
	stats.m_nHeightBytesLastTick = m_nGpuRegionHeightBytesLastTick;
	stats.m_nControlBytesLastTick = m_nGpuRegionControlBytesLastTick;
	stats.m_nColorBytesLastTick = m_nGpuRegionColorBytesLastTick;
	stats.m_nDerivedBytesLastTick = m_nGpuRegionDerivedBytesLastTick;
	stats.m_nServiceUsecLast = m_nGpuRegionServiceUsecLast;
	stats.m_nPreparePublicationUsecLast = m_nGpuPreparePublicationUsecLast;
	stats.m_nPrepareEndToEndUsecLast = m_nGpuPrepareEndToEndUsecLast;
	return stats;
}

Dictionary Terrain3DGpuRegionCache::getStatsDictionary( const bool bDetailed ) const {
	Dictionary stats;
	const t3dRuntimePageStats_t runtime = m_runtimePageStore.getStats();
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["budget_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["allocation_budget_mb"] = m_pTerrain && m_pTerrain->isEditorAuthoringMode()
										? calculateAuthoringGpuRegionBudgetMb()
										: m_nGpuRegionCacheBudgetMb;
	stats["capacity"] = m_nGpuRegionCacheCapacity;
	stats["physical_layers"] = m_nGpuRegionPhysicalLayerCount;
	stats["spare_layers"] = m_nGpuRegionSpareLayerCount;
	stats["retired_layers"] = m_nGpuRegionRetiredCount;
	stats["retire_frames"] = m_nGpuRegionRetireFrames;
	stats["resident"] = m_nGpuRegionResidentCount;
	stats["republish_pending"] = m_nGpuRegionRepublishPendingCount;
	stats["requested_pages"] = m_nGpuRegionRequestedPages;
	stats["required_radius_regions"] = m_nGpuRegionRequiredRadius;
	stats["desired_last_tick"] = m_nGpuRegionDesiredLastTick;
	stats["required_last_tick"] = m_nGpuRegionRequiredLastTick;
	stats["guard_last_tick"] = m_nGpuRegionGuardLastTick;
	stats["prefetch_last_tick"] = m_nGpuRegionPrefetchLastTick;
	stats["hysteresis_last_tick"] = m_nGpuRegionHysteresisLastTick;
	stats["missing_required_last_tick"] = m_nGpuRegionMissingRequiredLastTick;
	stats["stall_flags_last_tick"] = ( int64_t )( m_nGpuRegionStallFlagsLastTick );
	stats["upload_layer_budget"] = m_nGpuRegionUploadBudgetLayers;
	stats["upload_budget_mb"] = m_nGpuRegionUploadBudgetMb;
	stats["effective_upload_limit_last_tick"] = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t )( m_nGpuRegionUploadsSinceReset );
	stats["evictions_last_tick"] = m_nGpuRegionEvictionsLastTick;
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["page_table_updates_last_tick"] = m_nGpuRegionPageTableUpdatesLastTick;
	stats["height_updates_last_tick"] = m_nGpuRegionHeightUpdatesLastTick;
	stats["control_updates_last_tick"] = m_nGpuRegionControlUpdatesLastTick;
	stats["color_updates_last_tick"] = m_nGpuRegionColorUpdatesLastTick;
	stats["derived_updates_last_tick"] = m_nGpuRegionDerivedUpdatesLastTick;
	stats["terrain_shadow_updates_last_tick"] = m_nGpuRegionTerrainShadowUpdatesLastTick;
	stats["runtime_color_mips_last_tick"] = m_nGpuRegionRuntimeColorMipsLastTick;
	stats["runtime_derived_builds_last_tick"] = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats["prepare_job_active"] = runtime.m_bPrepareJobActive;
	stats["prepare_job_pending_pages"] = runtime.m_nPendingPages;
	stats["prepare_ready_pages"] = runtime.m_nPreparedPages;
	stats["prepare_publication_usec_last"] = ( int64_t )( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t )( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t )( m_nGpuRegionServiceUsecLast );
	stats["upload_bytes_last_tick"] = ( int64_t )( m_nGpuRegionUploadBytesLastTick );
	stats["reserved_bytes"] = ( int64_t )( getReservedBytes() );
	stats["estimated_layer_bytes"] = ( int64_t )( estimateLayerBytes() );
	stats["estimated_shared_bytes"] = ( int64_t )( estimateSharedBytes() );
	stats["authoring_bounded_pool_covers_all_regions"] = isAuthoringFullResidencyPossible();
	stats["authoring_readback_safe"] = isAuthoringReadbackSafe();
	stats["control_implicit"] = m_bControlMapsImplicit;
	stats["color_implicit"] = m_bColorMapsImplicit;
	stats["color_resolution_divisor"] = m_nGpuRegionColorDivisor;
	stats["derived_resolution_divisor"] = m_nGpuRegionDerivedDivisor;
	stats["center"] = m_gpuRegionCacheCenter;
	stats["selection_pending"] = m_bGpuRegionCacheSelectionDirty;
	if ( !bDetailed ) {
		return stats;
	}
	stats["prediction_direction"] = m_gpuRegionPredictionDirection;
	stats["streaming_generation_last"] = ( int64_t )( m_nGpuRegionStreamingGenerationLast );
	stats["prefetch_used_total"] = ( int64_t )( m_nGpuRegionPrefetchUsedTotal );
	stats["prefetch_discarded_total"] = ( int64_t )( m_nGpuRegionPrefetchDiscardedTotal );
	const uint64_t completed_prefetches = m_nGpuRegionPrefetchUsedTotal + m_nGpuRegionPrefetchDiscardedTotal;
	stats["prefetch_accuracy"] = completed_prefetches > 0u
									 ? ( double )( m_nGpuRegionPrefetchUsedTotal ) / ( double )( completed_prefetches )
									 : -1.0;
	stats["departed_pages_total"] = ( int64_t )( m_nGpuRegionDepartedPagesTotal );
	stats["average_resident_ticks"] =
		m_nGpuRegionDepartedPagesTotal > 0u
			? ( double )( m_nGpuRegionResidentTicksTotal ) / ( double )( m_nGpuRegionDepartedPagesTotal )
			: 0.0;
	return stats;
}
