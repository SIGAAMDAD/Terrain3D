#include "core/utility/terrain_3d_binary_io.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_hash_bytes.h"
#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "rendering/surface/terrain_3d_far_surface_atlas_cache.h"
#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include "streaming/residency/terrain_3d_raster_admission.h"

#include "assets/catalog/terrain_3d_assets.h"
#include "data/model/terrain_3d_data.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/runtime/terrain_3d_runtime_page_format.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "eastl_config.h"
#include "rendering/material/terrain_3d_material.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"

#include <string.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>

// t3dFarSurfaceBakeData_s centralized in Terrain3DTypes.h.

static bool t3dFarSurfaceAtlasCacheFnv1a64AppendFile(
	uint64_t	 &nHash,
	const String &rPath ) {
	const Ref< FileAccess > pFile = FileAccess::open( rPath, FileAccess::READ );
	if ( pFile.is_null() ) {
		return false;
	}
	const uint64_t file_bytes = pFile->get_length();
	t3dHashFnv1a64AppendNativeU64( nHash, file_bytes );
	uint8_t	 bytes[65536u];
	uint64_t remaining = file_bytes;
	while ( remaining > 0u ) {
		const uint64_t request = MIN( remaining, ( uint64_t ) ( sizeof( bytes ) ) );
		const uint64_t read = pFile->get_buffer( bytes, request );
		if ( read != request ) {
			return false;
		}
		t3dHashFnv1a64Append( nHash, bytes, ( size_t ) ( read ) );
		remaining -= read;
	}
	return true;
}

static void t3dFarSurfaceAtlasCacheFnv1a64AppendImage(
	uint64_t		   &nHash,
	const Ref< Image > &rImage ) {
	if ( rImage.is_null() || rImage->is_empty() ) {
		t3dHashFnv1a64AppendNativeU32( nHash, 0u );
		return;
	}
	t3dHashFnv1a64AppendNativeU32( nHash, 1u );
	t3dHashFnv1a64AppendNativeU32( nHash, ( uint32_t ) ( rImage->get_width() ) );
	t3dHashFnv1a64AppendNativeU32( nHash, ( uint32_t ) ( rImage->get_height() ) );
	t3dHashFnv1a64AppendNativeU32( nHash, ( uint32_t ) ( rImage->get_format() ) );
	const PackedByteArray bytes = rImage->get_data();
	t3dHashFnv1a64AppendNativeU64( nHash, ( uint64_t ) ( bytes.size() ) );
	if ( !bytes.is_empty() ) {
		t3dHashFnv1a64Append( nHash, bytes.ptr(), ( size_t ) ( bytes.size() ) );
	}
}

static uint32_t t3dFarSurfaceAtlasCacheReadU32Unaligned(
	const uint8_t *pBytes,
	const size_t   nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		value = t3dReadNativeUnaligned< decltype( value ) >( pBytes + nOffset );
	}
	return value;
}

static uint16_t t3dFarSurfaceAtlasCacheReadU16Unaligned(
	const uint8_t *pBytes,
	const size_t   nOffset ) {
	uint16_t value = 0u;
	if ( pBytes ) {
		value = t3dReadNativeUnaligned< decltype( value ) >( pBytes + nOffset );
	}
	return value;
}

EA_FORCE_INLINE static uint8_t t3dFarSurfaceAtlasCacheColorToByte(
	const float fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

static void t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty(
	PackedByteArray &rBytes ) {
	uint8_t *pWrite = rBytes.ptrw();
	if ( !pWrite ) {
		return;
	}
	const size_t pixel_count = ( size_t ) ( rBytes.size() ) / 4u;
	for ( size_t index = 0u; index < pixel_count; ++index ) {
		const size_t offset = index * 4u;
		pWrite[offset] = 255u;
		pWrite[offset + 1u] = 255u;
		pWrite[offset + 2u] = 255u;
		pWrite[offset + 3u] = 0u;
	}
}

static uint8_t t3dFarSurfaceAtlasCacheFarSurfaceAlpha(
	const float fSolidCoverage,
	const float fColorAlpha,
	const int	nMaterialId ) {
	if ( fSolidCoverage < 0.5f ) {
		return 0u;
	}
	const uint32_t material = ( uint32_t ) ( CLAMP( nMaterialId, 0, 31 ) );
	const uint32_t surface_alpha =
		( uint32_t ) ( CLAMP( Math::round( CLAMP( fColorAlpha, 0.0f, 1.0f ) * 3.0f ), 0.0f, 3.0f ) );
	return ( uint8_t ) ( 0x80u | ( material << 2u ) | surface_alpha );
}

static float t3dFarSurfaceAtlasCacheMaterialFloatParam(
	Terrain3DMaterial *pMaterial,
	const char		  *pszName,
	const float		   fDefault ) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? fDefault : ( float ) ( value );
}

static int t3dFarSurfaceAtlasCacheMaterialIntParam(
	Terrain3DMaterial *pMaterial,
	const char		  *pszName,
	const int		   nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int ) ( value );
}

static void t3dFarSurfaceAtlasCacheBuildBakeData(
	const Ref< Terrain3DAssets > &rAssets,
	t3dFarSurfaceBakeData_t		 &rData ) {
	rData = t3dFarSurfaceBakeData_t();
	if ( rAssets.is_null() ) {
		return;
	}
	rData.m_nCount = MIN( rAssets->getTextureCount(), T3D_MAX_TEXTURE_ASSETS );
	bool found_fallback = false;
	for ( int index = 0; index < rData.m_nCount; ++index ) {
		const bool populated = rAssets->getTextureAsset( index ).is_valid();
		rData.m_populated[( size_t ) ( index )] = populated ? 1u : 0u;
		if ( populated && !found_fallback ) {
			rData.m_nFallbackTexture = index;
			found_fallback = true;
		}
	}
}

static int t3dFarSurfaceAtlasCacheResolveMaterialId(
	const t3dFarSurfaceBakeData_t &rData,
	const int					   nTextureId ) {
	if ( nTextureId >= 0 && nTextureId < rData.m_nCount && rData.m_populated[( size_t ) ( nTextureId )] != 0u ) {
		return nTextureId;
	}
	return rData.m_nFallbackTexture;
}

Terrain3DFarSurfaceAtlasCache::Terrain3DFarSurfaceAtlasCache(
	Terrain3DRegionRepository *pRepository,
	Terrain3DSamplingStorage  *pSamplingStorage,
	Terrain3DCpuRegionCache	  *pCpuRegionCache ) :
	m_pRepository( pRepository ),
	m_pSamplingStorage( pSamplingStorage ),
	m_pCpuRegionCache( pCpuRegionCache ) {
	m_generatedHeightMap.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	m_generatedColorMap.setSourceRetentionPolicy( T3D_DISCARD_CPU_SOURCE_AFTER_UPLOAD );
	reset();
}

void Terrain3DFarSurfaceAtlasCache::setConfig(
	const t3dFarSurfaceAtlasConfig_t &rConfig ) {
	const bool layout_changed = rConfig.m_nHeightAtlasSide != m_config.m_nHeightAtlasSide ||
								rConfig.m_nSurfaceAtlasSide != m_config.m_nSurfaceAtlasSide ||
								rConfig.m_nFinePageLimit != m_config.m_nFinePageLimit;
	m_config = rConfig;
	if ( layout_changed ) {
		invalidateLayout();
	}
}

void Terrain3DFarSurfaceAtlasCache::reset(
	void ) {
	m_generatedHeightMap.clear();
	m_generatedColorMap.clear();
	m_heightImage.unref();
	m_colorImage.unref();
	m_heightDirtyRegions.reset();
	m_colorDirtyRegions.reset();
	m_bHeightDirty = true;
	m_bColorDirty = true;
	m_bColorCookDirty = true;
	memset( m_szPackedColorSlots, 0, sizeof( m_szPackedColorSlots ) );
	memset( m_szDesiredColorSlots, 0, sizeof( m_szDesiredColorSlots ) );
	memset( m_szFineSlotOwners, 0, sizeof( m_szFineSlotOwners ) );
	memset( m_szFineSlotRetireTick, 0, sizeof( m_szFineSlotRetireTick ) );
	memset( m_szDemandPriority, 0, sizeof( m_szDemandPriority ) );
	m_nDemandTick = 0u;
	m_nLastDemandServiceTick = 0u;
	m_nFineAdmissions = 0u;
	m_nFineEvictions = 0u;
	m_bFrameDemandAvailable = false;
	m_nPackedColorCount = 0u;
	m_nPackedColorAtlasSide = 0;
	m_nPackedColorAtlasHeight = 0;
	m_nPackedTopologyGeneration = 0u;
	++m_nPackedColorLayoutGeneration;
	m_stats.m_bHeightDirtyAll = true;
	m_stats.m_bColorDirtyAll = true;
	m_stats.m_bCookedLoaded = false;
	m_stats.m_bCookedSaved = false;
	m_stats.m_nHeightUpdatesLastTick = 0;
	m_stats.m_nColorUpdatesLastTick = 0;
	m_stats.m_nHeightRegionsLastTick = 0;
	m_stats.m_nColorRegionsLastTick = 0;
	m_stats.m_nHeightProcessingUsecLast = 0u;
	m_stats.m_nColorProcessingUsecLast = 0u;
	m_stats.m_nHeightDirtyTexelsLast = 0u;
	m_stats.m_nColorDirtyTexelsLast = 0u;
	m_stats.m_nHeightRebuiltBytesLast = 0u;
	m_stats.m_nColorRebuiltBytesLast = 0u;
	m_stats.m_nHeightUploadedBytesLast = 0u;
	m_stats.m_nColorUploadedBytesLast = 0u;
	m_stats.m_nHeightPublicationUsecLast = 0u;
	m_stats.m_nColorPublicationUsecLast = 0u;
	m_stats.m_nHeightPublicationCount = 0u;
	m_stats.m_nColorPublicationCount = 0u;
	m_stats.m_nHeightUploadedBytesTotal = 0u;
	m_stats.m_nColorUploadedBytesTotal = 0u;
	m_stats.m_nCookIoUsecLast = 0u;
}

