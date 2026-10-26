#include "core/utility/terrain_3d_binary_io.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/utility/terrain_3d_hash_bytes.h"
#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "rendering/surface/terrain_3d_far_regional_cooker.h"
#include "diagnostics/core/terrain_3d_streaming_profiler.h"
#include "streaming/residency/terrain_3d_raster_admission.h"
#include "streaming/io/terrain_3d_cold_chunk_file.h"
#include "rendering/rd/vt/core/terrain_3d_far_avt_contract.h"
#include "rendering/rd/vt/io/terrain_3d_far_avt_region_file.h"
#include <array>

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

static bool t3dFarRegionalCookFnv1a64AppendFile(
	uint64_t &nHash,
	const String &rPath,
	uint64_t *pReadBytes = NULL ) {
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
		if ( pReadBytes ) {
			*pReadBytes += read;
		}
		remaining -= read;
	}
	return true;
}

static void t3dFarRegionalCookFnv1a64AppendImage(
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

static uint32_t t3dFarRegionalCookReadU32Unaligned(
	const uint8_t *pBytes,
	const size_t   nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		value = t3dReadNativeUnaligned< decltype( value ) >( pBytes + nOffset );
	}
	return value;
}

static uint16_t t3dFarRegionalCookReadU16Unaligned(
	const uint8_t *pBytes,
	const size_t   nOffset ) {
	uint16_t value = 0u;
	if ( pBytes ) {
		value = t3dReadNativeUnaligned< decltype( value ) >( pBytes + nOffset );
	}
	return value;
}

EA_FORCE_INLINE static uint8_t t3dFarRegionalCookColorToByte(
	const float fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

static uint8_t t3dFarRegionalCookFarSurfaceAlpha(
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

static float t3dFarRegionalCookMaterialFloatParam(
	Terrain3DMaterial *pMaterial,
	const char		  *pszName,
	const float		   fDefault ) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? fDefault : ( float ) ( value );
}

static int t3dFarRegionalCookMaterialIntParam(
	Terrain3DMaterial *pMaterial,
	const char		  *pszName,
	const int		   nDefault ) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int ) ( value );
}

