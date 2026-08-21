// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/terrain_3d_gpu_region_cache.h"

#include "assets/terrain_3d_assets.h"
#include "core/logger.h"
#include "data/terrain_3d_map_edit_service.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampling_storage.h"
#include "rendering/terrain_3d_compute.h"
#include "rendering/terrain_3d_material.h"
#include "streaming/terrain_3d_cpu_region_cache.h"
#include "streaming/terrain_3d_runtime_page_store.h"
#include "terrain_3d.h"

#include "eastl_config.h"
#include <EASTL/array.h>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/engine.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

template <typename Callback>
static bool terrain3dForEachSquareRing( const Vector2i &rCenter, const int nMaxRadius, Callback &&rCallback ) {
	for ( int radius = 0; radius < nMaxRadius; ++radius ) {
		if ( radius == 0 ) {
			if ( !rCallback( rCenter ) ) {
				return false;
			}
			continue;
		}
		for ( int x = -radius; x <= radius; ++x ) {
			if ( !rCallback( rCenter + Vector2i( x, -radius ) ) ||
				!rCallback( rCenter + Vector2i( x, radius ) ) ) {
				return false;
			}
		}
		for ( int y = -radius + 1; y <= radius - 1; ++y ) {
			if ( !rCallback( rCenter + Vector2i( -radius, y ) ) ||
				!rCallback( rCenter + Vector2i( radius, y ) ) ) {
				return false;
			}
		}
	}
	return true;
}

struct T3DControlCookSource {
	PackedByteArray nBytes;
	bool bPresent = false;
	bool bImplicit = true;
};

static bool terrain3dReadCookControl(
	const eastl::array<T3DControlCookSource, 9> &rSources,
	const int nRegionSize, const int iX, const int iZ, uint32_t &nControl
) {
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
	const T3DControlCookSource &source = rSources[( size_t ) ( ( region_dz + 1 ) * 3 + ( region_dx + 1 ) )];
	if ( !source.bPresent ) {
		return false;
	}
	if ( source.bImplicit ) {
		nControl = TERRAIN3D_REGION_DEFAULT_CONTROL;
		return true;
	}
	const size_t offset = ( size_t ) ( local_z * nRegionSize + local_x ) * sizeof( uint32_t );
	std::memcpy( &nControl, source.nBytes.ptr() + offset, sizeof( uint32_t ) );
	return true;
}

static uint32_t terrain3dReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint32_t terrain3dMaterialMaskFromControlImage( const Ref<Image> &rControl ) {
	if ( rControl.is_null() || rControl->is_empty() || rControl->get_format() != Image::FORMAT_RF ) {
		return 1u;
	}
	const PackedByteArray bytes = rControl->get_data();
	const uint8_t *const pRead = bytes.ptr();
	if ( !pRead || bytes.size() < 4 ) {
		return 1u;
	}
	uint32_t mask = 0u;
	const size_t texels = ( size_t ) ( bytes.size() ) / 4u;
	for ( size_t index = 0; index < texels; ++index ) {
		const uint32_t control = terrain3dReadU32Unaligned( pRead, index * 4u );
		mask |= 1u << ( uint32_t ) ( getBase( control ) & 31u );
		mask |= 1u << ( uint32_t ) ( getOverlay( control ) & 31u );
	}
	return mask != 0u ? mask : 1u;
}

EA_FORCE_INLINE static uint8_t terrain3dColorToByte( const float fValue ) { return ( uint8_t ) ( CLAMP( Math::round( fValue * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) ); }

static int terrain3dMaterialIntParam( T3DMaterial *pMaterial, const char *pszName, const int nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int ) ( value );
}

EA_FORCE_INLINE static size_t terrain3dGpuImageBytes( const Ref<Image> &rImage ) { return rImage.is_valid() ? ( size_t ) ( rImage->get_data().size() ) : 0u; }

class T3DLastScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;

public:
	T3DLastScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &nElapsedUsec;
		Time *time = Time::get_singleton();
		m_nStartedUsec = time ? time->get_ticks_usec() : 0u;
	}
	~T3DLastScopeTimer( void ) {
		if ( !m_pElapsedUsec ) {
			return;
		}
		Time *time = Time::get_singleton();
		const uint64_t finished = time ? time->get_ticks_usec() : m_nStartedUsec;
		*m_pElapsedUsec = finished >= m_nStartedUsec ? finished - m_nStartedUsec : 0u;
	}
};

class T3DProfileScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;
	const uint64_t *m_pRequestUsec = nullptr;
	uint64_t *m_pEndToEndUsec = nullptr;

public:
	EA_FORCE_INLINE T3DProfileScopeTimer( uint64_t &nElapsedUsec, const uint64_t &nRequestUsec, uint64_t &nEndToEndUsec ) : m_pElapsedUsec( &nElapsedUsec ), m_pRequestUsec( &nRequestUsec ), m_pEndToEndUsec( &nEndToEndUsec ) { Time *time = Time::get_singleton(); m_nStartedUsec = time ? time->get_ticks_usec() : 0u; }
	~T3DProfileScopeTimer( void ) {
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

} // namespace

T3DGpuRegionCache::T3DGpuRegionCache(
	T3DRegionRepository &rRepository,
	T3DSamplingStorage &rSamplingStorage,
	T3DMapEditService &rMapEditService,
	T3DRuntimePageStore &rRuntimePageStore,
	T3DCpuRegionCache &rCpuRegionCache
) :
		m_regionRepository( rRepository ),
		m_samplingStorage( rSamplingStorage ),
		m_mapEditService( rMapEditService ),
		m_runtimePageStore( rRuntimePageStore ),
		m_cpuRegionCache( rCpuRegionCache ) {
	std::fill_n( m_szGpuRegionLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szGpuRegionPriorityLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
}

void T3DGpuRegionCache::setDefaultMaps(
	const Ref<Image> &rHeight, const Ref<Image> &rControl,
	const Ref<Image> &rColor, const Ref<Image> &rDerived,
	const bool bControlImplicit, const bool bColorImplicit
) {
	const bool layoutChanged = bControlImplicit != m_bControlMapsImplicit || bColorImplicit != m_bColorMapsImplicit;
	m_pDefaultHeightMap = rHeight;
	m_pDefaultControlMap = rControl;
	m_pDefaultColorMap = rColor;
	m_pDefaultDerivedMap = rDerived;
	m_bControlMapsImplicit = bControlImplicit;
	m_bColorMapsImplicit = bColorImplicit;
	if ( layoutChanged && m_bGpuRegionCacheEnabled ) {
		reset( true );
	}
}

Ref<Image> T3DGpuRegionCache::getRegionMapOrDefault( const T3DRegion *pRegion, const t3dMapType_t eMapType ) const {
	if ( pRegion && !pRegion->isDeleted() ) {
		Ref<Image> pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
		}
	}
	switch ( eMapType ) {
		case TYPE_HEIGHT: return m_pDefaultHeightMap;
		case TYPE_CONTROL: return m_pDefaultControlMap;
		case TYPE_COLOR: return m_pDefaultColorMap;
		default: return Ref<Image>();
	}
}

Ref<Image> T3DGpuRegionCache::makeGpuColorImage( const Ref<Image> &rImage ) const {
	Ref<Image> pSource = rImage.is_valid() ? rImage : m_pDefaultColorMap;
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
		pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ),
			MAX( 1, pSource->get_height() / divisor ), Image::INTERPOLATE_BILINEAR );
	}
	if ( pGpuImage->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pGpuImage;
}

Ref<Image> T3DGpuRegionCache::makeGpuDerivedImage( const Ref<Image> &rImage ) const {
	Ref<Image> pSource = rImage.is_valid() ? rImage : m_pDefaultDerivedMap;
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
	pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ),
		MAX( 1, pSource->get_height() / divisor ), Image::INTERPOLATE_BILINEAR );
	return pGpuImage;
}