void Terrain3DFarSurfaceAtlasCache::resetFrameStats(
	void ) {
	m_stats.m_nHeightUpdatesLastTick = 0;
	m_stats.m_nColorUpdatesLastTick = 0;
	m_stats.m_nHeightRegionsLastTick = 0;
	m_stats.m_nColorRegionsLastTick = 0;
}

void Terrain3DFarSurfaceAtlasCache::invalidateSurface(
	void ) {
	m_generatedColorMap.clear();
	m_colorImage.unref();
	m_colorDirtyRegions.reset();
	m_bColorDirty = true;
	m_bColorCookDirty = true;
	m_stats.m_bColorDirtyAll = true;
	m_stats.m_bCookedLoaded = false;
	m_stats.m_bCookedSaved = false;
}

void Terrain3DFarSurfaceAtlasCache::invalidateLayout(
	void ) {
	// A new far quality tier changes texels per region. Rebuild both the packed
	// physical atlas dimensions and its region-slot publication on next ensure().
	m_nPackedColorAtlasSide = 0;
	m_nPackedColorAtlasHeight = 0;
	m_nPackedColorCount = 0u;
	m_nPackedTopologyGeneration = 0u;
	memset( m_szPackedColorSlots, 0, sizeof( m_szPackedColorSlots ) );
	memset( m_szDesiredColorSlots, 0, sizeof( m_szDesiredColorSlots ) );
	memset( m_szFineSlotOwners, 0, sizeof( m_szFineSlotOwners ) );
	memset( m_szFineSlotRetireTick, 0, sizeof( m_szFineSlotRetireTick ) );
	++m_nPackedColorLayoutGeneration;
	m_generatedHeightMap.clear();
	m_generatedColorMap.clear();
	m_heightImage.unref();
	m_colorImage.unref();
	m_heightDirtyRegions.reset();
	m_colorDirtyRegions.reset();
	m_bHeightDirty = true;
	m_bColorDirty = true;
	m_bColorCookDirty = true;
	m_stats.m_bHeightDirtyAll = true;
	m_stats.m_bColorDirtyAll = true;
	m_stats.m_bCookedLoaded = false;
	m_stats.m_bCookedSaved = false;
}

String Terrain3DFarSurfaceAtlasCache::getCookedPath(
	const String &rDirectory ) const {
	const String directory = !rDirectory.is_empty() ? rDirectory : m_config.m_dataDirectory;
	return directory.is_empty() ? String() : directory.path_join( "terrain3d_far_atlas.t3dfar" );
}

Ref< Image > Terrain3DFarSurfaceAtlasCache::getRegionMapOrDefault(
	const Terrain3DRegion *pRegion,
	const t3dMapType_t	   eMapType ) const {
	if ( pRegion && !pRegion->isDeleted() ) {
		const Ref< Image > pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
		}
		// Sparse cold pages are the authoritative saved source in shipping.
		// The hierarchy is a visual-only fallback, never a collision input.
		if ( m_config.m_pData ) {
			const Ref< Image > cold = m_config.m_pData->getCookedSparseRegionChannel( pRegion, eMapType );
			if ( cold.is_valid() )
				return cold;
			if ( eMapType == TYPE_COLOR || eMapType == TYPE_CONTROL ) {
				const Ref< Image > parent = m_config.m_pData->getCookedSparseVisualFallback( pRegion, eMapType );
				if ( parent.is_valid() )
					return parent;
			}
		}
	}
	switch ( eMapType ) {
	case TYPE_HEIGHT:
		return m_config.m_defaultHeightMap;
	case TYPE_CONTROL:
		return m_config.m_defaultControlMap;
	case TYPE_COLOR:
		return m_config.m_defaultColorMap;
	default:
		return Ref< Image >();
	}
}

uint64_t Terrain3DFarSurfaceAtlasCache::computeSourceSignature(
	void ) const {
	if ( !m_pRepository ) {
		return 0u;
	}
	uint64_t hash = 1469598103934665603ull;
	t3dHashFnv1a64AppendNativeU32( hash, T3D_GPU_FAR_ATLAS_VERSION );
	t3dHashFnv1a64AppendNativeU32( hash, T3D_REGION_GEOMETRY_SIGNATURE );
	t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( m_config.m_nHeightAtlasSide ) );
	t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( m_config.m_nSurfaceAtlasSide ) );
	t3dHashFnv1a64AppendNativeF32( hash, ( float ) ( s_fRegionVertexSpacing ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, m_config.m_defaultHeightMap );
	t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, m_config.m_defaultControlMap );
	t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, m_config.m_defaultColorMap );
	t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( m_pRepository->getActiveRegionCount() ) );
	for ( int map_index = 0; map_index < T3D_REGION_SLOTS; ++map_index ) {
		const Vector2i		   location( map_index % T3D_REGION_MAP_SIZE - T3D_REGION_MAP_SIZE / 2,
								 map_index / T3D_REGION_MAP_SIZE - T3D_REGION_MAP_SIZE / 2 );
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( map_index ) );
		t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( pRegion->getChannelFlags() ) );
		const String path = pRegion->get_path();
		if ( !path.is_empty() && FileAccess::file_exists( path ) ) {
			// The cooked far atlas is allowed to survive timestamp changes, but it
			// must never survive source-content changes. Hash the persisted region
			// bytes instead of using mtime as terrain coherence state.
			t3dHashFnv1a64AppendNativeU32( hash, 1u );
			if ( !t3dFarSurfaceAtlasCacheFnv1a64AppendFile( hash, path ) ) {
				return 0u;
			}
		} else {
			// Unsaved/editor-only regions have no authoritative file. Include the
			// live maps that can influence far height/surface generation.
			t3dHashFnv1a64AppendNativeU32( hash, 0u );
			t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, pRegion->getHeightMap() );
			t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, pRegion->getControlMap() );
			t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, pRegion->getColorMap() );
			t3dFarSurfaceAtlasCacheFnv1a64AppendImage( hash, pRegion->getDerivedNormalSlopeMap() );
		}
	}
	t3dFarSurfaceBakeData_t bake_data;
	t3dFarSurfaceAtlasCacheBuildBakeData( m_config.m_assets, bake_data );
	t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( bake_data.m_nCount ) );
	t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( bake_data.m_nFallbackTexture ) );
	for ( int index = 0; index < bake_data.m_nCount; ++index ) {
		t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( index ) );
		t3dHashFnv1a64AppendNativeU32( hash, ( uint32_t ) ( bake_data.m_populated[( size_t ) ( index )] ) );
	}
	Terrain3DMaterial *const pMaterial = m_config.m_material.ptr();
	t3dHashFnv1a64AppendNativeU32( hash, pMaterial ? 1u : 0u );
	if ( pMaterial ) {
		t3dHashFnv1a64AppendNativeU32( hash, pMaterial->getAutoShaderEnabled() ? 1u : 0u );
		t3dHashFnv1a64AppendNativeF32( hash,
									   t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_slope", 1.0f ) );
		t3dHashFnv1a64AppendNativeF32(
			hash,
			t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f ) );
		t3dHashFnv1a64AppendNativeU32(
			hash,
			( uint32_t ) ( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_base_texture", 0 ) ) );
		t3dHashFnv1a64AppendNativeU32(
			hash,
			( uint32_t ) ( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ) ) );
	}
	return hash;
}