static void t3dFarRegionalCookBuildBakeData(
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

static int t3dFarRegionalCookResolveMaterialId(
	const t3dFarSurfaceBakeData_t &rData,
	const int					   nTextureId ) {
	if ( nTextureId >= 0 && nTextureId < rData.m_nCount && rData.m_populated[( size_t ) ( nTextureId )] != 0u ) {
		return nTextureId;
	}
	return rData.m_nFallbackTexture;
}


Terrain3DFarRegionalCooker::Terrain3DFarRegionalCooker(
    Terrain3DRegionRepository *pRepository, Terrain3DSamplingStorage *pSamplingStorage,
    Terrain3DCpuRegionCache *pCpuRegionCache ) :
    m_pRepository( pRepository ), m_pSamplingStorage( pSamplingStorage ),
    m_pCpuRegionCache( pCpuRegionCache )
{
    reset();
}

void Terrain3DFarRegionalCooker::setConfig( const t3dFarRegionalCookConfig_t &rConfig )
{
    const bool layout_changed = rConfig.m_nHeightSourceSide != m_config.m_nHeightSourceSide ||
        rConfig.m_nSurfaceSourceSide != m_config.m_nSurfaceSourceSide;
    const bool source_changed = rConfig.m_defaultHeightMap != m_config.m_defaultHeightMap ||
        rConfig.m_defaultControlMap != m_config.m_defaultControlMap ||
        rConfig.m_defaultColorMap != m_config.m_defaultColorMap ||
        rConfig.m_assets != m_config.m_assets || rConfig.m_material != m_config.m_material ||
        rConfig.m_dataDirectory != m_config.m_dataDirectory;
    m_config = rConfig;
    if ( layout_changed || source_changed ) markAllDirty( REGION_TYPE_MAX );
}

void Terrain3DFarRegionalCooker::reset( void )
{
    m_avtCookDirtyRegions.reset();
    memset( m_nAvtCookedPackageGenerations, 0, sizeof( m_nAvtCookedPackageGenerations ) );
    m_bAvtCookDirtyAll = true;
    m_nAvtPublicationRevision = 1u;
    m_nSourceRevision = 0u;
    m_nSourceFileHashBytesTotal = 0u;
    m_nSourceFileRehashCount = 0u;
    m_nAvtLastCookWrittenRegions = 0u;
    m_nAvtLastCookReusedRegions = 0u;
    m_nAvtLastCookWrittenBytes = 0u;
    invalidateSourceFingerprints();
}

void Terrain3DFarRegionalCooker::invalidateSurface( void )
{
    markAllDirty( REGION_TYPE_MAX );
}

void Terrain3DFarRegionalCooker::invalidateLayout( void )
{
    markAllDirty( REGION_TYPE_MAX );
}

void Terrain3DFarRegionalCooker::markAllDirty( const t3dMapType_t eMapType )
{
    ( void ) eMapType;
    if ( !m_bAvtCookDirtyAll )
        m_nAvtPublicationRevision = t3dNextGeneration( m_nAvtPublicationRevision );
    m_bAvtCookDirtyAll = true;
    invalidateSourceFingerprints();
}

void Terrain3DFarRegionalCooker::markRegionDirty( const Vector2i &vLocation, const t3dMapType_t eMapType )
{
    ( void ) eMapType;
    // Gutters use the eight neighboring authored regions. Keep dirty publication
    // distinct from the V1 GPU page scheduler and from per-frame cursor updates.
    bool newly_dirty = false;
    for ( int z = -1; z <= 1; ++z ) {
        for ( int x = -1; x <= 1; ++x ) {
            const Vector2i location = vLocation + Vector2i( x, z );
            if ( !m_pRepository || !m_pRepository->isLocationValid( location ) ) continue;
            const int index = Terrain3DRegionRepository::getRegionMapIndex( location );
            if ( index < 0 ) continue;
            const size_t slot = ( size_t ) index;
            newly_dirty |= !m_bAvtCookDirtyAll && !m_avtCookDirtyRegions.test( slot );
            m_avtCookDirtyRegions.set( slot );
            if ( location == vLocation ) {
                m_sourceFingerprints[index].m_bValid = false;
                ++m_sourceFingerprints[index].m_nRevision;
            }
        }
    }
    if ( newly_dirty ) m_nAvtPublicationRevision = t3dNextGeneration( m_nAvtPublicationRevision );
    ++m_nSourceRevision;
}

void Terrain3DFarRegionalCooker::noteRegionSaved(
    const Vector2i &vLocation, const String &rPath, const bool bDeleted )
{
    if ( !m_pRepository || !m_pRepository->isLocationValid( vLocation ) ) return;
    markRegionDirty( vLocation, REGION_TYPE_MAX );
    const int index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
    if ( index < 0 ) return;
    const Terrain3DRegion *const region = bDeleted ? NULL : m_pRepository->getRegionPtr( vLocation );
    if ( !updateSourceFingerprint( index, region, rPath ) )
        m_sourceFingerprints[index].m_bValid = false;
}

bool Terrain3DFarRegionalCooker::isRegionHeightDirty( const Vector2i &vLocation ) const
{
    return isRegionalAvtCookDirty( vLocation );
}

bool Terrain3DFarRegionalCooker::isRegionColorDirty( const Vector2i &vLocation ) const
{
    return isRegionalAvtCookDirty( vLocation );
}

int Terrain3DFarRegionalCooker::getDirtyRegionCount( void ) const
{
    if ( !m_pRepository ) return 0;
    return m_bAvtCookDirtyAll ? m_pRepository->getActiveRegionCount() :
        ( int ) m_avtCookDirtyRegions.count();
}

t3dFarRegionalCookStats_t Terrain3DFarRegionalCooker::getStats( void ) const
{
    t3dFarRegionalCookStats_t stats{};
    stats.m_nHeightDirtyRegions = getDirtyRegionCount();
    stats.m_nColorDirtyRegions = stats.m_nHeightDirtyRegions;
    stats.m_bHeightDirtyAll = m_bAvtCookDirtyAll;
    stats.m_bColorDirtyAll = m_bAvtCookDirtyAll;
    return stats;
}

Dictionary Terrain3DFarRegionalCooker::getStatsDictionary( void ) const
{
    Dictionary stats;
    stats["far_vt_source_dirty_regions"] = getDirtyRegionCount();
    stats["far_vt_source_revision"] = ( int64_t ) m_nSourceRevision;
    stats["far_vt_publication_revision"] = ( int64_t ) m_nAvtPublicationRevision;
    stats["far_avt_schema_version"] = ( int64_t ) T3D_FAR_AVT_SCHEMA_VERSION;
    stats["far_avt_last_cook_written_regions"] = ( int64_t ) m_nAvtLastCookWrittenRegions;
    stats["far_avt_last_cook_reused_regions"] = ( int64_t ) m_nAvtLastCookReusedRegions;
    stats["far_avt_last_cook_written_bytes"] = ( int64_t ) m_nAvtLastCookWrittenBytes;
    stats["far_vt_source_hash_bytes_total"] = ( int64_t ) m_nSourceFileHashBytesTotal;
    stats["far_vt_source_rehash_count"] = ( int64_t ) m_nSourceFileRehashCount;
    stats["far_height_dirty_regions"] = getDirtyRegionCount();
    stats["far_color_dirty_regions"] = getDirtyRegionCount();
    stats["far_height_cook_dirty_regions"] = getDirtyRegionCount();
    stats["far_color_cook_dirty_regions"] = getDirtyRegionCount();
    return stats;
}

Ref< Image > Terrain3DFarRegionalCooker::getRegionMapOrDefault(
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

void Terrain3DFarRegionalCooker::appendGlobalBakeSignature(
	uint64_t &rHash ) const {
	t3dFarSurfaceBakeData_t bake_data;
	t3dFarRegionalCookBuildBakeData( m_config.m_assets, bake_data );
	t3dHashFnv1a64AppendNativeU32( rHash, ( uint32_t ) ( bake_data.m_nCount ) );
	t3dHashFnv1a64AppendNativeU32( rHash, ( uint32_t ) ( bake_data.m_nFallbackTexture ) );
	for ( int index = 0; index < bake_data.m_nCount; ++index ) {
		t3dHashFnv1a64AppendNativeU32( rHash, ( uint32_t ) ( index ) );
		t3dHashFnv1a64AppendNativeU32( rHash, ( uint32_t ) ( bake_data.m_populated[( size_t ) ( index )] ) );
	}
	Terrain3DMaterial *const pMaterial = m_config.m_material.ptr();
	t3dHashFnv1a64AppendNativeU32( rHash, pMaterial ? 1u : 0u );
	if ( pMaterial ) {
		t3dHashFnv1a64AppendNativeU32( rHash, pMaterial->getAutoShaderEnabled() ? 1u : 0u );
		t3dHashFnv1a64AppendNativeF32( rHash,
									   t3dFarRegionalCookMaterialFloatParam( pMaterial, "auto_slope", 1.0f ) );
		t3dHashFnv1a64AppendNativeF32(
			rHash,
			t3dFarRegionalCookMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f ) );
		t3dHashFnv1a64AppendNativeU32(
			rHash,
			( uint32_t ) ( t3dFarRegionalCookMaterialIntParam( pMaterial, "auto_base_texture", 0 ) ) );
		t3dHashFnv1a64AppendNativeU32(
			rHash,
			( uint32_t ) ( t3dFarRegionalCookMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ) ) );
	}
}