Ref<Image> T3DGpuRegionCache::makeMiddleCompositeImage(
	const Vector2i &rLocation, const Ref<Image> &rControl, const Ref<Image> &rColor
) const {
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
	if ( control_bytes.size() < ( int64_t ) ( rControl->get_width() ) * rControl->get_height() * 4 ||
		color_bytes.size() < ( int64_t ) ( width ) * height * 4 ) {
		return Ref<Image>();
	}
	PackedColorArray material_colors;
	const Ref<T3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<T3DAssets>();
	if ( pAssets.is_valid() ) {
		material_colors = pAssets->getTextureColors();
	}
	const PackedFloat32Array material_scales = pAssets.is_valid() ?
		pAssets->getTextureUvScales() : PackedFloat32Array();
	eastl::array<Ref<Image>, TERRAIN3D_MAX_TEXTURE_ASSETS> material_images;
	if ( pAssets.is_valid() ) {
		for ( int material = 0; material < MIN( pAssets->getTextureCount(), TERRAIN3D_MAX_TEXTURE_ASSETS ); ++material ) {
			const Ref<T3DTextureAsset> pAsset = pAssets->getTextureAsset( material );
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getAlbedoTexture() : Ref<Texture2D>();
			Ref<Image> pMaterialImage = pTexture.is_valid() ? pTexture->get_image() : Ref<Image>();
			// Imported DDS/VRAM textures commonly expose compressed Images. get_pixel()
			// rejects those and logs once per sample, so prepare one private readable
			// copy per material before entering the composite's texel loops. If the
			// platform cannot decompress that format, the bake falls back to its tint.
			if ( pMaterialImage.is_valid() && pMaterialImage->is_compressed() ) {
				pMaterialImage = pMaterialImage->duplicate();
				if ( pMaterialImage->decompress() != OK ) {
					pMaterialImage.unref();
				}
			}
			if ( pMaterialImage.is_valid() && pMaterialImage->is_empty() ) {
				pMaterialImage.unref();
			}
			material_images[( size_t ) ( material )] = pMaterialImage;
		}
	}
	PackedByteArray output;
	output.resize( width * height * 4 );
	uint8_t *const pWrite = output.ptrw();
	const uint8_t *const pControl = control_bytes.ptr();
	const uint8_t *const pColor = color_bytes.ptr();
	for ( int y = 0; y < height; ++y ) {
		const int control_y = MIN( rControl->get_height() - 1, y * rControl->get_height() / height );
		for ( int x = 0; x < width; ++x ) {
			const int control_x = MIN( rControl->get_width() - 1, x * rControl->get_width() / width );
			const uint32_t control = terrain3dReadU32Unaligned( pControl,
				( size_t ) ( control_y * rControl->get_width() + control_x ) * 4u );
			const int base = ( int ) ( getBase( control ) );
			const int overlay = ( int ) ( getOverlay( control ) );
			const Color base_color = base < material_colors.size() ? material_colors[base] : COLOR_WHITE;
			const Color overlay_color = overlay < material_colors.size() ? material_colors[overlay] : base_color;
			const float control_scale = 0.9f - ( float ) ( ( ( getUvScale( control ) + 3u ) % 8u ) + 1u ) * 0.1f;
			const float angle = ( float ) ( getUvRotation( control ) ) * Math_TAU / 16.0f;
			const float cs = Math::cos( angle );
			const float sn = Math::sin( angle );
			const float local_x = ( ( float ) ( x ) + 0.5f ) * ( float ) ( m_regionRepository.getRegionSize() ) / ( float ) ( width );
			const float local_y = ( ( float ) ( y ) + 0.5f ) * ( float ) ( m_regionRepository.getRegionSize() ) / ( float ) ( height );
			const float world_x = ( ( float ) ( rLocation.x * m_regionRepository.getRegionSize() ) + local_x ) * ( float ) ( m_regionRepository.getVertexSpacing() );
			const float world_y = ( ( float ) ( rLocation.y * m_regionRepository.getRegionSize() ) + local_y ) * ( float ) ( m_regionRepository.getVertexSpacing() );
			auto sample_material = [&]( const int material_id, const Color &rTint ) -> Color {
				if ( material_id < 0 || material_id >= TERRAIN3D_MAX_TEXTURE_ASSETS ||
					material_images[( size_t ) ( material_id )].is_null() ) {
					return rTint;
				}
				const Ref<Image> &pImage = material_images[( size_t ) ( material_id )];
				const float asset_scale = material_id < material_scales.size() ? material_scales[material_id] : 0.1f;
				const float source_u = ( cs * world_x + sn * world_y ) * control_scale * asset_scale;
				const float source_v = ( cs * world_y - sn * world_x ) * control_scale * asset_scale;
				const float wrapped_u = source_u - Math::floor( source_u );
				const float wrapped_v = source_v - Math::floor( source_v );
				const int source_x = CLAMP( ( int ) ( wrapped_u * pImage->get_width() ), 0, pImage->get_width() - 1 );
				const int source_y = CLAMP( ( int ) ( wrapped_v * pImage->get_height() ), 0, pImage->get_height() - 1 );
				return pImage->get_pixel( source_x, source_y ) * rTint;
			};
			const Color base_sample = sample_material( base, base_color );
			const Color overlay_sample = sample_material( overlay, overlay_color );
			float blend = ( float ) ( getBlend( control ) ) / 255.0f;
			blend = CLAMP( ( blend - 0.12f ) / 0.76f, 0.0f, 1.0f );
			blend = blend * blend * ( 3.0f - 2.0f * blend );
			const Color material_color = base_sample.lerp( overlay_sample, blend );
			const size_t offset = ( size_t ) ( y * width + x ) * 4u;
			const float authored_r = ( float ) ( pColor[offset] ) / 255.0f;
			const float authored_g = ( float ) ( pColor[offset + 1u] ) / 255.0f;
			const float authored_b = ( float ) ( pColor[offset + 2u] ) / 255.0f;
			pWrite[offset] = terrain3dColorToByte( authored_r * material_color.r );
			pWrite[offset + 1u] = terrain3dColorToByte( authored_g * material_color.g );
			pWrite[offset + 2u] = terrain3dColorToByte( authored_b * material_color.b );
			pWrite[offset + 3u] = isHole( control ) ? 0u :
				( uint8_t ) ( 0x80u | ( ( uint32_t ) ( pColor[offset + 3u] ) * 127u + 127u ) / 255u );
		}
	}
	Ref<Image> pComposite = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, output );
	if ( pComposite.is_null() || pComposite->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pComposite;
}

