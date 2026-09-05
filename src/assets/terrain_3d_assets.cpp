// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_assets.h"

#include "core/logger.h"
#include "core/terrain_3d_owner.h"
#include "core/terrain_3d_util.h"
#include "eastl_config.h"

#include <EASTL/numeric_limits.h>
#include <EASTL/bitset.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

// -----------------------------------------------------------------------------
// Private functions


static uint32_t t3dAssetsMaterialChecksum( const uint8_t *const pData, const size_t nSize ) {
	uint32_t hash = 2166136261u;
	for ( size_t index = 0; index < nSize; ++index ) {
		hash ^= ( uint32_t )( pData[index] );
		hash *= 16777619u;
	}
	return hash;
}

static void t3dAssetsMaterialHashU64( uint64_t &rHash, const uint64_t nValue ) {
	for ( int shift = 0; shift < 64; shift += 8 ) {
		rHash ^= ( nValue >> shift ) & 0xffu;
		rHash *= 1099511628211ull;
	}
}

void Terrain3DAssets::advanceTexturePublicationGeneration( const bool bMappingChanged ) {
	m_nTexturePublicationGeneration = m_nTexturePublicationGeneration == UINT64_MAX ? 1u : m_nTexturePublicationGeneration + 1u;
	if ( bMappingChanged ) {
		m_nTextureLayerMappingGeneration = m_nTextureLayerMappingGeneration == UINT64_MAX ? 1u : m_nTextureLayerMappingGeneration + 1u;
	}
}

void Terrain3DAssets::advanceMeshSourceGeneration( void ) {
	m_nMeshSourceGeneration = m_nMeshSourceGeneration == UINT64_MAX ? 1u : m_nMeshSourceGeneration + 1u;
}

template <typename T>
static void t3dAssetsAppendResourceSignature( uint64_t &rHash, const Ref<T> &rResource ) {
	if ( rResource.is_null() ) {
		t3dAssetsMaterialHashU64( rHash, 0u );
		return;
	}
	const String path = rResource->get_path();
	t3dAssetsMaterialHashU64( rHash, ( uint64_t )path.hash() );
	t3dAssetsMaterialHashU64( rHash, !path.is_empty() && FileAccess::file_exists( path ) ? FileAccess::get_modified_time( path ) : 0u );
}

static Color t3dAssetsMaterialAverageAlbedo( const Ref<Image> &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		return s_colorWhite;
	}
	Ref<Image> pImage = rImage->duplicate();
	if ( pImage->is_compressed() && pImage->decompress() != OK ) {
		return s_colorWhite;
	}
	if ( pImage->has_mipmaps() ) {
		pImage->clear_mipmaps();
	}
	pImage->resize( 1, 1, Image::INTERPOLATE_LANCZOS );
	return pImage->get_pixel( 0, 0 );
}

static bool t3dAssetsIsNativePackedSurfaceFormat( const Image::Format format ) {
	// Terrain3D's optimized surface uses all four channels (normal XY, roughness, AO).
	// BC3 and BC7 preserve those channels and are directly usable by the sampler.
	return format == Image::FORMAT_RGBA8 || format == Image::FORMAT_DXT5 || format == Image::FORMAT_BPTC_RGBA;
}

static Image::Format t3dAssetsSelectPackedSurfaceArrayFormat( const Image::Format current, const Image::Format incoming ) {
	if ( current == incoming || incoming == Image::FORMAT_MAX ) {
		return current;
	}
	if ( current == Image::FORMAT_MAX ) {
		return incoming;
	}
	// Keep the array GPU-compressed when compatible packed-surface sources mix
	// RGBA8 with BC3/BC7. Prefer BC7 when any layer already supplies it, then
	// BC3, and use RGBA8 only when no compressed source exists.
	if ( current == Image::FORMAT_BPTC_RGBA || incoming == Image::FORMAT_BPTC_RGBA ) {
		return Image::FORMAT_BPTC_RGBA;
	}
	if ( current == Image::FORMAT_DXT5 || incoming == Image::FORMAT_DXT5 ) {
		return Image::FORMAT_DXT5;
	}
	return Image::FORMAT_RGBA8;
}

EA_FORCE_INLINE static bool t3dAssetsIsSupportedNativeArrayFormat( const Image::Format format ) {
	return format == Image::FORMAT_DXT1 || format == Image::FORMAT_DXT5 || format == Image::FORMAT_BPTC_RGBA
	        || format == Image::FORMAT_RGTC_R || format == Image::FORMAT_RGTC_RG;
}

