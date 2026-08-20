// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>

#include <algorithm>
#include "eastl_config.h"
#include <cstring>

#include "core/logger.h"
#include "data/terrain_3d_data.h"
#include "rendering/terrain_3d_compute.h"
#include "authoring/terrain_3d_erosion_job.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "authoring/terrain_3d_editor.h"
#include "rendering/terrain_3d_material.h"
#include "assets/terrain_3d_assets.h"
#include "instances/terrain_3d_instancer.h"
#include "memory/terrain_3d_linear_arena.h"
#include "data/terrain_3d_sampler.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_page_format.h"

namespace {

typedef struct t3dcontrolCookSource_s {
	PackedByteArray nBytes;
	bool bPresent = false;
	bool bImplicit = true;
} t3dcontrolCookSource_t;

static bool terrain3dReadCookControl(
	const eastl::array<t3dcontrolCookSource_t, 9> &rSources,
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
	const t3dcontrolCookSource_t &source =
		rSources[( size_t ) ( ( region_dz + 1 ) * 3 + ( region_dx + 1 ) )];
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

static uint32_t terrain3dReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset );

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

static void terrain3dFnv1a64Append( uint64_t &nHash, const uint8_t *pData, const size_t nSize ) {
	for ( size_t index = 0u; index < nSize; ++index ) {
		nHash ^= ( uint64_t ) ( pData[index] );
		nHash *= 1099511628211ull;
	}
}

static void terrain3dFnv1a64AppendU32( uint64_t &nHash, const uint32_t nValue ) {
	terrain3dFnv1a64Append( nHash, ( const uint8_t * ) ( &nValue ), sizeof( nValue ) );
}

static void terrain3dFnv1a64AppendU64( uint64_t &nHash, const uint64_t nValue ) {
	terrain3dFnv1a64Append( nHash, ( const uint8_t * ) ( &nValue ), sizeof( nValue ) );
}

static void terrain3dFnv1a64AppendF32( uint64_t &nHash, const float fValue ) {
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ) );
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	terrain3dFnv1a64AppendU32( nHash, bits );
}

class T3DLastScopeTimer {
	uint64_t _started_usec = 0u;
	uint64_t *_elapsed_usec = nullptr;

public:
	T3DLastScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		_elapsed_usec = &nElapsedUsec;
		Time *time = Time::get_singleton();
		_started_usec = time ? time->get_ticks_usec() : 0u;
	}
	~T3DLastScopeTimer( void ) {
		if ( !_elapsed_usec ) {
			return;
		}
		Time *time = Time::get_singleton();
		const uint64_t finished_usec = time ? time->get_ticks_usec() : _started_usec;
		*_elapsed_usec = finished_usec >= _started_usec ? finished_usec - _started_usec : 0u;
	}
};

class T3DProfileScopeTimer {
	uint64_t _started_usec = 0u;
	uint64_t *_elapsed_usec = nullptr;
	const uint64_t *_request_usec = nullptr;
	uint64_t *_end_to_end_usec = nullptr;

public:
	T3DProfileScopeTimer(
		uint64_t &nElapsedUsec, const uint64_t &nRequestUsec,
		uint64_t &nEndToEndUsec
	) :
			_elapsed_usec( &nElapsedUsec ), _request_usec( &nRequestUsec ),
			_end_to_end_usec( &nEndToEndUsec ) {
		Time *time = Time::get_singleton();
		_started_usec = time ? time->get_ticks_usec() : 0u;
	}

	~T3DProfileScopeTimer( void ) {
		Time *time = Time::get_singleton();
		const uint64_t finished_usec = time ? time->get_ticks_usec() : _started_usec;
		if ( _elapsed_usec ) {
			*_elapsed_usec = MAX( *_elapsed_usec, finished_usec - _started_usec );
		}
		if ( _end_to_end_usec && _request_usec && *_request_usec > 0u && finished_usec >= *_request_usec ) {
			*_end_to_end_usec = MAX( *_end_to_end_usec, finished_usec - *_request_usec );
		}
	}
};

static uint32_t terrain3dReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint8_t terrain3dColorToByte( const float fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( fValue * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
}

bool terrain3dRegionLocationValid( const Vector2i &vLocation, const int nMapSize ) {
	if ( nMapSize <= 0 ) {
		return false;
	}
	const int half = nMapSize / 2;
	return vLocation.x >= -half && vLocation.x < half && vLocation.y >= -half && vLocation.y < half;
}

// Returns the half-open region-coordinate bounds touched by a positive-sized
// area. Computing the exclusive end independently is required for offset and
// negative-coordinate rectangles; ceil(area.size / region_size) is only valid
// when the area starts on a region boundary.
Rect2i getRegionBoundsForArea( const Rect2i &rArea, const int nRegionSize ) {
	if ( nRegionSize <= 0 || rArea.size.x <= 0 || rArea.size.y <= 0 ) {
		return Rect2i();
	}

	const Vector2i begin = V2I_DIVIDE_FLOOR( rArea.position, nRegionSize );
	const Vector2i end = V2I_DIVIDE_CEIL( rArea.get_end(), nRegionSize );
	return Rect2i( begin, end - begin );
}


} // namespace

///////////////////////////
// Private Functions
///////////////////////////

bool T3DData::setRegionSizeInternal( const int nRegionSize ) {
	if ( nRegionSize != 128 ) {
		LOG( T3DTerrain::ERROR, "This Terrain3D fork requires 128-sample regions. Requested: ", nRegionSize );
		return false;
	}
	if ( !isValidRegionSize( nRegionSize ) || TERRAIN3D_ACTIVE_WORLD_TEXELS % nRegionSize != 0 ) {
		LOG( T3DTerrain::ERROR, "Region size ", nRegionSize, " cannot partition the fixed ", TERRAIN3D_ACTIVE_WORLD_TEXELS,
			"x", TERRAIN3D_ACTIVE_WORLD_TEXELS, " active terrain bank" );
		return false;
	}
	const bool bRegionSizeChanged = m_regionRepository.getRegionSize() != nRegionSize;
	if ( !m_samplingStorage.claimOrReconfigureActive( nRegionSize ) ) {
		LOG( T3DTerrain::ERROR, "Unable to claim process-static active terrain storage. Only one Terrain3DData owner is legal." );
		return false;
	}
	m_regionRepository.configureGeometry( nRegionSize, m_regionRepository.getVertexSpacing() );
	m_activeHeightResidency.markDirty();
	if ( bRegionSizeChanged ) {
		resetMiddleHeightCache();
		if ( m_gpuRegionCache.isEnabled() ) {
			resetGpuRegionCache( true );
		}
		if ( m_cpuRegionCache.isEnabled() ) {
			resetCpuRegionCache( false );
			m_cpuRegionCache.refreshCapacity();
		}
		m_farSurfaceAtlasCache.invalidateLayout();
	}
	return true;
}

Error T3DData::rehydrateActiveStorage( void ) {
	if ( !m_samplingStorage.isActiveOwner() || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return ERR_UNCONFIGURED;
	}

	// The physical bank is intentionally smaller than the logical world. Clear its
	// descriptors here and let the normal spatial residency pass publish only the
	// current working set. This avoids a synchronous all-world Image -> byte-array
	// rebuild during scene activation.
	if ( !m_samplingStorage.replaceActiveHeightPages( m_regionRepository.getRegionSize(), nullptr, nullptr, 0u ) ) {
		LOG( T3DTerrain::ERROR, "Failed to reset active terrain page descriptors during scene activation" );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	m_samplingStorage.refreshActiveGeneration();
	m_activeHeightResidency.markDirty();
	return OK;
}

bool T3DData::syncActiveRegion( const Vector2i &vLocation, const T3DRegion *pRegion ) {
	return m_samplingStorage.syncActiveRegion( vLocation, pRegion );
}

void T3DData::clear( void ) {
	m_regionResidency.reset();
	m_runtimePageStore.reset();
	m_samplingStorage.resetActive();
	m_samplingStorage.resetResidentPool();
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	m_farSurfaceAtlasCache.reset();
	LOG( T3DTerrain::INFO, "Clearing data" );
	m_regionRepository.clear();
	m_mapEditService.reset();
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_pDefaultHeightMap.unref();
	m_pDefaultControlMap.unref();
	m_pDefaultColorMap.unref();
	m_pDefaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_generatedHeightMaps.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();
	m_generatedMiddleCompositeMaps.clear();
	invalidateSamplingCache();
}

void T3DData::ensureDefaultMaps( void ) {
	if ( m_regionRepository.getRegionSize() <= 0 ) {
		return;
	}
	if ( m_pDefaultHeightMap.is_null() || m_pDefaultHeightMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_pDefaultHeightMap = Util::getFilledImage( m_regionRepository.getRegionSizeVector(), Terrain3DRegionCodec::defaultHeightColor(), false,
									 Image::FORMAT_R16 );
	}
	if ( m_pDefaultControlMap.is_null() || m_pDefaultControlMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_pDefaultControlMap = Util::getFilledImage( m_regionRepository.getRegionSizeVector(), COLOR_CONTROL, false, Image::FORMAT_RF );
	}
	if ( m_pDefaultColorMap.is_null() || m_pDefaultColorMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_pDefaultColorMap = Util::getFilledImage( m_regionRepository.getRegionSizeVector(), COLOR_ROUGHNESS, false, Image::FORMAT_RGBA8 );
	}
	if ( m_pDefaultDerivedMap.is_null() || m_pDefaultDerivedMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_pDefaultDerivedMap = Util::getFilledImage( m_regionRepository.getRegionSizeVector(), Color( 0.5f, 0.5f, 0.0f, 1.0f ), false,
									  Image::FORMAT_RG8 );
	}
	syncCacheDefaultMaps();
}

Ref<Image> T3DData::getRegionMapOrDefault( const T3DRegion *pRegion, const t3dmapType_t eMapType ) const {
	if ( pRegion && !pRegion->isDeleted() ) {
		Ref<Image> pMap = pRegion->getMap( eMapType );
		if ( pMap.is_valid() ) {
			return pMap;
		}
	}
	switch ( eMapType ) {
		case T3DRegion::TYPE_HEIGHT:
			return m_pDefaultHeightMap;
		case T3DRegion::TYPE_CONTROL:
			return m_pDefaultControlMap;
		case T3DRegion::TYPE_COLOR:
			return m_pDefaultColorMap;
		default:
			return Ref<Image>();
	}
}

bool T3DData::hasMaterializedMap( const t3dmapType_t eMapType ) const {
	if ( eMapType != T3DRegion::TYPE_CONTROL && eMapType != T3DRegion::TYPE_COLOR ) {
		return true;
	}
	T3DRegion *const *const ppRegions = m_regionRepository.getRegionPtrMapData();
	for ( int i = 0; i < TERRAIN3D_MAX_REGION_SLOTS; ++i ) {
		const T3DRegion *const pRegion = ppRegions[i];
		if ( pRegion && !pRegion->isDeleted() && pRegion->hasChannel( eMapType ) ) {
			return true;
		}
	}
	return false;
}

bool T3DData::refreshImplicitMapState( void ) {
	const Ref<T3DMaterial> pMaterial = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	const bool bForceFullArrays = pMaterial.is_valid() &&
		( pMaterial->isShaderOverrideEnabled() || pMaterial->isBufferShaderOverrideEnabled() );
	const bool bControlImplicit = !bForceFullArrays && !hasMaterializedMap( T3DRegion::TYPE_CONTROL );
	const bool bColorImplicit = !bForceFullArrays && !hasMaterializedMap( T3DRegion::TYPE_COLOR );
	const bool bChanged = bControlImplicit != m_bControlMapsImplicit || bColorImplicit != m_bColorMapsImplicit;
	if ( bControlImplicit != m_bControlMapsImplicit ) {
		m_bControlMapsImplicit = bControlImplicit;
		m_generatedControlMaps.clear();
	}
	if ( bColorImplicit != m_bColorMapsImplicit ) {
		m_bColorMapsImplicit = bColorImplicit;
		m_generatedColorMaps.clear();
	}
	if ( bChanged ) {
		syncCacheDefaultMaps();
	}
	return bChanged;
}

void T3DData::syncCacheDefaultMaps( void ) {
	m_gpuRegionCache.setTerrain( m_pTerrain );
	m_gpuRegionCache.setDefaultMaps(
		m_pDefaultHeightMap, m_pDefaultControlMap, m_pDefaultColorMap, m_pDefaultDerivedMap,
		m_bControlMapsImplicit, m_bColorMapsImplicit );
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
}

T3DFarSurfaceAtlasConfig T3DData::getFarSurfaceAtlasConfig( void ) const {
	T3DFarSurfaceAtlasConfig config;
	config.bRequired = isFarAtlasRequired();
	config.nHeightAtlasSide = m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	config.nSurfaceAtlasSide = m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
	config.dataDirectory = m_pTerrain ? m_pTerrain->getDataDirectory() : String();
	config.pDefaultHeightMap = m_pDefaultHeightMap;
	config.pDefaultControlMap = m_pDefaultControlMap;
	config.pDefaultColorMap = m_pDefaultColorMap;
	config.pMaterial = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	config.pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<T3DAssets>();
	return config;
}

T3DMiddleHeightRegionCacheConfig T3DData::getMiddleHeightCacheConfig( void ) const {
	T3DMiddleHeightRegionCacheConfig config;
	config.bAuthoring = m_pTerrain && m_pTerrain->isEditorAuthoringMode();
	config.nHotRadius = m_pTerrain ? m_pTerrain->getStreamingHotRadius() : 0;
	config.nResolutionDivisor = config.bAuthoring ? 1 :
		MAX( 1, m_pTerrain ? m_pTerrain->getGpuMiddleHeightResolutionDivisor() : 1 );
	config.nRetireFrames = m_pTerrain ? m_pTerrain->getGpuRegionRetireFrames() : TERRAIN3D_GPU_CACHE_DEFAULT_RETIRE_FRAMES;
	return config;
}

void T3DData::applyInstanceRegionCacheChanges( const T3DInstanceRegionCacheChanges &rChanges ) {
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) {
		return;
	}
	for ( int index = 0; index < rChanges.nEvicted; ++index ) {
		m_pTerrain->getInstancer()->destroyRegionMmis( rChanges.evicted[index] );
	}
	for ( int index = 0; index < rChanges.nLoaded; ++index ) {
		m_pTerrain->getInstancer()->updateMmis( -1, rChanges.loaded[index], false );
	}
}