Ref<Image> T3DGpuRegionCache::makeGpuRenderControlImage( const Vector2i &vLocation ) const {
	eastl::array<T3DControlCookSource, 9> sources;
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = vLocation + Vector2i( dx, dz );
			const int source_index = ( dz + 1 ) * 3 + ( dx + 1 );
			T3DControlCookSource &source = sources[( size_t ) ( source_index )];
			const T3DRegion *pRegion = m_regionRepository.getRegionPtr( location );
			Ref<T3DRegion> pLoadedSource;
			const bool bNeedsSourceLoad = !pRegion || pRegion->isDeleted() ||
				( pRegion->hasChannel( TYPE_CONTROL ) && pRegion->getControlMap().is_null() );
			if ( bNeedsSourceLoad && m_cpuRegionCache.loadSource( location, pLoadedSource ) ) {
				pRegion = pLoadedSource.ptr();
			}
			if ( !pRegion || pRegion->isDeleted() ) {
				continue;
			}
			source.bPresent = true;
			if ( !pRegion->hasChannel( TYPE_CONTROL ) || pRegion->getControlMap().is_null() ) {
				source.bImplicit = true;
				continue;
			}
			const Ref<Image> pControl = pRegion->getControlMap();
			if ( pControl->get_width() != m_regionRepository.getRegionSize() || pControl->get_height() != m_regionRepository.getRegionSize() ||
				pControl->get_format() != Image::FORMAT_RF || pControl->has_mipmaps() ) {
				return Ref<Image>();
			}
			source.nBytes = pControl->get_data();
			if ( source.nBytes.size() != m_regionRepository.getRegionSize() * m_regionRepository.getRegionSize() * ( int ) ( sizeof( uint32_t ) ) ) {
				return Ref<Image>();
			}
			source.bImplicit = false;
		}
	}


	PackedByteArray output;
	output.resize( m_regionRepository.getRegionSize() * m_regionRepository.getRegionSize() * ( int ) ( sizeof( uint32_t ) ) );
	uint8_t *const pWrite = output.ptrw();
	for ( int z = 0; z < m_regionRepository.getRegionSize(); ++z ) {
		for ( int x = 0; x < m_regionRepository.getRegionSize(); ++x ) {
			uint32_t center = TERRAIN3D_REGION_DEFAULT_CONTROL;
			if ( !terrain3dReadCookControl( sources, m_regionRepository.getRegionSize(), x, z, center ) ) {
				std::memcpy( pWrite + ( size_t ) ( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}
			if ( isAuto( center ) || isHole( center ) ) {
				std::memcpy( pWrite + ( size_t ) ( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}

			eastl::array<uint64_t, TERRAIN3D_MAX_TEXTURE_ASSETS> material_weights{};
			for ( int dz = -TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; dz <= TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; ++dz ) {
				for ( int dx = -TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; dx <= TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS; ++dx ) {
					uint32_t pControl = TERRAIN3D_REGION_DEFAULT_CONTROL;
					if ( !terrain3dReadCookControl( sources, m_regionRepository.getRegionSize(), x + dx, z + dz, pControl ) || isAuto( pControl ) || isHole( pControl ) ) {
						continue;
					}
					const uint64_t spatial_weight = ( uint64_t ) ( TERRAIN3D_RENDER_CONTROL_KERNEL[dx + TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS] * TERRAIN3D_RENDER_CONTROL_KERNEL[dz + TERRAIN3D_RENDER_CONTROL_BLEND_RADIUS] );
					const uint8_t base = getBase( pControl );
					const uint8_t overlay = getOverlay( pControl );
					const uint8_t blend = getBlend( pControl );
					if ( base == overlay ) {
						material_weights[( size_t ) ( base )] += spatial_weight * 255u;
					} else {
						material_weights[( size_t ) ( base )] += spatial_weight * ( uint64_t ) ( 255u - blend );
						material_weights[( size_t ) ( overlay )] += spatial_weight * ( uint64_t ) ( blend );
					}
				}
			}

			int strongest = -1;
			int second = -1;
			for ( int material = 0; material < TERRAIN3D_MAX_TEXTURE_ASSETS; ++material ) {
				if ( strongest < 0 || material_weights[( size_t ) ( material )] > material_weights[( size_t ) ( strongest )] ) {
					second = strongest;
					strongest = material;
				} else if ( second < 0 || material_weights[( size_t ) ( material )] > material_weights[( size_t ) ( second )] ) {
					second = material;
				}
			}
			if ( strongest < 0 || material_weights[( size_t ) ( strongest )] == 0u ) {
				std::memcpy( pWrite + ( size_t ) ( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}
			if ( second < 0 || material_weights[( size_t ) ( second )] == 0u ) {
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
			const uint64_t base_weight = material_weights[( size_t ) ( base )];
			const uint64_t overlay_weight = material_weights[( size_t ) ( overlay )];
			const uint64_t pair_weight = base_weight + overlay_weight;
			uint8_t blend = 0u;
			if ( base != overlay && pair_weight > 0u ) {
				blend = ( uint8_t ) ( CLAMP( ( int ) ( ( overlay_weight * 255u + pair_weight / 2u ) / pair_weight ), 0, 255 ) );
			}
			const uint32_t cooked = ( center & ~TERRAIN3D_RENDER_CONTROL_MATERIAL_MASK ) | encBase( ( uint8_t ) ( base ) ) |
				encOverlay( ( uint8_t ) ( overlay ) ) | encBlend( blend );
			std::memcpy( pWrite + ( size_t ) ( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ), &cooked, sizeof( uint32_t ) );
		}
	}
	return Image::create_from_data( m_regionRepository.getRegionSize(), m_regionRepository.getRegionSize(), false, Image::FORMAT_RF, output );
}

int T3DGpuRegionCache::calculateRequiredCorePagesForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int radius = terrain3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(),
		detail_lods, ( double ) ( m_regionRepository.getVertexSpacing() ), m_regionRepository.getRegionSize() );
	const int diameter = radius * 2 + 1;
	return MIN( diameter * diameter, m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
}

int T3DGpuRegionCache::calculateAuthoringGpuRegionBudgetMb( void ) const {
	// m_nGpuRegionCacheBudgetMb is already the effective allocation produced by
	// Terrain3D's aggregate memory-budget coordinator. Authoring may use more
	// physical layers than runtime, but it must fit those layers inside that same
	// coordinated GPU allocation instead of manufacturing extra editor memory.
	return CLAMP( MAX( 1, m_nGpuRegionCacheBudgetMb ),
		TERRAIN3D_GPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_GPU_REGION_CACHE_MAX_BUDGET_MB );
}

int T3DGpuRegionCache::calculateCapacityForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int required_core = calculateRequiredCorePagesForLods( detail_lods );
	// Allocation is budget-driven. The unified region policy determines which pages
	// are desirable; legacy GPU guard/prefetch controls must not define a second
	// spatial footprint or reduce the usable physical cache.
	const int requested = MIN( m_regionRepository.getActiveRegionCount(), TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	const int capacity = terrain3dGpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
		m_nGpuRegionCacheBudgetMb, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS, requested, m_bControlMapsImplicit,
		!isColorChannelEnabled(), isDerivedChannelEnabled(), m_pTerrain->getGpuRegionColorResolutionDivisor(),
		m_pTerrain->getGpuRegionDerivedResolutionDivisor(), m_pTerrain->getGpuFarQualityDivisor(),
		m_nGpuRegionSparePages, m_pTerrain->getGpuFarHeightAtlasSide() );
	return MAX( required_core > 0 ? 1 : 0, capacity );
}

int T3DGpuRegionCache::calculateGpuRegionCacheCapacity( void ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		const int page_limit = CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16,
			TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS );
		return terrain3dGpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
			calculateAuthoringGpuRegionBudgetMb(), page_limit,
			MIN( m_regionRepository.getActiveRegionCount(), page_limit ), m_bControlMapsImplicit, !isColorChannelEnabled(),
			isDerivedChannelEnabled(),
			m_pTerrain->getGpuRegionColorResolutionDivisor(),
			m_pTerrain->getGpuRegionDerivedResolutionDivisor(),
			m_pTerrain->getGpuFarQualityDivisor(), m_nGpuRegionSparePages,
			m_pTerrain->getGpuFarHeightAtlasSide() );
	}
	const int allocation_request = MIN( m_regionRepository.getActiveRegionCount(), TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	return terrain3dGpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
		m_nGpuRegionCacheBudgetMb, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS,
		allocation_request, m_bControlMapsImplicit, !isColorChannelEnabled(),
		isDerivedChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1, m_nGpuRegionSparePages,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DGpuRegionCache::calculateGpuRegionPhysicalLayers( const int nMappedCapacity ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		return terrain3dGpuCacheCalculatePhysicalLayers( nMappedCapacity, m_nGpuRegionSparePages,
			CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS ) );
	}
	return terrain3dGpuCacheCalculatePhysicalLayers( nMappedCapacity,
		m_nGpuRegionSparePages, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
}

int T3DGpuRegionCache::calculateGpuRegionRequiredRadius( void ) const {
	if ( !m_pTerrain ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	int detail_lods = m_pTerrain->getGpuRegionDetailLods();
	if ( detail_lods <= 0 || detail_lods > total_lods ) {
		detail_lods = total_lods;
	}
	return terrain3dGpuCacheCalculateFootprintRadiusRegions( m_pTerrain->getMeshSize(),
		detail_lods, ( double ) ( m_regionRepository.getVertexSpacing() ), m_regionRepository.getRegionSize() );
}

int T3DGpuRegionCache::calculateGpuRegionRequestedPages( void ) const {
	if ( !m_pTerrain || m_regionRepository.getActiveRegionCount() <= 0 ) {
		return 0;
	}
	if ( m_pTerrain->isEditorAuthoringMode() ) {
		return MIN( m_regionRepository.getActiveRegionCount(), m_pTerrain->getEditorGpuRegionCachePages() );
	}
	const Dictionary streaming = m_pTerrain->getRegionStreamingStats();
	const int active = ( int ) ( streaming["active"] );
	const int hot = ( int ) ( streaming["hot"] );
	const int predicted = ( int ) ( streaming["predicted_promotions"] );
	return MIN( m_regionRepository.getActiveRegionCount(), active + hot + predicted );
}

size_t T3DGpuRegionCache::estimateLayerBytes( void ) const {
	return terrain3dGpuCacheEstimateLayerBytes( m_regionRepository.getRegionSize(), m_bControlMapsImplicit,
		!isColorChannelEnabled(), isDerivedChannelEnabled(),
		m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
}

bool T3DGpuRegionCache::isColorChannelEnabled( void ) const {
	if ( m_bColorMapsImplicit ) {
		return false;
	}
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	const bool near_needed = m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		( pMaterial->getNearAuthoredColorContribution() > 0.0f || pMaterial->getNearWetnessContribution() > 0.0f );
	const bool middle_needed = m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		( pMaterial->getMiddleAuthoredColorContribution() > 0.0f || pMaterial->getMiddleWetnessContribution() > 0.0f );
	return near_needed || middle_needed;
}

bool T3DGpuRegionCache::isDerivedChannelEnabled( void ) const {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	return ( m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		pMaterial->getNearTerrainNormalQuality() == TERRAIN_NORMAL_QUALITY_DETAILED ) ||
		( m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		pMaterial->getMiddleTerrainNormalQuality() == TERRAIN_NORMAL_QUALITY_DETAILED );
}

size_t T3DGpuRegionCache::estimateSharedBytes( void ) const {
	return terrain3dGpuCacheEstimateSharedBytes( m_regionRepository.getRegionSize(), m_bControlMapsImplicit,
		!isColorChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DGpuRegionCache::estimateGpuRegionUploadCalls( const T3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	int calls = 1; // Height is always published.
	if ( isDerivedChannelEnabled() ) {
		++calls;
	}
	if ( !m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_CONTROL ) ) ) {
		++calls;
	}
	if ( isColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_COLOR ) ) ) {
		++calls;
	}
	return calls;
}

size_t T3DGpuRegionCache::estimateGpuRegionUploadBytes( const T3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0u;
	}
	const size_t full_texels = terrain3dGpuCacheTexelCount( m_regionRepository.getRegionSize(), 1 );
	const size_t derived_texels = terrain3dGpuCacheTexelCount( m_regionRepository.getRegionSize(),
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
	if ( full_texels == 0u || derived_texels == 0u ) {
		return 0u;
	}
	size_t bytes = full_texels * 2u;
	if ( isDerivedChannelEnabled() ) {
		bytes += derived_texels * 2u;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	if ( !m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_CONTROL ) ) ) {
		bytes += full_texels * 4u;
	}
	if ( isColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( REGION_CHANNEL_COLOR ) ) ) {
		bytes += terrain3dGpuCacheColorBytes( m_regionRepository.getRegionSize(),
			m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 );
	}
	return bytes;
}