static Error t3dAssetsCompressArrayImageToFormat( const Ref<Image> &rImage, const Image::Format format ) {
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

static Ref<Image> t3dAssetsMakeArrayDefault(
        const Vector2i &rSize, const Image::Format format, const bool bMipmaps, const Color &rDefaultColor ) {
	if ( !t3dAssetsIsSupportedNativeArrayFormat( format ) ) {
		return Terrain3DUtil::getFilledImage( rSize, rDefaultColor, bMipmaps, format );
	}
	Image::Format working_format = Image::FORMAT_RGBA8;
	if ( format == Image::FORMAT_DXT1 ) {
		working_format = Image::FORMAT_RGB8;
	} else if ( format == Image::FORMAT_RGTC_R ) {
		working_format = Image::FORMAT_R8;
	} else if ( format == Image::FORMAT_RGTC_RG ) {
		working_format = Image::FORMAT_RG8;
	}
	Ref<Image> pImage = Terrain3DUtil::getFilledImage( rSize, rDefaultColor, bMipmaps, working_format );
	if ( pImage.is_null() || t3dAssetsCompressArrayImageToFormat( pImage, format ) != OK || pImage->get_format() != format ) {
		return Ref<Image>();
	}
	return pImage;
}

static Ref<Image> t3dAssetsPrepareArrayImage( const Ref<Texture2D> &rTexture,
        const Vector2i &rSize,
        const Image::Format format,
        const bool bMipmaps,
        const Color &rDefaultColor ) {
	if ( rTexture.is_null() ) {
		return t3dAssetsMakeArrayDefault( rSize, format, bMipmaps, rDefaultColor );
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
	if ( t3dAssetsIsSupportedNativeArrayFormat( format ) ) {
		if ( bMipmaps && pImage->generate_mipmaps() != OK ) {
			return Ref<Image>();
		}
		if ( t3dAssetsCompressArrayImageToFormat( pImage, format ) != OK ) {
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
	return pImage->get_size() == rSize && pImage->get_format() == format && pImage->has_mipmaps() == bMipmaps ? pImage : Ref<Image>();
}

EA_FORCE_INLINE static void t3dAssetsReleaseGeneratedReplacements( Terrain3DGeneratedTexture &rAlbedo,
        Terrain3DGeneratedTexture &rNormal,
        Terrain3DGeneratedTexture &rSurface,
        Terrain3DGeneratedTexture &rDisplacement ) {
	rAlbedo.clear();
	rNormal.clear();
	rSurface.clear();
	rDisplacement.clear();
}

String Terrain3DAssets::getRuntimeMaterialCachePath( void ) const {
	if ( !m_pTerrain || m_pTerrain->getDataDirectory().is_empty() ) {
		return String();
	}
	return m_pTerrain->getDataDirectory().path_join( "terrain3d_materials.t3dmat" );
}

uint64_t Terrain3DAssets::computeRuntimeMaterialSourceSignature( void ) const {
	uint64_t hash = 1469598103934665603ull;
	t3dAssetsMaterialHashU64( hash, ( uint64_t )( m_texturePackingMode ) );
	t3dAssetsMaterialHashU64( hash, ( uint64_t )( m_textureList.size() ) );
	t3dAssetsMaterialHashU64( hash, m_pTerrain ? ( uint64_t )( m_pTerrain->getFarMaterialResolution() ) : 128u );
	for ( int index = 0; index < m_textureList.size(); ++index ) {
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[index];
		t3dAssetsMaterialHashU64( hash, ( uint64_t )( index ) );
		if ( pAsset.is_null() ) {
			continue;
		}
		t3dAssetsAppendResourceSignature( hash, pAsset->m_albedoTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_normalTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_surfaceTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_displacementTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_packedPbrResource );
	}
	return hash;
}

void Terrain3DAssets::resetRuntimeMaterialCacheState( const bool bClearGeneratedArrays ) {
	stopRuntimeMaterialWorker();
	m_bRuntimeMaterialCacheActive = false;
	m_bRuntimeMaterialCookedValid = false;
	m_runtimeMaterialCachePath = String();
	m_nRuntimeMaterialCookedSignature = 0u;
	m_nRuntimeMaterialTick = 0u;
	m_nRuntimeMaterialPhysicalLayers = 0;
	m_nRuntimeMaterialResidentCount = 0;
	m_nRuntimeMaterialUploadsLastTick = 0;
	m_nRuntimeMaterialEvictionsLastTick = 0;
	m_nRuntimeMaterialReadBytesLastTick = 0u;
	m_nRuntimeMaterialUploadBytesLastTick = 0u;
	m_nRuntimeMaterialBudgetRejects = 0u;
	m_nRuntimeMaterialChecksumCacheHits = 0u;
	m_nRuntimeMaterialWorkerFileOpens = 0u;
	m_nRuntimeMaterialCompletionsLastTick = 0;
	m_bRuntimeMaterialSourceTexturesReleased = false;
	for ( int index = 0; index < s_nMaxTextureAssets; ++index ) {
		memset( m_runtimeMaterialChecksumVerified[index], 0, sizeof( m_runtimeMaterialChecksumVerified[index] ) );
		m_runtimeMaterialLogicalToPhysical[( size_t )( index )] = -1;
		m_runtimeMaterialPhysicalToLogical[( size_t )( index )] = -1;
		m_runtimeMaterialLastUseTick[( size_t )( index )] = 0u;
		m_runtimeMaterialRetireAfterTick[( size_t )( index )] = 0u;
		m_runtimeMaterialCookedLayers[( size_t )( index )] = t3dRuntimeMaterialLayerInfo_t{};
	}
	if ( bClearGeneratedArrays ) {
		m_generatedAlbedoTextures.clear();
		m_generatedNormalTextures.clear();
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
	}
	m_generatedFarAlbedoTextures.clear();
	advanceTexturePublicationGeneration( true );
}

bool Terrain3DAssets::initializeFarMaterialArray( void ) {
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
	Terrain3DGeneratedTexture replacement;
	if ( !replacement.create( layers, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid() ) {
		return false;
	}
	m_generatedFarAlbedoTextures.swap( replacement );
	replacement.clear();
	return true;
}

bool Terrain3DAssets::loadRuntimeMaterialCookedIndex( void ) {
	const String path = getRuntimeMaterialCachePath();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nRuntimeMaterialDataOffset ) {
		return false;
	}
	const uint64_t magic = pFile->get_64();
	const uint32_t version = pFile->get_32();
	const uint32_t texture_count = pFile->get_32();
	const uint32_t packing_mode = pFile->get_32();
	const uint32_t flags = pFile->get_32();
	const uint64_t signature = pFile->get_64();
	( void )( flags );
	bool source_resources_available = false;
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_valid()
		        && ( pAsset->m_albedoTexture.is_valid() || pAsset->m_normalTexture.is_valid() || pAsset->m_surfaceTexture.is_valid()
		                || pAsset->m_displacementTexture.is_valid() || pAsset->m_packedPbrResource.is_valid() ) ) {
			source_resources_available = true;
			break;
		}
	}
	uint64_t expected_world_material_signature = 0u;
	if ( m_pTerrain && m_pTerrain->getData() ) {
		expected_world_material_signature = m_pTerrain->getData()->getCookedWorldMaterialSignatureInternal();
	}
	const bool signature_valid = expected_world_material_signature != 0u
	        ? signature == expected_world_material_signature
	        : ( !source_resources_available && !IS_EDITOR ? true : signature == computeRuntimeMaterialSourceSignature() );
	if ( magic != s_nRuntimeMaterialMagic || version != s_nRuntimeMaterialVersion || texture_count != ( uint32_t )( m_textureList.size() )
	        || texture_count == 0u || texture_count > s_nMaxTextureAssets || packing_mode != ( uint32_t )( m_texturePackingMode )
	        || signature == 0u || !signature_valid ) {
		return false;
	}
	const uint64_t file_length = pFile->get_length();
	t3dRuntimeMaterialLayerInfo_t loaded_layers[s_nMaxTextureAssets]{};
	for ( int logical = 0; logical < s_nMaxTextureAssets; ++logical ) {
		for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
			t3dRuntimeMaterialImageInfo_t &info = loaded_layers[( size_t )( logical )].m_szChannels[( size_t )( channel )];
			info.m_iOffset = pFile->get_64();
			info.m_nSize = pFile->get_32();
			info.m_nChecksum = pFile->get_32();
			info.m_nWidth = ( int32_t )( pFile->get_32() );
			info.m_nHeight = ( int32_t )( pFile->get_32() );
			info.m_nFormat = ( int32_t )( pFile->get_32() );
			info.m_nMipmaps = pFile->get_32();
			if ( info.m_nSize > 0u
			        && ( logical >= ( int )( texture_count ) || info.m_iOffset < s_nRuntimeMaterialDataOffset
			                || info.m_iOffset > file_length || ( uint64_t )( info.m_nSize ) > file_length - info.m_iOffset
			                || info.m_nWidth <= 0 || info.m_nHeight <= 0 || info.m_nFormat < 0
			                || info.m_nFormat >= ( int32_t )( Image::FORMAT_MAX ) || info.m_nMipmaps > 1u ) ) {
				return false;
			}
		}
		t3dRuntimeMaterialLayerInfo_t &layer = loaded_layers[( size_t )( logical )];
		layer.m_averageAlbedo = Color( pFile->get_float(), pFile->get_float(), pFile->get_float(), pFile->get_float() );
		layer.m_albedoColor = Color( pFile->get_float(), pFile->get_float(), pFile->get_float(), pFile->get_float() );
		layer.m_fNormalDepth = pFile->get_float();
		layer.m_fAoStrength = pFile->get_float();
		layer.m_fAoLightAffect = pFile->get_float();
		layer.m_fRoughness = pFile->get_float();
		layer.m_fMetallic = pFile->get_float();
		layer.m_fUvScale = pFile->get_float();
		layer.m_detile = Vector2( pFile->get_float(), pFile->get_float() );
		layer.m_displacement = Vector2( pFile->get_float(), pFile->get_float() );
		layer.m_iBundleIndex = pFile->get_32();
		if ( logical < ( int )texture_count && layer.m_iBundleIndex != ( uint32_t )logical ) {
			return false;
		}
	}
	for ( uint32_t logical = 0u; logical < texture_count; ++logical ) {
		const t3dRuntimeMaterialLayerInfo_t &layer = loaded_layers[( size_t )( logical )];
		if ( layer.m_szChannels[RUNTIME_MATERIAL_CHANNEL_ALBEDO].m_nSize == 0u
		        || layer.m_szChannels[RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO].m_nSize == 0u
		        || ( m_texturePackingMode == TEXTURE_PACKING_LEGACY && layer.m_szChannels[RUNTIME_MATERIAL_CHANNEL_NORMAL].m_nSize == 0u )
		        || ( m_texturePackingMode != TEXTURE_PACKING_LEGACY
		                && layer.m_szChannels[RUNTIME_MATERIAL_CHANNEL_SURFACE].m_nSize == 0u ) ) {
			return false;
		}
	}
	// Publish the index only after the entire descriptor table is valid. A failed
	// reload must never leave a partially overwritten live cache snapshot behind.
	const bool cooked_identity_changed = m_runtimeMaterialCachePath != path || m_nRuntimeMaterialCookedSignature != signature;
	if ( cooked_identity_changed ) {
		for ( int logical = 0; logical < s_nMaxTextureAssets; ++logical ) {
			memset( m_runtimeMaterialChecksumVerified[logical], 0, sizeof( m_runtimeMaterialChecksumVerified[logical] ) );
		}
	}
	memcpy( m_runtimeMaterialCookedLayers, loaded_layers, sizeof( loaded_layers[0] ) * ( size_t )( s_nMaxTextureAssets ) );
	m_runtimeMaterialCachePath = path;
	m_nRuntimeMaterialCookedSignature = signature;
	m_bRuntimeMaterialCookedValid = true;
	// In runtime the cooked bundle is the fixed-settings authority.
	if ( m_pTerrain && !m_pTerrain->isEditorAuthoringMode() ) {
		publishRuntimeMaterialCookedSettings();
	}
	return true;
}

void Terrain3DAssets::publishRuntimeMaterialCookedSettings( void ) {
	if ( !m_bRuntimeMaterialCookedValid ) {
		return;
	}
	m_textureSettings.m_nCount = MIN( m_textureList.size(), s_nMaxTextureAssets );
	for ( int logical = 0; logical < m_textureSettings.m_nCount; ++logical ) {
		const t3dRuntimeMaterialLayerInfo_t &layer = m_runtimeMaterialCookedLayers[( size_t )logical];
		m_textureSettings.m_szColors[( size_t )logical] = layer.m_albedoColor;
		m_textureSettings.m_szNormalDepths[( size_t )logical] = layer.m_fNormalDepth;
		m_textureSettings.m_szAoStrengths[( size_t )logical] = layer.m_fAoStrength;
		m_textureSettings.m_szAoLightAffects[( size_t )logical] = layer.m_fAoLightAffect;
		m_textureSettings.m_szRoughnessMods[( size_t )logical] = layer.m_fRoughness;
		m_textureSettings.m_szMetallics[( size_t )logical] = layer.m_fMetallic;
		m_textureSettings.m_szUvScales[( size_t )logical] = layer.m_fUvScale;
		m_textureSettings.m_szDetiles[( size_t )logical] = layer.m_detile;
		m_textureSettings.m_szDisplacements[( size_t )logical] = layer.m_displacement;
	}
	const int count = m_textureSettings.m_nCount;
	m_textureColors.resize( count );
	m_textureNormalDepths.resize( count );
	m_textureAoStrengths.resize( count );
	m_textureAoLightAffects.resize( count );
	m_textureRoughnessMods.resize( count );
	m_textureMetallics.resize( count );
	m_textureUvScales.resize( count );
	m_textureDetiles.resize( count );
	m_textureDisplacements.resize( count );
	if ( count <= 0 ) {
		return;
	}
	memcpy( m_textureColors.ptrw(), m_textureSettings.m_szColors, sizeof( m_textureSettings.m_szColors[0] ) * ( size_t )( count ) );
	memcpy( m_textureNormalDepths.ptrw(),
	        m_textureSettings.m_szNormalDepths,
	        sizeof( m_textureSettings.m_szNormalDepths[0] ) * ( size_t )( count ) );
	memcpy( m_textureAoStrengths.ptrw(),
	        m_textureSettings.m_szAoStrengths,
	        sizeof( m_textureSettings.m_szAoStrengths[0] ) * ( size_t )( count ) );
	memcpy( m_textureAoLightAffects.ptrw(),
	        m_textureSettings.m_szAoLightAffects,
	        sizeof( m_textureSettings.m_szAoLightAffects[0] ) * ( size_t )( count ) );
	memcpy( m_textureRoughnessMods.ptrw(),
	        m_textureSettings.m_szRoughnessMods,
	        sizeof( m_textureSettings.m_szRoughnessMods[0] ) * ( size_t )( count ) );
	memcpy( m_textureMetallics.ptrw(),
	        m_textureSettings.m_szMetallics,
	        sizeof( m_textureSettings.m_szMetallics[0] ) * ( size_t )( count ) );
	memcpy( m_textureUvScales.ptrw(), m_textureSettings.m_szUvScales, sizeof( m_textureSettings.m_szUvScales[0] ) * ( size_t )( count ) );
	memcpy( m_textureDetiles.ptrw(), m_textureSettings.m_szDetiles, sizeof( m_textureSettings.m_szDetiles[0] ) * ( size_t )( count ) );
	memcpy( m_textureDisplacements.ptrw(),
	        m_textureSettings.m_szDisplacements,
	        sizeof( m_textureSettings.m_szDisplacements[0] ) * ( size_t )( count ) );
}

Ref<Image> Terrain3DAssets::loadRuntimeMaterialCookedImage( const int nLogicalId, const int nChannel ) const {
	if ( !m_bRuntimeMaterialCookedValid || nLogicalId < 0 || nLogicalId >= m_textureList.size() || nChannel < 0
	        || nChannel >= RUNTIME_MATERIAL_CHANNEL_COUNT ) {
		return Ref<Image>();
	}
	const t3dRuntimeMaterialImageInfo_t &info =
	        m_runtimeMaterialCookedLayers[( size_t )( nLogicalId )].m_szChannels[( size_t )( nChannel )];
	if ( info.m_nSize == 0u ) {
		return Ref<Image>();
	}
	const Ref<FileAccess> pFile = FileAccess::open( m_runtimeMaterialCachePath, FileAccess::READ );
	if ( pFile.is_null() ) {
		return Ref<Image>();
	}
	pFile->seek( info.m_iOffset );
	const PackedByteArray bytes = pFile->get_buffer( info.m_nSize );
	if ( bytes.size() != ( int64_t )( info.m_nSize )
	        || t3dAssetsMaterialChecksum( bytes.ptr(), ( size_t )( bytes.size() ) ) != info.m_nChecksum ) {
		return Ref<Image>();
	}
	return Image::create_from_data( info.m_nWidth, info.m_nHeight, info.m_nMipmaps != 0u, ( Image::Format )( info.m_nFormat ), bytes );
}

static const uint64_t s_nRuntimeMaterialDiskBytesPerSecond = 64ull * 1024ull * 1024ull;
static const uint64_t s_nRuntimeMaterialDiskBurstBytes = 64ull * 1024ull * 1024ull;
static const uint64_t s_nRuntimeMaterialPreparedByteLimit = 64ull * 1024ull * 1024ull;
static const uint64_t s_nRuntimeMaterialGpuBytesPerSecond = 128ull * 1024ull * 1024ull;
static const uint64_t s_nRuntimeMaterialGpuBurstBytes = 128ull * 1024ull * 1024ull;
static const uint64_t s_nRuntimeMaterialUpdateCallsPerSecond = 240ull;
static const uint64_t s_nRuntimeMaterialUpdateCallBurst = 16ull;

void Terrain3DAssets::refillRuntimeMaterialBucket( t3dRuntimeMaterialTokenBucket_t &rBucket, const uint64_t nNowUsec ) {
	if ( rBucket.m_nLastRefillUsec == 0u ) {
		rBucket.m_nLastRefillUsec = nNowUsec;
		rBucket.m_nTokens = rBucket.m_nBurst;
		return;
	}
	if ( nNowUsec <= rBucket.m_nLastRefillUsec || rBucket.m_nRatePerSecond == 0u ) {
		return;
	}
	const uint64_t elapsed = nNowUsec - rBucket.m_nLastRefillUsec;
	const uint64_t add =
	        elapsed > UINT64_MAX / rBucket.m_nRatePerSecond ? rBucket.m_nBurst : elapsed * rBucket.m_nRatePerSecond / 1000000ull;
	if ( add > 0u ) {
		rBucket.m_nTokens = MIN( rBucket.m_nBurst, rBucket.m_nTokens + MIN( add, rBucket.m_nBurst ) );
		rBucket.m_nLastRefillUsec = nNowUsec;
	}
}

bool Terrain3DAssets::consumeRuntimeMaterialBucket(
        t3dRuntimeMaterialTokenBucket_t &rBucket, const uint64_t nAmount, const uint64_t nNowUsec ) {
	refillRuntimeMaterialBucket( rBucket, nNowUsec );
	if ( nAmount > rBucket.m_nTokens ) {
		return false;
	}
	rBucket.m_nTokens -= nAmount;
	return true;
}

void Terrain3DAssets::initializeRuntimeMaterialBudgets( void ) {
	const uint64_t now = t3dSteadyClockUsec();
	m_runtimeMaterialDiskBucket = {
		s_nRuntimeMaterialDiskBurstBytes, s_nRuntimeMaterialDiskBytesPerSecond, s_nRuntimeMaterialDiskBurstBytes, now
	};
	m_runtimeMaterialGpuBucket = {
		s_nRuntimeMaterialGpuBurstBytes, s_nRuntimeMaterialGpuBytesPerSecond, s_nRuntimeMaterialGpuBurstBytes, now
	};
	m_runtimeMaterialUpdateCallBucket = {
		s_nRuntimeMaterialUpdateCallBurst, s_nRuntimeMaterialUpdateCallsPerSecond, s_nRuntimeMaterialUpdateCallBurst, now
	};
}

void Terrain3DAssets::startRuntimeMaterialWorker( void ) {
	if ( m_bRuntimeMaterialWorkerRunning || !m_bRuntimeMaterialCookedValid || m_runtimeMaterialCachePath.is_empty() ) {
		return;
	}
	initializeRuntimeMaterialBudgets();
	{
		t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
		m_bRuntimeMaterialWorkerStop = false;
		m_bRuntimeMaterialWorkerRunning = true;
	}
	m_runtimeMaterialWorker = t3dThread_t( &Terrain3DAssets::runtimeMaterialWorkerMain, this );
}

void Terrain3DAssets::stopRuntimeMaterialWorker( void ) {
	if ( !m_bRuntimeMaterialWorkerRunning && !m_runtimeMaterialWorker.joinable() ) {
		return;
	}
	{
		t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
		m_bRuntimeMaterialWorkerStop = true;
	}
	m_runtimeMaterialWorkerWake.notifyAll();
	if ( m_runtimeMaterialWorker.joinable() ) {
		m_runtimeMaterialWorker.join();
	}
	{
		t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
		for ( int i = 0; i < s_nMaxTextureAssets; ++i ) {
			m_runtimeMaterialRequestState[i] = 0u;
			m_runtimeMaterialCompletionRing[i] = t3dRuntimeMaterialCompletion_t{};
		}
		m_iRuntimeMaterialRequestRead = 0;
		m_iRuntimeMaterialRequestWrite = 0;
		m_nRuntimeMaterialRequestCount = 0;
		m_iRuntimeMaterialCompletionRead = 0;
		m_iRuntimeMaterialCompletionWrite = 0;
		m_nRuntimeMaterialCompletionCount = 0;
		m_nRuntimeMaterialPreparedBytesInFlight = 0u;
		m_bRuntimeMaterialWorkerStop = false;
		m_bRuntimeMaterialWorkerRunning = false;
	}
}

bool Terrain3DAssets::queueRuntimeMaterialRead( const int nLogicalId ) {
	if ( !m_bRuntimeMaterialWorkerRunning || nLogicalId <= 0 || nLogicalId >= m_textureList.size() ) {
		return false;
	}
	uint64_t read_bytes = 0u;
	uint64_t prepared_bytes = 0u;
	for ( int channel = 0; channel <= RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT; ++channel ) {
		const t3dRuntimeMaterialImageInfo_t &info = m_runtimeMaterialCookedLayers[( size_t )nLogicalId].m_szChannels[channel];
		read_bytes += info.m_nSize;
		prepared_bytes += info.m_nSize;
	}
	if ( read_bytes == 0u || read_bytes > UINT32_MAX || prepared_bytes > UINT32_MAX ) {
		return false;
	}
	const uint64_t now = t3dSteadyClockUsec();
	t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
	if ( m_runtimeMaterialRequestState[nLogicalId] != 0u ) {
		return true;
	}
	if ( m_nRuntimeMaterialRequestCount >= s_nMaxTextureAssets
	        || m_nRuntimeMaterialPreparedBytesInFlight + prepared_bytes > s_nRuntimeMaterialPreparedByteLimit ) {
		++m_nRuntimeMaterialBudgetRejects;
		return false;
	}
	if ( !consumeRuntimeMaterialBucket( m_runtimeMaterialDiskBucket, read_bytes, now ) ) {
		++m_nRuntimeMaterialBudgetRejects;
		return false;
	}
	t3dRuntimeMaterialReadRequest_t &request = m_runtimeMaterialRequestRing[m_iRuntimeMaterialRequestWrite];
	request.m_nLogicalId = ( int16_t )nLogicalId;
	request.m_nReadBytes = ( uint32_t )read_bytes;
	request.m_nPreparedBytes = ( uint32_t )prepared_bytes;
	m_iRuntimeMaterialRequestWrite = ( m_iRuntimeMaterialRequestWrite + 1 ) % s_nMaxTextureAssets;
	++m_nRuntimeMaterialRequestCount;
	m_nRuntimeMaterialPreparedBytesInFlight += prepared_bytes;
	m_runtimeMaterialRequestState[nLogicalId] = 1u;
	m_runtimeMaterialWorkerWake.notifyOne();
	return true;
}

bool Terrain3DAssets::peekRuntimeMaterialCompletion( t3dRuntimeMaterialCompletion_t &rCompletion ) const {
	t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
	if ( m_nRuntimeMaterialCompletionCount <= 0 ) {
		return false;
	}
	rCompletion = m_runtimeMaterialCompletionRing[m_iRuntimeMaterialCompletionRead];
	return true;
}

void Terrain3DAssets::popRuntimeMaterialCompletion( void ) {
	t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
	if ( m_nRuntimeMaterialCompletionCount <= 0 ) {
		return;
	}
	t3dRuntimeMaterialCompletion_t &completion = m_runtimeMaterialCompletionRing[m_iRuntimeMaterialCompletionRead];
	const int logical = completion.m_nLogicalId;
	if ( completion.m_nUploadBytes <= m_nRuntimeMaterialPreparedBytesInFlight ) {
		m_nRuntimeMaterialPreparedBytesInFlight -= completion.m_nUploadBytes;
	} else {
		m_nRuntimeMaterialPreparedBytesInFlight = 0u;
	}
	if ( logical >= 0 && logical < s_nMaxTextureAssets ) {
		m_runtimeMaterialRequestState[logical] = 0u;
	}
	completion = t3dRuntimeMaterialCompletion_t{};
	m_iRuntimeMaterialCompletionRead = ( m_iRuntimeMaterialCompletionRead + 1 ) % s_nMaxTextureAssets;
	--m_nRuntimeMaterialCompletionCount;
}

void Terrain3DAssets::runtimeMaterialWorkerMain( void ) {
	Terrain3DAllocationPhaseScope phase_scope( TERRAIN3D_ALLOCATION_PHASE_STREAMING_WORKER );
	const Ref<FileAccess> pFile = FileAccess::open( m_runtimeMaterialCachePath, FileAccess::READ );
	{
		t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
		++m_nRuntimeMaterialWorkerFileOpens;
	}
	for ( ;; ) {
		t3dRuntimeMaterialReadRequest_t request;
		{
			t3dUniqueLock_t lock( m_runtimeMaterialWorkerMutex );
			while ( m_nRuntimeMaterialRequestCount <= 0 && !m_bRuntimeMaterialWorkerStop ) {
				m_runtimeMaterialWorkerWake.wait( lock );
			}
			if ( m_bRuntimeMaterialWorkerStop ) {
				break;
			}
			request = m_runtimeMaterialRequestRing[m_iRuntimeMaterialRequestRead];
			m_iRuntimeMaterialRequestRead = ( m_iRuntimeMaterialRequestRead + 1 ) % s_nMaxTextureAssets;
			--m_nRuntimeMaterialRequestCount;
		}

		t3dRuntimeMaterialCompletion_t completion;
		completion.m_nLogicalId = request.m_nLogicalId;
		completion.m_nReadBytes = request.m_nReadBytes;
		completion.m_nUploadBytes = request.m_nPreparedBytes;
		completion.m_bSuccess = pFile.is_valid();
		if ( completion.m_bSuccess ) {
			for ( int channel = 0; channel <= RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT; ++channel ) {
				const t3dRuntimeMaterialImageInfo_t &info =
				        m_runtimeMaterialCookedLayers[( size_t )request.m_nLogicalId].m_szChannels[channel];
				if ( info.m_nSize == 0u ) {
					continue;
				}
				pFile->seek( info.m_iOffset );
				const PackedByteArray bytes = pFile->get_buffer( info.m_nSize );
				if ( bytes.size() != ( int64_t )info.m_nSize ) {
					completion.m_bSuccess = false;
					break;
				}
				if ( !m_runtimeMaterialChecksumVerified[( size_t )request.m_nLogicalId][channel] ) {
					if ( t3dAssetsMaterialChecksum( bytes.ptr(), ( size_t )bytes.size() ) != info.m_nChecksum ) {
						completion.m_bSuccess = false;
						break;
					}
					m_runtimeMaterialChecksumVerified[( size_t )request.m_nLogicalId][channel] = 1u;
				} else {
					t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
					++m_nRuntimeMaterialChecksumCacheHits;
				}
				completion.m_szImages[channel] = Image::create_from_data(
				        info.m_nWidth, info.m_nHeight, info.m_nMipmaps != 0u, ( Image::Format )info.m_nFormat, bytes );
				if ( completion.m_szImages[channel].is_null() ) {
					completion.m_bSuccess = false;
					break;
				}
				++completion.m_nTextureUpdateCalls;
			}
		}
		{
			t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
			if ( m_nRuntimeMaterialCompletionCount < s_nMaxTextureAssets ) {
				m_runtimeMaterialCompletionRing[m_iRuntimeMaterialCompletionWrite] = completion;
				m_iRuntimeMaterialCompletionWrite = ( m_iRuntimeMaterialCompletionWrite + 1 ) % s_nMaxTextureAssets;
				++m_nRuntimeMaterialCompletionCount;
				if ( request.m_nLogicalId >= 0 && request.m_nLogicalId < s_nMaxTextureAssets ) {
					m_runtimeMaterialRequestState[request.m_nLogicalId] = 2u;
				}
			} else {
				if ( request.m_nPreparedBytes <= m_nRuntimeMaterialPreparedBytesInFlight ) {
					m_nRuntimeMaterialPreparedBytesInFlight -= request.m_nPreparedBytes;
				} else {
					m_nRuntimeMaterialPreparedBytesInFlight = 0u;
				}
				if ( request.m_nLogicalId >= 0 && request.m_nLogicalId < s_nMaxTextureAssets ) {
					m_runtimeMaterialRequestState[request.m_nLogicalId] = 0u;
				}
			}
		}
	}
}

bool Terrain3DAssets::publishRuntimeMaterialPreparedLayer( const t3dRuntimeMaterialCompletion_t &rCompletion, const int nPhysicalLayer ) {
	const int logical = rCompletion.m_nLogicalId;
	if ( !m_bRuntimeMaterialCacheActive || !rCompletion.m_bSuccess || logical <= 0 || logical >= m_textureList.size() || nPhysicalLayer <= 0
	        || nPhysicalLayer >= m_nRuntimeMaterialPhysicalLayers ) {
		return false;
	}
	const Ref<Image> &pAlbedo = rCompletion.m_szImages[RUNTIME_MATERIAL_CHANNEL_ALBEDO];
	const Ref<Image> &pNormal = rCompletion.m_szImages[RUNTIME_MATERIAL_CHANNEL_NORMAL];
	const Ref<Image> &pSurface = rCompletion.m_szImages[RUNTIME_MATERIAL_CHANNEL_SURFACE];
	const Ref<Image> &pDisplacement = rCompletion.m_szImages[RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT];
	if ( pAlbedo.is_null() || !m_generatedAlbedoTextures.update( pAlbedo, nPhysicalLayer ) ) {
		return false;
	}
	if ( m_generatedNormalTextures.getRid().is_valid()
	        && ( pNormal.is_null() || !m_generatedNormalTextures.update( pNormal, nPhysicalLayer ) ) ) {
		return false;
	}
	if ( m_generatedSurfaceTextures.getRid().is_valid()
	        && ( pSurface.is_null() || !m_generatedSurfaceTextures.update( pSurface, nPhysicalLayer ) ) ) {
		return false;
	}
	if ( m_generatedDisplacementTextures.getRid().is_valid()
	        && ( pDisplacement.is_null() || !m_generatedDisplacementTextures.update( pDisplacement, nPhysicalLayer ) ) ) {
		return false;
	}
	return true;
}

bool Terrain3DAssets::createRuntimeMaterialChannelArray(
        Terrain3DGeneratedTexture &rGenerated, const int nChannel, const int nPhysicalLayers, const bool bFullyResident ) const {
	const Ref<Image> pDefault = loadRuntimeMaterialCookedImage( 0, nChannel );
	if ( pDefault.is_null() ) {
		rGenerated.clear();
		return nChannel != RUNTIME_MATERIAL_CHANNEL_ALBEDO;
	}
	TypedArray<Image> layers;
	layers.resize( nPhysicalLayers );
	for ( int nPhysical = 0; nPhysical < nPhysicalLayers; ++nPhysical ) {
		const int nLogical = bFullyResident ? nPhysical : 0;
		const Ref<Image> pImage = nLogical == 0 ? pDefault : loadRuntimeMaterialCookedImage( nLogical, nChannel );
		if ( pImage.is_null() ) {
			return false;
		}
		layers[nPhysical] = pImage;
	}
	const t3dRdTextureSemantic_t eSemantic =
	        nChannel == RUNTIME_MATERIAL_CHANNEL_ALBEDO ? T3D_RD_TEXTURE_SOURCE_COLOR : T3D_RD_TEXTURE_LINEAR_DATA;
	return rGenerated.create( layers, eSemantic ).is_valid();
}

void Terrain3DAssets::refreshRuntimeMaterialBindings( void ) {
	if ( !m_pTerrain ) {
		return;
	}
	const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
		pMaterial->update( TEXTURE_ARRAYS );
	}
}

