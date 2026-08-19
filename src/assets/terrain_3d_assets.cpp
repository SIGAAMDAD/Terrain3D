// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include <algorithm>
#include <limits>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

#include "eastl_config.h"
#include <EASTL/bitset.h>
#include <vector>

#include "core/logger.h"
#include "assets/terrain_3d_assets.h"
#include "core/terrain_3d_owner.h"
#include "core/terrain_3d_util.h"

///////////////////////////
// Private Functions
///////////////////////////

namespace {

static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_MAGIC = 0x5433444D41543031ull; // T3DMAT01
static constexpr uint32_t TERRAIN3D_RUNTIME_MATERIAL_VERSION = 3u;
static constexpr int TERRAIN3D_RUNTIME_MATERIAL_SPARE_LAYERS = 2;
static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_HEADER_BYTES = 32u;
static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_IMAGE_DESCRIPTOR_BYTES = 32u;
static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_COOKED_CHANNELS = 5u;
static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_LAYER_DESCRIPTOR_BYTES =
	TERRAIN3D_RUNTIME_MATERIAL_COOKED_CHANNELS * TERRAIN3D_RUNTIME_MATERIAL_IMAGE_DESCRIPTOR_BYTES + 16u;
static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_DESCRIPTOR_TABLE_BYTES =
	TERRAIN3D_MAX_TEXTURE_ASSETS * TERRAIN3D_RUNTIME_MATERIAL_LAYER_DESCRIPTOR_BYTES;
static constexpr uint64_t TERRAIN3D_RUNTIME_MATERIAL_DATA_OFFSET =
	TERRAIN3D_RUNTIME_MATERIAL_HEADER_BYTES + TERRAIN3D_RUNTIME_MATERIAL_DESCRIPTOR_TABLE_BYTES;

static uint32_t terrain3dMaterialChecksum( const uint8_t *const pData, const size_t nSize ) {
	uint32_t hash = 2166136261u;
	for ( size_t index = 0; index < nSize; ++index ) {
		hash ^= ( uint32_t ) ( pData[index] );
		hash *= 16777619u;
	}
	return hash;
}

static void terrain3dMaterialHashU64( uint64_t &rHash, const uint64_t nValue ) {
	for ( int shift = 0; shift < 64; shift += 8 ) {
		rHash ^= ( nValue >> shift ) & 0xffu;
		rHash *= 1099511628211ull;
	}
}

static Color terrain3dMaterialAverageAlbedo( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return COLOR_WHITE;
	}
	Ref<Image> pImage = rImage->duplicate();
	if ( pImage->is_compressed() && pImage->decompress() != OK ) {
		return COLOR_WHITE;
	}
	if ( pImage->has_mipmaps() ) {
		pImage->clear_mipmaps();
	}
	pImage->resize( 1, 1, Image::INTERPOLATE_LANCZOS );
	return pImage->get_pixel( 0, 0 );
}

static bool terrain3dIsNativePackedSurfaceFormat( const Image::Format format ) {
	// Terrain3D's optimized surface uses all four channels (normal XY, roughness, AO).
	// BC3 and BC7 preserve those channels and are directly usable by the sampler.
	return format == Image::FORMAT_RGBA8 || format == Image::FORMAT_DXT5 ||
		format == Image::FORMAT_BPTC_RGBA;
}

static bool terrain3dIsSupportedNativeArrayFormat( const Image::Format format ) {
	return format == Image::FORMAT_DXT1 || format == Image::FORMAT_DXT5 ||
		format == Image::FORMAT_BPTC_RGBA || format == Image::FORMAT_RGTC_R ||
		format == Image::FORMAT_RGTC_RG;
}

static Error terrain3dCompressArrayImageToFormat( const Ref<Image> &rImage, const Image::Format format ) {
	if ( rImage.is_null() ) {
		return ERR_INVALID_PARAMETER;
	}
	switch ( format ) {
		case Image::FORMAT_DXT1:
			rImage->convert( Image::FORMAT_RGB8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RGB );
		case Image::FORMAT_DXT5:
			rImage->convert( Image::FORMAT_RGBA8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RGBA );
		case Image::FORMAT_RGTC_R:
			rImage->convert( Image::FORMAT_R8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_R );
		case Image::FORMAT_RGTC_RG:
			rImage->convert( Image::FORMAT_RG8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RG );
		case Image::FORMAT_BPTC_RGBA:
			rImage->convert( Image::FORMAT_RGBA8 );
			return rImage->compress( Image::COMPRESS_BPTC, Image::COMPRESS_SOURCE_GENERIC );
		default:
			return ERR_UNAVAILABLE;
	}
}

static Ref<Image> terrain3dMakeArrayDefault(
	const Vector2i &rSize, const Image::Format format,
	const bool bMipmaps, const Color &rDefaultColor
) {
	if ( !terrain3dIsSupportedNativeArrayFormat( format ) ) {
		return Util::getFilledImage( rSize, rDefaultColor, bMipmaps, format );
	}
	Image::Format working_format = Image::FORMAT_RGBA8;
	if ( format == Image::FORMAT_DXT1 ) {
		working_format = Image::FORMAT_RGB8;
	} else if ( format == Image::FORMAT_RGTC_R ) {
		working_format = Image::FORMAT_R8;
	} else if ( format == Image::FORMAT_RGTC_RG ) {
		working_format = Image::FORMAT_RG8;
	}
	Ref<Image> pImage = Util::getFilledImage( rSize, rDefaultColor, bMipmaps, working_format );
	if ( pImage.is_null() || terrain3dCompressArrayImageToFormat( pImage, format ) != OK ||
		pImage->get_format() != format ) {
		return Ref<Image>();
	}
	return pImage;
}

static Ref<Image> terrain3dPrepareArrayImage(
	const Ref<Texture2D> &rTexture, const Vector2i &rSize,
	const Image::Format format, const bool bMipmaps, const Color &rDefaultColor
) {
	if ( rTexture.is_null() ) {
		return terrain3dMakeArrayDefault( rSize, format, bMipmaps, rDefaultColor );
	}
	Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		return Ref<Image>();
	}
	if ( pImage->get_size() == rSize && pImage->get_format() == format && pImage->has_mipmaps() == bMipmaps ) {
		// Preserve a native DDS/VRAM-compressed layer byte-for-byte. This is the
		// zero-expansion path used by production texture arrays.
		return pImage;
	}
	pImage = pImage->duplicate();
	if ( pImage->is_compressed() && pImage->decompress() != OK ) {
		return Ref<Image>();
	}
	if ( pImage->has_mipmaps() ) {
		pImage->clear_mipmaps();
	}
	if ( pImage->get_size() != rSize ) {
		pImage->resize( rSize.x, rSize.y, Image::INTERPOLATE_LANCZOS );
	}
	if ( terrain3dIsSupportedNativeArrayFormat( format ) ) {
		if ( bMipmaps && pImage->generate_mipmaps() != OK ) {
			return Ref<Image>();
		}
		if ( terrain3dCompressArrayImageToFormat( pImage, format ) != OK ) {
			return Ref<Image>();
		}
	} else {
		if ( pImage->get_format() != format ) {
			pImage->convert( format );
		}
		if ( bMipmaps && pImage->generate_mipmaps() != OK ) {
			return Ref<Image>();
		}
	}
	return pImage->get_size() == rSize && pImage->get_format() == format && pImage->has_mipmaps() == bMipmaps ?
		pImage : Ref<Image>();
}

static void terrain3dReleaseGeneratedReplacements(
	T3DGeneratedTexture &rAlbedo, T3DGeneratedTexture &rNormal,
	T3DGeneratedTexture &rSurface, T3DGeneratedTexture &rDisplacement
) {
	rAlbedo.clear();
	rNormal.clear();
	rSurface.clear();
	rDisplacement.clear();
}

} // namespace

String T3DAssets::getRuntimeMaterialCachePath( void ) const {
	if ( !m_pTerrain || m_pTerrain->getDataDirectory().is_empty() ) {
		return String();
	}
	return m_pTerrain->getDataDirectory().path_join( "terrain3d_materials.t3dmat" );
}

uint64_t T3DAssets::computeRuntimeMaterialSourceSignature( void ) const {
	uint64_t hash = 1469598103934665603ull;
	terrain3dMaterialHashU64( hash, ( uint64_t ) ( m_texturePackingMode ) );
	terrain3dMaterialHashU64( hash, ( uint64_t ) ( m_textureList.size() ) );
	terrain3dMaterialHashU64( hash, m_pTerrain ? ( uint64_t ) ( m_pTerrain->getFarMaterialResolution() ) : 128u );
	for ( int index = 0; index < m_textureList.size(); ++index ) {
		const Ref<T3DTextureAsset> pAsset = m_textureList[index];
		terrain3dMaterialHashU64( hash, ( uint64_t ) ( index ) );
		if ( pAsset.is_null() ) {
			continue;
		}
		auto append_resource = [&]( const auto &rResource ) {
			if ( rResource.is_null() ) {
				terrain3dMaterialHashU64( hash, 0u );
				return;
			}
			const String path = rResource->get_path();
			terrain3dMaterialHashU64( hash, ( uint64_t ) ( path.hash() ) );
			terrain3dMaterialHashU64( hash, !path.is_empty() && FileAccess::file_exists( path ) ?
				FileAccess::get_modified_time( path ) : 0u );
		};
		append_resource( pAsset->m_pAlbedoTexture );
		append_resource( pAsset->m_pNormalTexture );
		append_resource( pAsset->m_pSurfaceTexture );
		append_resource( pAsset->m_pDisplacementTexture );
		append_resource( pAsset->m_pPackedPbrResource );
	}
	return hash;
}

void T3DAssets::resetRuntimeMaterialCacheState( const bool bClearGeneratedArrays ) {
	m_bRuntimeMaterialCacheActive = false;
	m_bRuntimeMaterialCookedValid = false;
	m_runtimeMaterialCachePath = String();
	m_nRuntimeMaterialCookedSignature = 0u;
	m_nRuntimeMaterialTick = 0u;
	m_nRuntimeMaterialPhysicalLayers = 0;
	m_nRuntimeMaterialResidentCount = 0;
	m_nRuntimeMaterialUploadsLastTick = 0;
	m_nRuntimeMaterialEvictionsLastTick = 0;
	m_bRuntimeMaterialSourceTexturesReleased = false;
	for ( int index = 0; index < TERRAIN3D_MAX_TEXTURE_ASSETS; ++index ) {
		m_runtimeMaterialLogicalToPhysical[( size_t ) ( index )] = -1;
		m_runtimeMaterialPhysicalToLogical[( size_t ) ( index )] = -1;
		m_runtimeMaterialLastUseTick[( size_t ) ( index )] = 0u;
		m_runtimeMaterialRetireAfterTick[( size_t ) ( index )] = 0u;
		m_runtimeMaterialCookedLayers[( size_t ) ( index )] = t3druntimeMaterialLayerInfo_t{};
	}
	if ( bClearGeneratedArrays ) {
		m_generatedAlbedoTextures.clear();
		m_generatedNormalTextures.clear();
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
	}
	m_generatedFarAlbedoTextures.clear();
}

bool T3DAssets::initializeFarMaterialArray( void ) {
	if ( !m_bRuntimeMaterialCookedValid || m_textureList.is_empty() ) {
		return false;
	}
	TypedArray<Image> layers;
	layers.resize( m_textureList.size() );
	for ( int logical = 0; logical < m_textureList.size(); ++logical ) {
		const Ref<Image> pImage = loadRuntimeMaterialCookedImage( logical, RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO );
		if ( pImage.is_null() ) {
			return false;
		}
		layers[logical] = pImage;
	}
	T3DGeneratedTexture replacement;
	if ( !replacement.create( layers ).is_valid() ) {
		return false;
	}
	m_generatedFarAlbedoTextures.swap( replacement );
	replacement.clear();
	return true;
}