int T3DGpuRegionCache::getGpuRegionTextureUpdateCallsLastTick( void ) const {
	return m_nGpuRegionHeightUpdatesLastTick + m_nGpuRegionControlUpdatesLastTick +
		m_nGpuRegionColorUpdatesLastTick + m_nGpuRegionDerivedUpdatesLastTick +
		m_nGpuRegionPageTableUpdatesLastTick;
}

void T3DGpuRegionCache::reset( const bool bClearTextures ) {
	std::fill_n( m_szGpuRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szGpuRegionLastUseTick, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionLoadedTick, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionRetireAfterFrame, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionSlotStates, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
	std::fill_n( m_szGpuRegionPrefetchOrigin, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionLayerChannelFlags, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionMaterialMasks, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint32_t ) ( 0 ) );
	std::fill_n( m_szGpuRegionPriorityLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szGpuRegionPriorityClasses, TERRAIN3D_GPU_CACHE_MAX_LAYERS, ( uint8_t ) ( 0 ) );
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
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
	m_gpuRegionCacheCenter = V2I_MAX;
	m_gpuRegionPredictionDirection = V2I_ZERO;
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
	if ( bClearTextures ) {
		m_generatedHeightMaps.clear();
		m_generatedControlMaps.clear();
		m_generatedColorMaps.clear();
		m_generatedDerivedMaps.clear();
		m_generatedMiddleCompositeMaps.clear();
	}
}

bool T3DGpuRegionCache::rebuildTextures( void ) {
	const int capacity = calculateGpuRegionCacheCapacity();
	LOG( INFO, "[gpu-cache] Rebuild requested: mapped_capacity=", capacity,
		"; region_size=", m_regionRepository.getRegionSize(), "; implicit_control=", m_bControlMapsImplicit,
		"; implicit_color=", m_bColorMapsImplicit );
	if ( capacity <= 0 || m_pDefaultHeightMap.is_null() || m_pDefaultControlMap.is_null() ||
		m_pDefaultColorMap.is_null() || m_pDefaultDerivedMap.is_null() ) {
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
	const Ref<Image> pDefaultGpuColor = makeGpuColorImage( m_pDefaultColorMap );
	const Ref<Image> pDefaultGpuDerived = makeGpuDerivedImage( m_pDefaultDerivedMap );
	if ( pDefaultGpuColor.is_null() || pDefaultGpuDerived.is_null() ) {
		return false;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuHeightMaps[layer] = m_pDefaultHeightMap;
	}
	for ( int layer = 0; layer < m_gpuDerivedMaps.size(); ++layer ) {
		m_gpuDerivedMaps[layer] = pDefaultGpuDerived;
	}
	for ( int layer = 0; layer < control_layers; ++layer ) {
		m_gpuControlMaps[layer] = m_pDefaultControlMap;
	}
	for ( int layer = 0; layer < color_layers; ++layer ) {
		m_gpuColorMaps[layer] = pDefaultGpuColor;
	}
	const Ref<Image> pDefaultMiddleComposite = makeMiddleCompositeImage( V2I_ZERO, m_pDefaultControlMap, pDefaultGpuColor );
	if ( pDefaultMiddleComposite.is_null() ) {
		return false;
	}
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		m_gpuMiddleCompositeMaps[layer] = pDefaultMiddleComposite;
	}

	const bool bHeightOk = m_generatedHeightMaps.create( m_gpuHeightMaps ).is_valid();
	const bool bControlOk = m_generatedControlMaps.create( m_gpuControlMaps ).is_valid();
	const bool bColorOk = m_generatedColorMaps.create( m_gpuColorMaps ).is_valid();
	const bool bDerivedOk = m_generatedDerivedMaps.create( m_gpuDerivedMaps ).is_valid();
	const bool bMiddleCompositeOk = m_generatedMiddleCompositeMaps.create( m_gpuMiddleCompositeMaps ).is_valid();
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_bGpuRegionCacheTexturesReady = bHeightOk && bControlOk && bColorOk && bDerivedOk && bMiddleCompositeOk;
	if ( !m_bGpuRegionCacheTexturesReady ) {
		LOG( ERROR, "Failed to create bounded GPU region cache with ", capacity, " physical layers" );
		return false;
	}
	m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	const size_t budget_bytes = ( size_t ) ( m_nGpuRegionCacheBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	const size_t layer_bytes = estimateLayerBytes();
	const size_t shared_bytes = estimateSharedBytes();
	const size_t estimated_bytes = shared_bytes + layer_bytes * ( size_t ) ( m_nGpuRegionPhysicalLayerCount );
	if ( estimated_bytes > budget_bytes ) {
		LOG( WARN, "One terrain region requires approximately ", layer_bytes,
			" GPU bytes and shared channels require ", shared_bytes,
			" bytes. The selected cache exceeds the configured budget of ", budget_bytes,
			" bytes because one physical layer is the minimum legal configuration." );
	}
	m_nGpuRegionRequestedPages = calculateGpuRegionRequestedPages();
	m_nGpuRegionRequiredRadius = calculateGpuRegionRequiredRadius();
	LOG( INFO, "[gpu-cache] Created bounded GPU region cache: ", capacity, " mapped pages, ",
		m_nGpuRegionSpareLayerCount, " spare/retirement pages, ", m_nGpuRegionPhysicalLayerCount,
		" physical layers, requested ", m_nGpuRegionRequestedPages, ", required radius ",
		m_nGpuRegionRequiredRadius, ", estimated bytes ", estimated_bytes, ", budget ", budget_bytes );
	return true;
}

int T3DGpuRegionCache::findGpuRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int layer = ( int ) ( m_szGpuRegionMap[map_index] ) - 1;
	if ( layer < 0 || layer >= m_nGpuRegionPhysicalLayerCount ||
		m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
		m_szGpuRegionLocations[layer] != vLocation ) {
		return -1;
	}
	return layer;
}

int T3DGpuRegionCache::findFreeGpuRegionSlot( void ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionLocations[layer] == V2I_MAX &&
			m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE ) {
			return layer;
		}
	}
	return -1;
}

void T3DGpuRegionCache::recordGpuRegionDeparture( const int nLayer, const bool bCountUnusedPrefetch ) {
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

void T3DGpuRegionCache::retireGpuRegionSlot( const int nLayer ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ||
		m_szGpuRegionSlotStates[nLayer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
		return;
	}
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_RETIRED;
	m_szGpuRegionRetireAfterFrame[nLayer] = std::numeric_limits<uint64_t>::max();
	++m_nGpuRegionRetiredCount;
}

void T3DGpuRegionCache::armPendingRetirements( void ) {
	Engine *const pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	const uint64_t release_frame = current_frame + ( uint64_t ) ( MAX( 1, m_nGpuRegionRetireFrames ) );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED &&
			m_szGpuRegionRetireAfterFrame[layer] == std::numeric_limits<uint64_t>::max() ) {
			m_szGpuRegionRetireAfterFrame[layer] = release_frame;
		}
	}
}

void T3DGpuRegionCache::reclaimRetiredGpuRegionSlots( void ) {
	Engine *const pEngine = Engine::get_singleton();
	const uint64_t current_frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nGpuRegionTick;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_RETIRED ||
			m_szGpuRegionRetireAfterFrame[layer] == std::numeric_limits<uint64_t>::max() ||
			current_frame < m_szGpuRegionRetireAfterFrame[layer] ) {
			continue;
		}
		m_szGpuRegionLocations[layer] = V2I_MAX;
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

void T3DGpuRegionCache::invalidate( const Vector2i &vLocation ) {
	if ( !m_bGpuRegionCacheEnabled ) {
		return;
	}
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		return;
	}
	const int map_index = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szGpuRegionMap[map_index] = 0u;
	}
	recordGpuRegionDeparture( layer, true );
	retireGpuRegionSlot( layer );
	m_nGpuRegionResidentCount = MAX( 0, m_nGpuRegionResidentCount - 1 );
	m_bGpuRegionCacheSelectionDirty = true;
}