bool Terrain3DAssets::initializeRuntimeMaterialPhysicalArrays( const int nLayerLimit ) {
	if ( !m_bRuntimeMaterialCookedValid || !m_pTerrain || m_textureList.is_empty() ) {
		return false;
	}
	const int cache_limit = nLayerLimit > 0 ? CLAMP( nLayerLimit, 1, s_nMaxTextureAssets )
	                                        : CLAMP( m_pTerrain->getRuntimeMaterialCacheLayers(), 4, s_nMaxTextureAssets );
	const int physical_layers = MIN( m_textureList.size(), cache_limit );
	const bool fully_resident = m_textureList.size() <= cache_limit;
	if ( physical_layers <= 0 || !initializeFarMaterialArray() ) {
		return false;
	}

	Terrain3DGeneratedTexture replacement_albedo;
	Terrain3DGeneratedTexture replacement_normal;
	Terrain3DGeneratedTexture replacement_surface;
	Terrain3DGeneratedTexture replacement_displacement;
	if ( !createRuntimeMaterialChannelArray( replacement_albedo, RUNTIME_MATERIAL_CHANNEL_ALBEDO, physical_layers, fully_resident )
	        || !createRuntimeMaterialChannelArray( replacement_normal, RUNTIME_MATERIAL_CHANNEL_NORMAL, physical_layers, fully_resident )
	        || !createRuntimeMaterialChannelArray( replacement_surface, RUNTIME_MATERIAL_CHANNEL_SURFACE, physical_layers, fully_resident )
	        || !createRuntimeMaterialChannelArray(
	                replacement_displacement, RUNTIME_MATERIAL_CHANNEL_DISPLACEMENT, physical_layers, fully_resident ) ) {
		t3dAssetsReleaseGeneratedReplacements( replacement_albedo, replacement_normal, replacement_surface, replacement_displacement );
		return false;
	}
	m_generatedAlbedoTextures.swap( replacement_albedo );
	m_generatedNormalTextures.swap( replacement_normal );
	m_generatedSurfaceTextures.swap( replacement_surface );
	m_generatedDisplacementTextures.swap( replacement_displacement );
	t3dAssetsReleaseGeneratedReplacements( replacement_albedo, replacement_normal, replacement_surface, replacement_displacement );

	m_nRuntimeMaterialPhysicalLayers = physical_layers;
	for ( int index = 0; index < s_nMaxTextureAssets; ++index ) {
		m_runtimeMaterialLogicalToPhysical[( size_t )( index )] = -1;
		m_runtimeMaterialPhysicalToLogical[( size_t )( index )] = -1;
		m_runtimeMaterialLastUseTick[( size_t )( index )] = 0u;
		m_runtimeMaterialRetireAfterTick[( size_t )( index )] = 0u;
	}
	m_nRuntimeMaterialResidentCount = fully_resident ? m_textureList.size() : 1;
	for ( int physical = 0; physical < m_nRuntimeMaterialResidentCount; ++physical ) {
		m_runtimeMaterialPhysicalToLogical[( size_t )( physical )] = physical;
		m_runtimeMaterialLogicalToPhysical[( size_t )( physical )] = physical;
		m_runtimeMaterialLastUseTick[( size_t )( physical )] = ++m_nRuntimeMaterialTick;
	}
	m_bRuntimeMaterialCacheActive = true;
	advanceTexturePublicationGeneration( true );
	return true;
}