bool T3DAssets::loadRuntimeMaterialCookedIndex( void ) {
	const String path = getRuntimeMaterialCachePath();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < TERRAIN3D_RUNTIME_MATERIAL_DATA_OFFSET ) {
		return false;
	}
	const uint64_t magic = pFile->get_64();
	const uint32_t version = pFile->get_32();
	const uint32_t texture_count = pFile->get_32();
	const uint32_t packing_mode = pFile->get_32();
	const uint32_t flags = pFile->get_32();
	const uint64_t signature = pFile->get_64();
	( void ) ( flags );
	bool source_resources_available = false;
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		if ( pAsset.is_valid() && ( pAsset->m_pAlbedoTexture.is_valid() || pAsset->m_pNormalTexture.is_valid() ||
			pAsset->m_pSurfaceTexture.is_valid() || pAsset->m_pDisplacementTexture.is_valid() ||
			pAsset->m_pPackedPbrResource.is_valid() ) ) {
			source_resources_available = true;
			break;
		}
	}
	const bool signature_valid = !source_resources_available && !IS_EDITOR ? true :
		signature == computeRuntimeMaterialSourceSignature();
	if ( magic != TERRAIN3D_RUNTIME_MATERIAL_MAGIC || version != TERRAIN3D_RUNTIME_MATERIAL_VERSION ||
		texture_count != ( uint32_t ) ( m_textureList.size() ) || texture_count == 0u ||
		texture_count > TERRAIN3D_MAX_TEXTURE_ASSETS || packing_mode != ( uint32_t ) ( m_texturePackingMode ) ||
		signature == 0u || !signature_valid ) {
		return false;
	}
	const uint64_t file_length = pFile->get_length();
	eastl::array<t3druntimeMaterialLayerInfo_t, TERRAIN3D_MAX_TEXTURE_ASSETS> loaded_layers{};
	for ( int logical = 0; logical < TERRAIN3D_MAX_TEXTURE_ASSETS; ++logical ) {
		for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
			t3druntimeMaterialImageInfo_t &info = loaded_layers[( size_t ) ( logical )].channels[( size_t ) ( channel )];
			info.nOffset = pFile->get_64();
			info.nSize = pFile->get_32();
			info.nChecksum = pFile->get_32();
			info.nWidth = ( int32_t ) ( pFile->get_32() );
			info.nHeight = ( int32_t ) ( pFile->get_32() );
			info.nFormat = ( int32_t ) ( pFile->get_32() );
			info.bMipmaps = pFile->get_32();
			if ( info.nSize > 0u && ( logical >= ( int ) ( texture_count ) ||
				info.nOffset < TERRAIN3D_RUNTIME_MATERIAL_DATA_OFFSET || info.nOffset > file_length ||
				( uint64_t ) ( info.nSize ) > file_length - info.nOffset ||
				info.nWidth <= 0 || info.nHeight <= 0 || info.nFormat < 0 ||
				info.nFormat >= ( int32_t ) ( Image::FORMAT_MAX ) || info.bMipmaps > 1u ) ) {
				return false;
			}
		}
		loaded_layers[( size_t ) ( logical )].averageAlbedo = Color(
			pFile->get_float(), pFile->get_float(), pFile->get_float(), pFile->get_float() );
	}
	for ( uint32_t logical = 0u; logical < texture_count; ++logical ) {
		const t3druntimeMaterialLayerInfo_t &layer = loaded_layers[( size_t ) ( logical )];
		if ( layer.channels[RUNTIME_MATERIAL_CHANNEL_ALBEDO].nSize == 0u ||
			layer.channels[RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO].nSize == 0u ||
			( m_texturePackingMode == TEXTURE_PACKING_LEGACY &&
				layer.channels[RUNTIME_MATERIAL_CHANNEL_NORMAL].nSize == 0u ) ||
			( m_texturePackingMode != TEXTURE_PACKING_LEGACY &&
				layer.channels[RUNTIME_MATERIAL_CHANNEL_SURFACE].nSize == 0u ) ) {
			return false;
		}
	}
	// Publish the index only after the entire descriptor table is valid. A failed
	// reload must never leave a partially overwritten live cache snapshot behind.
	m_runtimeMaterialCookedLayers = loaded_layers;
	m_runtimeMaterialCachePath = path;
	m_nRuntimeMaterialCookedSignature = signature;
	m_bRuntimeMaterialCookedValid = true;
	return true;
}

Ref<Image> T3DAssets::loadRuntimeMaterialCookedImage( const int nLogicalId, const int nChannel ) const {
	if ( !m_bRuntimeMaterialCookedValid || nLogicalId < 0 || nLogicalId >= m_textureList.size() ||
		nChannel < 0 || nChannel >= RUNTIME_MATERIAL_CHANNEL_COUNT ) {
		return Ref<Image>();
	}
	const t3druntimeMaterialImageInfo_t &info =
		m_runtimeMaterialCookedLayers[( size_t ) ( nLogicalId )].channels[( size_t ) ( nChannel )];
	if ( info.nSize == 0u ) {
		return Ref<Image>();
	}
	const Ref<FileAccess> pFile = FileAccess::open( m_runtimeMaterialCachePath, FileAccess::READ );
	if ( pFile.is_null() ) {
		return Ref<Image>();
	}
	pFile->seek( info.nOffset );
	const PackedByteArray bytes = pFile->get_buffer( info.nSize );
	if ( bytes.size() != ( int64_t ) ( info.nSize ) ||
		terrain3dMaterialChecksum( bytes.ptr(), ( size_t ) ( bytes.size() ) ) != info.nChecksum ) {
		return Ref<Image>();
	}
	return Image::create_from_data( info.nWidth, info.nHeight, info.bMipmaps != 0u,
		( Image::Format ) ( info.nFormat ), bytes );
}

bool T3DAssets::initializeRuntimeMaterialPhysicalArrays( const int nLayerLimit ) {
	if ( !m_bRuntimeMaterialCookedValid || !m_pTerrain || m_textureList.is_empty() ) {
		return false;
	}
	const int cache_limit = nLayerLimit > 0 ? CLAMP( nLayerLimit, 1, TERRAIN3D_MAX_TEXTURE_ASSETS ) :
		CLAMP( m_pTerrain->getRuntimeMaterialCacheLayers(), 4, TERRAIN3D_MAX_TEXTURE_ASSETS );
	const int physical_layers = MIN( m_textureList.size(), cache_limit );
	const bool fully_resident = m_textureList.size() <= cache_limit;
	if ( physical_layers <= 0 || !initializeFarMaterialArray() ) {
		return false;
	}

	T3DGeneratedTexture replacement_albedo;
	T3DGeneratedTexture replacement_normal;
	T3DGeneratedTexture replacement_surface;
	T3DGeneratedTexture replacement_displacement;
	auto create_channel_array = [&]( T3DGeneratedTexture &rGenerated, const int channel ) -> bool {
		const Ref<Image> pDefault = loadRuntimeMaterialCookedImage( 0, channel );
		if ( pDefault.is_null() ) {
			rGenerated.clear();
			return channel != RUNTIME_MATERIAL_CHANNEL_ALBEDO;
		}
		TypedArray<Image> layers;
		layers.resize( physical_layers );
		for ( int physical = 0; physical < physical_layers; ++physical ) {
			const int logical = fully_resident ? physical : 0;
			const Ref<Image> pImage = logical == 0 ? pDefault : loadRuntimeMaterialCookedImage( logical, channel );
			if ( pImage.is_null() ) {
				return false;
			}
			layers[physical] = pImage;
		}
		return rGenerated.create( layers ).is_valid();
	};
	if ( !create_channel_array( replacement_albedo, RUNTIME_MATERIAL_CHANNEL_ALBEDO ) ||
		!create_channel_array( replacement_normal, RUNTIME_MATERIAL_CHANNEL_NORMAL ) ||
		!create_channel_array( replacement_surface, RUNTIME_MATERIAL_CHANNEL_SURFACE ) ||
		!create_channel_array( replacement_displacement, RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT ) ) {
		terrain3dReleaseGeneratedReplacements( replacement_albedo, replacement_normal,
			replacement_surface, replacement_displacement );
		return false;
	}
	m_generatedAlbedoTextures.swap( replacement_albedo );
	m_generatedNormalTextures.swap( replacement_normal );
	m_generatedSurfaceTextures.swap( replacement_surface );
	m_generatedDisplacementTextures.swap( replacement_displacement );
	terrain3dReleaseGeneratedReplacements( replacement_albedo, replacement_normal,
		replacement_surface, replacement_displacement );

	m_nRuntimeMaterialPhysicalLayers = physical_layers;
	for ( int index = 0; index < TERRAIN3D_MAX_TEXTURE_ASSETS; ++index ) {
		m_runtimeMaterialLogicalToPhysical[( size_t ) ( index )] = -1;
		m_runtimeMaterialPhysicalToLogical[( size_t ) ( index )] = -1;
		m_runtimeMaterialLastUseTick[( size_t ) ( index )] = 0u;
		m_runtimeMaterialRetireAfterTick[( size_t ) ( index )] = 0u;
	}
	m_nRuntimeMaterialResidentCount = fully_resident ? m_textureList.size() : 1;
	for ( int physical = 0; physical < m_nRuntimeMaterialResidentCount; ++physical ) {
		m_runtimeMaterialPhysicalToLogical[( size_t ) ( physical )] = physical;
		m_runtimeMaterialLogicalToPhysical[( size_t ) ( physical )] = physical;
		m_runtimeMaterialLastUseTick[( size_t ) ( physical )] = ++m_nRuntimeMaterialTick;
	}
	m_bRuntimeMaterialCacheActive = true;
	return true;
}

bool T3DAssets::publishRuntimeMaterialLayer( const int nLogicalId, const int nPhysicalLayer ) {
	if ( !m_bRuntimeMaterialCacheActive || nLogicalId < 0 || nLogicalId >= m_textureList.size() ||
		nPhysicalLayer <= 0 || nPhysicalLayer >= m_nRuntimeMaterialPhysicalLayers ) {
		return false;
	}
	const Ref<Image> pAlbedo = loadRuntimeMaterialCookedImage( nLogicalId, 0 );
	const Ref<Image> pNormal = loadRuntimeMaterialCookedImage( nLogicalId, 1 );
	const Ref<Image> pSurface = loadRuntimeMaterialCookedImage( nLogicalId, 2 );
	const Ref<Image> pDisplacement = loadRuntimeMaterialCookedImage( nLogicalId, 3 );
	if ( pAlbedo.is_null() || !m_generatedAlbedoTextures.update( pAlbedo, nPhysicalLayer ) ) {
		return false;
	}
	if ( m_generatedNormalTextures.getRid().is_valid() &&
		( pNormal.is_null() || !m_generatedNormalTextures.update( pNormal, nPhysicalLayer ) ) ) {
		return false;
	}
	if ( m_generatedSurfaceTextures.getRid().is_valid() &&
		( pSurface.is_null() || !m_generatedSurfaceTextures.update( pSurface, nPhysicalLayer ) ) ) {
		return false;
	}
	if ( m_generatedDisplacementTextures.getRid().is_valid() &&
		( pDisplacement.is_null() || !m_generatedDisplacementTextures.update( pDisplacement, nPhysicalLayer ) ) ) {
		return false;
	}
	return true;
}

void T3DAssets::releaseRuntimeMaterialSourceTextures( void ) {
	if ( IS_EDITOR ) {
		return;
	}
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		if ( pAsset.is_null() ) {
			continue;
		}
		pAsset->disconnectPackedPbrResource();
		pAsset->m_pAlbedoTexture.unref();
		pAsset->m_pNormalTexture.unref();
		pAsset->m_pSurfaceTexture.unref();
		pAsset->m_pDisplacementTexture.unref();
		pAsset->m_pPbrTexture.unref();
		pAsset->m_pPackedPbrResource.unref();
	}
	m_bRuntimeMaterialSourceTexturesReleased = true;
}