void T3DGpuRegionCache::queueRepublish( const Vector2i &vLocation ) {
	const int map_index = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 || map_index >= TERRAIN3D_MAX_REGION_SLOTS ) {
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

bool T3DGpuRegionCache::republish( const Vector2i &vLocation, T3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	const int old_layer = findGpuRegionSlot( vLocation );
	if ( old_layer < 0 ) {
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
	const int map_index = T3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_FREE;
		return false;
	}
	m_szGpuRegionLocations[staging_layer] = vLocation;
	m_szGpuRegionLastUseTick[staging_layer] = m_nGpuRegionTick;
	m_szGpuRegionLoadedTick[staging_layer] = m_szGpuRegionLoadedTick[old_layer];
	m_szGpuRegionPrefetchOrigin[staging_layer] = m_szGpuRegionPrefetchOrigin[old_layer];
	m_szGpuRegionSlotStates[staging_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
	m_szGpuRegionMap[map_index] = ( uint16_t ) ( staging_layer + 1 );
	if ( m_szGpuRegionRepublishPending[map_index] != 0u ) {
		m_szGpuRegionRepublishPending[map_index] = 0u;
		m_nGpuRegionRepublishPendingCount = MAX( 0, m_nGpuRegionRepublishPendingCount - 1 );
	}
	m_szGpuRegionLoadedTick[old_layer] = 0u;
	m_szGpuRegionPrefetchOrigin[old_layer] = 0u;
	retireGpuRegionSlot( old_layer );
	return true;
}

int T3DGpuRegionCache::processPendingGpuRegionRepublishes( const int nLimit ) {
	if ( m_nGpuRegionRepublishPendingCount <= 0 || nLimit <= 0 || m_nGpuRegionPhysicalLayerCount <= 0 ) {
		return 0;
	}
	int published = 0;
	int visited = 0;
	int cursor = m_nGpuRegionRepublishCursor;
	while ( visited < TERRAIN3D_MAX_REGION_SLOTS && m_nGpuRegionRepublishPendingCount > 0 && published < nLimit ) {
		const int map_index = cursor;
		cursor = ( cursor + 1 ) % TERRAIN3D_MAX_REGION_SLOTS;
		++visited;
		if ( m_szGpuRegionRepublishPending[map_index] == 0u ) {
			continue;
		}
		const int map_x = map_index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
		const int map_y = map_index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE;
		const Vector2i location( map_x - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
			map_y - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2 );
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
		}
	}
	m_nGpuRegionRepublishCursor = cursor;
	return published;
}

int T3DGpuRegionCache::collectDesiredGpuRegions(
	const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity, t3dGpuCacheSelectionStats_t &rStats
) {
	rStats = t3dGpuCacheSelectionStats_s();
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_GPU_CACHE_MAX_LAYERS );
	int output_count = 0;
	uint8_t selected_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	auto append_location = [&]( const Vector2i &location, const t3dRegionStreamingState_t wanted ) {
		if ( output_count >= capacity ) {
			return false;
		}
		if ( !m_regionRepository.isLocationValid( location ) || m_regionRepository.getRegionPtr( location ) == nullptr ||
			rFrame.streamingState( location ) != wanted ) {
			return true;
		}
		const int map_index = T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u ) {
			return true;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
		return output_count < capacity;
	};
	auto append_state = [&]( const t3dRegionStreamingState_t wanted ) {
		terrain3dForEachSquareRing( rCenter, m_regionRepository.getRegionMapSize(),
			[&]( const Vector2i &location ) { return append_location( location, wanted ); } );
	};

	// Full surface pages are mandatory only for ACTIVE. HOT geometry is served by
	// the independent height cache below, so surface pressure can never move vertices.
	append_state( TERRAIN3D_REGION_STREAMING_ACTIVE );
	rStats.required = output_count;
	append_state( TERRAIN3D_REGION_STREAMING_HOT );
	rStats.guard = output_count - rStats.required;
	const int prefetch_begin = output_count;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount() && output_count < capacity; ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		const int map_index = location == V2I_MAX ? -1 : T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || m_regionRepository.getRegionPtr( location ) == nullptr ||
			!rFrame.isPrefetch( location ) ) {
			continue;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
	}
	rStats.prefetch = output_count - prefetch_begin;
	rStats.written = output_count;
	return output_count;
}