Ref<Image> T3DData::makeGpuColorImage( const Ref<Image> &rImage ) const {
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

Ref<Image> T3DData::makeGpuDerivedImage( const Ref<Image> &rImage ) const {
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

Ref<Image> T3DData::makeMiddleCompositeImage(
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

Ref<Image> T3DData::makeGpuRenderControlImage( const Vector2i &vLocation ) const {
	eastl::array<t3dcontrolCookSource_t, 9> sources;
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = vLocation + Vector2i( dx, dz );
			const int source_index = ( dz + 1 ) * 3 + ( dx + 1 );
			t3dcontrolCookSource_t &source = sources[( size_t ) ( source_index )];
			const T3DRegion *pRegion = getRegionPtr( location );
			Ref<T3DRegion> pLoadedSource;
			const bool bNeedsSourceLoad = !pRegion || pRegion->isDeleted() ||
				( pRegion->hasChannel( T3DRegion::T3DRegion::TYPE_CONTROL ) && pRegion->getControlMap().is_null() );
			if ( bNeedsSourceLoad && loadCpuRegionSource( location, pLoadedSource ) ) {
				pRegion = pLoadedSource.ptr();
			}
			if ( !pRegion || pRegion->isDeleted() ) {
				continue;
			}
			source.bPresent = true;
			if ( !pRegion->hasChannel( T3DRegion::T3DRegion::TYPE_CONTROL ) || pRegion->getControlMap().is_null() ) {
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

T3DRuntimePageConfig T3DData::getRuntimePageConfig( void ) const {
	T3DRuntimePageConfig config;
	if ( !m_pTerrain ) {
		return config;
	}
	config.dataDirectory = m_pTerrain->getDataDirectory();
	config.colorDivisor = MAX( 1, m_pTerrain->getGpuRegionColorResolutionDivisor() );
	config.derivedDivisor = MAX( 1, m_pTerrain->getGpuRegionDerivedResolutionDivisor() );
	config.middleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
	config.prepareThreads = MAX( 1, m_pTerrain->getRuntimePagePrepareThreads() );
	config.bCookedPagesEnabled = m_pTerrain->isGpuRegionCookedPagesEnabled();
	config.bEditorAuthoring = m_pTerrain->isEditorAuthoringMode();
	const Ref<T3DAssets> pAssets = m_pTerrain->getAssets();
	config.materialSourceSignature = pAssets.is_valid() ? pAssets->getRuntimeMaterialSourceSignature() : 0u;
	return config;
}

T3DResidencyFrameState T3DData::makeResidencyFrameState( const Vector3 &rWorldPosition ) const {
	T3DResidencyFrameState frame;
	frame.worldPosition = rWorldPosition;
	frame.nRegionMapSize = m_regionRepository.getRegionMapSize();
	const bool bGeometryReady = m_regionRepository.getRegionSize() > 0 &&
		m_regionRepository.getVertexSpacing() > 0.0f;
	if ( bGeometryReady && frame.nRegionMapSize > 0 ) {
		const Vector2i rawCenter = m_regionRepository.getRegionLocation( rWorldPosition );
		const t3dgpuCacheCoord_t clampedCenter = terrain3dGpuCacheClampCenter(
			{rawCenter.x, rawCenter.y}, frame.nRegionMapSize );
		frame.residencyCenter = Vector2i( clampedCenter.x, clampedCenter.y );
	}
	frame.bCpuRegionCacheEnabled = m_cpuRegionCache.isEnabled();
	if ( !m_pTerrain ) {
		return frame;
	}
	frame.streamingCenter = m_pTerrain->getRegionStreamingCenterInternal();
	frame.predictedCenter = m_pTerrain->getRegionStreamingPredictedCenterInternal();
	if ( bGeometryReady ) {
		frame.activeHeightCenter = m_regionRepository.getRegionLocation( m_pTerrain->getClipmapTargetPosition() );
	}
	frame.pStreamingStates = m_pTerrain->getRegionStreamingStatesInternal();
	frame.pStreamingPrefetch = m_pTerrain->getRegionStreamingPrefetchInternal();
	frame.streamingGeneration = m_pTerrain->getRegionStreamingGeneration();
	frame.nStreamingActiveRadius = MAX( 0, m_pTerrain->getStreamingActiveRadius() );
	frame.nMiddleLods = m_pTerrain->getRuntimeMiddleShaderLods();
	frame.nGpuHysteresisTicks = m_pTerrain->getGpuRegionHysteresisTicks();
	frame.bAuthoring = m_pTerrain->isEditorAuthoringMode();
	frame.bRuntimeTiering = m_pTerrain->isRuntimeShaderTieringActive();
	frame.bCookedPagesEnabled = m_pTerrain->isGpuRegionCookedPagesEnabled();
	frame.bProfilingEnabled = m_pTerrain->isRuntimeProfilingEnabled();
	frame.runtimePageConfig = getRuntimePageConfig();
	return frame;
}

bool T3DData::isRuntimePageWanted( void *const pContext, const Vector2i &rLocation ) {
	const T3DResidencyFrameState *const pFrame = static_cast<const T3DResidencyFrameState *>( pContext );
	return pFrame && ( pFrame->streamingState( rLocation ) >= TERRAIN3D_REGION_STREAMING_HOT ||
		pFrame->isPrefetch( rLocation ) );
}

uint64_t T3DData::computeGpuRuntimePageSourceSignature( const T3DRegion *const pRegion ) const {
	return m_runtimePageStore.computeSourceSignature( pRegion, getRuntimePageConfig() );
}

bool T3DData::readGpuRuntimePageSourceSignature( const String &rPagePath, uint64_t &nSignature ) const {
	return m_runtimePageStore.readSourceSignature( rPagePath, nSignature );
}

String T3DData::getGpuRuntimePagePath( const T3DRegion *const pRegion ) const {
	return m_runtimePageStore.getPagePath( pRegion, m_pTerrain ? m_pTerrain->getDataDirectory() : String() );
}

void T3DData::pollGpuRuntimePagePrepare( const bool bWait ) {
	m_runtimePageStore.poll( bWait );
}

bool T3DData::isGpuRuntimePagePreparePending( const Vector2i &vLocation ) const {
	return m_runtimePageStore.isPending( vLocation );
}

void T3DData::requestGpuRuntimePage(
	const Vector2i &vLocation, const uint8_t nConsumers, const uint8_t nPriority
) {
	m_runtimePageStore.request(
		vLocation, nConsumers, nPriority,
		m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled()
	);
}

void T3DData::serviceGpuRuntimePagePrepare( const T3DResidencyFrameState &rFrame ) {
	T3DRuntimePageServiceState state;
	state.config = rFrame.runtimePageConfig;
	state.predictedCenter = rFrame.predictedCenter;
	state.streamingGeneration = rFrame.streamingGeneration;
	state.pIsWanted = &T3DData::isRuntimePageWanted;
	state.pWantedContext = const_cast<T3DResidencyFrameState *>( &rFrame );
	m_runtimePageStore.service( state );
}

bool T3DData::takePreparedRuntimeHeight(
	const Vector2i &vLocation, const uint8_t nConsumer, Ref<Image> &rHeight, const bool bMiddle,
	PackedByteArray *const pNativeHeightBytes
) {
	return m_runtimePageStore.takeHeight(
		vLocation, nConsumer, rHeight, bMiddle, pNativeHeightBytes );
}

bool T3DData::takePreparedRuntimeInstance( const Vector2i &vLocation, T3DInstanceStore &rStore ) {
	return m_runtimePageStore.takeInstance( vLocation, rStore );
}

bool T3DData::takePreparedGpuRuntimePage(
	const Vector2i &vLocation, Ref<Image> &rHeight,
	Ref<Image> &rControl, Ref<Image> &rColor, Ref<Image> &rDerived, Ref<Image> &rMiddleComposite,
	uint32_t &rMaterialMask, bool &bAttempted, uint64_t *const pRequestedUsec
) {
	return m_runtimePageStore.takeGpuPage(
		vLocation, rHeight, rControl, rColor, rDerived, rMiddleComposite,
		rMaterialMask, bAttempted, pRequestedUsec );
}

bool T3DData::writeGpuRuntimePage( T3DRegion *const pRegion, String *const pError ) {
	if ( !pRegion || pRegion->isDeleted() || !m_pTerrain ) {
		if ( pError ) {
			*pError = "region/terrain owner is unavailable";
		}
		return false;
	}

	T3DRuntimePageCookPayload payload;
	payload.height = pRegion->getHeightMap();
	if ( pRegion->hasChannel( T3DRegion::TYPE_CONTROL ) ) {
		payload.pControl = makeGpuRenderControlImage( pRegion->getLocation() );
	}
	if ( pRegion->hasChannel( T3DRegion::TYPE_COLOR ) ) {
		payload.pColor = makeGpuColorImage( pRegion->getColorMap() );
	}
	payload.pMiddleHeight = m_middleHeightRegionCache.makeHeightImage( pRegion->getLocation(), true );
	payload.pDerived = pRegion->getDerivedNormalSlopeMap();
	if ( payload.pDerived.is_null() ) {
		regenerateDerivedMaps( {pRegion->getLocation()} );
		payload.pDerived = pRegion->getDerivedNormalSlopeMap();
	}
	payload.pDerived = makeGpuDerivedImage( payload.pDerived );
	payload.pMiddleComposite = makeMiddleCompositeImage(
		pRegion->getLocation(),
		payload.pControl.is_valid() ? payload.pControl : m_pDefaultControlMap,
		payload.pColor.is_valid() ? payload.pColor : makeGpuColorImage( m_pDefaultColorMap )
	);
	payload.materialMask = payload.pControl.is_valid() ? terrain3dMaterialMaskFromControlImage( payload.pControl ) : 1u;
	return m_runtimePageStore.writePage( pRegion, getRuntimePageConfig(), payload, pError );
}

int T3DData::getRegionSlotReserve( void ) const {
	if ( !IS_EDITOR ) {
		return 0;
	}
	const int regionSize = m_regionRepository.getRegionSize();
	if ( regionSize <= 128 ) {
		return 16;
	}
	if ( regionSize <= 256 ) {
		return 8;
	}
	if ( regionSize <= 512 ) {
		return 2;
	}
	return 1;
}

bool T3DData::canUpdateRegionSlotsIncrementally( void ) const {
	if ( m_gpuRegionCache.isEnabled() ) {
		return false;
	}
	const int slotCount = m_regionRepository.getSlotCount();
	return !m_regionRepository.isRegionMapDirty() && slotCount > 0 && !m_generatedHeightMaps.isDirty() &&
		!m_generatedControlMaps.isDirty() && !m_generatedColorMaps.isDirty() &&
		!m_generatedDerivedMaps.isDirty() && m_generatedHeightMaps.size() == slotCount &&
		m_generatedControlMaps.size() == slotCount && m_generatedColorMaps.size() == slotCount &&
		m_generatedDerivedMaps.size() == slotCount && m_heightMaps.size() == slotCount &&
		m_controlMaps.size() == slotCount && m_colorMaps.size() == slotCount && m_derivedMaps.size() == slotCount;
}

int T3DData::calculateGpuRegionRequiredCorePagesForLods( const int nDetailLods ) const {
	return m_gpuRegionCache.calculateRequiredCorePagesForLods( nDetailLods );
}





int T3DData::calculateGpuRegionCapacityForLods( const int nDetailLods ) const {
	return m_gpuRegionCache.calculateCapacityForLods( nDetailLods );
}

















bool T3DData::isGpuRegionColorChannelEnabled( void ) const {
	return m_gpuRegionCache.isColorChannelEnabled();
}


bool T3DData::isGpuRegionDerivedChannelEnabled( void ) const {
	return m_gpuRegionCache.isDerivedChannelEnabled();
}


























void T3DData::resetGpuRegionCache( const bool bClearTextures ) {
	m_gpuRegionCache.reset( bClearTextures );
}











bool T3DData::isFarAtlasRequired( void ) const {
	return m_gpuRegionCache.isEnabled() || ( m_pTerrain && m_pTerrain->isRuntimeShaderTieringActive() );
}








bool T3DData::writeCookedGpuFarAtlases( const String &rDirectory ) {
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	return m_farSurfaceAtlasCache.writeCooked( rDirectory );
}


void T3DData::markAllGpuFarRegionsDirty( const t3dmapType_t eMapType ) {
	m_farSurfaceAtlasCache.markAllDirty( eMapType );
}


void T3DData::markGpuFarRegionDirty( const Vector2i &vLocation, const t3dmapType_t eMapType ) {
	m_farSurfaceAtlasCache.markRegionDirty( vLocation, eMapType );
}


























void T3DData::armPendingGpuRegionRetirements( void ) {
	m_gpuRegionCache.armPendingRetirements();
}





void T3DData::invalidateGpuRegionLocation( const Vector2i &vLocation ) {
	m_gpuRegionCache.invalidate( vLocation );
}





void T3DData::queueGpuRegionRepublish( const Vector2i &vLocation ) {
	m_gpuRegionCache.queueRepublish( vLocation );
}


bool T3DData::republishGpuRegionLocation( const Vector2i &vLocation ) {
	const bool bChanged = m_gpuRegionCache.republish( vLocation, getComputeBackendIfCreated() );
	if ( bChanged ) {
		emitGpuRegionMapChanged();
	}
	return bChanged;
}








void T3DData::resetMiddleHeightCache( void ) {
	m_middleHeightRegionCache.reset();
}


bool T3DData::configureMiddleHeightCache( void ) {
	return m_middleHeightRegionCache.configure( getMiddleHeightCacheConfig() );
}


















void T3DData::updateMiddleHeightResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	m_middleHeightRegionCache.update(
		rFrame, bForce, getEffectiveGpuRegionUploadBudgetLayers(), getMiddleHeightCacheConfig() );
}



Dictionary T3DData::getMiddleHeightCacheStats( void ) const {
	return m_middleHeightRegionCache.getStatsDictionary();
}


int T3DData::fillGpuMaterialPriorityIds( uint8_t *const pIds, const int nCapacity ) const {
	return m_gpuRegionCache.fillMaterialPriorityIds( pIds, nCapacity );
}


size_t T3DData::getMiddleHeightCacheReservedBytes( void ) const {
	return m_middleHeightRegionCache.getReservedBytes();
}








void T3DData::resetCpuRegionCache( const bool bReleaseCleanMaps ) {
	m_cpuRegionCache.reset( bReleaseCleanMaps );
}





void T3DData::invalidateCpuRegionLocation( const Vector2i &vLocation ) {
	m_cpuRegionCache.invalidate( vLocation );
}





bool T3DData::requestCpuRegionSourceAsync( const Vector2i &vLocation ) {
	return m_cpuRegionCache.requestSourceAsync( vLocation );
}


















bool T3DData::ensureCpuRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	return m_cpuRegionCache.ensureResident( vLocation, bDemandLoad );
}











void T3DData::setRuntimePageBudgetOverrides(
	const int nGpuUploadLayers,
	const int nCpuLoadLayers, const int nInstanceLoadPages
) {
	m_regionResidency.setBudgetOverrides( nGpuUploadLayers, nCpuLoadLayers, nInstanceLoadPages );
}

void T3DData::clearRuntimePageBudgetOverrides( void ) {
	m_regionResidency.clearBudgetOverrides();
}

int T3DData::getEffectiveGpuRegionUploadBudgetLayers( void ) const {
	return m_regionResidency.effectiveGpuUploadLayers( m_gpuRegionCache.getUploadBudgetLayers() );
}

int T3DData::getEffectiveCpuRegionLoadBudgetLayers( void ) const {
	return m_regionResidency.effectiveCpuLoadLayers( m_cpuRegionCache.getLoadBudgetLayers() );
}

int T3DData::getEffectiveInstanceRegionLoadBudgetPages( void ) const {
	return m_regionResidency.effectiveInstanceLoadPages( m_instanceRegionCache.getLoadBudgetPages() );
}


void T3DData::configureCpuRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadLayers, const int nLoadBudgetMb
) {
	m_cpuRegionCache.configure( bEnabled, nBudgetMb, nLoadLayers, nLoadBudgetMb );
}


bool T3DData::ensureCpuRegionResident( const Vector2i &vLocation ) {
	return m_cpuRegionCache.ensureResident( vLocation, true );
}



bool T3DData::ensureCpuSampleArea( const Vector3 &rWorldPosition, const int nRadiusTexels ) {
	return m_cpuRegionCache.ensureSampleArea( rWorldPosition, nRadiusTexels );
}


void T3DData::updateCpuRegionResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	m_cpuRegionCache.update( rFrame, bForce, getEffectiveCpuRegionLoadBudgetLayers() );
}



Dictionary T3DData::getCpuRegionCacheStatsFast( void ) const {
	return m_cpuRegionCache.getStatsDictionary( false, getEffectiveCpuRegionLoadBudgetLayers() );
}


Dictionary T3DData::getCpuRegionCacheStats( void ) const {
	return m_cpuRegionCache.getStatsDictionary( true, getEffectiveCpuRegionLoadBudgetLayers() );
}


void T3DData::emitGpuRegionMapChanged( void ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	m_gpuRegionCache.recordPageTableUpdate();
	emit_signal( "gpu_region_page_table_changed" );
	armPendingGpuRegionRetirements();
}


void T3DData::resetInstanceRegionCache( const bool bRestoreAll ) {
	T3DInstanceRegionCacheChanges changes;
	m_instanceRegionCache.reset( bRestoreAll, &changes );
	applyInstanceRegionCacheChanges( changes );
}





void T3DData::invalidateInstanceRegionLocation( const Vector2i &vLocation ) {
	T3DInstanceRegionCacheChanges changes;
	m_instanceRegionCache.invalidate( vLocation, &changes );
	applyInstanceRegionCacheChanges( changes );
}





bool T3DData::ensureInstanceRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	T3DInstanceRegionCacheChanges changes;
	const bool bReady = m_instanceRegionCache.ensureResident( vLocation, bDemandLoad, &changes );
	applyInstanceRegionCacheChanges( changes );
	return bReady;
}





bool T3DData::evictInstanceRegion( const Vector2i &vLocation ) {
	T3DInstanceRegionCacheChanges changes;
	const bool bEvicted = m_instanceRegionCache.evict( vLocation, &changes );
	applyInstanceRegionCacheChanges( changes );
	return bEvicted;
}


void T3DData::configureInstanceRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadPages, const int nLoadBudgetMb
) {
	T3DInstanceRegionCacheChanges changes;
	m_instanceRegionCache.configure( bEnabled, nBudgetMb, nLoadPages, nLoadBudgetMb, &changes );
	applyInstanceRegionCacheChanges( changes );
}


void T3DData::updateInstanceRegionResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	T3DInstanceRegionCacheChanges changes;
	m_instanceRegionCache.update( rFrame, bForce, getEffectiveInstanceRegionLoadBudgetPages(), changes );
	applyInstanceRegionCacheChanges( changes );
}


void T3DData::updateCpuRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	updateCpuRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void T3DData::updateInstanceRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	updateInstanceRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void T3DData::updateGpuRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	updateGpuRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void T3DData::updateRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	m_regionResidency.update( *this, makeResidencyFrameState( rWorldPosition ), bForce );
}

void T3DData::resetRuntimeStreamingCaches( void ) {
	m_regionResidency.resetRuntimeCaches( *this );
}

void T3DData::appendRuntimeProfileSample( t3druntimeProfileSample_t &rSample ) const {
	m_regionResidency.appendRuntimeProfileSample( *this, rSample );
}

Dictionary T3DData::getInstanceRegionCacheStatsFast( void ) const {
	return m_instanceRegionCache.getStatsDictionary( false, getEffectiveInstanceRegionLoadBudgetPages() );
}


Dictionary T3DData::getInstanceRegionCacheStats( void ) const {
	return m_instanceRegionCache.getStatsDictionary( true, getEffectiveInstanceRegionLoadBudgetPages() );
}


void T3DData::updateMapsBounded( const t3dmapType_t eMapType, const bool bAllRegions ) {
	const bool bImplicitStateChanged = refreshImplicitMapState();
	bool bTopologyChanged = false;
	if ( m_regionRepository.isRegionMapDirty() ) {
		m_regionRepository.rebuildRegionMapFromSlots();
		bTopologyChanged = true;
		m_gpuRegionCache.markSelectionDirty();
		if ( !m_bStartupBulkLoad ) {
			emit_signal( "region_map_changed" );
		}
	}

	ensureDefaultMaps();
	bool bPhysicalMappingChanged = false;
	if ( bAllRegions && m_gpuRegionCache.isTexturesReady() ) {
		bPhysicalMappingChanged = m_gpuRegionCache.republishAllResidents( getComputeBackendIfCreated() );
	}

	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		calcHeightRange();
	}

	if ( bImplicitStateChanged || bTopologyChanged || !m_gpuRegionCache.isTexturesReady() ) {
		m_gpuRegionCache.markSelectionDirty();
		const Vector3 center = m_pTerrain ? m_pTerrain->getClipmapTargetPosition() : Vector3();
		updateGpuRegionResidency( center, false );
	}
	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}

	if ( !m_bStartupBulkLoad ) {
		const bool bHeightChanged = bAllRegions && ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX );
		const bool bControlChanged = bAllRegions && ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX );
		const bool bColorChanged = bAllRegions && ( eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX );
		if ( bHeightChanged ) {
			emit_signal( "height_maps_changed" );
		}
		if ( bControlChanged || bImplicitStateChanged ) {
			emit_signal( "control_maps_changed" );
		}
		if ( bColorChanged || bImplicitStateChanged ) {
			emit_signal( "color_maps_changed" );
		}
		if ( bHeightChanged || bControlChanged || bColorChanged || bImplicitStateChanged || bTopologyChanged ) {
			emit_signal( "maps_changed" );
		}
	}
}

void T3DData::emitRegionLayoutChanged( const bool bTextureLayersChanged ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	LOG( T3DTerrain::DEBUG, "Emitting region_map_changed" );
	emit_signal( "region_map_changed" );
	if ( bTextureLayersChanged ) {
		LOG( T3DTerrain::DEBUG, "Emitting height_maps_changed" );
		emit_signal( "height_maps_changed" );
		LOG( T3DTerrain::DEBUG, "Emitting control_maps_changed" );
		emit_signal( "control_maps_changed" );
		LOG( T3DTerrain::DEBUG, "Emitting color_maps_changed" );
		emit_signal( "color_maps_changed" );
	}
	LOG( T3DTerrain::DEBUG, "Emitting maps_changed" );
	emit_signal( "maps_changed" );
	m_pTerrain->snap();
}

TypedArray<Vector2i> T3DData::getRegionLocations( void ) const {
	return m_regionRepository.getActiveLocations();
}

TypedArray<Image> T3DData::getActiveMaps( const t3dmapType_t eMapType ) const {
	TypedArray<Image> maps;
	if ( m_cpuRegionCache.isEnabled() ) {
		LOG( T3DTerrain::ERROR, "Whole-world map arrays are unavailable while bounded CPU residency is enabled. "
			"Page one region with ensure_cpu_region_resident() and read that Terrain3DRegion, or disable the CPU cache." );
		return maps;
	}
	maps.resize( m_regionRepository.getActiveRegionCount() );
	int write_index = 0;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		const T3DRegion *pRegion = getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() ) {
			maps[write_index++] = getRegionMapOrDefault( pRegion, eMapType );
		}
	}
	if ( write_index != maps.size() ) {
		maps.resize( write_index );
	}
	return maps;
}

TypedArray<Image> T3DData::getHeightMaps( void ) const {
	return getActiveMaps( T3DRegion::TYPE_HEIGHT );
}

TypedArray<Image> T3DData::getControlMaps( void ) const {
	return getActiveMaps( T3DRegion::TYPE_CONTROL );
}

TypedArray<Image> T3DData::getColorMaps( void ) const {
	return getActiveMaps( T3DRegion::TYPE_COLOR );
}