void Terrain3DFarRegionalCooker::invalidateSourceFingerprints(
	void ) {
	for ( int i = 0; i < T3D_REGION_SLOTS; ++i ) {
		m_sourceFingerprints[i] = t3dFarRegionSourceFingerprint_t();
	}
}

bool Terrain3DFarRegionalCooker::updateSourceFingerprint(
	const int nIndex,
	const Terrain3DRegion *pRegion,
	const String &rSavedPath ) {
	if ( nIndex < 0 || nIndex >= T3D_REGION_SLOTS ) {
		return false;
	}
	t3dFarRegionSourceFingerprint_t &rEntry = m_sourceFingerprints[nIndex];
	if ( !pRegion || pRegion->isDeleted() ) {
		rEntry.m_path = String();
		rEntry.m_nHash = 0u;
		rEntry.m_bValid = true;
		rEntry.m_bFileSource = false;
		return true;
	}
	const String path = !rSavedPath.is_empty() ? rSavedPath : pRegion->get_path();
	const bool is_file = !path.is_empty() && FileAccess::file_exists( path );
	if ( rEntry.m_bValid && rEntry.m_path == path && rEntry.m_bFileSource == is_file ) {
		return true;
	}
	uint64_t hash = 1469598103934665603ull;
	if ( is_file ) {
		// The file hash includes its encoded byte length. It is recomputed only
		// for a changed region, not for every region on every save.
		uint64_t source_bytes = 0u;
		if ( !t3dFarRegionalCookFnv1a64AppendFile( hash, path, &source_bytes ) ) {
			return false;
		}
		m_nSourceFileHashBytesTotal += source_bytes;
		++m_nSourceFileRehashCount;
	} else {
		// Unsaved/editor-only content has no file identity. Preserve the old
		// height/control/color/derived-map signature semantics for this case.
		t3dFarRegionalCookFnv1a64AppendImage( hash, pRegion->getHeightMap() );
		t3dFarRegionalCookFnv1a64AppendImage( hash, pRegion->getControlMap() );
		t3dFarRegionalCookFnv1a64AppendImage( hash, pRegion->getColorMap() );
		t3dFarRegionalCookFnv1a64AppendImage( hash, pRegion->getDerivedNormalSlopeMap() );
	}
	rEntry.m_path = path;
	rEntry.m_nHash = hash;
	rEntry.m_bFileSource = is_file;
	rEntry.m_bValid = true;
	return true;
}