int T3DGpuRegionCache::fillMaterialPriorityIds( uint8_t *const pIds, const int nCapacity ) const {
	if ( !pIds || nCapacity <= 0 ) {
		return 0;
	}
	uint32_t emitted_mask = 0u;
	int count = 0;
	auto append_mask = [&]( uint32_t mask ) {
		while ( mask != 0u && count < nCapacity ) {
			int id = 0;
			while ( id < TERRAIN3D_MAX_TEXTURE_ASSETS && ( mask & ( 1u << ( uint32_t ) ( id ) ) ) == 0u ) {
				++id;
			}
			if ( id >= TERRAIN3D_MAX_TEXTURE_ASSETS ) {
				break;
			}
			const uint32_t bit = 1u << ( uint32_t ) ( id );
			mask &= ~bit;
			if ( ( emitted_mask & bit ) != 0u ) {
				continue;
			}
			pIds[count++] = ( uint8_t ) ( id );
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

bool T3DGpuRegionCache::uploadGpuRegionLayer(
	const int nLayer, const Vector2i &vLocation,
	const t3dMapType_t eMapType
) {
	uint64_t prepared_request_usec = 0u;
	const T3DProfileScopeTimer publicationTimer(
		m_nGpuPreparePublicationUsecLast,
		prepared_request_usec, m_nGpuPrepareEndToEndUsecLast
	);
	if ( !m_bGpuRegionCacheEnabled || !m_bGpuRegionCacheTexturesReady || nLayer < 0 ||
		nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return false;
	}
	T3DRegion *const pRegion = m_regionRepository.getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	const uint8_t source_channel_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t previous_channel_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	const bool bUploadHeight = eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX;
	// Optional channels are updated only when either the incoming page or the
	// previous physical contents are explicit. Reusing default-on-default layers
	// requires no texture update, while explicit-to-default transitions still clear
	// stale physical contents before publication.
	const bool bUploadControl = ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX ) &&
		!m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
			source_channel_flags, previous_channel_flags, ( uint8_t ) ( REGION_CHANNEL_CONTROL ) );
	const bool bUploadColor = ( eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) &&
		isColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
			source_channel_flags, previous_channel_flags, ( uint8_t ) ( REGION_CHANNEL_COLOR ) );
	Ref<Image> pHeight;
	Ref<Image> pDerived;
	Ref<Image> pControl;
	Ref<Image> pColor;
	Ref<Image> pMiddleComposite;
	const uint8_t previous_state = m_szGpuRegionSlotStates[nLayer];
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_STAGING;

	bool bCookedPage = false;
	uint32_t cooked_material_mask = 1u;
	if ( eMapType == REGION_TYPE_MAX && pRegion->runtimeMapsEvictableInternal() && m_pTerrain &&
		m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		bool bPreparedAttempted = false;
		bCookedPage = m_runtimePageStore.takeGpuPage( vLocation, pHeight, pControl, pColor, pDerived, pMiddleComposite,
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
				m_mapEditService.regenerateDerivedMaps( {vLocation}, AABB(), m_pCompute );
				pDerived = pRegion->getDerivedNormalSlopeMap();
			}
			if ( pDerived.is_null() ) {
				pDerived = m_pDefaultDerivedMap;
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
			if ( pSourceColor.is_valid() && ( !pSourceColor->has_mipmaps() ||
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
	if ( eMapType == REGION_TYPE_MAX && pMiddleComposite.is_null() ) {
		Ref<Image> pCompositeControl = pControl;
		if ( pCompositeControl.is_null() ) {
			pCompositeControl = makeGpuRenderControlImage( vLocation );
		}
		Ref<Image> pCompositeColor = pColor;
		if ( pCompositeColor.is_null() ) {
			pCompositeColor = makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) );
		}
		pMiddleComposite = makeMiddleCompositeImage( vLocation, pCompositeControl, pCompositeColor );
		if ( pMiddleComposite.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
	}

	bool bUploadOk = true;
	if ( bUploadHeight ) {
		bUploadOk = m_generatedHeightMaps.update( pHeight, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionHeightUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pHeight );
			m_nGpuRegionHeightBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
		if ( bUploadOk && bDerivedChannelEnabled ) {
			bUploadOk = m_generatedDerivedMaps.update( pDerived, nLayer );
		}
		if ( bUploadOk && bDerivedChannelEnabled ) {
			++m_nGpuRegionDerivedUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pDerived );
			m_nGpuRegionDerivedBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
		if ( bDerivedChannelEnabled && !bCookedPage && !IS_EDITOR && pDerived != m_pDefaultDerivedMap ) {
			pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
		}
	}
	if ( bUploadOk && bUploadControl ) {
		bUploadOk = m_generatedControlMaps.update( pControl, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionControlUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pControl );
			m_nGpuRegionControlBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
	}
	if ( bUploadOk && bUploadColor ) {
		bUploadOk = m_generatedColorMaps.update( pColor, nLayer );
		if ( bUploadOk ) {
			++m_nGpuRegionColorUpdatesLastTick;
			const size_t bytes = terrain3dGpuImageBytes( pColor );
			m_nGpuRegionColorBytesLastTick += bytes;
			m_nGpuRegionUploadBytesLastTick += bytes;
		}
	}
	if ( bUploadOk && eMapType == REGION_TYPE_MAX ) {
		bUploadOk = m_generatedMiddleCompositeMaps.update( pMiddleComposite, nLayer );
		if ( bUploadOk ) {
			m_nGpuRegionUploadBytesLastTick += terrain3dGpuImageBytes( pMiddleComposite );
		}
	}
	if ( !bUploadOk ) {
		m_szGpuRegionSlotStates[nLayer] = previous_state;
		return false;
	}
	uint8_t published_flags = previous_channel_flags;
	if ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX ) {
		published_flags &= ( uint8_t ) ( ~REGION_CHANNEL_CONTROL );
		published_flags |= source_channel_flags & ( uint8_t ) ( REGION_CHANNEL_CONTROL );
	}
	if ( eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) {
		published_flags &= ( uint8_t ) ( ~REGION_CHANNEL_COLOR );
		published_flags |= source_channel_flags & ( uint8_t ) ( REGION_CHANNEL_COLOR );
	}
	m_szGpuRegionLayerChannelFlags[nLayer] = published_flags;
	uint32_t material_mask = bCookedPage ? cooked_material_mask :
		( pControl.is_valid() ? terrain3dMaterialMaskFromControlImage( pControl ) : 1u );
	if ( m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
			const int auto_base = CLAMP( terrain3dMaterialIntParam( pMaterial.ptr(), "auto_base_texture", 0 ), 0, 31 );
			const int auto_overlay = CLAMP( terrain3dMaterialIntParam( pMaterial.ptr(), "auto_overlay_texture", 1 ), 0, 31 );
			material_mask |= 1u << ( uint32_t ) ( auto_base );
			material_mask |= 1u << ( uint32_t ) ( auto_overlay );
		}
	}
	m_szGpuRegionMaterialMasks[nLayer] = material_mask;
	m_szGpuRegionSlotStates[nLayer] = m_szGpuRegionLocations[nLayer] == vLocation ?
		TERRAIN3D_GPU_SLOT_MAPPED : TERRAIN3D_GPU_SLOT_READY;
	return true;
}

bool T3DGpuRegionCache::isRuntimePageWanted( void *const pContext, const Vector2i &rLocation ) {
	const T3DResidencyFrameState *const pFrame = static_cast<const T3DResidencyFrameState *>( pContext );
	return pFrame && ( pFrame->streamingState( rLocation ) >= TERRAIN3D_REGION_STREAMING_HOT ||
		pFrame->isPrefetch( rLocation ) );
}

bool T3DGpuRegionCache::serviceRuntimePages( const T3DResidencyFrameState &rFrame ) {
	T3DRuntimePageServiceState state;
	state.config = rFrame.runtimePageConfig;
	state.predictedCenter = rFrame.predictedCenter;
	state.streamingGeneration = rFrame.streamingGeneration;
	state.pIsWanted = &T3DGpuRegionCache::isRuntimePageWanted;
	state.pWantedContext = const_cast<T3DResidencyFrameState *>( &rFrame );
	m_runtimePageStore.service( state );
	return true;
}

bool T3DGpuRegionCache::configure(
	const bool bEnabled, const int nBudgetMb,
	const int nUploadLayers, const int nUploadBudgetMb, const int nSparePages,
	const int nRetireFrames
) {
	const bool bOldEnabled = m_bGpuRegionCacheEnabled;
	const int old_budget = m_nGpuRegionCacheBudgetMb;
	const int old_upload_layers = m_nGpuRegionUploadBudgetLayers;
	const int old_upload_budget = m_nGpuRegionUploadBudgetMb;
	const int old_spare_pages = m_nGpuRegionSparePages;
	const int old_retire_frames = m_nGpuRegionRetireFrames;
	m_bGpuRegionCacheEnabled = bEnabled;
	m_nGpuRegionCacheBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_GPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_GPU_REGION_CACHE_MAX_BUDGET_MB );
	m_nGpuRegionUploadBudgetLayers = CLAMP( nUploadLayers, 1, TERRAIN3D_GPU_REGION_UPLOAD_MAX_LAYERS );
	m_nGpuRegionUploadBudgetMb = CLAMP( nUploadBudgetMb, 1, TERRAIN3D_GPU_REGION_UPLOAD_MAX_BUDGET_MB );
	m_nGpuRegionSparePages = CLAMP( nSparePages, TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS,
		TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS - 1 );
	m_nGpuRegionRetireFrames = CLAMP( nRetireFrames, 1, 16 );

	const int requested_pages = calculateGpuRegionRequestedPages();
	const int required_radius = calculateGpuRegionRequiredRadius();
	const int color_divisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	const int derived_divisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const int required_capacity = m_bGpuRegionCacheEnabled ? calculateGpuRegionCacheCapacity() : 0;
	const int required_physical_layers = m_bGpuRegionCacheEnabled ?
		calculateGpuRegionPhysicalLayers( required_capacity ) : 0;
	const bool color_channel_enabled = isColorChannelEnabled();
	const bool derived_channel_enabled = isDerivedChannelEnabled();
	const bool bLayoutChanged = m_bGpuRegionCacheEnabled && m_bGpuRegionCacheTexturesReady &&
		( required_capacity != m_nGpuRegionCacheCapacity || color_divisor != m_nGpuRegionColorDivisor ||
			derived_divisor != m_nGpuRegionDerivedDivisor ||
			color_channel_enabled != m_bGpuRegionColorChannelAllocated ||
			derived_channel_enabled != m_bGpuRegionDerivedChannelAllocated ||
			required_physical_layers != m_nGpuRegionPhysicalLayerCount );
	const bool bSelectionChanged = requested_pages != m_nGpuRegionRequestedPages ||
		required_radius != m_nGpuRegionRequiredRadius;
	const bool bStorageChanged = bOldEnabled != m_bGpuRegionCacheEnabled ||
		( m_bGpuRegionCacheEnabled && old_budget != m_nGpuRegionCacheBudgetMb ) || bLayoutChanged;
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

T3DGpuRegionCacheUpdateResult T3DGpuRegionCache::update( const T3DResidencyFrameState &rFrame, const bool bForce, const int nEffectiveUploadLayers, T3DCompute *pCompute ) {
	const T3DLastScopeTimer serviceTimer( m_nGpuRegionServiceUsecLast, rFrame.bProfilingEnabled );
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
	m_nGpuRegionRuntimeColorMipsLastTick = 0;
	m_nGpuRegionRuntimeDerivedBuildsLastTick = 0;
	const T3DRuntimePageStats runtime_page_stats = m_runtimePageStore.getStats();
	m_nGpuRegionPreparedPagesLastTick = runtime_page_stats.nPreparedPages;
	m_nGpuRegionPrepareWorkersLastTick = runtime_page_stats.nPrepareWorkers;
	m_nGpuRegionEffectiveUploadLimitLastTick = m_nGpuRegionUploadBudgetLayers;
	m_bGpuRegionCameraMovedLastTick = false;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	if ( !m_bGpuRegionCacheEnabled || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getVertexSpacing() <= 0.0f ) {
		return T3DGpuRegionCacheUpdateResult{};
	}
	++m_nGpuRegionTick;
	reclaimRetiredGpuRegionSlots();
	const uint64_t streaming_generation = rFrame.streamingGeneration;
	const Vector2i prediction_delta = rFrame.predictedCenter - rFrame.streamingCenter;
	m_gpuRegionPredictionDirection = Vector2i( SIGN( prediction_delta.x ), SIGN( prediction_delta.y ) );
	if ( !m_bGpuRegionCacheTexturesReady && !rebuildTextures() ) {
		return T3DGpuRegionCacheUpdateResult{};
	}
	const Vector2i center = rFrame.residencyCenter;
	m_bGpuRegionCameraMovedLastTick = m_gpuRegionCacheCenter != V2I_MAX &&
		center != m_gpuRegionCacheCenter;
	const int runtime_upload_budget = nEffectiveUploadLayers;
	const int effective_upload_limit = MAX( 1, runtime_upload_budget );
	m_nGpuRegionEffectiveUploadLimitLastTick = effective_upload_limit;
	const int deferred_republish_count = processPendingGpuRegionRepublishes( effective_upload_limit );
	if ( deferred_republish_count > 0 ) {
		m_nGpuRegionUploadsLastTick += deferred_republish_count;
		m_nGpuRegionUploadsSinceReset += ( uint64_t ) ( deferred_republish_count );
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	if ( !bForce && center == m_gpuRegionCacheCenter && !m_bGpuRegionCacheSelectionDirty &&
		streaming_generation == m_nGpuRegionStreamingGenerationLast ) {
		if ( deferred_republish_count > 0 ) {
			notePageTableChanged();
		}
		return T3DGpuRegionCacheUpdateResult{m_bPageTableChangedThisTick};
	}

	Vector2i desired[TERRAIN3D_GPU_CACHE_MAX_LAYERS];
	t3dGpuCacheSelectionStats_t selection_stats;
	int desired_count = collectDesiredGpuRegions( rFrame, center, desired, m_nGpuRegionCacheCapacity, selection_stats );
	const int footprint_count = desired_count;
	m_nGpuRegionRequiredLastTick = selection_stats.required;
	m_nGpuRegionGuardLastTick = selection_stats.guard;
	m_nGpuRegionPrefetchLastTick = selection_stats.prefetch;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = T3DRegionRepository::getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
		const int layer = findGpuRegionSlot( desired[index] );
		if ( layer >= 0 ) {
			m_szGpuRegionLastUseTick[layer] = m_nGpuRegionTick;
			if ( index < selection_stats.required && m_szGpuRegionPrefetchOrigin[layer] != 0u ) {
				++m_nGpuRegionPrefetchUsedTotal;
				m_szGpuRegionPrefetchOrigin[layer] = 0u;
			}
		}
	}

	const uint64_t hysteresis_ticks = ( uint64_t ) ( MAX( 0, rFrame.nGpuHysteresisTicks ) );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount && desired_count < m_nGpuRegionCacheCapacity; ++layer ) {
		const Vector2i location = m_szGpuRegionLocations[layer];
		if ( location == V2I_MAX || m_szGpuRegionLastUseTick[layer] == 0u ||
			m_nGpuRegionTick - m_szGpuRegionLastUseTick[layer] > hysteresis_ticks ) {
			continue;
		}
		const int map_index = T3DRegionRepository::getRegionMapIndex( location );
		if ( map_index < 0 || desired_map[map_index] != 0u ) {
			continue;
		}
		desired[desired_count++] = location;
		desired_map[map_index] = 1u;
		++m_nGpuRegionHysteresisLastTick;
	}
	m_nGpuRegionDesiredLastTick = desired_count;
	m_nGpuRegionPriorityCount = MIN( desired_count, TERRAIN3D_GPU_CACHE_MAX_LAYERS );
	const int guard_end = selection_stats.required + selection_stats.guard;
	const int priority_prefetch_end = guard_end + selection_stats.prefetch;
	for ( int index = 0; index < m_nGpuRegionPriorityCount; ++index ) {
		m_szGpuRegionPriorityLocations[index] = desired[index];
		if ( index < selection_stats.required ) {
			m_szGpuRegionPriorityClasses[index] = 1u;
		} else if ( index < guard_end ) {
			m_szGpuRegionPriorityClasses[index] = 2u;
		} else if ( index < priority_prefetch_end ) {
			m_szGpuRegionPriorityClasses[index] = 3u;
		} else {
			m_szGpuRegionPriorityClasses[index] = 4u;
		}
	}
	for ( int index = m_nGpuRegionPriorityCount; index < TERRAIN3D_GPU_CACHE_MAX_LAYERS; ++index ) {
		m_szGpuRegionPriorityLocations[index] = V2I_MAX;
		m_szGpuRegionPriorityClasses[index] = 0u;
	}

	if ( rFrame.bCookedPagesEnabled ) {
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findGpuRegionSlot( desired[index] ) >= 0 ) {
				continue;
			}
			T3DRegion *const pRegion = m_regionRepository.getRegionPtr( desired[index] );
			if ( pRegion && !pRegion->isDeleted() && pRegion->runtimeMapsEvictableInternal() ) {
				const uint8_t priority = index < m_nGpuRegionPriorityCount ?
					m_szGpuRegionPriorityClasses[index] : 4u;
				m_runtimePageStore.request( desired[index], RUNTIME_PAGE_CONSUMER_GPU_DETAIL, priority, rFrame.bCookedPagesEnabled );
			}
		}
		serviceRuntimePages( rFrame );
	}

	bool bMapChanged = false;
	bool bAllDesiredResident = true;
	const size_t byte_budget = ( size_t ) ( m_nGpuRegionUploadBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	const int update_call_budget = MAX( 5, effective_upload_limit * 3 + 1 );
	for ( int desired_index = 0; desired_index < desired_count; ++desired_index ) {
		const Vector2i location = desired[desired_index];
		const int existing_layer = findGpuRegionSlot( location );
		if ( existing_layer >= 0 ) {
			m_szGpuRegionLastUseTick[existing_layer] = m_nGpuRegionTick;
			continue;
		}
		bAllDesiredResident = false;
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
		T3DRegion *const pUploadRegion = m_regionRepository.getRegionPtr( location );
		if ( !pUploadRegion || pUploadRegion->isDeleted() ) {
			continue;
		}
		const size_t expected_bytes = estimateGpuRegionUploadBytes( pUploadRegion, target_layer );
		const int expected_calls = estimateGpuRegionUploadCalls( pUploadRegion, target_layer );
		const int page_table_reserve = bMapChanged ? 0 : 1;
		if ( m_nGpuRegionUploadsLastTick > 0 &&
			m_nGpuRegionUploadBytesLastTick + expected_bytes > byte_budget ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPLOAD_BYTE_BUDGET;
			break;
		}
		if ( m_nGpuRegionUploadsLastTick > 0 &&
			getGpuRegionTextureUpdateCallsLastTick() + expected_calls + page_table_reserve >
				update_call_budget ) {
			m_nGpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_GPU_UPDATE_CALL_BUDGET;
			break;
		}

		int victim_layer = -1;
		if ( m_nGpuRegionResidentCount >= m_nGpuRegionCacheCapacity ) {
			uint64_t oldest_tick = std::numeric_limits<uint64_t>::max();
			int farthest_distance = -1;
			for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
				const Vector2i old_location = m_szGpuRegionLocations[layer];
				if ( old_location == V2I_MAX || m_szGpuRegionSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
					continue;
				}
				const int old_map_index = T3DRegionRepository::getRegionMapIndex( old_location );
				if ( old_map_index >= 0 && desired_map[old_map_index] != 0u ) {
					continue;
				}
				const int distance = MAX( std::abs( old_location.x - center.x ), std::abs( old_location.y - center.y ) );
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

		if ( victim_layer >= 0 ) {
			const Vector2i old_location = m_szGpuRegionLocations[victim_layer];
			const int old_map_index = T3DRegionRepository::getRegionMapIndex( old_location );
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
		const int prefetch_begin = selection_stats.required + selection_stats.guard;
		const int prefetch_end = prefetch_begin + selection_stats.prefetch;
		m_szGpuRegionPrefetchOrigin[target_layer] = desired_index >= prefetch_begin &&
			desired_index < prefetch_end ? 1u : 0u;
		m_szGpuRegionSlotStates[target_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int new_map_index = T3DRegionRepository::getRegionMapIndex( location );
		if ( new_map_index >= 0 ) {
			m_szGpuRegionMap[new_map_index] = ( uint16_t ) ( target_layer + 1 );
		}
		++m_nGpuRegionUploadsLastTick;
		++m_nGpuRegionUploadsSinceReset;
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
		bMapChanged = true;
	}

	int resident_required = 0;
	const int written_required = MIN( selection_stats.required, footprint_count );
	for ( int index = 0; index < written_required; ++index ) {
		if ( findGpuRegionSlot( desired[index] ) >= 0 ) {
			++resident_required;
		}
	}
	m_nGpuRegionMissingRequiredLastTick = selection_stats.required - resident_required;
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
	m_bGpuRegionCacheSelectionDirty = !bAllDesiredResident || m_nGpuRegionMissingRequiredLastTick > 0 ||
		m_nGpuRegionRepublishPendingCount > 0;
	if ( bMapChanged || deferred_republish_count > 0 ) {
		notePageTableChanged();
	}
	return T3DGpuRegionCacheUpdateResult{m_bPageTableChangedThisTick};
}


bool T3DGpuRegionCache::updateResidentLocation(
	const Vector2i &vLocation, const t3dMapType_t eMapType, T3DCompute *pCompute
) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		m_bGpuRegionCacheSelectionDirty = true;
		return false;
	}
	if ( eMapType == REGION_TYPE_MAX ) {
		const bool changed = republish( vLocation, m_pCompute );
		if ( changed ) {
			notePageTableChanged();
		}
		return changed;
	}
	const bool updated = uploadGpuRegionLayer( layer, vLocation, eMapType );
	if ( updated ) {
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	return updated;
}

bool T3DGpuRegionCache::republishAllResidents( T3DCompute *pCompute ) {
	m_pCompute = pCompute ? pCompute : m_pCompute;
	Vector2i locations[TERRAIN3D_GPU_CACHE_MAX_LAYERS];
	int count = 0;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED &&
			m_szGpuRegionLocations[layer] != V2I_MAX ) {
			locations[count++] = m_szGpuRegionLocations[layer];
		}
	}
	bool changed = false;
	for ( int index = 0; index < count; ++index ) {
		changed = republish( locations[index], m_pCompute ) || changed;
	}
	if ( changed ) {
		notePageTableChanged();
	}
	return changed;
}