// Structured to work with do_for_regions. Should be renamed when copy_paste is expanded.
void T3DData::copyPasteDfr(
	const T3DRegion *pSrcRegion, const Rect2i &rSrcRect,
	const Rect2i &rDstRect, T3DRegion *pDstRegion
) {
	if ( !pSrcRegion || !pDstRegion ) {
		return;
	}
	m_mapEditService.copyRegionMaps( pSrcRegion, rSrcRect, rDstRect, pDstRegion );
	m_pTerrain->getInstancer()->copyPasteDfr( pSrcRegion, rSrcRect, pDstRegion );
}

///////////////////////////
// Public Functions
///////////////////////////

T3DData::T3DData( void ) :
		m_mapEditService( &m_regionRepository, &m_samplingStorage ),
		m_regionPersistence( &m_regionRepository ),
		m_runtimePageStore( &m_regionRepository, &m_regionPersistence ),
		m_cpuRegionCache( &m_regionRepository, &m_regionPersistence, &m_samplingStorage ),
		m_instanceRegionCache( &m_regionRepository, &m_runtimePageStore ),
		m_middleHeightRegionCache( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore ),
		m_gpuRegionCache( m_regionRepository, m_samplingStorage, m_mapEditService, m_runtimePageStore, m_cpuRegionCache ),
		m_farSurfaceAtlasCache( &m_regionRepository, &m_samplingStorage, &m_cpuRegionCache ),
		m_activeHeightResidency( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore ),
		m_regionResidency( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore, &m_activeHeightResidency,
			&m_cpuRegionCache, &m_instanceRegionCache, &m_middleHeightRegionCache, &m_gpuRegionCache, &m_farSurfaceAtlasCache ) {
}

T3DData::~T3DData( void ) {
	clear();
	if ( m_pCompute ) {
		memdelete( m_pCompute );
		m_pCompute = nullptr;
	}
}

T3DCompute *T3DData::getComputeBackend( void ) {
	if ( !m_pCompute ) {
		m_pCompute = memnew( T3DCompute );
	}
	return m_pCompute;
}

void T3DData::configureResidentPool( const int nCapacity ) {
	if ( !m_samplingStorage.configureResidentPool(
		m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(), m_bControlMapsImplicit,
		nCapacity, TERRAIN3D_DEFAULT_RESIDENT_POOL_BUDGET ) ) {
		LOG( T3DTerrain::WARN, "Unable to configure experimental resident region map pool" );
	}
}

void T3DData::configureGpuRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nUploadLayers, const int nUploadBudgetMb, const int nSparePages,
	const int nRetireFrames
) {
	ensureDefaultMaps();
	m_gpuRegionCache.setTerrain( m_pTerrain );
	syncCacheDefaultMaps();
	const bool bStorageChanged = m_gpuRegionCache.configure(
		bEnabled, nBudgetMb, nUploadLayers, nUploadBudgetMb, nSparePages, nRetireFrames );
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	if ( bStorageChanged && m_pTerrain && m_regionRepository.getRegionSize() > 0 ) {
		updateMaps( T3DRegion::TYPE_MAX, true, false );
	}
}


void T3DData::updateGpuRegionResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	m_farSurfaceAtlasCache.resetFrameStats();
	if ( isFarAtlasRequired() ) {
		ensureFarAtlases();
	}
	const T3DGpuRegionCacheUpdateResult result = m_gpuRegionCache.update(
		rFrame, bForce, getEffectiveGpuRegionUploadBudgetLayers(), getComputeBackendIfCreated() );
	if ( result.bPageTableChanged ) {
		emitGpuRegionMapChanged();
	}
}


void T3DData::invalidateGpuFarSurface( void ) {
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	m_farSurfaceAtlasCache.invalidateSurface();
	if ( isFarAtlasRequired() && !m_gpuRegionCache.isEnabled() ) {
		ensureFarAtlases();
	}
}


void T3DData::invalidateGpuFarAtlasLayout( void ) {
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	m_farSurfaceAtlasCache.invalidateLayout();
	if ( isFarAtlasRequired() && m_samplingStorage.isActiveOwner() &&
		m_regionRepository.getRegionSize() > 0 && m_regionRepository.getRegionMapSize() > 0 ) {
		ensureFarAtlases();
	}
}


bool T3DData::ensureFarAtlases( void ) {
	if ( !isFarAtlasRequired() || !m_samplingStorage.isActiveOwner() ||
		m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return false;
	}
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	const bool bReady = m_farSurfaceAtlasCache.ensure();
	if ( bReady && m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateFarAtlasTextures();
		}
	}
	return bReady;
}


Dictionary T3DData::getRuntimeResidencyGrid( const int nRadius ) const {
	Dictionary grid;
	const int radius = CLAMP( nRadius, 1, 15 );
	Vector2i center;
	Vector2i predicted_center;
	Dictionary streaming_stats;
	if ( m_pTerrain ) {
		streaming_stats = m_pTerrain->getRegionStreamingStats();
		center = streaming_stats["center"];
		predicted_center = streaming_stats["predicted_center"];
	}
	grid["center"] = center;
	grid["predicted_center"] = predicted_center;
	grid["radius"] = radius;
	grid["motion_direction"] = predicted_center - center;

	Array cells;
	const int diameter = radius * 2 + 1;
	cells.resize( diameter * diameter );
	int write_index = 0;
	for ( int z = center.y - radius; z <= center.y + radius; ++z ) {
		for ( int x = center.x - radius; x <= center.x + radius; ++x ) {
			const Vector2i location( x, z );
			Dictionary cell;
			cell["location"] = location;
			cell["offset"] = location - center;
			const bool bValid = isRegionLocationValidInternal( location );
			const bool bAuthored = bValid && hasRegion( location );
			const t3dregionStreamingState_t state = m_pTerrain && bValid ?
				m_pTerrain->getRegionStreamingStateInternal( location ) : TERRAIN3D_REGION_STREAMING_UNLOADED;
			const bool predicted = m_pTerrain && bValid && m_pTerrain->isRegionStreamingPrefetchInternal( location );
			cell["valid_region_coordinate"] = bValid;
			cell["active"] = bAuthored;
			cell["streaming_state"] = ( int ) ( state );
			cell["streaming_state_name"] = String( terrain3dStreamingStateName( state ) );
			cell["predicted"] = predicted;
			cell["desired_class"] = state == TERRAIN3D_REGION_STREAMING_ACTIVE ? 1 :
				( state == TERRAIN3D_REGION_STREAMING_HOT ? 2 : ( predicted ? 3 : 0 ) );
			const int gpu_slot = m_gpuRegionCache.getLayerForLocation( location );
			cell["gpu_resident"] = gpu_slot >= 0;
			cell["gpu_layer"] = gpu_slot;
			cell["middle_height_resident"] = m_middleHeightRegionCache.isResident( location );
			cell["cpu_resident"] = m_cpuRegionCache.isResident( location );
			cell["instance_resident"] = m_instanceRegionCache.isResident( location );
			cell["prepared"] = m_runtimePageStore.isPrepared( location );
			cell["prepare_pending"] = isGpuRuntimePagePreparePending( location );
			cell["gpu_slot_state"] = m_gpuRegionCache.getSlotStateForLocation( location );
			cell["far_height_dirty"] = m_farSurfaceAtlasCache.isRegionHeightDirty( location );
			cell["far_color_dirty"] = m_farSurfaceAtlasCache.isRegionColorDirty( location );
			cells[write_index++] = cell;
		}
	}
	grid["cells"] = cells;
	grid["active_count"] = streaming_stats.get( "active", 0 );
	grid["hot_count"] = streaming_stats.get( "hot", 0 );
	grid["cold_count"] = streaming_stats.get( "cold", 0 );
	grid["unloaded_count"] = streaming_stats.get( "unloaded", 0 );
	grid["predicted_count"] = streaming_stats.get( "predicted_promotions", 0 );
	grid["required_count"] = streaming_stats.get( "active", 0 );
	grid["guard_count"] = streaming_stats.get( "hot", 0 );
	grid["prefetch_count"] = streaming_stats.get( "predicted_promotions", 0 );
	return grid;
}

Dictionary T3DData::getGpuRegionCacheStatsFast( void ) const {
	Dictionary stats = m_gpuRegionCache.getStatsDictionary( false );
	const Dictionary farStats = m_farSurfaceAtlasCache.getStatsDictionary();
	const Array keys = farStats.keys();
	for ( int index = 0; index < keys.size(); ++index ) {
		stats[keys[index]] = farStats[keys[index]];
	}
	return stats;
}


bool T3DData::isAuthoringGpuFullResidencyPossible( void ) const {
	return m_gpuRegionCache.isAuthoringFullResidencyPossible();
}


bool T3DData::isAuthoringGpuReadbackSafe( void ) const {
	return m_gpuRegionCache.isAuthoringReadbackSafe();
}


Dictionary T3DData::getStartupLoadStats( void ) const {
	return m_regionPersistence.getStartupLoadStatsDictionary();
}

Dictionary T3DData::getGpuRegionCacheStats( void ) const {
	Dictionary stats = m_gpuRegionCache.getStatsDictionary( true );
	const Dictionary farStats = m_farSurfaceAtlasCache.getStatsDictionary();
	const Array keys = farStats.keys();
	for ( int index = 0; index < keys.size(); ++index ) {
		stats[keys[index]] = farStats[keys[index]];
	}
	if ( m_pTerrain ) {
		stats["far_height_atlas_side"] = m_pTerrain->getGpuFarHeightAtlasSide();
		stats["far_surface_atlas_side"] = m_pTerrain->getGpuFarSurfaceAtlasSide();
		stats["far_quality_divisor"] = m_pTerrain->getGpuFarQualityDivisor();
		stats["cooked_pages_enabled"] = m_pTerrain->isGpuRegionCookedPagesEnabled();
		stats["cook_on_save"] = m_pTerrain->isGpuRegionCookOnSave();
	}
	return stats;
}


size_t T3DData::getGpuRegionCacheReservedBytes( void ) const {
	return m_gpuRegionCache.getReservedBytes();
}


size_t T3DData::getGpuRegionCacheMinimumBytes( void ) const {
	return m_gpuRegionCache.getMinimumBytes();
}


size_t T3DData::getGpuRegionCacheHardCapBytes( void ) const {
	return m_gpuRegionCache.getHardCapBytes();
}


size_t T3DData::getCpuRegionCacheReservedBytes( void ) const {
	return m_cpuRegionCache.getReservedBytes();
}


size_t T3DData::getCpuRegionCacheMinimumBytes( void ) const {
	return m_cpuRegionCache.getMinimumBytes();
}


size_t T3DData::getCpuRegionCacheHardCapBytes( void ) const {
	return m_cpuRegionCache.getHardCapBytes();
}


size_t T3DData::getMemoryTransientStagingBytes( void ) const {
	const size_t preparedPageBytes = m_cpuRegionCache.estimateLayerBytes();
	size_t bytes = preparedPageBytes * ( size_t ) ( MAX( 0, m_runtimePageStore.getTransientPageCount() ) );
	bytes += m_mapEditService.getTransientScratchBytes();
	bytes += m_farSurfaceAtlasCache.getImageBytes();
	return bytes;
}

size_t T3DData::getMemoryFixedRepositoryBytes( void ) const {
	return sizeof( *this ) + m_samplingStorage.getActiveReservedBytes() + m_samplingStorage.getResidentReservedBytes();
}

Error T3DData::initialize( T3DTerrain *const pTerrain ) {
	if ( !pTerrain ) {
		LOG( T3DTerrain::ERROR, "Initialization failed, p_terrain is null" );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_bRuntimeActive && m_pTerrain == pTerrain && m_samplingStorage.isActiveOwner() ) {
		const uint32_t previous_generation = m_samplingStorage.getActiveGeneration();
		const uint32_t current_generation = m_samplingStorage.refreshActiveGeneration();
		if ( current_generation != 0u && current_generation != previous_generation ) {
			resetMiddleHeightCache();
			resetGpuRegionCache( true );
		}
		return OK;
	}
	LOG( T3DTerrain::INFO, "Initializing data runtime" );
	m_pTerrain = pTerrain;
	m_regionRepository.setVertexSpacing( m_pTerrain->getVertexSpacing() );

	const uint32_t previous_generation = m_samplingStorage.getActiveGeneration();
	if ( !setRegionSizeInternal( m_pTerrain->getRegionSize() ) ) {
		m_bRuntimeActive = false;
		return ERR_ALREADY_IN_USE;
	}
	const uint32_t active_generation = m_samplingStorage.refreshActiveGeneration();
	if ( active_generation == 0u ) {
		deactivateRuntimeStorage( pTerrain );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if ( previous_generation != active_generation ) {
		resetMiddleHeightCache();
		resetGpuRegionCache( true );
	}

	ensureDefaultMaps();
	const bool bLoadInitialDirectory = !m_regionRepository.isLoaded() && !m_pTerrain->getDataDirectory().is_empty();
	if ( bLoadInitialDirectory ) {
		m_bStartupBulkLoad = true;
		m_bSkipSlotReserveOnce = true;
	}

	configureCpuRegionCache( m_pTerrain->isCpuRegionCacheEffective(),
		m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(), m_pTerrain->getCpuRegionLoadBudgetLayers(),
		m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
		m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(), m_pTerrain->getInstanceRegionLoadBudgetPages(),
		m_pTerrain->getInstanceRegionLoadBudgetMb() );
	configureGpuRegionCache( m_pTerrain->isGpuRegionCacheEffective(),
		m_pTerrain->getEffectiveGpuRegionCacheBudgetMb(), m_pTerrain->getGpuRegionUploadBudgetLayers(),
		m_pTerrain->getGpuRegionUploadBudgetMb(), m_pTerrain->getGpuRegionSparePages(),
		m_pTerrain->getGpuRegionRetireFrames() );

	bool bLoadedDirectoryThisActivation = false;
	if ( bLoadInitialDirectory ) {
		const Error load_error = loadDirectory( m_pTerrain->getDataDirectory() );
		m_bStartupBulkLoad = false;
		m_bSkipSlotReserveOnce = false;
		if ( load_error != OK && load_error != ERR_FILE_NOT_FOUND ) {
			LOG( T3DTerrain::ERROR, "Failed to load terrain directory: ", m_pTerrain->getDataDirectory(), " error: ", load_error );
			deactivateRuntimeStorage( pTerrain );
			return load_error;
		}
		bLoadedDirectoryThisActivation = load_error == OK;
	}
	m_regionRepository.setLoaded( true );

	// A returning scene owns a complete repository but process-static height pages
	// were deliberately released while inactive. Rebuild the logical map from the
	// retained slot table and repopulate active storage before any renderer sees it.
	if ( !bLoadedDirectoryThisActivation && ( m_regionRepository.getActiveRegionCount() > 0 || m_regionRepository.getSlotCount() > 0 ) ) {
		const Error rehydrate_error = rehydrateActiveStorage();
		if ( rehydrate_error != OK ) {
			deactivateRuntimeStorage( pTerrain );
			return rehydrate_error;
		}
	} else {
		m_regionRepository.rebuildRegionMapFromSlots();
		m_samplingStorage.refreshActiveGeneration();
	}

	configureResidentPool( 64 );
	m_bRuntimeActive = true;
	return OK;
}

void T3DData::deactivateRuntimeStorage( T3DTerrain *const pTerrain ) {
	if ( pTerrain && m_pTerrain && pTerrain != m_pTerrain ) {
		return;
	}
	if ( !m_bRuntimeActive && !m_samplingStorage.isActiveOwner() ) {
		return;
	}
	m_bRuntimeActive = false;
	m_regionResidency.reset();
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	m_samplingStorage.resetResidentPool();
	m_samplingStorage.releaseActive();
}

Dictionary T3DData::getRuntimeState( void ) const {
	Dictionary state;
	state["terrain_initialized"] = m_pTerrain && m_pTerrain->isInitialized();
	state["repository_loaded"] = m_regionRepository.isLoaded();
	state["repository_topology_generation"] = ( int64_t ) ( m_regionRepository.getTopologyGeneration() );
	state["runtime_active"] = m_bRuntimeActive;
	state["active_storage_attached"] = m_samplingStorage.isActiveOwner();
	state["active_storage_generation"] = ( int64_t ) ( getActiveStorageGeneration() );
	state["region_map_size"] = m_regionRepository.getRegionMapSize();
	state["logical_region_count"] = m_regionRepository.getActiveRegionCount();
	state["gpu_resident_page_count"] = m_gpuRegionCache.getResidentCount();
	state["middle_height_resident_page_count"] = m_middleHeightRegionCache.getResidentCount();
	state["middle_height_page_table_valid"] = m_middleHeightRegionCache.getRegionMapRid().is_valid();
	return state;
}

void T3DData::setRegionLocations( const TypedArray<Vector2i> &vLocations ) {
	LOG( T3DTerrain::INFO, "Reconciling active region locations with array sized: ", vLocations.size() );

	// Public locations are compact while render texture slots deliberately retain
	// holes. The repository owns that stable slot topology; T3DData only performs
	// the cache/render side effects caused by the topology change.
	const bool bArraysReady = canUpdateRegionSlotsIncrementally();
	const T3DRegionLocationReconcileResult reconcile = m_regionRepository.reconcileLocations( vLocations );
	if ( reconcile.bCapacityExceeded ) {
		LOG( T3DTerrain::ERROR, "Region slot capacity exceeded while reconciling active locations" );
	}

	for ( const Vector2i location : reconcile.removedLocations ) {
		m_samplingStorage.invalidateResident( location );
		if ( m_samplingStorage.isActiveOwner() ) {
			m_samplingStorage.invalidateActive( location );
		}
		invalidateGpuRegionLocation( location );
		invalidateCpuRegionLocation( location );
		invalidateInstanceRegionLocation( location );
	}

	if ( !reconcile.bSlotsChanged ) {
		if ( m_regionRepository.isRegionMapDirty() ) {
			updateMaps( T3DRegion::TYPE_MAX, false, false );
		}
		return;
	}

	const bool bCanUploadAddedLayers = bArraysReady &&
		m_regionRepository.getSlotCount() == reconcile.nOldSlotCount;
	if ( !bCanUploadAddedLayers ) {
		updateMaps( T3DRegion::TYPE_MAX, true, false );
		if ( m_pTerrain && m_pTerrain->getInstancer() ) {
			m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, false );
		}
		invalidateSamplingCache();
		return;
	}

	m_regionRepository.rebuildRegionMapFromSlots();
	bool bTextureLayersChanged = false;
	const Vector2i *const pSlots = m_regionRepository.getSlotsData();
	for ( const int slot : reconcile.addedSlots ) {
		if ( slot < 0 || slot >= m_regionRepository.getSlotCount() ) {
			continue;
		}
		const Vector2i location = pSlots[slot];
		if ( location == V2I_MAX ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			updateMaps( T3DRegion::TYPE_MAX, true, false );
			if ( m_pTerrain && m_pTerrain->getInstancer() ) {
				m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, false );
			}
			invalidateSamplingCache();
			return;
		}

		if ( !syncActiveRegion( location, pRegion ) ) {
			LOG( T3DTerrain::ERROR, "Unable to restore active terrain storage for region ", location );
			continue;
		}
		ensureDefaultMaps();
		m_heightMaps[slot] = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
		m_controlMaps[slot] = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_CONTROL );
		m_colorMaps[slot] = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR );
		Ref<Image> pDerived = pRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( {location} );
			pDerived = pRegion->getDerivedNormalSlopeMap();
		}
		pDerived = pDerived.is_valid() ? pDerived : m_pDefaultDerivedMap;
		m_derivedMaps[slot] = pDerived;
		m_generatedHeightMaps.update( m_heightMaps[slot], slot );
		m_generatedControlMaps.update( makeGpuRenderControlImage( location ), slot );
		m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[slot] ), slot );
		m_generatedDerivedMaps.update( pDerived, slot );
		if ( !IS_EDITOR ) {
			m_samplingStorage.syncResidentDerived( location, pRegion );
			pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
			m_derivedMaps[slot] = Ref<Image>();
		}
		bTextureLayersChanged = true;
	}

	calcHeightRange();
	emitRegionLayoutChanged( bTextureLayersChanged );
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, false );
	}
	invalidateSamplingCache();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( T3DTerrain::ERROR, "Region repository invariant failure after setRegionLocations" );
	}