bool Terrain3DFarRegionalCooker::patchHeightRegionAt(
    const Vector2i &vLocation, PackedByteArray &rAtlasBytes, const int nDestinationSide,
    const int nDestinationX, const int nDestinationY ) {
	if ( !m_pRepository ) {
		return false;
	}
	const int atlas_size = nDestinationSide;
	const int samples_per_region = m_config.m_nHeightSourceSide / T3D_REGION_MAP_SIZE;
	if ( T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 || atlas_size < samples_per_region ) {
		return false;
	}
	if ( samples_per_region <= 0 || T3D_REGION_SIZE % samples_per_region != 0 ) {
		return false;
	}
	const int atlas_origin_x = nDestinationX;
	const int atlas_origin_y = nDestinationY;
	if ( atlas_origin_x < 0 || atlas_origin_y < 0 || atlas_origin_x + samples_per_region > atlas_size ||
		 atlas_origin_y + samples_per_region > atlas_size ||
		 rAtlasBytes.size() < ( int64_t ) atlas_size * atlas_size * 2 ) {
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

bool Terrain3DFarRegionalCooker::patchColorRegionAt(
	const Vector2i	&vLocation,
	PackedByteArray &rAtlasBytes,
	const int		 nDestinationSide,
	const int		 nDestinationX,
	const int		 nDestinationY ) {
	if ( !m_pRepository ) {
		return false;
	}
	const int atlas_size = nDestinationSide;
	if ( T3D_REGION_SIZE <= 0 || T3D_REGION_MAP_SIZE <= 0 || m_config.m_nSurfaceSourceSide % T3D_REGION_MAP_SIZE != 0 ) {
		return false;
	}
	const int samples_per_region = m_config.m_nSurfaceSourceSide / T3D_REGION_MAP_SIZE;
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
	t3dFarRegionalCookBuildBakeData( m_config.m_assets, bake_data );
	Terrain3DMaterial *const pMaterial = m_config.m_material.ptr();
	const bool				 auto_shader_enabled = pMaterial && pMaterial->getAutoShaderEnabled();
	const float				 auto_slope = t3dFarRegionalCookMaterialFloatParam( pMaterial, "auto_slope", 1.0f );
	const float				 auto_height_reduction =
		t3dFarRegionalCookMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f );
	const int auto_layer_max = MAX( bake_data.m_nCount - 1, 0 );
	const int auto_base_texture =
		CLAMP( t3dFarRegionalCookMaterialIntParam( pMaterial, "auto_base_texture", 0 ), 0, auto_layer_max );
	const int auto_overlay_texture =
		CLAMP( t3dFarRegionalCookMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ), 0, auto_layer_max );
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
				t3dFarRegionalCookReadU32Unaligned( pControlRead, ( size_t ) ( center_index ) * 4u );
			float solid_coverage = 0.0f;
			for ( int probe_y = 0; probe_y < 2; ++probe_y ) {
				for ( int probe_x = 0; probe_x < 2; ++probe_x ) {
					const int px = MIN( T3D_REGION_SIZE - 1, region_x0 + ( ( probe_x * 2 + 1 ) * source_step ) / 4 );
					const int py = MIN( T3D_REGION_SIZE - 1, region_y0 + ( ( probe_y * 2 + 1 ) * source_step ) / 4 );
					const uint32_t probe_control =
						t3dFarRegionalCookReadU32Unaligned( pControlRead,
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
						t3dFarRegionalCookReadU16Unaligned( pHeightRead, ( size_t ) ( center_index ) * 2u ) );
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
						t3dFarRegionalCookReadU16Unaligned( pHeightRead, ( size_t ) ( sample_x ) * 2u ) );
					const float height_z = t3dRegionCodecDecodeHeightU16(
						t3dFarRegionalCookReadU16Unaligned( pHeightRead, ( size_t ) ( sample_z ) * 2u ) );
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
			base_id = t3dFarRegionalCookResolveMaterialId( bake_data, base_id );
			overlay_id = t3dFarRegionalCookResolveMaterialId( bake_data, overlay_id );
			const int	 dominant_material = blend >= 0.5f ? overlay_id : base_id;
			const float	 inv_samples = color_samples > 0 ? 1.0f / ( float ) ( color_samples ) : 0.0f;
			const size_t atlas_offset = ( size_t ) ( ( atlas_origin_y + y ) * atlas_size + atlas_origin_x + x ) * 4u;
			pAtlasWrite[atlas_offset] =
				t3dFarRegionalCookColorToByte( ( float ) ( sum_r ) *inv_samples / s_fByteMaxFloat );
			pAtlasWrite[atlas_offset + 1u] =
				t3dFarRegionalCookColorToByte( ( float ) ( sum_g ) *inv_samples / s_fByteMaxFloat );
			pAtlasWrite[atlas_offset + 2u] =
				t3dFarRegionalCookColorToByte( ( float ) ( sum_b ) *inv_samples / s_fByteMaxFloat );
			const float color_alpha = color_samples > 0 ? ( float ) ( sum_a ) *inv_samples / s_fByteMaxFloat : 0.5f;
			pAtlasWrite[atlas_offset + 3u] =
				t3dFarRegionalCookFarSurfaceAlpha( solid_coverage, color_alpha, dominant_material );
		}
	}
	return true;
}

