// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/terrain_3d_far_surface_atlas_cache.h"

#include "assets/terrain_3d_assets.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_runtime_page_format.h"
#include "data/terrain_3d_sampling_storage.h"
#include "eastl_config.h"
#include "rendering/terrain_3d_material.h"
#include "streaming/terrain_3d_cpu_region_cache.h"

#include <EASTL/array.h>
#include <cstring>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>

typedef struct t3dFarSurfaceBakeData_s {
	eastl::array<uint8_t, s_nMaxTextureAssets> m_populated = {};
	int m_nCount = 0;
	int m_nFallbackTexture = 0;
} t3dFarSurfaceBakeData_t;

static void t3dFarSurfaceAtlasCacheFnv1a64Append( uint64_t &nHash, const uint8_t *pData, const size_t nSize ) {
	for ( size_t i = 0; i < nSize; ++i ) {
		nHash ^= ( uint64_t )( pData[i] );
		nHash *= 1099511628211ull;
	}
}

EA_FORCE_INLINE static void t3dFarSurfaceAtlasCacheFnv1a64AppendU32( uint64_t &nHash, const uint32_t nValue ) {
	t3dFarSurfaceAtlasCacheFnv1a64Append( nHash, ( const uint8_t * )( &nValue ), sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dFarSurfaceAtlasCacheFnv1a64AppendU64( uint64_t &nHash, const uint64_t nValue ) {
	t3dFarSurfaceAtlasCacheFnv1a64Append( nHash, ( const uint8_t * )( &nValue ), sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dFarSurfaceAtlasCacheFnv1a64AppendF32( uint64_t &nHash, const float fValue ) {
	uint32_t bits = 0u;
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( nHash, bits );
}

static uint32_t t3dFarSurfaceAtlasCacheReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint16_t t3dFarSurfaceAtlasCacheReadU16Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint16_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

EA_FORCE_INLINE static uint8_t t3dFarSurfaceAtlasCacheColorToByte( const float fValue ) {
	return ( uint8_t )( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

static void t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty( PackedByteArray &rBytes ) {
	uint8_t *pWrite = rBytes.ptrw();
	if ( !pWrite ) {
		return;
	}
	const size_t pixel_count = ( size_t )( rBytes.size() ) / 4u;
	for ( size_t index = 0u; index < pixel_count; ++index ) {
		const size_t offset = index * 4u;
		pWrite[offset] = 255u;
		pWrite[offset + 1u] = 255u;
		pWrite[offset + 2u] = 255u;
		pWrite[offset + 3u] = 0u;
	}
}

static uint8_t
t3dFarSurfaceAtlasCacheFarSurfaceAlpha( const float fSolidCoverage, const float fColorAlpha, const int nMaterialId ) {
	if ( fSolidCoverage < 0.5f ) {
		return 0u;
	}
	const uint32_t material = ( uint32_t )( CLAMP( nMaterialId, 0, 31 ) );
	const uint32_t surface_alpha =
			( uint32_t )( CLAMP( Math::round( CLAMP( fColorAlpha, 0.0f, 1.0f ) * 3.0f ), 0.0f, 3.0f ) );
	return ( uint8_t )( 0x80u | ( material << 2u ) | surface_alpha );
}

static float
t3dFarSurfaceAtlasCacheMaterialFloatParam( Terrain3DMaterial *pMaterial, const char *pszName, const float fDefault ) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? fDefault : ( float )( value );
}

static int t3dFarSurfaceAtlasCacheMaterialIntParam( Terrain3DMaterial *pMaterial, const char *pszName, const int nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int )( value );
}

static void t3dFarSurfaceAtlasCacheBuildBakeData( const Ref<Terrain3DAssets> &rAssets, t3dFarSurfaceBakeData_t &rData ) {
	rData = t3dFarSurfaceBakeData_t();
	if ( rAssets.is_null() ) {
		return;
	}
	rData.m_nCount = MIN( rAssets->getTextureCount(), s_nMaxTextureAssets );
	bool found_fallback = false;
	for ( int index = 0; index < rData.m_nCount; ++index ) {
		const bool populated = rAssets->getTextureAsset( index ).is_valid();
		rData.m_populated[( size_t )( index )] = populated ? 1u : 0u;
		if ( populated && !found_fallback ) {
			rData.m_nFallbackTexture = index;
			found_fallback = true;
		}
	}
}

static int t3dFarSurfaceAtlasCacheResolveMaterialId( const t3dFarSurfaceBakeData_t &rData, const int nTextureId ) {
	if ( nTextureId >= 0 && nTextureId < rData.m_nCount && rData.m_populated[( size_t )( nTextureId )] != 0u ) {
		return nTextureId;
	}
	return rData.m_nFallbackTexture;
}

Terrain3DFarSurfaceAtlasCache::Terrain3DFarSurfaceAtlasCache( Terrain3DRegionRepository *pRepository,
		Terrain3DSamplingStorage *pSamplingStorage,
		Terrain3DCpuRegionCache *pCpuRegionCache ) :
		m_pRepository( pRepository ), m_pSamplingStorage( pSamplingStorage ), m_pCpuRegionCache( pCpuRegionCache ) {
	reset();
}

void Terrain3DFarSurfaceAtlasCache::setConfig( const t3dFarSurfaceAtlasConfig_t &rConfig ) {
	const bool layout_changed = rConfig.m_nHeightAtlasSide != m_config.m_nHeightAtlasSide ||
			rConfig.m_nSurfaceAtlasSide != m_config.m_nSurfaceAtlasSide;
	m_config = rConfig;
	if ( layout_changed ) {
		invalidateLayout();
	}
}

void Terrain3DFarSurfaceAtlasCache::reset( void ) {
	m_generatedHeightMap.clear();
	m_generatedColorMap.clear();
	m_heightImage.unref();
	m_colorImage.unref();
	m_heightDirtyRegions.reset();
	m_colorDirtyRegions.reset();
	m_bHeightDirty = true;
	m_bColorDirty = true;
	m_bHeightDirtyAll = true;
	m_bColorDirtyAll = true;
	m_bCookedLoaded = false;
	m_bCookedSaved = false;
	m_nHeightUpdatesLastTick = 0;
	m_nColorUpdatesLastTick = 0;
	m_nHeightRegionsLastTick = 0;
	m_nColorRegionsLastTick = 0;
	m_nHeightProcessingUsecLast = 0u;
	m_nColorProcessingUsecLast = 0u;
	m_nHeightDirtyTexelsLast = 0u;
	m_nColorDirtyTexelsLast = 0u;
	m_nHeightRebuiltBytesLast = 0u;
	m_nColorRebuiltBytesLast = 0u;
	m_nHeightUploadedBytesLast = 0u;
	m_nColorUploadedBytesLast = 0u;
	m_nHeightPublicationUsecLast = 0u;
	m_nColorPublicationUsecLast = 0u;
	m_nHeightPublicationCount = 0u;
	m_nColorPublicationCount = 0u;
	m_nHeightUploadedBytesTotal = 0u;
	m_nColorUploadedBytesTotal = 0u;
	m_nCookIoUsecLast = 0u;
}

void Terrain3DFarSurfaceAtlasCache::resetFrameStats( void ) {
	m_nHeightUpdatesLastTick = 0;
	m_nColorUpdatesLastTick = 0;
	m_nHeightRegionsLastTick = 0;
	m_nColorRegionsLastTick = 0;
}

void Terrain3DFarSurfaceAtlasCache::invalidateSurface( void ) {
	m_generatedColorMap.clear();
	m_colorImage.unref();
	m_colorDirtyRegions.reset();
	m_bColorDirty = true;
	m_bColorDirtyAll = true;
	m_bCookedLoaded = false;
	m_bCookedSaved = false;
}

void Terrain3DFarSurfaceAtlasCache::invalidateLayout( void ) {
	m_generatedHeightMap.clear();
	m_generatedColorMap.clear();
	m_heightImage.unref();
	m_colorImage.unref();
	m_heightDirtyRegions.reset();
	m_colorDirtyRegions.reset();
	m_bHeightDirty = true;
	m_bColorDirty = true;
	m_bHeightDirtyAll = true;
	m_bColorDirtyAll = true;
	m_bCookedLoaded = false;
	m_bCookedSaved = false;
}

String Terrain3DFarSurfaceAtlasCache::getCookedPath( const String &rDirectory ) const {
	const String directory = !rDirectory.is_empty() ? rDirectory : m_config.m_dataDirectory;
	return directory.is_empty() ? String() : directory.path_join( "terrain3d_far_atlas.t3dfar" );
}

Ref<Image> Terrain3DFarSurfaceAtlasCache::getRegionMapOrDefault( const Terrain3DRegion *pRegion,
		const t3dMapType_t eMapType ) const {
	if ( pRegion && !pRegion->isDeleted() ) {
		const Ref<Image> pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
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
			return Ref<Image>();
	}
}

uint64_t Terrain3DFarSurfaceAtlasCache::computeSourceSignature( void ) const {
	if ( !m_pRepository ) {
		return 0u;
	}
	uint64_t hash = 1469598103934665603ull;
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, s_nGpuFarAtlasVersion );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( m_pRepository->getRegionSize() ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( m_pRepository->getRegionMapSize() ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( m_config.m_nHeightAtlasSide ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( m_config.m_nSurfaceAtlasSide ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendF32( hash, ( float )( m_pRepository->getVertexSpacing() ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( m_pRepository->getActiveRegionCount() ) );
	for ( int map_index = 0; map_index < s_nMaxRegionSlots; ++map_index ) {
		const Vector2i location( map_index % s_nActiveMaxRegionMapSize - s_nActiveMaxRegionMapSize / 2,
				map_index / s_nActiveMaxRegionMapSize - s_nActiveMaxRegionMapSize / 2 );
		Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( map_index ) );
		const String path = pRegion->get_path();
		const uint64_t modified =
				!path.is_empty() && FileAccess::file_exists( path ) ? FileAccess::get_modified_time( path ) : 0u;
		t3dFarSurfaceAtlasCacheFnv1a64AppendU64( hash, modified );
	}
	t3dFarSurfaceBakeData_t bake_data;
	t3dFarSurfaceAtlasCacheBuildBakeData( m_config.m_assets, bake_data );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( bake_data.m_nCount ) );
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( bake_data.m_nFallbackTexture ) );
	for ( int index = 0; index < bake_data.m_nCount; ++index ) {
		t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( index ) );
		t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, ( uint32_t )( bake_data.m_populated[( size_t )( index )] ) );
	}
	Terrain3DMaterial *const pMaterial = m_config.m_material.ptr();
	t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, pMaterial ? 1u : 0u );
	if ( pMaterial ) {
		t3dFarSurfaceAtlasCacheFnv1a64AppendU32( hash, pMaterial->getAutoShaderEnabled() ? 1u : 0u );
		t3dFarSurfaceAtlasCacheFnv1a64AppendF32(
				hash, t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_slope", 1.0f ) );
		t3dFarSurfaceAtlasCacheFnv1a64AppendF32(
				hash, t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f ) );
		t3dFarSurfaceAtlasCacheFnv1a64AppendU32(
				hash, ( uint32_t )( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_base_texture", 0 ) ) );
		t3dFarSurfaceAtlasCacheFnv1a64AppendU32(
				hash, ( uint32_t )( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ) ) );
	}
	return hash;
}

bool Terrain3DFarSurfaceAtlasCache::initializeImages( void ) {
	const int height_side = MAX( 1, m_config.m_nHeightAtlasSide );
	const int surface_side = MAX( 1, m_config.m_nSurfaceAtlasSide );
	if ( m_heightImage.is_null() || m_heightImage->get_width() != height_side ||
			m_heightImage->get_height() != height_side || m_heightImage->get_format() != Image::FORMAT_R16 ) {
		const uint16_t default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
		PackedByteArray bytes;
		bytes.resize( ( int64_t )( height_side )*height_side * 2 );
		uint8_t *const pWrite = bytes.ptrw();
		for ( int index = 0; index < height_side * height_side; ++index ) {
			std::memcpy( pWrite + ( size_t )( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		m_heightImage = Image::create_from_data( height_side, height_side, false, Image::FORMAT_R16, bytes );
	}
	if ( m_colorImage.is_null() || m_colorImage->get_width() != surface_side ||
			m_colorImage->get_height() != surface_side || m_colorImage->get_format() != Image::FORMAT_RGBA8 ) {
		PackedByteArray bytes;
		bytes.resize( ( int64_t )( surface_side )*surface_side * 4 );
		t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty( bytes );
		m_colorImage = Image::create_from_data( surface_side, surface_side, false, Image::FORMAT_RGBA8, bytes );
	}
	return m_heightImage.is_valid() && !m_heightImage->is_empty() && m_colorImage.is_valid() &&
			!m_colorImage->is_empty();
}

bool Terrain3DFarSurfaceAtlasCache::loadCooked( void ) {
	if ( !m_pRepository ) {
		return false;
	}
	const String path = getCookedPath();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < s_nGpuFarAtlasHeaderBytes ) {
		return false;
	}
	const uint64_t magic = pFile->get_64();
	const uint32_t version = pFile->get_32();
	const uint32_t region_size = pFile->get_32();
	const uint32_t region_map_size = pFile->get_32();
	const uint32_t height_atlas_size = pFile->get_32();
	const uint32_t surface_atlas_size = pFile->get_32();
	const uint32_t height_size = pFile->get_32();
	const uint32_t color_size = pFile->get_32();
	const uint32_t height_checksum = pFile->get_32();
	const uint32_t color_checksum = pFile->get_32();
	const uint64_t source_signature = pFile->get_64();
	const uint32_t expected_height_size = ( uint32_t )( m_config.m_nHeightAtlasSide * m_config.m_nHeightAtlasSide * 2 );
	const uint32_t expected_color_size =
			( uint32_t )( m_config.m_nSurfaceAtlasSide * m_config.m_nSurfaceAtlasSide * 4 );
	if ( magic != s_nGpuFarAtlasMagic || version != s_nGpuFarAtlasVersion ||
			region_size != ( uint32_t )( m_pRepository->getRegionSize() ) ||
			region_map_size != ( uint32_t )( m_pRepository->getRegionMapSize() ) ||
			height_atlas_size != ( uint32_t )( m_config.m_nHeightAtlasSide ) ||
			surface_atlas_size != ( uint32_t )( m_config.m_nSurfaceAtlasSide ) || height_size != expected_height_size ||
			color_size != expected_color_size ||
			pFile->get_length() !=
					s_nGpuFarAtlasHeaderBytes + ( uint64_t )( height_size ) + ( uint64_t )( color_size ) ) {
		return false;
	}
	if ( source_signature == 0u || ( !m_config.m_bStrictCooked && source_signature != computeSourceSignature() ) ) {
		return false;
	}
	const PackedByteArray height_bytes = pFile->get_buffer( height_size );
	const PackedByteArray color_bytes = pFile->get_buffer( color_size );
	if ( height_bytes.size() != height_size || color_bytes.size() != color_size ||
			t3dRuntimePageChecksum( height_bytes.ptr(), ( size_t )( height_bytes.size() ) ) != height_checksum ||
			t3dRuntimePageChecksum( color_bytes.ptr(), ( size_t )( color_bytes.size() ) ) != color_checksum ) {
		return false;
	}
	m_heightImage = Image::create_from_data(
			m_config.m_nHeightAtlasSide, m_config.m_nHeightAtlasSide, false, Image::FORMAT_R16, height_bytes );
	m_colorImage = Image::create_from_data(
			m_config.m_nSurfaceAtlasSide, m_config.m_nSurfaceAtlasSide, false, Image::FORMAT_RGBA8, color_bytes );
	if ( m_heightImage.is_null() || m_colorImage.is_null() ) {
		return false;
	}
	m_bHeightDirty = false;
	m_bColorDirty = false;
	m_bHeightDirtyAll = false;
	m_bColorDirtyAll = false;
	m_heightDirtyRegions.reset();
	m_colorDirtyRegions.reset();
	m_bCookedLoaded = true;
	m_bCookedSaved = false;
	m_nCookIoUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::writeCooked( const String &rDirectory ) {
	if ( !m_pRepository ) {
		return false;
	}
	const String path = getCookedPath( rDirectory );
	if ( path.is_empty() ) {
		return false;
	}
	if ( !m_bHeightDirty && !m_bColorDirty && ( m_bCookedLoaded || m_bCookedSaved ) &&
			FileAccess::file_exists( path ) ) {
		return true;
	}
	if ( !initializeImages() || ( m_bHeightDirty && !rebuildHeightAtlas() ) ||
			( m_bColorDirty && !rebuildColorAtlas() ) ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	const PackedByteArray height_bytes = m_heightImage->get_data();
	const PackedByteArray color_bytes = m_colorImage->get_data();
	const String temporary_path = path + String( ".tmp" );
	const String backup_path = path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		return false;
	}
	pFile->store_64( s_nGpuFarAtlasMagic );
	pFile->store_32( s_nGpuFarAtlasVersion );
	pFile->store_32( ( uint32_t )( m_pRepository->getRegionSize() ) );
	pFile->store_32( ( uint32_t )( m_pRepository->getRegionMapSize() ) );
	pFile->store_32( ( uint32_t )( m_config.m_nHeightAtlasSide ) );
	pFile->store_32( ( uint32_t )( m_config.m_nSurfaceAtlasSide ) );
	pFile->store_32( ( uint32_t )( height_bytes.size() ) );
	pFile->store_32( ( uint32_t )( color_bytes.size() ) );
	pFile->store_32( t3dRuntimePageChecksum( height_bytes.ptr(), ( size_t )( height_bytes.size() ) ) );
	pFile->store_32( t3dRuntimePageChecksum( color_bytes.ptr(), ( size_t )( color_bytes.size() ) ) );
	pFile->store_64( computeSourceSignature() );
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
	m_bCookedSaved = true;
	m_bCookedLoaded = false;
	if ( IS_EDITOR && path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( path );
		}
	}
	m_nCookIoUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	return true;
}

void Terrain3DFarSurfaceAtlasCache::markAllDirty( const t3dMapType_t eMapType ) {
	if ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX ) {
		m_bHeightDirty = true;
		m_bHeightDirtyAll = true;
		m_heightDirtyRegions.reset();
	}
	if ( eMapType == TYPE_CONTROL || eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) {
		m_bColorDirty = true;
		m_bColorDirtyAll = true;
		m_colorDirtyRegions.reset();
	}
	m_bCookedLoaded = false;
	m_bCookedSaved = false;
}

void Terrain3DFarSurfaceAtlasCache::markRegionDirty( const Vector2i &vLocation, const t3dMapType_t eMapType ) {
	if ( !m_pRepository || !m_pRepository->isLocationValid( vLocation ) ) {
		return;
	}
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return;
	}
	if ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX ) {
		m_bHeightDirty = true;
		if ( !m_bHeightDirtyAll ) {
			m_heightDirtyRegions.set( ( size_t )( map_index ) );
		}
	}
	if ( eMapType == TYPE_CONTROL || eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX ) {
		m_bColorDirty = true;
		if ( !m_bColorDirtyAll ) {
			m_colorDirtyRegions.set( ( size_t )( map_index ) );
		}
	}
	m_bCookedLoaded = false;
	m_bCookedSaved = false;
}

bool Terrain3DFarSurfaceAtlasCache::patchHeightRegion( const Vector2i &vLocation, PackedByteArray &rAtlasBytes ) {
	if ( !m_pRepository ) {
		return false;
	}
	const int atlas_size = m_config.m_nHeightAtlasSide;
	if ( m_pRepository->getRegionSize() <= 0 || m_pRepository->getRegionMapSize() <= 0 ||
			atlas_size % m_pRepository->getRegionMapSize() != 0 ) {
		return false;
	}
	const int samples_per_region = atlas_size / m_pRepository->getRegionMapSize();
	if ( samples_per_region <= 0 || m_pRepository->getRegionSize() % samples_per_region != 0 ) {
		return false;
	}
	const int atlas_half = m_pRepository->getRegionMapSize() / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( atlas_origin_x < 0 || atlas_origin_y < 0 || atlas_origin_x + samples_per_region > atlas_size ||
			atlas_origin_y + samples_per_region > atlas_size ) {
		return false;
	}
	const uint16_t default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
	const uint16_t *height = nullptr;
	PackedByteArray region_height_bytes;
	Terrain3DRegion *const pRegion = m_pRepository->getRegionPtr( vLocation );
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
			const Ref<Image> pHeight = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
			if ( pHeight.is_valid() && pHeight->get_format() == Image::FORMAT_R16 ) {
				region_height_bytes = pHeight->get_data();
				if ( region_height_bytes.size() >=
						( int64_t )( m_pRepository->getRegionSize() ) * m_pRepository->getRegionSize() * 2 ) {
					height = ( const uint16_t * )( region_height_bytes.ptr() );
				}
			}
		}
	}
	const int source_step = m_pRepository->getRegionSize() / samples_per_region;
	uint8_t *const pWrite = rAtlasBytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int y = 0; y < samples_per_region; ++y ) {
		const int source_y = MIN( m_pRepository->getRegionSize() - 1, y * source_step );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int source_x = MIN( m_pRepository->getRegionSize() - 1, x * source_step );
			const uint16_t encoded =
					height ? height[source_y * m_pRepository->getRegionSize() + source_x] : default_height;
			const int atlas_index = ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x;
			std::memcpy( pWrite + ( size_t )( atlas_index ) * 2u, &encoded, sizeof( encoded ) );
		}
	}
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::patchColorRegion( const Vector2i &vLocation, PackedByteArray &rAtlasBytes ) {
	if ( !m_pRepository ) {
		return false;
	}
	const int atlas_size = m_config.m_nSurfaceAtlasSide;
	if ( m_pRepository->getRegionSize() <= 0 || m_pRepository->getRegionMapSize() <= 0 ||
			atlas_size % m_pRepository->getRegionMapSize() != 0 ) {
		return false;
	}
	const int samples_per_region = atlas_size / m_pRepository->getRegionMapSize();
	const int atlas_half = m_pRepository->getRegionMapSize() / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( samples_per_region <= 0 || atlas_origin_x < 0 || atlas_origin_y < 0 ||
			atlas_origin_x + samples_per_region > atlas_size || atlas_origin_y + samples_per_region > atlas_size ) {
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
						pAtlasWrite + ( size_t )( ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x ) * 4u;
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
	const Ref<Image> pSourceColor = getRegionMapOrDefault( pRegion, TYPE_COLOR );
	const Ref<Image> pSourceControl = getRegionMapOrDefault( pRegion, TYPE_CONTROL );
	const Ref<Image> pSourceHeight = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
	const Ref<Image> pSourceDerived = pRegion->getDerivedNormalSlopeMap();
	if ( pSourceColor.is_null() || pSourceControl.is_null() || pSourceColor->get_format() != Image::FORMAT_RGBA8 ||
			pSourceControl->get_format() != Image::FORMAT_RF ) {
		return false;
	}
	const PackedByteArray color_bytes = pSourceColor->get_data();
	const PackedByteArray control_bytes = pSourceControl->get_data();
	const PackedByteArray height_bytes = pSourceHeight.is_valid() ? pSourceHeight->get_data() : PackedByteArray();
	const PackedByteArray derived_bytes = pSourceDerived.is_valid() ? pSourceDerived->get_data() : PackedByteArray();
	const uint8_t *pColorRead = color_bytes.ptr();
	const uint8_t *pControlRead = control_bytes.ptr();
	const uint8_t *pHeightRead = height_bytes.ptr();
	const uint8_t *pDerivedRead = derived_bytes.ptr();
	const int color_width = pSourceColor->get_width();
	const int color_height = pSourceColor->get_height();
	const bool height_valid = pSourceHeight.is_valid() && pSourceHeight->get_format() == Image::FORMAT_R16 &&
			height_bytes.size() >= ( int64_t )( m_pRepository->getRegionSize() ) * m_pRepository->getRegionSize() * 2;
	const int derived_width = pSourceDerived.is_valid() ? pSourceDerived->get_width() : 0;
	const int derived_height = pSourceDerived.is_valid() ? pSourceDerived->get_height() : 0;
	const bool derived_valid = pSourceDerived.is_valid() && pSourceDerived->get_format() == Image::FORMAT_RG8 &&
			derived_width > 0 && derived_height > 0 &&
			derived_bytes.size() >= ( int64_t )( derived_width )*derived_height * 2;
	if ( !pColorRead || !pControlRead || color_width <= 0 || color_height <= 0 ||
			control_bytes.size() <
					( int64_t )( m_pRepository->getRegionSize() ) * m_pRepository->getRegionSize() * 4 ) {
		return false;
	}
	t3dFarSurfaceBakeData_t bake_data;
	t3dFarSurfaceAtlasCacheBuildBakeData( m_config.m_assets, bake_data );
	Terrain3DMaterial *const pMaterial = m_config.m_material.ptr();
	const bool auto_shader_enabled = pMaterial && pMaterial->getAutoShaderEnabled();
	const float auto_slope = t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_slope", 1.0f );
	const float auto_height_reduction =
			t3dFarSurfaceAtlasCacheMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f );
	const int auto_layer_max = MAX( bake_data.m_nCount - 1, 0 );
	const int auto_base_texture =
			CLAMP( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_base_texture", 0 ), 0, auto_layer_max );
	const int auto_overlay_texture =
			CLAMP( t3dFarSurfaceAtlasCacheMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ), 0, auto_layer_max );
	const int source_step = MAX( 1, m_pRepository->getRegionSize() / samples_per_region );

	for ( int y = 0; y < samples_per_region; ++y ) {
		const int region_y0 = y * m_pRepository->getRegionSize() / samples_per_region;
		const int region_y1 = MAX( region_y0 + 1, ( y + 1 ) * m_pRepository->getRegionSize() / samples_per_region );
		const int color_y0 = y * color_height / samples_per_region;
		const int color_y1 = MAX( color_y0 + 1, ( y + 1 ) * color_height / samples_per_region );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int region_x0 = x * m_pRepository->getRegionSize() / samples_per_region;
			const int region_x1 = MAX( region_x0 + 1, ( x + 1 ) * m_pRepository->getRegionSize() / samples_per_region );
			const int color_x0 = x * color_width / samples_per_region;
			const int color_x1 = MAX( color_x0 + 1, ( x + 1 ) * color_width / samples_per_region );
			uint64_t sum_r = 0u;
			uint64_t sum_g = 0u;
			uint64_t sum_b = 0u;
			uint64_t sum_a = 0u;
			int color_samples = 0;
			for ( int color_y = color_y0; color_y < MIN( color_y1, color_height ); ++color_y ) {
				for ( int color_x = color_x0; color_x < MIN( color_x1, color_width ); ++color_x ) {
					const size_t color_offset = ( size_t )( color_y * color_width + color_x ) * 4u;
					sum_r += pColorRead[color_offset];
					sum_g += pColorRead[color_offset + 1u];
					sum_b += pColorRead[color_offset + 2u];
					sum_a += pColorRead[color_offset + 3u];
					++color_samples;
				}
			}
			const int center_x = MIN( m_pRepository->getRegionSize() - 1, region_x0 + ( region_x1 - region_x0 ) / 2 );
			const int center_y = MIN( m_pRepository->getRegionSize() - 1, region_y0 + ( region_y1 - region_y0 ) / 2 );
			const int center_index = center_y * m_pRepository->getRegionSize() + center_x;
			const uint32_t control =
					t3dFarSurfaceAtlasCacheReadU32Unaligned( pControlRead, ( size_t )( center_index ) * 4u );
			float solid_coverage = 0.0f;
			for ( int probe_y = 0; probe_y < 2; ++probe_y ) {
				for ( int probe_x = 0; probe_x < 2; ++probe_x ) {
					const int px = MIN(
							m_pRepository->getRegionSize() - 1, region_x0 + ( ( probe_x * 2 + 1 ) * source_step ) / 4 );
					const int py = MIN(
							m_pRepository->getRegionSize() - 1, region_y0 + ( ( probe_y * 2 + 1 ) * source_step ) / 4 );
					const uint32_t probe_control = t3dFarSurfaceAtlasCacheReadU32Unaligned(
							pControlRead, ( size_t )( py * m_pRepository->getRegionSize() + px ) * 4u );
					solid_coverage += t3dIsHole( probe_control ) ? 0.0f : 0.25f;
				}
			}
			int base_id = ( int )( t3dGetBase( control ) );
			int overlay_id = ( int )( t3dGetOverlay( control ) );
			float blend = ( float )( t3dGetBlend( control ) ) / s_fByteMaxFloat;
			if ( auto_shader_enabled && t3dIsAuto( control ) ) {
				float normal_y = 1.0f;
				float height = 0.0f;
				if ( height_valid ) {
					height = t3dRegionCodecDecodeHeightU16(
							t3dFarSurfaceAtlasCacheReadU16Unaligned( pHeightRead, ( size_t )( center_index ) * 2u ) );
				}
				if ( derived_valid ) {
					const int derived_x =
							MIN( derived_width - 1, center_x * derived_width / m_pRepository->getRegionSize() );
					const int derived_y =
							MIN( derived_height - 1, center_y * derived_height / m_pRepository->getRegionSize() );
					const size_t derived_offset = ( size_t )( derived_y * derived_width + derived_x ) * 2u;
					const float normal_x = ( float )( pDerivedRead[derived_offset] ) / s_fByteMaxFloat * 2.0f - 1.0f;
					const float normal_z =
							( float )( pDerivedRead[derived_offset + 1u] ) / s_fByteMaxFloat * 2.0f - 1.0f;
					normal_y = Math::sqrt( MAX( 1.0f - normal_x * normal_x - normal_z * normal_z, 0.0f ) );
				} else if ( height_valid ) {
					const int sample_x = center_y * m_pRepository->getRegionSize() +
							MIN( m_pRepository->getRegionSize() - 1, center_x + 1 );
					const int sample_z =
							MIN( m_pRepository->getRegionSize() - 1, center_y + 1 ) * m_pRepository->getRegionSize() +
							center_x;
					const float height_x = t3dRegionCodecDecodeHeightU16(
							t3dFarSurfaceAtlasCacheReadU16Unaligned( pHeightRead, ( size_t )( sample_x ) * 2u ) );
					const float height_z = t3dRegionCodecDecodeHeightU16(
							t3dFarSurfaceAtlasCacheReadU16Unaligned( pHeightRead, ( size_t )( sample_z ) * 2u ) );
					const float spacing = MAX( ( float )( m_pRepository->getVertexSpacing() ), 0.0001f );
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
			const int dominant_material = blend >= 0.5f ? overlay_id : base_id;
			const float inv_samples = color_samples > 0 ? 1.0f / ( float )( color_samples ) : 0.0f;
			const size_t atlas_offset = ( size_t )( ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x ) * 4u;
			pAtlasWrite[atlas_offset] =
					t3dFarSurfaceAtlasCacheColorToByte( ( float )( sum_r )*inv_samples / s_fByteMaxFloat );
			pAtlasWrite[atlas_offset + 1u] =
					t3dFarSurfaceAtlasCacheColorToByte( ( float )( sum_g )*inv_samples / s_fByteMaxFloat );
			pAtlasWrite[atlas_offset + 2u] =
					t3dFarSurfaceAtlasCacheColorToByte( ( float )( sum_b )*inv_samples / s_fByteMaxFloat );
			const float color_alpha = color_samples > 0 ? ( float )( sum_a )*inv_samples / s_fByteMaxFloat : 0.5f;
			pAtlasWrite[atlas_offset + 3u] =
					t3dFarSurfaceAtlasCacheFarSurfaceAlpha( solid_coverage, color_alpha, dominant_material );
		}
	}
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::rebuildHeightAtlas( void ) {
	if ( !m_config.m_bRequired || !m_pRepository || !m_pSamplingStorage || !m_pSamplingStorage->isActiveOwner() ||
			!initializeImages() ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	PackedByteArray atlas_bytes = m_heightImage->get_data();
	m_nHeightRegionsLastTick = 0;
	m_nHeightDirtyTexelsLast = 0u;
	m_nHeightRebuiltBytesLast = 0u;
	m_nHeightUploadedBytesLast = 0u;
	m_nHeightPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t )( m_config.m_nHeightAtlasSide ) * m_config.m_nHeightAtlasSide;
	const int samples_per_region =
			m_pRepository->getRegionMapSize() > 0 ? m_config.m_nHeightAtlasSide / m_pRepository->getRegionMapSize() : 0;
	const uint64_t region_texels = samples_per_region > 0 ? ( uint64_t )( samples_per_region )*samples_per_region : 0u;
	if ( m_bHeightDirtyAll ) {
		m_nHeightDirtyTexelsLast = atlas_texels;
		const uint16_t default_height = t3dRegionCodecEncodeHeightU16( 0.0f );
		uint8_t *const pWrite = atlas_bytes.ptrw();
		for ( int index = 0; index < m_config.m_nHeightAtlasSide * m_config.m_nHeightAtlasSide; ++index ) {
			std::memcpy( pWrite + ( size_t )( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			if ( location != s_v2iMax && patchHeightRegion( location, atlas_bytes ) ) {
				++m_nHeightRegionsLastTick;
			}
		}
	} else {
		m_nHeightDirtyTexelsLast = ( uint64_t )( m_heightDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < s_nMaxRegionSlots; ++index ) {
			if ( !m_heightDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location( ( int )( index % s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2,
					( int )( index / s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2 );
			if ( patchHeightRegion( location, atlas_bytes ) ) {
				++m_nHeightRegionsLastTick;
			}
		}
	}
	m_nHeightRebuiltBytesLast = m_bHeightDirtyAll ? ( uint64_t )( atlas_bytes.size() )
												  : ( uint64_t )( m_nHeightRegionsLastTick )*region_texels * 2u;
	m_heightImage->set_data(
			m_config.m_nHeightAtlasSide, m_config.m_nHeightAtlasSide, false, Image::FORMAT_R16, atlas_bytes );
	m_nHeightProcessingUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	const uint64_t publication_started = pTime ? pTime->get_ticks_usec() : 0u;
	bool uploaded = true;
	const bool bFullUpload = m_bHeightDirtyAll || !m_generatedHeightMap.getRid().is_valid();
	if ( bFullUpload ) {
		uploaded = m_generatedHeightMap.getRid().is_valid() ? m_generatedHeightMap.update2d( m_heightImage )
														  : m_generatedHeightMap.create( m_heightImage ).is_valid();
		m_nHeightUploadedBytesLast = uploaded ? ( uint64_t )( atlas_bytes.size() ) : 0u;
	} else {
		const int atlas_half = m_pRepository->getRegionMapSize() / 2;
		for ( size_t index = 0; uploaded && index < s_nMaxRegionSlots; ++index ) {
			if ( !m_heightDirtyRegions.test( index ) ) continue;
			const Vector2i location( ( int )( index % s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2,
					( int )( index / s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2 );
			const Rect2i destination( ( location.x + atlas_half ) * samples_per_region,
					( location.y + atlas_half ) * samples_per_region,
					samples_per_region,
					samples_per_region );
			const Ref<Image> region = m_heightImage->get_region( destination );
			uploaded = region.is_valid() && !region->is_empty() && m_generatedHeightMap.updateRegion2d( region, destination );
			if ( uploaded ) m_nHeightUploadedBytesLast += ( uint64_t )( region->get_data().size() );
		}
	}
	m_nHeightPublicationUsecLast = pTime ? pTime->get_ticks_usec() - publication_started : 0u;
	if ( !uploaded ) {
		return false;
	}
	m_nHeightUploadedBytesTotal += m_nHeightUploadedBytesLast;
	++m_nHeightPublicationCount;
	m_bHeightDirty = false;
	m_bHeightDirtyAll = false;
	m_heightDirtyRegions.reset();
	++m_nHeightUpdatesLastTick;
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::rebuildColorAtlas( void ) {
	if ( !m_config.m_bRequired || !m_pRepository || !initializeImages() ) {
		return false;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	PackedByteArray atlas_bytes = m_colorImage->get_data();
	m_nColorRegionsLastTick = 0;
	m_nColorDirtyTexelsLast = 0u;
	m_nColorRebuiltBytesLast = 0u;
	m_nColorUploadedBytesLast = 0u;
	m_nColorPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t )( m_config.m_nSurfaceAtlasSide ) * m_config.m_nSurfaceAtlasSide;
	const int samples_per_region = m_pRepository->getRegionMapSize() > 0
			? m_config.m_nSurfaceAtlasSide / m_pRepository->getRegionMapSize()
			: 0;
	const uint64_t region_texels = samples_per_region > 0 ? ( uint64_t )( samples_per_region )*samples_per_region : 0u;
	if ( m_bColorDirtyAll ) {
		m_nColorDirtyTexelsLast = atlas_texels;
		t3dFarSurfaceAtlasCacheFillFarSurfaceEmpty( atlas_bytes );
		for ( int slot = 0; slot < m_pRepository->getSlotCount(); ++slot ) {
			const Vector2i location = m_pRepository->getSlotsData()[slot];
			if ( location != s_v2iMax && patchColorRegion( location, atlas_bytes ) ) {
				++m_nColorRegionsLastTick;
			}
		}
	} else {
		m_nColorDirtyTexelsLast = ( uint64_t )( m_colorDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < s_nMaxRegionSlots; ++index ) {
			if ( !m_colorDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location( ( int )( index % s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2,
					( int )( index / s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2 );
			if ( patchColorRegion( location, atlas_bytes ) ) {
				++m_nColorRegionsLastTick;
			}
		}
	}
	m_nColorRebuiltBytesLast = m_bColorDirtyAll ? ( uint64_t )( atlas_bytes.size() )
												: ( uint64_t )( m_nColorRegionsLastTick )*region_texels * 4u;
	m_colorImage->set_data(
			m_config.m_nSurfaceAtlasSide, m_config.m_nSurfaceAtlasSide, false, Image::FORMAT_RGBA8, atlas_bytes );
	m_nColorProcessingUsecLast = pTime ? pTime->get_ticks_usec() - started_usec : 0u;
	const uint64_t publication_started = pTime ? pTime->get_ticks_usec() : 0u;
	bool uploaded = true;
	const bool bFullUpload = m_bColorDirtyAll || !m_generatedColorMap.getRid().is_valid();
	if ( bFullUpload ) {
		uploaded = m_generatedColorMap.getRid().is_valid() ? m_generatedColorMap.update2d( m_colorImage )
														 : m_generatedColorMap.create( m_colorImage ).is_valid();
		m_nColorUploadedBytesLast = uploaded ? ( uint64_t )( atlas_bytes.size() ) : 0u;
	} else {
		const int atlas_half = m_pRepository->getRegionMapSize() / 2;
		for ( size_t index = 0; uploaded && index < s_nMaxRegionSlots; ++index ) {
			if ( !m_colorDirtyRegions.test( index ) ) continue;
			const Vector2i location( ( int )( index % s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2,
					( int )( index / s_nActiveMaxRegionMapSize ) - s_nActiveMaxRegionMapSize / 2 );
			const Rect2i destination( ( location.x + atlas_half ) * samples_per_region,
					( location.y + atlas_half ) * samples_per_region,
					samples_per_region,
					samples_per_region );
			const Ref<Image> region = m_colorImage->get_region( destination );
			uploaded = region.is_valid() && !region->is_empty() && m_generatedColorMap.updateRegion2d( region, destination );
			if ( uploaded ) m_nColorUploadedBytesLast += ( uint64_t )( region->get_data().size() );
		}
	}
	m_nColorPublicationUsecLast = pTime ? pTime->get_ticks_usec() - publication_started : 0u;
	if ( !uploaded ) {
		return false;
	}
	m_nColorUploadedBytesTotal += m_nColorUploadedBytesLast;
	++m_nColorPublicationCount;
	m_bColorDirty = false;
	m_bColorDirtyAll = false;
	m_colorDirtyRegions.reset();
	++m_nColorUpdatesLastTick;
	return true;
}

bool Terrain3DFarSurfaceAtlasCache::ensure( void ) {
	if ( !m_config.m_bRequired || !m_pRepository || !m_pSamplingStorage || !m_pSamplingStorage->isActiveOwner() ||
			m_pRepository->getRegionSize() <= 0 || m_pRepository->getRegionMapSize() <= 0 ) {
		return false;
	}
	if ( ( m_heightImage.is_null() || m_colorImage.is_null() ) && !loadCooked() ) {
		if ( m_config.m_bStrictCooked ) {
			return false;
		}
		markAllDirty( REGION_TYPE_MAX );
	}
	if ( ( m_bHeightDirty || !m_generatedHeightMap.getRid().is_valid() ) && !rebuildHeightAtlas() ) {
		return false;
	}
	if ( ( m_bColorDirty || !m_generatedColorMap.getRid().is_valid() ) && !rebuildColorAtlas() ) {
		return false;
	}
	return isReady();
}

bool Terrain3DFarSurfaceAtlasCache::isRegionHeightDirty( const Vector2i &vLocation ) const {
	if ( m_bHeightDirtyAll ) {
		return true;
	}
	const int index = m_pRepository && m_pRepository->isLocationValid( vLocation )
			? Terrain3DRegionRepository::getRegionMapIndex( vLocation )
			: -1;
	return index >= 0 && m_heightDirtyRegions.test( ( size_t )( index ) );
}

bool Terrain3DFarSurfaceAtlasCache::isRegionColorDirty( const Vector2i &vLocation ) const {
	if ( m_bColorDirtyAll ) {
		return true;
	}
	const int index = m_pRepository && m_pRepository->isLocationValid( vLocation )
			? Terrain3DRegionRepository::getRegionMapIndex( vLocation )
			: -1;
	return index >= 0 && m_colorDirtyRegions.test( ( size_t )( index ) );
}

int Terrain3DFarSurfaceAtlasCache::getDirtyRegionCount( void ) const {
	if ( !m_pRepository ) {
		return 0;
	}
	return ( m_bHeightDirtyAll || m_bColorDirtyAll )
			? m_pRepository->getActiveRegionCount()
			: ( int )( ( m_heightDirtyRegions | m_colorDirtyRegions ).count() );
}

size_t Terrain3DFarSurfaceAtlasCache::getImageBytes( void ) const {
	size_t bytes = 0u;
	if ( m_heightImage.is_valid() ) {
		bytes += ( size_t )( m_heightImage->get_data().size() );
	}
	if ( m_colorImage.is_valid() ) {
		bytes += ( size_t )( m_colorImage->get_data().size() );
	}
	return bytes;
}

t3dFarSurfaceAtlasStats_t Terrain3DFarSurfaceAtlasCache::getStats( void ) const {
	t3dFarSurfaceAtlasStats_t stats;
	stats.m_nHeightUpdatesLastTick = m_nHeightUpdatesLastTick;
	stats.m_nColorUpdatesLastTick = m_nColorUpdatesLastTick;
	stats.m_nHeightRegionsLastTick = m_nHeightRegionsLastTick;
	stats.m_nColorRegionsLastTick = m_nColorRegionsLastTick;
	stats.m_nHeightDirtyRegions = m_bHeightDirtyAll && m_pRepository ? m_pRepository->getActiveRegionCount()
																	 : ( int )( m_heightDirtyRegions.count() );
	stats.m_nColorDirtyRegions = m_bColorDirtyAll && m_pRepository ? m_pRepository->getActiveRegionCount()
																   : ( int )( m_colorDirtyRegions.count() );
	stats.m_nHeightProcessingUsecLast = m_nHeightProcessingUsecLast;
	stats.m_nColorProcessingUsecLast = m_nColorProcessingUsecLast;
	stats.m_nHeightDirtyTexelsLast = m_nHeightDirtyTexelsLast;
	stats.m_nColorDirtyTexelsLast = m_nColorDirtyTexelsLast;
	stats.m_nHeightRebuiltBytesLast = m_nHeightRebuiltBytesLast;
	stats.m_nColorRebuiltBytesLast = m_nColorRebuiltBytesLast;
	stats.m_nHeightUploadedBytesLast = m_nHeightUploadedBytesLast;
	stats.m_nColorUploadedBytesLast = m_nColorUploadedBytesLast;
	stats.m_nHeightPublicationUsecLast = m_nHeightPublicationUsecLast;
	stats.m_nColorPublicationUsecLast = m_nColorPublicationUsecLast;
	stats.m_nHeightPublicationCount = m_nHeightPublicationCount;
	stats.m_nColorPublicationCount = m_nColorPublicationCount;
	stats.m_nHeightUploadedBytesTotal = m_nHeightUploadedBytesTotal;
	stats.m_nColorUploadedBytesTotal = m_nColorUploadedBytesTotal;
	stats.m_nCookIoUsecLast = m_nCookIoUsecLast;
	stats.m_bHeightDirtyAll = m_bHeightDirtyAll;
	stats.m_bColorDirtyAll = m_bColorDirtyAll;
	stats.m_bCookedLoaded = m_bCookedLoaded;
	stats.m_bCookedSaved = m_bCookedSaved;
	return stats;
}

Dictionary Terrain3DFarSurfaceAtlasCache::getStatsDictionary( void ) const {
	const t3dFarSurfaceAtlasStats_t values = getStats();
	Dictionary stats;
	stats["far_height_updates_last_tick"] = values.m_nHeightUpdatesLastTick;
	stats["far_color_updates_last_tick"] = values.m_nColorUpdatesLastTick;
	stats["far_height_regions_processed_last_tick"] = values.m_nHeightRegionsLastTick;
	stats["far_color_regions_processed_last_tick"] = values.m_nColorRegionsLastTick;
	stats["far_height_dirty_regions"] = values.m_nHeightDirtyRegions;
	stats["far_color_dirty_regions"] = values.m_nColorDirtyRegions;
	stats["far_height_processing_usec_last"] = ( int64_t )( values.m_nHeightProcessingUsecLast );
	stats["far_color_processing_usec_last"] = ( int64_t )( values.m_nColorProcessingUsecLast );
	stats["far_height_dirty_texels_last"] = ( int64_t )( values.m_nHeightDirtyTexelsLast );
	stats["far_color_dirty_texels_last"] = ( int64_t )( values.m_nColorDirtyTexelsLast );
	stats["far_height_rebuilt_bytes_last"] = ( int64_t )( values.m_nHeightRebuiltBytesLast );
	stats["far_color_rebuilt_bytes_last"] = ( int64_t )( values.m_nColorRebuiltBytesLast );
	stats["far_height_uploaded_bytes_last"] = ( int64_t )( values.m_nHeightUploadedBytesLast );
	stats["far_color_uploaded_bytes_last"] = ( int64_t )( values.m_nColorUploadedBytesLast );
	stats["far_height_publication_usec_last"] = ( int64_t )( values.m_nHeightPublicationUsecLast );
	stats["far_color_publication_usec_last"] = ( int64_t )( values.m_nColorPublicationUsecLast );
	stats["far_height_publication_count"] = ( int64_t )( values.m_nHeightPublicationCount );
	stats["far_color_publication_count"] = ( int64_t )( values.m_nColorPublicationCount );
	stats["far_height_uploaded_bytes_total"] = ( int64_t )( values.m_nHeightUploadedBytesTotal );
	stats["far_color_uploaded_bytes_total"] = ( int64_t )( values.m_nColorUploadedBytesTotal );
	stats["far_cook_io_usec_last"] = ( int64_t )( values.m_nCookIoUsecLast );
	stats["far_cooked_loaded"] = values.m_bCookedLoaded;
	stats["far_cooked_saved"] = values.m_bCookedSaved;
	return stats;
}