void Terrain3DAssets::releaseRuntimeMaterialSourceTextures( void ) {
	if ( IS_EDITOR ) {
		return;
	}
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_null() ) {
			continue;
		}
		pAsset->disconnectPackedPbrResource();
		pAsset->m_albedoTexture.unref();
		pAsset->m_normalTexture.unref();
		pAsset->m_surfaceTexture.unref();
		pAsset->m_displacementTexture.unref();
		pAsset->m_pbrTexture.unref();
		pAsset->m_packedPbrResource.unref();
	}
	m_bRuntimeMaterialSourceTexturesReleased = true;
}

bool Terrain3DAssets::configureRuntimeMaterialCache( void ) {
	stopRuntimeMaterialWorker();
	if ( !m_pTerrain || m_textureList.is_empty() ) {
		m_bRuntimeMaterialCacheActive = false;
		return false;
	}
	const bool runtime_cache_requested = m_pTerrain->isRuntimeMaterialStreamingEnabled() && !m_pTerrain->isEditorAuthoringMode();
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
			refreshRuntimeMaterialBindings();
			return false;
		}
		if ( !m_bRuntimeMaterialCacheActive ) {
			return false;
		}
		if ( !m_bRuntimeMaterialCookedValid && !loadRuntimeMaterialCookedIndex() ) {
			LOG( WARN,
			        "Cannot disable runtime material streaming because the cooked material bundle is unavailable; "
			        "keeping the bounded cache active." );
			return true;
		}
		if ( !initializeRuntimeMaterialPhysicalArrays( s_nMaxTextureAssets ) ) {
			LOG( WARN,
			        "Cannot rebuild full terrain material arrays from the cooked bundle; keeping the bounded runtime "
			        "material cache active." );
			return true;
		}
		m_bRuntimeMaterialCacheActive = false;
		refreshRuntimeMaterialBindings();
		return false;
	}

	const bool strict_baked_runtime = m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode();
	if ( !m_bRuntimeMaterialCookedValid && !loadRuntimeMaterialCookedIndex() ) {
		if ( strict_baked_runtime ) {
			LOG( ERROR, "Strict baked runtime requires a valid terrain3d_materials.t3dmatcache artifact." );
			m_bRuntimeMaterialCacheActive = false;
			return false;
		}
		LOG( WARN, "Cooked runtime material cache unavailable or stale; retaining full terrain material arrays." );
		return m_bRuntimeMaterialCacheActive;
	}
	if ( !initializeRuntimeMaterialPhysicalArrays() ) {
		if ( strict_baked_runtime ) {
			LOG( ERROR, "Strict baked runtime could not initialize material arrays from the cooked bundle." );
			m_bRuntimeMaterialCacheActive = false;
			return false;
		}
		LOG( WARN, "Failed to rebuild the bounded runtime material cache; retaining the previous material arrays." );
		return m_bRuntimeMaterialCacheActive;
	}

	startRuntimeMaterialWorker();

	// Seed the first priority set without blocking startup. The owner admits
	// byte-bounded requests here; the worker prepares them asynchronously and
	// later owner ticks publish completed layers within upload/call budgets.
	if ( m_pTerrain->getData() ) {
		uint8_t priority_ids[s_nMaxTextureAssets] = {};
		const int priority_count = m_pTerrain->getData()->fillGpuMaterialPriorityIds( priority_ids, s_nMaxTextureAssets );
		const int mapped_capacity = MAX( 1, m_nRuntimeMaterialPhysicalLayers - s_nRuntimeMaterialSpareLayers );
		updateRuntimeMaterialResidencyInternal( priority_ids, priority_count, mapped_capacity );
	}
	if ( m_pTerrain->isRuntimeReleaseSourceMaterialTextures() ) {
		releaseRuntimeMaterialSourceTextures();
	}
	refreshRuntimeMaterialBindings();
	return true;
}

void Terrain3DAssets::updateRuntimeMaterialResidencyInternal(
        const uint8_t *const pPriorityIds, const int nPriorityCount, const int nUploadBudget ) {
	m_nRuntimeMaterialUploadsLastTick = 0;
	m_nRuntimeMaterialEvictionsLastTick = 0;
	m_nRuntimeMaterialReadBytesLastTick = 0u;
	m_nRuntimeMaterialUploadBytesLastTick = 0u;
	m_nRuntimeMaterialCompletionsLastTick = 0;
	if ( !m_bRuntimeMaterialCacheActive || !m_pTerrain || !pPriorityIds || nPriorityCount <= 0 ) {
		return;
	}

	++m_nRuntimeMaterialTick;
	for ( int physical = 1; physical < m_nRuntimeMaterialPhysicalLayers; ++physical ) {
		if ( m_runtimeMaterialPhysicalToLogical[( size_t )physical] == -2
		        && m_nRuntimeMaterialTick >= m_runtimeMaterialRetireAfterTick[( size_t )physical] ) {
			m_runtimeMaterialPhysicalToLogical[( size_t )physical] = -1;
			m_runtimeMaterialRetireAfterTick[( size_t )physical] = 0u;
		}
	}

	uint32_t desired_mask = 1u;
	for ( int index = 0; index < nPriorityCount; ++index ) {
		const int logical = ( int )pPriorityIds[index];
		if ( logical >= 0 && logical < m_textureList.size() ) {
			desired_mask |= 1u << ( uint32_t )logical;
		}
	}

	// Admission is metadata-only on the owner. Disk I/O, checksum validation and
	// Image construction are performed by the persistent streaming worker.
	for ( int priority = 0; priority < nPriorityCount; ++priority ) {
		const int logical = ( int )pPriorityIds[priority];
		if ( logical <= 0 || logical >= m_textureList.size() ) {
			continue;
		}
		const int existing = m_runtimeMaterialLogicalToPhysical[( size_t )logical];
		if ( existing > 0 && existing < m_nRuntimeMaterialPhysicalLayers
		        && m_runtimeMaterialPhysicalToLogical[( size_t )existing] == logical ) {
			m_runtimeMaterialLastUseTick[( size_t )existing] = m_nRuntimeMaterialTick;
			continue;
		}
		queueRuntimeMaterialRead( logical );
	}

	const int mapped_capacity = MAX( 1, m_nRuntimeMaterialPhysicalLayers - s_nRuntimeMaterialSpareLayers );
	const int completion_budget = MAX( 1, nUploadBudget );
	bool mapping_changed = false;
	while ( m_nRuntimeMaterialCompletionsLastTick < completion_budget ) {
		t3dRuntimeMaterialCompletion_t completion;
		if ( !peekRuntimeMaterialCompletion( completion ) ) {
			break;
		}

		const int logical = completion.m_nLogicalId;
		if ( logical <= 0 || logical >= m_textureList.size() || ( desired_mask & ( 1u << ( uint32_t )logical ) ) == 0u
		        || !completion.m_bSuccess ) {
			m_nRuntimeMaterialReadBytesLastTick += completion.m_nReadBytes;
			popRuntimeMaterialCompletion();
			++m_nRuntimeMaterialCompletionsLastTick;
			continue;
		}

		const int existing = m_runtimeMaterialLogicalToPhysical[( size_t )logical];
		if ( existing > 0 && existing < m_nRuntimeMaterialPhysicalLayers
		        && m_runtimeMaterialPhysicalToLogical[( size_t )existing] == logical ) {
			m_runtimeMaterialLastUseTick[( size_t )existing] = m_nRuntimeMaterialTick;
			m_nRuntimeMaterialReadBytesLastTick += completion.m_nReadBytes;
			popRuntimeMaterialCompletion();
			++m_nRuntimeMaterialCompletionsLastTick;
			continue;
		}

		const uint64_t now = t3dSteadyClockUsec();
		refillRuntimeMaterialBucket( m_runtimeMaterialGpuBucket, now );
		refillRuntimeMaterialBucket( m_runtimeMaterialUpdateCallBucket, now );
		if ( completion.m_nUploadBytes > m_runtimeMaterialGpuBucket.m_nTokens
		        || completion.m_nTextureUpdateCalls > m_runtimeMaterialUpdateCallBucket.m_nTokens ) {
			++m_nRuntimeMaterialBudgetRejects;
			break;
		}

		if ( m_nRuntimeMaterialResidentCount >= mapped_capacity ) {
			int victim_physical = -1;
			uint64_t oldest = eastl::numeric_limits<uint64_t>::max();
			for ( int physical = 1; physical < m_nRuntimeMaterialPhysicalLayers; ++physical ) {
				const int victim_logical = m_runtimeMaterialPhysicalToLogical[( size_t )physical];
				if ( victim_logical <= 0 || ( desired_mask & ( 1u << ( uint32_t )victim_logical ) ) != 0u ) {
					continue;
				}
				if ( m_runtimeMaterialLastUseTick[( size_t )physical] < oldest ) {
					oldest = m_runtimeMaterialLastUseTick[( size_t )physical];
					victim_physical = physical;
				}
			}
			if ( victim_physical < 0 ) {
				// Keep the prepared completion queued. It is already byte-accounted and
				// can publish as soon as a mapped layer becomes replaceable.
				break;
			}
			const int victim_logical = m_runtimeMaterialPhysicalToLogical[( size_t )victim_physical];
			m_runtimeMaterialLogicalToPhysical[( size_t )victim_logical] = -1;
			m_runtimeMaterialPhysicalToLogical[( size_t )victim_physical] = -2;
			m_runtimeMaterialRetireAfterTick[( size_t )victim_physical] = m_nRuntimeMaterialTick + 4u;
			--m_nRuntimeMaterialResidentCount;
			++m_nRuntimeMaterialEvictionsLastTick;
			mapping_changed = true;
		}

		int free_physical = -1;
		for ( int physical = 1; physical < m_nRuntimeMaterialPhysicalLayers; ++physical ) {
			if ( m_runtimeMaterialPhysicalToLogical[( size_t )physical] == -1 ) {
				free_physical = physical;
				break;
			}
		}
		if ( free_physical < 0 ) {
			break;
		}

		m_runtimeMaterialGpuBucket.m_nTokens -= completion.m_nUploadBytes;
		m_runtimeMaterialUpdateCallBucket.m_nTokens -= completion.m_nTextureUpdateCalls;

		const bool published = publishRuntimeMaterialPreparedLayer( completion, free_physical );
		m_nRuntimeMaterialReadBytesLastTick += completion.m_nReadBytes;
		popRuntimeMaterialCompletion();
		++m_nRuntimeMaterialCompletionsLastTick;
		if ( !published ) {
			continue;
		}

		m_runtimeMaterialPhysicalToLogical[( size_t )free_physical] = logical;
		m_runtimeMaterialLogicalToPhysical[( size_t )logical] = free_physical;
		m_runtimeMaterialLastUseTick[( size_t )free_physical] = m_nRuntimeMaterialTick;
		++m_nRuntimeMaterialResidentCount;
		++m_nRuntimeMaterialUploadsLastTick;
		m_nRuntimeMaterialUploadBytesLastTick += completion.m_nUploadBytes;
		mapping_changed = true;
	}

	if ( mapping_changed ) {
		advanceTexturePublicationGeneration( true );
		const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateRuntimeMaterialLayerMap();
		}
	}
}

void Terrain3DAssets::updateRuntimeMaterialResidency( const uint8_t *const pPriorityIds, const int nPriorityCount ) {
	const int upload_budget = m_pTerrain ? MAX( 1, m_pTerrain->getRuntimeMaterialUploadBudgetLayers() ) : 1;
	updateRuntimeMaterialResidencyInternal( pPriorityIds, nPriorityCount, upload_budget );
}

PackedInt32Array Terrain3DAssets::getTextureLayerMap( void ) const {
	PackedInt32Array map;
	map.resize( s_nMaxTextureAssets );
	int32_t *const pWrite = map.ptrw();
	for ( int index = 0; index < s_nMaxTextureAssets; ++index ) {
		pWrite[index] = m_bRuntimeMaterialCacheActive ? m_runtimeMaterialLogicalToPhysical[( size_t )( index )] : index;
	}
	return map;
}

PackedColorArray Terrain3DAssets::getRuntimeMaterialAverageAlbedo( void ) const {
	PackedColorArray averages;
	averages.resize( s_nMaxTextureAssets );
	Color *const pWrite = averages.ptrw();
	for ( int index = 0; index < s_nMaxTextureAssets; ++index ) {
		pWrite[index] = m_bRuntimeMaterialCookedValid ? m_runtimeMaterialCookedLayers[( size_t )( index )].m_averageAlbedo : s_colorWhite;
	}
	return averages;
}