bool Terrain3DFarRegionalCooker::writeOneRegionalAvtCold(
    const Vector2i &vLocation, const String &rPath, const uint64_t nMaterialSignature,
    bool &rWritten )
{
    rWritten = false;
    const int index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
    if ( !m_pRepository || index < 0 || index >= T3D_REGION_SLOTS || !nMaterialSignature ) return false;
    const int colorSide = m_config.m_nSurfaceSourceSide / T3D_REGION_MAP_SIZE;
    const int heightSide = m_config.m_nHeightSourceSide / T3D_REGION_MAP_SIZE;
    if ( colorSide <= 0 || heightSide <= 0 ||
         m_config.m_nSurfaceSourceSide % T3D_REGION_MAP_SIZE != 0 ||
         m_config.m_nHeightSourceSide % T3D_REGION_MAP_SIZE != 0 ||
         T3D_REGION_SIZE % colorSide != 0 || T3D_REGION_SIZE % heightSide != 0 ) return false;

    uint64_t sourceSignature = 1469598103934665603ull;
    t3dHashFnv1a64AppendNativeU32( sourceSignature, T3D_FAR_AVT_SCHEMA_VERSION );
    t3dHashFnv1a64AppendNativeU32( sourceSignature, ( uint32_t ) index );
    struct Neighborhood
    {
        PackedByteArray color;
        PackedByteArray height;
    };
    std::array< Neighborhood, 9 > samples;
    for ( int oz = -1; oz <= 1; ++oz ) {
        for ( int ox = -1; ox <= 1; ++ox ) {
            const Vector2i neighbor = vLocation + Vector2i( ox, oz );
            const uint32_t slot = ( uint32_t ) ( oz + 1 ) * 3u + ( uint32_t ) ( ox + 1 );
            if ( samples[slot].color.resize( colorSide * colorSide * 4 ) != OK ) return false;
            if ( samples[slot].height.resize( heightSide * heightSide * 2 ) != OK ) return false;
            if ( m_pRepository->isLocationValid( neighbor ) ) {
                const int neighborIndex = Terrain3DRegionRepository::getRegionMapIndex( neighbor );
                const Terrain3DRegion *const region = m_pRepository->getRegionPtr( neighbor );
                if ( neighborIndex < 0 ) return false;
                const String path = region ? region->get_path() : String();
                const t3dFarRegionSourceFingerprint_t &entry = m_sourceFingerprints[neighborIndex];
                if ( !entry.m_bValid || entry.m_path != path ) {
                    if ( !updateSourceFingerprint( neighborIndex, region ) ) return false;
                }
                t3dHashFnv1a64AppendNativeU64( sourceSignature, m_sourceFingerprints[neighborIndex].m_nHash );
                if ( !patchColorRegionAt( neighbor, samples[slot].color, colorSide, 0, 0 ) ||
                     !patchHeightRegionAt( neighbor, samples[slot].height, heightSide, 0, 0 ) ) return false;
            } else {
                t3dHashFnv1a64AppendNativeU64( sourceSignature, 0u );
                uint8_t *const pColor = samples[slot].color.ptrw();
                uint8_t *const pHeight = samples[slot].height.ptrw();
                const uint16_t encoded = t3dRegionCodecEncodeHeightU16( 0.0f );
                for ( int i = 0; i < colorSide * colorSide; ++i ) {
                    pColor[i * 4 + 0] = 255u; pColor[i * 4 + 1] = 255u;
                    pColor[i * 4 + 2] = 255u; pColor[i * 4 + 3] = 0u;
                }
                for ( int i = 0; i < heightSide * heightSide; ++i ) memcpy( pHeight + i * 2, &encoded, 2u );
            }
        }
    }
    if ( !sourceSignature ) sourceSignature = 1u;
    const uint64_t versionGeneration = sourceSignature;
    // A valid unchanged indexed file is a no-op across editor restarts.
    if ( FileAccess::file_exists( rPath ) ) {
        const Ref< FileAccess > previous = FileAccess::open( rPath, FileAccess::READ );
        t3dColdIndexHeader_t header;
        eastl::vector< t3dColdChunkRecord_t > records;
        if ( t3dFarAvtOpenRegionCold( previous, vLocation.x, vLocation.y,
                 nMaterialSignature, header, records ) &&
             header.m_nGeneration == versionGeneration ) {
            bool current = true;
            for ( uint32_t i = 0u; i < records.size(); ++i ) {
                current = current && t3dFarAvtValidateRecord( records[i] ) &&
                    records[i].m_nRegionGeneration == versionGeneration &&
                    records[i].m_nMaterialGeneration == nMaterialSignature;
            }
            if ( current ) {
                PackedByteArray encodedScratch, decodedScratch;
                const uint32_t maxBytes = T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE * 4u;
                current = encodedScratch.resize( maxBytes ) == OK && decodedScratch.resize( maxBytes ) == OK;
                for ( uint32_t kind = 1u; current && kind <= T3D_FAR_AVT_CHANNEL_COUNT; ++kind ) {
                    current = t3dFarAvtReadTileCold( previous, header, records, vLocation.x, vLocation.y,
                        kind, 0u, nMaterialSignature, encodedScratch.ptrw(), ( size_t ) encodedScratch.size(),
                        decodedScratch.ptrw(), ( size_t ) decodedScratch.size() );
                }
            }
            if ( current ) {
                uint32_t generation = ( uint32_t ) ( versionGeneration ^ ( versionGeneration >> 32u ) );
                m_nAvtCookedPackageGenerations[index] = generation ? generation : 1u;
                ++m_nAvtLastCookReusedRegions;
                return true;
            }
        }
    }

    std::array< PackedByteArray, T3D_FAR_AVT_REGION_RECORD_COUNT > decoded;
    std::array< PackedByteArray, T3D_FAR_AVT_REGION_RECORD_COUNT > encoded;
    std::array< t3dColdChunkSource_t, T3D_FAR_AVT_REGION_RECORD_COUNT > entries{};
    const uint32_t tile = T3D_VT_PHYSICAL_TILE;
    for ( uint32_t kind = 1u; kind <= T3D_FAR_AVT_CHANNEL_COUNT; ++kind ) {
        const uint32_t components = t3dFarAvtBytesPerTexel( kind );
        const int sourceSide = kind == T3D_FAR_AVT_HEIGHT_R16 ? heightSide : colorSide;
        for ( uint32_t mip = 0u; mip < T3D_FAR_AVT_MIP_COUNT; ++mip ) {
            const uint32_t output = ( kind - 1u ) * T3D_FAR_AVT_MIP_COUNT + mip;
            const uint32_t pixels = tile * tile;
            if ( decoded[output].resize( ( int64_t ) pixels * components ) != OK ) return false;
            uint8_t *const pDest = decoded[output].ptrw();
            const int step = 1 << mip;
            const int interior = MAX( 1, ( int ) T3D_VT_PAGE_INTERIOR >> mip );
            for ( uint32_t y = 0u; y < tile; ++y ) {
                const int sampleY = CLAMP( ( ( int ) y - ( int ) T3D_VT_PAGE_GUTTER ), -4, interior + 3 );
                const int fullY = CLAMP( sampleY * step + step / 2, -128, 255 );
                const int sy = CLAMP( ( fullY * sourceSide ) / 128, -sourceSide, 2 * sourceSide - 1 );
                const int neighborY = sy < 0 ? 0 : ( sy >= sourceSide ? 2 : 1 );
                const int withinY = sy < 0 ? sy + sourceSide : ( sy >= sourceSide ? sy - sourceSide : sy );
                for ( uint32_t x = 0u; x < tile; ++x ) {
                    const int sampleX = CLAMP( ( ( int ) x - ( int ) T3D_VT_PAGE_GUTTER ), -4, interior + 3 );
                    const int fullX = CLAMP( sampleX * step + step / 2, -128, 255 );
                    const int sx = CLAMP( ( fullX * sourceSide ) / 128, -sourceSide, 2 * sourceSide - 1 );
                    const int neighborX = sx < 0 ? 0 : ( sx >= sourceSide ? 2 : 1 );
                    const int withinX = sx < 0 ? sx + sourceSide : ( sx >= sourceSide ? sx - sourceSide : sx );
                    const Neighborhood &src = samples[neighborY * 3 + neighborX];
                    const uint32_t srcIndex = ( uint32_t ) ( withinY * sourceSide + withinX );
                    uint8_t *const target = pDest + ( y * tile + x ) * components;
                    if ( kind == T3D_FAR_AVT_TINT_RGBA8 ) {
                        const uint8_t *const color = src.color.ptr() + srcIndex * 4u;
                        target[0] = color[0]; target[1] = color[1]; target[2] = color[2]; target[3] = 255u;
                    } else if ( kind == T3D_FAR_AVT_PACKED_INDEX_R8 ) {
                        target[0] = src.color.ptr()[srcIndex * 4u + 3u];
                    } else {
                        memcpy( target, src.height.ptr() + srcIndex * 2u, 2u );
                    }
                }
            }
            // RLE is an optional cold codec. Retain raw when the data is noisy.
            t3dColdChunkSource_t &entry = entries[output];
            entry.m_record.m_iRegionX = vLocation.x;
            entry.m_record.m_iRegionY = vLocation.y;
            entry.m_record.m_nKind = kind;
            entry.m_record.m_nFormat = kind;
            entry.m_record.m_nLevel = mip;
            entry.m_record.m_nRegionGeneration = versionGeneration;
            entry.m_record.m_nMaterialGeneration = nMaterialSignature;
            entry.m_record.m_nDecodedBytes = decoded[output].size();
            entry.m_record.m_nEncodedBytes = decoded[output].size();
            entry.m_record.m_nCodec = T3D_COLD_CHUNK_CODEC_RAW;
            PackedByteArray scratch;
            if ( scratch.resize( ( int64_t ) decoded[output].size() * 2u ) != OK ) return false;
            uint32_t rleBytes = 0u;
            if ( t3dColdChunkEncodeRle( decoded[output].ptr(), decoded[output].size(),
                   scratch.ptrw(), scratch.size(), rleBytes ) && rleBytes < decoded[output].size() ) {
                if ( encoded[output].resize( rleBytes ) != OK ) return false;
                memcpy( encoded[output].ptrw(), scratch.ptr(), rleBytes );
                entry.m_record.m_nCodec = T3D_COLD_CHUNK_CODEC_RLE;
                entry.m_record.m_nEncodedBytes = rleBytes;
            } else {
                encoded[output] = decoded[output];
            }
            entry.m_pDecoded = decoded[output].ptr();
            entry.m_pEncoded = encoded[output].ptr();
        }
    }
    if ( !t3dColdIndexPublishFile( rPath, entries.data(), entries.size(), versionGeneration ) ) return false;
    const Ref< FileAccess > written = FileAccess::open( rPath, FileAccess::READ );
    m_nAvtLastCookWrittenBytes += written.is_valid() ? written->get_length() : 0u;
    ++m_nAvtLastCookWrittenRegions;
    rWritten = true;
    uint32_t generation = ( uint32_t ) ( versionGeneration ^ ( versionGeneration >> 32u ) );
    m_nAvtCookedPackageGenerations[index] = generation ? generation : 1u;
    return true;
}