bool T3DAssets::configureRuntimeMaterialCache( void ) {
	if ( !m_pTerrain || m_textureList.is_empty() ) {
		m_bRuntimeMaterialCacheActive = false;
		return false;
	}
	auto refresh_material_bindings = [&]() {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->update( T3DMaterial::TEXTURE_ARRAYS );
		}
	};

	const bool runtime_cache_requested = m_pTerrain->isRuntimeMaterialStreamingEnabled() &&
		!m_pTerrain->isEditorAuthoringMode();
	if ( !runtime_cache_requested ) {
		// Editor Runtime Profile can transition back to Authoring without
		// reinitializing the Terrain3D resource graph. Restore the full authoring
		// arrays from their source textures instead of merely disabling the layer
		// map while leaving the bounded physical cache bound to the material.
		if ( m_pTerrain->isEditorAuthoringMode() ) {
			if ( m_bRuntimeMaterialCacheActive || m_generatedAlbedoTextures.size() != m_textureList.size() ) {
				updateTextureList();
			} else {
				m_bRuntimeMaterialCacheActive = false;
			}
			m_bRuntimeMaterialSourceTexturesReleased = false;
			refresh_material_bindings();
			return false;
		}
		if ( !m_bRuntimeMaterialCacheActive ) {
			return false;
		}
		if ( !m_bRuntimeMaterialCookedValid && !loadRuntimeMaterialCookedIndex() ) {
			LOG( T3DTerrain::WARN,
				"Cannot disable runtime material streaming because the cooked material bundle is unavailable; keeping the bounded cache active." );
			return true;
		}
		if ( !initializeRuntimeMaterialPhysicalArrays( TERRAIN3D_MAX_TEXTURE_ASSETS ) ) {
			LOG( T3DTerrain::WARN,
				"Cannot rebuild full terrain material arrays from the cooked bundle; keeping the bounded runtime material cache active." );
			return true;
		}
		m_bRuntimeMaterialCacheActive = false;
		refresh_material_bindings();
		return false;
	}

	if ( !m_bRuntimeMaterialCookedValid && !loadRuntimeMaterialCookedIndex() ) {
		LOG( T3DTerrain::WARN, "Cooked runtime material cache unavailable or stale; retaining full terrain material arrays." );
		return m_bRuntimeMaterialCacheActive;
	}
	if ( !initializeRuntimeMaterialPhysicalArrays() ) {
		LOG( T3DTerrain::WARN, "Failed to rebuild the bounded runtime material cache; retaining the previous material arrays." );
		return m_bRuntimeMaterialCacheActive;
	}

	// Cold runtime startup and Editor Runtime Profile must expose the same
	// material working set before the first visible frame. Populate the bounded
	// cache from the already-established GPU region priorities in one bootstrap
	// pass; subsequent motion returns to the configured per-frame upload budget.
	if ( m_pTerrain->getData() ) {
		uint8_t priority_ids[TERRAIN3D_MAX_TEXTURE_ASSETS] = {};
		const int priority_count = m_pTerrain->getData()->fillGpuMaterialPriorityIds(
			priority_ids, TERRAIN3D_MAX_TEXTURE_ASSETS );
		const int mapped_capacity = MAX( 1, m_nRuntimeMaterialPhysicalLayers - TERRAIN3D_RUNTIME_MATERIAL_SPARE_LAYERS );
		updateRuntimeMaterialResidencyInternal( priority_ids, priority_count, mapped_capacity );
	}
	if ( m_pTerrain->isRuntimeReleaseSourceMaterialTextures() ) {
		releaseRuntimeMaterialSourceTextures();
	}
	refresh_material_bindings();
	return true;
}

void T3DAssets::updateRuntimeMaterialResidencyInternal(
	const uint8_t *const pPriorityIds, const int nPriorityCount,
	const int nUploadBudget
) {
	m_nRuntimeMaterialUploadsLastTick = 0;
	m_nRuntimeMaterialEvictionsLastTick = 0;
	if ( !m_bRuntimeMaterialCacheActive || !m_pTerrain || !pPriorityIds || nPriorityCount <= 0 ) {
		return;
	}
	++m_nRuntimeMaterialTick;
	for ( int physical = 1; physical < m_nRuntimeMaterialPhysicalLayers; ++physical ) {
		if ( m_runtimeMaterialPhysicalToLogical[( size_t ) ( physical )] == -2 &&
			m_nRuntimeMaterialTick >= m_runtimeMaterialRetireAfterTick[( size_t ) ( physical )] ) {
			m_runtimeMaterialPhysicalToLogical[( size_t ) ( physical )] = -1;
			m_runtimeMaterialRetireAfterTick[( size_t ) ( physical )] = 0u;
		}
	}
	uint32_t desired_mask = 1u;
	for ( int index = 0; index < nPriorityCount; ++index ) {
		const int id = ( int ) ( pPriorityIds[index] );
		if ( id >= 0 && id < m_textureList.size() ) {
			desired_mask |= 1u << ( uint32_t ) ( id );
		}
	}
	const int mapped_capacity = MAX( 1, m_nRuntimeMaterialPhysicalLayers - TERRAIN3D_RUNTIME_MATERIAL_SPARE_LAYERS );
	const int upload_budget = MAX( 1, nUploadBudget );
	bool mapping_changed = false;
	for ( int priority = 0; priority < nPriorityCount && m_nRuntimeMaterialUploadsLastTick < upload_budget; ++priority ) {
		const int logical = ( int ) ( pPriorityIds[priority] );
		if ( logical < 0 || logical >= m_textureList.size() ) {
			continue;
		}
		const int existing = m_runtimeMaterialLogicalToPhysical[( size_t ) ( logical )];
		if ( logical == 0 || ( existing > 0 && existing < m_nRuntimeMaterialPhysicalLayers &&
			m_runtimeMaterialPhysicalToLogical[( size_t ) ( existing )] == logical ) ) {
			m_runtimeMaterialLastUseTick[( size_t ) ( existing )] = m_nRuntimeMaterialTick;
			continue;
		}
		if ( m_nRuntimeMaterialResidentCount >= mapped_capacity ) {
			int victim_physical = -1;
			uint64_t oldest = std::numeric_limits<uint64_t>::max();
			for ( int physical = 1; physical < m_nRuntimeMaterialPhysicalLayers; ++physical ) {
				const int victim_logical = m_runtimeMaterialPhysicalToLogical[( size_t ) ( physical )];
				if ( victim_logical <= 0 || ( desired_mask & ( 1u << ( uint32_t ) ( victim_logical ) ) ) != 0u ) {
					continue;
				}
				if ( m_runtimeMaterialLastUseTick[( size_t ) ( physical )] < oldest ) {
					oldest = m_runtimeMaterialLastUseTick[( size_t ) ( physical )];
					victim_physical = physical;
				}
			}
			if ( victim_physical >= 0 ) {
				const int victim_logical = m_runtimeMaterialPhysicalToLogical[( size_t ) ( victim_physical )];
				m_runtimeMaterialLogicalToPhysical[( size_t ) ( victim_logical )] = -1;
				m_runtimeMaterialPhysicalToLogical[( size_t ) ( victim_physical )] = -2;
				m_runtimeMaterialRetireAfterTick[( size_t ) ( victim_physical )] = m_nRuntimeMaterialTick + 4u;
				--m_nRuntimeMaterialResidentCount;
				++m_nRuntimeMaterialEvictionsLastTick;
				mapping_changed = true;
			} else {
				// All mapped pages are currently desired. Never consume retirement/staging
				// layers as extra logical capacity; the lower-priority material will use
				// physical fallback zero until a mapped slot becomes replaceable.
				continue;
			}
		}
		int free_physical = -1;
		for ( int physical = 1; physical < m_nRuntimeMaterialPhysicalLayers; ++physical ) {
			if ( m_runtimeMaterialPhysicalToLogical[( size_t ) ( physical )] == -1 ) {
				free_physical = physical;
				break;
			}
		}
		if ( free_physical < 0 || !publishRuntimeMaterialLayer( logical, free_physical ) ) {
			continue;
		}
		m_runtimeMaterialPhysicalToLogical[( size_t ) ( free_physical )] = logical;
		m_runtimeMaterialLogicalToPhysical[( size_t ) ( logical )] = free_physical;
		m_runtimeMaterialLastUseTick[( size_t ) ( free_physical )] = m_nRuntimeMaterialTick;
		++m_nRuntimeMaterialResidentCount;
		++m_nRuntimeMaterialUploadsLastTick;
		mapping_changed = true;
	}
	if ( mapping_changed ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateRuntimeMaterialLayerMap();
		}
	}
}

void T3DAssets::updateRuntimeMaterialResidency( const uint8_t *const pPriorityIds, const int nPriorityCount ) {
	const int upload_budget = m_pTerrain ? MAX( 1, m_pTerrain->getRuntimeMaterialUploadBudgetLayers() ) : 1;
	updateRuntimeMaterialResidencyInternal( pPriorityIds, nPriorityCount, upload_budget );
}

PackedInt32Array T3DAssets::getTextureLayerMap( void ) const {
	PackedInt32Array map;
	map.resize( TERRAIN3D_MAX_TEXTURE_ASSETS );
	int32_t *const pWrite = map.ptrw();
	for ( int index = 0; index < TERRAIN3D_MAX_TEXTURE_ASSETS; ++index ) {
		pWrite[index] = m_bRuntimeMaterialCacheActive ?
			m_runtimeMaterialLogicalToPhysical[( size_t ) ( index )] : index;
	}
	return map;
}

PackedColorArray T3DAssets::getRuntimeMaterialAverageAlbedo( void ) const {
	PackedColorArray averages;
	averages.resize( TERRAIN3D_MAX_TEXTURE_ASSETS );
	Color *const pWrite = averages.ptrw();
	for ( int index = 0; index < TERRAIN3D_MAX_TEXTURE_ASSETS; ++index ) {
		pWrite[index] = m_bRuntimeMaterialCookedValid ?
			m_runtimeMaterialCookedLayers[( size_t ) ( index )].averageAlbedo : COLOR_WHITE;
	}
	return averages;
}

Dictionary T3DAssets::getRuntimeMaterialCacheStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled();
	stats["active"] = m_bRuntimeMaterialCacheActive;
	stats["cooked_valid"] = m_bRuntimeMaterialCookedValid;
	stats["logical_materials"] = m_textureList.size();
	stats["physical_layers"] = m_nRuntimeMaterialPhysicalLayers;
	stats["resident"] = m_nRuntimeMaterialResidentCount;
	stats["uploads_last_tick"] = m_nRuntimeMaterialUploadsLastTick;
	stats["evictions_last_tick"] = m_nRuntimeMaterialEvictionsLastTick;
	stats["generated_payload_bytes"] = ( int64_t ) ( getGeneratedTexturePayloadBytes() );
	stats["far_albedo_layers"] = m_generatedFarAlbedoTextures.size();
	stats["far_albedo_max_lod"] = m_generatedFarAlbedoTextures.maxMipmapLevel();
	stats["source_textures_released"] = m_bRuntimeMaterialSourceTexturesReleased;
	uint64_t mapping_hash = 1469598103934665603ull;
	for ( int logical = 0; logical < TERRAIN3D_MAX_TEXTURE_ASSETS; ++logical ) {
		mapping_hash ^= ( uint64_t ) ( ( uint32_t ) ( m_runtimeMaterialLogicalToPhysical[( size_t ) ( logical )] + 2 ) );
		mapping_hash *= 1099511628211ull;
	}
	stats["logical_to_physical_hash"] = String::num_uint64( mapping_hash, 16 );

	int required_count = 0;
	int required_resident = 0;
	int required_pending = 0;
	if ( m_pTerrain && m_pTerrain->getData() && m_bRuntimeMaterialCacheActive ) {
		uint8_t required_ids[TERRAIN3D_MAX_TEXTURE_ASSETS] = {};
		required_count = m_pTerrain->getData()->fillGpuMaterialPriorityIds(
			required_ids, TERRAIN3D_MAX_TEXTURE_ASSETS );
		for ( int index = 0; index < required_count; ++index ) {
			const int logical = ( int ) ( required_ids[index] );
			const int physical = logical >= 0 && logical < TERRAIN3D_MAX_TEXTURE_ASSETS ?
				m_runtimeMaterialLogicalToPhysical[( size_t ) ( logical )] : -1;
			if ( logical == 0 || physical >= 0 ) {
				++required_resident;
			} else {
				++required_pending;
			}
		}
	}
	stats["first_visible_required"] = required_count;
	stats["first_visible_resident"] = required_resident;
	stats["first_visible_pending"] = required_pending;
	stats["first_visible_ready"] = !m_bRuntimeMaterialCacheActive || required_pending == 0;
	return stats;
}