Dictionary Terrain3DAssets::getRuntimeMaterialCacheStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled();
	stats["active"] = m_bRuntimeMaterialCacheActive;
	stats["cooked_valid"] = m_bRuntimeMaterialCookedValid;
	stats["logical_materials"] = m_textureList.size();
	stats["physical_layers"] = m_nRuntimeMaterialPhysicalLayers;
	stats["resident"] = m_nRuntimeMaterialResidentCount;
	stats["uploads_last_tick"] = m_nRuntimeMaterialUploadsLastTick;
	stats["evictions_last_tick"] = m_nRuntimeMaterialEvictionsLastTick;
	stats["read_bytes_last_tick"] = ( int64_t )m_nRuntimeMaterialReadBytesLastTick;
	stats["upload_bytes_last_tick"] = ( int64_t )m_nRuntimeMaterialUploadBytesLastTick;
	stats["completed_layers_last_tick"] = m_nRuntimeMaterialCompletionsLastTick;
	{
		t3dScopedLock_t lock( m_runtimeMaterialWorkerMutex );
		stats["queued_reads"] = m_nRuntimeMaterialRequestCount;
		stats["prepared_completions"] = m_nRuntimeMaterialCompletionCount;
		stats["prepared_bytes_in_flight"] = ( int64_t )m_nRuntimeMaterialPreparedBytesInFlight;
		stats["budget_rejects"] = ( int64_t )m_nRuntimeMaterialBudgetRejects;
		stats["checksum_cache_hits"] = ( int64_t )m_nRuntimeMaterialChecksumCacheHits;
		stats["worker_file_opens"] = ( int64_t )m_nRuntimeMaterialWorkerFileOpens;
		stats["worker_running"] = m_bRuntimeMaterialWorkerRunning;
	}
	stats["disk_budget_bytes_per_second"] = ( int64_t )s_nRuntimeMaterialDiskBytesPerSecond;
	stats["prepared_byte_limit"] = ( int64_t )s_nRuntimeMaterialPreparedByteLimit;
	stats["gpu_budget_bytes_per_second"] = ( int64_t )s_nRuntimeMaterialGpuBytesPerSecond;
	stats["texture_update_calls_per_second"] = ( int64_t )s_nRuntimeMaterialUpdateCallsPerSecond;
	stats["generated_payload_bytes"] = ( int64_t )( getGeneratedTexturePayloadBytes() );
	stats["far_albedo_layers"] = m_generatedFarAlbedoTextures.size();
	stats["far_albedo_max_lod"] = m_generatedFarAlbedoTextures.maxMipmapLevel();
	stats["source_textures_released"] = m_bRuntimeMaterialSourceTexturesReleased;
	uint64_t mapping_hash = 1469598103934665603ull;
	for ( int logical = 0; logical < s_nMaxTextureAssets; ++logical ) {
		mapping_hash ^= ( uint64_t )( ( uint32_t )( m_runtimeMaterialLogicalToPhysical[( size_t )( logical )] + 2 ) );
		mapping_hash *= 1099511628211ull;
	}
	stats["logical_to_physical_hash"] = String::num_uint64( mapping_hash, 16 );

	int required_count = 0;
	int required_resident = 0;
	int required_pending = 0;
	if ( m_pTerrain && m_pTerrain->getData() && m_bRuntimeMaterialCacheActive ) {
		uint8_t required_ids[s_nMaxTextureAssets] = {};
		required_count = m_pTerrain->getData()->fillGpuMaterialPriorityIds( required_ids, s_nMaxTextureAssets );
		for ( int index = 0; index < required_count; ++index ) {
			const int logical = ( int )( required_ids[index] );
			const int physical =
			        logical >= 0 && logical < s_nMaxTextureAssets ? m_runtimeMaterialLogicalToPhysical[( size_t )( logical )] : -1;
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

Dictionary Terrain3DAssets::cookRuntimeMaterialCache( void ) {
	stopRuntimeMaterialWorker();
	Dictionary result;
	result["complete"] = false;
	result["materials"] = m_textureList.size();
	const bool restore_runtime_cache =
	        m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled() && !m_pTerrain->isEditorAuthoringMode();
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
	pFile->store_64( s_nRuntimeMaterialMagic );
	pFile->store_32( s_nRuntimeMaterialVersion );
	pFile->store_32( ( uint32_t )( m_textureList.size() ) );
	pFile->store_32( ( uint32_t )( m_texturePackingMode ) );
	pFile->store_32( m_generatedDisplacementTextures.getRid().is_valid() ? 1u : 0u );
	pFile->store_64( signature );
	const uint64_t descriptor_offset = pFile->get_position();
	for ( int logical = 0; logical < s_nMaxTextureAssets; ++logical ) {
		for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
			pFile->store_64( 0u );
			for ( int field = 0; field < 6; ++field ) {
				pFile->store_32( 0u );
			}
		}
		for ( int component = 0; component < 4; ++component ) {
			pFile->store_float( 1.0f );
		}
		for ( int component = 0; component < 4; ++component ) {
			pFile->store_float( 1.0f );
		}
		for ( int setting = 0; setting < 10; ++setting ) {
			pFile->store_float( 0.0f );
		}
		pFile->store_32( ( uint32_t )logical );
	}
	t3dRuntimeMaterialLayerInfo_t descriptors[s_nMaxTextureAssets]{};
	const Terrain3DGeneratedTexture *channel_textures[RUNTIME_MATERIAL_CHANNEL_COUNT] = { &m_generatedAlbedoTextures,
		&m_generatedNormalTextures,
		&m_generatedSurfaceTextures,
		&m_generatedDisplacementTextures,
		&m_generatedAlbedoTextures };
	bool ok = true;
	for ( int logical = 0; logical < m_textureList.size() && ok; ++logical ) {
		t3dRuntimeMaterialLayerInfo_t &layer = descriptors[( size_t )logical];
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[logical];
		if ( pAsset.is_valid() ) {
			layer.m_albedoColor = pAsset->getAlbedoColor();
			layer.m_fNormalDepth = pAsset->getNormalDepth();
			layer.m_fAoStrength = pAsset->getAoStrength();
			layer.m_fAoLightAffect = pAsset->getAoLightAffect();
			layer.m_fRoughness = pAsset->getRoughness();
			layer.m_fMetallic = pAsset->getMetallic();
			layer.m_fUvScale = pAsset->getUvScale();
			layer.m_detile = Vector2( pAsset->getDetilingRotation(), pAsset->getDetilingShift() );
			layer.m_displacement = Vector2( pAsset->getDisplacementOffset(), pAsset->getDisplacementScale() );
		}
		layer.m_iBundleIndex = ( uint32_t )logical;
		for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
			const Terrain3DGeneratedTexture *const pTexture = channel_textures[channel];
			if ( !pTexture || ( channel != RUNTIME_MATERIAL_CHANNEL_FAR_ALBEDO && !pTexture->getRid().is_valid() ) ) {
				continue;
			}
			// Terrain3DGeneratedTexture retains the authoritative CPU layer images used to
			// create/update its direct RD texture. Runtime material bundles consume that
			// retained source instead of bridging Terrain3D-owned textures back through a native texture bridge.
			Ref<Image> pImage = pTexture->getLayerImage( logical );
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
				descriptors[( size_t )( logical )].m_averageAlbedo = t3dAssetsMaterialAverageAlbedo( pImage );
			}
			const PackedByteArray bytes = pImage->get_data();
			t3dRuntimeMaterialImageInfo_t &info = descriptors[( size_t )( logical )].m_szChannels[( size_t )( channel )];
			info.m_iOffset = pFile->get_position();
			info.m_nSize = ( uint32_t )( bytes.size() );
			info.m_nChecksum = t3dAssetsMaterialChecksum( bytes.ptr(), ( size_t )( bytes.size() ) );
			info.m_nWidth = pImage->get_width();
			info.m_nHeight = pImage->get_height();
			info.m_nFormat = ( int32_t )( pImage->get_format() );
			info.m_nMipmaps = pImage->has_mipmaps() ? 1u : 0u;
			pFile->store_buffer( bytes );
		}
	}
	if ( ok ) {
		pFile->seek( descriptor_offset );
		for ( int logical = 0; logical < s_nMaxTextureAssets; ++logical ) {
			for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
				const t3dRuntimeMaterialImageInfo_t &info = descriptors[( size_t )( logical )].m_szChannels[( size_t )( channel )];
				pFile->store_64( info.m_iOffset );
				pFile->store_32( info.m_nSize );
				pFile->store_32( info.m_nChecksum );
				pFile->store_32( ( uint32_t )( info.m_nWidth ) );
				pFile->store_32( ( uint32_t )( info.m_nHeight ) );
				pFile->store_32( ( uint32_t )( info.m_nFormat ) );
				pFile->store_32( info.m_nMipmaps );
			}
			const t3dRuntimeMaterialLayerInfo_t &layer = descriptors[( size_t )logical];
			const Color average = layer.m_averageAlbedo;
			pFile->store_float( average.r );
			pFile->store_float( average.g );
			pFile->store_float( average.b );
			pFile->store_float( average.a );
			pFile->store_float( layer.m_albedoColor.r );
			pFile->store_float( layer.m_albedoColor.g );
			pFile->store_float( layer.m_albedoColor.b );
			pFile->store_float( layer.m_albedoColor.a );
			pFile->store_float( layer.m_fNormalDepth );
			pFile->store_float( layer.m_fAoStrength );
			pFile->store_float( layer.m_fAoLightAffect );
			pFile->store_float( layer.m_fRoughness );
			pFile->store_float( layer.m_fMetallic );
			pFile->store_float( layer.m_fUvScale );
			pFile->store_float( layer.m_detile.x );
			pFile->store_float( layer.m_detile.y );
			pFile->store_float( layer.m_displacement.x );
			pFile->store_float( layer.m_displacement.y );
			pFile->store_32( layer.m_iBundleIndex );
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
	t3dRuntimeMaterialLayerInfo_t previous_layers[s_nMaxTextureAssets]{};
	memcpy( previous_layers,
	        m_runtimeMaterialCookedLayers,
	        sizeof( m_runtimeMaterialCookedLayers[0] ) * ( size_t )( s_nMaxTextureAssets ) );
	const String previous_cache_path = m_runtimeMaterialCachePath;
	const uint64_t previous_signature = m_nRuntimeMaterialCookedSignature;
	const bool previous_cooked_valid = m_bRuntimeMaterialCookedValid;
	bool published_valid = loadRuntimeMaterialCookedIndex();
	if ( published_valid ) {
		for ( int logical = 0; logical < m_textureList.size() && published_valid; ++logical ) {
			for ( int channel = 0; channel < RUNTIME_MATERIAL_CHANNEL_COUNT; ++channel ) {
				const t3dRuntimeMaterialImageInfo_t &info =
				        m_runtimeMaterialCookedLayers[( size_t )( logical )].m_szChannels[( size_t )( channel )];
				if ( info.m_nSize > 0u && loadRuntimeMaterialCookedImage( logical, channel ).is_null() ) {
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
		memcpy( m_runtimeMaterialCookedLayers, previous_layers, sizeof( previous_layers[0] ) * ( size_t )( s_nMaxTextureAssets ) );
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
		memcpy( m_runtimeMaterialCookedLayers, previous_layers, sizeof( previous_layers[0] ) * ( size_t )( s_nMaxTextureAssets ) );
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
	result["signature"] = ( int64_t )( signature );
	result["complete"] = true;
	return result;
}

void Terrain3DAssets::connectAssetSignals( const t3dAssetType_t type, const Ref<Terrain3DAssetResource> &rAsset ) {
	if ( rAsset.is_null() ) {
		return;
	}
	const Callable id_callback = callable_mp( this, &Terrain3DAssets::swapIds );
	if ( !rAsset->is_connected( "id_changed", id_callback ) ) {
		rAsset->connect( "id_changed", id_callback );
	}
	if ( type == TYPE_TEXTURE ) {
		Ref<Terrain3DTextureAsset> pTexture = rAsset;
		if ( pTexture.is_null() ) {
			return;
		}
		const Callable file_callback = callable_mp( this, &Terrain3DAssets::updateTextureFiles );
		const Callable setting_callback = callable_mp( this, &Terrain3DAssets::updateTextureSettings );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateTexturePreview );
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
		Ref<Terrain3DMeshAsset> pMesh = rAsset;
		if ( pMesh.is_null() ) {
			return;
		}
		const Callable settings_callback = callable_mp( this, &Terrain3DAssets::updateMesh );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateMeshPreview );
		if ( !pMesh->is_connected( "instancer_setting_changed", settings_callback ) ) {
			pMesh->connect( "instancer_setting_changed", settings_callback );
		}
		if ( !pMesh->is_connected( "preview_changed", preview_callback ) ) {
			pMesh->connect( "preview_changed", preview_callback );
		}
	}
}

void Terrain3DAssets::disconnectAssetSignals( const t3dAssetType_t type, const Ref<Terrain3DAssetResource> &rAsset ) {
	if ( rAsset.is_null() ) {
		return;
	}
	const Callable id_callback = callable_mp( this, &Terrain3DAssets::swapIds );
	if ( rAsset->is_connected( "id_changed", id_callback ) ) {
		rAsset->disconnect( "id_changed", id_callback );
	}
	if ( type == TYPE_TEXTURE ) {
		Ref<Terrain3DTextureAsset> pTexture = rAsset;
		if ( pTexture.is_null() ) {
			return;
		}
		const Callable file_callback = callable_mp( this, &Terrain3DAssets::updateTextureFiles );
		const Callable setting_callback = callable_mp( this, &Terrain3DAssets::updateTextureSettings );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateTexturePreview );
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
		Ref<Terrain3DMeshAsset> pMesh = rAsset;
		if ( pMesh.is_null() ) {
			return;
		}
		const Callable settings_callback = callable_mp( this, &Terrain3DAssets::updateMesh );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateMeshPreview );
		if ( pMesh->is_connected( "instancer_setting_changed", settings_callback ) ) {
			pMesh->disconnect( "instancer_setting_changed", settings_callback );
		}
		if ( pMesh->is_connected( "preview_changed", preview_callback ) ) {
			pMesh->disconnect( "preview_changed", preview_callback );
		}
	}
}

Ref<Terrain3DAssetResource> Terrain3DAssets::makePlaceholderAsset( const t3dAssetType_t type, const int nId ) const {
	Ref<Terrain3DAssetResource> pAsset;
	if ( type == TYPE_TEXTURE ) {
		Ref<Terrain3DTextureAsset> pTexture;
		pTexture.instantiate();
		pAsset = pTexture;
	} else if ( type == TYPE_MESH ) {
		Ref<Terrain3DMeshAsset> pMesh;
		pMesh.instantiate();
		pAsset = pMesh;
	}
	if ( pAsset.is_valid() ) {
		pAsset->m_nId = nId;
		pAsset->m_bPlaceholder = true;
	}
	return pAsset;
}