#endif
}

// Returns an array of active regions, optionally a shallow or deep copy
TypedArray<Vector2i> T3DData::getRegionSlots( void ) const {
	return m_regionRepository.getSlots();
}

TypedArray<Vector2i> T3DData::getRenderRegionSlots( void ) const {
	return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getRenderRegionSlots() : getRegionSlots();
}

PackedInt32Array T3DData::getRegionMap( void ) const {
	PackedInt32Array region_map;
	if ( m_regionRepository.getRegionMapSize() <= 0 ) {
		return region_map;
	}
	region_map.resize( m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
	int32_t *pWrite = region_map.ptrw();
	const int half = m_regionRepository.getRegionMapSize() / 2;
	const uint16_t *pSourceMap = m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getRegionMapData() : m_regionRepository.getLogicalRegionMapData();
	for ( int y = 0; y < m_regionRepository.getRegionMapSize(); ++y ) {
		for ( int x = 0; x < m_regionRepository.getRegionMapSize(); ++x ) {
			const Vector2i location( x - half, y - half );
			const int source_index = getRegionMapIndex( location );
			pWrite[y * m_regionRepository.getRegionMapSize() + x] = source_index >= 0 ? pSourceMap[source_index] : 0;
		}
	}
	return region_map;
}

PackedInt32Array T3DData::getLogicalRegionMap( void ) const {
	return m_regionRepository.getLogicalRegionMapPacked();
}

TypedArray<T3DRegion> T3DData::getRegionsActive( const bool bCopy, const bool bDeep ) const {
	TypedArray<T3DRegion> region_arr;
	if ( bCopy && m_cpuRegionCache.isEnabled() ) {
		LOG( T3DTerrain::ERROR, "Whole-repository region copies are unavailable while bounded CPU residency is enabled. "
			"Disable the CPU cache before requesting copied Terrain3DRegion resources." );
		return region_arr;
	}
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i region_loc = m_regionRepository.getSlotsData()[slot];
		Ref<T3DRegion> pRegion = getRegion( region_loc );
		if ( pRegion.is_valid() ) {
			region_arr.push_back( bCopy ? pRegion->duplicate( bDeep ) : pRegion );
		}
	}
	return region_arr;
}

// Calls the callback function for every region within the given (descaled) area
// The callable receives: source Terrain3DRegion, source Rect2i, dest Rect2i, (bindings)
// Used with change_region_size, dest Terrain3DRegion is bound as the 4th parameter
void T3DData::doForRegions( const Rect2i &rArea, const Callable &rCallback ) {
	const Rect2i location_bounds = getRegionBoundsForArea( rArea, m_regionRepository.getRegionSize() );
	if ( location_bounds.size.x <= 0 || location_bounds.size.y <= 0 ) {
		return;
	}
	LOG( T3DTerrain::DEBUG, "Processing global area: ", rArea, " -> ", location_bounds );
	Point2i current_region_loc;
	for ( int y = location_bounds.position.y; y < location_bounds.get_end().y; y++ ) {
		current_region_loc.y = y;
		for ( int x = location_bounds.position.x; x < location_bounds.get_end().x; x++ ) {
			current_region_loc.x = x;
			const T3DRegion *pRegion = getRegionPtr( current_region_loc );
			if ( pRegion && !pRegion->isDeleted() ) {
				if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResident( current_region_loc ) ) {
					LOG( T3DTerrain::ERROR, "Unable to page CPU terrain maps for region callback: ", current_region_loc );
					continue;
				}
				pRegion = getRegionPtr( current_region_loc );
				LOG( T3DTerrain::DEBUG, "Current region: ", current_region_loc );
				const Rect2i regionArea = rArea.intersection( Rect2i( current_region_loc * m_regionRepository.getRegionSize(), m_regionRepository.getRegionSizeVector() ) );
				LOG( T3DTerrain::DEBUG, "Region bounds: ", Rect2i( current_region_loc * m_regionRepository.getRegionSize(), m_regionRepository.getRegionSizeVector() ) );
				LOG( T3DTerrain::DEBUG, "Region area: ", regionArea );
				const Rect2i dstCoords( regionArea.position - rArea.position, regionArea.size );
				const Rect2i srcCoords( regionArea.position - ( pRegion->getLocation() * m_regionRepository.getRegionSizeVector() ), dstCoords.size );
				LOG( T3DTerrain::DEBUG, "src map coords: ", srcCoords );
				LOG( T3DTerrain::DEBUG, "dst map coords: ", dstCoords );
				rCallback.call( pRegion, srcCoords, dstCoords );
			}
		}
	}
}

void T3DData::changeRegionSize( const int nNewSize ) {
	LOG( T3DTerrain::INFO, "Changing region size from: ", m_regionRepository.getRegionSize(), " to ", nNewSize );
	if ( !m_pTerrain ) {
		LOG( T3DTerrain::WARN, "Region-size migration requested before Terrain3DData has an initialized Terrain3D owner. "
			"Deferring migration until Terrain3D initialization." );
		return;
	}
	if ( nNewSize != 128 ) {
		LOG( T3DTerrain::ERROR, "This Terrain3D fork is specialized for 128-sample regions and rejects all other sizes" );
		return;
	}
	if ( !isValidRegionSize( nNewSize ) ) {
		LOG( T3DTerrain::ERROR, "Invalid region size: ", nNewSize, ". Must be power of 2, 64-2048" );
		return;
	}
	if ( nNewSize == m_regionRepository.getRegionSize() ) {
		return;
	}

	const TypedArray<T3DRegion> old_regions = getRegionsActive();
	if ( old_regions.is_empty() ) {
		m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( nNewSize ) );
		return;
	}

	// Preflight the complete conversion before allocating or deleting anything.
	// The world remains 4096x4096 texels; changing region size changes only the
	// number of logical regions that partition that fixed extent.
	Vector2i worldMin( INT32_MAX, INT32_MAX );
	Vector2i worldMax( INT32_MIN, INT32_MIN );
	for ( const Ref<T3DRegion> pRegion : old_regions ) {
		if ( pRegion.is_null() ) {
			continue;
		}
		const Vector2i region_begin = pRegion->getLocation() * m_regionRepository.getRegionSize();
		const Vector2i region_end = region_begin + m_regionRepository.getRegionSizeVector();
		worldMin.x = MIN( worldMin.x, region_begin.x );
		worldMin.y = MIN( worldMin.y, region_begin.y );
		worldMax.x = MAX( worldMax.x, region_end.x );
		worldMax.y = MAX( worldMax.y, region_end.y );
	}

	const Rect2i converted_bounds = getRegionBoundsForArea( Rect2i( worldMin, worldMax - worldMin ), nNewSize );
	const Vector2i converted_last = converted_bounds.get_end() - Vector2i( 1, 1 );
	const int target_map_size = TERRAIN3D_ACTIVE_WORLD_TEXELS / nNewSize;
	if ( !terrain3dRegionLocationValid( converted_bounds.position, target_map_size ) ||
		!terrain3dRegionLocationValid( converted_last, target_map_size ) ) {
		LOG( T3DTerrain::ERROR, "Cannot change region size from ", m_regionRepository.getRegionSize(), " to ", nNewSize,
			" without losing data. Converted region coordinates would span ", converted_bounds.position, " through ",
			converted_last, ", but valid coordinates are ", -target_map_size / 2, " through ",
			target_map_size / 2 - 1, ". Source data was left unchanged." );
		return;
	}

	// Copy each source region directly into its intersecting destination regions.
	// This removes Callable/Variant dispatch and repeated source-region lookups
	// from the hot path, which is particularly costly when splitting down to 64.
	Dictionary new_regions_by_location;
	TypedArray<T3DRegion> new_regions;
	for ( const Ref<T3DRegion> pSrcRegion : old_regions ) {
		if ( pSrcRegion.is_null() ) {
			continue;
		}
		if ( !ensureCpuRegionResident( pSrcRegion->getLocation() ) ) {
			LOG( T3DTerrain::ERROR, "Unable to page source region during region-size conversion: ", pSrcRegion->getLocation() );
			return;
		}

		const Rect2i srcArea( pSrcRegion->getLocation() * m_regionRepository.getRegionSize(), m_regionRepository.getRegionSizeVector() );
		const Rect2i target_bounds = getRegionBoundsForArea( srcArea, nNewSize );
		for ( int y = target_bounds.position.y; y < target_bounds.get_end().y; y++ ) {
			for ( int x = target_bounds.position.x; x < target_bounds.get_end().x; x++ ) {
				const Vector2i targetLocation( x, y );
				Ref<T3DRegion> pDstRegion;
				if ( new_regions_by_location.has( targetLocation ) ) {
					pDstRegion = new_regions_by_location[targetLocation];
				} else {
					pDstRegion.instantiate();
					pDstRegion->setLocation( targetLocation );
					pDstRegion->setRegionSize( nNewSize );
					pDstRegion->setVertexSpacing( m_regionRepository.getVertexSpacing() );
					pDstRegion->setModified( true );
					pDstRegion->sanitizeMaps();
					new_regions_by_location[targetLocation] = pDstRegion;
					new_regions.push_back( pDstRegion );
				}

				const Rect2i dstArea( targetLocation * nNewSize, V2I( nNewSize ) );
				const Rect2i overlap = srcArea.intersection( dstArea );
				if ( overlap.size.x <= 0 || overlap.size.y <= 0 ) {
					continue;
				}

				const Rect2i srcRect( overlap.position - srcArea.position, overlap.size );
				const Rect2i dstRect( overlap.position - dstArea.position, overlap.size );
				copyPasteDfr( pSrcRegion.ptr(), srcRect, dstRect, pDstRegion.ptr() );
			}
		}
	}

	// Remove old data
	m_pTerrain->getInstancer()->destroy();
	for ( const Ref<T3DRegion> pRegion : old_regions ) {
		removeRegion( pRegion, false );
	}

	// Change region size
	m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( nNewSize ) );

	// Add new regions and rebuild
	for ( const Ref<T3DRegion> pRegion : new_regions ) {
		addRegion( pRegion, false );
	}

	calcHeightRange( true );
	updateMaps( T3DRegion::TYPE_MAX, true, true );
	m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, true );
}


void T3DData::setRegionModified( const Vector2i &rRegionLoc, const bool bModified ) {
	T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setModified( bModified );
}

bool T3DData::isRegionModified( const Vector2i &rRegionLoc ) const {
	T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return false;
	}
	return pRegion->isModified();
}

void T3DData::setRegionDeleted( const Vector2i &rRegionLoc, const bool bDeleted ) {
	T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setDeleted( bDeleted );
}

bool T3DData::isRegionDeleted( const Vector2i &rRegionLoc ) const {
	const T3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( T3DTerrain::ERROR, "Region not found at: ", rRegionLoc );
		return true;
	}
	return pRegion->isDeleted();
}

Ref<T3DRegion> T3DData::addRegionBlankp( const Vector3 &rGlobalPosition, const bool bUpdate ) {
	return addRegionBlank( getRegionLocation( rGlobalPosition ), bUpdate );
}

Ref<T3DRegion> T3DData::addRegionBlank( const Vector2i &rRegionLoc, const bool bUpdate ) {
	Ref<T3DRegion> pRegion;
	pRegion.instantiate();
	pRegion->setLocation( rRegionLoc );
	pRegion->setRegionSize( m_regionRepository.getRegionSize() );
	pRegion->setVertexSpacing( m_regionRepository.getVertexSpacing() );
	if ( addRegion( pRegion, bUpdate ) == OK ) {
		pRegion->setModified( true );
		return pRegion;
	}
	return Ref<T3DRegion>();
}

/** Adds a Terrain3DRegion to the terrain
 * Marks region as modified
 *	p_update - rebuild the maps if true. Set to false if bulk adding many regions.
 */
Error T3DData::addRegion( const Ref<T3DRegion> &rRegion, const bool bUpdate ) {
	if ( rRegion.is_null() ) {
		return ERR_INVALID_PARAMETER;
	}
	return addRegionInternal( rRegion, bUpdate, true );
}

Error T3DData::replaceRegionData( const Vector2i &rRegionLoc, const Dictionary &dictData ) {
	if ( !ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( T3DTerrain::ERROR, "Cannot load CPU map payload for replacement region: ", rRegionLoc );
		return ERR_CANT_OPEN;
	}
	Ref<T3DRegion> pCurrent = getRegion( rRegionLoc );
	if ( pCurrent.is_null() || pCurrent->isDeleted() || getRegionId( rRegionLoc ) < 0 ) {
		LOG( T3DTerrain::ERROR, "Cannot replace data for inactive region: ", rRegionLoc );
		return ERR_DOES_NOT_EXIST;
	}

	// Mutate a detached deep copy first. If validation fails, the currently active
	// resource, render state, collision, and GPU layers remain untouched.
	Ref<T3DRegion> pReplacement = pCurrent->duplicate( true );
	if ( pReplacement.is_null() ) {
		return ERR_CANT_CREATE;
	}
	pReplacement->setData( dictData );
	pReplacement->setLocation( rRegionLoc ); // Repository location is authoritative.
	pReplacement->setDeleted( false );
	pReplacement->setEdited( false );
	pReplacement->setVertexSpacing( m_regionRepository.getVertexSpacing() );
	if ( pReplacement->getRegionSize() != m_regionRepository.getRegionSize() ) {
		LOG( T3DTerrain::ERROR, "Replacement region size ", pReplacement->getRegionSize(), " does not match terrain region size ",
			m_regionRepository.getRegionSize() );
		return ERR_INVALID_DATA;
	}
	pReplacement->sanitizeMaps();
	if ( !pReplacement->validateMapSize( pReplacement->getHeightMap() ) ||
		( pReplacement->getControlMap().is_valid() && !pReplacement->validateMapSize( pReplacement->getControlMap() ) ) ||
		( pReplacement->getColorMap().is_valid() && !pReplacement->validateMapSize( pReplacement->getColorMap() ) ) ) {
		LOG( T3DTerrain::ERROR, "Replacement snapshot contains invalid map dimensions" );
		return ERR_INVALID_DATA;
	}
	pReplacement->setModified( true );
	return addRegionInternal( pReplacement, true, false );
}

Error T3DData::addRegionInternal(
	const Ref<T3DRegion> &rRegion, const bool bUpdate,
	const bool bSanitize
) {
	if ( rRegion.is_null() ) {
		LOG( T3DTerrain::ERROR, "Provided region is null. Returning" );
		return FAILED;
	}
	const Vector2i regionLoc = rRegion->getLocation();
	LOG( T3DTerrain::DEBUG, "Adding region at location ", regionLoc, ", update maps: ", bUpdate ? "yes" : "no" );

	if ( !isRegionLocationValidInternal( regionLoc ) ) {
		LOG( T3DTerrain::ERROR, "Location ", regionLoc, " out of active bounds. Max: ",
			-m_regionRepository.getRegionMapSize() / 2, " to ", m_regionRepository.getRegionMapSize() / 2 - 1 );
		return FAILED;
	}

	if ( bSanitize ) {
		rRegion->sanitizeMaps();
	}
	rRegion->setDeleted( false );

	int existingSlot = getRegionId( regionLoc );
	const Ref<T3DRegion> pPriorRegion = m_regionRepository.getRegion( regionLoc );
	T3DRegion *const pStoredRegion = pPriorRegion.ptr();
	const bool bHadPriorRegionResource = pStoredRegion != nullptr;
	if ( existingSlot < 0 && pStoredRegion && !pStoredRegion->isDeleted() ) {
		existingSlot = m_regionRepository.findSlot( regionLoc );
	}
	const bool bReplacingExistingRegion = existingSlot >= 0;
	if ( !bReplacingExistingRegion ) {
		const int maximumRegions = m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize();
		if ( m_regionRepository.getActiveRegionCount() >= maximumRegions ) {
			LOG( T3DTerrain::ERROR, "Region slot capacity exceeded while adding region ", regionLoc );
			return ERR_OUT_OF_MEMORY;
		}
	}

	if ( !syncActiveRegion( regionLoc, rRegion.ptr() ) ) {
		if ( pStoredRegion && !pStoredRegion->isDeleted() ) {
			syncActiveRegion( regionLoc, pStoredRegion );
		}
		LOG( T3DTerrain::ERROR, "Unable to publish region ", regionLoc, " into process-static active storage" );
		return ERR_INVALID_DATA;
	}

	const bool bRequiresInstanceReconciliation = bHadPriorRegionResource || rRegion->hasInstances();
	m_samplingStorage.invalidateResident( regionLoc );
	if ( !m_regionRepository.storeRegion( regionLoc, rRegion ) ) {
		if ( pPriorRegion.is_valid() && !pPriorRegion->isDeleted() ) {
			syncActiveRegion( regionLoc, pPriorRegion.ptr() );
		} else if ( m_samplingStorage.isActiveOwner() ) {
			m_samplingStorage.invalidateActive( regionLoc );
		}
		LOG( T3DTerrain::ERROR, "Region repository rejected region publication at ", regionLoc );
		return ERR_INVALID_DATA;
	}

	// Replacing an active resource does not alter topology or texture-array layout.
	if ( bReplacingExistingRegion && bUpdate && canUpdateRegionSlotsIncrementally() ) {
		ensureDefaultMaps();
		m_heightMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_HEIGHT );
		m_controlMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_CONTROL );
		m_colorMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_COLOR );
		Ref<Image> pDerived = rRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( {regionLoc} );
			pDerived = rRegion->getDerivedNormalSlopeMap();
		}
		if ( pDerived.is_null() ) {
			pDerived = m_pDefaultDerivedMap;
		}
		m_derivedMaps[existingSlot] = pDerived;
		m_generatedHeightMaps.update( m_heightMaps[existingSlot], existingSlot );
		m_generatedControlMaps.update( makeGpuRenderControlImage( regionLoc ), existingSlot );
		m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[existingSlot] ), existingSlot );
		m_generatedDerivedMaps.update( pDerived, existingSlot );
		calcHeightRange();
		emitRegionLayoutChanged( true );
		m_pTerrain->getInstancer()->updateMmis( -1, regionLoc, false );
		invalidateSamplingCache();
		return OK;
	}

	if ( !bReplacingExistingRegion ) {
		const int newSlot = m_regionRepository.activateLocation( regionLoc, bUpdate );
		if ( newSlot < 0 ) {
			if ( pPriorRegion.is_valid() ) {
				( void ) m_regionRepository.storeRegion( regionLoc, pPriorRegion );
				if ( !pPriorRegion->isDeleted() ) {
					syncActiveRegion( regionLoc, pPriorRegion.ptr() );
				}
			} else {
				m_regionRepository.eraseRegionResource( regionLoc );
				if ( m_samplingStorage.isActiveOwner() ) {
					m_samplingStorage.invalidateActive( regionLoc );
				}
			}
			LOG( T3DTerrain::ERROR, "Region slot capacity exceeded while adding region ", regionLoc );
			return ERR_OUT_OF_MEMORY;
		}

		const bool bCanUploadSingleLayer = bUpdate && canUpdateRegionSlotsIncrementally();
		if ( bCanUploadSingleLayer ) {
			ensureDefaultMaps();
			m_heightMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_HEIGHT );
			m_controlMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_CONTROL );
			m_colorMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), T3DRegion::TYPE_COLOR );
			regenerateDerivedMaps( {regionLoc} );
			Ref<Image> pDerived = rRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() ) {
				pDerived = m_pDefaultDerivedMap;
			}
			m_derivedMaps[newSlot] = pDerived;
			m_generatedHeightMaps.update( m_heightMaps[newSlot], newSlot );
			m_generatedControlMaps.update( makeGpuRenderControlImage( regionLoc ), newSlot );
			m_generatedColorMaps.update( makeGpuColorImage( m_colorMaps[newSlot] ), newSlot );
			m_generatedDerivedMaps.update( pDerived, newSlot );
			updateMasterHeights( rRegion->getHeightRange() );
			emitRegionLayoutChanged( true );
			if ( bRequiresInstanceReconciliation ) {
				m_pTerrain->getInstancer()->updateMmis( -1, regionLoc, false );
			}
			invalidateSamplingCache();