bool Terrain3DFarRegionalCooker::isRegionalAvtCookDirty( const Vector2i &vLocation ) const
{
    const int index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
    return index < 0 || m_bAvtCookDirtyAll || m_avtCookDirtyRegions.test( ( size_t ) index );
}

uint32_t Terrain3DFarRegionalCooker::getRegionalAvtCookedPackageGeneration(
    const Vector2i &vLocation ) const
{
    const int index = Terrain3DRegionRepository::getRegionMapIndex( vLocation );
    if ( index < 0 || m_bAvtCookDirtyAll || m_avtCookDirtyRegions.test( ( size_t ) index ) ) return 0u;
    return m_nAvtCookedPackageGenerations[index];
}

// A clean editor session can outlive external cache deletion or corruption.
// Verify existing derived inputs before deciding that an explicit bake is a
// no-op; the authored region maps, not these files, remain authoritative.
static bool t3dFarRegionalCookValidateCachedFileCold(
    const String &rPath, const Vector2i &vLocation, const uint64_t nMaterialSignature )
{
    const Ref< FileAccess > file = FileAccess::open( rPath, FileAccess::READ );
    t3dColdIndexHeader_t header{};
    eastl::vector< t3dColdChunkRecord_t > records;
    if ( !t3dFarAvtOpenRegionCold( file, vLocation.x, vLocation.y,
            nMaterialSignature, header, records ) ) return false;
    const uint32_t maxBytes = T3D_VT_PHYSICAL_TILE * T3D_VT_PHYSICAL_TILE * 4u;
    PackedByteArray encoded, decoded;
    if ( encoded.resize( maxBytes ) != OK || decoded.resize( maxBytes ) != OK ) return false;
    for ( uint32_t kind = 1u; kind <= T3D_FAR_AVT_CHANNEL_COUNT; ++kind ) {
        if ( !t3dFarAvtReadTileCold( file, header, records, vLocation.x, vLocation.y,
                kind, 0u, nMaterialSignature, encoded.ptrw(), ( size_t ) encoded.size(),
                decoded.ptrw(), ( size_t ) decoded.size() ) ) return false;
    }
    return true;
}