Dictionary T3DAssets::cookRuntimeMaterialCache( void ) {
	Dictionary result;
	result["complete"] = false;
	result["materials"] = m_textureList.size();
	const bool restore_runtime_cache = m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled() &&
		!m_pTerrain->isEditorAuthoringMode();
	const String path = getRuntimeMaterialCachePath();
	if ( path.is_empty() || m_textureList.is_empty() ) {
		result["error"] = "No terrain material set or data directory.";
		return result;
	}
	if ( getGeneratedArraySize() != m_textureList.size() ) {
		updateTextureFiles();
	}
	if ( getGeneratedArraySize() != m_textureList.size() || !m_generatedAlbedoTextures.getRid().is_valid() ) {
		result["error"] = "Full authoring material arrays are unavailable for cooking.";
		return result;
	}
	const String temporary_path = path + String( ".tmp" );
	const String backup_path = path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		result["error"] = "Cannot open cooked material cache for writing.";
		return result;
	}
	const uint64_t signature = computeRuntimeMaterialSourceSignature();
	pFile->store_64( TERRAIN3D_RUNTIME_MATERIAL_MAGIC );
	pFile->store_32( TERRAIN3D_RUNTIME_MATERIAL_VERSION );
	pFile->store_32( ( uint32_t ) ( m_textureList.size() ) );
	pFile->store_32( ( uint32_t ) ( m_texturePackingMode ) );
	pFile->store_32( m_generatedDisplacementTextures.getRid().is_valid() ? 1u : 0u );
	pFile->store_64( signature );
	const uint64_t descriptor_offset = pFile->get_position();
	for ( int logical = 0; logical < TERRAIN3D_MAX_TEXTURE_ASSETS; ++logical ) {
		for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
			pFile->store_64( 0u );
			for ( int field = 0; field < 6; ++field ) {
				pFile->store_32( 0u );
			}
		}
		for ( int component = 0; component < 4; ++component ) {
			pFile->store_float( 1.0f );
		}
	}
	eastl::array<t3druntimeMaterialLayerInfo_t, TERRAIN3D_MAX_TEXTURE_ASSETS> descriptors{};
	const RID channel_rids[RUNTIME_MATERIAL_CHANNEL_COUNT] = {
		m_generatedAlbedoTextures.getRid(), m_generatedNormalTextures.getRid(),
		m_generatedSurfaceTextures.getRid(), m_generatedDisplacementTextures.getRid(), RID()
	};
	bool ok = true;
	for ( int logical = 0; logical < m_textureList.size() && ok; ++logical ) {
		for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
			if ( channel != RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO && !channel_rids[channel].is_valid() ) {
				continue;
			}
			Ref<Image> pImage = RS->texture_2d_layer_get(
				channel == RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO ? channel_rids[RUNTIME_MATERIAL_CHANNEL_ALBEDO] : channel_rids[channel], logical );
			if ( channel == RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO && pImage.is_valid() ) {
				pImage = pImage->duplicate();
				if ( pImage->is_compressed() && pImage->decompress() != OK ) {
					pImage.unref();
				}
				if ( pImage.is_valid() && pImage->has_mipmaps() ) {
					pImage->clear_mipmaps();
				}
				const int far_resolution = m_pTerrain ? m_pTerrain->getFarMaterialResolution() : 128;
				if ( pImage.is_valid() ) {
					pImage->resize( far_resolution, far_resolution, Image::INTERPOLATE_LANCZOS );
				}
				if ( pImage.is_valid() && pImage->generate_mipmaps() != OK ) {
					pImage.unref();
				}
			}
			if ( pImage.is_null() || pImage->is_empty() ) {
				ok = false;
				break;
			}
			if ( channel == RUNTIME_MATERIAL_CHANNEL_ALBEDO ) {
				descriptors[( size_t ) ( logical )].averageAlbedo = terrain3dMaterialAverageAlbedo( pImage );
			}
			const PackedByteArray bytes = pImage->get_data();
			t3druntimeMaterialImageInfo_t &info = descriptors[( size_t ) ( logical )].channels[( size_t ) ( channel )];
			info.nOffset = pFile->get_position();
			info.nSize = ( uint32_t ) ( bytes.size() );
			info.nChecksum = terrain3dMaterialChecksum( bytes.ptr(), ( size_t ) ( bytes.size() ) );
			info.nWidth = pImage->get_width();
			info.nHeight = pImage->get_height();
			info.nFormat = ( int32_t ) ( pImage->get_format() );
			info.bMipmaps = pImage->has_mipmaps() ? 1u : 0u;
			pFile->store_buffer( bytes );
		}
	}
	if ( ok ) {
		pFile->seek( descriptor_offset );
		for ( int logical = 0; logical < TERRAIN3D_MAX_TEXTURE_ASSETS; ++logical ) {
			for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
				const t3druntimeMaterialImageInfo_t &info = descriptors[( size_t ) ( logical )].channels[( size_t ) ( channel )];
				pFile->store_64( info.nOffset );
				pFile->store_32( info.nSize );
				pFile->store_32( info.nChecksum );
				pFile->store_32( ( uint32_t ) ( info.nWidth ) );
				pFile->store_32( ( uint32_t ) ( info.nHeight ) );
				pFile->store_32( ( uint32_t ) ( info.nFormat ) );
				pFile->store_32( info.bMipmaps );
			}
			const Color average = descriptors[( size_t ) ( logical )].averageAlbedo;
			pFile->store_float( average.r );
			pFile->store_float( average.g );
			pFile->store_float( average.b );
			pFile->store_float( average.a );
		}
	}
	const Error write_error = pFile->get_error();
	pFile->close();
	ok = ok && write_error == OK;
	if ( !ok ) {
		DirAccess::remove_absolute( temporary_path );
		result["error"] = "Failed while reading or writing material-array layers.";
		return result;
	}
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	const bool had_previous = FileAccess::file_exists( path );
	if ( had_previous && DirAccess::rename_absolute( path, backup_path ) != OK ) {
		DirAccess::remove_absolute( temporary_path );
		result["error"] = "Cannot rotate previous cooked material cache.";
		return result;
	}
	if ( DirAccess::rename_absolute( temporary_path, path ) != OK ) {
		if ( had_previous ) {
			DirAccess::rename_absolute( backup_path, path );
		}
		result["error"] = "Cannot publish cooked material cache.";
		return result;
	}
	const auto previous_layers = m_runtimeMaterialCookedLayers;
	const String previous_cache_path = m_runtimeMaterialCachePath;
	const uint64_t previous_signature = m_nRuntimeMaterialCookedSignature;
	const bool previous_cooked_valid = m_bRuntimeMaterialCookedValid;
	bool published_valid = loadRuntimeMaterialCookedIndex();
	if ( published_valid ) {
		for ( int logical = 0; logical < m_textureList.size() && published_valid; ++logical ) {
			for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
				const t3druntimeMaterialImageInfo_t &info =
					m_runtimeMaterialCookedLayers[( size_t ) ( logical )].channels[( size_t ) ( channel )];
				if ( info.nSize > 0u && loadRuntimeMaterialCookedImage( logical, channel ).is_null() ) {
					published_valid = false;
					break;
				}
			}
		}
	}
	if ( !published_valid ) {
		DirAccess::remove_absolute( path );
		if ( had_previous ) {
			DirAccess::rename_absolute( backup_path, path );
		}
		m_runtimeMaterialCookedLayers = previous_layers;
		m_runtimeMaterialCachePath = previous_cache_path;
		m_nRuntimeMaterialCookedSignature = previous_signature;
		m_bRuntimeMaterialCookedValid = previous_cooked_valid;
		result["error"] = "Published runtime material cache failed reload validation.";
		return result;
	}
	// Cooking a bounded runtime cache temporarily expands the generated arrays to
	// logical authoring size. Restore the requested runtime configuration before
	// reporting success so Terrain3D never receives an identity layer map paired
	// with a runtime-streaming policy.
	if ( restore_runtime_cache && !configureRuntimeMaterialCache() ) {
		DirAccess::remove_absolute( path );
		if ( had_previous ) {
			DirAccess::rename_absolute( backup_path, path );
		}
		m_runtimeMaterialCookedLayers = previous_layers;
		m_runtimeMaterialCachePath = previous_cache_path;
		m_nRuntimeMaterialCookedSignature = previous_signature;
		m_bRuntimeMaterialCookedValid = previous_cooked_valid;
		result["error"] = "Published runtime material cache could not be activated safely.";
		return result;
	}
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( IS_EDITOR && path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( path );
		}
	}
	result["path"] = path;
	result["signature"] = ( int64_t ) ( signature );
	result["complete"] = true;
	return result;
}

void T3DAssets::connectAssetSignals( const t3dassetType_t type, const Ref<T3DAssetResource> &rAsset ) {
	if ( rAsset.is_null() ) {
		return;
	}
	const Callable id_callback = callable_mp( this, &T3DAssets::swapIds );
	if ( !rAsset->is_connected( "id_changed", id_callback ) ) {
		rAsset->connect( "id_changed", id_callback );
	}
	if ( type == TYPE_TEXTURE ) {
		Ref<T3DTextureAsset> pTexture = rAsset;
		if ( pTexture.is_null() ) {
			return;
		}
		const Callable file_callback = callable_mp( this, &T3DAssets::updateTextureFiles );
		const Callable setting_callback = callable_mp( this, &T3DAssets::updateTextureSettings );
		const Callable preview_callback = callable_mp( this, &T3DAssets::updateTexturePreview );
		if ( !pTexture->is_connected( "file_changed", file_callback ) ) {
			pTexture->connect( "file_changed", file_callback );
		}
		if ( !pTexture->is_connected( "setting_changed", setting_callback ) ) {
			pTexture->connect( "setting_changed", setting_callback );
		}
		if ( !pTexture->is_connected( "preview_changed", preview_callback ) ) {
			pTexture->connect( "preview_changed", preview_callback );
		}
	} else if ( type == TYPE_MESH ) {
		Ref<T3DMeshAsset> pMesh = rAsset;
		if ( pMesh.is_null() ) {
			return;
		}
		const Callable settings_callback = callable_mp( this, &T3DAssets::updateMesh );
		const Callable preview_callback = callable_mp( this, &T3DAssets::updateMeshPreview );
		if ( !pMesh->is_connected( "instancer_setting_changed", settings_callback ) ) {
			pMesh->connect( "instancer_setting_changed", settings_callback );
		}
		if ( !pMesh->is_connected( "preview_changed", preview_callback ) ) {
			pMesh->connect( "preview_changed", preview_callback );
		}
	}
}

void T3DAssets::disconnectAssetSignals( const t3dassetType_t type, const Ref<T3DAssetResource> &rAsset ) {
	if ( rAsset.is_null() ) {
		return;
	}
	const Callable id_callback = callable_mp( this, &T3DAssets::swapIds );
	if ( rAsset->is_connected( "id_changed", id_callback ) ) {
		rAsset->disconnect( "id_changed", id_callback );
	}
	if ( type == TYPE_TEXTURE ) {
		Ref<T3DTextureAsset> pTexture = rAsset;
		if ( pTexture.is_null() ) {
			return;
		}
		const Callable file_callback = callable_mp( this, &T3DAssets::updateTextureFiles );
		const Callable setting_callback = callable_mp( this, &T3DAssets::updateTextureSettings );
		const Callable preview_callback = callable_mp( this, &T3DAssets::updateTexturePreview );
		if ( pTexture->is_connected( "file_changed", file_callback ) ) {
			pTexture->disconnect( "file_changed", file_callback );
		}
		if ( pTexture->is_connected( "setting_changed", setting_callback ) ) {
			pTexture->disconnect( "setting_changed", setting_callback );
		}
		if ( pTexture->is_connected( "preview_changed", preview_callback ) ) {
			pTexture->disconnect( "preview_changed", preview_callback );
		}
	} else if ( type == TYPE_MESH ) {
		Ref<T3DMeshAsset> pMesh = rAsset;
		if ( pMesh.is_null() ) {
			return;
		}
		const Callable settings_callback = callable_mp( this, &T3DAssets::updateMesh );
		const Callable preview_callback = callable_mp( this, &T3DAssets::updateMeshPreview );
		if ( pMesh->is_connected( "instancer_setting_changed", settings_callback ) ) {
			pMesh->disconnect( "instancer_setting_changed", settings_callback );
		}
		if ( pMesh->is_connected( "preview_changed", preview_callback ) ) {
			pMesh->disconnect( "preview_changed", preview_callback );
		}
	}
}

Ref<T3DAssetResource> T3DAssets::makePlaceholderAsset( const t3dassetType_t type, const int nId ) const {
	Ref<T3DAssetResource> pAsset;
	if ( type == TYPE_TEXTURE ) {
		Ref<T3DTextureAsset> pTexture;
		pTexture.instantiate();
		pAsset = pTexture;
	} else if ( type == TYPE_MESH ) {
		Ref<T3DMeshAsset> pMesh;
		pMesh.instantiate();
		pAsset = pMesh;
	}
	if ( pAsset.is_valid() ) {
		pAsset->m_nId = nId;
		pAsset->m_bPlaceholder = true;
	}
	return pAsset;
}