#ifdef DEBUG_ENABLED
			if ( !m_regionRepository.validateInvariants() ) {
				LOG( T3DTerrain::ERROR, "Region repository invariant failure after incremental add" );
			}
#endif
			return OK;
		}
		m_regionRepository.markRegionMapDirty();
	}

	LOG( T3DTerrain::DEBUG, "Storing region ", regionLoc, " version ", vformat( "%.3f", rRegion->getVersion() ),
		" active count: ", m_regionRepository.getActiveRegionCount(), ", slots: ", m_regionRepository.getSlotCount() );
	if ( bUpdate ) {
		updateMaps( T3DRegion::TYPE_MAX, true, false );
		if ( bRequiresInstanceReconciliation ) {
			m_pTerrain->getInstancer()->updateMmis( -1, regionLoc, false );
		}
	}
	invalidateSamplingCache();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( T3DTerrain::ERROR, "Region repository invariant failure after addRegionInternal" );
	}
#endif
	return OK;
}

void T3DData::removeRegionp( const Vector3 &rGlobalPosition, const bool bUpdate ) {
	const Ref<T3DRegion> pRegion = getRegion( getRegionLocation( rGlobalPosition ) );
	removeRegion( pRegion, bUpdate );
}

void T3DData::removeRegionl( const Vector2i &rRegionLoc, const bool bUpdate ) {
	const Ref<T3DRegion> pRegion = getRegion( rRegionLoc );
	removeRegion( pRegion, bUpdate );
}

// Remove region marks the region for deletion, and removes it from the active arrays indexed by ID
// It remains stored in _regions and the file remains on disk until saved, when both are removed
void T3DData::removeRegion( const Ref<T3DRegion> &rRegion, const bool bUpdate ) {
	if ( rRegion.is_null() ) {
		LOG( T3DTerrain::ERROR, "Region not found or is null. Returning" );
		return;
	}

	const Vector2i regionLoc = rRegion->getLocation();
	const int regionSlot = m_regionRepository.findSlot( regionLoc );
	LOG( T3DTerrain::INFO, "Marking region ", regionLoc, " for deletion. update maps: ", bUpdate ? "yes" : "no" );
	if ( regionSlot < 0 ) {
		LOG( T3DTerrain::ERROR, "Region ", regionLoc, " not found in active region slots. Returning" );
		return;
	}
	m_samplingStorage.invalidateResident( regionLoc );
	if ( m_samplingStorage.isActiveOwner() ) {
		m_samplingStorage.invalidateActive( regionLoc );
	}
	invalidateGpuRegionLocation( regionLoc );
	invalidateCpuRegionLocation( regionLoc );
	invalidateInstanceRegionLocation( regionLoc );

	rRegion->setDeleted( true );
	if ( m_regionRepository.deactivateLocation( regionLoc ) < 0 ) {
		LOG( T3DTerrain::ERROR, "Region repository failed to deactivate ", regionLoc );
		return;
	}
	markGpuFarRegionDirty( regionLoc, T3DRegion::TYPE_MAX );
	calcHeightRange();

	invalidateSamplingCache();
	if ( bUpdate ) {
		emitRegionLayoutChanged( false );
		m_pTerrain->getInstancer()->destroyRegionMmis( regionLoc );
	}
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( T3DTerrain::ERROR, "Region repository invariant failure after removeRegion" );
	}
#endif
}

bool T3DData::ensurePersistenceRegionResident( void *const pContext, const Vector2i &rLocation ) {
	T3DData *const pData = static_cast<T3DData *>( pContext );
	return pData && pData->ensureCpuRegionResidentInternal( rLocation, true );
}

void T3DData::processRegionSaveResult( const T3DRegionSaveResult &rResult ) {
	if ( rResult.bDeleted && !m_runtimePageStore.removePageForSource( rResult.path ) ) {
		LOG( T3DTerrain::WARN, "Region deleted, but cooked runtime page could not be removed: ", rResult.path );
	}
	if ( !rResult.bSourceChanged || !m_pTerrain || !m_pTerrain->isGpuRegionCookOnSave() ) {
		return;
	}

	// The compiled render-control field uses a two-texel cross-region halo. A
	// source edit or deletion can change immediate neighbors even when their
	// authored resources did not change, so refresh the whole 3x3 source area.
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			if ( rResult.bDeleted && dx == 0 && dz == 0 ) {
				continue;
			}
			const Vector2i cookLocation = rResult.location + Vector2i( dx, dz );
			T3DRegion *const pCookRegion = getRegionPtr( cookLocation );
			if ( !pCookRegion || pCookRegion->isDeleted() ) {
				continue;
			}
			if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResidentInternal( cookLocation, true ) ) {
				LOG( T3DTerrain::ERROR, rResult.bDeleted ?
					"Region deleted, but neighboring runtime page source could not be loaded: " :
					"Region saved, but neighboring runtime page source could not be loaded: ", cookLocation );
				continue;
			}
			if ( !writeGpuRuntimePage( pCookRegion ) ) {
				LOG( T3DTerrain::ERROR, rResult.bDeleted ?
					"Region deleted, but neighboring runtime GPU page cooking failed: " :
					"Region saved, but runtime GPU page cooking failed: ", cookLocation );
			}
		}
	}
}

void T3DData::saveDirectory( const String &rDir ) {
	LOG( T3DTerrain::INFO, "Saving data files to ", rDir );
	const T3DDirectorySaveResult saveResult = m_regionPersistence.saveDirectory(
		rDir, m_pTerrain->getSave16Bit(), &T3DData::ensurePersistenceRegionResident, this );
	for ( const T3DRegionSaveResult &regionResult : saveResult.regions ) {
		processRegionSaveResult( regionResult );
	}
	if ( m_pTerrain && m_pTerrain->isGpuRegionCookOnSave() && !writeCookedGpuFarAtlases( rDir ) ) {
		LOG( T3DTerrain::ERROR, "Terrain regions saved, but cooked far-atlas generation failed" );
	}
	// Persistence updates only the exact resource paths Terrain3D changed. Do not
	// force a project-wide EditorFileSystem scan from a scene-save callback.
}

void T3DData::saveRegion( const Vector2i &rRegionLoc, const String &rDir, const bool b16Bit ) {
	const T3DRegionSaveResult result = m_regionPersistence.saveRegion(
		rRegionLoc, rDir, b16Bit, &T3DData::ensurePersistenceRegionResident, this );
	processRegionSaveResult( result );
}

bool T3DData::collectGpuRegionPageSources(
	String &rDataDirectory, PackedStringArray &rSourceFiles,
	eastl::vector<Vector2i> &rLocations, PackedStringArray &rErrors
) const {
	rDataDirectory = m_pTerrain ? m_pTerrain->getDataDirectory().trim_suffix( "/" ) : String();
	if ( !m_pTerrain ) {
		rSourceFiles.clear();
		rLocations.clear();
		rErrors.clear();
		rErrors.push_back( "Terrain3D owner is unavailable" );
		return false;
	}
	return m_runtimePageStore.collectPageSources( rDataDirectory, rSourceFiles, rLocations, rErrors );
}

Dictionary T3DData::inspectGpuRegionPages( void ) {
	Dictionary result;
	PackedStringArray source_files;
	PackedStringArray errors;
	eastl::vector<Vector2i> locations;
	String data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int discovered = source_files.size();

	result["data_directory"] = data_dir;
	result["discovered"] = discovered;
	result["valid"] = ( int ) ( locations.size() );
	result["preflight_failed"] = !preflight_ok;
	if ( !preflight_ok ) {
		result["current"] = 0;
		result["missing"] = 0;
		result["stale"] = 0;
		result["invalid"] = 0;
		result["source_unavailable"] = errors.size();
		result["needs_cook"] = discovered;
		result["orphan_count"] = 0;
		result["orphan_pages"] = PackedStringArray();
		result["needs_cook_regions"] = PackedStringArray();
		result["errors"] = errors;
		result["complete"] = false;
		return result;
	}

	PackedStringArray needs_cook_regions;
	PackedStringArray orphan_pages;
	int current = 0;
	int missing = 0;
	int stale = 0;
	int invalid = 0;
	int source_unavailable = 0;
	for ( const Vector2i location : locations ) {
		T3DRegion *const pRegion = getRegionPtr( location );
		const String source_path = data_dir.path_join( Util::locationToFilename( location ) );
		const String page_path = getGpuRuntimePagePath( pRegion );
		if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResidentInternal( location, true ) ) {
			++source_unavailable;
			needs_cook_regions.push_back( String( location ) );
			errors.push_back( "Region " + String( location ) + ": unable to materialize source maps" );
			continue;
		}

		const uint64_t expected_signature = computeGpuRuntimePageSourceSignature( pRegion );
		if ( expected_signature == 0u ) {
			++source_unavailable;
			needs_cook_regions.push_back( String( location ) );
			errors.push_back( "Region " + String( location ) + ": source signature generation failed" );
			continue;
		}
		if ( !FileAccess::file_exists( page_path ) ) {
			++missing;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}

		uint64_t existing_signature = 0u;
		if ( !readGpuRuntimePageSourceSignature( page_path, existing_signature ) ) {
			++invalid;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}
		if ( existing_signature != expected_signature ) {
			++stale;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}

		if ( !m_runtimePageStore.validatePageForRegion( pRegion, getRuntimePageConfig() ) ) {
			++invalid;
			needs_cook_regions.push_back( String( location ) );
			continue;
		}
		++current;
	}

	PackedStringArray page_files = Util::getFiles( data_dir, "terrain3d*.t3dpage" );
	page_files.sort();
	for ( const String &fname : page_files ) {
		const String source_name = fname.get_basename() + String( ".res" );
		const String source_path = data_dir.path_join( source_name );
		if ( !FileAccess::file_exists( source_path ) ) {
			orphan_pages.push_back( data_dir.path_join( fname ) );
		}
	}

	releaseUntrackedCpuMaps();
	const int needs_cook = missing + stale + invalid + source_unavailable;
	result["current"] = current;
	result["missing"] = missing;
	result["stale"] = stale;
	result["invalid"] = invalid;
	result["source_unavailable"] = source_unavailable;
	result["needs_cook"] = needs_cook;
	result["orphan_count"] = orphan_pages.size();
	result["orphan_pages"] = orphan_pages;
	result["needs_cook_regions"] = needs_cook_regions;
	result["errors"] = errors;
	result["complete"] = source_unavailable == 0 && current + missing + stale + invalid == discovered;
	return result;
}

Dictionary T3DData::cookGpuRegionPages( void ) {
	Dictionary result;
	PackedStringArray source_files;
	PackedStringArray errors;
	eastl::vector<Vector2i> locations;
	String data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int discovered = source_files.size();
	const int valid = ( int ) ( locations.size() );
	if ( !preflight_ok ) {
		releaseUntrackedCpuMaps();
		result["data_directory"] = data_dir;
		result["far_atlas_cooked"] = false;
		result["material_cache"] = Dictionary();
		result["material_cache_cooked"] = false;
		result["preflight_failed"] = true;
		result["discovered"] = discovered;
		result["valid"] = valid;
		result["cooked"] = 0;
		result["unchanged"] = 0;
		result["usable"] = 0;
		result["skipped"] = 0;
		result["failed"] = errors.size();
		result["orphan_removed"] = 0;
		result["orphan_failed"] = 0;
		result["region_count"] = m_regionRepository.getActiveRegionCount();
		result["complete"] = false;
		result["errors"] = errors;
		return result;
	}

	int cooked = 0;
	int unchanged = 0;
	int failed = 0;
	int orphan_removed = 0;
	int orphan_failed = 0;
	for ( const Vector2i location : locations ) {
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResidentInternal( location, true ) ) {
			++failed;
			errors.push_back( "Region " + String( location ) + ": unable to materialize source maps" );
			continue;
		}

		const uint64_t expected_signature = computeGpuRuntimePageSourceSignature( pRegion );
		if ( expected_signature == 0u ) {
			++failed;
			errors.push_back( "Region " + String( location ) + ": source signature generation failed" );
			continue;
		}
		const String source_path = data_dir.path_join( Util::locationToFilename( location ) );
		const String page_path = getGpuRuntimePagePath( pRegion );
		uint64_t existing_signature = 0u;
		bool bCurrent = readGpuRuntimePageSourceSignature( page_path, existing_signature ) &&
			existing_signature == expected_signature;
		if ( bCurrent ) {
			bCurrent = m_runtimePageStore.validatePageForRegion( pRegion, getRuntimePageConfig() );
		}
		if ( bCurrent ) {
			++unchanged;
			continue;
		}
		String cook_error;
		if ( writeGpuRuntimePage( pRegion, &cook_error ) ) {
			++cooked;
		} else {
			++failed;
			errors.push_back( "Region " + String( location ) + ": " +
				( cook_error.is_empty() ? String( "cook failed" ) : cook_error ) );
		}
	}

	PackedStringArray page_files = Util::getFiles( data_dir, "terrain3d*.t3dpage" );
	for ( const String &fname : page_files ) {
		const String source_name = fname.get_basename() + String( ".res" );
		const String source_path = data_dir.path_join( source_name );
		if ( FileAccess::file_exists( source_path ) ) {
			continue;
		}
		const String orphan_path = data_dir.path_join( fname );
		if ( m_runtimePageStore.removePage( orphan_path ) ) {
			++orphan_removed;
		} else {
			++orphan_failed;
			errors.push_back( "Unable to remove orphan runtime page: " + orphan_path );
		}
	}

	const bool bFarAtlasCooked = writeCookedGpuFarAtlases();
	if ( !bFarAtlasCooked ) {
		errors.push_back( "Far atlas cook failed" );
	}
	Dictionary material_cache_result;
	bool bMaterialCacheCooked = true;
	if ( m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled() ) {
		const Ref<T3DAssets> pAssets = m_pTerrain->getAssets();
		if ( pAssets.is_valid() ) {
			material_cache_result = pAssets->cookRuntimeMaterialCache();
			bMaterialCacheCooked = ( bool ) ( material_cache_result.get( "complete", false ) );
			if ( !bMaterialCacheCooked ) {
				errors.push_back( "Runtime material cache cook failed: " +
					String( material_cache_result.get( "error", "unknown error" ) ) );
			}
		}
	}
	releaseUntrackedCpuMaps();
	const int usable = cooked + unchanged;
	result["data_directory"] = data_dir;
	result["far_atlas_cooked"] = bFarAtlasCooked;
	result["material_cache"] = material_cache_result;
	result["material_cache_cooked"] = bMaterialCacheCooked;
	result["preflight_failed"] = false;
	result["discovered"] = discovered;
	result["valid"] = valid;
	result["cooked"] = cooked;
	result["unchanged"] = unchanged;
	result["usable"] = usable;
	result["skipped"] = 0;
	result["failed"] = failed;
	result["orphan_removed"] = orphan_removed;
	result["orphan_failed"] = orphan_failed;
	result["region_count"] = m_regionRepository.getActiveRegionCount();
	result["complete"] = failed == 0 && usable == valid && valid == discovered &&
		orphan_failed == 0 && bFarAtlasCooked && bMaterialCacheCooked;
	result["errors"] = errors;
	return result;
}

Error T3DData::loadDirectory( const String &rDir ) {
	T3DStagedRepository staged;
	const bool bBoundedCpuLoad = m_cpuRegionCache.isEnabled();
	LOG( T3DTerrain::INFO, "[terrain-data] Candidate repository load; CPU cache ",
		bBoundedCpuLoad ? "bounded" : "authoring/unbounded", "; GPU cache ",
		m_gpuRegionCache.isEnabled() ? "enabled" : "disabled" );
	const Error stageError = m_regionPersistence.stageDirectory( rDir, bBoundedCpuLoad, staged );
	if ( stageError != OK ) {
		return stageError;
	}

	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 3/4: publishing active storage and region ownership" );
	m_regionPersistence.beginPublication();

	// Publish the complete validated repository in one state transition. Generated
	// textures and cache mirrors are rebuilt from this complete state afterward.
	m_samplingStorage.resetResidentPool();
	const int previousRegionSize = m_regionRepository.getRegionSize();
	bool activeStorageReady = false;
	if ( bBoundedCpuLoad ) {
		const uint8_t *pStagedHeightPages[TERRAIN3D_MAX_REGION_SLOTS] = {};
		int stagedHeightSlots[TERRAIN3D_MAX_REGION_SLOTS] = {};
		size_t stagedHeightCount = 0u;
		for ( const T3DStagedRegion &entry : staged.regions ) {
			if ( entry.nActiveSlot < 0 || entry.stagedHeight.is_empty() ) {
				LOG( T3DTerrain::ERROR, "Missing staged height payload for region ", entry.location );
				m_regionPersistence.finishPublication( 0 );
				m_regionPersistence.finishDirectoryLoad( false );
				return ERR_FILE_CORRUPT;
			}
			pStagedHeightPages[stagedHeightCount] = entry.stagedHeight.ptr();
			stagedHeightSlots[stagedHeightCount] = entry.nActiveSlot;
			++stagedHeightCount;
		}
		activeStorageReady = m_samplingStorage.replaceActiveHeightPages(
			staged.nTargetRegionSize, pStagedHeightPages, stagedHeightSlots, stagedHeightCount );
		if ( activeStorageReady ) {
			activeStorageReady = setRegionSizeInternal( staged.nTargetRegionSize );
		}
	} else {
		if ( !setRegionSizeInternal( staged.nTargetRegionSize ) ) {
			m_regionPersistence.finishPublication( 0 );
			m_regionPersistence.finishDirectoryLoad( false );
			return ERR_ALREADY_IN_USE;
		}
		// Same-size repository replacement does not pass through the reconfigure
		// clear path. Reset old slots before publishing the staged candidate.
		m_samplingStorage.resetActive();
		activeStorageReady = true;
		for ( const T3DStagedRegion &entry : staged.regions ) {
			if ( !syncActiveRegion( entry.location, entry.pRegion.ptr() ) ) {
				activeStorageReady = false;
				break;
			}
		}
	}
	if ( !activeStorageReady ) {
		if ( !bBoundedCpuLoad ) {
			setRegionSizeInternal( previousRegionSize );
			T3DRegion *const *const ppRegions = m_regionRepository.getRegionPtrMapData();
			for ( int i = 0; i < TERRAIN3D_MAX_REGION_SLOTS; ++i ) {
				T3DRegion *const pRegion = ppRegions[i];
				if ( pRegion && !pRegion->isDeleted() ) {
					syncActiveRegion( pRegion->getLocation(), pRegion );
				}
			}
		}
		m_regionPersistence.finishPublication( 0 );
		m_regionPersistence.finishDirectoryLoad( false );
		LOG( T3DTerrain::ERROR,
			"Failed to populate process-static active terrain storage; previous repository restored" );
		return ERR_INVALID_DATA;
	}

	m_regionRepository.replaceAll(
		staged.resources, staged.regionPtrs.data(), staged.regionLocations.data(),
		staged.getRegionCount(), staged.logicalRegionMap.data() );
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( T3DTerrain::ERROR, "Region repository invariant failure after loadDirectory publication" );
	}