TypedArray<Vector2i> T3DGpuRegionCache::getRenderRegionSlots( void ) const {
	TypedArray<Vector2i> slots;
	slots.resize( m_nGpuRegionPhysicalLayerCount );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		slots[layer] = m_szGpuRegionLocations[layer];
	}
	return slots;
}

int T3DGpuRegionCache::getSlotStateForLocation( const Vector2i &vLocation ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionLocations[layer] == vLocation ) {
			return ( int ) ( m_szGpuRegionSlotStates[layer] );
		}
	}
	return ( int ) ( TERRAIN3D_GPU_SLOT_FREE );
}

bool T3DGpuRegionCache::isAuthoringFullResidencyPossible( void ) const {
	return !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ||
		calculateGpuRegionCacheCapacity() >= m_regionRepository.getActiveRegionCount();
}

bool T3DGpuRegionCache::isAuthoringReadbackSafe( void ) const {
	if ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() || !m_bGpuRegionCacheEnabled ) {
		return true;
	}
	if ( !m_bGpuRegionCacheTexturesReady || m_runtimePageStore.isPrepareJobActive() ||
		m_bGpuRegionCacheSelectionDirty || m_nGpuRegionUploadsLastTick > 0 ) {
		return false;
	}
	return m_nGpuRegionTick >= m_nGpuRegionLastPublicationTick + 2u;
}