void T3DAssets::swapIds( const t3dassetType_t type, const int nSrcId, const int nDstId ) {
	m_bPersistenceDirty = true;
	LOG( T3DTerrain::INFO, "Swapping asset ID: ", nSrcId, " and ID: ", nDstId );
	Array list;
	switch ( type ) {
		case TYPE_TEXTURE:
			list = m_textureList;
			break;
		case TYPE_MESH:
			list = m_meshList;
			break;
		default:
			return;
	}
	if ( nSrcId < 0 || nSrcId >= list.size() ) {
		LOG( T3DTerrain::ERROR, "Source ID out of range: ", nSrcId );
		return;
	}
	Ref<T3DAssetResource> pResSrc = list[nSrcId];
	if ( pResSrc.is_null() ) {
		LOG( T3DTerrain::ERROR, "Source Asset is null at ID: ", nSrcId );
		return;
	}
	// User can type in any value, -1 means swap with first, >size means swap with last
	int dst_id = CLAMP( nDstId, 0, list.size() - 1 );
	if ( dst_id == nSrcId ) {
		// They're likely the same due to the clamp, when new ID is <0 or >= array_size)
		pResSrc->m_nId = nSrcId;
		return;
	}
	Ref<T3DAssetResource> pResDst = list[dst_id];
	if ( pResDst.is_valid() ) {
		pResDst->m_nId = nSrcId;
	}
	pResSrc->m_nId = dst_id;
	list[dst_id] = pResSrc;
	list[nSrcId] = pResDst;

	switch ( type ) {
		case TYPE_TEXTURE:
			updateTextureList();
			break;
		case TYPE_MESH:
			if ( m_pTerrain ) {
				m_pTerrain->getInstancer()->swapIds( nSrcId, dst_id );
			} else {
				LOG( T3DTerrain::ERROR, "Changing IDs before the terrain is initialized. Meshes on the ground may be wrong." );
			}
			updateMeshList();
			break;
		default:
			return;
	}
}

/**
 * _set_asset_list attempts to keep the asset ID as saved in the resource file.
 * But if an ID is invalid or already taken, the new ID is changed to the next available one
 */
void T3DAssets::setAssetList( const t3dassetType_t type, const TypedArray<T3DAssetResource> &rList ) {
	m_bPersistenceDirty = true;
	const int max_size = type == TYPE_TEXTURE ? TERRAIN3D_MAX_TEXTURE_ASSETS : type == TYPE_MESH ? TERRAIN3D_MAX_MESH_ASSETS : 0;
	if ( max_size == 0 ) {
		return;
	}

	// Build a fresh fixed-slot registry. Reusing the old Array makes replacement
	// depend on its previous occupants and can silently mix old and new assets.
	const int array_size = CLAMP( rList.size(), 0, max_size );
	Array replacement;
	replacement.resize( array_size );
	eastl::bitset<TERRAIN3D_MAX_MESH_ASSETS> occupied;
	int next_free = 0;
	for ( int i = 0; i < array_size; ++i ) {
		Ref<T3DAssetResource> pAsset = rList[i];
		if ( pAsset.is_null() ) {
			continue;
		}
		int target_id = pAsset->getId();
		if ( target_id < 0 || target_id >= array_size || occupied.test( ( size_t ) ( target_id ) ) ) {
			while ( next_free < array_size && occupied.test( ( size_t ) ( next_free ) ) ) {
				++next_free;
			}
			if ( next_free >= array_size ) {
				LOG( T3DTerrain::ERROR, "No free slot for asset: ", pAsset->get_name() );
				continue;
			}
			LOG( T3DTerrain::WARN, pAsset->get_class(), " ID ", target_id, " is invalid or duplicated. Assigning slot ", next_free );
			target_id = next_free;
		}
		pAsset->m_nId = target_id;
		pAsset->m_bPlaceholder = false;
		replacement[target_id] = pAsset;
		occupied.set( ( size_t ) ( target_id ) );
	}

	// Holes are explicit placeholder resources. Texture/control IDs and mesh IDs
	// are stable slot identifiers and therefore cannot be compacted.
	for ( int slot = 0; slot < array_size; ++slot ) {
		if ( !occupied.test( ( size_t ) ( slot ) ) ) {
			replacement[slot] = makePlaceholderAsset( type, slot );
		}
	}

	if ( type == TYPE_TEXTURE ) {
		for ( const Ref<T3DTextureAsset> pOldAsset : m_textureList ) {
			disconnectAssetSignals( TYPE_TEXTURE, pOldAsset );
		}
		m_textureList.clear();
		m_textureList.resize( array_size );
		for ( int i = 0; i < array_size; ++i ) {
			Ref<T3DTextureAsset> pAsset = replacement[i];
			m_textureList[i] = pAsset;
			connectAssetSignals( TYPE_TEXTURE, pAsset );
			if ( occupied.test( ( size_t ) ( i ) ) ) {
				pAsset->initialize();
			}
		}
	} else {
		for ( const Ref<T3DMeshAsset> pOldAsset : m_meshList ) {
			disconnectAssetSignals( TYPE_MESH, pOldAsset );
		}
		m_meshList.clear();
		m_meshList.resize( array_size );
		for ( int i = 0; i < array_size; ++i ) {
			Ref<T3DMeshAsset> pAsset = replacement[i];
			m_meshList[i] = pAsset;
			connectAssetSignals( TYPE_MESH, pAsset );
			if ( occupied.test( ( size_t ) ( i ) ) ) {
				pAsset->initialize();
			}
		}
	}
}

void T3DAssets::setAsset( const t3dassetType_t type, const int nId, const Ref<T3DAssetResource> &rAsset ) {
	m_bPersistenceDirty = true;
	LOG( T3DTerrain::INFO, "t3dsetting_t asset type: ", type, ", ID: ", nId, ", asset: ", rAsset );
	const int max_size = type == TYPE_TEXTURE ? TERRAIN3D_MAX_TEXTURE_ASSETS : type == TYPE_MESH ? TERRAIN3D_MAX_MESH_ASSETS : 0;
	if ( max_size == 0 || nId < 0 || nId >= max_size ) {
		LOG( T3DTerrain::ERROR, "Invalid asset id: ", nId, " range is 0-", max_size - 1 );
		return;
	}
	Array list = type == TYPE_TEXTURE ? Array( m_textureList ) : Array( m_meshList );
	if ( rAsset.is_null() && nId >= list.size() ) {
		return;
	}
	if ( nId >= list.size() ) {
		const int old_size = list.size();
		list.resize( nId + 1 );
		for ( int slot = old_size; slot < nId; ++slot ) {
			const Ref<T3DAssetResource> placeholder = makePlaceholderAsset( type, slot );
			list[slot] = placeholder;
			connectAssetSignals( type, placeholder );
		}
	}

	const Ref<T3DAssetResource> pOldAsset = list[nId];
	if ( rAsset.is_null() ) {
		// Texture IDs are persisted in control-map pixels. Removing the final
		// array entry cannot compact the registry without remapping every painted
		// pixel, so texture deletion always leaves a placeholder layer. Mesh IDs
		// may trim the tail only after set_mesh_asset() has cleared all instances.
		const bool bPreserveStableSlot = type == TYPE_TEXTURE || nId < list.size() - 1;
		disconnectAssetSignals( type, pOldAsset );
		if ( bPreserveStableSlot ) {
			const Ref<T3DAssetResource> placeholder = makePlaceholderAsset( type, nId );
			list[nId] = placeholder;
			connectAssetSignals( type, placeholder );
		} else {
			list.pop_back();
		}
	} else {
		if ( pOldAsset != rAsset ) {
			disconnectAssetSignals( type, pOldAsset );
		}
		rAsset->m_nId = nId;
		rAsset->m_bPlaceholder = false;
		list[nId] = rAsset;
		connectAssetSignals( type, rAsset );
		rAsset->initialize();
	}

	if ( type == TYPE_TEXTURE ) {
		m_textureList = list;
	} else {
		m_meshList = list;
	}
}

void T3DAssets::validateArrayMember(
	const Ref<Texture2D> &rTexture,
	const Ref<T3DTextureAsset> &rAsset, const String &rLabel, t3dtextureArrayLayout_t &rLayout,
	const Image::Format eRequiredFormat,
	const bool bAllowCompressedSurface
) {
	if ( rTexture.is_null() ) {
		return;
	}
	const Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
		LOG( T3DTerrain::ERROR, "Texture ID ", rAsset->getId(), " ", rLabel, " image is invalid or empty." );
		return;
	}
	const Vector2i vTextureSize = pImage->get_size();
	const Image::Format eFormat = pImage->get_format();
	const bool bMipmaps = pImage->has_mipmaps();
	const bool bAcceptedSurfaceCompression = bAllowCompressedSurface &&
		eRequiredFormat == Image::FORMAT_RGBA8 && terrain3dIsNativePackedSurfaceFormat( eFormat );
	if ( eRequiredFormat != Image::FORMAT_MAX && eFormat != eRequiredFormat && !bAcceptedSurfaceCompression ) {
		m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
		LOG( T3DTerrain::ERROR, "Texture ID ", rAsset->getId(), " ", rLabel, " must use format ", ( int ) ( eRequiredFormat ),
			" or a supported native compressed equivalent, received ", ( int ) ( eFormat ), "." );
		return;
	}
	if ( rLayout.eFormat == Image::FORMAT_MAX ) {
		rLayout.vSize = vTextureSize;
		rLayout.eFormat = eFormat;
		rLayout.bMipmaps = bMipmaps;
		return;
	}
	if ( vTextureSize != rLayout.vSize ) {
		rLayout.bNeedsNormalization = true;
		rLayout.vSize.x = MAX( rLayout.vSize.x, vTextureSize.x );
		rLayout.vSize.y = MAX( rLayout.vSize.y, vTextureSize.y );
		LOG( T3DTerrain::WARN, "Texture ID ", rAsset->getId(), " ", rLabel, " size ", vTextureSize,
			" differs from another layer in the same array and will be normalized to ", rLayout.vSize, "." );
	}
	if ( eFormat != rLayout.eFormat ) {
		rLayout.bNeedsNormalization = true;
		LOG( T3DTerrain::WARN, "Texture ID ", rAsset->getId(), " ", rLabel, " format ", ( int ) ( eFormat ),
			" differs from another layer in the same array; the array will be normalized instead of rejected." );
	}
	if ( bMipmaps != rLayout.bMipmaps ) {
		rLayout.bNeedsNormalization = true;
		rLayout.bMipmaps = rLayout.bMipmaps || bMipmaps;
		LOG( T3DTerrain::WARN, "Texture ID ", rAsset->getId(), " ", rLabel,
			" mipmap layout differs from another layer in the same array and will be normalized." );
	}
}