#endif
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_pDefaultHeightMap.unref();
	m_pDefaultControlMap.unref();
	m_pDefaultColorMap.unref();
	m_pDefaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_mapEditService.reset();
	m_generatedHeightMaps.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();

	// Path takeover happens only after the repository owns every staged resource.
	// Validation therefore cannot steal paths from the active repository.
	m_regionPersistence.publishStagedPaths( staged );
	m_regionPersistence.finishPublication( m_regionRepository.getActiveRegionCount() );
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 3/4 complete: ",
		m_regionRepository.getActiveRegionCount(),
		" active regions published; rebuilding residency and renderer resources" );

	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 4/4: cache policy, GPU textures, and sampling state" );
	m_regionPersistence.beginFinalize();
	m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( staged.nTargetRegionSize ) );
	configureResidentPool( 64 );
	configureCpuRegionCache(
		m_pTerrain->isCpuRegionCacheEffective(), m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(),
		m_pTerrain->getCpuRegionLoadBudgetLayers(), m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache(
		m_pTerrain->isInstanceRegionCacheEffective(), m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(),
		m_pTerrain->getInstanceRegionLoadBudgetPages(), m_pTerrain->getInstanceRegionLoadBudgetMb() );
	if ( m_cpuRegionCache.isEnabled() ) {
		updateCpuRegionResidency( Vector3(), true );
	}
	updateMaps( T3DRegion::TYPE_MAX, true, false );
	invalidateSamplingCache();
	LOG( T3DTerrain::DEBUG, "[terrain-data] Phase 4/4 complete: terrain repository is ready for consumers" );
	if ( m_pTerrain->isInitialized() && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, V2I_MAX, true );
	}
	m_regionPersistence.finishDirectoryLoad( true );
	const T3DStartupLoadStats &loadStats = m_regionPersistence.getStartupLoadStats();
	const double elapsedMs = ( double ) ( loadStats.nTotalUsecLast ) / 1000.0;
	LOG( T3DTerrain::INFO, "Loaded ", m_regionRepository.getActiveRegionCount(), " terrain regions (",
		m_regionRepository.getSlotCount(), " texture layers) in ", vformat( "%.2f", elapsedMs ),
		" ms; synchronous Resource deserialize ",
		vformat( "%.2f", ( double ) ( loadStats.nDeserializeUsecLast ) / 1000.0 ), " ms" );
	return OK;
}

// TODO have load_directory call load_region, or make a load_file that loads a specific path
void T3DData::loadRegion( const Vector2i &rRegionLoc, const String &rDir, const bool bUpdate ) {
	const T3DRegionLoadResult result = m_regionPersistence.loadRegion( rRegionLoc, rDir );
	if ( !result.succeeded() ) {
		return;
	}
	const Ref<T3DRegion> pRegion = result.pRegion;
	if ( m_regionRepository.isEmpty() ) {
		m_pTerrain->setRegionSize( ( T3DTerrain::t3dregionSize_t ) ( pRegion->getRegionSize() ) );
	} else if ( m_pTerrain->getRegionSize() != ( T3DTerrain::t3dregionSize_t ) ( pRegion->getRegionSize() ) ) {
		LOG( T3DTerrain::ERROR, "Region size mismatch. First loaded: ", m_pTerrain->getRegionSize(),
			" next: ", pRegion->getRegionSize(), " in file: ", result.path );
		return;
	}

	if ( addRegionInternal( pRegion, bUpdate, true ) == OK ) {
		m_regionPersistence.publishLoadedRegionPath( result );
		pRegion->setModified( result.bStorageUpgrade || pRegion->isModified() );
		pRegion->setEdited( false );
		m_cpuRegionCache.markSelectionDirty();
	}
}

TypedArray<Image> T3DData::getMaps( const t3dmapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Specified map type out of range" );
		return TypedArray<Image>();
	}
	switch ( eMapType ) {
		case T3DRegion::TYPE_HEIGHT:
			return getHeightMaps();
			break;
		case T3DRegion::TYPE_CONTROL:
			return getControlMaps();
			break;
		case T3DRegion::TYPE_COLOR:
			return getColorMaps();
			break;
		default:
			break;
	}
	return TypedArray<Image>();
}

void T3DData::updateMaps( const t3dmapType_t eMapType, const bool bAllRegions, const bool bGenerateMipmaps ) {
	( void ) bGenerateMipmaps; // GPU publication builds transient mip chains; CPU maps stay base-level only.
	if ( isFarAtlasRequired() ) {
		markAllGpuFarRegionsDirty( eMapType );
	}
	if ( m_gpuRegionCache.isEnabled() ) {
		updateMapsBounded( eMapType, bAllRegions );
		return;
	}

	const bool bImplicitStateChanged = refreshImplicitMapState();
	if ( bImplicitStateChanged ) {
		configureResidentPool( 64 );
	}

	// A full rebuild also provisions spare texture-array layers. The spare layers
	// are the key to constant-time add/remove operations between rebuilds.
	const bool bSkipReserve = bAllRegions && m_bSkipSlotReserveOnce;
	const bool bSlotCapacityChanged = bAllRegions && !bSkipReserve && m_regionRepository.ensureSlotReserve( getRegionSlotReserve() );
	if ( bSkipReserve ) {
		m_bSkipSlotReserveOnce = false;
	}
	if ( bAllRegions ) {
		LOG( T3DTerrain::EXTREME, "Marking dirty maps of type: ", eMapType );
		if ( bSlotCapacityChanged || eMapType == T3DRegion::TYPE_MAX ) {
			m_generatedHeightMaps.clear();
			m_generatedControlMaps.clear();
			m_generatedColorMaps.clear();
			m_generatedDerivedMaps.clear();
		} else {
			switch ( eMapType ) {
				case T3DRegion::TYPE_HEIGHT:
					m_generatedHeightMaps.clear();
					m_generatedDerivedMaps.clear();
					break;
				case T3DRegion::TYPE_CONTROL:
					m_generatedControlMaps.clear();
					break;
				case T3DRegion::TYPE_COLOR:
					m_generatedColorMaps.clear();
					break;
				default:
					break;
			}
		}
	}

	bool bAnyChanged = bImplicitStateChanged;

	if ( m_regionRepository.isRegionMapDirty() ) {
		LOG( T3DTerrain::EXTREME, "Regenerating ", V2I( m_regionRepository.getRegionMapSize() ), " active region map from fixed slot storage" );
		m_regionRepository.rebuildRegionMapFromSlots();
		bAnyChanged = true;
		if ( !m_bStartupBulkLoad ) {
			LOG( T3DTerrain::DEBUG, "Emitting region_map_changed" );
			emit_signal( "region_map_changed" );
		}
	}

	const bool bRebuildHeight = m_generatedHeightMaps.isDirty();
	const bool bRebuildControl = m_generatedControlMaps.isDirty();
	const bool bRebuildColor = m_generatedColorMaps.isDirty();
	const bool bRebuildDerived = m_generatedDerivedMaps.isDirty() || bRebuildHeight;

	// Full/startup rebuilds used to traverse the region slots once per map and
	// once again for the height range. Resolve each region once and populate all
	// requested arrays in a single pass; GPU array creation remains separate.
	if ( bRebuildHeight || bRebuildControl || bRebuildColor || bRebuildDerived ) {
		ensureDefaultMaps();
		const int slot_count = m_regionRepository.getSlotCount();
		TypedArray<Image> gpu_control_maps;
		TypedArray<Image> gpu_color_maps;
		if ( bRebuildHeight ) {
			m_heightMaps.resize( slot_count );
			m_mapEditService.resetHeightRange();
		}
		if ( bRebuildControl ) {
			m_controlMaps.resize( slot_count );
			gpu_control_maps.resize( slot_count );
		}
		if ( bRebuildColor ) {
			m_colorMaps.resize( slot_count );
			gpu_color_maps.resize( slot_count );
		}
		if ( bRebuildDerived ) {
			m_derivedMaps.resize( slot_count );
			eastl::vector<Vector2i> missing;
			missing.reserve( ( size_t ) ( m_regionRepository.getActiveRegionCount() ) );
			for ( int slot = 0; slot < slot_count; ++slot ) {
				const Vector2i location = m_regionRepository.getSlotsData()[slot];
				T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
				if ( pRegion && !pRegion->isDeleted() && !pRegion->hasValidDerivedNormalSlopeMap() ) {
					missing.push_back( location );
				}
			}
			regenerateDerivedMaps( missing );
		}

		for ( int slot = 0; slot < slot_count; ++slot ) {
			const Vector2i location = m_regionRepository.getSlotsData()[slot];
			const T3DRegion *pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
			const bool bValid = pRegion && !pRegion->isDeleted();
			if ( bRebuildHeight ) {
				m_heightMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, T3DRegion::TYPE_HEIGHT );
				if ( bValid ) {
					updateMasterHeights( pRegion->getHeightRange() );
				}
			}
			if ( bRebuildControl ) {
				m_controlMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, T3DRegion::TYPE_CONTROL );
				gpu_control_maps[slot] = bValid ? makeGpuRenderControlImage( location ) : m_pDefaultControlMap;
			}
			if ( bRebuildColor ) {
				m_colorMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, T3DRegion::TYPE_COLOR );
				gpu_color_maps[slot] = makeGpuColorImage( m_colorMaps[slot] );
			}
			if ( bRebuildDerived ) {
				const Ref<Image> pDerived = bValid ? pRegion->getDerivedNormalSlopeMap() : Ref<Image>();
				m_derivedMaps[slot] = pDerived.is_valid() ? pDerived : m_pDefaultDerivedMap;
			}
		}

		if ( bRebuildHeight ) {
			m_generatedHeightMaps.create( m_heightMaps );
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "height_maps_changed" );
			}
		}
		if ( bRebuildControl ) {
			if ( m_bControlMapsImplicit ) {
				const TypedArray<Image> implicit_control = {m_pDefaultControlMap};
				m_generatedControlMaps.create( implicit_control );
			} else {
				m_generatedControlMaps.create( gpu_control_maps );
			}
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "control_maps_changed" );
			}
		}
		if ( bRebuildColor ) {
			if ( m_bColorMapsImplicit ) {
				const TypedArray<Image> implicit_color = {makeGpuColorImage( m_pDefaultColorMap )};
				m_generatedColorMaps.create( implicit_color );
			} else {
				m_generatedColorMaps.create( gpu_color_maps );
			}
			bAnyChanged = true;
			if ( !m_bStartupBulkLoad ) {
				emit_signal( "color_maps_changed" );
			}
		}
		if ( bRebuildDerived ) {
			m_generatedDerivedMaps.create( m_derivedMaps );
			bAnyChanged = true;
			if ( !IS_EDITOR ) {
				for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
					const Vector2i location = m_regionRepository.getSlotsData()[slot];
					T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
					if ( !pRegion || pRegion->isDeleted() ) {
						continue;
					}
					m_samplingStorage.syncResidentDerived( location, pRegion );
					pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
				}
				m_derivedMaps.clear();
				m_derivedMaps.resize( slot_count );
			}
		}
	}

	// If no map has been rebuilt, update only edited layers. Emit once per map
	// type rather than once per region to avoid multiplying downstream work.
	if ( !bAnyChanged ) {
		bool bHeightChanged = false;
		bool bControlChanged = false;
		bool bColorChanged = false;
		for ( int region_id = 0; region_id < m_regionRepository.getSlotCount(); ++region_id ) {
			const Vector2i region_loc = m_regionRepository.getSlotsData()[region_id];
			if ( region_loc == V2I_MAX ) {
				continue;
			}
			const T3DRegion *pRegion = getRegionPtr( region_loc );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->isEdited() ) {
				continue;
			}
			switch ( eMapType ) {
				case T3DRegion::TYPE_HEIGHT:
					m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), region_id );
					bHeightChanged = true;
					break;
				case T3DRegion::TYPE_CONTROL:
					m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
					bControlChanged = true;
					break;
				case T3DRegion::TYPE_COLOR:
					m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ), region_id );
					bColorChanged = true;
					break;
				default:
					m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), region_id );
					m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
					m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ), region_id );
					bHeightChanged = true;
					bControlChanged = true;
					bColorChanged = true;
					break;
			}
		}
		if ( !m_bStartupBulkLoad ) {
			if ( bHeightChanged ) {
				emit_signal( "height_maps_changed" );
			}
			if ( bControlChanged ) {
				emit_signal( "control_maps_changed" );
			}
			if ( bColorChanged ) {
				emit_signal( "color_maps_changed" );
			}
		}
	}

	if ( !m_gpuRegionCache.isEnabled() && isFarAtlasRequired() ) {
		ensureFarAtlases();
	}

	if ( bAnyChanged && !m_bStartupBulkLoad ) {
		LOG( T3DTerrain::DEBUG, "Emitting maps_changed" );
		emit_signal( "maps_changed" );
		m_pTerrain->snap();
	}
}

void T3DData::updateMapRegions( const t3dmapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations ) {
	if ( rRegionLocations.empty() ) {
		return;
	}

	if ( refreshImplicitMapState() ) {
		updateMaps( eMapType, true, false );
		return;
	}

	eastl::vector<Vector2i> render_locations = rRegionLocations;
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX ) {
		// The compiled render-control field uses a two-texel source halo. A control
		// edit can therefore change the rendered transition on any immediate
		// neighboring region even though only the authored owner map was modified.
		for ( const Vector2i &source_location : rRegionLocations ) {
			for ( int y = -1; y <= 1; ++y ) {
				for ( int x = -1; x <= 1; ++x ) {
					const Vector2i dependency_location = source_location + Vector2i( x, y );
					if ( dependency_location == source_location ) {
						continue;
					}
					T3DRegion *const pDependencyRegion = getRegionPtr( dependency_location );
					if ( !pDependencyRegion || pDependencyRegion->isDeleted() ) {
						continue;
					}
					bool bAlreadyPresent = false;
					for ( const Vector2i &existing_location : render_locations ) {
						if ( existing_location == dependency_location ) {
							bAlreadyPresent = true;
							break;
						}
					}
					if ( !bAlreadyPresent ) {
						render_locations.push_back( dependency_location );
					}
				}
			}
		}
	}

	bool bHeightChanged = false;
	bool bControlChanged = false;
	bool bColorChanged = false;
	bool bPhysicalMappingChanged = false;
	for ( const Vector2i region_loc : render_locations ) {
		if ( isFarAtlasRequired() ) {
			markGpuFarRegionDirty( region_loc, eMapType );
		}
		T3DRegion *const pRegion = getRegionPtr( region_loc );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		const int repository_slot = getRegionId( region_loc );
		if ( repository_slot < 0 ) {
			continue;
		}
		if ( m_gpuRegionCache.isEnabled() ) {
			if ( !m_gpuRegionCache.isResident( region_loc ) ) {
				m_gpuRegionCache.markSelectionDirty();
				continue;
			}
			bHeightChanged = bHeightChanged || eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX;
			bPhysicalMappingChanged = m_gpuRegionCache.republish( region_loc, getComputeBackendIfCreated() ) || bPhysicalMappingChanged;
			continue;
		}

		switch ( eMapType ) {
			case T3DRegion::TYPE_HEIGHT:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), repository_slot );
				if ( pRegion->getDerivedNormalSlopeMap().is_valid() ) {
					m_derivedMaps[repository_slot] = pRegion->getDerivedNormalSlopeMap();
					m_generatedDerivedMaps.update( pRegion->getDerivedNormalSlopeMap(), repository_slot );
					if ( !IS_EDITOR ) {
						pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
						m_derivedMaps[repository_slot] = Ref<Image>();
					}
				}
				bHeightChanged = true;
				break;
			case T3DRegion::TYPE_CONTROL:
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
				bControlChanged = true;
				break;
			case T3DRegion::TYPE_COLOR:
				m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ),
					repository_slot );
				bColorChanged = true;
				break;
			default:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT ), repository_slot );
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
				m_generatedColorMaps.update( makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) ),
					repository_slot );
				if ( pRegion->getDerivedNormalSlopeMap().is_valid() ) {
					m_derivedMaps[repository_slot] = pRegion->getDerivedNormalSlopeMap();
					m_generatedDerivedMaps.update( pRegion->getDerivedNormalSlopeMap(), repository_slot );
					if ( !IS_EDITOR ) {
						pRegion->setDerivedNormalSlopeMapInternal( Ref<Image>() );
						m_derivedMaps[repository_slot] = Ref<Image>();
					}
				}
				bHeightChanged = true;
				bControlChanged = true;
				bColorChanged = true;
				break;
		}
	}

	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}
	if ( bHeightChanged ) {
		emit_signal( "height_maps_changed" );
	}
	if ( bControlChanged ) {
		emit_signal( "control_maps_changed" );
	}
	if ( bColorChanged ) {
		emit_signal( "color_maps_changed" );
	}
	if ( !m_gpuRegionCache.isEnabled() && isFarAtlasRequired() ) {
		ensureFarAtlases();
	}
}