void Terrain3DAssets::swapIds( const t3dAssetType_t type, const int nSrcId, const int nDstId ) {
	m_bPersistenceDirty = true;
	LOG( INFO, "Swapping asset ID: ", nSrcId, " and ID: ", nDstId );
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
		LOG( ERROR, "Source ID out of range: ", nSrcId );
		return;
	}
	Ref<Terrain3DAssetResource> pResSrc = list[nSrcId];
	if ( pResSrc.is_null() ) {
		LOG( ERROR, "Source Asset is null at ID: ", nSrcId );
		return;
	}
	// User can type in any value, -1 means swap with first, >size means swap with last
	int dst_id = CLAMP( nDstId, 0, list.size() - 1 );
	if ( dst_id == nSrcId ) {
		// They're likely the same due to the clamp, when new ID is <0 or >= array_size)
		pResSrc->m_nId = nSrcId;
		return;
	}
	Ref<Terrain3DAssetResource> pResDst = list[dst_id];
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
				LOG( ERROR, "Changing IDs before the terrain is initialized. Meshes on the ground may be wrong." );
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
void Terrain3DAssets::setAssetList( const t3dAssetType_t type, const TypedArray<Terrain3DAssetResource> &rList ) {
	m_bPersistenceDirty = true;
	const int max_size = type == TYPE_TEXTURE ? s_nMaxTextureAssets : type == TYPE_MESH ? s_nMaxMeshAssets : 0;
	if ( max_size == 0 ) {
		return;
	}

	// Build a fresh fixed-slot registry. Reusing the old Array makes replacement
	// depend on its previous occupants and can silently mix old and new assets.
	const int array_size = CLAMP( rList.size(), 0, max_size );
	Array replacement;
	replacement.resize( array_size );
	eastl::bitset<s_nMaxMeshAssets> occupied;
	int next_free = 0;
	for ( int i = 0; i < array_size; ++i ) {
		Ref<Terrain3DAssetResource> pAsset = rList[i];
		if ( pAsset.is_null() ) {
			continue;
		}
		int target_id = pAsset->getId();
		if ( target_id < 0 || target_id >= array_size || occupied.test( ( size_t )( target_id ) ) ) {
			while ( next_free < array_size && occupied.test( ( size_t )( next_free ) ) ) {
				++next_free;
			}
			if ( next_free >= array_size ) {
				LOG( ERROR, "No free slot for asset: ", pAsset->get_name() );
				continue;
			}
			LOG( WARN, pAsset->get_class(), " ID ", target_id, " is invalid or duplicated. Assigning slot ", next_free );
			target_id = next_free;
		}
		pAsset->m_nId = target_id;
		pAsset->m_bPlaceholder = false;
		replacement[target_id] = pAsset;
		occupied.set( ( size_t )( target_id ) );
	}

	// Holes are explicit placeholder resources. Texture/control IDs and mesh IDs
	// are stable slot identifiers and therefore cannot be compacted.
	for ( int slot = 0; slot < array_size; ++slot ) {
		if ( !occupied.test( ( size_t )( slot ) ) ) {
			replacement[slot] = makePlaceholderAsset( type, slot );
		}
	}

	if ( type == TYPE_TEXTURE ) {
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pOldAsset = ( m_textureList )[iTexture];
			disconnectAssetSignals( TYPE_TEXTURE, pOldAsset );
		}
		m_textureList.clear();
		m_textureList.resize( array_size );
		for ( int i = 0; i < array_size; ++i ) {
			Ref<Terrain3DTextureAsset> pAsset = replacement[i];
			m_textureList[i] = pAsset;
			connectAssetSignals( TYPE_TEXTURE, pAsset );
			if ( occupied.test( ( size_t )( i ) ) ) {
				pAsset->initialize();
			}
		}
	} else {
		for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
			const Ref<Terrain3DMeshAsset> pOldAsset = ( m_meshList )[iMesh];
			disconnectAssetSignals( TYPE_MESH, pOldAsset );
		}
		m_meshList.clear();
		m_meshList.resize( array_size );
		for ( int i = 0; i < array_size; ++i ) {
			Ref<Terrain3DMeshAsset> pAsset = replacement[i];
			m_meshList[i] = pAsset;
			connectAssetSignals( TYPE_MESH, pAsset );
			if ( occupied.test( ( size_t )( i ) ) ) {
				pAsset->initialize();
			}
		}
	}
}

void Terrain3DAssets::setAsset( const t3dAssetType_t type, const int nId, const Ref<Terrain3DAssetResource> &rAsset ) {
	m_bPersistenceDirty = true;
	LOG( INFO, "Setting asset type: ", type, ", ID: ", nId, ", asset: ", rAsset );
	const int max_size = type == TYPE_TEXTURE ? s_nMaxTextureAssets : type == TYPE_MESH ? s_nMaxMeshAssets : 0;
	if ( max_size == 0 || nId < 0 || nId >= max_size ) {
		LOG( ERROR, "Invalid asset id: ", nId, " range is 0-", max_size - 1 );
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
			const Ref<Terrain3DAssetResource> placeholder = makePlaceholderAsset( type, slot );
			list[slot] = placeholder;
			connectAssetSignals( type, placeholder );
		}
	}

	const Ref<Terrain3DAssetResource> pOldAsset = list[nId];
	if ( rAsset.is_null() ) {
		// Texture IDs are persisted in control-map pixels. Removing the final
		// array entry cannot compact the registry without remapping every painted
		// pixel, so texture deletion always leaves a placeholder layer. Mesh IDs
		// may trim the tail only after set_mesh_asset() has cleared all instances.
		const bool bPreserveStableSlot = type == TYPE_TEXTURE || nId < list.size() - 1;
		disconnectAssetSignals( type, pOldAsset );
		if ( bPreserveStableSlot ) {
			const Ref<Terrain3DAssetResource> placeholder = makePlaceholderAsset( type, nId );
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

void Terrain3DAssets::validateArrayMember( const Ref<Texture2D> &rTexture,
        const Ref<Terrain3DTextureAsset> &rAsset,
        const String &rLabel,
        t3dTextureArrayLayout_t &rLayout,
        const Image::Format eRequiredFormat,
        const bool bAllowCompressedSurface ) {
	if ( rTexture.is_null() ) {
		return;
	}
	const Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
		LOG( ERROR, "Texture ID ", rAsset->getId(), " ", rLabel, " image is invalid or empty." );
		return;
	}
	const Vector2i vTextureSize = pImage->get_size();
	const Image::Format eFormat = pImage->get_format();
	const bool bMipmaps = pImage->has_mipmaps();
	const bool bAcceptedSurfaceCompression =
	        bAllowCompressedSurface && eRequiredFormat == Image::FORMAT_RGBA8 && t3dAssetsIsNativePackedSurfaceFormat( eFormat );
	if ( eRequiredFormat != Image::FORMAT_MAX && eFormat != eRequiredFormat && !bAcceptedSurfaceCompression ) {
		m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
		LOG( ERROR,
		        "Texture ID ",
		        rAsset->getId(),
		        " ",
		        rLabel,
		        " must use format ",
		        ( int )( eRequiredFormat ),
		        " or a supported native compressed equivalent, received ",
		        ( int )( eFormat ),
		        "." );
		return;
	}
	if ( rLayout.m_eFormat == Image::FORMAT_MAX ) {
		rLayout.m_vSize = vTextureSize;
		rLayout.m_eFormat = eFormat;
		rLayout.m_bMipmaps = bMipmaps;
		return;
	}
	if ( vTextureSize != rLayout.m_vSize ) {
		rLayout.m_bNeedsNormalization = true;
		rLayout.m_vSize.x = MAX( rLayout.m_vSize.x, vTextureSize.x );
		rLayout.m_vSize.y = MAX( rLayout.m_vSize.y, vTextureSize.y );
		LOG( WARN,
		        "Texture ID ",
		        rAsset->getId(),
		        " ",
		        rLabel,
		        " size ",
		        vTextureSize,
		        " differs from another layer in the same array and will be normalized to ",
		        rLayout.m_vSize,
		        "." );
	}
	if ( eFormat != rLayout.m_eFormat ) {
		rLayout.m_bNeedsNormalization = true;
		if ( bAllowCompressedSurface && t3dAssetsIsNativePackedSurfaceFormat( rLayout.m_eFormat )
		        && t3dAssetsIsNativePackedSurfaceFormat( eFormat ) ) {
			rLayout.m_eFormat = t3dAssetsSelectPackedSurfaceArrayFormat( rLayout.m_eFormat, eFormat );
		} else {
			LOG( WARN,
			        "Texture ID ",
			        rAsset->getId(),
			        " ",
			        rLabel,
			        " format ",
			        ( int )( eFormat ),
			        " differs from another layer in the same array; the array will be normalized instead of rejected." );
		}
	}
	if ( bMipmaps != rLayout.m_bMipmaps ) {
		rLayout.m_bNeedsNormalization = true;
		rLayout.m_bMipmaps = rLayout.m_bMipmaps || bMipmaps;
		LOG( WARN,
		        "Texture ID ",
		        rAsset->getId(),
		        " ",
		        rLabel,
		        " mipmap layout differs from another layer in the same array and will be normalized." );
	}
}

void Terrain3DAssets::updateTextureFiles( void ) {
	advanceTexturePublicationGeneration( false );
	m_bPersistenceDirty = true;
	IS_INIT( VOID );
	LOG( DEBUG, "Received texture_changed signal" );
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
	t3dTextureArrayLayout_t albedoLayout;
	t3dTextureArrayLayout_t secondaryLayout;
	t3dTextureArrayLayout_t displacementLayout;
	m_pTerrain->setWarning( WARN_ALL, false );

	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_null() ) {
			continue;
		}
		validateArrayMember( pAsset->m_albedoTexture, pAsset, "albedo", albedoLayout );
		if ( bPackedPbr ) {
			const Ref<Texture2D> pSurface = pAsset->getEffectiveSurfaceTexture();
			if ( pAsset->m_packedPbrResource.is_valid() && pSurface.is_null() ) {
				m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
				LOG( ERROR,
				        "Texture ID ",
				        pAsset->getId(),
				        " packed-PBR/displacement compatibility resource failed to build: ",
				        pAsset->m_packedPbrResource->getLastError() );
			}
			validateArrayMember( pSurface, pAsset, "surface", secondaryLayout, Image::FORMAT_RGBA8, true );
			if ( bUseDisplacement ) {
				const Ref<Texture2D> pDisplacement = pAsset->getEffectiveDisplacementTexture();
				validateArrayMember( pDisplacement, pAsset, "R8 displacement", displacementLayout, Image::FORMAT_R8 );
			}
		} else {
			validateArrayMember( pAsset->m_normalTexture, pAsset, "normal", secondaryLayout );
		}
	}
	if ( m_pTerrain->getWarnings() ) {
		return;
	}

	if ( bPackedPbr ) {
		if ( albedoLayout.m_vSize == s_v2iZero && secondaryLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = V2I( 1024 );
			secondaryLayout.m_vSize = V2I( 512 );
		} else if ( albedoLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = secondaryLayout.m_vSize * 2;
		} else if ( secondaryLayout.m_vSize == s_v2iZero ) {
			secondaryLayout.m_vSize = Vector2i( MAX( albedoLayout.m_vSize.x / 2, 1 ), MAX( albedoLayout.m_vSize.y / 2, 1 ) );
		}
		if ( albedoLayout.m_eFormat == Image::FORMAT_MAX ) {
			albedoLayout.m_eFormat = Image::FORMAT_RGB8;
		}
		if ( secondaryLayout.m_eFormat == Image::FORMAT_MAX ) {
			secondaryLayout.m_eFormat = Image::FORMAT_RGBA8;
		}
		if ( displacementLayout.m_vSize == s_v2iZero ) {
			displacementLayout.m_vSize = secondaryLayout.m_vSize;
		}
		displacementLayout.m_eFormat = Image::FORMAT_R8;
	} else {
		if ( secondaryLayout.m_vSize == s_v2iZero ) {
			secondaryLayout.m_vSize = albedoLayout.m_vSize;
		} else if ( albedoLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = secondaryLayout.m_vSize;
		}
		if ( albedoLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = V2I( 1024 );
			secondaryLayout.m_vSize = albedoLayout.m_vSize;
		}
	}

	// GPU texture arrays require a uniform layout only among their own layers.
	// Albedo, packed surface/PBR, normal, and displacement are separate arrays and
	// deliberately keep independent dimensions and formats. Preserve native packed
	// surface compression across size/mipmap normalization and transcode mixed
	// RGBA8/BC3/BC7 surface sources to one compatible compressed array format.
	if ( albedoLayout.m_bNeedsNormalization ) {
		albedoLayout.m_eFormat = Image::FORMAT_RGBA8;
	}
	if ( secondaryLayout.m_bNeedsNormalization && ( !bPackedPbr || !t3dAssetsIsNativePackedSurfaceFormat( secondaryLayout.m_eFormat ) ) ) {
		secondaryLayout.m_eFormat = Image::FORMAT_RGBA8;
	}
	if ( displacementLayout.m_bNeedsNormalization ) {
		displacementLayout.m_eFormat = Image::FORMAT_R8;
	}

	Terrain3DGeneratedTexture replacement_albedo;
	Terrain3DGeneratedTexture replacement_normal;
	Terrain3DGeneratedTexture replacement_surface;
	Terrain3DGeneratedTexture replacement_displacement;
	bool bReplacementOk = true;

	if ( albedoLayout.m_vSize != s_v2iZero ) {
		TypedArray<Image> layers;
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->m_albedoTexture : Ref<Texture2D>();
			const Ref<Image> pImage = t3dAssetsPrepareArrayImage(
			        pTexture, albedoLayout.m_vSize, albedoLayout.m_eFormat, albedoLayout.m_bMipmaps, COLOR_CHECKED );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( ERROR, "Failed to normalize albedo texture layer ", layers.size(), "." );
				break;
			}
			layers.push_back( pImage );
		}
		if ( bReplacementOk && !layers.is_empty() ) {
			bReplacementOk = replacement_albedo.create( layers, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
		}
	}

	if ( bReplacementOk && bPackedPbr && secondaryLayout.m_vSize != s_v2iZero ) {
		TypedArray<Image> surface_layers;
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getEffectiveSurfaceTexture() : Ref<Texture2D>();
			const Ref<Image> pImage = t3dAssetsPrepareArrayImage( pTexture,
			        secondaryLayout.m_vSize,
			        secondaryLayout.m_eFormat,
			        secondaryLayout.m_bMipmaps,
			        Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( ERROR, "Failed to normalize packed surface texture layer ", surface_layers.size(), "." );
				break;
			}
			surface_layers.push_back( pImage );
		}
		if ( bReplacementOk && !surface_layers.is_empty() ) {
			bReplacementOk = replacement_surface.create( surface_layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
		}

		if ( bReplacementOk && bUseDisplacement ) {
			TypedArray<Image> displacement_layers;
			for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
				const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
				const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getEffectiveDisplacementTexture() : Ref<Texture2D>();
				const Ref<Image> pImage = t3dAssetsPrepareArrayImage( pTexture,
				        displacementLayout.m_vSize,
				        displacementLayout.m_eFormat,
				        displacementLayout.m_bMipmaps,
				        Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
				if ( pImage.is_null() ) {
					bReplacementOk = false;
					LOG( ERROR, "Failed to normalize displacement texture layer ", displacement_layers.size(), "." );
					break;
				}
				displacement_layers.push_back( pImage );
			}
			if ( bReplacementOk && !displacement_layers.is_empty() ) {
				bReplacementOk = replacement_displacement.create( displacement_layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
			}
		}
	} else if ( bReplacementOk && !bPackedPbr && secondaryLayout.m_vSize != s_v2iZero ) {
		TypedArray<Image> layers;
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->m_normalTexture : Ref<Texture2D>();
			const Ref<Image> pImage = t3dAssetsPrepareArrayImage(
			        pTexture, secondaryLayout.m_vSize, secondaryLayout.m_eFormat, secondaryLayout.m_bMipmaps, COLOR_NORMAL );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( ERROR, "Failed to normalize normal texture layer ", layers.size(), "." );
				break;
			}
			layers.push_back( pImage );
		}
		if ( bReplacementOk && !layers.is_empty() ) {
			bReplacementOk = replacement_normal.create( layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
		}
	}

	if ( !bReplacementOk ) {
		t3dAssetsReleaseGeneratedReplacements( replacement_albedo, replacement_normal, replacement_surface, replacement_displacement );
		LOG( ERROR, "Terrain texture-array rebuild failed. Keeping the last complete generated texture set." );
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
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_null() ) {
			continue;
		}
		if ( pAsset->m_albedoTexture.is_null() ) {
			const Ref<Image> pImage =
			        t3dAssetsMakeArrayDefault( albedoLayout.m_vSize, albedoLayout.m_eFormat, albedoLayout.m_bMipmaps, COLOR_CHECKED );
			if ( pImage.is_valid() ) {
				pAsset->m_albedoTexture = ImageTexture::create_from_image( pImage );
			}
		}
		if ( bPackedPbr ) {
			if ( pAsset->m_packedPbrResource.is_null() && pAsset->m_surfaceTexture.is_null() ) {
				const Ref<Image> pImage = t3dAssetsMakeArrayDefault(
				        secondaryLayout.m_vSize, secondaryLayout.m_eFormat, secondaryLayout.m_bMipmaps, Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
				if ( pImage.is_valid() ) {
					pAsset->m_surfaceTexture = ImageTexture::create_from_image( pImage );
				}
			}
			if ( bUseDisplacement && pAsset->m_packedPbrResource.is_null() && pAsset->m_displacementTexture.is_null() ) {
				const Ref<Image> pImage = t3dAssetsMakeArrayDefault( displacementLayout.m_vSize,
				        displacementLayout.m_eFormat,
				        displacementLayout.m_bMipmaps,
				        Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
				if ( pImage.is_valid() ) {
					pAsset->m_displacementTexture = ImageTexture::create_from_image( pImage );
				}
			}
		} else if ( pAsset->m_normalTexture.is_null() ) {
			const Ref<Image> pImage = t3dAssetsMakeArrayDefault(
			        secondaryLayout.m_vSize, secondaryLayout.m_eFormat, secondaryLayout.m_bMipmaps, COLOR_NORMAL );
			if ( pImage.is_valid() ) {
				pAsset->m_normalTexture = ImageTexture::create_from_image( pImage );
			}
		}
	}

	t3dAssetsReleaseGeneratedReplacements( replacement_albedo, replacement_normal, replacement_surface, replacement_displacement );
	resetRuntimeMaterialCacheState( false );

	if ( !m_bSuppressTextureChanged ) {
		emit_signal( "textures_changed" );
	}
}