void T3DAssets::updateTextureFiles( void ) {
	m_bPersistenceDirty = true;
	IS_INIT( VOID );
	LOG( T3DTerrain::DEBUG, "Received texture_changed signal" );
	if ( m_textureList.is_empty() ) {
		resetRuntimeMaterialCacheState( false );
		m_generatedAlbedoTextures.clear();
		m_generatedNormalTextures.clear();
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
		if ( !m_bSuppressTextureChanged ) {
			emit_signal( "textures_changed" );
		}
		return;
	}

	const bool bPackedPbr = usesPackedPbr();
	const bool bUseDisplacement = bPackedPbr && m_pTerrain->getTessellationLevel() > 0;
	t3dtextureArrayLayout_t albedoLayout;
	t3dtextureArrayLayout_t secondaryLayout;
	t3dtextureArrayLayout_t displacementLayout;
	m_pTerrain->setWarning( WARN_ALL, false );

	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		if ( pAsset.is_null() ) {
			continue;
		}
		validateArrayMember( pAsset->m_pAlbedoTexture, pAsset, "albedo", albedoLayout );
		if ( bPackedPbr ) {
			const Ref<Texture2D> pSurface = pAsset->getEffectiveSurfaceTexture();
			if ( pAsset->m_pPackedPbrResource.is_valid() && pSurface.is_null() ) {
				m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
				LOG( T3DTerrain::ERROR, "Texture ID ", pAsset->getId(),
					" packed-PBR/displacement compatibility resource failed to build: ", pAsset->m_pPackedPbrResource->getLastError() );
			}
			validateArrayMember( pSurface, pAsset, "surface", secondaryLayout, Image::FORMAT_RGBA8, true );
			if ( bUseDisplacement ) {
				const Ref<Texture2D> pDisplacement = pAsset->getEffectiveDisplacementTexture();
				validateArrayMember( pDisplacement, pAsset, "R8 displacement", displacementLayout, Image::FORMAT_R8 );
			}
		} else {
			validateArrayMember( pAsset->m_pNormalTexture, pAsset, "normal", secondaryLayout );
		}
	}
	if ( m_pTerrain->getWarnings() ) {
		return;
	}

	if ( bPackedPbr ) {
		if ( albedoLayout.vSize == V2I_ZERO && secondaryLayout.vSize == V2I_ZERO ) {
			albedoLayout.vSize = V2I( 1024 );
			secondaryLayout.vSize = V2I( 512 );
		} else if ( albedoLayout.vSize == V2I_ZERO ) {
			albedoLayout.vSize = secondaryLayout.vSize * 2;
		} else if ( secondaryLayout.vSize == V2I_ZERO ) {
			secondaryLayout.vSize = Vector2i( MAX( albedoLayout.vSize.x / 2, 1 ), MAX( albedoLayout.vSize.y / 2, 1 ) );
		}
		if ( albedoLayout.eFormat == Image::FORMAT_MAX ) {
			albedoLayout.eFormat = Image::FORMAT_RGB8;
		}
		if ( secondaryLayout.eFormat == Image::FORMAT_MAX ) {
			secondaryLayout.eFormat = Image::FORMAT_RGBA8;
		}
		if ( displacementLayout.vSize == V2I_ZERO ) {
			displacementLayout.vSize = secondaryLayout.vSize;
		}
		displacementLayout.eFormat = Image::FORMAT_R8;
	} else {
		if ( secondaryLayout.vSize == V2I_ZERO ) {
			secondaryLayout.vSize = albedoLayout.vSize;
		} else if ( albedoLayout.vSize == V2I_ZERO ) {
			albedoLayout.vSize = secondaryLayout.vSize;
		}
		if ( albedoLayout.vSize == V2I_ZERO ) {
			albedoLayout.vSize = V2I( 1024 );
			secondaryLayout.vSize = albedoLayout.vSize;
		}
	}

	// GPU texture arrays require a uniform layout only among their own layers.
	// Albedo, packed surface/PBR, normal, and displacement are separate arrays and
	// deliberately keep independent dimensions and formats. Preserve identical
	// native compressed layers byte-for-byte; only fall back to a canonical
	// uncompressed representation when layers *within the same array* disagree.
	if ( albedoLayout.bNeedsNormalization ) {
		albedoLayout.eFormat = Image::FORMAT_RGBA8;
	}
	if ( secondaryLayout.bNeedsNormalization ) {
		secondaryLayout.eFormat = Image::FORMAT_RGBA8;
	}
	if ( displacementLayout.bNeedsNormalization ) {
		displacementLayout.eFormat = Image::FORMAT_R8;
	}

	T3DGeneratedTexture replacement_albedo;
	T3DGeneratedTexture replacement_normal;
	T3DGeneratedTexture replacement_surface;
	T3DGeneratedTexture replacement_displacement;
	bool bReplacementOk = true;

	if ( albedoLayout.vSize != V2I_ZERO ) {
		TypedArray<Image> layers;
		for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->m_pAlbedoTexture : Ref<Texture2D>();
			const Ref<Image> pImage = terrain3dPrepareArrayImage( pTexture, albedoLayout.vSize, albedoLayout.eFormat,
				albedoLayout.bMipmaps, COLOR_CHECKED );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( T3DTerrain::ERROR, "Failed to normalize albedo texture layer ", layers.size(), "." );
				break;
			}
			layers.push_back( pImage );
		}
		if ( bReplacementOk && !layers.is_empty() ) {
			bReplacementOk = replacement_albedo.create( layers ).is_valid();
		}
	}

	if ( bReplacementOk && bPackedPbr && secondaryLayout.vSize != V2I_ZERO ) {
		TypedArray<Image> surface_layers;
		for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getEffectiveSurfaceTexture() : Ref<Texture2D>();
			const Ref<Image> pImage = terrain3dPrepareArrayImage( pTexture, secondaryLayout.vSize, secondaryLayout.eFormat,
				secondaryLayout.bMipmaps, Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( T3DTerrain::ERROR, "Failed to normalize packed surface texture layer ", surface_layers.size(), "." );
				break;
			}
			surface_layers.push_back( pImage );
		}
		if ( bReplacementOk && !surface_layers.is_empty() ) {
			bReplacementOk = replacement_surface.create( surface_layers ).is_valid();
		}

		if ( bReplacementOk && bUseDisplacement ) {
			TypedArray<Image> displacement_layers;
			for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
				const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getEffectiveDisplacementTexture() : Ref<Texture2D>();
				const Ref<Image> pImage = terrain3dPrepareArrayImage( pTexture, displacementLayout.vSize,
					displacementLayout.eFormat, displacementLayout.bMipmaps, Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
				if ( pImage.is_null() ) {
					bReplacementOk = false;
					LOG( T3DTerrain::ERROR, "Failed to normalize displacement texture layer ", displacement_layers.size(), "." );
					break;
				}
				displacement_layers.push_back( pImage );
			}
			if ( bReplacementOk && !displacement_layers.is_empty() ) {
				bReplacementOk = replacement_displacement.create( displacement_layers ).is_valid();
			}
		}
	} else if ( bReplacementOk && !bPackedPbr && secondaryLayout.vSize != V2I_ZERO ) {
		TypedArray<Image> layers;
		for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->m_pNormalTexture : Ref<Texture2D>();
			const Ref<Image> pImage = terrain3dPrepareArrayImage( pTexture, secondaryLayout.vSize, secondaryLayout.eFormat,
				secondaryLayout.bMipmaps, COLOR_NORMAL );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( T3DTerrain::ERROR, "Failed to normalize normal texture layer ", layers.size(), "." );
				break;
			}
			layers.push_back( pImage );
		}
		if ( bReplacementOk && !layers.is_empty() ) {
			bReplacementOk = replacement_normal.create( layers ).is_valid();
		}
	}

	if ( !bReplacementOk ) {
		terrain3dReleaseGeneratedReplacements( replacement_albedo, replacement_normal,
			replacement_surface, replacement_displacement );
		LOG( T3DTerrain::ERROR, "Terrain texture-array rebuild failed. Keeping the last complete generated texture set." );
		return;
	}

	m_generatedAlbedoTextures.swap( replacement_albedo );
	if ( bPackedPbr ) {
		m_generatedSurfaceTextures.swap( replacement_surface );
		m_generatedNormalTextures.clear();
		if ( bUseDisplacement ) {
			m_generatedDisplacementTextures.swap( replacement_displacement );
		} else {
			m_generatedDisplacementTextures.clear();
		}
	} else {
		m_generatedNormalTextures.swap( replacement_normal );
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
	}

	// Preserve the asset-level placeholder behavior of the original pipeline, but only
	// after a complete replacement set has been committed. This keeps failed rebuilds
	// transactional while editor previews and serialized asset state still see defaults.
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		if ( pAsset.is_null() ) {
			continue;
		}
		if ( pAsset->m_pAlbedoTexture.is_null() ) {
			const Ref<Image> pImage = terrain3dMakeArrayDefault( albedoLayout.vSize, albedoLayout.eFormat,
				albedoLayout.bMipmaps, COLOR_CHECKED );
			if ( pImage.is_valid() ) {
				pAsset->m_pAlbedoTexture = ImageTexture::create_from_image( pImage );
			}
		}
		if ( bPackedPbr ) {
			if ( pAsset->m_pPackedPbrResource.is_null() && pAsset->m_pSurfaceTexture.is_null() ) {
				const Ref<Image> pImage = terrain3dMakeArrayDefault( secondaryLayout.vSize, secondaryLayout.eFormat,
					secondaryLayout.bMipmaps, Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
				if ( pImage.is_valid() ) {
					pAsset->m_pSurfaceTexture = ImageTexture::create_from_image( pImage );
				}
			}
			if ( bUseDisplacement && pAsset->m_pPackedPbrResource.is_null() && pAsset->m_pDisplacementTexture.is_null() ) {
				const Ref<Image> pImage = terrain3dMakeArrayDefault( displacementLayout.vSize, displacementLayout.eFormat,
					displacementLayout.bMipmaps, Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
				if ( pImage.is_valid() ) {
					pAsset->m_pDisplacementTexture = ImageTexture::create_from_image( pImage );
				}
			}
		} else if ( pAsset->m_pNormalTexture.is_null() ) {
			const Ref<Image> pImage = terrain3dMakeArrayDefault( secondaryLayout.vSize, secondaryLayout.eFormat,
				secondaryLayout.bMipmaps, COLOR_NORMAL );
			if ( pImage.is_valid() ) {
				pAsset->m_pNormalTexture = ImageTexture::create_from_image( pImage );
			}
		}
	}

	terrain3dReleaseGeneratedReplacements( replacement_albedo, replacement_normal,
		replacement_surface, replacement_displacement );
	resetRuntimeMaterialCacheState( false );

	if ( !m_bSuppressTextureChanged ) {
		emit_signal( "textures_changed" );
	}
}

void T3DAssets::updateTextureSettingsInternal( const bool bPersistentChange ) {
	if ( bPersistentChange ) {
		m_bPersistenceDirty = true;
	}
	LOG( T3DTerrain::DEBUG, "Received setting_changed signal" );
	m_textureSettings.nCount = MIN( m_textureList.size(), TERRAIN3D_MAX_TEXTURE_ASSETS );
	for ( int index = 0; index < m_textureSettings.nCount; ++index ) {
		const Ref<T3DTextureAsset> pTa = m_textureList[index];
		if ( pTa.is_null() ) {
			m_textureSettings.colors[( size_t ) ( index )] = COLOR_WHITE;
			m_textureSettings.normal_depths[( size_t ) ( index )] = 1.0f;
			m_textureSettings.ao_strengths[( size_t ) ( index )] = 0.5f;
			m_textureSettings.ao_light_affects[( size_t ) ( index )] = 0.0f;
			m_textureSettings.roughness_mods[( size_t ) ( index )] = 0.0f;
			m_textureSettings.metallics[( size_t ) ( index )] = 0.0f;
			m_textureSettings.uv_scales[( size_t ) ( index )] = 0.1f;
			m_textureSettings.detiles[( size_t ) ( index )] = Vector2();
			m_textureSettings.displacements[( size_t ) ( index )] = Vector2();
			continue;
		}

		m_textureSettings.colors[( size_t ) ( index )] =
			pTa->isHighlighted() ? pTa->getHighlightColor() : pTa->getAlbedoColor();
		m_textureSettings.normal_depths[( size_t ) ( index )] = pTa->getNormalDepth();
		m_textureSettings.ao_strengths[( size_t ) ( index )] = pTa->getAoStrength();
		m_textureSettings.ao_light_affects[( size_t ) ( index )] = pTa->getAoLightAffect();
		m_textureSettings.roughness_mods[( size_t ) ( index )] = pTa->getRoughness();
		m_textureSettings.metallics[( size_t ) ( index )] = pTa->getMetallic();
		m_textureSettings.uv_scales[( size_t ) ( index )] = pTa->getUvScale();
		m_textureSettings.detiles[( size_t ) ( index )] =
			Vector2( pTa->getDetilingRotation(), pTa->getDetilingShift() );
		m_textureSettings.displacements[( size_t ) ( index )] =
			Vector2( pTa->getDisplacementOffset(), pTa->getDisplacementScale() );
	}

	const int count = m_textureSettings.nCount;
	m_textureColors.resize( count );
	m_textureNormalDepths.resize( count );
	m_textureAoStrengths.resize( count );
	m_textureAoLightAffects.resize( count );
	m_textureRoughnessMods.resize( count );
	m_textureMetallics.resize( count );
	m_textureUvScales.resize( count );
	m_textureDetiles.resize( count );
	m_textureDisplacements.resize( count );
	if ( count > 0 ) {
		std::copy_n( m_textureSettings.colors.data(), count, m_textureColors.ptrw() );
		std::copy_n( m_textureSettings.normal_depths.data(), count, m_textureNormalDepths.ptrw() );
		std::copy_n( m_textureSettings.ao_strengths.data(), count, m_textureAoStrengths.ptrw() );
		std::copy_n( m_textureSettings.ao_light_affects.data(), count, m_textureAoLightAffects.ptrw() );
		std::copy_n( m_textureSettings.roughness_mods.data(), count, m_textureRoughnessMods.ptrw() );
		std::copy_n( m_textureSettings.metallics.data(), count, m_textureMetallics.ptrw() );
		std::copy_n( m_textureSettings.uv_scales.data(), count, m_textureUvScales.ptrw() );
		std::copy_n( m_textureSettings.detiles.data(), count, m_textureDetiles.ptrw() );
		std::copy_n( m_textureSettings.displacements.data(), count, m_textureDisplacements.ptrw() );
	}

	LOG( T3DTerrain::DEBUG, "Emitting textures_changed" );
	if ( !m_bSuppressTextureChanged ) {
		emit_signal( "textures_changed" );
	}
}