bool Terrain3DFarSurfaceAtlasCache::initializeHeightImage(
	void ) {
	const int height_side = MAX( 1, m_config.m_nHeightAtlasSide );
	if ( m_heightImage.is_null() || m_heightImage->get_width() != height_side ||
		 m_heightImage->get_height() != height_side || m_heightImage->get_format() != Image::FORMAT_R16 ) {
		const uint16_t	default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
		PackedByteArray bytes;
		bytes.resize( ( int64_t ) ( height_side ) *height_side * 2 );
		uint8_t *const pWrite = bytes.ptrw();
		for ( int index = 0; index < height_side * height_side; ++index ) {
			memcpy( pWrite + ( size_t ) ( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		m_heightImage = Image::create_from_data( height_side, height_side, false, Image::FORMAT_R16, bytes );
	}
	return m_heightImage.is_valid() && !m_heightImage->is_empty();
}

bool Terrain3DFarSurfaceAtlasCache::initializeColorImage(
	void ) {
	const int surface_side = MAX( 1, m_config.m_nSurfaceAtlasSide );
	if ( m_colorImage.is_null() || m_colorImage->get_width() != surface_side ||
		 m_colorImage->get_height() != surface_side || m_colorImage->get_format() != Image::FORMAT_RGBA8 ) {
		PackedByteArray bytes;
		bytes.resize( ( int64_t ) ( surface_side ) *surface_side * 4 );
		t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty( bytes );
		m_colorImage = Image::create_from_data( surface_side, surface_side, false, Image::FORMAT_RGBA8, bytes );
	}
	return m_colorImage.is_valid() && !m_colorImage->is_empty();
}

bool Terrain3DFarSurfaceAtlasCache::loadCooked(
	void ) {
	if ( !m_pRepository ) {
		return false;
	}
	const String path = getCookedPath();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	Time *const				pTime = Time::get_singleton();
	const uint64_t			started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	const Ref< FileAccess > pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < T3D_GPU_FAR_ATLAS_HEADER_BYTES ) {
		return false;
	}
	const uint64_t magic = pFile->get_64();
	const uint32_t version = pFile->get_32();
	const uint32_t geometry_signature = pFile->get_32();
	const uint32_t geometry_reserved = pFile->get_32();
	const uint32_t height_atlas_size = pFile->get_32();
	const uint32_t surface_atlas_size = pFile->get_32();
	const uint32_t height_size = pFile->get_32();
	const uint32_t color_size = pFile->get_32();
	const uint32_t height_checksum = pFile->get_32();
	const uint32_t color_checksum = pFile->get_32();
	const uint64_t source_signature = pFile->get_64();
	const uint32_t expected_height_size =
		( uint32_t ) ( m_config.m_nHeightAtlasSide * m_config.m_nHeightAtlasSide * 2 );
	const uint32_t expected_color_size =
		( uint32_t ) ( m_config.m_nSurfaceAtlasSide * m_config.m_nSurfaceAtlasSide * 4 );
	if ( magic != T3D_GPU_FAR_ATLAS_MAGIC || version != T3D_GPU_FAR_ATLAS_VERSION ||
		 geometry_signature != T3D_REGION_GEOMETRY_SIGNATURE || geometry_reserved != 0u ||
		 height_atlas_size != ( uint32_t ) ( m_config.m_nHeightAtlasSide ) ||
		 surface_atlas_size != ( uint32_t ) ( m_config.m_nSurfaceAtlasSide ) || height_size != expected_height_size ||
		 color_size != expected_color_size ||
		 pFile->get_length() !=
			 T3D_GPU_FAR_ATLAS_HEADER_BYTES + ( uint64_t ) ( height_size ) + ( uint64_t ) ( color_size ) ) {
		return false;
	}
	if ( source_signature == 0u || ( !m_config.m_bStrictCooked && source_signature != computeSourceSignature() ) ) {
		return false;
	}
	const PackedByteArray height_bytes = pFile->get_buffer( height_size );
	const PackedByteArray color_bytes = pFile->get_buffer( color_size );
	if ( height_bytes.size() != height_size || color_bytes.size() != color_size ||
		 t3dRuntimePageChecksum( height_bytes.ptr(), ( size_t ) ( height_bytes.size() ) ) != height_checksum ||
		 t3dRuntimePageChecksum( color_bytes.ptr(), ( size_t ) ( color_bytes.size() ) ) != color_checksum ) {
		return false;
	}
	m_heightImage = Image::create_from_data( m_config.m_nHeightAtlasSide,
											 m_config.m_nHeightAtlasSide,
											 false,
											 Image::FORMAT_R16,
											 height_bytes );
	m_colorImage = Image::create_from_data( m_config.m_nSurfaceAtlasSide,
											m_config.m_nSurfaceAtlasSide,
											false,
											Image::FORMAT_RGBA8,
											color_bytes );
	if ( m_heightImage.is_null() || m_colorImage.is_null() ) {
		return false;
	}
	m_bHeightDirty = false;
	m_bColorDirty = false;
	m_bColorCookDirty = false;
	m_stats.m_bHeightDirtyAll = false;
	m_stats.m_bColorDirtyAll = false;
	m_heightDirtyRegions.reset();
	m_colorDirtyRegions.reset();
	m_stats.m_bCookedLoaded = true;
	m_stats.m_bCookedSaved = false;
	m_stats.m_nCookIoUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::writeCooked(
	const String &rDirectory ) {
	if ( !m_pRepository ) {
		return false;
	}
	const String path = getCookedPath( rDirectory );
	if ( path.is_empty() ) {
		return false;
	}
	if ( !m_bHeightDirty && !m_bColorCookDirty && ( m_stats.m_bCookedLoaded || m_stats.m_bCookedSaved ) &&
		 FileAccess::file_exists( path ) ) {
		return true;
	}
	// A cook is CPU work. It must not depend on the atlases being required by the
	// active render tiering, nor on a successful RenderingDevice publication.
	if ( !cookAtlases() ) {
		return false;
	}
	Time *const			  pTime = Time::get_singleton();
	const uint64_t		  started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	const PackedByteArray height_bytes = m_heightImage->get_data();
	const PackedByteArray color_bytes = m_colorImage->get_data();
	const uint64_t		  source_signature = computeSourceSignature();
	if ( source_signature == 0u ) {
		return false;
	}
	const String temporary_path = path + String( ".tmp" );
	const String backup_path = path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref< FileAccess > pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		return false;
	}
	pFile->store_64( T3D_GPU_FAR_ATLAS_MAGIC );
	pFile->store_32( T3D_GPU_FAR_ATLAS_VERSION );
	pFile->store_32( T3D_REGION_GEOMETRY_SIGNATURE );
	pFile->store_32( 0u );
	pFile->store_32( ( uint32_t ) ( m_config.m_nHeightAtlasSide ) );
	pFile->store_32( ( uint32_t ) ( m_config.m_nSurfaceAtlasSide ) );
	pFile->store_32( ( uint32_t ) ( height_bytes.size() ) );
	pFile->store_32( ( uint32_t ) ( color_bytes.size() ) );
	pFile->store_32( t3dRuntimePageChecksum( height_bytes.ptr(), ( size_t ) ( height_bytes.size() ) ) );
	pFile->store_32( t3dRuntimePageChecksum( color_bytes.ptr(), ( size_t ) ( color_bytes.size() ) ) );
	pFile->store_64( source_signature );
	pFile->store_buffer( height_bytes );
	pFile->store_buffer( color_bytes );
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return false;
	}
	const bool had_previous = FileAccess::file_exists( path );
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( had_previous && DirAccess::rename_absolute( path, backup_path ) != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary_path, path ) != OK ) {
		if ( had_previous ) {
			DirAccess::rename_absolute( backup_path, path );
		}
		return false;
	}
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	m_stats.m_bCookedSaved = true;
	m_stats.m_bCookedLoaded = false;
	if ( IS_EDITOR && path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( path );
		}
	}
	m_stats.m_nCookIoUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	return true;
}

void Terrain3DFarSurfaceAtlasCache::markAllDirty(
	const t3dMapType_t eMapType ) {
	if ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX ) {
		m_bHeightDirty = true;
		m_stats.m_bHeightDirtyAll = true;
		m_heightDirtyRegions.reset();
	}
	if ( eMapType == TYPE_CONTROL || eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) {
		m_bColorDirty = true;
		m_bColorCookDirty = true;
		m_stats.m_bColorDirtyAll = true;
		m_colorDirtyRegions.reset();
	}
	m_stats.m_bCookedLoaded = false;
	m_stats.m_bCookedSaved = false;
}

void Terrain3DFarSurfaceAtlasCache::markRegionDirty(
	const Vector2i	  &vLocation,
	const t3dMapType_t eMapType ) {
	if ( !m_pRepository || !m_pRepository->isLocationValid( vLocation ) ) {
		return;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return;
	}
	if ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX ) {
		m_bHeightDirty = true;
		if ( !m_stats.m_bHeightDirtyAll ) {
			m_heightDirtyRegions.set( ( size_t ) ( map_index ) );
		}
	}
	if ( eMapType == TYPE_CONTROL || eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) {
		m_bColorDirty = true;
		m_bColorCookDirty = true;
		if ( !m_stats.m_bColorDirtyAll ) {
			m_colorDirtyRegions.set( ( size_t ) ( map_index ) );
		}
	}
	m_stats.m_bCookedLoaded = false;
	m_stats.m_bCookedSaved = false;
}

bool Terrain3DFarSurfaceAtlasCache::patchHeightRegion(
	const Vector2i	&vLocation,
	PackedByteArray &rAtlasBytes ) {
	if ( !m_pRepository ) {
		return false;
	}
	const int atlas_size = m_config.m_nHeightAtlasSide;
	if ( T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 || atlas_size % T3D_REGION_MAP_SIZE != 0 ) {
		return false;
	}
	const int samples_per_region = atlas_size / T3D_REGION_MAP_SIZE;
	if ( samples_per_region <= 0 || T3D_REGION_SIZE % samples_per_region != 0 ) {
		return false;
	}
	const int atlas_half = T3D_REGION_MAP_SIZE / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( atlas_origin_x < 0 || atlas_origin_y < 0 || atlas_origin_x + samples_per_region > atlas_size ||
		 atlas_origin_y + samples_per_region > atlas_size ) {
		return false;
	}
	const uint16_t			default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
	const uint8_t			*height_bytes = NULL;
	const uint16_t		   *height = NULL;
	PackedByteArray			region_height_bytes;
	Terrain3DRegion *const	pRegion = m_pRepository->getRegionPtr( vLocation );
	Terrain3DActiveMapLease lease;
	if ( pRegion && !pRegion->isDeleted() ) {
		if ( m_pCpuRegionCache && m_pCpuRegionCache->isEnabled() &&
			 !m_pCpuRegionCache->ensureResident( vLocation, true ) ) {
			return false;
		}
		if ( m_pSamplingStorage ) {
			lease = m_pSamplingStorage->acquireActive( vLocation );
		}
		if ( lease.valid() ) {
			height = lease.view().m_pHeight;
		} else {
			const Ref< Image > pHeight = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
			if ( pHeight.is_valid() && pHeight->get_format() == Image::FORMAT_R16 ) {
				region_height_bytes = pHeight->get_data();
				if ( region_height_bytes.size() >= ( int64_t ) ( T3D_REGION_SIZE ) *T3D_REGION_SIZE * 2 ) {
					height_bytes = region_height_bytes.ptr();
				}
			}
		}
	}
	const int	   source_step = T3D_REGION_SIZE / samples_per_region;
	uint8_t *const pWrite = rAtlasBytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int y = 0; y < samples_per_region; ++y ) {
		const int source_y = MIN( T3D_REGION_SIZE - 1, y * source_step );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int	   source_x = MIN( T3D_REGION_SIZE - 1, x * source_step );
			const size_t sample_index = ( size_t ) source_y * T3D_REGION_SIZE + source_x;
			const uint16_t encoded = height ? height[sample_index]
										: ( height_bytes ? t3dReadNativeUnaligned< uint16_t >( height_bytes + sample_index * 2u )
															: default_height );
			const int	   atlas_index = ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x;
			memcpy( pWrite + ( size_t ) ( atlas_index ) * 2u, &encoded, sizeof( encoded ) );
		}
	}
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::patchColorRegion(
	const Vector2i	&vLocation,
	PackedByteArray &rAtlasBytes ) {
	const int side = m_config.m_nSurfaceAtlasSide;
	const int region_samples = side / T3D_REGION_MAP_SIZE;
	return patchColorRegionAt( vLocation,
							   rAtlasBytes,
							   side,
							   ( vLocation.x + T3D_REGION_MAP_HALF ) * region_samples,
							   ( vLocation.y + T3D_REGION_MAP_HALF ) * region_samples );
}