size_t T3DGpuRegionCache::getReservedBytes( void ) const {
	const size_t far_height_side = ( size_t ) ( m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
	const size_t far_surface_side = ( size_t ) ( m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
	const size_t fixed_far_bytes = far_height_side * far_height_side * 2u + far_surface_side * far_surface_side * 4u;
	const size_t shared_bytes = estimateSharedBytes();
	return ( shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u ) +
		estimateLayerBytes() * ( size_t ) ( MAX( 0, m_nGpuRegionPhysicalLayerCount ) );
}

size_t T3DGpuRegionCache::getMinimumBytes( void ) const {
	const size_t far_height_side = ( size_t ) ( m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
	const size_t far_surface_side = ( size_t ) ( m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
	const size_t fixed_far_bytes = far_height_side * far_height_side * 2u + far_surface_side * far_surface_side * 4u;
	const size_t shared_bytes = estimateSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateLayerBytes() * ( size_t ) ( TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS + 1 );
}

size_t T3DGpuRegionCache::getHardCapBytes( void ) const {
	const size_t far_height_side = ( size_t ) ( m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
	const size_t far_surface_side = ( size_t ) ( m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
	const size_t fixed_far_bytes = far_height_side * far_height_side * 2u + far_surface_side * far_surface_side * 4u;
	const size_t shared_bytes = estimateSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateLayerBytes() * ( size_t ) ( TERRAIN3D_GPU_CACHE_MAX_LAYERS );
}

T3DGpuRegionCacheStats T3DGpuRegionCache::getStats( void ) const {
	T3DGpuRegionCacheStats stats;
	stats.bEnabled = m_bGpuRegionCacheEnabled;
	stats.bTexturesReady = m_bGpuRegionCacheTexturesReady;
	stats.bSelectionPending = m_bGpuRegionCacheSelectionDirty;
	stats.bCameraMovedLastTick = m_bGpuRegionCameraMovedLastTick;
	stats.nBudgetMb = m_nGpuRegionCacheBudgetMb;
	stats.nCapacity = m_nGpuRegionCacheCapacity;
	stats.nPhysicalLayers = m_nGpuRegionPhysicalLayerCount;
	stats.nSpareLayers = m_nGpuRegionSpareLayerCount;
	stats.nResident = m_nGpuRegionResidentCount;
	stats.nRetired = m_nGpuRegionRetiredCount;
	stats.nDesiredLastTick = m_nGpuRegionDesiredLastTick;
	stats.nRequiredLastTick = m_nGpuRegionRequiredLastTick;
	stats.nGuardLastTick = m_nGpuRegionGuardLastTick;
	stats.nPrefetchLastTick = m_nGpuRegionPrefetchLastTick;
	stats.nHysteresisLastTick = m_nGpuRegionHysteresisLastTick;
	stats.nMissingRequiredLastTick = m_nGpuRegionMissingRequiredLastTick;
	stats.nRequestedPages = m_nGpuRegionRequestedPages;
	stats.nRequiredRadius = m_nGpuRegionRequiredRadius;
	stats.nUploadsLastTick = m_nGpuRegionUploadsLastTick;
	stats.nEvictionsLastTick = m_nGpuRegionEvictionsLastTick;
	stats.nCookedHitsLastTick = m_nGpuRegionCookedHitsLastTick;
	stats.nCookedMissesLastTick = m_nGpuRegionCookedMissesLastTick;
	stats.nRuntimePreparesLastTick = m_nGpuRegionRuntimePreparesLastTick;
	stats.nReusedLayersLastTick = m_nGpuRegionReusedLayersLastTick;
	stats.nPageTableUpdatesLastTick = m_nGpuRegionPageTableUpdatesLastTick;
	stats.nHeightUpdatesLastTick = m_nGpuRegionHeightUpdatesLastTick;
	stats.nControlUpdatesLastTick = m_nGpuRegionControlUpdatesLastTick;
	stats.nColorUpdatesLastTick = m_nGpuRegionColorUpdatesLastTick;
	stats.nDerivedUpdatesLastTick = m_nGpuRegionDerivedUpdatesLastTick;
	stats.nRuntimeColorMipsLastTick = m_nGpuRegionRuntimeColorMipsLastTick;
	stats.nRuntimeDerivedBuildsLastTick = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats.nPreparedPagesLastTick = m_nGpuRegionPreparedPagesLastTick;
	stats.nPrepareWorkersLastTick = m_nGpuRegionPrepareWorkersLastTick;
	stats.nEffectiveUploadLimitLastTick = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats.nStallFlagsLastTick = m_nGpuRegionStallFlagsLastTick;
	stats.nUploadBytesLastTick = m_nGpuRegionUploadBytesLastTick;
	stats.nHeightBytesLastTick = m_nGpuRegionHeightBytesLastTick;
	stats.nControlBytesLastTick = m_nGpuRegionControlBytesLastTick;
	stats.nColorBytesLastTick = m_nGpuRegionColorBytesLastTick;
	stats.nDerivedBytesLastTick = m_nGpuRegionDerivedBytesLastTick;
	stats.nServiceUsecLast = m_nGpuRegionServiceUsecLast;
	stats.nPreparePublicationUsecLast = m_nGpuPreparePublicationUsecLast;
	stats.nPrepareEndToEndUsecLast = m_nGpuPrepareEndToEndUsecLast;
	return stats;
}

Dictionary T3DGpuRegionCache::getStatsDictionary( const bool bDetailed ) const {
	Dictionary stats;
	const T3DRuntimePageStats runtime = m_runtimePageStore.getStats();
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["budget_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["allocation_budget_mb"] = m_pTerrain && m_pTerrain->isEditorAuthoringMode() ?
		calculateAuthoringGpuRegionBudgetMb() : m_nGpuRegionCacheBudgetMb;
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
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nGpuRegionStallFlagsLastTick );
	stats["upload_layer_budget"] = m_nGpuRegionUploadBudgetLayers;
	stats["upload_budget_mb"] = m_nGpuRegionUploadBudgetMb;
	stats["effective_upload_limit_last_tick"] = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t ) ( m_nGpuRegionUploadsSinceReset );
	stats["evictions_last_tick"] = m_nGpuRegionEvictionsLastTick;
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["page_table_updates_last_tick"] = m_nGpuRegionPageTableUpdatesLastTick;
	stats["height_updates_last_tick"] = m_nGpuRegionHeightUpdatesLastTick;
	stats["control_updates_last_tick"] = m_nGpuRegionControlUpdatesLastTick;
	stats["color_updates_last_tick"] = m_nGpuRegionColorUpdatesLastTick;
	stats["derived_updates_last_tick"] = m_nGpuRegionDerivedUpdatesLastTick;
	stats["runtime_color_mips_last_tick"] = m_nGpuRegionRuntimeColorMipsLastTick;
	stats["runtime_derived_builds_last_tick"] = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats["prepare_job_active"] = runtime.bPrepareJobActive;
	stats["prepare_job_pending_pages"] = runtime.nPendingPages;
	stats["prepare_ready_pages"] = runtime.nPreparedPages;
	stats["prepare_publication_usec_last"] = ( int64_t ) ( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t ) ( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nGpuRegionServiceUsecLast );
	stats["upload_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionUploadBytesLastTick );
	stats["reserved_bytes"] = ( int64_t ) ( getReservedBytes() );
	stats["estimated_layer_bytes"] = ( int64_t ) ( estimateLayerBytes() );
	stats["estimated_shared_bytes"] = ( int64_t ) ( estimateSharedBytes() );
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
	stats["streaming_generation_last"] = ( int64_t ) ( m_nGpuRegionStreamingGenerationLast );
	stats["prefetch_used_total"] = ( int64_t ) ( m_nGpuRegionPrefetchUsedTotal );
	stats["prefetch_discarded_total"] = ( int64_t ) ( m_nGpuRegionPrefetchDiscardedTotal );
	const uint64_t completed_prefetches = m_nGpuRegionPrefetchUsedTotal + m_nGpuRegionPrefetchDiscardedTotal;
	stats["prefetch_accuracy"] = completed_prefetches > 0u ?
		( double ) ( m_nGpuRegionPrefetchUsedTotal ) / ( double ) ( completed_prefetches ) : -1.0;
	stats["departed_pages_total"] = ( int64_t ) ( m_nGpuRegionDepartedPagesTotal );
	stats["average_resident_ticks"] = m_nGpuRegionDepartedPagesTotal > 0u ?
		( double ) ( m_nGpuRegionResidentTicksTotal ) / ( double ) ( m_nGpuRegionDepartedPagesTotal ) : 0.0;
	return stats;
}