void T3DAssets::updateTextureSettings( void ) {
	updateTextureSettingsInternal( true );
}

void T3DAssets::updateTexturePreview( void ) {
	updateTextureSettingsInternal( false );
}

void T3DAssets::updateMeshInternal( const int nId, const bool bPersistentChange ) {
	if ( bPersistentChange ) {
		m_bPersistenceDirty = true;
	}
	if ( nId < 0 || nId >= m_meshList.size() ) {
		LOG( T3DTerrain::ERROR, "Invalid mesh ID: ", nId );
		return;
	}
	IS_INSTANCER_INIT( VOID );
	T3DInstancer *const pInstancer = m_pTerrain->getInstancer();
	pInstancer->updateMmis( nId, V2I_MAX, false );
}

void T3DAssets::updateMesh( const int nId ) {
	updateMeshInternal( nId, true );
}

void T3DAssets::updateMeshPreview( const int nId ) {
	updateMeshInternal( nId, false );
}

void T3DAssets::setupThumbnailCreation( void ) {
	IS_INIT( VOID );
	if ( m_scenario.is_valid() ) {
		return;
	}
	LOG( T3DTerrain::INFO, "t3dsetting_t up mesh thumbnail creation viewports" );
	// Setup Mesh preview environment
	m_scenario = RS->scenario_create();

	m_viewport = RS->viewport_create();
	RS->viewport_set_update_mode( m_viewport, RenderingServer::VIEWPORT_UPDATE_DISABLED );
	RS->viewport_set_scenario( m_viewport, m_scenario );
	RS->viewport_set_size( m_viewport, 128, 128 );
	RS->viewport_set_transparent_background( m_viewport, true );
	RS->viewport_set_active( m_viewport, true );
	m_viewportTexture = RS->viewport_get_texture( m_viewport );

	m_camera = RS->camera_create();
	RS->viewport_attach_camera( m_viewport, m_camera );
	RS->camera_set_transform( m_camera, Transform3D( Basis(), Vector3( 0, 0, 3 ) ) );
	RS->camera_set_orthogonal( m_camera, 1.0, 0.01, 1000.0 );

	m_keyLight = RS->directional_light_create();
	m_keyLightInstance = RS->instance_create2( m_keyLight, m_scenario );
	RS->instance_set_transform( m_keyLightInstance, Transform3D().looking_at( V3( -1 ), V3_UP ) );

	m_fillLight = RS->directional_light_create();
	RS->light_set_color( m_fillLight, Color( 0.3, 0.3, 0.3 ) );
	m_fillLightInstance = RS->instance_create2( m_fillLight, m_scenario );
	RS->instance_set_transform( m_fillLightInstance, Transform3D().looking_at( V3_UP, Vector3( 0, 0, 1 ) ) );

	m_meshInstance = RS->instance_create();
	RS->instance_set_scenario( m_meshInstance, m_scenario );
}

///////////////////////////
// Public Functions
///////////////////////////

void T3DAssets::destroyThumbnailCreation( void ) {
	if ( !m_scenario.is_valid() ) {
		return;
	}
	RS->free_rid( m_meshInstance );
	RS->free_rid( m_fillLightInstance );
	RS->free_rid( m_fillLight );
	RS->free_rid( m_keyLightInstance );
	RS->free_rid( m_keyLight );
	RS->free_rid( m_camera );
	RS->free_rid( m_viewport );
	RS->free_rid( m_scenario );
	m_meshInstance = RID();
	m_fillLightInstance = RID();
	m_fillLight = RID();
	m_keyLightInstance = RID();
	m_keyLight = RID();
	m_camera = RID();
	m_viewportTexture = RID();
	m_viewport = RID();
	m_scenario = RID();
}

void T3DAssets::setTexturePackingMode( const t3dtexturePackingMode_t eMode ) {
	const t3dtexturePackingMode_t mode =
		( t3dtexturePackingMode_t ) ( CLAMP( ( int ) ( eMode ), ( int ) ( TEXTURE_PACKING_LEGACY ), ( int ) ( TEXTURE_PACKING_PACKED_PBR_R8 ) ) );
	if ( m_texturePackingMode == mode ) {
		return;
	}
	m_texturePackingMode = mode;
	m_bPersistenceDirty = true;
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		if ( pAsset.is_valid() ) {
			pAsset->setPackedPbrMode( usesPackedPbr() );
		}
	}
	m_generatedAlbedoTextures.clear();
	m_generatedNormalTextures.clear();
	m_generatedSurfaceTextures.clear();
	m_generatedDisplacementTextures.clear();
	LOG( T3DTerrain::INFO, "t3dsetting_t texture packing mode: ", ( int ) ( m_texturePackingMode ) );
	if ( m_pTerrain ) {
		updateTextureList();
		Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitialized() ) {
			pMaterial->update( T3DMaterial::FULL_REBUILD );
		}
	}
	emit_changed();
}

void T3DAssets::initialize( T3DTerrain *pTerrain ) {
	if ( pTerrain ) {
		T3DTerrain *previous_owner = Terrain3DOwnership::transfer( m_pTerrain, pTerrain );
		if ( previous_owner && previous_owner != pTerrain ) {
			LOG( T3DTerrain::INFO, "Transferring asset runtime ownership from ", previous_owner, " to ", pTerrain );
		}
	} else {
		LOG( T3DTerrain::ERROR, "Initialization failed, p_terrain is null" );
		return;
	}
	LOG( T3DTerrain::INFO, "Initializing assets" );

	// Runtime/Profile mode can load the cooked physical material working set
	// directly. Building the full logical arrays first would create a large
	// transient duplicate and defeat the purpose of runtime material streaming.
	bool runtime_materials_ready = false;
	if ( m_pTerrain->isRuntimeMaterialStreamingEnabled() && !m_pTerrain->isEditorAuthoringMode() ) {
		for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
			if ( pAsset.is_valid() ) {
				pAsset->setPackedPbrMode( usesPackedPbr() );
				connectAssetSignals( TYPE_TEXTURE, pAsset );
			}
		}
		updateTextureSettings();
		runtime_materials_ready = configureRuntimeMaterialCache();
	}
	if ( !runtime_materials_ready ) {
		updateTextureList();
	}
	updateMeshList();
	m_bPersistenceDirty = false;
}

void T3DAssets::uninitialize( T3DTerrain *pTerrain ) {
	if ( !Terrain3DOwnership::release( m_pTerrain, pTerrain ) ) {
		LOG( T3DTerrain::DEBUG, "Ignoring stale asset uninitialize request from ", pTerrain, "; current owner: ", m_pTerrain );
		return;
	}
	LOG( T3DTerrain::INFO, "Uninitializing assets" );
}

void T3DAssets::destroy( void ) {
	LOG( T3DTerrain::INFO, "Destroying assets" );
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		disconnectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	for ( const Ref<T3DMeshAsset> pAsset : m_meshList ) {
		disconnectAssetSignals( TYPE_MESH, pAsset );
	}
	m_pTerrain = nullptr;
	m_generatedAlbedoTextures.clear();
	m_generatedNormalTextures.clear();
	m_generatedSurfaceTextures.clear();
	m_generatedDisplacementTextures.clear();
	m_textureList.clear();
	m_meshList.clear();
	m_textureColors.clear();
	m_textureNormalDepths.clear();
	m_textureAoStrengths.clear();
	m_textureAoLightAffects.clear();
	m_textureRoughnessMods.clear();
	m_textureMetallics.clear();
	m_textureUvScales.clear();
	m_textureDetiles.clear();
	m_textureDisplacements.clear();
	resetRuntimeMaterialCacheState( false );

	destroyThumbnailCreation();
}

// Called when creating a new asset
void T3DAssets::setTextureAsset( const int nId, const Ref<T3DTextureAsset> &rTexture ) {
	if ( nId < 0 || nId >= TERRAIN3D_MAX_TEXTURE_ASSETS ) {
		LOG( T3DTerrain::ERROR, "Invalid texture id: ", nId, " range is 0-", TERRAIN3D_MAX_TEXTURE_ASSETS - 1 );
		return;
	}
	if ( nId < m_textureList.size() && m_textureList[nId] == rTexture ) {
		return;
	}
	LOG( T3DTerrain::INFO, "t3dsetting_t texture id: ", nId );
	setAsset( TYPE_TEXTURE, nId, rTexture );
	updateTextureList();
}

// Called when loading a list of assets from an assets resource file on disk
TypedArray<T3DTextureAsset> T3DAssets::getTextureList( void ) const {
	TypedArray<T3DTextureAsset> result;
	result.resize( m_textureList.size() );
	for ( int i = 0; i < m_textureList.size(); ++i ) {
		const Ref<T3DTextureAsset> pAsset = m_textureList[i];
		if ( pAsset.is_valid() && !pAsset->m_bPlaceholder ) {
			result[i] = pAsset;
		}
	}
	return result;
}

void T3DAssets::setTextureList( const TypedArray<T3DTextureAsset> &rTextureList ) {
	LOG( T3DTerrain::INFO, "t3dsetting_t texture list with ", rTextureList.size(), " entries" );
	if ( !differs( getTextureList(), rTextureList ) ) {
		return;
	}
	setAssetList( TYPE_TEXTURE, rTextureList );
	updateTextureList();
}

void T3DAssets::clearTextures( const bool bUpdate ) {
	m_bPersistenceDirty = true;
	LOG( T3DTerrain::INFO, "Clearing texture list" );
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		disconnectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	m_textureList.clear();
	if ( bUpdate ) {
		updateTextureList();
	}
}

void T3DAssets::updateTextureList( void ) {
	LOG( T3DTerrain::INFO, "Refreshing texture registry" );
	for ( const Ref<T3DTextureAsset> pAsset : m_textureList ) {
		if ( pAsset.is_valid() ) {
			pAsset->setPackedPbrMode( usesPackedPbr() );
		}
		connectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	m_bSuppressTextureChanged = true;
	updateTextureFiles();
	updateTextureSettings();
	m_bSuppressTextureChanged = false;
	emit_signal( "textures_changed" );
}

// Called when creating a new asset
void T3DAssets::setMeshAsset( const int nId, const Ref<T3DMeshAsset> &rMeshAsset ) {
	if ( nId < 0 || nId >= TERRAIN3D_MAX_MESH_ASSETS ) {
		LOG( T3DTerrain::ERROR, "Invalid mesh id: ", nId, " range is 0-", TERRAIN3D_MAX_MESH_ASSETS - 1 );
		return;
	}
	if ( nId < m_meshList.size() && m_meshList[nId] == rMeshAsset ) {
		return;
	}
	LOG( T3DTerrain::INFO, "t3dsetting_t mesh id: ", nId, ", ", rMeshAsset );
	if ( rMeshAsset.is_null() ) {
		IS_INSTANCER_INIT( VOID );
		m_pTerrain->getInstancer()->clearByMesh( nId );
	}
	setAsset( TYPE_MESH, nId, rMeshAsset );
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( nId, V2I_MAX, false );
	}
}

// Called when loading a list of assets from an assets resource file on disk
TypedArray<T3DMeshAsset> T3DAssets::getMeshList( void ) const {
	TypedArray<T3DMeshAsset> result;
	result.resize( m_meshList.size() );
	for ( int i = 0; i < m_meshList.size(); ++i ) {
		const Ref<T3DMeshAsset> pAsset = m_meshList[i];
		if ( pAsset.is_valid() && !pAsset->m_bPlaceholder ) {
			result[i] = pAsset;
		}
	}
	return result;
}

void T3DAssets::setMeshList( const TypedArray<T3DMeshAsset> &rMeshList ) {
	LOG( T3DTerrain::INFO, "t3dsetting_t mesh list with ", rMeshList.size(), " entries" );
	if ( !differs( getMeshList(), rMeshList ) ) {
		return;
	}
	setAssetList( TYPE_MESH, rMeshList );
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, true );
	}
}