bool Terrain3DFarRegionalCooker::writeRegionalAvtCooked( const String &rDirectory )
{
    if ( !m_pRepository ) return false;
    m_nAvtLastCookWrittenRegions = 0u;
    m_nAvtLastCookReusedRegions = 0u;
    m_nAvtLastCookWrittenBytes = 0u;
    const String directory = ( rDirectory.is_empty() ? m_config.m_dataDirectory : rDirectory )
        .path_join( "terrain3d_far_vt" );
    if ( directory.is_empty() || DirAccess::make_dir_recursive_absolute( directory ) != OK ) return false;
    uint64_t materialSignature = 1469598103934665603ull;
    t3dHashFnv1a64AppendNativeU32( materialSignature, T3D_FAR_AVT_SCHEMA_VERSION );
    t3dHashFnv1a64AppendNativeU32( materialSignature, T3D_REGION_GEOMETRY_SIGNATURE );
    t3dFarRegionalCookFnv1a64AppendImage( materialSignature, m_config.m_defaultHeightMap );
    t3dFarRegionalCookFnv1a64AppendImage( materialSignature, m_config.m_defaultControlMap );
    t3dFarRegionalCookFnv1a64AppendImage( materialSignature, m_config.m_defaultColorMap );
    appendGlobalBakeSignature( materialSignature );
    if ( materialSignature == 0u ) materialSignature = 1u;
    if ( !m_bAvtCookDirtyAll && m_avtCookDirtyRegions.none() ) {
        for ( uint32_t index = 0u; index < T3D_REGION_SLOTS; ++index ) {
            const Vector2i location( ( int ) ( index & T3D_REGION_MAP_MASK ) - T3D_REGION_MAP_HALF,
                                     ( int ) ( index >> T3D_REGION_MAP_SHIFT ) - T3D_REGION_MAP_HALF );
            const String path = directory.path_join( "region_" + String::num_int64( location.x ) +
                                                      "_" + String::num_int64( location.y ) + ".t3avt" );
            const Terrain3DRegion *const region = m_pRepository->getRegionPtr( location );
            const bool active = region && !region->isDeleted();
            const bool exists = FileAccess::file_exists( path );
            if ( active && ( !exists || !t3dFarRegionalCookValidateCachedFileCold(
                    path, location, materialSignature ) ) ) m_avtCookDirtyRegions.set( index );
            if ( !active && exists ) m_avtCookDirtyRegions.set( index );
        }
        if ( m_avtCookDirtyRegions.none() ) return true;
    }
    bool published = false;
    for ( uint32_t index = 0u; index < T3D_REGION_SLOTS; ++index ) {
        if ( !m_bAvtCookDirtyAll && !m_avtCookDirtyRegions.test( index ) ) continue;
        const Vector2i location( ( int ) ( index & T3D_REGION_MAP_MASK ) - T3D_REGION_MAP_HALF,
                                 ( int ) ( index >> T3D_REGION_MAP_SHIFT ) - T3D_REGION_MAP_HALF );
        const String path = directory.path_join( "region_" + String::num_int64( location.x ) +
                                                  "_" + String::num_int64( location.y ) + ".t3avt" );
        const Terrain3DRegion *const region = m_pRepository->getRegionPtr( location );
        // A removed authored region must not remain a live V1 allocation.
        // Retire its file only during explicit cook, after a successful source save.
        if ( !region || region->isDeleted() ) {
            if ( FileAccess::file_exists( path ) ) {
                if ( DirAccess::remove_absolute( path ) != OK ) return false;
                published = true;
            }
            m_nAvtCookedPackageGenerations[index] = 0u;
            m_avtCookDirtyRegions.reset( index );
            continue;
        }
        bool written = false;
        if ( !writeOneRegionalAvtCold( location, path, materialSignature, written ) ) return false;
        published |= written;
        m_avtCookDirtyRegions.reset( index );
    }
    m_bAvtCookDirtyAll = false;
    if ( published ) m_nAvtPublicationRevision = t3dNextGeneration( m_nAvtPublicationRevision );
    return true;
}