void Terrain3DAssets::updateTextureSettingsInternal( const bool bPersistentChange ) {
	advanceTexturePublicationGeneration( false );
	if ( bPersistentChange ) {
		m_bPersistenceDirty = true;
	}
	if ( m_pTerrain && !m_pTerrain->isEditorAuthoringMode() && m_bRuntimeMaterialCookedValid ) {
		publishRuntimeMaterialCookedSettings();
		if ( !m_bSuppressTextureChanged ) {
			emit_signal( "textures_changed" );
		}
		return;
	}
	LOG( DEBUG, "Received setting_changed signal" );
	m_textureSettings.m_nCount = MIN( m_textureList.size(), s_nMaxTextureAssets );
	for ( int index = 0; index < m_textureSettings.m_nCount; ++index ) {
		const Ref<Terrain3DTextureAsset> pTa = m_textureList[index];
		if ( pTa.is_null() ) {
			m_textureSettings.m_szColors[( size_t )( index )] = s_colorWhite;
			m_textureSettings.m_szNormalDepths[( size_t )( index )] = 1.0f;
			m_textureSettings.m_szAoStrengths[( size_t )( index )] = 0.5f;
			m_textureSettings.m_szAoLightAffects[( size_t )( index )] = 0.0f;
			m_textureSettings.m_szRoughnessMods[( size_t )( index )] = 0.0f;
			m_textureSettings.m_szMetallics[( size_t )( index )] = 0.0f;
			m_textureSettings.m_szUvScales[( size_t )( index )] = 0.1f;
			m_textureSettings.m_szDetiles[( size_t )( index )] = Vector2();
			m_textureSettings.m_szDisplacements[( size_t )( index )] = Vector2();
			continue;
		}

		m_textureSettings.m_szColors[( size_t )( index )] = pTa->isHighlighted() ? pTa->getHighlightColor() : pTa->getAlbedoColor();
		m_textureSettings.m_szNormalDepths[( size_t )( index )] = pTa->getNormalDepth();
		m_textureSettings.m_szAoStrengths[( size_t )( index )] = pTa->getAoStrength();
		m_textureSettings.m_szAoLightAffects[( size_t )( index )] = pTa->getAoLightAffect();
		m_textureSettings.m_szRoughnessMods[( size_t )( index )] = pTa->getRoughness();
		m_textureSettings.m_szMetallics[( size_t )( index )] = pTa->getMetallic();
		m_textureSettings.m_szUvScales[( size_t )( index )] = pTa->getUvScale();
		m_textureSettings.m_szDetiles[( size_t )( index )] = Vector2( pTa->getDetilingRotation(), pTa->getDetilingShift() );
		m_textureSettings.m_szDisplacements[( size_t )( index )] = Vector2( pTa->getDisplacementOffset(), pTa->getDisplacementScale() );
	}

	const int count = m_textureSettings.m_nCount;
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
		memcpy( m_textureColors.ptrw(), m_textureSettings.m_szColors, sizeof( m_textureSettings.m_szColors[0] ) * ( size_t )( count ) );
		memcpy( m_textureNormalDepths.ptrw(),
		        m_textureSettings.m_szNormalDepths,
		        sizeof( m_textureSettings.m_szNormalDepths[0] ) * ( size_t )( count ) );
		memcpy( m_textureAoStrengths.ptrw(),
		        m_textureSettings.m_szAoStrengths,
		        sizeof( m_textureSettings.m_szAoStrengths[0] ) * ( size_t )( count ) );
		memcpy( m_textureAoLightAffects.ptrw(),
		        m_textureSettings.m_szAoLightAffects,
		        sizeof( m_textureSettings.m_szAoLightAffects[0] ) * ( size_t )( count ) );
		memcpy( m_textureRoughnessMods.ptrw(),
		        m_textureSettings.m_szRoughnessMods,
		        sizeof( m_textureSettings.m_szRoughnessMods[0] ) * ( size_t )( count ) );
		memcpy( m_textureMetallics.ptrw(),
		        m_textureSettings.m_szMetallics,
		        sizeof( m_textureSettings.m_szMetallics[0] ) * ( size_t )( count ) );
		memcpy( m_textureUvScales.ptrw(),
		        m_textureSettings.m_szUvScales,
		        sizeof( m_textureSettings.m_szUvScales[0] ) * ( size_t )( count ) );
		memcpy( m_textureDetiles.ptrw(), m_textureSettings.m_szDetiles, sizeof( m_textureSettings.m_szDetiles[0] ) * ( size_t )( count ) );
		memcpy( m_textureDisplacements.ptrw(),
		        m_textureSettings.m_szDisplacements,
		        sizeof( m_textureSettings.m_szDisplacements[0] ) * ( size_t )( count ) );
	}

	LOG( DEBUG, "Emitting textures_changed" );
	if ( !m_bSuppressTextureChanged ) {
		emit_signal( "textures_changed" );
	}
}

void Terrain3DAssets::updateTextureSettings( void ) {
	updateTextureSettingsInternal( true );
}

void Terrain3DAssets::updateTexturePreview( void ) {
	updateTextureSettingsInternal( false );
}

void Terrain3DAssets::updateMeshInternal( const int nId, const bool bPersistentChange ) {
	advanceMeshSourceGeneration();
	if ( bPersistentChange ) {
		m_bPersistenceDirty = true;
	}
	if ( nId < 0 || nId >= m_meshList.size() ) {
		LOG( ERROR, "Invalid mesh ID: ", nId );
		return;
	}
	IS_INSTANCER_INIT( VOID );
	Terrain3DInstancer *const pInstancer = m_pTerrain->getInstancer();
	pInstancer->updateMmis( nId, s_v2iMax, false );
}

void Terrain3DAssets::updateMesh( const int nId ) {
	updateMeshInternal( nId, true );
}

void Terrain3DAssets::updateMeshPreview( const int nId ) {
	updateMeshInternal( nId, false );
}

// -----------------------------------------------------------------------------
// Public functions

void Terrain3DAssets::setTexturePackingMode( const t3dTexturePackingMode_t eMode ) {
	const t3dTexturePackingMode_t mode = ( t3dTexturePackingMode_t )( CLAMP(
	        ( int )( eMode ), ( int )( TEXTURE_PACKING_LEGACY ), ( int )( TEXTURE_PACKING_PACKED_PBR_R8 ) ) );
	if ( m_texturePackingMode == mode ) {
		return;
	}
	m_texturePackingMode = mode;
	m_bPersistenceDirty = true;
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_valid() ) {
			pAsset->setPackedPbrMode( usesPackedPbr() );
		}
	}
	m_generatedAlbedoTextures.clear();
	m_generatedNormalTextures.clear();
	m_generatedSurfaceTextures.clear();
	m_generatedDisplacementTextures.clear();
	LOG( INFO, "Setting texture packing mode: ", ( int )( m_texturePackingMode ) );
	if ( m_pTerrain ) {
		updateTextureList();
		Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitialized() ) {
			pMaterial->update( FULL_REBUILD );
		}
	}
	emit_changed();
}

void Terrain3DAssets::initialize( Terrain3D *pTerrain ) {
	if ( pTerrain ) {
		Terrain3D *previous_owner = t3dTransferOwner( m_pTerrain, pTerrain );
		if ( previous_owner && previous_owner != pTerrain ) {
			LOG( INFO, "Transferring asset runtime ownership from ", previous_owner, " to ", pTerrain );
		}
	} else {
		LOG( ERROR, "Initialization failed, p_terrain is null" );
		return;
	}
	LOG( INFO, "Initializing assets" );

	// Runtime/Profile mode can load the cooked physical material working set
	// directly. Building the full logical arrays first would create a large
	// transient duplicate and defeat the purpose of runtime material streaming.
	bool runtime_materials_ready = false;
	const bool strict_baked_runtime = m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode();
	if ( m_pTerrain->isRuntimeMaterialStreamingEnabled() && !m_pTerrain->isEditorAuthoringMode() ) {
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			if ( pAsset.is_valid() ) {
				pAsset->setPackedPbrMode( usesPackedPbr() );
				connectAssetSignals( TYPE_TEXTURE, pAsset );
			}
		}
		if ( !strict_baked_runtime ) {
			updateTextureSettings();
		}
		runtime_materials_ready = configureRuntimeMaterialCache();
	}
	if ( !runtime_materials_ready ) {
		if ( strict_baked_runtime && m_pTerrain->isRuntimeMaterialStreamingEnabled() ) {
			LOG( ERROR, "Strict baked runtime material initialization failed; source texture fallback is disabled." );
		} else {
			updateTextureList();
		}
	}
	updateMeshList();
	m_bPersistenceDirty = false;
}

void Terrain3DAssets::uninitialize( Terrain3D *pTerrain ) {
	if ( !t3dReleaseOwner( m_pTerrain, pTerrain ) ) {
		LOG( DEBUG, "Ignoring stale asset uninitialize request from ", pTerrain, "; current owner: ", m_pTerrain );
		return;
	}
	stopRuntimeMaterialWorker();
	LOG( INFO, "Uninitializing assets" );
}