// p_id = -1 for all meshes
// Adapted from godot\editor\plugins\editor_preview_plugins.cpp:EditorMeshPreviewPlugin
void T3DAssets::createMeshThumbnails( const int nId, const Vector2i &rSize, const bool bForce ) {
	if ( !IS_EDITOR ) {
		return;
	}
	if ( m_pTerrain && !m_pTerrain->isAuthoringResidencyReadbackSafe() ) {
		LOG( T3DTerrain::DEBUG, "Deferring mesh thumbnail GPU readback until Authoring residency is idle" );
		return;
	}
	LOG( T3DTerrain::INFO, "Creating mesh thumbnails for ID: ", nId, ", size: ", rSize );
	const int max = getMeshCount();
	if ( nId < -1 || nId >= max ) {
		return;
	}
	int start, end;
	if ( nId < 0 ) {
		start = 0;
		end = max;
	} else {
		start = CLAMP( nId, 0, max - 1 );
		end = CLAMP( nId + 1, 0, max );
	}
	const Vector2i size = CLAMP( rSize, V2I( 2 ), V2I( 4096 ) );

	LOG( T3DTerrain::DEBUG, "Creating thumbnails for ids: ", start, " through ", end - 1 );
	for ( int i = start; i < end; i++ ) {
		Ref<T3DMeshAsset> pMa = getMeshAsset( i );
		LOG( T3DTerrain::EXTREME, i, ": Getting Terrain3DMeshAsset: ", ptrToStr( *pMa ) );
		if ( pMa.is_null() ) {
			LOG( T3DTerrain::ERROR, i, ": Terrain3DMeshAsset is null, skipping" );
			continue;
		}
		if ( !bForce && pMa->getThumbnail().is_valid() ) {
			LOG( T3DTerrain::EXTREME, "Thumbnail already generated, skipping" );
			continue;
		}
		// Setup mesh
		if ( !m_meshInstance.is_valid() ) {
			setupThumbnailCreation();
			if ( !m_meshInstance.is_valid() ) {
				LOG( T3DTerrain::ERROR, "Mesh thumbnail preview resources could not be created" );
				break;
			}
		}
		const Ref<Mesh> pMesh = pMa->getMesh( 0 );
		LOG( T3DTerrain::EXTREME, i, ": Getting Mesh 0: ", pMesh );
		if ( pMesh.is_null() ) {
			LOG( T3DTerrain::ERROR, i, ": Mesh is null, skipping" );
			continue;
		}
		RS->instance_set_base( m_meshInstance, pMesh->get_rid() );

		// Setup material
		Ref<Material> pMat = pMa->getMaterialOverride();
		RID rid = pMat.is_valid() ? pMat->get_rid() : RID();
		RS->instance_geometry_set_material_override( m_meshInstance, rid );
		pMat = pMa->getMaterialOverlay();
		rid = pMat.is_valid() ? pMat->get_rid() : RID();
		RS->instance_geometry_set_material_overlay( m_meshInstance, rid );

		// Setup scene
		AABB aabb = pMesh->get_aabb();
		const Vector3 ofs = aabb.get_center();
		aabb.position -= ofs;
		Transform3D xform;
		xform.basis = Basis().rotated( V3_UP, -Math_PI * 0.125f );
		xform.basis = Basis().rotated( Vector3( 1.f, 0.f, 0.f ), Math_PI * 0.125f ) * xform.basis;
		const AABB rot_aabb = xform.xform( aabb );
		real_t fM = MAX( rot_aabb.size.x, rot_aabb.size.y ) * 0.5f;
		if ( fM == 0.f ) {
			fM = 1.f;
		}
		fM = .5f / fM;
		xform.basis.scale( V3( fM ) );
		xform.origin = -xform.basis.xform( ofs );
		xform.origin.z -= rot_aabb.size.z * 2.f;
		RS->instance_set_transform( m_meshInstance, xform );

		// Capture image
		RS->viewport_set_size( m_viewport, size.x, size.y );
		RS->viewport_set_update_mode( m_viewport, RenderingServer::VIEWPORT_UPDATE_ONCE );
		RS->force_draw();

		const Ref<Image> pImg = RS->texture_2d_get( m_viewportTexture );
		RS->instance_set_base( m_meshInstance, RID() ); // Clear mesh
		if ( pImg.is_valid() ) {
			LOG( T3DTerrain::EXTREME, i, ": Retrieving image: ", pImg, " size: ", pImg->get_size(), " format: ", pImg->get_format() );
			pMa->setThumbnail( ImageTexture::create_from_image( pImg ) );
		} else {
			LOG( T3DTerrain::ERROR, "_viewport_texture is null. Couldn't create thumbnail picture" );
		}
	}
	destroyThumbnailCreation();
}

void T3DAssets::updateMeshList( void ) {
	m_bPersistenceDirty = true;
	IS_INSTANCER_INIT( VOID );
	LOG( T3DTerrain::INFO, "Updating mesh registry" );
	if ( m_meshList.is_empty() ) {
		LOG( T3DTerrain::DEBUG, "Mesh list empty, clearing instancer and adding a default mesh" );
		m_pTerrain->getInstancer()->destroy();
		Ref<T3DMeshAsset> pNewAsset;
		pNewAsset.instantiate();
		setMeshAsset( 0, pNewAsset );
		return;
	}
	for ( const Ref<T3DMeshAsset> pAsset : m_meshList ) {
		connectAssetSignals( TYPE_MESH, pAsset );
	}
	emit_signal( "meshes_changed" );
}

void T3DAssets::loadPendingMeshes( void ) {
	// Reload meshes for all mesh assets that have changed. Used by the instancer after clearing
	for ( const Ref<T3DMeshAsset> pMa : m_meshList ) {
		if ( pMa.is_valid() && pMa->isSceneFilePending() ) {
			pMa->commitMeshes();
		}
	}
}

Error T3DAssets::save( const String &rPath ) {
	if ( rPath.is_empty() && get_path().is_empty() ) {
		return ERR_FILE_NOT_FOUND;
	}
	if ( !rPath.is_empty() ) {
		LOG( T3DTerrain::DEBUG, "t3dsetting_t file path to ", rPath );
		take_over_path( rPath );
	}
	// Avoid rewriting clean asset resources during scene pre-save. The nested
	// asset signal callbacks above mark this resource dirty when authored content
	// actually changes.
	Error err = OK;
	String path = get_path();
	const bool explicit_path = !rPath.is_empty();
	if ( path.get_extension() == "tres" || path.get_extension() == "res" ) {
		if ( !explicit_path && !m_bPersistenceDirty && FileAccess::file_exists( path ) ) {
			return OK;
		}
		LOG( T3DTerrain::DEBUG, "Attempting to save external file: " + path );
		err = ResourceSaver::get_singleton()->save( this, path, ResourceSaver::FLAG_COMPRESS );
		if ( err == OK ) {
			m_bPersistenceDirty = false;
			LOG( T3DTerrain::INFO, "File saved successfully: ", path );
			if ( IS_EDITOR && path.begins_with( "res://" ) ) {
				EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
				if ( pFilesystem && !pFilesystem->is_scanning() ) {
					pFilesystem->update_file( path );
				}
			}
		} else {
			LOG( T3DTerrain::ERROR, "Cannot save file: ", path, ". Error code: ", T3DTerrain::ERROR,
				". Look up @GlobalScope Error enum in the Godot docs" );
		}
	}
	return err;
}

///////////////////////////
// Protected Functions
///////////////////////////

void T3DAssets::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( TYPE_TEXTURE );
	BIND_ENUM_CONSTANT( TYPE_MESH );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_LEGACY );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_PACKED_PBR_R8 );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_SURFACE_R16 );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_PBR_RGBA16UI );
	BIND_CONSTANT( TERRAIN3D_MAX_TEXTURE_ASSETS );
	BIND_CONSTANT( TERRAIN3D_MAX_MESH_ASSETS );

	ClassDB::bind_method( D_METHOD( "set_texture_asset", "id", "texture" ), &T3DAssets::setTextureAsset );
	ClassDB::bind_method( D_METHOD( "get_texture_asset", "id" ), &T3DAssets::getTextureAsset );
	ClassDB::bind_method( D_METHOD( "set_texture_list", "texture_list" ), &T3DAssets::setTextureList );
	ClassDB::bind_method( D_METHOD( "get_texture_list" ), &T3DAssets::getTextureList );
	ClassDB::bind_method( D_METHOD( "get_texture_count" ), &T3DAssets::getTextureCount );
	ClassDB::bind_method( D_METHOD( "get_albedo_array_rid" ), &T3DAssets::getAlbedoArrayRid );
	ClassDB::bind_method( D_METHOD( "get_normal_array_rid" ), &T3DAssets::getNormalArrayRid );
	ClassDB::bind_method( D_METHOD( "get_surface_array_rid" ), &T3DAssets::getSurfaceArrayRid );
	ClassDB::bind_method( D_METHOD( "get_displacement_array_rid" ), &T3DAssets::getDisplacementArrayRid );
	ClassDB::bind_method( D_METHOD( "get_pbr_array_rid" ), &T3DAssets::getPbrArrayRid );
	ClassDB::bind_method( D_METHOD( "set_texture_packing_mode", "mode" ), &T3DAssets::setTexturePackingMode );
	ClassDB::bind_method( D_METHOD( "get_texture_packing_mode" ), &T3DAssets::getTexturePackingMode );
	ClassDB::bind_method( D_METHOD( "get_packed_pbr_mipmap_count" ), &T3DAssets::getPackedPbrMipmapCount );
	ClassDB::bind_method( D_METHOD( "get_texture_colors" ), &T3DAssets::getTextureColors );
	ClassDB::bind_method( D_METHOD( "get_texture_normal_depths" ), &T3DAssets::getTextureNormalDepths );
	ClassDB::bind_method( D_METHOD( "get_texture_ao_strengths" ), &T3DAssets::getTextureAoStrengths );
	ClassDB::bind_method( D_METHOD( "get_texture_ao_light_affects" ), &T3DAssets::getTextureAoLightAffects );
	ClassDB::bind_method( D_METHOD( "get_texture_roughness_mods" ), &T3DAssets::getTextureRoughnessMods );
	ClassDB::bind_method( D_METHOD( "get_texture_metallics" ), &T3DAssets::getTextureMetallics );
	ClassDB::bind_method( D_METHOD( "get_texture_uv_scales" ), &T3DAssets::getTextureUvScales );
	ClassDB::bind_method( D_METHOD( "get_texture_detiles" ), &T3DAssets::getTextureDetiles );
	ClassDB::bind_method( D_METHOD( "get_texture_displacements" ), &T3DAssets::getTextureDisplacements );
	ClassDB::bind_method( D_METHOD( "clear_textures", "update" ), &T3DAssets::clearTextures, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "update_texture_list" ), &T3DAssets::updateTextureList );

	ClassDB::bind_method( D_METHOD( "set_mesh_asset", "id", "mesh" ), &T3DAssets::setMeshAsset );
	ClassDB::bind_method( D_METHOD( "get_mesh_asset", "id" ), &T3DAssets::getMeshAsset );
	ClassDB::bind_method( D_METHOD( "set_mesh_list", "mesh_list" ), &T3DAssets::setMeshList );
	ClassDB::bind_method( D_METHOD( "get_mesh_list" ), &T3DAssets::getMeshList );
	ClassDB::bind_method( D_METHOD( "get_mesh_count" ), &T3DAssets::getMeshCount );
	ClassDB::bind_method( D_METHOD( "create_mesh_thumbnails", "id", "size", "force" ),
						 &T3DAssets::createMeshThumbnails, DEFVAL( -1 ), DEFVAL( V2I( 256 ) ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "update_mesh_list" ), &T3DAssets::updateMeshList );

	ClassDB::bind_method( D_METHOD( "save", "path" ), &T3DAssets::save, DEFVAL( "" ) );

	const int ro_flags = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Packing / Runtime Format", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "texture_packing_mode", PROPERTY_HINT_ENUM,
							  "Legacy AlbedoHeight + NormalRoughness,Packed PBR RGBA8 + Displacement R8" ),
				 "set_texture_packing_mode", "get_texture_packing_mode" );

	ADD_GROUP( "Texture Asset Registry", "" );
	ADD_PROPERTY(
		PropertyInfo( Variant::ARRAY, "texture_list", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DTextureAsset", ro_flags ),
		"set_texture_list", "get_texture_list" );

	ADD_GROUP( "Mesh Asset Registry", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "mesh_list", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DMeshAsset", ro_flags ),
				 "set_mesh_list", "get_mesh_list" );

	ADD_SIGNAL( MethodInfo( "meshes_changed" ) );
	ADD_SIGNAL( MethodInfo( "textures_changed" ) );
}