void T3DData::publishDerivedMapUpdates( const eastl::vector<T3DDerivedMapUpdate> &rUpdates ) {
	for ( const T3DDerivedMapUpdate &update : rUpdates ) {
		if ( !update.bSuccess ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( update.location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( m_gpuRegionCache.isEnabled() ) {
			if ( m_gpuRegionCache.isResident( update.location ) ) {
				m_gpuRegionCache.updateResidentLocation(
					update.location, T3DRegion::TYPE_HEIGHT, getComputeBackendIfCreated() );
			} else {
				m_gpuRegionCache.markSelectionDirty();
			}
			continue;
		}

		const int repositorySlot = getRegionId( update.location );
		const Ref<Image> pDerivedMap = pRegion->getDerivedNormalSlopeMap();
		if ( repositorySlot >= 0 && pDerivedMap.is_valid() && !m_generatedDerivedMaps.isDirty() &&
			repositorySlot < m_derivedMaps.size() && repositorySlot < m_generatedDerivedMaps.size() ) {
			m_derivedMaps[repositorySlot] = pDerivedMap;
			m_generatedDerivedMaps.update( pDerivedMap, repositorySlot );
		}
	}
}

void T3DData::regenerateDerivedMaps(
	const eastl::vector<Vector2i> &rRegionLocations,
	const AABB &rEditedArea
) {
	if ( rRegionLocations.empty() ) {
		return;
	}
	const eastl::vector<T3DDerivedMapUpdate> &updates =
		m_mapEditService.regenerateDerivedMaps( rRegionLocations, rEditedArea, getComputeBackend() );
	publishDerivedMapUpdates( updates );
}

void T3DData::commitMapEdits(
	const t3dmapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations,
	const AABB &rEditedArea, const uint32_t nFlags
) {
	if ( eMapType < T3DRegion::TYPE_HEIGHT || eMapType >= T3DRegion::TYPE_MAX || rRegionLocations.empty() ) {
		return;
	}

	const bool bKeepEdited = ( nFlags & MAP_EDIT_KEEP_EDITED ) != 0u;
	T3DCompute *const pCompute = eMapType == T3DRegion::TYPE_HEIGHT ? getComputeBackend() : nullptr;
	T3DMapMutation mutation = m_mapEditService.commitMapEdits(
		eMapType, rRegionLocations, rEditedArea, pCompute
	);
	if ( !mutation.valid() ) {
		return;
	}

	const eastl::vector<Vector2i> &activeLocations = mutation.changedLocations;
	const eastl::vector<Vector2i> &derivedLocations = mutation.derivedLocations;
	publishDerivedMapUpdates( mutation.derivedUpdates );

	if ( m_gpuRegionCache.isEnabled() ) {
		for ( const Vector2i &location : activeLocations ) {
			markGpuFarRegionDirty( location, eMapType );
		}
	}
	if ( eMapType == T3DRegion::TYPE_HEIGHT && isFarAtlasRequired() && m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
			for ( const Vector2i &location : derivedLocations ) {
				markGpuFarRegionDirty( location, T3DRegion::TYPE_COLOR );
			}
		}
	}

	updateMapRegions( eMapType, activeLocations );
	m_mapEditService.finalizeMapEdits( mutation, bKeepEdited );

	if ( mutation.editedArea.size.x > 0.0f && mutation.editedArea.size.z > 0.0f ) {
		LOG( T3DTerrain::DEBUG, "Emitting maps_edited" );
		emit_signal( "maps_edited", mutation.editedArea );
	}

	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_INSTANCES ) != 0u && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateTransforms( mutation.editedArea );
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_COLLISION ) != 0u ) {
		T3DCollision *const pCollision = m_pTerrain->getCollision();
		if ( pCollision && pCollision->isEnabled() ) {
			if ( pCollision->isDynamicMode() ) {
				pCollision->update( V2I_MAX, true );
			} else {
				for ( const Vector2i &location : activeLocations ) {
					pCollision->update( location, true );
				}
			}
		}
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_SNAP ) != 0u ) {
		m_pTerrain->snap();
	}
	m_mapEditService.recycleMutation( std::move( mutation ) );
}

void T3DData::commitMapEdit(
	const t3dmapType_t eMapType, const Vector2i &rRegionLocation,
	const Rect2i &rDirtyRect
) {
	if ( eMapType < T3DRegion::TYPE_HEIGHT || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Invalid map type: ", eMapType );
		return;
	}

	AABB editedArea;
	if ( !m_mapEditService.buildRegionEditArea( rRegionLocation, rDirtyRect, editedArea ) ) {
		LOG( T3DTerrain::ERROR, "No active region found at: ", rRegionLocation );
		return;
	}

	uint32_t flags = MAP_EDIT_SNAP;
	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_CONTROL ) {
		flags |= MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION;
	}
	commitMapEdits( eMapType, {rRegionLocation}, editedArea, flags );
}

Ref<T3DErosionJob> T3DData::createErosionJob( const Ref<T3DErosionSettings> &rSettings ) {
	Ref<T3DErosionJob> pJob;
	pJob.instantiate();
	if ( pJob->setup( this, rSettings ) != OK ) {
		return pJob;
	}
	return pJob;
}

void T3DData::setPixel( const t3dmapType_t eMapType, const Vector3 &rGlobalPosition, const Color &rPixel ) {
	if ( eMapType < T3DRegion::TYPE_HEIGHT || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Specified map type out of range" );
		return;
	}
	const Vector2i regionLocation = getRegionLocation( rGlobalPosition );
	if ( !ensureCpuRegionResident( regionLocation ) ) {
		LOG( T3DTerrain::ERROR, "Unable to load CPU map payload for region: ", regionLocation );
		return;
	}

	Vector2i writtenLocation;
	Rect2i dirtyRect;
	const Error writeError = m_mapEditService.setPixel(
		eMapType, rGlobalPosition, rPixel, writtenLocation, dirtyRect
	);
	if ( writeError != OK ) {
		LOG( T3DTerrain::ERROR, "Unable to edit terrain map at: ", rGlobalPosition, " error: ", writeError );
		return;
	}
	commitMapEdit( eMapType, writtenLocation, dirtyRect );
}

Color T3DData::getPixel( const t3dmapType_t eMapType, const Vector3 &rGlobalPosition ) const {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Specified map type out of range" );
		return COLOR_NAN;
	}
	const Vector2i region_loc = getRegionLocation( rGlobalPosition );
	if ( eMapType != T3DRegion::TYPE_HEIGHT && m_cpuRegionCache.isEnabled() &&
		!const_cast<T3DData *>( this )->ensureCpuRegionResident( region_loc ) ) {
		return COLOR_NAN;
	}
	const T3DRegion *pRegion = getRegionPtr( region_loc );
	if ( !pRegion ) {
		return COLOR_NAN;
	}
	if ( pRegion->isDeleted() ) {
		return COLOR_NAN;
	}
	const Vector2i global_offset = region_loc * m_regionRepository.getRegionSize();
	const Vector3 descaled_pos = rGlobalPosition / m_regionRepository.getVertexSpacing();
	Vector2i img_pos = Vector2i( descaled_pos.x - global_offset.x, descaled_pos.z - global_offset.y );
	img_pos = img_pos.clamp( V2I_ZERO, V2I( m_regionRepository.getRegionSize() - 1 ) );
	const T3DActiveMapLease active = m_samplingStorage.acquireActive( region_loc );
	if ( active.valid() ) {
		const t3dactiveMapView_t &view = active.view();
		const int index = img_pos.y * m_regionRepository.getRegionSize() + img_pos.x;
		switch ( eMapType ) {
				case T3DRegion::TYPE_HEIGHT:
				return Color( Terrain3DRegionCodec::decodeHeightU16( view.pHeight[index] ), 0.0f, 0.0f, 1.0f );
			default:
				break;
		}
	}
	Image *pMap = pRegion->getMapPtr( eMapType );
	if ( !pMap ) {
		if ( eMapType == T3DRegion::TYPE_CONTROL ) {
			return COLOR_CONTROL;
		}
		if ( eMapType == T3DRegion::TYPE_COLOR ) {
			return COLOR_ROUGHNESS;
		}
		return COLOR_NAN;
	}
	if ( eMapType == T3DRegion::TYPE_HEIGHT ) {
		return Color( Terrain3DRegionCodec::readHeightPixel( pMap, img_pos ), 0.0f, 0.0f, 1.0f );
	}
	return pMap->get_pixelv( img_pos );
}

real_t T3DData::getHeight( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return NAN;
	}
	T3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleHeight( rGlobalPosition );
}

uint32_t T3DData::getControl( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 0 ) ) {
		return UINT32_MAX;
	}
	T3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleControl( rGlobalPosition );
}

PackedFloat32Array T3DData::getHeights( const PackedVector3Array &rGlobalPositions ) const {
	PackedFloat32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	T3DSampler sampler( getSamplingSourceInternal() );
	if ( !m_cpuRegionCache.isEnabled() ) {
		sampler.sampleHeights( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	float *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<T3DData *>( this )->ensureCpuSampleArea( position, 1 )
			? sampler.sampleHeight( position )
			: NAN;
	}
	return result;
}

PackedInt32Array T3DData::getControls( const PackedVector3Array &rGlobalPositions ) const {
	PackedInt32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	T3DSampler sampler( getSamplingSourceInternal() );
	if ( !m_cpuRegionCache.isEnabled() ) {
		sampler.sampleControls( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	int32_t *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<T3DData *>( this )->ensureCpuSampleArea( position, 0 )
			? ( int32_t ) ( sampler.sampleControl( position ) )
			: ( int32_t ) ( UINT32_MAX );
	}
	return result;
}

PackedVector3Array T3DData::getNormals( const PackedVector3Array &rGlobalPositions ) const {
	PackedVector3Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	T3DSampler sampler( getSamplingSourceInternal() );
	if ( !m_cpuRegionCache.isEnabled() ) {
		sampler.sampleNormals( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	Vector3 *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<T3DData *>( this )->ensureCpuSampleArea( position, 1 )
			? sampler.sampleNormal( position )
			: V3_NAN;
	}
	return result;
}

Vector3 T3DData::getNormal( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return V3_NAN;
	}
	T3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleNormal( rGlobalPosition );
}

bool T3DData::isInSlope(
	const Vector3 &rGlobalPosition, const Vector2 &rSlopeRange,
	const Vector3 &rNormal
) const {
	const Vector2 slope_range = CLAMP( rSlopeRange, V2_ZERO, V2( 90.f ) );
	if ( slope_range.y - slope_range.x > 89.99f ) {
		return true;
	}

	Vector3 slope_normal = rNormal;
	if ( !slope_normal.is_zero_approx() ) {
		slope_normal.normalize();
	} else {
		if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
			return false;
		}
		T3DSampler sampler( getSamplingSourceInternal() );
		uint32_t control = UINT32_MAX;
		if ( !sampler.sampleControlAndSlopeNormal( rGlobalPosition, control, slope_normal ) ) {
			return false;
		}
	}

	const real_t fSlopeAngleDegrees = Math::rad_to_deg( Math::acos( CLAMP( slope_normal.dot( V3_UP ), -1.0f, 1.0f ) ) );
	return slope_range.x <= fSlopeAngleDegrees && fSlopeAngleDegrees <= slope_range.y;
}

/**
 * Returns:
 * X = base index
 * Y = overlay index
 * Z = percentage blend between X and Y. Limited to the fixed values in RANGE.
 * Interpretation of this data is up to the gamedev. Unfortunately due to blending, this isn't
 * pixel perfect. I would have your player print this location as you walk around to see how the
 * blending values look, then consider that the overlay texture is visible starting at a blend
 * value of .3-.5, otherwise it's the base texture.
 **/
Vector3 T3DData::getTextureId( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return V3_NAN;
	}
	T3DSampler sampler( getSamplingSourceInternal() );
	const uint32_t control = sampler.sampleControl( rGlobalPosition );
	if ( control == UINT32_MAX || isHole( control ) ) {
		return V3_NAN;
	}

	// If material available, autoshader enabled, and pixel set to auto
	if ( m_pTerrain ) {
		const Ref<T3DMaterial> pTMaterial = m_pTerrain->getMaterial();
		const bool bControlAuto = isAuto( control );
		if ( pTMaterial.is_valid() && pTMaterial->getAutoShaderEnabled() && bControlAuto ) {
			const real_t fAutoSlope = ( real_t ) ( pTMaterial->getShaderParam( "auto_slope" ) );
			const real_t fAutoHeightReduction = ( real_t ) ( pTMaterial->getShaderParam( "auto_height_reduction" ) );
			float fHeight = NAN;
			Vector3 normal = V3_NAN;
			if ( !sampler.sampleHeightAndNormal( rGlobalPosition, fHeight, normal ) ) {
				return V3_NAN;
			}
			const uint32_t base_id = pTMaterial->getShaderParam( "auto_base_texture" );
			const uint32_t overlay_id = pTMaterial->getShaderParam( "auto_overlay_texture" );
			const real_t fBlend =
				CLAMP( ( fAutoSlope * 2.f * ( normal.y - 1.f ) + 1.f ) - fAutoHeightReduction * .01f * fHeight, 0.f, 1.f );
			return Vector3( ( real_t ) ( base_id ), ( real_t ) ( overlay_id ), fBlend );
		}
	}

	// Else, just get textures from control map
	const uint32_t base_id = getBase( control );
	const uint32_t overlay_id = getOverlay( control );
	const real_t fBlend = ( real_t ) ( getBlend( control ) ) / TERRAIN3D_BYTE_MAX_FLOAT;
	return Vector3( ( real_t ) ( base_id ), ( real_t ) ( overlay_id ), fBlend );
}

/**
 * Returns the location of a terrain vertex at a certain LOD. If there is a hole at the position, it returns
 * NAN in the vector's Y coordinate.
 * p_lod (0-8): Determines how many heights around the given global position will be sampled.
 * p_filter:
 *  HEIGHT_FILTER_NEAREST: Samples the height map at the exact coordinates given.
 *  HEIGHT_FILTER_MINIMUM: Samples (1 << p_lod) ** 2 heights around the given coordinates and returns the lowest.
 * p_global_position: X and Z coordinates of the vertex. Heights will be sampled around these coordinates.
 */
Vector3 T3DData::getMeshVertex(
	const int32_t nLod, const t3dheightFilter_t filter,
	const Vector3 &rGlobalPosition
) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	if ( !const_cast<T3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, step / 2 + 1 ) ) {
		return Vector3( rGlobalPosition.x, NAN, rGlobalPosition.z );
	}
	real_t fHeight = 0.0f;
	T3DSampler sampler( getSamplingSourceInternal() );

	switch ( filter ) {
		case HEIGHT_FILTER_NEAREST: {
			fHeight = sampler.sampleHeight( rGlobalPosition );
		} break;
		case HEIGHT_FILTER_MINIMUM: {
			const double fGridX = ( double ) ( rGlobalPosition.x ) / ( double ) ( m_regionRepository.getVertexSpacing() );
			const double fGridZ = ( double ) ( rGlobalPosition.z ) / ( double ) ( m_regionRepository.getVertexSpacing() );
			const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
			const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
			const double fWorldDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( m_regionRepository.getVertexSpacing() );
			const double fWorldDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( m_regionRepository.getVertexSpacing() );
			if ( fWorldDx * fWorldDx + fWorldDz * fWorldDz < 0.0001 ) {
				float fMinimum = NAN;
				fHeight = sampler.minimumHeightRect( rounded_x - step / 2, rounded_z - step / 2, step, step,
					fMinimum, true )
					? fMinimum
					: NAN;
				break;
			}

			// Preserve bilinear sampling semantics for callers that request a minimum
			// filter at a fractional grid position. Mesh and navigation bake callers
			// use the grid-aligned bulk path above.
			fHeight = sampler.sampleHeight( rGlobalPosition );
			bool bInvalid = std::isnan( fHeight );
			for ( int32_t dz = -step / 2; dz < step / 2 && !bInvalid; ++dz ) {
				for ( int32_t dx = -step / 2; dx < step / 2; ++dx ) {
					const Vector3 position = rGlobalPosition + Vector3( dx, 0.f, dz ) * m_regionRepository.getVertexSpacing();
					const real_t fSample = sampler.sampleHeight( position );
					if ( std::isnan( fSample ) ) {
						bInvalid = true;
						break;
					}
					fHeight = MIN( fHeight, fSample );
				}
			}
			if ( bInvalid ) {
				fHeight = NAN;
			}
		} break;
	}
	return Vector3( rGlobalPosition.x, fHeight, rGlobalPosition.z );
}

void T3DData::addEditedArea( const AABB &rArea ) {
	m_mapEditService.addEditedArea( rArea );
	LOG( T3DTerrain::DEBUG, "Emitting maps_edited" );
	emit_signal( "maps_edited", rArea );
}

void T3DData::calcHeightRange( const bool bRecursive ) {
	m_mapEditService.calculateHeightRange(
		bRecursive,
		[]( void *const pContext, const Vector2i &rLocation ) -> bool {
			T3DData *const pData = static_cast<T3DData *>( pContext );
			if ( pData->ensureCpuRegionResident( rLocation ) ) {
				return true;
			}
			LOG( T3DTerrain::ERROR, "Unable to page region for recursive height-range calculation: ", rLocation );
			return false;
		},
		this
	);
	LOG( T3DTerrain::EXTREME, "Accumulated height range for all regions: ", m_mapEditService.getHeightRange() );
}

/**
 * Imports an Image set (Height, Control, Color) into Terrain3DData
 * It does NOT normalize values to 0-1. You must do that using get_min_max() and adjusting scale and offset.
 * Parameters:
 *	p_images - t3dmapType_t.TYPE_MAX sized array of Images for Height, Control, Color. Images can be blank or null
 *	p_global_position - X,0,Z location on the region map. Valid range is ~ (+/-8192, +/-8192)
 *	p_offset - Add this factor to all height values, can be negative
 *	p_scale - Scale all height values by this factor (applied after offset)
 */
void T3DData::importImages(
	const TypedArray<Image> &rImages, const Vector3 &rGlobalPosition,
	const real_t fOffset, const real_t fScale
) {
	IS_INIT_MESG( "Data not initialized", VOID );

	bool bGenerateMipmaps = false;
	const Error error = m_mapEditService.importImages(
		rImages, rGlobalPosition, fOffset, fScale,
		[]( void *const pContext, const Vector2i &rLocation, Ref<T3DRegion> &rRegion ) -> Error {
			T3DData *const pData = static_cast<T3DData *>( pContext );
			rRegion = pData->getRegion( rLocation );

			if ( rRegion.is_null() ) {
				rRegion.instantiate();
				rRegion->setLocation( rLocation );
				rRegion->setRegionSize( pData->m_regionRepository.getRegionSize() );
				rRegion->setVertexSpacing( pData->m_regionRepository.getVertexSpacing() );
				return pData->addRegion( rRegion, false );
			}

			if ( !rRegion->isDeleted() ) {
				if ( !pData->ensureCpuRegionResident( rLocation ) ) {
					LOG( T3DTerrain::ERROR, "Unable to page existing CPU terrain maps for image import at ", rLocation );
					return ERR_CANT_ACQUIRE_RESOURCE;
				}
				rRegion = pData->getRegion( rLocation );
				return rRegion.is_valid() ? OK : ERR_DOES_NOT_EXIST;
			}

			rRegion->clear();
			rRegion->setLocation( rLocation );
			rRegion->setRegionSize( pData->m_regionRepository.getRegionSize() );
			rRegion->setVertexSpacing( pData->m_regionRepository.getVertexSpacing() );
			return OK;
		},
		this, bGenerateMipmaps
	);
	if ( error != OK ) {
		LOG( T3DTerrain::ERROR, "Image import failed with error: ", error );
		return;
	}

	updateMaps( T3DRegion::TYPE_MAX, true, bGenerateMipmaps );
}