void Terrain3DAssets::destroy( void ) {
	stopRuntimeMaterialWorker();
	LOG( INFO, "Destroying assets" );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		disconnectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
		const Ref<Terrain3DMeshAsset> pAsset = ( m_meshList )[iMesh];
		disconnectAssetSignals( TYPE_MESH, pAsset );
	}
	m_pTerrain = NULL;
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
}

// Called when creating a new asset
void Terrain3DAssets::setTextureAsset( const int nId, const Ref<Terrain3DTextureAsset> &rTexture ) {
	if ( nId < 0 || nId >= s_nMaxTextureAssets ) {
		LOG( ERROR, "Invalid texture id: ", nId, " range is 0-", s_nMaxTextureAssets - 1 );
		return;
	}
	if ( nId < m_textureList.size() && m_textureList[nId] == rTexture ) {
		return;
	}
	LOG( INFO, "Setting texture id: ", nId );
	setAsset( TYPE_TEXTURE, nId, rTexture );
	updateTextureList();
}

// Called when loading a list of assets from an assets resource file on disk
TypedArray<Terrain3DTextureAsset> Terrain3DAssets::getTextureList( void ) const {
	TypedArray<Terrain3DTextureAsset> result;
	result.resize( m_textureList.size() );
	for ( int i = 0; i < m_textureList.size(); ++i ) {
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[i];
		if ( pAsset.is_valid() && !pAsset->m_bPlaceholder ) {
			result[i] = pAsset;
		}
	}
	return result;
}

void Terrain3DAssets::setTextureList( const TypedArray<Terrain3DTextureAsset> &rTextureList ) {
	LOG( INFO, "Setting texture list with ", rTextureList.size(), " entries" );
	if ( !t3dDiffers( getTextureList(), rTextureList ) ) {
		return;
	}
	setAssetList( TYPE_TEXTURE, rTextureList );
	updateTextureList();
}

void Terrain3DAssets::clearTextures( const bool bUpdate ) {
	m_bPersistenceDirty = true;
	LOG( INFO, "Clearing texture list" );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		disconnectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	m_textureList.clear();
	if ( bUpdate ) {
		updateTextureList();
	}
}

void Terrain3DAssets::updateTextureList( void ) {
	LOG( INFO, "Refreshing texture registry" );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
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
void Terrain3DAssets::setMeshAsset( const int nId, const Ref<Terrain3DMeshAsset> &rMeshAsset ) {
	if ( nId < 0 || nId >= s_nMaxMeshAssets ) {
		LOG( ERROR, "Invalid mesh id: ", nId, " range is 0-", s_nMaxMeshAssets - 1 );
		return;
	}
	if ( nId < m_meshList.size() && m_meshList[nId] == rMeshAsset ) {
		return;
	}
	LOG( INFO, "Setting mesh id: ", nId, ", ", rMeshAsset );
	if ( rMeshAsset.is_null() ) {
		IS_INSTANCER_INIT( VOID );
		m_pTerrain->getInstancer()->clearByMesh( nId );
	}
	setAsset( TYPE_MESH, nId, rMeshAsset );
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( nId, s_v2iMax, false );
	}
}

// Called when loading a list of assets from an assets resource file on disk
TypedArray<Terrain3DMeshAsset> Terrain3DAssets::getMeshList( void ) const {
	TypedArray<Terrain3DMeshAsset> result;
	result.resize( m_meshList.size() );
	for ( int i = 0; i < m_meshList.size(); ++i ) {
		const Ref<Terrain3DMeshAsset> pAsset = m_meshList[i];
		if ( pAsset.is_valid() && !pAsset->m_bPlaceholder ) {
			result[i] = pAsset;
		}
	}
	return result;
}

void Terrain3DAssets::setMeshList( const TypedArray<Terrain3DMeshAsset> &rMeshList ) {
	LOG( INFO, "Setting mesh list with ", rMeshList.size(), " entries" );
	if ( !t3dDiffers( getMeshList(), rMeshList ) ) {
		return;
	}
	setAssetList( TYPE_MESH, rMeshList );
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, s_v2iMax, true );
	}
}

// p_id = -1 for all meshes
// Adapted from godot\editor\plugins\editor_preview_plugins.cpp:EditorMeshPreviewPlugin
void Terrain3DAssets::createMeshThumbnails( const int nId, const Vector2i &rSize, const bool bForce ) {
	if ( !IS_EDITOR || !m_pTerrain ) {
		return;
	}
	if ( !m_pTerrain->isAuthoringResidencyReadbackSafe() ) {
		LOG( DEBUG, "Deferring mesh thumbnail GPU readback until Authoring residency is idle" );
		return;
	}
	LOG( INFO, "Creating RD mesh thumbnails for ID: ", nId, ", size: ", rSize );
	const int max = getMeshCount();
	if ( nId < -1 || nId >= max || max <= 0 ) {
		return;
	}
	const int start_id = nId < 0 ? 0 : CLAMP( nId, 0, max - 1 );
	const int end_id = nId < 0 ? max : CLAMP( nId + 1, 0, max );
	const Vector2i size = CLAMP( rSize, V2I( 2 ), V2I( 4096 ) );
	for ( int i = start_id; i < end_id; ++i ) {
		const Ref<Terrain3DMeshAsset> asset = getMeshAsset( i );
		if ( asset.is_null() ) {
			LOG( ERROR, i, ": Terrain3DMeshAsset is null, skipping" );
			continue;
		}
		if ( !bForce && asset->getThumbnail().is_valid() ) {
			continue;
		}
		if ( asset->getMesh( 0 ).is_null() ) {
			LOG( ERROR, i, ": Mesh LOD0 is null, skipping thumbnail" );
			continue;
		}
		const Ref<Image> image = m_pTerrain->createRdMeshPreview( ( uint16_t )( i ), size );
		if ( image.is_null() ) {
			LOG( ERROR, i, ": RenderingDevice mesh preview failed" );
			continue;
		}
		asset->setThumbnail( ImageTexture::create_from_image( image ) );
	}
}

void Terrain3DAssets::updateMeshList( void ) {
	m_bPersistenceDirty = true;
	advanceMeshSourceGeneration();
	IS_INSTANCER_INIT( VOID );
	LOG( INFO, "Updating mesh registry" );
	if ( m_meshList.is_empty() ) {
		LOG( DEBUG, "Mesh list empty, clearing instancer and adding a default mesh" );
		m_pTerrain->getInstancer()->destroy();
		Ref<Terrain3DMeshAsset> pNewAsset;
		pNewAsset.instantiate();
		setMeshAsset( 0, pNewAsset );
		return;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
		const Ref<Terrain3DMeshAsset> pAsset = ( m_meshList )[iMesh];
		connectAssetSignals( TYPE_MESH, pAsset );
	}
	emit_signal( "meshes_changed" );
}

void Terrain3DAssets::loadPendingMeshes( void ) {
	// Reload meshes for all mesh assets that have changed. Used by the instancer after clearing
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
		const Ref<Terrain3DMeshAsset> pMa = ( m_meshList )[iMesh];
		if ( pMa.is_valid() && pMa->isSceneFilePending() ) {
			pMa->commitMeshes();
		}
	}
}

Error Terrain3DAssets::save( const String &rPath ) {
	if ( rPath.is_empty() && get_path().is_empty() ) {
		return ERR_FILE_NOT_FOUND;
	}
	if ( !rPath.is_empty() ) {
		LOG( DEBUG, "Setting file path to ", rPath );
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
		LOG( DEBUG, String( "Attempting to save external file: " ) + path );
		err = ResourceSaver::get_singleton()->save( this, path, ResourceSaver::FLAG_COMPRESS );
		if ( err == OK ) {
			m_bPersistenceDirty = false;
			LOG( INFO, "File saved successfully: ", path );
			if ( IS_EDITOR && path.begins_with( "res://" ) ) {
				EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
				if ( pFilesystem && !pFilesystem->is_scanning() ) {
					pFilesystem->update_file( path );
				}
			}
		} else {
			LOG( ERROR, "Cannot save file: ", path, ". Error code: ", ERROR, ". Look up @GlobalScope Error enum in the Godot docs" );
		}
	}
	return err;
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DAssets::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( TYPE_TEXTURE );
	BIND_ENUM_CONSTANT( TYPE_MESH );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_LEGACY );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_PACKED_PBR_R8 );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_SURFACE_R16 );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_PBR_RGBA16UI );
	BIND_CONSTANT( MAX_TEXTURE_ASSETS );
	BIND_CONSTANT( MAX_MESH_ASSETS );

	ClassDB::bind_method( D_METHOD( "set_texture_asset", "id", "texture" ), &Terrain3DAssets::setTextureAsset );
	ClassDB::bind_method( D_METHOD( "get_texture_asset", "id" ), &Terrain3DAssets::getTextureAsset );
	ClassDB::bind_method( D_METHOD( "set_texture_list", "texture_list" ), &Terrain3DAssets::setTextureList );
	ClassDB::bind_method( D_METHOD( "get_texture_list" ), &Terrain3DAssets::getTextureList );
	ClassDB::bind_method( D_METHOD( "get_texture_count" ), &Terrain3DAssets::getTextureCount );
	ClassDB::bind_method( D_METHOD( "get_albedo_array_rid" ), &Terrain3DAssets::getAlbedoArrayRid );
	ClassDB::bind_method( D_METHOD( "get_normal_array_rid" ), &Terrain3DAssets::getNormalArrayRid );
	ClassDB::bind_method( D_METHOD( "get_surface_array_rid" ), &Terrain3DAssets::getSurfaceArrayRid );
	ClassDB::bind_method( D_METHOD( "get_displacement_array_rid" ), &Terrain3DAssets::getDisplacementArrayRid );
	ClassDB::bind_method( D_METHOD( "get_pbr_array_rid" ), &Terrain3DAssets::getPbrArrayRid );
	ClassDB::bind_method( D_METHOD( "set_texture_packing_mode", "mode" ), &Terrain3DAssets::setTexturePackingMode );
	ClassDB::bind_method( D_METHOD( "get_texture_packing_mode" ), &Terrain3DAssets::getTexturePackingMode );
	ClassDB::bind_method( D_METHOD( "get_packed_pbr_mipmap_count" ), &Terrain3DAssets::getPackedPbrMipmapCount );
	ClassDB::bind_method( D_METHOD( "get_texture_colors" ), &Terrain3DAssets::getTextureColors );
	ClassDB::bind_method( D_METHOD( "get_texture_normal_depths" ), &Terrain3DAssets::getTextureNormalDepths );
	ClassDB::bind_method( D_METHOD( "get_texture_ao_strengths" ), &Terrain3DAssets::getTextureAoStrengths );
	ClassDB::bind_method( D_METHOD( "get_texture_ao_light_affects" ), &Terrain3DAssets::getTextureAoLightAffects );
	ClassDB::bind_method( D_METHOD( "get_texture_roughness_mods" ), &Terrain3DAssets::getTextureRoughnessMods );
	ClassDB::bind_method( D_METHOD( "get_texture_metallics" ), &Terrain3DAssets::getTextureMetallics );
	ClassDB::bind_method( D_METHOD( "get_texture_uv_scales" ), &Terrain3DAssets::getTextureUvScales );
	ClassDB::bind_method( D_METHOD( "get_texture_detiles" ), &Terrain3DAssets::getTextureDetiles );
	ClassDB::bind_method( D_METHOD( "get_texture_displacements" ), &Terrain3DAssets::getTextureDisplacements );
	ClassDB::bind_method( D_METHOD( "clear_textures", "update" ), &Terrain3DAssets::clearTextures, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "update_texture_list" ), &Terrain3DAssets::updateTextureList );

	ClassDB::bind_method( D_METHOD( "set_mesh_asset", "id", "mesh" ), &Terrain3DAssets::setMeshAsset );
	ClassDB::bind_method( D_METHOD( "get_mesh_asset", "id" ), &Terrain3DAssets::getMeshAsset );
	ClassDB::bind_method( D_METHOD( "set_mesh_list", "mesh_list" ), &Terrain3DAssets::setMeshList );
	ClassDB::bind_method( D_METHOD( "get_mesh_list" ), &Terrain3DAssets::getMeshList );
	ClassDB::bind_method( D_METHOD( "get_mesh_count" ), &Terrain3DAssets::getMeshCount );
	ClassDB::bind_method( D_METHOD( "create_mesh_thumbnails", "id", "size", "force" ),
	        &Terrain3DAssets::createMeshThumbnails,
	        DEFVAL( -1 ),
	        DEFVAL( V2I( 256 ) ),
	        DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "update_mesh_list" ), &Terrain3DAssets::updateMeshList );

	ClassDB::bind_method( D_METHOD( "save", "path" ), &Terrain3DAssets::save, DEFVAL( "" ) );

	const int ro_flags = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Packing / Runtime Format", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
	                      "texture_packing_mode",
	                      PROPERTY_HINT_ENUM,
	                      "Legacy AlbedoHeight + NormalRoughness,Packed PBR RGBA8 + Displacement R8" ),
	        "set_texture_packing_mode",
	        "get_texture_packing_mode" );

	ADD_GROUP( "Texture Asset Registry", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "texture_list", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DTextureAsset", ro_flags ),
	        "set_texture_list",
	        "get_texture_list" );

	ADD_GROUP( "Mesh Asset Registry", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "mesh_list", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DMeshAsset", ro_flags ),
	        "set_mesh_list",
	        "get_mesh_list" );

	ADD_SIGNAL( MethodInfo( "meshes_changed" ) );
	ADD_SIGNAL( MethodInfo( "textures_changed" ) );
}