bool Terrain3DFarSurfaceAtlasCache::patchColorRegionAt(
	const Vector2i	&vLocation,
	PackedByteArray &rAtlasBytes,
	const int		 nDestinationSide,
	const int		 nDestinationX,
	const int		 nDestinationY ) {
	if ( !m_pRepository ) {
		return false;
	}
	const int atlas_size = nDestinationSide;
	if ( T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 || m_config.m_nSurfaceAtlasSide % T3D_REGION_MAP_SIZE != 0 ) {
		return false;
	}
	const int samples_per_region = m_config.m_nSurfaceAtlasSide / T3D_REGION_MAP_SIZE;
	const int atlas_origin_x = nDestinationX;
	const int atlas_origin_y = nDestinationY;
	if ( samples_per_region <= 0 || atlas_origin_x < 0 || atlas_origin_y < 0 ||
		 atlas_origin_x + samples_per_region > atlas_size || atlas_origin_y + samples_per_region > atlas_size ||
		 rAtlasBytes.size() < ( int64_t ) ( atlas_size ) *atlas_size * 4 ) {
		return false;
	}
	uint8_t *pAtlasWrite = rAtlasBytes.ptrw();
	if ( !pAtlasWrite ) {
		return false;
	}
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		for ( int y = 0; y < samples_per_region; ++y ) {
			for ( int x = 0; x < samples_per_region; ++x ) {
				uint8_t *pixel =
					pAtlasWrite + ( size_t ) ( ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x ) * 4u;
				pixel[0] = 255u;
				pixel[1] = 255u;
				pixel[2] = 255u;
				pixel[3] = 0u;
			}
		}
		return true;
	}
	if ( m_pCpuRegionCache && m_pCpuRegionCache->isEnabled() &&
		 !m_pCpuRegionCache->ensureResident( vLocation, true ) ) {
		return false;
	}
	const Ref< Image > pSourceColor = getRegionMapOrDefault( pRegion, TYPE_COLOR );
	const Ref< Image > pSourceControl = getRegionMapOrDefault( pRegion, TYPE_CONTROL );
	const Ref< Image > pSourceHeight = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
	const Ref< Image > pSourceDerived = pRegion->getDerivedNormalSlopeMap();
	if ( pSourceColor.is_null() || pSourceControl.is_null() || pSourceColor->get_format() != Image::FORMAT_RGBA8 ||
		 pSourceControl->get_format() != Image::FORMAT_RF ) {
		return false;
	}
	const PackedByteArray color_bytes = pSourceColor->get_data();
	const PackedByteArray control_bytes = pSourceControl->get_data();
	const PackedByteArray height_bytes = pSourceHeight.is_valid() ? pSourceHeight->get_data() : PackedByteArray();
	const PackedByteArray derived_bytes = pSourceDerived.is_valid() ? pSourceDerived->get_data() : PackedByteArray();
	const uint8_t		 *pColorRead = color_bytes.ptr();
	const uint8_t		 *pControlRead = control_bytes.ptr();
	const uint8_t		 *pHeightRead = height_bytes.ptr();
	const uint8_t		 *pDerivedRead = derived_bytes.ptr();
	const int			  color_width = pSourceColor->get_width();
	const int			  color_height = pSourceColor->get_height();
	const bool			  height_valid = pSourceHeight.is_valid() && pSourceHeight->get_format() == Image::FORMAT_R16 &&
							  height_bytes.size() >= ( int64_t ) ( T3D_REGION_SIZE ) *T3D_REGION_SIZE * 2;
	const int  derived_width = pSourceDerived.is_valid() ? pSourceDerived->get_width() : 0;
	const int  derived_height = pSourceDerived.is_valid() ? pSourceDerived->get_height() : 0;
	const bool derived_valid = pSourceDerived.is_valid() && pSourceDerived->get_format() == Image::FORMAT_RG8 &&
							   derived_width > 0 && derived_height > 0 &&
							   derived_bytes.size() >= ( int64_t ) ( derived_width ) *derived_height * 2;
	if ( !pColorRead || !pControlRead || color_width <= 0 || color_height <= 0 ||
		 control_bytes.size() < ( int64_t ) ( T3D_REGION_SIZE ) *T3D_REGION_SIZE * 4 ) {
		return false;
	}
	t3dFarSurfaceBakeData_t bake_data;
	t3dFarSurfaceAtlasCacheBuildBakeData( m_config.m_assets, bake_data );
	Terrain3DMaterial *const pMaterial = m_config.m_material.ptr();
	const bool				 auto_shader_enabled = pMaterial && pMaterial->getAutoShaderEnabled();
	const float				 auto_slope = t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_slope", 1.0f );
	const float				 auto_height_reduction =
		t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f );
	const int auto_layer_max = MAX( bake_data.m_nCount - 1, 0 );
	const int auto_base_texture =
		CLAMP( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_base_texture", 0 ), 0, auto_layer_max );
	const int auto_overlay_texture =
		CLAMP( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ), 0, auto_layer_max );
	const int source_step = MAX( 1, T3D_REGION_SIZE / samples_per_region );

	for ( int y = 0; y < samples_per_region; ++y ) {
		const int region_y0 = y * T3D_REGION_SIZE / samples_per_region;
		const int region_y1 = MAX( region_y0 + 1, ( y + 1 ) * T3D_REGION_SIZE / samples_per_region );
		const int color_y0 = y * color_height / samples_per_region;
		const int color_y1 = MAX( color_y0 + 1, ( y + 1 ) * color_height / samples_per_region );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int region_x0 = x * T3D_REGION_SIZE / samples_per_region;
			const int region_x1 = MAX( region_x0 + 1, ( x + 1 ) * T3D_REGION_SIZE / samples_per_region );
			const int color_x0 = x * color_width / samples_per_region;
			const int color_x1 = MAX( color_x0 + 1, ( x + 1 ) * color_width / samples_per_region );
			uint64_t  sum_r = 0u;
			uint64_t  sum_g = 0u;
			uint64_t  sum_b = 0u;
			uint64_t  sum_a = 0u;
			int		  color_samples = 0;
			for ( int color_y = color_y0; color_y < MIN( color_y1, color_height ); ++color_y ) {
				for ( int color_x = color_x0; color_x < MIN( color_x1, color_width ); ++color_x ) {
					const size_t color_offset = ( size_t ) ( color_y * color_width + color_x ) * 4u;
					sum_r += pColorRead[color_offset];
					sum_g += pColorRead[color_offset + 1u];
					sum_b += pColorRead[color_offset + 2u];
					sum_a += pColorRead[color_offset + 3u];
					++color_samples;
				}
			}
			const int	   center_x = MIN( T3D_REGION_SIZE - 1, region_x0 + ( region_x1 - region_x0 ) / 2 );
			const int	   center_y = MIN( T3D_REGION_SIZE - 1, region_y0 + ( region_y1 - region_y0 ) / 2 );
			const int	   center_index = center_y * T3D_REGION_SIZE + center_x;
			const uint32_t control =
				t3dFarSurfaceAtlasCacheReadU32Unaligned( pControlRead, ( size_t ) ( center_index ) * 4u );
			float solid_coverage = 0.0f;
			for ( int probe_y = 0; probe_y < 2; ++probe_y ) {
				for ( int probe_x = 0; probe_x < 2; ++probe_x ) {
					const int px = MIN( T3D_REGION_SIZE - 1, region_x0 + ( ( probe_x * 2 + 1 ) * source_step ) / 4 );
					const int py = MIN( T3D_REGION_SIZE - 1, region_y0 + ( ( probe_y * 2 + 1 ) * source_step ) / 4 );
					const uint32_t probe_control =
						t3dFarSurfaceAtlasCacheReadU32Unaligned( pControlRead,
																 ( size_t ) ( py * T3D_REGION_SIZE + px ) * 4u );
					solid_coverage += t3dIsHole( probe_control ) ? 0.0f : 0.25f;
				}
			}
			int	  base_id = ( int ) ( t3dGetBase( control ) );
			int	  overlay_id = ( int ) ( t3dGetOverlay( control ) );
			float blend = ( float ) ( t3dGetBlend( control ) ) / s_fByteMaxFloat;
			if ( auto_shader_enabled && t3dIsAuto( control ) ) {
				float normal_y = 1.0f;
				float height = 0.0f;
				if ( height_valid ) {
					height = t3dRegionCodecDecodeHeightU16(
						t3dFarSurfaceAtlasCacheReadU16Unaligned( pHeightRead, ( size_t ) ( center_index ) * 2u ) );
				}
				if ( derived_valid ) {
					const int	 derived_x = MIN( derived_width - 1, center_x * derived_width / T3D_REGION_SIZE );
					const int	 derived_y = MIN( derived_height - 1, center_y * derived_height / T3D_REGION_SIZE );
					const size_t derived_offset = ( size_t ) ( derived_y * derived_width + derived_x ) * 2u;
					const float	 normal_x = ( float ) ( pDerivedRead[derived_offset] ) / s_fByteMaxFloat * 2.0f - 1.0f;
					const float	 normal_z =
						( float ) ( pDerivedRead[derived_offset + 1u] ) / s_fByteMaxFloat * 2.0f - 1.0f;
					normal_y = Math::sqrt( MAX( 1.0f - normal_x * normal_x - normal_z * normal_z, 0.0f ) );
				} else if ( height_valid ) {
					const int	sample_x = center_y * T3D_REGION_SIZE + MIN( T3D_REGION_SIZE - 1, center_x + 1 );
					const int	sample_z = MIN( T3D_REGION_SIZE - 1, center_y + 1 ) * T3D_REGION_SIZE + center_x;
					const float height_x = t3dRegionCodecDecodeHeightU16(
						t3dFarSurfaceAtlasCacheReadU16Unaligned( pHeightRead, ( size_t ) ( sample_x ) * 2u ) );
					const float height_z = t3dRegionCodecDecodeHeightU16(
						t3dFarSurfaceAtlasCacheReadU16Unaligned( pHeightRead, ( size_t ) ( sample_z ) * 2u ) );
					const float spacing = MAX( ( float ) ( s_fRegionVertexSpacing ), 0.0001f );
					const float dx = height_x - height;
					const float dz = height_z - height;
					normal_y = spacing / Math::sqrt( dx * dx + spacing * spacing + dz * dz );
				}
				base_id = auto_base_texture;
				overlay_id = auto_overlay_texture;
				blend = CLAMP( auto_slope * 2.0f * ( normal_y - 1.0f ) + 1.0f - auto_height_reduction * 0.01f * height,
							   0.0f,
							   1.0f );
			}
			base_id = t3dFarSurfaceAtlasCacheResolveMaterialId( bake_data, base_id );
			overlay_id = t3dFarSurfaceAtlasCacheResolveMaterialId( bake_data, overlay_id );
			const int	 dominant_material = blend >= 0.5f ? overlay_id : base_id;
			const float	 inv_samples = color_samples > 0 ? 1.0f / ( float ) ( color_samples ) : 0.0f;
			const size_t atlas_offset = ( size_t ) ( ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x ) * 4u;
			pAtlasWrite[atlas_offset] =
				t3dFarSurfaceAtlasCacheColorToByte( ( float ) ( sum_r ) *inv_samples / s_fByteMaxFloat );
			pAtlasWrite[atlas_offset + 1u] =
				t3dFarSurfaceAtlasCacheColorToByte( ( float ) ( sum_g ) *inv_samples / s_fByteMaxFloat );
			pAtlasWrite[atlas_offset + 2u] =
				t3dFarSurfaceAtlasCacheColorToByte( ( float ) ( sum_b ) *inv_samples / s_fByteMaxFloat );
			const float color_alpha = color_samples > 0 ? ( float ) ( sum_a ) *inv_samples / s_fByteMaxFloat : 0.5f;
			pAtlasWrite[atlas_offset + 3u] =
				t3dFarSurfaceAtlasCacheFarSurfaceAlpha( solid_coverage, color_alpha, dominant_material );
		}
	}
	return true;
}