/** Exports a specified map as one of r16/raw, exr, jpg, png, webp, res, tres
 * r16 or exr are recommended for roundtrip external editing
 * r16 can be edited by Krita, however you must know the dimensions and min/max before reimporting
 * res/tres stores in Godot's native format.
 */
Error T3DData::exportImage( const String &rFileName, const t3dmapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= T3DRegion::TYPE_MAX ) {
		LOG( T3DTerrain::ERROR, "Invalid map type specified: ", eMapType, " max: ", T3DRegion::TYPE_MAX - 1 );
		return FAILED;
	}

	if ( rFileName.is_empty() ) {
		LOG( T3DTerrain::ERROR, "No file specified. Nothing to export" );
		return FAILED;
	}

	if ( getRegionCount() == 0 ) {
		LOG( T3DTerrain::ERROR, "No valid regions. Nothing to export" );
		return FAILED;
	}

	// Simple file name validation
	static const String s_badChars = "?*|%<>\"";
	for ( int i = 0; i < s_badChars.length(); ++i ) {
		for ( int j = 0; j < rFileName.length(); ++j ) {
			if ( s_badChars[i] == rFileName[j] ) {
				LOG( T3DTerrain::ERROR, "Invalid file path '" + rFileName + "'" );
				return FAILED;
			}
		}
	}

	// Update path delimeter
	String file_name = rFileName.replace( "\\", "/" );

	// Check if p_file_name has a path and prepend "res://" if not
	bool bIsSimpleFilename = true;
	for ( int i = 0; i < file_name.length(); ++i ) {
		const char32_t c = file_name[i];
		if ( c == '/' || c == ':' ) {
			bIsSimpleFilename = false;
			break;
		}
	}
	if ( bIsSimpleFilename ) {
		file_name = "res://" + file_name;
	}

	// Check if the file can be opened for writing
	Ref<FileAccess> pFileRef = FileAccess::open( file_name, FileAccess::ModeFlags::WRITE );
	if ( pFileRef.is_null() ) {
		LOG( T3DTerrain::ERROR, "Cannot open file '" + file_name + "' for writing" );
		return FAILED;
	}
	pFileRef->close();

	// Filename is validated. Begin export image generation
	Ref<Image> pImg = layeredToImage( eMapType );
	if ( pImg.is_null() || pImg->is_empty() ) {
		LOG( T3DTerrain::ERROR, "Cannot create an export image for map type: ", TERRAIN3D_REGION_MAP_TYPE_NAMES[eMapType] );
		return FAILED;
	}

	String ext = file_name.get_extension().to_lower();
	LOG( T3DTerrain::MESG, "Saving ", pImg->get_size(), " sized ", TERRAIN3D_REGION_MAP_TYPE_NAMES[eMapType], " map in format ", pImg->get_format(), " as ",
		ext, " to: ", file_name );
	const Vector2i minmax = Util::getMinMax( pImg );
	LOG( T3DTerrain::MESG, "Minimum height: ", minmax.x, ", Maximum height: ", minmax.y );
	if ( ext == "r16" || ext == "raw" ) {
		Ref<FileAccess> pFile = FileAccess::open( file_name, FileAccess::WRITE );
		const real_t fHeightMin = minmax.x;
		const real_t fHeightMax = minmax.y;
		const real_t fHscale = 65535.0 / ( fHeightMax - fHeightMin );
		for ( int y = 0; y < pImg->get_height(); y++ ) {
			for ( int x = 0; x < pImg->get_width(); x++ ) {
				int h = ( int ) ( ( pImg->get_pixel( x, y ).r - fHeightMin ) * fHscale );
				h = CLAMP( h, 0, 65535 );
				pFile->store_16( h );
			}
		}
		return pFile->get_error();
	} else if ( ext == "exr" ) {
		return pImg->save_exr( file_name, ( eMapType == T3DRegion::TYPE_HEIGHT ) ? true : false );
	} else if ( ext == "png" ) {
		return pImg->save_png( file_name );
	} else if ( ext == "jpg" ) {
		return pImg->save_jpg( file_name );
	} else if ( ext == "webp" ) {
		return pImg->save_webp( file_name );
	} else if ( ( ext == "res" ) || ( ext == "tres" ) ) {
		return ResourceSaver::get_singleton()->save( pImg, file_name, ResourceSaver::FLAG_COMPRESS );
	}

	LOG( T3DTerrain::ERROR, "No recognized file type. See docs for valid extensions" );
	return FAILED;
}

Ref<Image> T3DData::layeredToImage( const t3dmapType_t eMapType ) const {
	return m_mapEditService.layeredToImage(
		eMapType,
		[]( void *const pContext, const Vector2i &rLocation ) -> bool {
			T3DData *const pData = static_cast<T3DData *>( pContext );
			if ( pData->ensureCpuRegionResident( rLocation ) ) {
				return true;
			}
			LOG( T3DTerrain::ERROR, "Unable to page region for image export: ", rLocation );
			return false;
		},
		const_cast<T3DData *>( this )
	);
}

void T3DData::dump( const bool bVerbose ) const {
	const TypedArray<Vector2i> region_slots = getRegionSlots();
	LOG( T3DTerrain::MESG, "_region_locations (", m_regionRepository.getSlotCount(), "): ", region_slots );
	const Array keys = m_regionRepository.getRegionKeys();
	LOG( T3DTerrain::MESG, "_regions (", keys.size(), "):" );
	for ( const Vector2i region_loc : keys ) {
		const T3DRegion *pRegion = getRegionPtr( region_loc );
		if ( !pRegion ) {
			LOG( T3DTerrain::WARN, "No region found at: ", region_loc );
			continue;
		}
		pRegion->dump( bVerbose );
	}
	if ( bVerbose ) {
		for ( int i = 0; i < TERRAIN3D_MAX_REGION_SLOTS; i++ ) {
			if ( m_regionRepository.getLogicalRegionMapData()[i] ) {
				LOG( T3DTerrain::MESG, "Region map array index: ", i, " / ", TERRAIN3D_MAX_REGION_SLOTS - 1,
					", Region id: ", m_regionRepository.getLogicalRegionMapData()[i] );
			}
		}
		Util::dumpMaps( m_heightMaps, "Height maps" );
		Util::dumpGentex( m_generatedHeightMaps, "height" );
		Util::dumpMaps( m_controlMaps, "Control maps" );
		Util::dumpGentex( m_generatedControlMaps, "control" );
		Util::dumpMaps( m_colorMaps, "Color maps" );
		Util::dumpGentex( m_generatedColorMaps, "color" );
	}
}

///////////////////////////
// Protected Functions
///////////////////////////

void T3DData::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_NEAREST );
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_MINIMUM );

	ClassDB::bind_integer_constant( get_class_static(), "", "WORLD_TEXEL_SIZE", TERRAIN3D_ACTIVE_WORLD_TEXELS );
	ClassDB::bind_integer_constant( get_class_static(), "", "REGION_MAP_SIZE", TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE );

	ClassDB::bind_method( D_METHOD( "get_region_count" ), &T3DData::getRegionCount );
	ClassDB::bind_method( D_METHOD( "set_region_locations", "region_locations" ), &T3DData::setRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_region_locations" ), &T3DData::getRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_render_region_slots" ), &T3DData::getRenderRegionSlots );
	ClassDB::bind_method( D_METHOD( "get_regions_active", "copy", "deep" ), &T3DData::getRegionsActive,
						 DEFVAL( false ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_regions_all" ), &T3DData::getRegionsAll );
	ClassDB::bind_method( D_METHOD( "get_region_map" ), &T3DData::getRegionMap );
	ClassDB::bind_method( D_METHOD( "get_region_map_size" ), &T3DData::getRegionMapSize );
	ClassDB::bind_method( D_METHOD( "is_runtime_ready" ), &T3DData::isRuntimeReady );
	ClassDB::bind_method( D_METHOD( "get_active_storage_generation" ), &T3DData::getActiveStorageGeneration );
	ClassDB::bind_method( D_METHOD( "get_runtime_state" ), &T3DData::getRuntimeState );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &T3DData::getGpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_middle_height_cache_stats" ), &T3DData::getMiddleHeightCacheStats );
	ClassDB::bind_method( D_METHOD( "get_startup_load_stats" ), &T3DData::getStartupLoadStats );
	ClassDB::bind_method( D_METHOD( "get_runtime_residency_grid", "radius" ),
		&T3DData::getRuntimeResidencyGrid, DEFVAL( 6 ) );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &T3DData::getCpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ),
		&T3DData::getInstanceRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "ensure_instance_region_resident", "location" ),
		&T3DData::ensureInstanceRegionResident );
	ClassDB::bind_method( D_METHOD( "ensure_cpu_region_resident", "region_location" ),
		&T3DData::ensureCpuRegionResident );
	ClassDB::bind_static_method( "Terrain3DData", D_METHOD( "get_region_map_index", "region_location" ),
								&T3DData::getRegionMapIndex );

	ClassDB::bind_method( D_METHOD( "do_for_regions", "area", "callback" ), &T3DData::doForRegions );
	ClassDB::bind_method( D_METHOD( "change_region_size", "region_size" ), &T3DData::changeRegionSize );

	ClassDB::bind_method( D_METHOD( "get_region_location", "global_position" ), &T3DData::getRegionLocation );
	ClassDB::bind_method( D_METHOD( "get_region_id", "region_location" ), &T3DData::getRegionId );
	ClassDB::bind_method( D_METHOD( "get_region_idp", "global_position" ), &T3DData::getRegionIdp );

	ClassDB::bind_method( D_METHOD( "has_region", "region_location" ), &T3DData::hasRegion );
	ClassDB::bind_method( D_METHOD( "has_regionp", "global_position" ), &T3DData::hasRegionp );
	ClassDB::bind_method( D_METHOD( "get_region", "region_location" ), &T3DData::getRegion );
	ClassDB::bind_method( D_METHOD( "get_regionp", "global_position" ), &T3DData::getRegionp );

	ClassDB::bind_method( D_METHOD( "set_region_modified", "region_location", "modified" ),
						 &T3DData::setRegionModified );
	ClassDB::bind_method( D_METHOD( "is_region_modified", "region_location" ), &T3DData::isRegionModified );
	ClassDB::bind_method( D_METHOD( "set_region_deleted", "region_location", "deleted" ),
						 &T3DData::setRegionDeleted );
	ClassDB::bind_method( D_METHOD( "is_region_deleted", "region_location" ), &T3DData::isRegionDeleted );

	ClassDB::bind_method( D_METHOD( "add_region_blankp", "global_position", "update" ), &T3DData::addRegionBlankp,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region_blank", "region_location", "update" ), &T3DData::addRegionBlank,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region", "region", "update" ), &T3DData::addRegion, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "replace_region_data", "region_location", "data" ),
						 &T3DData::replaceRegionData );
	ClassDB::bind_method( D_METHOD( "remove_regionp", "global_position", "update" ), &T3DData::removeRegionp,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_regionl", "region_location", "update" ), &T3DData::removeRegionl,
						 DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_region", "region", "update" ), &T3DData::removeRegion, DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "save_directory", "directory" ), &T3DData::saveDirectory );
	ClassDB::bind_method( D_METHOD( "save_region", "region_location", "directory", "save_16_bit" ),
						 &T3DData::saveRegion, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "inspect_gpu_region_pages" ), &T3DData::inspectGpuRegionPages );
	ClassDB::bind_method( D_METHOD( "cook_gpu_region_pages" ), &T3DData::cookGpuRegionPages );
	ClassDB::bind_method( D_METHOD( "load_directory", "directory" ), &T3DData::loadDirectory );
	ClassDB::bind_method( D_METHOD( "load_region", "region_location", "directory", "update" ), &T3DData::loadRegion,
						 DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "get_height_maps" ), &T3DData::getHeightMaps );
	ClassDB::bind_method( D_METHOD( "get_control_maps" ), &T3DData::getControlMaps );
	ClassDB::bind_method( D_METHOD( "get_color_maps" ), &T3DData::getColorMaps );
	ClassDB::bind_method( D_METHOD( "get_maps", "map_type" ), &T3DData::getMaps );
	ClassDB::bind_method( D_METHOD( "update_maps", "map_type", "all_regions", "generate_mipmaps" ),
						 &T3DData::updateMaps, DEFVAL( T3DRegion::TYPE_MAX ), DEFVAL( true ), DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "create_erosion_job", "settings" ), &T3DData::createErosionJob );
	ClassDB::bind_method( D_METHOD( "get_height_maps_rid" ), &T3DData::getHeightMapsRid );
	ClassDB::bind_method( D_METHOD( "get_derived_maps_rid" ), &T3DData::getDerivedMapsRid );
	ClassDB::bind_method( D_METHOD( "get_control_maps_rid" ), &T3DData::getControlMapsRid );
	ClassDB::bind_method( D_METHOD( "get_color_maps_rid" ), &T3DData::getColorMapsRid );
	ClassDB::bind_method( D_METHOD( "are_control_maps_implicit" ), &T3DData::areControlMapsImplicit );
	ClassDB::bind_method( D_METHOD( "are_color_maps_implicit" ), &T3DData::areColorMapsImplicit );

	ClassDB::bind_method( D_METHOD( "set_pixel", "map_type", "global_position", "pixel" ), &T3DData::setPixel );
	ClassDB::bind_method( D_METHOD( "commit_map_edit", "map_type", "region_location", "dirty_rect" ),
						 &T3DData::commitMapEdit, DEFVAL( Rect2i() ) );
	ClassDB::bind_method( D_METHOD( "get_pixel", "map_type", "global_position" ), &T3DData::getPixel );
	ClassDB::bind_method( D_METHOD( "set_height", "global_position", "height" ), &T3DData::setHeight );
	ClassDB::bind_method( D_METHOD( "get_height", "global_position" ), &T3DData::getHeight );
	ClassDB::bind_method( D_METHOD( "get_heights", "global_positions" ), &T3DData::getHeights );
	ClassDB::bind_method( D_METHOD( "get_normals", "global_positions" ), &T3DData::getNormals );
	ClassDB::bind_method( D_METHOD( "get_controls", "global_positions" ), &T3DData::getControls );
	ClassDB::bind_method( D_METHOD( "set_color", "global_position", "color" ), &T3DData::setColor );
	ClassDB::bind_method( D_METHOD( "get_color", "global_position" ), &T3DData::getColor );
	ClassDB::bind_method( D_METHOD( "set_control", "global_position", "control" ), &T3DData::setControl );
	ClassDB::bind_method( D_METHOD( "get_control", "global_position" ), &T3DData::getControl );
	ClassDB::bind_method( D_METHOD( "set_roughness", "global_position", "roughness" ), &T3DData::setRoughness );
	ClassDB::bind_method( D_METHOD( "get_roughness", "global_position" ), &T3DData::getRoughness );

	ClassDB::bind_method( D_METHOD( "set_control_base_id", "global_position", "texture_id" ),
						 &T3DData::setControlBaseId );
	ClassDB::bind_method( D_METHOD( "get_control_base_id", "global_position" ), &T3DData::getControlBaseId );
	ClassDB::bind_method( D_METHOD( "set_control_overlay_id", "global_position", "texture_id" ),
						 &T3DData::setControlOverlayId );
	ClassDB::bind_method( D_METHOD( "get_control_overlay_id", "global_position" ), &T3DData::getControlOverlayId );
	ClassDB::bind_method( D_METHOD( "set_control_blend", "global_position", "blend_value" ),
						 &T3DData::setControlBlend );
	ClassDB::bind_method( D_METHOD( "get_control_blend", "global_position" ), &T3DData::getControlBlend );
	ClassDB::bind_method( D_METHOD( "set_control_angle", "global_position", "degrees" ),
						 &T3DData::setControlAngle );
	ClassDB::bind_method( D_METHOD( "get_control_angle", "global_position" ), &T3DData::getControlAngle );
	ClassDB::bind_method( D_METHOD( "set_control_scale", "global_position", "percentage_modifier" ),
						 &T3DData::setControlScale );
	ClassDB::bind_method( D_METHOD( "get_control_scale", "global_position" ), &T3DData::getControlScale );
	ClassDB::bind_method( D_METHOD( "set_control_hole", "global_position", "enable" ), &T3DData::setControlHole );
	ClassDB::bind_method( D_METHOD( "get_control_hole", "global_position" ), &T3DData::getControlHole );
	ClassDB::bind_method( D_METHOD( "set_control_navigation", "global_position", "enable" ),
						 &T3DData::setControlNavigation );
	ClassDB::bind_method( D_METHOD( "get_control_navigation", "global_position" ), &T3DData::getControlNavigation );
	ClassDB::bind_method( D_METHOD( "set_control_auto", "global_position", "enable" ), &T3DData::setControlAuto );
	ClassDB::bind_method( D_METHOD( "get_control_auto", "global_position" ), &T3DData::getControlAuto );

	ClassDB::bind_method( D_METHOD( "get_normal", "global_position" ), &T3DData::getNormal );
	ClassDB::bind_method( D_METHOD( "is_in_slope", "global_position", "slope_range", "normal" ),
						 &T3DData::isInSlope, DEFVAL( V3_ZERO ) );
	ClassDB::bind_method( D_METHOD( "get_texture_id", "global_position" ), &T3DData::getTextureId );
	ClassDB::bind_method( D_METHOD( "get_mesh_vertex", "lod", "filter", "global_position" ),
						 &T3DData::getMeshVertex );

	ClassDB::bind_method( D_METHOD( "get_height_range" ), &T3DData::getHeightRange );
	ClassDB::bind_method( D_METHOD( "calc_height_range", "recursive" ), &T3DData::calcHeightRange, DEFVAL( false ) );

	ClassDB::bind_method( D_METHOD( "import_images", "images", "global_position", "offset", "scale" ),
						 &T3DData::importImages, DEFVAL( V3_ZERO ), DEFVAL( 0.f ), DEFVAL( 1.f ) );
	ClassDB::bind_method( D_METHOD( "export_image", "file_name", "map_type" ), &T3DData::exportImage );
	ClassDB::bind_method( D_METHOD( "layered_to_image", "map_type" ), &T3DData::layeredToImage );
	ClassDB::bind_method( D_METHOD( "dump", "verbose" ), &T3DData::dump, DEFVAL( false ) );

	const int ro_flags = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Runtime Repository Status", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "region_locations", PROPERTY_HINT_ARRAY_TYPE, "Vector2i", ro_flags ),
				 "set_region_locations", "get_region_locations" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "height_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ), "",
				 "get_height_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "control_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ), "",
				 "get_control_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "color_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ), "",
				 "get_color_maps" );

	ADD_SIGNAL( MethodInfo( "maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "gpu_region_page_table_changed" ) );
	ADD_SIGNAL( MethodInfo( "region_map_changed" ) );
	ADD_SIGNAL( MethodInfo( "height_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "control_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "color_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "maps_edited", PropertyInfo( Variant::AABB, "edited_area" ) ) );
}