// CPU-only height atlas rebuild. Deliberately free of both the "is this atlas
// required for rendering" gate and any RenderingDevice work, so a disk cook can
// use it while the GPU region cache and runtime shader tiering are switched off.
bool Terrain3DFarSurfaceAtlasCache::rebuildHeightImage(
	void ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H11_ATLAS_REBUILD );
	if ( !m_pRepository || !initializeHeightImage() ) {
		return false;
	}
	Time *const		pTime = Time::get_singleton();
	const uint64_t	started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	PackedByteArray atlas_bytes = m_heightImage->get_data();
	T3D_STREAM_PROFILE_BYTES( T3D_SP_H11_ATLAS_REBUILD, ( uint64_t ) atlas_bytes.size() );
	m_stats.m_nHeightRegionsLastTick = 0;
	m_stats.m_nHeightDirtyTexelsLast = 0u;
	m_stats.m_nHeightRebuiltBytesLast = 0u;
	m_stats.m_nHeightUploadedBytesLast = 0u;
	m_stats.m_nHeightPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t ) ( m_config.m_nHeightAtlasSide ) * m_config.m_nHeightAtlasSide;
	const int	   samples_per_region = T3D_REGION_MAP_SIZE > 0 ? m_config.m_nHeightAtlasSide / T3D_REGION_MAP_SIZE : 0;
	const uint64_t region_texels =
		samples_per_region > 0 ? ( uint64_t ) ( samples_per_region ) *samples_per_region : 0u;
	if ( m_stats.m_bHeightDirtyAll ) {
		m_stats.m_nHeightDirtyTexelsLast = atlas_texels;
		const uint16_t default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
		uint8_t *const pWrite = atlas_bytes.ptrw();
		for ( int index = 0; index < m_config.m_nHeightAtlasSide * m_config.m_nHeightAtlasSide; ++index ) {
			memcpy( pWrite + ( size_t ) ( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
			if ( location != s_v2iMax && patchHeightRegion( location, atlas_bytes ) ) {
				++m_stats.m_nHeightRegionsLastTick;
			}
		}
	} else {
		m_stats.m_nHeightDirtyTexelsLast = ( uint64_t ) ( m_heightDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < T3D_REGION_SLOTS; ++index ) {
			if ( !m_heightDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location( ( int ) ( index % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_SIZE / 2,
									 ( int ) ( index / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_SIZE / 2 );
			if ( patchHeightRegion( location, atlas_bytes ) ) {
				++m_stats.m_nHeightRegionsLastTick;
			}
		}
	}
	m_stats.m_nHeightRebuiltBytesLast = m_stats.m_bHeightDirtyAll
											? ( uint64_t ) ( atlas_bytes.size() )
											: ( uint64_t ) ( m_stats.m_nHeightRegionsLastTick ) * region_texels * 2u;
	m_heightImage->set_data( m_config.m_nHeightAtlasSide,
							 m_config.m_nHeightAtlasSide,
							 false,
							 Image::FORMAT_R16,
							 atlas_bytes );
	m_stats.m_nHeightProcessingUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	return true;
}

// GPU publication for the height atlas. Requires an already-rebuilt CPU image and
// reads the current dirty set to decide between a full and an incremental upload.
bool Terrain3DFarSurfaceAtlasCache::publishHeightAtlas(
	void ) {
	if ( !m_pRepository || m_heightImage.is_null() ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const int	samples_per_region = T3D_REGION_MAP_SIZE > 0 ? m_config.m_nHeightAtlasSide / T3D_REGION_MAP_SIZE : 0;
	const PackedByteArray atlas_bytes = m_heightImage->get_data();
	m_stats.m_nHeightUploadedBytesLast = 0u;
	const uint64_t publication_started = pTime ? pTime->get_ticks_usec() : 0u;
	bool		   uploaded = true;
	const bool	   bFullUpload = m_stats.m_bHeightDirtyAll || !m_generatedHeightMap.getRid().is_valid();
	if ( bFullUpload ) {
		uploaded = m_generatedHeightMap.getRid().is_valid()
					   ? m_generatedHeightMap.update2d( m_heightImage )
					   : m_generatedHeightMap.create( m_heightImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
		m_stats.m_nHeightUploadedBytesLast = uploaded ? ( uint64_t ) ( atlas_bytes.size() ) : 0u;
	} else {
		const int atlas_half = T3D_REGION_MAP_SIZE / 2;
		for ( size_t index = 0; uploaded && index < T3D_REGION_SLOTS; ++index ) {
			if ( !m_heightDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i	   location( ( int ) ( index % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_SIZE / 2,
									 ( int ) ( index / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_SIZE / 2 );
			const Rect2i	   destination( ( location.x + atlas_half ) * samples_per_region,
										( location.y + atlas_half ) * samples_per_region,
										samples_per_region,
										samples_per_region );
			const Ref< Image > region = m_heightImage->get_region( destination );
			uploaded =
				region.is_valid() && !region->is_empty() && m_generatedHeightMap.updateRegion2d( region, destination );
			if ( uploaded ) {
				m_stats.m_nHeightUploadedBytesLast += ( uint64_t ) ( region->get_data().size() );
			}
		}
	}
	m_stats.m_nHeightPublicationUsecLast = pTime ? pTime->get_ticks_usec() - publication_started : 0u;
	if ( !uploaded ) {
		return false;
	}
	m_stats.m_nHeightUploadedBytesTotal += m_stats.m_nHeightUploadedBytesLast;
	++m_stats.m_nHeightPublicationCount;
	++m_stats.m_nHeightUpdatesLastTick;
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::rebuildHeightAtlas(
	void ) {
	if ( !m_config.m_bRequired || !m_pSamplingStorage || !m_pSamplingStorage->isActiveOwner() ) {
		return false;
	}
	if ( !rebuildHeightImage() || !publishHeightAtlas() ) {
		return false;
	}
	m_bHeightDirty = false;
	m_stats.m_bHeightDirtyAll = false;
	m_heightDirtyRegions.reset();
	return true;
}

// CPU-only surface atlas rebuild. See rebuildHeightImage() for why the required/GPU
// gates are absent here.
bool Terrain3DFarSurfaceAtlasCache::rebuildColorImage(
	void ) {
	T3D_STREAM_PROFILE_SCOPE( T3D_SP_H11_ATLAS_REBUILD );
	if ( !m_pRepository || !initializeColorImage() ) {
		return false;
	}
	Time *const		pTime = Time::get_singleton();
	const uint64_t	started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	PackedByteArray atlas_bytes = m_colorImage->get_data();
	T3D_STREAM_PROFILE_BYTES( T3D_SP_H11_ATLAS_REBUILD, ( uint64_t ) atlas_bytes.size() );
	m_stats.m_nColorRegionsLastTick = 0;
	m_stats.m_nColorDirtyTexelsLast = 0u;
	m_stats.m_nColorRebuiltBytesLast = 0u;
	m_stats.m_nColorUploadedBytesLast = 0u;
	m_stats.m_nColorPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t ) ( m_config.m_nSurfaceAtlasSide ) * m_config.m_nSurfaceAtlasSide;
	const int samples_per_region = T3D_REGION_MAP_SIZE > 0 ? m_config.m_nSurfaceAtlasSide / T3D_REGION_MAP_SIZE : 0;
	const uint64_t region_texels =
		samples_per_region > 0 ? ( uint64_t ) ( samples_per_region ) *samples_per_region : 0u;
	if ( m_stats.m_bColorDirtyAll ) {
		m_stats.m_nColorDirtyTexelsLast = atlas_texels;
		t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty( atlas_bytes );
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[slot] );
			if ( location != s_v2iMax && patchColorRegion( location, atlas_bytes ) ) {
				++m_stats.m_nColorRegionsLastTick;
			}
		}
	} else {
		m_stats.m_nColorDirtyTexelsLast = ( uint64_t ) ( m_colorDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < T3D_REGION_SLOTS; ++index ) {
			if ( !m_colorDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location( ( int ) ( index % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_SIZE / 2,
									 ( int ) ( index / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_SIZE / 2 );
			if ( patchColorRegion( location, atlas_bytes ) ) {
				++m_stats.m_nColorRegionsLastTick;
			}
		}
	}
	m_stats.m_nColorRebuiltBytesLast = m_stats.m_bColorDirtyAll
										   ? ( uint64_t ) ( atlas_bytes.size() )
										   : ( uint64_t ) ( m_stats.m_nColorRegionsLastTick ) * region_texels * 4u;
	m_colorImage->set_data( m_config.m_nSurfaceAtlasSide,
							m_config.m_nSurfaceAtlasSide,
							false,
							Image::FORMAT_RGBA8,
							atlas_bytes );
	m_bColorCookDirty = false;
	m_stats.m_nColorProcessingUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	return true;
}

// The high-resolution far pages use a fixed physical grant. The bottom 512
// rows of the same RGBA8 texture hold 16x16 samples for every logical region.
// Only uploaded/committed mappings are returned to the render publication builder.
uint16_t Terrain3DFarSurfaceAtlasCache::getPackedColorSlot(
	const Vector2i &rLocation ) const {
	if ( !m_pRepository || !m_pRepository->isLocationValid( rLocation ) ) {
		return 0u;
	}
	const int index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	return index >= 0 ? m_szPackedColorSlots[index] : 0u;
}

void Terrain3DFarSurfaceAtlasCache::setFrameDemand(
	const t3dResidencyFrameState_s &rFrame ) {
	for ( uint32_t i = 0u; i < T3D_REGION_SLOTS; ++i ) {
		const Vector2i location( ( int ) ( i % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF,
								 ( int ) ( i / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF );
		m_szDemandPriority[i] = t3dRasterRegionPriority( rFrame, location );
	}
	++m_nDemandTick;
	m_bFrameDemandAvailable = true;
}

bool Terrain3DFarSurfaceAtlasCache::refreshPackedColorLayout(
	void ) {
	if ( !m_pRepository || m_config.m_nSurfaceAtlasSide < T3D_REGION_MAP_SIZE ||
		 m_config.m_nSurfaceAtlasSide % T3D_REGION_MAP_SIZE != 0 ) {
		return false;
	}
	const int samples = m_config.m_nSurfaceAtlasSide / T3D_REGION_MAP_SIZE;
	const int limit = CLAMP( m_config.m_nFinePageLimit, 16, 64 );
	int		  columns = 1;
	while ( columns * columns < limit ) {
		++columns;
	}
	const int  rows = ( limit + columns - 1 ) / columns;
	const int  width = MAX( columns * samples, 512 );
	const int  height = rows * samples + 512;
	const bool layoutChanged = width != m_nPackedColorAtlasSide || height != m_nPackedColorAtlasHeight;
	if ( layoutChanged ) {
		m_nPackedColorAtlasSide = width;
		m_nPackedColorAtlasHeight = height;
		memset( m_szPackedColorSlots, 0, sizeof( m_szPackedColorSlots ) );
		memset( m_szDesiredColorSlots, 0, sizeof( m_szDesiredColorSlots ) );
		memset( m_szFineSlotOwners, 0, sizeof( m_szFineSlotOwners ) );
		memset( m_szFineSlotRetireTick, 0, sizeof( m_szFineSlotRetireTick ) );
		m_nPackedColorCount = 0u;
		m_generatedColorMap.clear();
	}
	uint8_t present[T3D_REGION_SLOTS]{};
	for ( int i = 0; i < m_pRepository->getSlotCount(); ++i ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[i] );
		if ( location == s_v2iMax || !m_pRepository->isLocationValid( location ) ) {
			continue;
		}
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( mapIndex >= 0 ) {
			present[mapIndex] = 1u;
		}
	}
	for ( int i = 0; i < limit; ++i ) {
		const uint16_t owner = m_szFineSlotOwners[i];
		if ( owner == 0u || present[owner - 1u] ) {
			continue;
		}
		m_szFineSlotOwners[i] = 0u;
		m_szFineSlotRetireTick[i] = m_nDemandTick + 4u;
		m_szDesiredColorSlots[owner - 1u] = 0u;
	}
	m_stats.m_bColorDirtyAll = true; // refresh permanent coarse tier on topology changes
	m_bColorDirty = true;
	return true;
}

// CPU and GPU residency use the same priorities. The GPU atlas is a fixed
// allocation: no resizes occur because an editor brush, visibility, or page
// queue changed. Each frame admits only a bounded number of new fine regions.
void Terrain3DFarSurfaceAtlasCache::serviceFineResidency(
	void ) {
	if ( !m_pRepository || !m_bFrameDemandAvailable || m_nPackedColorAtlasSide == 0 ||
		 m_nLastDemandServiceTick == m_nDemandTick ) {
		return;
	}
	m_nLastDemandServiceTick = m_nDemandTick;
	const int limit = CLAMP( m_config.m_nFinePageLimit, 16, 64 );
	const int operations = CLAMP( m_config.m_nFineUploadBudget, 1, 8 );
	uint8_t	  present[T3D_REGION_SLOTS]{};
	for ( int i = 0; i < m_pRepository->getSlotCount(); ++i ) {
		const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[i] );
		if ( location == s_v2iMax || !m_pRepository->isLocationValid( location ) ) {
			continue;
		}
		const int mapIndex = Terrain3DRegionRepository::getRegionMapIndex( location );
		if ( mapIndex >= 0 ) {
			present[mapIndex] = 1u;
		}
	}
	for ( int action = 0; action < operations; ++action ) {
		int		 bestRegion = -1;
		uint32_t bestScore = 0u;
		for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
			if ( !present[i] || m_szDesiredColorSlots[i] != 0u ) {
				continue;
			}
			const uint32_t score = m_szDemandPriority[i];
			if ( bestRegion < 0 || score > bestScore ) {
				bestRegion = i;
				bestScore = score;
			}
		}
		if ( bestRegion < 0 ) {
			break;
		}
		int		 freeSlot = -1;
		int		 worstSlot = -1;
		uint32_t worstScore = UINT32_MAX;
		for ( int i = 0; i < limit; ++i ) {
			const uint16_t owner = m_szFineSlotOwners[i];
			if ( owner == 0u ) {
				bool stillPublished = false;
				for ( int region = 0; region < T3D_REGION_SLOTS; ++region ) {
					if ( m_szPackedColorSlots[region] == ( uint16_t ) ( i + 1 ) ) {
						stillPublished = true;
						break;
					}
				}
				if ( !stillPublished && m_szFineSlotRetireTick[i] <= m_nDemandTick && freeSlot < 0 ) {
					freeSlot = i;
				}
				continue;
			}
			const uint32_t score = m_szDemandPriority[owner - 1u];
			if ( score < worstScore ) {
				worstScore = score;
				worstSlot = i;
			}
		}
		if ( freeSlot >= 0 ) {
			m_szFineSlotOwners[freeSlot] = ( uint16_t ) ( bestRegion + 1 );
			m_szDesiredColorSlots[bestRegion] = ( uint16_t ) ( freeSlot + 1 );
			++m_nFineAdmissions;
			m_colorDirtyRegions.set( ( size_t ) bestRegion );
			m_bColorDirty = true;
			continue;
		}
		// Eviction publishes a zero fine slot before the physical texels are
		// reused. The permanent coarse level covers the retirement interval.
		if ( worstSlot >= 0 && bestScore > worstScore + 128u ) {
			const uint32_t oldRegion = m_szFineSlotOwners[worstSlot] - 1u;
			m_szFineSlotOwners[worstSlot] = 0u;
			m_szDesiredColorSlots[oldRegion] = 0u;
			++m_nFineEvictions;
			m_szFineSlotRetireTick[worstSlot] = m_nDemandTick + 4u;
			m_bColorDirty = true;
		}
		break;
	}
}

// Categorical far-alpha (coverage/material) is sampled at cell centers, never
// averaged as though it were linear color. Encoded values remain a valid input
// for the existing far-surface shader without changing VT V3 or region images.
bool Terrain3DFarSurfaceAtlasCache::writeCoarseRegion(
	const PackedByteArray &rFine,
	PackedByteArray		  &rAtlas,
	const int			   nAtlasWidth,
	const int			   nX,
	const int			   nY ) const {
	const int samples = m_config.m_nSurfaceAtlasSide / T3D_REGION_MAP_SIZE;
	if ( samples < 16 || rFine.size() != ( int64_t ) samples * samples * 4 || nX < 0 || nY < 0 ||
		 nX + 16 > nAtlasWidth || ( int64_t ) ( nY + 16 ) * nAtlasWidth * 4 > rAtlas.size() ) {
		return false;
	}
	const uint8_t *const src = rFine.ptr();
	uint8_t *const		 dst = rAtlas.ptrw();
	for ( int y = 0; y < 16; ++y ) {
		for ( int x = 0; x < 16; ++x ) {
			const int sx = ( ( x * 2 + 1 ) * samples ) / 32;
			const int sy = ( ( y * 2 + 1 ) * samples ) / 32;
			memcpy( dst + ( ( size_t ) ( nY + y ) * nAtlasWidth + nX + x ) * 4u,
					src + ( ( size_t ) sy * samples + sx ) * 4u,
					4u );
		}
	}
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::copyCookedColorRegion(
	const Vector2i	&rLocation,
	PackedByteArray &rBytes,
	const int		 nSide,
	const int		 nX,
	const int		 nY ) const {
	if ( m_colorImage.is_null() || m_colorImage->get_width() != m_config.m_nSurfaceAtlasSide )
		return false;
	const int samples = m_config.m_nSurfaceAtlasSide / T3D_REGION_MAP_SIZE;
	const int src_x = ( rLocation.x + T3D_REGION_MAP_HALF ) * samples;
	const int src_y = ( rLocation.y + T3D_REGION_MAP_HALF ) * samples;
	if ( src_x < 0 || src_y < 0 || src_x + samples > m_colorImage->get_width() ||
		 src_y + samples > m_colorImage->get_height() || nX < 0 || nY < 0 || nX + samples > nSide ||
		 nY + samples > nSide )
		return false;
	const PackedByteArray source = m_colorImage->get_data();
	if ( source.size() != ( int64_t ) ( m_config.m_nSurfaceAtlasSide ) * m_config.m_nSurfaceAtlasSide * 4 ||
		 rBytes.size() < ( int64_t ) nSide * nSide * 4 )
		return false;
	const uint8_t *const read = source.ptr();
	uint8_t *const		 write = rBytes.ptrw();
	for ( int y = 0; y < samples; ++y ) {
		const size_t from = ( ( size_t ) ( src_y + y ) * m_config.m_nSurfaceAtlasSide + src_x ) * 4u;
		const size_t to = ( ( size_t ) ( nY + y ) * nSide + nX ) * 4u;
		memcpy( write + to, read + from, ( size_t ) samples * 4u );
	}
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::patchPackedColorRegion(
	const Vector2i	&rLocation,
	PackedByteArray &rBytes,
	const int		 nSide,
	const int		 nX,
	const int		 nY ) {
	if ( m_colorImage.is_valid() && !m_bColorCookDirty && copyCookedColorRegion( rLocation, rBytes, nSide, nX, nY ) )
		return true;
	return patchColorRegionAt( rLocation, rBytes, nSide, nX, nY );
}

// A sparse far-color atlas is a compact collection of authoritative region
// samples. The native Forward+ descriptor remains sampler2D binding 19;
// the existing region SSBO carries its physical slot without extra bindings.
bool Terrain3DFarSurfaceAtlasCache::rebuildPackedColorAtlas(
	void ) {
	if ( m_nPackedColorAtlasSide <= 0 || m_nPackedColorAtlasHeight <= 0 ) {
		return false;
	}
	Time *const	   pTime = Time::get_singleton();
	const uint64_t startTime = pTime ? pTime->get_ticks_usec() : 0u;
	const int	   samples = m_config.m_nSurfaceAtlasSide / T3D_REGION_MAP_SIZE;
	const int	   width = m_nPackedColorAtlasSide;
	const int	   height = m_nPackedColorAtlasHeight;
	const int	   columns = MAX( 1, width / samples );
	const int	   coarseY = height - 512;
	const bool	   full = m_stats.m_bColorDirtyAll || !m_generatedColorMap.getRid().is_valid();
	uint64_t	   rebuilt = 0u;
	uint64_t	   uploaded = 0u;
	uint32_t	   regions = 0u;
	if ( full ) {
		PackedByteArray bytes;
		bytes.resize( ( int64_t ) width * height * 4 );
		t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty( bytes );
		for ( int i = 0; i < m_pRepository->getSlotCount(); ++i ) {
			const Vector2i location = t3dUnpackRegionCoord( m_pRepository->getSlotsData()[i] );
			if ( location == s_v2iMax || !m_pRepository->isLocationValid( location ) ) {
				continue;
			}
			const int index = Terrain3DRegionRepository::getRegionMapIndex( location );
			if ( index < 0 ) {
				continue;
			}
			PackedByteArray fine;
			fine.resize( ( int64_t ) samples * samples * 4 );
			if ( !patchPackedColorRegion( location, fine, samples, 0, 0 ) ) {
				return false;
			}
			if ( !writeCoarseRegion( fine,
									 bytes,
									 width,
									 ( index % T3D_REGION_MAP_SIZE ) * 16,
									 coarseY + ( index / T3D_REGION_MAP_SIZE ) * 16 ) ) {
				return false;
			}
			const int slot = ( int ) m_szDesiredColorSlots[index] - 1;
			if ( slot >= 0 ) {
				const int	   dstX = ( slot % columns ) * samples;
				const int	   dstY = ( slot / columns ) * samples;
				uint8_t *const dst = bytes.ptrw();
				for ( int y = 0; y < samples; ++y ) {
					memcpy( dst + ( ( size_t ) ( dstY + y ) * width + dstX ) * 4u,
							fine.ptr() + ( size_t ) y * samples * 4u,
							( size_t ) samples * 4u );
				}
			}
			++regions;
		}
		const Ref< Image > image = Image::create_from_data( width, height, false, Image::FORMAT_RGBA8, bytes );
		if ( image.is_null() || image->is_empty() ||
			 !m_generatedColorMap.create( image, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid() ) {
			return false;
		}
		// Reserve both fixed-size staging RIDs during cold publication. If the
		// scratch budget is temporarily exhausted, keep the valid atlas and
		// retry a future partial upload instead of recreating the atlas per tick.
		m_generatedColorMap.primeRegionStaging( 16, 16, Image::FORMAT_RGBA8 );
		m_generatedColorMap.primeRegionStaging( samples, samples, Image::FORMAT_RGBA8 );
		rebuilt = ( uint64_t ) bytes.size();
		uploaded = rebuilt;
	} else {
		for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
			if ( !m_colorDirtyRegions.test( ( size_t ) i ) ) {
				continue;
			}
			const Vector2i location( ( i % T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF,
									 ( i / T3D_REGION_MAP_SIZE ) - T3D_REGION_MAP_HALF );
			if ( !m_pRepository->getRegionPtr( location ) ) {
				continue;
			}
			PackedByteArray fine;
			fine.resize( ( int64_t ) samples * samples * 4 );
			if ( !patchPackedColorRegion( location, fine, samples, 0, 0 ) ) {
				return false;
			}
			PackedByteArray coarse;
			coarse.resize( 16 * 16 * 4 );
			if ( !writeCoarseRegion( fine, coarse, 16, 0, 0 ) ) {
				return false;
			}
			const Ref< Image > coarseImage = Image::create_from_data( 16, 16, false, Image::FORMAT_RGBA8, coarse );
			const Rect2i	   coarseDest( ( i % T3D_REGION_MAP_SIZE ) * 16,
									   coarseY + ( i / T3D_REGION_MAP_SIZE ) * 16,
									   16,
									   16 );
			if ( coarseImage.is_null() || !m_generatedColorMap.updateRegion2d( coarseImage, coarseDest ) ) {
				return false;
			}
			rebuilt += ( uint64_t ) fine.size() + ( uint64_t ) coarse.size();
			uploaded += ( uint64_t ) coarse.size();
			const int slot = ( int ) m_szDesiredColorSlots[i] - 1;
			if ( slot >= 0 ) {
				const Ref< Image > fineImage =
					Image::create_from_data( samples, samples, false, Image::FORMAT_RGBA8, fine );
				const Rect2i fineDest( ( slot % columns ) * samples, ( slot / columns ) * samples, samples, samples );
				if ( fineImage.is_null() || !m_generatedColorMap.updateRegion2d( fineImage, fineDest ) ) {
					return false;
				}
				uploaded += ( uint64_t ) fine.size();
			}
			++regions;
		}
	}
	// Publication is transactional with respect to the shader's page mapping.
	// A failed texture update never advertises the pending physical slot.
	if ( memcmp( m_szPackedColorSlots, m_szDesiredColorSlots, sizeof( m_szPackedColorSlots ) ) != 0 ) {
		memcpy( m_szPackedColorSlots, m_szDesiredColorSlots, sizeof( m_szPackedColorSlots ) );
		++m_nPackedColorLayoutGeneration;
	}
	m_nPackedColorCount = 0u;
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		m_nPackedColorCount += m_szPackedColorSlots[i] != 0u ? 1u : 0u;
	}
	m_stats.m_nColorRegionsLastTick = ( int ) regions;
	m_stats.m_nColorDirtyTexelsLast = rebuilt / 4u;
	m_stats.m_nColorRebuiltBytesLast = rebuilt;
	m_stats.m_nColorUploadedBytesLast = uploaded;
	m_stats.m_nColorUploadedBytesTotal += uploaded;
	m_stats.m_nColorProcessingUsecLast = pTime ? pTime->get_ticks_usec() - startTime : 0u;
	m_stats.m_nColorPublicationUsecLast = m_stats.m_nColorProcessingUsecLast;
	++m_stats.m_nColorPublicationCount;
	++m_stats.m_nColorUpdatesLastTick;
	m_bColorDirty = false;
	m_stats.m_bColorDirtyAll = false;
	m_colorDirtyRegions.reset();
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::rebuildColorAtlas(
	void ) {
	if ( !m_config.m_bRequired ) {
		return false;
	}
	if ( !rebuildPackedColorAtlas() ) {
		return false;
	}
	m_bColorDirty = false;
	m_stats.m_bColorDirtyAll = false;
	m_colorDirtyRegions.reset();
	return true;
}

// Bring the CPU atlases up to date for an on-disk cook. Runs whether or not the
// atlases are needed by the current render tiering, performs no GPU work, and leaves
// the dirty set intact so a later ensure() still publishes to the device.
bool Terrain3DFarSurfaceAtlasCache::cookAtlases(
	void ) {
	if ( !m_pRepository || T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 ) {
		return false;
	}
	if ( ( m_bHeightDirty || m_heightImage.is_null() ) && !rebuildHeightImage() ) {
		return false;
	}
	if ( ( m_bColorCookDirty || m_colorImage.is_null() ) && !rebuildColorImage() ) {
		return false;
	}
	return m_heightImage.is_valid() && m_colorImage.is_valid();
}

bool Terrain3DFarSurfaceAtlasCache::ensure(
	void ) {
	if ( !m_config.m_bRequired || !m_pRepository || !m_pSamplingStorage || !m_pSamplingStorage->isActiveOwner() ||
		 T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 ) {
		return false;
	}
	// Sparse physical slot assignments are a topology publication, not a map-edit
	// publication. A stationary camera, even while painting, pays no full-world
	// repack or texture recreation unless logical region occupancy changes.
	const uint64_t topology = m_pRepository->getTopologyGeneration();
	if ( m_nPackedColorAtlasSide == 0 || topology != m_nPackedTopologyGeneration ) {
		if ( !refreshPackedColorLayout() )
			return false;
		m_nPackedTopologyGeneration = topology;
	}
	serviceFineResidency();
	// Runtime GPU images are authoritative once published. A clean far atlas need
	// not retain or reload its 4096x4096 RGBA8 CPU backing every frame.
	const bool height_gpu_missing = !m_generatedHeightMap.getRid().is_valid();
	const bool color_gpu_missing = !m_generatedColorMap.getRid().is_valid();
	// A valid cooked package is an initial cold-start source. Changing the
	// physical slot topology later must not reload/revalidate a 64 MiB atlas
	// from disk on the edit frame; rebuild from authoritative region channels.
	if ( m_stats.m_nColorPublicationCount == 0u &&
		 ( ( height_gpu_missing && m_heightImage.is_null() ) || ( color_gpu_missing && m_colorImage.is_null() ) ) &&
		 !loadCooked() ) {
		if ( m_config.m_bStrictCooked ) {
			return false;
		}
		markAllDirty( REGION_TYPE_MAX );
	}
	// If a previously cold image receives a runtime edit, construct it from all
	// active source regions. Partial atlas patching requires a valid CPU backing.
	if ( m_bHeightDirty && m_heightImage.is_null() ) {
		m_stats.m_bHeightDirtyAll = true;
	}
	// Compact physical slots are rebuilt only when region topology changes.
	if ( ( m_bHeightDirty || height_gpu_missing ) && !rebuildHeightAtlas() ) {
		return false;
	}
	if ( ( m_bColorDirty || color_gpu_missing ) && !rebuildColorAtlas() ) {
		return false;
	}
	if ( isReady() ) {
		// Packed-color GPU updates use bounded region tiles, not the original
		// full-world authoring atlas. Release its 4096x4096 CPU backing even in
		// the editor; explicit disk cooking reconstructs it when requested.
		m_colorImage.unref();
		if ( !IS_EDITOR ) {
			m_heightImage.unref();
		}
	}
	return isReady();
}

bool Terrain3DFarSurfaceAtlasCache::isRegionHeightDirty(
	const Vector2i &vLocation ) const {
	if ( m_stats.m_bHeightDirtyAll ) {
		return true;
	}
	const int index = m_pRepository && m_pRepository->isLocationValid( vLocation )
						  ? Terrain3DRegionRepository::getRegionMapIndex( vLocation )
						  : -1;
	return index >= 0 && m_heightDirtyRegions.test( ( size_t ) ( index ) );
}

bool Terrain3DFarSurfaceAtlasCache::isRegionColorDirty(
	const Vector2i &vLocation ) const {
	if ( m_stats.m_bColorDirtyAll ) {
		return true;
	}
	const int index = m_pRepository && m_pRepository->isLocationValid( vLocation )
						  ? Terrain3DRegionRepository::getRegionMapIndex( vLocation )
						  : -1;
	return index >= 0 && m_colorDirtyRegions.test( ( size_t ) ( index ) );
}

int Terrain3DFarSurfaceAtlasCache::getDirtyRegionCount(
	void ) const {
	if ( !m_pRepository ) {
		return 0;
	}
	return ( m_stats.m_bHeightDirtyAll || m_stats.m_bColorDirtyAll )
			   ? m_pRepository->getActiveRegionCount()
			   : ( int ) ( ( m_heightDirtyRegions | m_colorDirtyRegions ).count() );
}

size_t Terrain3DFarSurfaceAtlasCache::getImageBytes(
	void ) const {
	size_t bytes = 0u;
	if ( m_heightImage.is_valid() ) {
		bytes += ( size_t ) ( m_heightImage->get_data().size() );
	}
	if ( m_colorImage.is_valid() ) {
		bytes += ( size_t ) ( m_colorImage->get_data().size() );
	}
	return bytes;
}

t3dFarSurfaceAtlasStats_t Terrain3DFarSurfaceAtlasCache::getStats(
	void ) const {
	t3dFarSurfaceAtlasStats_t stats;
	stats.m_nHeightUpdatesLastTick = m_stats.m_nHeightUpdatesLastTick;
	stats.m_nColorUpdatesLastTick = m_stats.m_nColorUpdatesLastTick;
	stats.m_nHeightRegionsLastTick = m_stats.m_nHeightRegionsLastTick;
	stats.m_nColorRegionsLastTick = m_stats.m_nColorRegionsLastTick;
	stats.m_nHeightDirtyRegions = m_stats.m_bHeightDirtyAll && m_pRepository ? m_pRepository->getActiveRegionCount()
																			 : ( int ) ( m_heightDirtyRegions.count() );
	stats.m_nColorDirtyRegions = m_stats.m_bColorDirtyAll && m_pRepository ? m_pRepository->getActiveRegionCount()
																		   : ( int ) ( m_colorDirtyRegions.count() );
	stats.m_nHeightProcessingUsecLast = m_stats.m_nHeightProcessingUsecLast;
	stats.m_nColorProcessingUsecLast = m_stats.m_nColorProcessingUsecLast;
	stats.m_nHeightDirtyTexelsLast = m_stats.m_nHeightDirtyTexelsLast;
	stats.m_nColorDirtyTexelsLast = m_stats.m_nColorDirtyTexelsLast;
	stats.m_nHeightRebuiltBytesLast = m_stats.m_nHeightRebuiltBytesLast;
	stats.m_nColorRebuiltBytesLast = m_stats.m_nColorRebuiltBytesLast;
	stats.m_nHeightUploadedBytesLast = m_stats.m_nHeightUploadedBytesLast;
	stats.m_nColorUploadedBytesLast = m_stats.m_nColorUploadedBytesLast;
	stats.m_nHeightPublicationUsecLast = m_stats.m_nHeightPublicationUsecLast;
	stats.m_nColorPublicationUsecLast = m_stats.m_nColorPublicationUsecLast;
	stats.m_nHeightPublicationCount = m_stats.m_nHeightPublicationCount;
	stats.m_nColorPublicationCount = m_stats.m_nColorPublicationCount;
	stats.m_nHeightUploadedBytesTotal = m_stats.m_nHeightUploadedBytesTotal;
	stats.m_nColorUploadedBytesTotal = m_stats.m_nColorUploadedBytesTotal;
	stats.m_nCookIoUsecLast = m_stats.m_nCookIoUsecLast;
	stats.m_bHeightDirtyAll = m_stats.m_bHeightDirtyAll;
	stats.m_bColorDirtyAll = m_stats.m_bColorDirtyAll;
	stats.m_bCookedLoaded = m_stats.m_bCookedLoaded;
	stats.m_bCookedSaved = m_stats.m_bCookedSaved;
	return stats;
}

Dictionary Terrain3DFarSurfaceAtlasCache::getStatsDictionary(
	void ) const {
	const t3dFarSurfaceAtlasStats_t values = getStats();
	Dictionary						stats;
	stats["far_height_updates_last_tick"] = values.m_nHeightUpdatesLastTick;
	stats["far_color_updates_last_tick"] = values.m_nColorUpdatesLastTick;
	stats["far_height_regions_processed_last_tick"] = values.m_nHeightRegionsLastTick;
	stats["far_color_regions_processed_last_tick"] = values.m_nColorRegionsLastTick;
	stats["far_height_dirty_regions"] = values.m_nHeightDirtyRegions;
	stats["far_color_dirty_regions"] = values.m_nColorDirtyRegions;
	stats["far_height_processing_usec_last"] = ( int64_t ) ( values.m_nHeightProcessingUsecLast );
	stats["far_color_processing_usec_last"] = ( int64_t ) ( values.m_nColorProcessingUsecLast );
	stats["far_height_dirty_texels_last"] = ( int64_t ) ( values.m_nHeightDirtyTexelsLast );
	stats["far_color_dirty_texels_last"] = ( int64_t ) ( values.m_nColorDirtyTexelsLast );
	stats["far_height_rebuilt_bytes_last"] = ( int64_t ) ( values.m_nHeightRebuiltBytesLast );
	stats["far_color_rebuilt_bytes_last"] = ( int64_t ) ( values.m_nColorRebuiltBytesLast );
	stats["far_height_uploaded_bytes_last"] = ( int64_t ) ( values.m_nHeightUploadedBytesLast );
	stats["far_color_uploaded_bytes_last"] = ( int64_t ) ( values.m_nColorUploadedBytesLast );
	stats["far_height_publication_usec_last"] = ( int64_t ) ( values.m_nHeightPublicationUsecLast );
	stats["far_color_publication_usec_last"] = ( int64_t ) ( values.m_nColorPublicationUsecLast );
	stats["far_height_publication_count"] = ( int64_t ) ( values.m_nHeightPublicationCount );
	stats["far_color_publication_count"] = ( int64_t ) ( values.m_nColorPublicationCount );
	stats["far_height_uploaded_bytes_total"] = ( int64_t ) ( values.m_nHeightUploadedBytesTotal );
	stats["far_color_uploaded_bytes_total"] = ( int64_t ) ( values.m_nColorUploadedBytesTotal );
	stats["far_cook_io_usec_last"] = ( int64_t ) ( values.m_nCookIoUsecLast );
	stats["far_cooked_loaded"] = values.m_bCookedLoaded;
	stats["far_cooked_saved"] = values.m_bCookedSaved;
	stats["far_color_physical_regions"] = ( int ) m_nPackedColorCount;
	stats["far_color_physical_atlas_side"] = m_nPackedColorAtlasSide;
	stats["far_color_physical_atlas_height"] = m_nPackedColorAtlasHeight;
	stats["far_color_fine_page_capacity"] = CLAMP( m_config.m_nFinePageLimit, 16, 64 );
	stats["far_color_coarse_regions"] = m_pRepository ? m_pRepository->getActiveRegionCount() : 0;
	stats["far_color_fine_pending"] = m_bColorDirty;
	stats["far_color_demand_tick"] = ( int64_t ) m_nDemandTick;
	stats["far_color_fine_admissions"] = ( int64_t ) m_nFineAdmissions;
	stats["far_color_fine_evictions"] = ( int64_t ) m_nFineEvictions;
	const uint64_t physicalGpuBytes =
		m_generatedColorMap.getRid().is_valid() ? ( uint64_t ) m_generatedColorMap.payloadBytes() : 0u;
	const uint64_t fullLogicalBytes = ( uint64_t ) m_config.m_nSurfaceAtlasSide * m_config.m_nSurfaceAtlasSide * 4u;
	stats["far_color_physical_gpu_bytes"] = ( int64_t ) physicalGpuBytes;
	stats["far_color_logical_full_gpu_bytes"] = ( int64_t ) fullLogicalBytes;
	stats["far_color_logical_payload_reduction_bytes"] =
		( int64_t ) ( fullLogicalBytes > physicalGpuBytes ? fullLogicalBytes - physicalGpuBytes : 0u );
	stats["far_color_layout_generation"] = ( int64_t ) m_nPackedColorLayoutGeneration;
	return stats;
}
