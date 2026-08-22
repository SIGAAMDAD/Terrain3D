// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_data.h"

#include "Terrain3DConstants.h"
#include "assets/terrain_3d_assets.h"
#include "authoring/terrain_3d_cook_database.h"
#include "authoring/terrain_3d_editor.h"
#include "authoring/terrain_3d_erosion_job.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "core/logger.h"
#include "data/terrain_3d_region_codec.h"
#include "data/terrain_3d_runtime_bake_codec.h"
#include "data/terrain_3d_runtime_page_format.h"
#include "data/terrain_3d_sampler.h"
#include "eastl_config.h"
#include "instances/terrain_3d_instancer.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "memory/terrain_3d_linear_arena.h"
#include "physics/terrain_3d_collision_surface_builder.h"
#include "rendering/terrain_3d_compute.h"
#include "rendering/terrain_3d_material.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>
#include <utility>

typedef struct t3dControlCookSource_s {
	PackedByteArray m_bytes;
	bool m_bPresent = false;
	bool m_bImplicit = true;
} t3dControlCookSource_t;

static bool t3dDataReadCookControl( const eastl::array<t3dControlCookSource_t, 9> &rSources,
		const int nRegionSize,
		const int iX,
		const int iZ,
		uint32_t &nControl ) {
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

static uint32_t t3dDataReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset );

static uint32_t t3dDataMaterialMaskFromControlImage( const Ref<Image> &rControl ) {
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
		const uint32_t control = t3dDataReadU32Unaligned( pRead, index * 4u );
		mask |= 1u << ( uint32_t )( t3dGetBase( control ) & 31u );
		mask |= 1u << ( uint32_t )( t3dGetOverlay( control ) & 31u );
	}
	return mask != 0u ? mask : 1u;
}

static void t3dDataFnv1a64Append( uint64_t &nHash, const uint8_t *pData, const size_t nSize ) {
	for ( size_t index = 0u; index < nSize; ++index ) {
		nHash ^= ( uint64_t )( pData[index] );
		nHash *= 1099511628211ull;
	}
}

EA_FORCE_INLINE static void t3dDataFnv1a64AppendU32( uint64_t &nHash, const uint32_t nValue ) {
	t3dDataFnv1a64Append( nHash, ( const uint8_t * )( &nValue ), sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dDataFnv1a64AppendU64( uint64_t &nHash, const uint64_t nValue ) {
	t3dDataFnv1a64Append( nHash, ( const uint8_t * )( &nValue ), sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dDataFnv1a64AppendF32( uint64_t &nHash, const float fValue ) {
	uint32_t bits = 0u;
	static_assert( sizeof( bits ) == sizeof( fValue ) );
	std::memcpy( &bits, &fValue, sizeof( bits ) );
	t3dDataFnv1a64AppendU32( nHash, bits );
}

class Terrain3DDataLastScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;

public:
	Terrain3DDataLastScopeTimer( uint64_t &nElapsedUsec, const bool bEnabled ) {
		nElapsedUsec = 0u;
		if ( !bEnabled ) {
			return;
		}
		m_pElapsedUsec = &nElapsedUsec;
		Time *pTime = Time::get_singleton();
		m_nStartedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	}
	~Terrain3DDataLastScopeTimer( void ) {
		if ( !m_pElapsedUsec ) {
			return;
		}
		Time *pTime = Time::get_singleton();
		const uint64_t finishedUsec = pTime ? pTime->get_ticks_usec() : m_nStartedUsec;
		*m_pElapsedUsec = finishedUsec >= m_nStartedUsec ? finishedUsec - m_nStartedUsec : 0u;
	}
};

class Terrain3DDataProfileScopeTimer {
	uint64_t m_nStartedUsec = 0u;
	uint64_t *m_pElapsedUsec = nullptr;
	const uint64_t *m_pRequestUsec = nullptr;
	uint64_t *m_pEndToEndUsec = nullptr;

public:
	EA_FORCE_INLINE
	Terrain3DDataProfileScopeTimer( uint64_t &nElapsedUsec, const uint64_t &nRequestUsec, uint64_t &nEndToEndUsec ) :
			m_pElapsedUsec( &nElapsedUsec ), m_pRequestUsec( &nRequestUsec ), m_pEndToEndUsec( &nEndToEndUsec ) {
		Time *pTime = Time::get_singleton();
		m_nStartedUsec = pTime ? pTime->get_ticks_usec() : 0u;
	}

	~Terrain3DDataProfileScopeTimer( void ) {
		Time *pTime = Time::get_singleton();
		const uint64_t finishedUsec = pTime ? pTime->get_ticks_usec() : m_nStartedUsec;
		if ( m_pElapsedUsec ) {
			*m_pElapsedUsec = MAX( *m_pElapsedUsec, finishedUsec - m_nStartedUsec );
		}
		if ( m_pEndToEndUsec && m_pRequestUsec && *m_pRequestUsec > 0u && finishedUsec >= *m_pRequestUsec ) {
			*m_pEndToEndUsec = MAX( *m_pEndToEndUsec, finishedUsec - *m_pRequestUsec );
		}
	}
};

static uint32_t t3dDataReadU32Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint32_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

EA_FORCE_INLINE static uint8_t t3dDataColorToByte( const float fValue ) {
	return ( uint8_t )( CLAMP( Math::round( fValue * s_fByteMaxFloat ), 0.0f, s_fByteMaxFloat ) );
}

// Returns the half-open region-coordinate bounds touched by a positive-sized
// area. Computing the exclusive end independently is required for offset and
// negative-coordinate rectangles; ceil(area.size / region_size) is only valid
// when the area starts on a region boundary.
static Rect2i t3dDataGetRegionBoundsForArea( const Rect2i &rArea, const int nRegionSize ) {
	if ( nRegionSize <= 0 || rArea.size.x <= 0 || rArea.size.y <= 0 ) {
		return Rect2i();
	}

	const Vector2i begin = V2I_DIVIDE_FLOOR( rArea.position, nRegionSize );
	const Vector2i end = V2I_DIVIDE_CEIL( rArea.get_end(), nRegionSize );
	return Rect2i( begin, end - begin );
}

// -----------------------------------------------------------------------------
// Private functions

bool Terrain3DData::setRegionSizeInternal( const int nRegionSize ) {
	if ( nRegionSize != 128 ) {
		LOG( ERROR, "This Terrain3D fork requires 128-sample regions. Requested: ", nRegionSize );
		return false;
	}
	if ( !t3dIsValidRegionSize( nRegionSize ) || s_nActiveWorldTexels % nRegionSize != 0 ) {
		LOG( ERROR,
				"Region size ",
				nRegionSize,
				" cannot partition the fixed ",
				s_nActiveWorldTexels,
				"x",
				s_nActiveWorldTexels,
				" active terrain bank" );
		return false;
	}
	const bool bRegionSizeChanged = m_regionRepository.getRegionSize() != nRegionSize;
	if ( !m_samplingStorage.claimOrReconfigureActive( nRegionSize ) ) {
		LOG( ERROR, "Unable to claim process-static active terrain storage. Only one Terrain3DData owner is legal." );
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

Error Terrain3DData::rehydrateActiveStorage( void ) {
	if ( !m_samplingStorage.isActiveOwner() || m_regionRepository.getRegionSize() <= 0 ||
			m_regionRepository.getRegionMapSize() <= 0 ) {
		return ERR_UNCONFIGURED;
	}

	// The physical bank is intentionally smaller than the logical world. Clear its
	// descriptors here and let the normal spatial residency pass publish only the
	// current working set. This avoids a synchronous all-world Image -> byte-array
	// rebuild during scene activation.
	if ( !m_samplingStorage.replaceActiveHeightPages( m_regionRepository.getRegionSize(), nullptr, nullptr, 0u ) ) {
		LOG( ERROR, "Failed to reset active terrain page descriptors during scene activation" );
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	m_samplingStorage.refreshActiveGeneration();
	m_activeHeightResidency.markDirty();
	return OK;
}

bool Terrain3DData::syncActiveRegion( const Vector2i &vLocation, const Terrain3DRegion *pRegion ) {
	return m_samplingStorage.syncActiveRegion( vLocation, pRegion );
}

void Terrain3DData::clear( void ) {
	m_regionResidency.reset();
	m_runtimePageStore.reset();
	m_samplingStorage.resetActive();
	m_samplingStorage.resetResidentPool();
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
	resetMiddleHeightCache();
	resetGpuRegionCache( true );
	m_farSurfaceAtlasCache.reset();
	LOG( INFO, "Clearing data" );
	m_regionRepository.clear();
	m_mapEditService.reset();
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_defaultHeightMap.unref();
	m_defaultControlMap.unref();
	m_defaultColorMap.unref();
	m_defaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_generatedHeightMaps.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();
	m_generatedMiddleCompositeMaps.clear();
	invalidateSamplingCache();
}

void Terrain3DData::ensureDefaultMaps( void ) {
	if ( m_regionRepository.getRegionSize() <= 0 ) {
		return;
	}
	if ( m_defaultHeightMap.is_null() || m_defaultHeightMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_defaultHeightMap = Terrain3DUtil::getFilledImage( m_regionRepository.getRegionSizeVector(),
				t3dRegionCodecDefaultHeightColor(),
				false,
				Image::FORMAT_R16 );
	}
	if ( m_defaultControlMap.is_null() || m_defaultControlMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_defaultControlMap = Terrain3DUtil::getFilledImage(
				m_regionRepository.getRegionSizeVector(), COLOR_CONTROL, false, Image::FORMAT_RF );
	}
	if ( m_defaultColorMap.is_null() || m_defaultColorMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_defaultColorMap = Terrain3DUtil::getFilledImage(
				m_regionRepository.getRegionSizeVector(), COLOR_ROUGHNESS, false, Image::FORMAT_RGBA8 );
	}
	if ( m_defaultDerivedMap.is_null() || m_defaultDerivedMap->get_width() != m_regionRepository.getRegionSize() ) {
		m_defaultDerivedMap = Terrain3DUtil::getFilledImage(
				m_regionRepository.getRegionSizeVector(), Color( 0.5f, 0.5f, 0.0f, 1.0f ), false, Image::FORMAT_RG8 );
	}
	syncCacheDefaultMaps();
}

Ref<Image> Terrain3DData::getRegionMapOrDefault( const Terrain3DRegion *pRegion, const t3dMapType_t eMapType ) const {
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

bool Terrain3DData::hasMaterializedMap( const t3dMapType_t eMapType ) const {
	if ( eMapType != TYPE_CONTROL && eMapType != TYPE_COLOR ) {
		return true;
	}
	Terrain3DRegion *const *const ppRegions = m_regionRepository.getRegionPtrMapData();
	for ( int i = 0; i < s_nMaxRegionSlots; ++i ) {
		const Terrain3DRegion *const pRegion = ppRegions[i];
		if ( pRegion && !pRegion->isDeleted() && pRegion->hasChannel( eMapType ) ) {
			return true;
		}
	}
	return false;
}

bool Terrain3DData::refreshImplicitMapState( void ) {
	const Ref<Terrain3DMaterial> pMaterial = m_pTerrain ? m_pTerrain->getMaterial() : Ref<Terrain3DMaterial>();
	const bool bForceFullArrays = pMaterial.is_valid() &&
			( pMaterial->isShaderOverrideEnabled() || pMaterial->isBufferShaderOverrideEnabled() );
	const bool bControlImplicit = !bForceFullArrays && !hasMaterializedMap( TYPE_CONTROL );
	const bool bColorImplicit = !bForceFullArrays && !hasMaterializedMap( TYPE_COLOR );
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

void Terrain3DData::syncCacheDefaultMaps( void ) {
	m_gpuRegionCache.setTerrain( m_pTerrain );
	m_gpuRegionCache.setDefaultMaps( m_defaultHeightMap,
			m_defaultControlMap,
			m_defaultColorMap,
			m_defaultDerivedMap,
			m_bControlMapsImplicit,
			m_bColorMapsImplicit );
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
}

t3dFarSurfaceAtlasConfig_t Terrain3DData::getFarSurfaceAtlasConfig( void ) const {
	t3dFarSurfaceAtlasConfig_t config;
	config.m_bRequired = isFarAtlasRequired();
	config.m_bStrictCooked =
			m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode();
	config.m_nHeightAtlasSide = m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide;
	config.m_nSurfaceAtlasSide = m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : s_nGpuCacheFarHeightSide;
	config.m_dataDirectory = m_pTerrain ? m_pTerrain->getDataDirectory() : String();
	config.m_defaultHeightMap = m_defaultHeightMap;
	config.m_defaultControlMap = m_defaultControlMap;
	config.m_defaultColorMap = m_defaultColorMap;
	config.m_material = m_pTerrain ? m_pTerrain->getMaterial() : Ref<Terrain3DMaterial>();
	config.m_assets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	return config;
}

t3dMiddleHeightRegionCacheConfig_t Terrain3DData::getMiddleHeightCacheConfig( void ) const {
	t3dMiddleHeightRegionCacheConfig_t config;
	config.m_bAuthoring = m_pTerrain && m_pTerrain->isEditorAuthoringMode();
	config.m_nHotRadius = m_pTerrain ? m_pTerrain->getStreamingHotRadius() : 0;
	config.m_nResolutionDivisor =
			config.m_bAuthoring ? 1 : MAX( 1, m_pTerrain ? m_pTerrain->getGpuMiddleHeightResolutionDivisor() : 1 );
	config.m_nRetireFrames = m_pTerrain ? m_pTerrain->getGpuRegionRetireFrames() : s_nGpuCacheDefaultRetireFrames;
	return config;
}

void Terrain3DData::applyInstanceRegionCacheChanges( const t3dInstanceRegionCacheChanges_t &rChanges ) {
	if ( !m_pTerrain || !m_pTerrain->getInstancer() ) {
		return;
	}
	for ( int index = 0; index < rChanges.m_nEvicted; ++index ) {
		m_pTerrain->getInstancer()->destroyRegionMmis( rChanges.m_szEvicted[index] );
	}
	for ( int index = 0; index < rChanges.m_nLoaded; ++index ) {
		m_pTerrain->getInstancer()->updateMmis( -1, rChanges.m_szLoaded[index], false );
	}
}

Ref<Image> Terrain3DData::makeGpuColorImage( const Ref<Image> &rImage ) const {
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
		pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ),
				MAX( 1, pSource->get_height() / divisor ),
				Image::INTERPOLATE_BILINEAR );
	}
	if ( pGpuImage->generate_mipmaps() != OK ) {
		return Ref<Image>();
	}
	return pGpuImage;
}

Ref<Image> Terrain3DData::makeGpuDerivedImage( const Ref<Image> &rImage ) const {
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
	pGpuImage->resize( MAX( 1, pSource->get_width() / divisor ),
			MAX( 1, pSource->get_height() / divisor ),
			Image::INTERPOLATE_BILINEAR );
	return pGpuImage;
}

Ref<Image> Terrain3DData::makeMiddleCompositeImage( const Vector2i &rLocation,
		const Ref<Image> &rControl,
		const Ref<Image> &rColor ) const {
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
			color_bytes.size() < ( int64_t )( width )*height * 4 ) {
		return Ref<Image>();
	}
	PackedColorArray material_colors;
	const Ref<Terrain3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	if ( pAssets.is_valid() ) {
		material_colors = pAssets->getTextureColors();
	}
	const PackedFloat32Array material_scales =
			pAssets.is_valid() ? pAssets->getTextureUvScales() : PackedFloat32Array();
	eastl::array<Ref<Image>, s_nMaxTextureAssets> material_images;
	if ( pAssets.is_valid() ) {
		for ( int material = 0; material < MIN( pAssets->getTextureCount(), s_nMaxTextureAssets ); ++material ) {
			const Ref<Terrain3DTextureAsset> pAsset = pAssets->getTextureAsset( material );
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
			material_images[( size_t )( material )] = pMaterialImage;
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
			const uint32_t control = t3dDataReadU32Unaligned(
					pControl, ( size_t )( control_y * rControl->get_width() + control_x ) * 4u );
			const int base = ( int )( t3dGetBase( control ) );
			const int overlay = ( int )( t3dGetOverlay( control ) );
			const Color base_color = base < material_colors.size() ? material_colors[base] : s_colorWhite;
			const Color overlay_color = overlay < material_colors.size() ? material_colors[overlay] : base_color;
			const float control_scale = 0.9f - ( float )( ( ( t3dGetUvScale( control ) + 3u ) % 8u ) + 1u ) * 0.1f;
			const float angle = ( float )( t3dGetUvRotation( control ) )*Math_TAU / 16.0f;
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
						material_images[( size_t )( material_id )].is_null() ) {
					return rTint;
				}
				const Ref<Image> &pImage = material_images[( size_t )( material_id )];
				const float asset_scale = material_id < material_scales.size() ? material_scales[material_id] : 0.1f;
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
			pWrite[offset] = t3dDataColorToByte( authored_r * material_color.r );
			pWrite[offset + 1u] = t3dDataColorToByte( authored_g * material_color.g );
			pWrite[offset + 2u] = t3dDataColorToByte( authored_b * material_color.b );
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

Ref<Image> Terrain3DData::makeGpuRenderControlImage( const Vector2i &vLocation ) const {
	eastl::array<t3dControlCookSource_t, 9> sources;
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			const Vector2i location = vLocation + Vector2i( dx, dz );
			const int source_index = ( dz + 1 ) * 3 + ( dx + 1 );
			t3dControlCookSource_t &source = sources[( size_t )( source_index )];
			const Terrain3DRegion *pRegion = getRegionPtr( location );
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
			if ( source.m_bytes.size() !=
					m_regionRepository.getRegionSize() * m_regionRepository.getRegionSize() *
							( int )( sizeof( uint32_t ) ) ) {
				return Ref<Image>();
			}
			source.m_bImplicit = false;
		}
	}

	PackedByteArray output;
	output.resize(
			m_regionRepository.getRegionSize() * m_regionRepository.getRegionSize() * ( int )( sizeof( uint32_t ) ) );
	uint8_t *const pWrite = output.ptrw();
	for ( int z = 0; z < m_regionRepository.getRegionSize(); ++z ) {
		for ( int x = 0; x < m_regionRepository.getRegionSize(); ++x ) {
			uint32_t center = s_nRegionDefaultControl;
			if ( !t3dDataReadCookControl( sources, m_regionRepository.getRegionSize(), x, z, center ) ) {
				std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
						&center,
						sizeof( uint32_t ) );
				continue;
			}
			if ( t3dIsAuto( center ) || t3dIsHole( center ) ) {
				std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
						&center,
						sizeof( uint32_t ) );
				continue;
			}

			eastl::array<uint64_t, s_nMaxTextureAssets> material_weights{};
			for ( int dz = -s_nRenderControlBlendRadius; dz <= s_nRenderControlBlendRadius; ++dz ) {
				for ( int dx = -s_nRenderControlBlendRadius; dx <= s_nRenderControlBlendRadius; ++dx ) {
					uint32_t pControl = s_nRegionDefaultControl;
					if ( !t3dDataReadCookControl(
								 sources, m_regionRepository.getRegionSize(), x + dx, z + dz, pControl ) ||
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
						&center,
						sizeof( uint32_t ) );
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
				blend = ( uint8_t )( CLAMP(
						( int )( ( overlay_weight * 255u + pair_weight / 2u ) / pair_weight ), 0, 255 ) );
			}
			const uint32_t cooked = ( center & ~s_nRenderControlMaterialMask ) | t3dEncBase( ( uint8_t )( base ) ) |
					t3dEncOverlay( ( uint8_t )( overlay ) ) | t3dEncBlend( blend );
			std::memcpy( pWrite + ( size_t )( z * m_regionRepository.getRegionSize() + x ) * sizeof( uint32_t ),
					&cooked,
					sizeof( uint32_t ) );
		}
	}
	return Image::create_from_data(
			m_regionRepository.getRegionSize(), m_regionRepository.getRegionSize(), false, Image::FORMAT_RF, output );
}

t3dRuntimePageConfig_t Terrain3DData::getRuntimePageConfig( void ) const {
	t3dRuntimePageConfig_t config;
	if ( !m_pTerrain ) {
		return config;
	}
	config.m_dataDirectory = m_pTerrain->getDataDirectory();
	config.m_nColorDivisor = MAX( 1, m_pTerrain->getGpuRegionColorResolutionDivisor() );
	config.m_nDerivedDivisor = MAX( 1, m_pTerrain->getGpuRegionDerivedResolutionDivisor() );
	config.m_nMiddleHeightDivisor = MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
	config.m_nPrepareThreads = MAX( 1, m_pTerrain->getRuntimePagePrepareThreads() );
	config.m_bCookedPagesEnabled = m_pTerrain->isGpuRegionCookedPagesEnabled();
	config.m_bEditorAuthoring = m_pTerrain->isEditorAuthoringMode();
	const Ref<Terrain3DAssets> pAssets = m_pTerrain->getAssets();
	config.m_nMaterialSourceSignature = pAssets.is_valid() ? pAssets->getRuntimeMaterialSourceSignature() : 0u;
	const Ref<Terrain3DMaterial> material = m_pTerrain->getMaterial();
	if ( material.is_valid() ) {
		uint64_t collision_signature = 1469598103934665603ull;
		t3dDataFnv1a64AppendU32( collision_signature, ( uint32_t )material->getWorldBackground() );
		t3dDataFnv1a64AppendF32( collision_signature, ( float )( real_t )material->get( "ground_level" ) );
		t3dDataFnv1a64AppendF32( collision_signature, ( float )( real_t )material->get( "region_blend" ) );
		config.m_nCollisionSurfaceSignature = collision_signature;
	}
	return config;
}

t3dResidencyFrameState_t Terrain3DData::makeResidencyFrameState( const Vector3 &rWorldPosition ) const {
	t3dResidencyFrameState_t frame;
	frame.m_worldPosition = rWorldPosition;
	frame.m_nRegionMapSize = m_regionRepository.getRegionMapSize();
	const bool bGeometryReady = m_regionRepository.getRegionSize() > 0 && m_regionRepository.getVertexSpacing() > 0.0f;
	if ( bGeometryReady && frame.m_nRegionMapSize > 0 ) {
		const Vector2i rawCenter = m_regionRepository.getRegionLocation( rWorldPosition );
		const t3dGpuCacheCoord_t clampedCenter =
				t3dGpuCacheClampCenter( { rawCenter.x, rawCenter.y }, frame.m_nRegionMapSize );
		frame.m_residencyCenter = Vector2i( clampedCenter.m_nX, clampedCenter.m_nY );
	}
	frame.m_bCpuRegionCacheEnabled = m_cpuRegionCache.isEnabled();
	if ( !m_pTerrain ) {
		return frame;
	}
	frame.m_streamingCenter = m_pTerrain->getRegionStreamingCenterInternal();
	frame.m_predictedCenter = m_pTerrain->getRegionStreamingPredictedCenterInternal();
	if ( bGeometryReady ) {
		frame.m_activeHeightCenter = m_regionRepository.getRegionLocation( m_pTerrain->getClipmapTargetPosition() );
	}
	frame.m_pStreamingStates = m_pTerrain->getRegionStreamingStatesInternal();
	frame.m_pStreamingPrefetch = m_pTerrain->getRegionStreamingPrefetchInternal();
	frame.m_nStreamingGeneration = m_pTerrain->getRegionStreamingGeneration();
	frame.m_nStreamingActiveRadius = MAX( 0, m_pTerrain->getStreamingActiveRadius() );
	frame.m_nMiddleLods = m_pTerrain->getRuntimeMiddleShaderLods();
	frame.m_nGpuHysteresisTicks = m_pTerrain->getGpuRegionHysteresisTicks();
	frame.m_bAuthoring = m_pTerrain->isEditorAuthoringMode();
	frame.m_bRuntimeTiering = m_pTerrain->isRuntimeShaderTieringActive();
	frame.m_bCookedPagesEnabled = m_pTerrain->isGpuRegionCookedPagesEnabled();
	frame.m_bProfilingEnabled = m_pTerrain->isRuntimeProfilingEnabled();
	frame.m_runtimePageConfig = getRuntimePageConfig();
	return frame;
}

bool Terrain3DData::isRuntimePageWanted( void *const pContext, const Vector2i &rLocation ) {
	const t3dResidencyFrameState_t *const pFrame = static_cast<const t3dResidencyFrameState_t *>( pContext );
	return pFrame &&
			( pFrame->streamingState( rLocation ) >= TERRAIN3D_REGION_STREAMING_HOT ||
					pFrame->isPrefetch( rLocation ) );
}

uint64_t Terrain3DData::computeGpuRuntimePageSourceSignature( const Terrain3DRegion *const pRegion ) const {
	return m_runtimePageStore.computeSourceSignature( pRegion, getRuntimePageConfig() );
}

bool Terrain3DData::readGpuRuntimePageSourceSignature( const String &rPagePath, uint64_t &nSignature ) const {
	return m_runtimePageStore.readSourceSignature( rPagePath, nSignature );
}

String Terrain3DData::getGpuRuntimePagePath( const Terrain3DRegion *const pRegion ) const {
	return m_runtimePageStore.getPagePath( pRegion, m_pTerrain ? m_pTerrain->getDataDirectory() : String() );
}

void Terrain3DData::pollGpuRuntimePagePrepare( const bool bWait ) {
	m_runtimePageStore.poll( bWait );
}

bool Terrain3DData::isGpuRuntimePagePreparePending( const Vector2i &vLocation ) const {
	return m_runtimePageStore.isPending( vLocation );
}

void Terrain3DData::requestGpuRuntimePage( const Vector2i &vLocation, const uint16_t nConsumers, const uint8_t nPriority ) {
	m_runtimePageStore.request(
			vLocation, nConsumers, nPriority, m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() );
}

void Terrain3DData::serviceGpuRuntimePagePrepare( const t3dResidencyFrameState_t &rFrame ) {
	t3dRuntimePageServiceState_t state;
	state.m_config = rFrame.m_runtimePageConfig;
	state.m_predictedCenter = rFrame.m_predictedCenter;
	state.m_nStreamingGeneration = rFrame.m_nStreamingGeneration;
	state.m_isWanted = &Terrain3DData::isRuntimePageWanted;
	state.m_pWantedContext = const_cast<t3dResidencyFrameState_t *>( &rFrame );
	m_runtimePageStore.service( state );
}

bool Terrain3DData::takePreparedRuntimeHeight( const Vector2i &vLocation,
		const uint16_t nConsumer,
		Ref<Image> &rHeight,
		const bool bMiddle,
		PackedByteArray *const pNativeHeightBytes ) {
	return m_runtimePageStore.takeHeight( vLocation, nConsumer, rHeight, bMiddle, pNativeHeightBytes );
}

bool Terrain3DData::takePreparedRuntimeInstance( const Vector2i &vLocation, Terrain3DInstanceStore &rStore ) {
	return m_runtimePageStore.takeInstance( vLocation, rStore );
}

bool Terrain3DData::takePreparedGpuRuntimePage( const Vector2i &vLocation,
		Ref<Image> &rHeight,
		Ref<Image> &rControl,
		Ref<Image> &rColor,
		Ref<Image> &rDerived,
		Ref<Image> &rMiddleComposite,
		uint32_t &rMaterialMask,
		bool &bAttempted,
		uint64_t *const pRequestedUsec ) {
	return m_runtimePageStore.takeGpuPage( vLocation,
			rHeight,
			rControl,
			rColor,
			rDerived,
			rMiddleComposite,
			rMaterialMask,
			bAttempted,
			pRequestedUsec );
}

bool Terrain3DData::ensureCookedNodeDescriptors( const Vector2i &vLocation ) {
	Terrain3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->getStreamedNodeCount() == 0 || !pRegion->getStreamedNodesRuntimeInternal().empty() ) {
		return true;
	}
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_NODE_DESCRIPTORS, 0u );
	PackedByteArray descriptors;
	if ( !m_runtimePageStore.takeNodeDescriptors( vLocation, descriptors ) ) {
		return false;
	}
	if ( descriptors.is_empty() ) {
		return pRegion->getStreamedNodeCount() == 0;
	}
	return Terrain3DRuntimeBakeCodec::decodeNodeDescriptors( descriptors, m_runtimeWorldManifest, *pRegion );
}

PackedStringArray Terrain3DData::getCookedSceneDependencies( const Vector2i &vLocation ) const {
	PackedStringArray dependencies;
	const t3dRuntimeWorldRegionRecord_t *const pRecord = m_runtimeWorldManifest.findRegion( vLocation );
	if ( !pRecord ) {
		return dependencies;
	}
	const eastl::vector<uint16_t> &ids = m_runtimeWorldManifest.getRegionSceneDependencies();
	const uint32_t end = pRecord->m_nSceneDependencyFirst + pRecord->m_nSceneDependencyCount;
	for ( uint32_t index = pRecord->m_nSceneDependencyFirst; index < end && index < ids.size(); ++index ) {
		const String &path = m_runtimeWorldManifest.getSceneDependency( ids[index] );
		if ( !path.is_empty() ) {
			dependencies.push_back( path );
		}
	}
	return dependencies;
}

void Terrain3DData::requestCookedCollisionTile( const Vector2i &vLocation, const uint8_t nPriority ) {
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_COLLISION, nPriority );
}

bool Terrain3DData::takeCookedCollisionTile( const Vector2i &vLocation,
		PackedByteArray &rHeight,
		PackedByteArray &rBounds,
		PackedByteArray &rHoleMask ) {
	return m_runtimePageStore.takeCollision( vLocation, rHeight, rBounds, rHoleMask );
}

void Terrain3DData::requestCookedNavigationTile( const Vector2i &vLocation, const uint8_t nPriority ) {
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_NAVIGATION, nPriority );
}

bool Terrain3DData::takeCookedNavigationTile( const Vector2i &vLocation, PackedByteArray &rNavigation ) {
	return m_runtimePageStore.takeNavigation( vLocation, rNavigation );
}

void Terrain3DData::requestCookedTerrainShadowMask( const Vector2i &vLocation, const uint8_t nPriority ) {
	requestGpuRuntimePage( vLocation, RUNTIME_PAGE_CONSUMER_TERRAIN_SHADOW, nPriority );
}

bool Terrain3DData::takeCookedTerrainShadowMask( const Vector2i &vLocation, Ref<Image> &rShadowMask ) {
	return m_runtimePageStore.takeTerrainShadowMask( vLocation, rShadowMask );
}

bool Terrain3DData::loadCookedCollisionTileNow( const Vector2i &vLocation,
		PackedByteArray &rHeight,
		PackedByteArray &rBounds,
		PackedByteArray &rHoleMask ) const {
	return m_runtimePageStore.loadCollisionNow( vLocation, getRuntimePageConfig(), rHeight, rBounds, rHoleMask );
}

bool Terrain3DData::loadCookedNavigationTileNow( const Vector2i &vLocation,
		PackedByteArray &rNavigation,
		PackedByteArray *const pCollisionHeight,
		PackedByteArray *const pMinMaxHeight ) const {
	return m_runtimePageStore.loadNavigationNow(
			vLocation, getRuntimePageConfig(), rNavigation, pCollisionHeight, pMinMaxHeight );
}

bool Terrain3DData::loadCookedLightingReceiversNow( const Vector2i &vLocation, PackedByteArray &rReceivers ) const {
	return m_runtimePageStore.loadLightingReceiversNow( vLocation, getRuntimePageConfig(), rReceivers );
}

bool Terrain3DData::writeGpuRuntimePage( Terrain3DRegion *const pRegion, String *const pError ) {
	if ( !pRegion || pRegion->isDeleted() || !m_pTerrain ) {
		if ( pError ) {
			*pError = "region/terrain owner is unavailable";
		}
		return false;
	}

	const int region_size = m_regionRepository.getRegionSize();
	const Vector2i location = pRegion->getLocation();
	// Collision and edge-derived cook data can touch the positive X/Z neighbor
	// border. Keep only that 2x2 source footprint resident while the page is built.
	if ( m_cpuRegionCache.isEnabled() ) {
		for ( int dz = 0; dz <= 1; ++dz ) {
			for ( int dx = 0; dx <= 1; ++dx ) {
				const Vector2i neighbor = location + Vector2i( dx, dz );
				Terrain3DRegion *const pNeighbor = getRegionPtr( neighbor );
				if ( pNeighbor && !pNeighbor->isDeleted() && !ensureCpuRegionResidentInternal( neighbor, true ) ) {
					if ( pError ) {
						*pError = String( "collision bake neighbor could not be made CPU-resident: " ) + String( neighbor );
					}
					return false;
				}
			}
		}
	}

	t3dRuntimePageCookPayload_t payload;
	payload.m_height = pRegion->getHeightMap();
	const Ref<Image> authored_control = pRegion->getControlMap();
	if ( pRegion->hasChannel( TYPE_CONTROL ) ) {
		payload.m_control = makeGpuRenderControlImage( location );
	}
	if ( pRegion->hasChannel( TYPE_COLOR ) ) {
		payload.m_color = makeGpuColorImage( pRegion->getColorMap() );
	}
	payload.m_middleHeight = m_middleHeightRegionCache.makeHeightImage( location, true );
	payload.m_derived = pRegion->getDerivedNormalSlopeMap();
	if ( payload.m_derived.is_null() ) {
		regenerateDerivedMaps( { location } );
		payload.m_derived = pRegion->getDerivedNormalSlopeMap();
	}
	payload.m_derived = makeGpuDerivedImage( payload.m_derived );
	payload.m_middleComposite = makeMiddleCompositeImage( location,
			payload.m_control.is_valid() ? payload.m_control : m_defaultControlMap,
			payload.m_color.is_valid() ? payload.m_color : makeGpuColorImage( m_defaultColorMap ) );
	const Ref<Terrain3DRegionLighting> lighting = pRegion->getLighting();
	if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
		const Ref<Resource> shadow_resource =
				ResourceLoader::get_singleton()->load( lighting->getTerrainShadowMaskPath() );
		const Ref<Image> shadow_mask = shadow_resource;
		if ( shadow_mask.is_null() || shadow_mask->is_empty() || shadow_mask->get_format() != Image::FORMAT_L8 ||
				!shadow_mask->has_mipmaps() || shadow_mask->get_width() != shadow_mask->get_height() ) {
			if ( pError ) {
				*pError = "regional terrain shadow mask is missing or invalid";
			}
			return false;
		}
		payload.m_terrainShadowMask = shadow_mask;
	}
	payload.m_nMaterialMask =
			payload.m_control.is_valid() ? t3dDataMaterialMaskFromControlImage( payload.m_control ) : 1u;

	// Bake compact logical masks from authored control data, not the render halo.
	payload.m_holeMask = Terrain3DRuntimeBakeCodec::encodeHoleMask( authored_control, region_size );
	payload.m_minMaxHeight = Terrain3DRuntimeBakeCodec::encodeMinMaxHierarchy( payload.m_height, region_size );

	// Navigation classification uses the same positive border as collision so each
	// region page can generate all of its quads without consulting a neighbor page.
	const int collision_width = region_size + 1;
	PackedInt32Array navigation_controls;
	navigation_controls.resize( collision_width * collision_width );
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	const int grid_x = location.x * region_size;
	const int grid_z = location.y * region_size;
	for ( int z = 0; z < collision_width; ++z ) {
		for ( int x = 0; x < collision_width; ++x ) {
			navigation_controls.set(
					z * collision_width + x, ( int32_t )sampler.sampleControlGrid( grid_x + x, grid_z + z ) );
		}
	}
	payload.m_navigation = Terrain3DRuntimeBakeCodec::encodeNavigationMask( navigation_controls, collision_width );

	// Build the exact final physics surface through the same canonical path used
	// by live collision. This keeps holes and FLAT/NOISE world-background blend
	// semantics identical between cooked and sampler-backed tiles.
	PackedFloat32Array collision_heights;
	float collision_min = 0.0f;
	float collision_max = 0.0f;
	Terrain3DCollisionSurfaceBuilder collision_builder;
	if ( !collision_builder.build( this,
				 m_pTerrain->getMaterial(),
				 Vector2i( grid_x, grid_z ),
				 region_size,
				 collision_heights,
				 collision_min,
				 collision_max ) ) {
		if ( pError ) {
			*pError = "canonical collision surface generation failed";
		}
		return false;
	}
	payload.m_collisionHeight = Terrain3DRuntimeBakeCodec::encodeCollisionHeight( collision_heights, collision_width );
	payload.m_collisionBounds =
			Terrain3DRuntimeBakeCodec::encodeCollisionBounds( collision_min, collision_max, collision_width );

	if ( pRegion->getStreamedNodeCount() > 0 ) {
		payload.m_nodeDescriptors = Terrain3DRuntimeBakeCodec::encodeNodeDescriptors( *pRegion, m_runtimeWorldManifest );
		if ( payload.m_nodeDescriptors.is_empty() ) {
			if ( pError ) {
				*pError = "streamed-node descriptor bake failed";
			}
			return false;
		}
	}
	if ( pRegion->hasInstances() ) {
		payload.m_instanceMetadata = Terrain3DRuntimeBakeCodec::encodeInstanceMetadata( *pRegion );
		payload.m_multimeshFastStart = Terrain3DRuntimeBakeCodec::encodeMultimeshFastStart( *pRegion );
	}
	payload.m_lightingReceivers = Terrain3DRuntimeBakeCodec::encodeLightingReceivers( *pRegion );

	if ( payload.m_holeMask.is_empty() || payload.m_navigation.is_empty() || payload.m_minMaxHeight.is_empty() ||
			payload.m_collisionHeight.is_empty() || payload.m_collisionBounds.is_empty() ) {
		if ( pError ) {
			*pError = "mandatory baked runtime metadata generation failed";
		}
		return false;
	}
	return m_runtimePageStore.writePage( pRegion, getRuntimePageConfig(), payload, pError );
}

int Terrain3DData::getRegionSlotReserve( void ) const {
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

bool Terrain3DData::canUpdateRegionSlotsIncrementally( void ) const {
	if ( m_gpuRegionCache.isEnabled() ) {
		return false;
	}
	const int slotCount = m_regionRepository.getSlotCount();
	return !m_regionRepository.isRegionMapDirty() && slotCount > 0 && !m_generatedHeightMaps.isDirty() &&
			!m_generatedControlMaps.isDirty() && !m_generatedColorMaps.isDirty() && !m_generatedDerivedMaps.isDirty() &&
			m_generatedHeightMaps.size() == slotCount && m_generatedControlMaps.size() == slotCount &&
			m_generatedColorMaps.size() == slotCount && m_generatedDerivedMaps.size() == slotCount &&
			m_heightMaps.size() == slotCount && m_controlMaps.size() == slotCount && m_colorMaps.size() == slotCount &&
			m_derivedMaps.size() == slotCount;
}

int Terrain3DData::calculateGpuRegionRequiredCorePagesForLods( const int nDetailLods ) const {
	return m_gpuRegionCache.calculateRequiredCorePagesForLods( nDetailLods );
}

int Terrain3DData::calculateGpuRegionCapacityForLods( const int nDetailLods ) const {
	return m_gpuRegionCache.calculateCapacityForLods( nDetailLods );
}

bool Terrain3DData::isGpuRegionColorChannelEnabled( void ) const {
	return m_gpuRegionCache.isColorChannelEnabled();
}

bool Terrain3DData::isGpuRegionDerivedChannelEnabled( void ) const {
	return m_gpuRegionCache.isDerivedChannelEnabled();
}

void Terrain3DData::resetGpuRegionCache( const bool bClearTextures ) {
	m_gpuRegionCache.reset( bClearTextures );
}

bool Terrain3DData::isFarAtlasRequired( void ) const {
	return m_gpuRegionCache.isEnabled() || ( m_pTerrain && m_pTerrain->isRuntimeShaderTieringActive() );
}

bool Terrain3DData::writeCookedGpuFarAtlases( const String &rDirectory ) {
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	return m_farSurfaceAtlasCache.writeCooked( rDirectory );
}

void Terrain3DData::markAllGpuFarRegionsDirty( const t3dMapType_t eMapType ) {
	m_farSurfaceAtlasCache.markAllDirty( eMapType );
}

void Terrain3DData::markGpuFarRegionDirty( const Vector2i &vLocation, const t3dMapType_t eMapType ) {
	m_farSurfaceAtlasCache.markRegionDirty( vLocation, eMapType );
}

void Terrain3DData::armPendingGpuRegionRetirements( void ) {
	m_gpuRegionCache.armPendingRetirements();
}

void Terrain3DData::invalidateGpuRegionLocation( const Vector2i &vLocation ) {
	m_gpuRegionCache.invalidate( vLocation );
}

void Terrain3DData::queueGpuRegionRepublish( const Vector2i &vLocation ) {
	m_gpuRegionCache.queueRepublish( vLocation );
}

bool Terrain3DData::republishGpuRegionLocation( const Vector2i &vLocation ) {
	const bool bChanged = m_gpuRegionCache.republish( vLocation, getComputeBackendIfCreated() );
	if ( bChanged ) {
		emitGpuRegionMapChanged();
	}
	return bChanged;
}

void Terrain3DData::resetMiddleHeightCache( void ) {
	m_middleHeightRegionCache.reset();
}

bool Terrain3DData::configureMiddleHeightCache( void ) {
	return m_middleHeightRegionCache.configure( getMiddleHeightCacheConfig() );
}

void Terrain3DData::updateMiddleHeightResidencyInternal( const t3dResidencyFrameState_t &rFrame, const bool bForce ) {
	m_middleHeightRegionCache.update(
			rFrame, bForce, getEffectiveGpuRegionUploadBudgetLayers(), getMiddleHeightCacheConfig() );
}

Dictionary Terrain3DData::getMiddleHeightCacheStats( void ) const {
	return m_middleHeightRegionCache.getStatsDictionary();
}

int Terrain3DData::fillGpuMaterialPriorityIds( uint8_t *const pIds, const int nCapacity ) const {
	return m_gpuRegionCache.fillMaterialPriorityIds( pIds, nCapacity );
}

size_t Terrain3DData::getMiddleHeightCacheReservedBytes( void ) const {
	return m_middleHeightRegionCache.getReservedBytes();
}

void Terrain3DData::resetCpuRegionCache( const bool bReleaseCleanMaps ) {
	m_cpuRegionCache.reset( bReleaseCleanMaps );
}

void Terrain3DData::invalidateCpuRegionLocation( const Vector2i &vLocation ) {
	m_cpuRegionCache.invalidate( vLocation );
}

bool Terrain3DData::requestCpuRegionSourceAsync( const Vector2i &vLocation ) {
	return m_cpuRegionCache.requestSourceAsync( vLocation );
}

bool Terrain3DData::ensureCpuRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	return m_cpuRegionCache.ensureResident( vLocation, bDemandLoad );
}

void Terrain3DData::setRuntimePageBudgetOverrides( const int nGpuUploadLayers,
		const int nCpuLoadLayers,
		const int nInstanceLoadPages ) {
	m_regionResidency.setBudgetOverrides( nGpuUploadLayers, nCpuLoadLayers, nInstanceLoadPages );
}

void Terrain3DData::clearRuntimePageBudgetOverrides( void ) {
	m_regionResidency.clearBudgetOverrides();
}

int Terrain3DData::getEffectiveGpuRegionUploadBudgetLayers( void ) const {
	return m_regionResidency.effectiveGpuUploadLayers( m_gpuRegionCache.getUploadBudgetLayers() );
}

int Terrain3DData::getEffectiveCpuRegionLoadBudgetLayers( void ) const {
	return m_regionResidency.effectiveCpuLoadLayers( m_cpuRegionCache.getLoadBudgetLayers() );
}

int Terrain3DData::getEffectiveInstanceRegionLoadBudgetPages( void ) const {
	return m_regionResidency.effectiveInstanceLoadPages( m_instanceRegionCache.getLoadBudgetPages() );
}

void Terrain3DData::configureCpuRegionCache( const bool bEnabled,
		const int nBudgetMb,
		const int nLoadLayers,
		const int nLoadBudgetMb ) {
	m_cpuRegionCache.configure( bEnabled, nBudgetMb, nLoadLayers, nLoadBudgetMb );
}

bool Terrain3DData::ensureCpuRegionResident( const Vector2i &vLocation ) {
	return m_cpuRegionCache.ensureResident( vLocation, true );
}

bool Terrain3DData::ensureCpuSampleArea( const Vector3 &rWorldPosition, const int nRadiusTexels ) {
	return m_cpuRegionCache.ensureSampleArea( rWorldPosition, nRadiusTexels );
}

void Terrain3DData::updateCpuRegionResidencyInternal( const t3dResidencyFrameState_t &rFrame, const bool bForce ) {
	m_cpuRegionCache.update( rFrame, bForce, getEffectiveCpuRegionLoadBudgetLayers() );
}

Dictionary Terrain3DData::getCpuRegionCacheStatsFast( void ) const {
	return m_cpuRegionCache.getStatsDictionary( false, getEffectiveCpuRegionLoadBudgetLayers() );
}

Dictionary Terrain3DData::getCpuRegionCacheStats( void ) const {
	return m_cpuRegionCache.getStatsDictionary( true, getEffectiveCpuRegionLoadBudgetLayers() );
}

void Terrain3DData::emitGpuRegionMapChanged( void ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	m_gpuRegionCache.recordPageTableUpdate();
	emit_signal( "gpu_region_page_table_changed" );
	armPendingGpuRegionRetirements();
}

void Terrain3DData::resetInstanceRegionCache( const bool bRestoreAll ) {
	t3dInstanceRegionCacheChanges_t changes;
	m_instanceRegionCache.reset( bRestoreAll, &changes );
	applyInstanceRegionCacheChanges( changes );
}

void Terrain3DData::invalidateInstanceRegionLocation( const Vector2i &vLocation ) {
	t3dInstanceRegionCacheChanges_t changes;
	m_instanceRegionCache.invalidate( vLocation, &changes );
	applyInstanceRegionCacheChanges( changes );
}

bool Terrain3DData::ensureInstanceRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	t3dInstanceRegionCacheChanges_t changes;
	const bool bReady = m_instanceRegionCache.ensureResident( vLocation, bDemandLoad, &changes );
	applyInstanceRegionCacheChanges( changes );
	return bReady;
}

bool Terrain3DData::evictInstanceRegion( const Vector2i &vLocation ) {
	t3dInstanceRegionCacheChanges_t changes;
	const bool bEvicted = m_instanceRegionCache.evict( vLocation, &changes );
	applyInstanceRegionCacheChanges( changes );
	return bEvicted;
}

void Terrain3DData::configureInstanceRegionCache( const bool bEnabled,
		const int nBudgetMb,
		const int nLoadPages,
		const int nLoadBudgetMb ) {
	t3dInstanceRegionCacheChanges_t changes;
	m_instanceRegionCache.configure( bEnabled, nBudgetMb, nLoadPages, nLoadBudgetMb, &changes );
	applyInstanceRegionCacheChanges( changes );
}

void Terrain3DData::updateInstanceRegionResidencyInternal( const t3dResidencyFrameState_t &rFrame, const bool bForce ) {
	t3dInstanceRegionCacheChanges_t changes;
	m_instanceRegionCache.update( rFrame, bForce, getEffectiveInstanceRegionLoadBudgetPages(), changes );
	applyInstanceRegionCacheChanges( changes );
}

void Terrain3DData::updateCpuRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	updateCpuRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::updateInstanceRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	updateInstanceRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::updateGpuRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	updateGpuRegionResidencyInternal( makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::updateRegionResidency( const Vector3 &rWorldPosition, const bool bForce ) {
	m_regionResidency.update( *this, makeResidencyFrameState( rWorldPosition ), bForce );
}

void Terrain3DData::resetRuntimeStreamingCaches( void ) {
	m_regionResidency.resetRuntimeCaches( *this );
}

void Terrain3DData::appendRuntimeProfileSample( t3dRuntimeProfileSample_t &rSample ) const {
	m_regionResidency.appendRuntimeProfileSample( *this, rSample );
}

Dictionary Terrain3DData::getInstanceRegionCacheStatsFast( void ) const {
	return m_instanceRegionCache.getStatsDictionary( false, getEffectiveInstanceRegionLoadBudgetPages() );
}

Dictionary Terrain3DData::getInstanceRegionCacheStats( void ) const {
	return m_instanceRegionCache.getStatsDictionary( true, getEffectiveInstanceRegionLoadBudgetPages() );
}

void Terrain3DData::updateMapsBounded( const t3dMapType_t eMapType, const bool bAllRegions ) {
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

	if ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX ) {
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
		const bool bHeightChanged = bAllRegions && ( eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX );
		const bool bControlChanged = bAllRegions && ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX );
		const bool bColorChanged = bAllRegions && ( eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX );
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

void Terrain3DData::emitRegionLayoutChanged( const bool bTextureLayersChanged ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	LOG( DEBUG, "Emitting region_map_changed" );
	emit_signal( "region_map_changed" );
	if ( bTextureLayersChanged ) {
		LOG( DEBUG, "Emitting height_maps_changed" );
		emit_signal( "height_maps_changed" );
		LOG( DEBUG, "Emitting control_maps_changed" );
		emit_signal( "control_maps_changed" );
		LOG( DEBUG, "Emitting color_maps_changed" );
		emit_signal( "color_maps_changed" );
	}
	LOG( DEBUG, "Emitting maps_changed" );
	emit_signal( "maps_changed" );
	m_pTerrain->snap();
}

TypedArray<Vector2i> Terrain3DData::getRegionLocations( void ) const {
	return m_regionRepository.getActiveLocations();
}

TypedArray<Image> Terrain3DData::getActiveMaps( const t3dMapType_t eMapType ) const {
	TypedArray<Image> maps;
	if ( m_cpuRegionCache.isEnabled() ) {
		LOG( ERROR,
				"Whole-world map arrays are unavailable while bounded CPU residency is enabled. "
				"Page one region with ensure_cpu_region_resident() and read that Terrain3DRegion, or disable the CPU "
				"cache." );
		return maps;
	}
	maps.resize( m_regionRepository.getActiveRegionCount() );
	int write_index = 0;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		if ( location == s_v2iMax ) {
			continue;
		}
		const Terrain3DRegion *pRegion = getRegionPtr( location );
		if ( pRegion && !pRegion->isDeleted() ) {
			maps[write_index++] = getRegionMapOrDefault( pRegion, eMapType );
		}
	}
	if ( write_index != maps.size() ) {
		maps.resize( write_index );
	}
	return maps;
}

TypedArray<Image> Terrain3DData::getHeightMaps( void ) const {
	return getActiveMaps( TYPE_HEIGHT );
}

TypedArray<Image> Terrain3DData::getControlMaps( void ) const {
	return getActiveMaps( TYPE_CONTROL );
}

TypedArray<Image> Terrain3DData::getColorMaps( void ) const {
	return getActiveMaps( TYPE_COLOR );
}

// Specialized copy/paste callback used by doForRegions().
void Terrain3DData::copyPasteDfr( const Terrain3DRegion *pSrcRegion,
		const Rect2i &rSrcRect,
		const Rect2i &rDstRect,
		Terrain3DRegion *pDstRegion ) {
	if ( !pSrcRegion || !pDstRegion ) {
		return;
	}
	m_mapEditService.copyRegionMaps( pSrcRegion, rSrcRect, rDstRect, pDstRegion );
	m_pTerrain->getInstancer()->copyPasteDfr( pSrcRegion, rSrcRect, pDstRegion );
}

// -----------------------------------------------------------------------------
// Public functions

Terrain3DData::Terrain3DData( void ) :
		m_mapEditService( &m_regionRepository, &m_samplingStorage ),
		m_regionPersistence( &m_regionRepository ),
		m_runtimePageStore( &m_regionRepository, &m_regionPersistence ),
		m_cpuRegionCache( &m_regionRepository, &m_regionPersistence, &m_samplingStorage, &m_runtimePageStore ),
		m_instanceRegionCache( &m_regionRepository, &m_runtimePageStore ),
		m_middleHeightRegionCache( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore ),
		m_gpuRegionCache( m_regionRepository,
				m_samplingStorage,
				m_mapEditService,
				m_runtimePageStore,
				m_cpuRegionCache ),
		m_farSurfaceAtlasCache( &m_regionRepository, &m_samplingStorage, &m_cpuRegionCache ),
		m_activeHeightResidency( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore ),
		m_regionResidency( &m_regionRepository,
				&m_samplingStorage,
				&m_runtimePageStore,
				&m_activeHeightResidency,
				&m_cpuRegionCache,
				&m_instanceRegionCache,
				&m_middleHeightRegionCache,
				&m_gpuRegionCache,
				&m_farSurfaceAtlasCache ) {}

Terrain3DData::~Terrain3DData( void ) {
	clear();
	if ( m_pCompute ) {
		memdelete( m_pCompute );
		m_pCompute = nullptr;
	}
}

Terrain3DCompute *Terrain3DData::getComputeBackend( void ) {
	if ( !m_pCompute ) {
		m_pCompute = memnew( Terrain3DCompute );
	}
	return m_pCompute;
}

void Terrain3DData::configureResidentPool( const int nCapacity ) {
	if ( !m_samplingStorage.configureResidentPool( m_regionRepository.getRegionSize(),
				 m_regionRepository.getRegionMapSize(),
				 m_bControlMapsImplicit,
				 nCapacity,
				 s_nDefaultResidentPoolBudget ) ) {
		LOG( WARN, "Unable to configure experimental resident region map pool" );
	}
}

void Terrain3DData::configureGpuRegionCache( const bool bEnabled,
		const int nBudgetMb,
		const int nUploadLayers,
		const int nUploadBudgetMb,
		const int nSparePages,
		const int nRetireFrames ) {
	ensureDefaultMaps();
	m_gpuRegionCache.setTerrain( m_pTerrain );
	syncCacheDefaultMaps();
	const bool bStorageChanged = m_gpuRegionCache.configure(
			bEnabled, nBudgetMb, nUploadLayers, nUploadBudgetMb, nSparePages, nRetireFrames );
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	if ( bStorageChanged && m_pTerrain && m_regionRepository.getRegionSize() > 0 ) {
		updateMaps( REGION_TYPE_MAX, true, false );
	}
}

void Terrain3DData::updateGpuRegionResidencyInternal( const t3dResidencyFrameState_t &rFrame, const bool bForce ) {
	m_farSurfaceAtlasCache.resetFrameStats();
	if ( isFarAtlasRequired() ) {
		ensureFarAtlases();
	}
	const t3dGpuRegionCacheUpdateResult_t result = m_gpuRegionCache.update(
			rFrame, bForce, getEffectiveGpuRegionUploadBudgetLayers(), getComputeBackendIfCreated() );
	if ( result.m_bPageTableChanged ) {
		emitGpuRegionMapChanged();
	}
}

void Terrain3DData::invalidateGpuFarSurface( void ) {
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	m_farSurfaceAtlasCache.invalidateSurface();
	if ( isFarAtlasRequired() && !m_gpuRegionCache.isEnabled() ) {
		ensureFarAtlases();
	}
}

void Terrain3DData::invalidateGpuFarAtlasLayout( void ) {
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	m_farSurfaceAtlasCache.invalidateLayout();
	if ( isFarAtlasRequired() && m_samplingStorage.isActiveOwner() && m_regionRepository.getRegionSize() > 0 &&
			m_regionRepository.getRegionMapSize() > 0 ) {
		ensureFarAtlases();
	}
}

bool Terrain3DData::ensureFarAtlases( void ) {
	if ( !isFarAtlasRequired() || !m_samplingStorage.isActiveOwner() || m_regionRepository.getRegionSize() <= 0 ||
			m_regionRepository.getRegionMapSize() <= 0 ) {
		return false;
	}
	m_farSurfaceAtlasCache.setConfig( getFarSurfaceAtlasConfig() );
	const bool bReady = m_farSurfaceAtlasCache.ensure();
	if ( bReady && m_pTerrain ) {
		const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateFarAtlasTextures();
		}
	}
	return bReady;
}

Dictionary Terrain3DData::getRuntimeResidencyGrid( const int nRadius ) const {
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
			const t3dRegionStreamingState_t state = m_pTerrain && bValid
					? m_pTerrain->getRegionStreamingStateInternal( location )
					: TERRAIN3D_REGION_STREAMING_UNLOADED;
			const bool predicted = m_pTerrain && bValid && m_pTerrain->isRegionStreamingPrefetchInternal( location );
			cell["valid_region_coordinate"] = bValid;
			cell["active"] = bAuthored;
			cell["streaming_state"] = ( int )( state );
			cell["streaming_state_name"] = String( t3dStreamingStateName( state ) );
			cell["predicted"] = predicted;
			cell["desired_class"] = state == TERRAIN3D_REGION_STREAMING_ACTIVE
					? 1
					: ( state == TERRAIN3D_REGION_STREAMING_HOT ? 2 : ( predicted ? 3 : 0 ) );
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

Dictionary Terrain3DData::getGpuRegionCacheStatsFast( void ) const {
	Dictionary stats = m_gpuRegionCache.getStatsDictionary( false );
	const Dictionary farStats = m_farSurfaceAtlasCache.getStatsDictionary();
	const Array keys = farStats.keys();
	for ( int index = 0; index < keys.size(); ++index ) {
		stats[keys[index]] = farStats[keys[index]];
	}
	return stats;
}

bool Terrain3DData::isAuthoringGpuFullResidencyPossible( void ) const {
	return m_gpuRegionCache.isAuthoringFullResidencyPossible();
}

bool Terrain3DData::isAuthoringGpuReadbackSafe( void ) const {
	return m_gpuRegionCache.isAuthoringReadbackSafe();
}

Dictionary Terrain3DData::getStartupLoadStats( void ) const {
	return m_regionPersistence.getStartupLoadStatsDictionary();
}

Dictionary Terrain3DData::getGpuRegionCacheStats( void ) const {
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

size_t Terrain3DData::getGpuRegionCacheReservedBytes( void ) const {
	return m_gpuRegionCache.getReservedBytes();
}

size_t Terrain3DData::getGpuRegionCacheMinimumBytes( void ) const {
	return m_gpuRegionCache.getMinimumBytes();
}

size_t Terrain3DData::getGpuRegionCacheHardCapBytes( void ) const {
	return m_gpuRegionCache.getHardCapBytes();
}

size_t Terrain3DData::getCpuRegionCacheReservedBytes( void ) const {
	return m_cpuRegionCache.getReservedBytes();
}

size_t Terrain3DData::getCpuRegionCacheMinimumBytes( void ) const {
	return m_cpuRegionCache.getMinimumBytes();
}

size_t Terrain3DData::getCpuRegionCacheHardCapBytes( void ) const {
	return m_cpuRegionCache.getHardCapBytes();
}

size_t Terrain3DData::getMemoryTransientStagingBytes( void ) const {
	const size_t preparedPageBytes = m_cpuRegionCache.estimateLayerBytes();
	size_t bytes = preparedPageBytes * ( size_t )( MAX( 0, m_runtimePageStore.getTransientPageCount() ) );
	bytes += m_mapEditService.getTransientScratchBytes();
	bytes += m_farSurfaceAtlasCache.getImageBytes();
	return bytes;
}

size_t Terrain3DData::getMemoryFixedRepositoryBytes( void ) const {
	return sizeof( *this ) + m_samplingStorage.getActiveReservedBytes() + m_samplingStorage.getResidentReservedBytes();
}

Error Terrain3DData::initialize( Terrain3D *const pTerrain ) {
	if ( !pTerrain ) {
		LOG( ERROR, "Initialization failed, p_terrain is null" );
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
	LOG( INFO, "Initializing data runtime" );
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
			m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(),
			m_pTerrain->getCpuRegionLoadBudgetLayers(),
			m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
			m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(),
			m_pTerrain->getInstanceRegionLoadBudgetPages(),
			m_pTerrain->getInstanceRegionLoadBudgetMb() );
	configureGpuRegionCache( m_pTerrain->isGpuRegionCacheEffective(),
			m_pTerrain->getEffectiveGpuRegionCacheBudgetMb(),
			m_pTerrain->getGpuRegionUploadBudgetLayers(),
			m_pTerrain->getGpuRegionUploadBudgetMb(),
			m_pTerrain->getGpuRegionSparePages(),
			m_pTerrain->getGpuRegionRetireFrames() );

	bool bLoadedDirectoryThisActivation = false;
	if ( bLoadInitialDirectory ) {
		const Error load_error = loadDirectory( m_pTerrain->getDataDirectory() );
		m_bStartupBulkLoad = false;
		m_bSkipSlotReserveOnce = false;
		if ( load_error != OK && load_error != ERR_FILE_NOT_FOUND ) {
			LOG( ERROR, "Failed to load terrain directory: ", m_pTerrain->getDataDirectory(), " error: ", load_error );
			deactivateRuntimeStorage( pTerrain );
			return load_error;
		}
		bLoadedDirectoryThisActivation = load_error == OK;
	}
	m_regionRepository.setLoaded( true );

	// A returning scene owns a complete repository but process-static height pages
	// were deliberately released while inactive. Rebuild the logical map from the
	// retained slot table and repopulate active storage before any renderer sees it.
	if ( !bLoadedDirectoryThisActivation &&
			( m_regionRepository.getActiveRegionCount() > 0 || m_regionRepository.getSlotCount() > 0 ) ) {
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

void Terrain3DData::deactivateRuntimeStorage( Terrain3D *const pTerrain ) {
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

Dictionary Terrain3DData::getRuntimeState( void ) const {
	Dictionary state;
	state["terrain_initialized"] = m_pTerrain && m_pTerrain->isInitialized();
	state["repository_loaded"] = m_regionRepository.isLoaded();
	state["repository_topology_generation"] = ( int64_t )( m_regionRepository.getTopologyGeneration() );
	state["runtime_active"] = m_bRuntimeActive;
	state["active_storage_attached"] = m_samplingStorage.isActiveOwner();
	state["active_storage_generation"] = ( int64_t )( getActiveStorageGeneration() );
	state["region_map_size"] = m_regionRepository.getRegionMapSize();
	state["logical_region_count"] = m_regionRepository.getActiveRegionCount();
	state["gpu_resident_page_count"] = m_gpuRegionCache.getResidentCount();
	state["middle_height_resident_page_count"] = m_middleHeightRegionCache.getResidentCount();
	state["middle_height_page_table_valid"] = m_middleHeightRegionCache.getRegionMapRid().is_valid();
	return state;
}

void Terrain3DData::setRegionLocations( const TypedArray<Vector2i> &vLocations ) {
	LOG( INFO, "Reconciling active region locations with array sized: ", vLocations.size() );

	// Public locations are compact while render texture slots deliberately retain
	// holes. The repository owns that stable slot topology; Terrain3DData only performs
	// the cache/render side effects caused by the topology change.
	const bool bArraysReady = canUpdateRegionSlotsIncrementally();
	const t3dRegionLocationReconcileResult_t reconcile = m_regionRepository.reconcileLocations( vLocations );
	if ( reconcile.m_bCapacityExceeded ) {
		LOG( ERROR, "Region slot capacity exceeded while reconciling active locations" );
	}

	for ( const Vector2i location : reconcile.m_removedLocations ) {
		m_samplingStorage.invalidateResident( location );
		if ( m_samplingStorage.isActiveOwner() ) {
			m_samplingStorage.invalidateActive( location );
		}
		invalidateGpuRegionLocation( location );
		invalidateCpuRegionLocation( location );
		invalidateInstanceRegionLocation( location );
	}

	if ( !reconcile.m_bSlotsChanged ) {
		if ( m_regionRepository.isRegionMapDirty() ) {
			updateMaps( REGION_TYPE_MAX, false, false );
		}
		return;
	}

	const bool bCanUploadAddedLayers = bArraysReady && m_regionRepository.getSlotCount() == reconcile.m_nOldSlotCount;
	if ( !bCanUploadAddedLayers ) {
		updateMaps( REGION_TYPE_MAX, true, false );
		if ( m_pTerrain && m_pTerrain->getInstancer() ) {
			m_pTerrain->getInstancer()->updateMmis( -1, s_v2iMax, false );
		}
		invalidateSamplingCache();
		return;
	}

	m_regionRepository.rebuildRegionMapFromSlots();
	bool bTextureLayersChanged = false;
	const Vector2i *const pSlots = m_regionRepository.getSlotsData();
	for ( const int slot : reconcile.m_addedSlots ) {
		if ( slot < 0 || slot >= m_regionRepository.getSlotCount() ) {
			continue;
		}
		const Vector2i location = pSlots[slot];
		if ( location == s_v2iMax ) {
			continue;
		}
		Terrain3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			updateMaps( REGION_TYPE_MAX, true, false );
			if ( m_pTerrain && m_pTerrain->getInstancer() ) {
				m_pTerrain->getInstancer()->updateMmis( -1, s_v2iMax, false );
			}
			invalidateSamplingCache();
			return;
		}

		if ( !syncActiveRegion( location, pRegion ) ) {
			LOG( ERROR, "Unable to restore active terrain storage for region ", location );
			continue;
		}
		ensureDefaultMaps();
		m_heightMaps[slot] = getRegionMapOrDefault( pRegion, TYPE_HEIGHT );
		m_controlMaps[slot] = getRegionMapOrDefault( pRegion, TYPE_CONTROL );
		m_colorMaps[slot] = getRegionMapOrDefault( pRegion, TYPE_COLOR );
		Ref<Image> pDerived = pRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( { location } );
			pDerived = pRegion->getDerivedNormalSlopeMap();
		}
		pDerived = pDerived.is_valid() ? pDerived : m_defaultDerivedMap;
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
		m_pTerrain->getInstancer()->updateMmis( -1, s_v2iMax, false );
	}
	invalidateSamplingCache();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after setRegionLocations" );
	}
#endif
}

// Returns an array of active regions, optionally a shallow or deep copy
TypedArray<Vector2i> Terrain3DData::getRegionSlots( void ) const {
	return m_regionRepository.getSlots();
}

TypedArray<Vector2i> Terrain3DData::getRenderRegionSlots( void ) const {
	return m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getRenderRegionSlots() : getRegionSlots();
}

PackedInt32Array Terrain3DData::getRegionMap( void ) const {
	PackedInt32Array region_map;
	if ( m_regionRepository.getRegionMapSize() <= 0 ) {
		return region_map;
	}
	region_map.resize( m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
	int32_t *pWrite = region_map.ptrw();
	const int half = m_regionRepository.getRegionMapSize() / 2;
	const uint16_t *pSourceMap = m_gpuRegionCache.isEnabled() ? m_gpuRegionCache.getRegionMapData()
															  : m_regionRepository.getLogicalRegionMapData();
	for ( int y = 0; y < m_regionRepository.getRegionMapSize(); ++y ) {
		for ( int x = 0; x < m_regionRepository.getRegionMapSize(); ++x ) {
			const Vector2i location( x - half, y - half );
			const int source_index = getRegionMapIndex( location );
			pWrite[y * m_regionRepository.getRegionMapSize() + x] = source_index >= 0 ? pSourceMap[source_index] : 0;
		}
	}
	return region_map;
}

PackedInt32Array Terrain3DData::getLogicalRegionMap( void ) const {
	return m_regionRepository.getLogicalRegionMapPacked();
}

TypedArray<Terrain3DRegion> Terrain3DData::getRegionsActive( const bool bCopy, const bool bDeep ) const {
	TypedArray<Terrain3DRegion> region_arr;
	if ( bCopy && m_cpuRegionCache.isEnabled() ) {
		LOG( ERROR,
				"Whole-repository region copies are unavailable while bounded CPU residency is enabled. "
				"Disable the CPU cache before requesting copied Terrain3DRegion resources." );
		return region_arr;
	}
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i region_loc = m_regionRepository.getSlotsData()[slot];
		Ref<Terrain3DRegion> pRegion = getRegion( region_loc );
		if ( pRegion.is_valid() ) {
			region_arr.push_back( bCopy ? pRegion->duplicate( bDeep ) : pRegion );
		}
	}
	return region_arr;
}

// Calls the callback for every region intersecting the descaled area.
// changeRegionSize() binds the destination Terrain3DRegion as the fourth callback argument.
void Terrain3DData::doForRegions( const Rect2i &rArea, const Callable &rCallback ) {
	const Rect2i location_bounds = t3dDataGetRegionBoundsForArea( rArea, m_regionRepository.getRegionSize() );
	if ( location_bounds.size.x <= 0 || location_bounds.size.y <= 0 ) {
		return;
	}
	LOG( DEBUG, "Processing global area: ", rArea, " -> ", location_bounds );
	Point2i current_region_loc;
	for ( int y = location_bounds.position.y; y < location_bounds.get_end().y; y++ ) {
		current_region_loc.y = y;
		for ( int x = location_bounds.position.x; x < location_bounds.get_end().x; x++ ) {
			current_region_loc.x = x;
			const Terrain3DRegion *pRegion = getRegionPtr( current_region_loc );
			if ( pRegion && !pRegion->isDeleted() ) {
				if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResident( current_region_loc ) ) {
					LOG( ERROR, "Unable to page CPU terrain maps for region callback: ", current_region_loc );
					continue;
				}
				pRegion = getRegionPtr( current_region_loc );
				LOG( DEBUG, "Current region: ", current_region_loc );
				const Rect2i regionArea =
						rArea.intersection( Rect2i( current_region_loc * m_regionRepository.getRegionSize(),
								m_regionRepository.getRegionSizeVector() ) );
				LOG( DEBUG,
						"Region bounds: ",
						Rect2i( current_region_loc * m_regionRepository.getRegionSize(),
								m_regionRepository.getRegionSizeVector() ) );
				LOG( DEBUG, "Region area: ", regionArea );
				const Rect2i dstCoords( regionArea.position - rArea.position, regionArea.size );
				const Rect2i srcCoords(
						regionArea.position - ( pRegion->getLocation() * m_regionRepository.getRegionSizeVector() ),
						dstCoords.size );
				LOG( DEBUG, "src map coords: ", srcCoords );
				LOG( DEBUG, "dst map coords: ", dstCoords );
				rCallback.call( pRegion, srcCoords, dstCoords );
			}
		}
	}
}

void Terrain3DData::changeRegionSize( const int nNewSize ) {
	LOG( INFO, "Changing region size from: ", m_regionRepository.getRegionSize(), " to ", nNewSize );
	if ( !m_pTerrain ) {
		LOG( WARN,
				"Region-size migration requested before Terrain3DData has an initialized Terrain3D owner. "
				"Deferring migration until Terrain3D initialization." );
		return;
	}
	if ( nNewSize != 128 ) {
		LOG( ERROR, "This Terrain3D fork is specialized for 128-sample regions and rejects all other sizes" );
		return;
	}
	if ( !t3dIsValidRegionSize( nNewSize ) ) {
		LOG( ERROR, "Invalid region size: ", nNewSize, ". Must be power of 2, 64-2048" );
		return;
	}
	if ( nNewSize == m_regionRepository.getRegionSize() ) {
		return;
	}

	const TypedArray<Terrain3DRegion> old_regions = getRegionsActive();
	if ( old_regions.is_empty() ) {
		m_pTerrain->setRegionSize( ( t3dRegionSize_t )( nNewSize ) );
		return;
	}

	// Preflight the complete conversion before allocating or deleting anything.
	// The world remains 4096x4096 texels; changing region size changes only the
	// number of logical regions that partition that fixed extent.
	Vector2i worldMin( INT32_MAX, INT32_MAX );
	Vector2i worldMax( INT32_MIN, INT32_MIN );
	for ( const Ref<Terrain3DRegion> pRegion : old_regions ) {
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

	const Rect2i converted_bounds = t3dDataGetRegionBoundsForArea( Rect2i( worldMin, worldMax - worldMin ), nNewSize );
	const Vector2i converted_last = converted_bounds.get_end() - Vector2i( 1, 1 );
	const int target_map_size = s_nActiveWorldTexels / nNewSize;
	if ( !t3dRegionLocationValid( converted_bounds.position, target_map_size ) ||
			!t3dRegionLocationValid( converted_last, target_map_size ) ) {
		LOG( ERROR,
				"Cannot change region size from ",
				m_regionRepository.getRegionSize(),
				" to ",
				nNewSize,
				" without losing data. Converted region coordinates would span ",
				converted_bounds.position,
				" through ",
				converted_last,
				", but valid coordinates are ",
				-target_map_size / 2,
				" through ",
				target_map_size / 2 - 1,
				". Source data was left unchanged." );
		return;
	}

	// Copy each source region directly into its intersecting destination regions.
	// This removes Callable/Variant dispatch and repeated source-region lookups
	// from the hot path, which is particularly costly when splitting down to 64.
	Dictionary new_regions_by_location;
	TypedArray<Terrain3DRegion> new_regions;
	for ( const Ref<Terrain3DRegion> pSrcRegion : old_regions ) {
		if ( pSrcRegion.is_null() ) {
			continue;
		}
		if ( !ensureCpuRegionResident( pSrcRegion->getLocation() ) ) {
			LOG( ERROR, "Unable to page source region during region-size conversion: ", pSrcRegion->getLocation() );
			return;
		}

		const Rect2i srcArea( pSrcRegion->getLocation() * m_regionRepository.getRegionSize(),
				m_regionRepository.getRegionSizeVector() );
		const Rect2i target_bounds = t3dDataGetRegionBoundsForArea( srcArea, nNewSize );
		for ( int y = target_bounds.position.y; y < target_bounds.get_end().y; y++ ) {
			for ( int x = target_bounds.position.x; x < target_bounds.get_end().x; x++ ) {
				const Vector2i targetLocation( x, y );
				Ref<Terrain3DRegion> pDstRegion;
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
	for ( const Ref<Terrain3DRegion> pRegion : old_regions ) {
		removeRegion( pRegion, false );
	}

	// Change region size
	m_pTerrain->setRegionSize( ( t3dRegionSize_t )( nNewSize ) );

	// Add new regions and rebuild
	for ( const Ref<Terrain3DRegion> pRegion : new_regions ) {
		addRegion( pRegion, false );
	}

	calcHeightRange( true );
	updateMaps( REGION_TYPE_MAX, true, true );
	m_pTerrain->getInstancer()->updateMmis( -1, s_v2iMax, true );
}

void Terrain3DData::setRegionModified( const Vector2i &rRegionLoc, const bool bModified ) {
	Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setModified( bModified );
}

bool Terrain3DData::isRegionModified( const Vector2i &rRegionLoc ) const {
	Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return false;
	}
	return pRegion->isModified();
}

void Terrain3DData::setRegionDeleted( const Vector2i &rRegionLoc, const bool bDeleted ) {
	Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return;
	}
	return pRegion->setDeleted( bDeleted );
}

bool Terrain3DData::isRegionDeleted( const Vector2i &rRegionLoc ) const {
	const Terrain3DRegion *pRegion = getRegionPtr( rRegionLoc );
	if ( !pRegion ) {
		LOG( ERROR, "Region not found at: ", rRegionLoc );
		return true;
	}
	return pRegion->isDeleted();
}

Ref<Terrain3DRegion> Terrain3DData::addRegionBlankp( const Vector3 &rGlobalPosition, const bool bUpdate ) {
	return addRegionBlank( getRegionLocation( rGlobalPosition ), bUpdate );
}

Ref<Terrain3DRegion> Terrain3DData::addRegionBlank( const Vector2i &rRegionLoc, const bool bUpdate ) {
	Ref<Terrain3DRegion> pRegion;
	pRegion.instantiate();
	pRegion->setLocation( rRegionLoc );
	pRegion->setRegionSize( m_regionRepository.getRegionSize() );
	pRegion->setVertexSpacing( m_regionRepository.getVertexSpacing() );
	if ( addRegion( pRegion, bUpdate ) == OK ) {
		pRegion->setModified( true );
		return pRegion;
	}
	return Ref<Terrain3DRegion>();
}

/** Adds a Terrain3DRegion to the terrain
 * Marks region as modified
 *	p_update - rebuild the maps if true. Set to false if bulk adding many regions.
 */
Error Terrain3DData::addRegion( const Ref<Terrain3DRegion> &rRegion, const bool bUpdate ) {
	if ( rRegion.is_null() ) {
		return ERR_INVALID_PARAMETER;
	}
	return addRegionInternal( rRegion, bUpdate, true );
}

Error Terrain3DData::replaceRegionData( const Vector2i &rRegionLoc, const Dictionary &dictData ) {
	if ( !ensureCpuRegionResident( rRegionLoc ) ) {
		LOG( ERROR, "Cannot load CPU map payload for replacement region: ", rRegionLoc );
		return ERR_CANT_OPEN;
	}
	Ref<Terrain3DRegion> pCurrent = getRegion( rRegionLoc );
	if ( pCurrent.is_null() || pCurrent->isDeleted() || getRegionId( rRegionLoc ) < 0 ) {
		LOG( ERROR, "Cannot replace data for inactive region: ", rRegionLoc );
		return ERR_DOES_NOT_EXIST;
	}

	// Mutate a detached deep copy first. If validation fails, the currently active
	// resource, render state, collision, and GPU layers remain untouched.
	Ref<Terrain3DRegion> pReplacement = pCurrent->duplicate( true );
	if ( pReplacement.is_null() ) {
		return ERR_CANT_CREATE;
	}
	pReplacement->setData( dictData );
	pReplacement->setLocation( rRegionLoc ); // Repository location is authoritative.
	pReplacement->setDeleted( false );
	pReplacement->setEdited( false );
	pReplacement->setVertexSpacing( m_regionRepository.getVertexSpacing() );
	if ( pReplacement->getRegionSize() != m_regionRepository.getRegionSize() ) {
		LOG( ERROR,
				"Replacement region size ",
				pReplacement->getRegionSize(),
				" does not match terrain region size ",
				m_regionRepository.getRegionSize() );
		return ERR_INVALID_DATA;
	}
	pReplacement->sanitizeMaps();
	if ( !pReplacement->validateMapSize( pReplacement->getHeightMap() ) ||
			( pReplacement->getControlMap().is_valid() &&
					!pReplacement->validateMapSize( pReplacement->getControlMap() ) ) ||
			( pReplacement->getColorMap().is_valid() &&
					!pReplacement->validateMapSize( pReplacement->getColorMap() ) ) ) {
		LOG( ERROR, "Replacement snapshot contains invalid map dimensions" );
		return ERR_INVALID_DATA;
	}
	pReplacement->setModified( true );
	return addRegionInternal( pReplacement, true, false );
}

Error Terrain3DData::addRegionInternal( const Ref<Terrain3DRegion> &rRegion, const bool bUpdate, const bool bSanitize ) {
	if ( rRegion.is_null() ) {
		LOG( ERROR, "Provided region is null. Returning" );
		return FAILED;
	}
	const Vector2i regionLoc = rRegion->getLocation();
	LOG( DEBUG, "Adding region at location ", regionLoc, ", update maps: ", bUpdate ? "yes" : "no" );

	if ( !isRegionLocationValidInternal( regionLoc ) ) {
		LOG( ERROR,
				"Location ",
				regionLoc,
				" out of active bounds. Max: ",
				-m_regionRepository.getRegionMapSize() / 2,
				" to ",
				m_regionRepository.getRegionMapSize() / 2 - 1 );
		return FAILED;
	}

	if ( bSanitize ) {
		rRegion->sanitizeMaps();
	}
	rRegion->setDeleted( false );

	int existingSlot = getRegionId( regionLoc );
	const Ref<Terrain3DRegion> pPriorRegion = m_regionRepository.getRegion( regionLoc );
	Terrain3DRegion *const pStoredRegion = pPriorRegion.ptr();
	const bool bHadPriorRegionResource = pStoredRegion != nullptr;
	if ( existingSlot < 0 && pStoredRegion && !pStoredRegion->isDeleted() ) {
		existingSlot = m_regionRepository.findSlot( regionLoc );
	}
	const bool bReplacingExistingRegion = existingSlot >= 0;
	if ( !bReplacingExistingRegion ) {
		const int maximumRegions = m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize();
		if ( m_regionRepository.getActiveRegionCount() >= maximumRegions ) {
			LOG( ERROR, "Region slot capacity exceeded while adding region ", regionLoc );
			return ERR_OUT_OF_MEMORY;
		}
	}

	if ( !syncActiveRegion( regionLoc, rRegion.ptr() ) ) {
		if ( pStoredRegion && !pStoredRegion->isDeleted() ) {
			syncActiveRegion( regionLoc, pStoredRegion );
		}
		LOG( ERROR, "Unable to publish region ", regionLoc, " into process-static active storage" );
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
		LOG( ERROR, "Region repository rejected region publication at ", regionLoc );
		return ERR_INVALID_DATA;
	}

	// Replacing an active resource does not alter topology or texture-array layout.
	if ( bReplacingExistingRegion && bUpdate && canUpdateRegionSlotsIncrementally() ) {
		ensureDefaultMaps();
		m_heightMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_HEIGHT );
		m_controlMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_CONTROL );
		m_colorMaps[existingSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_COLOR );
		Ref<Image> pDerived = rRegion->getDerivedNormalSlopeMap();
		if ( pDerived.is_null() ) {
			regenerateDerivedMaps( { regionLoc } );
			pDerived = rRegion->getDerivedNormalSlopeMap();
		}
		if ( pDerived.is_null() ) {
			pDerived = m_defaultDerivedMap;
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
				( void )m_regionRepository.storeRegion( regionLoc, pPriorRegion );
				if ( !pPriorRegion->isDeleted() ) {
					syncActiveRegion( regionLoc, pPriorRegion.ptr() );
				}
			} else {
				m_regionRepository.eraseRegionResource( regionLoc );
				if ( m_samplingStorage.isActiveOwner() ) {
					m_samplingStorage.invalidateActive( regionLoc );
				}
			}
			LOG( ERROR, "Region slot capacity exceeded while adding region ", regionLoc );
			return ERR_OUT_OF_MEMORY;
		}

		const bool bCanUploadSingleLayer = bUpdate && canUpdateRegionSlotsIncrementally();
		if ( bCanUploadSingleLayer ) {
			ensureDefaultMaps();
			m_heightMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_HEIGHT );
			m_controlMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_CONTROL );
			m_colorMaps[newSlot] = getRegionMapOrDefault( rRegion.ptr(), TYPE_COLOR );
			regenerateDerivedMaps( { regionLoc } );
			Ref<Image> pDerived = rRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() ) {
				pDerived = m_defaultDerivedMap;
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
				LOG( ERROR, "Region repository invariant failure after incremental add" );
			}
#endif
			return OK;
		}
		m_regionRepository.markRegionMapDirty();
	}

	LOG( DEBUG,
			"Storing region ",
			regionLoc,
			" version ",
			vformat( "%.3f", rRegion->getVersion() ),
			" active count: ",
			m_regionRepository.getActiveRegionCount(),
			", slots: ",
			m_regionRepository.getSlotCount() );
	if ( bUpdate ) {
		updateMaps( REGION_TYPE_MAX, true, false );
		if ( bRequiresInstanceReconciliation ) {
			m_pTerrain->getInstancer()->updateMmis( -1, regionLoc, false );
		}
	}
	invalidateSamplingCache();
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after addRegionInternal" );
	}
#endif
	return OK;
}

void Terrain3DData::removeRegionp( const Vector3 &rGlobalPosition, const bool bUpdate ) {
	const Ref<Terrain3DRegion> pRegion = getRegion( getRegionLocation( rGlobalPosition ) );
	removeRegion( pRegion, bUpdate );
}

void Terrain3DData::removeRegionl( const Vector2i &rRegionLoc, const bool bUpdate ) {
	const Ref<Terrain3DRegion> pRegion = getRegion( rRegionLoc );
	removeRegion( pRegion, bUpdate );
}

// Remove region marks the region for deletion, and removes it from the active arrays indexed by ID
// It remains stored in _regions and the file remains on disk until saved, when both are removed
void Terrain3DData::removeRegion( const Ref<Terrain3DRegion> &rRegion, const bool bUpdate ) {
	if ( rRegion.is_null() ) {
		LOG( ERROR, "Region not found or is null. Returning" );
		return;
	}

	const Vector2i regionLoc = rRegion->getLocation();
	const int regionSlot = m_regionRepository.findSlot( regionLoc );
	LOG( INFO, "Marking region ", regionLoc, " for deletion. update maps: ", bUpdate ? "yes" : "no" );
	if ( regionSlot < 0 ) {
		LOG( ERROR, "Region ", regionLoc, " not found in active region slots. Returning" );
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
		LOG( ERROR, "Region repository failed to deactivate ", regionLoc );
		return;
	}
	markGpuFarRegionDirty( regionLoc, REGION_TYPE_MAX );
	calcHeightRange();

	invalidateSamplingCache();
	if ( bUpdate ) {
		emitRegionLayoutChanged( false );
		m_pTerrain->getInstancer()->destroyRegionMmis( regionLoc );
	}
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after removeRegion" );
	}
#endif
}

bool Terrain3DData::ensurePersistenceRegionResident( void *const pContext, const Vector2i &rLocation ) {
	Terrain3DData *const pData = static_cast<Terrain3DData *>( pContext );
	return pData && pData->ensureCpuRegionResidentInternal( rLocation, true );
}

void Terrain3DData::processRegionSaveResult( const t3dRegionSaveResult_t &rResult ) {
	if ( rResult.m_bDeleted && !m_runtimePageStore.removePageForSource( rResult.m_path ) ) {
		LOG( WARN, "Region deleted, but cooked runtime page could not be removed: ", rResult.m_path );
	}
	if ( !rResult.m_bSourceChanged || !m_pTerrain || !m_pTerrain->isGpuRegionCookOnSave() ) {
		return;
	}

	// The compiled render-control field uses a two-texel cross-region halo. A
	// source edit or deletion can change immediate neighbors even when their
	// authored resources did not change, so refresh the whole 3x3 source area.
	for ( int dz = -1; dz <= 1; ++dz ) {
		for ( int dx = -1; dx <= 1; ++dx ) {
			if ( rResult.m_bDeleted && dx == 0 && dz == 0 ) {
				continue;
			}
			const Vector2i cookLocation = rResult.m_location + Vector2i( dx, dz );
			Terrain3DRegion *const pCookRegion = getRegionPtr( cookLocation );
			if ( !pCookRegion || pCookRegion->isDeleted() ) {
				continue;
			}
			if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResidentInternal( cookLocation, true ) ) {
				LOG( ERROR,
						rResult.m_bDeleted ? "Region deleted, but neighboring runtime page source could not be loaded: "
										   : "Region saved, but neighboring runtime page source could not be loaded: ",
						cookLocation );
				continue;
			}
			if ( !writeGpuRuntimePage( pCookRegion ) ) {
				LOG( ERROR,
						rResult.m_bDeleted ? "Region deleted, but neighboring runtime GPU page cooking failed: "
										   : "Region saved, but runtime GPU page cooking failed: ",
						cookLocation );
			}
		}
	}
}

void Terrain3DData::saveDirectory( const String &rDir ) {
	LOG( INFO, "Saving data files to ", rDir );
	const bool cook_on_save = m_pTerrain && m_pTerrain->isGpuRegionCookOnSave();
	if ( cook_on_save && !m_runtimeWorldManifest.buildAuthoringIndex( m_regionRepository ) ) {
		LOG( ERROR, "Failed to build runtime-world dependency index before save-time cooking" );
	}
	const t3dDirectorySaveResult_t saveResult = m_regionPersistence.saveDirectory(
			rDir, m_pTerrain->getSave16Bit(), &Terrain3DData::ensurePersistenceRegionResident, this );
	for ( const t3dRegionSaveResult_t &regionResult : saveResult.m_regions ) {
		processRegionSaveResult( regionResult );
	}
	if ( cook_on_save && !writeCookedGpuFarAtlases( rDir ) ) {
		LOG( ERROR, "Terrain regions saved, but cooked far-atlas generation failed" );
	}
	if ( cook_on_save ) {
		PackedStringArray manifest_errors;
		if ( !m_runtimeWorldManifest.buildAuthoringIndex( m_regionRepository ) ||
				!m_runtimeWorldManifest.write(
						rDir, m_regionRepository, m_runtimePageStore, getRuntimePageConfig(), &manifest_errors ) ) {
			LOG( ERROR, "Terrain regions saved, but runtime-world manifest publication failed: ", manifest_errors );
		}
	}
	// Persistence updates only the exact resource paths Terrain3D changed. Do not
	// force a project-wide EditorFileSystem scan from a scene-save callback.
}

void Terrain3DData::saveRegion( const Vector2i &rRegionLoc, const String &rDir, const bool b16Bit ) {
	const bool cook_on_save = m_pTerrain && m_pTerrain->isGpuRegionCookOnSave();
	if ( cook_on_save ) {
		m_runtimeWorldManifest.buildAuthoringIndex( m_regionRepository );
	}
	const t3dRegionSaveResult_t result =
			m_regionPersistence.saveRegion( rRegionLoc, rDir, b16Bit, &Terrain3DData::ensurePersistenceRegionResident, this );
	processRegionSaveResult( result );
	if ( cook_on_save ) {
		PackedStringArray manifest_errors;
		if ( !m_runtimeWorldManifest.buildAuthoringIndex( m_regionRepository ) ||
				!m_runtimeWorldManifest.write(
						rDir, m_regionRepository, m_runtimePageStore, getRuntimePageConfig(), &manifest_errors ) ) {
			LOG( ERROR, "Region saved, but runtime-world manifest publication failed: ", manifest_errors );
		}
	}
}

bool Terrain3DData::collectGpuRegionPageSources( String &rDataDirectory,
		PackedStringArray &rSourceFiles,
		eastl::vector<Vector2i> &rLocations,
		PackedStringArray &rErrors ) const {
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

Dictionary Terrain3DData::inspectGpuRegionPages( void ) {
	Dictionary result;
	PackedStringArray source_files;
	PackedStringArray errors;
	eastl::vector<Vector2i> locations;
	String data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int discovered = source_files.size();

	result["data_directory"] = data_dir;
	result["discovered"] = discovered;
	result["valid"] = ( int )( locations.size() );
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
		Terrain3DRegion *const pRegion = getRegionPtr( location );
		const String source_path = data_dir.path_join( Terrain3DUtil::locationToFilename( location ) );
		const String page_path = getGpuRuntimePagePath( pRegion );
		if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResidentInternal( location, true ) ) {
			++source_unavailable;
			needs_cook_regions.push_back( String( location ) );
			errors.push_back( String( "Region " ) + String( location ) + String( ": unable to materialize source maps" ) );
			continue;
		}

		const uint64_t expected_signature = computeGpuRuntimePageSourceSignature( pRegion );
		if ( expected_signature == 0u ) {
			++source_unavailable;
			needs_cook_regions.push_back( String( location ) );
			errors.push_back( String( "Region " ) + String( location ) + String( ": source signature generation failed" ) );
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

	PackedStringArray page_files = Terrain3DUtil::getFiles( data_dir, "terrain3d*.t3dpage" );
	page_files.sort();
	for ( const String &fname : page_files ) {
		const String source_name = fname.get_basename() + String( ".res" );
		const String source_path = data_dir.path_join( source_name );
		if ( !FileAccess::file_exists( source_path ) ) {
			orphan_pages.push_back( data_dir.path_join( fname ) );
		}
	}

	m_cpuRegionCache.releaseUntrackedMaps();
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

Dictionary Terrain3DData::cookGpuRegionPages( void ) {
	Dictionary result;
	PackedStringArray source_files;
	PackedStringArray errors;
	eastl::vector<Vector2i> locations;
	String data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int discovered = source_files.size();
	const int valid = ( int )( locations.size() );
	if ( !preflight_ok ) {
		m_cpuRegionCache.releaseUntrackedMaps();
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

	if ( !m_runtimeWorldManifest.buildAuthoringIndex( m_regionRepository ) ) {
		errors.push_back( "World manifest dependency index could not be built before page cooking" );
		m_cpuRegionCache.releaseUntrackedMaps();
		result["complete"] = false;
		result["errors"] = errors;
		return result;
	}

	const t3dRuntimePageConfig_t runtime_page_config = getRuntimePageConfig();
	Terrain3DCookDatabase cook_database;
	const bool bCookDatabaseLoaded = cook_database.load(
			data_dir, m_regionRepository.getRegionSize(), runtime_page_config.m_nMaterialSourceSignature );

	int cooked = 0;
	int unchanged = 0;
	int failed = 0;
	int orphan_removed = 0;
	int orphan_failed = 0;
	for ( const Vector2i location : locations ) {
		Terrain3DRegion *const pRegion = getRegionPtr( location );
		if ( m_cpuRegionCache.isEnabled() && !ensureCpuRegionResidentInternal( location, true ) ) {
			++failed;
			errors.push_back( String( "Region " ) + String( location ) + String( ": unable to materialize source maps" ) );
			continue;
		}

		const uint64_t expected_signature = computeGpuRuntimePageSourceSignature( pRegion );
		if ( expected_signature == 0u ) {
			++failed;
			errors.push_back( String( "Region " ) + String( location ) + String( ": source signature generation failed" ) );
			continue;
		}
		const String page_path = getGpuRuntimePagePath( pRegion );
		bool bCurrent = bCookDatabaseLoaded && FileAccess::file_exists( page_path ) &&
				cook_database.hasCurrentRegion( location, expected_signature );
		if ( !bCurrent ) {
			uint64_t existing_signature = 0u;
			bCurrent = readGpuRuntimePageSourceSignature( page_path, existing_signature ) &&
					existing_signature == expected_signature;
			if ( bCurrent ) {
				bCurrent = m_runtimePageStore.validatePageForRegion( pRegion, runtime_page_config );
			}
		}
		if ( bCurrent ) {
			cook_database.setRegion( location, expected_signature );
			++unchanged;
			continue;
		}
		String cook_error;
		if ( writeGpuRuntimePage( pRegion, &cook_error ) ) {
			cook_database.setRegion( location, expected_signature );
			++cooked;
		} else {
			cook_database.clearRegion( location );
			++failed;
			errors.push_back( String( "Region " ) + String( location ) + String( ": " ) +
					( cook_error.is_empty() ? String( "cook failed" ) : cook_error ) );
		}
	}

	PackedStringArray page_files = Terrain3DUtil::getFiles( data_dir, "terrain3d*.t3dpage" );
	for ( const String &fname : page_files ) {
		const Vector2i page_location = Terrain3DUtil::filenameToLocation( fname.get_basename() + String( ".res" ) );
		Terrain3DRegion *const pPageRegion = getRegionPtr( page_location );
		if ( pPageRegion && !pPageRegion->isDeleted() ) {
			continue;
		}
		const String orphan_path = data_dir.path_join( fname );
		if ( m_runtimePageStore.removePage( orphan_path ) ) {
			cook_database.clearRegion( page_location );
			++orphan_removed;
		} else {
			++orphan_failed;
			errors.push_back( String( "Unable to remove orphan runtime page: " ) + orphan_path );
		}
	}

	const bool bFarAtlasCooked = writeCookedGpuFarAtlases();
	if ( !bFarAtlasCooked ) {
		errors.push_back( "Far atlas cook failed" );
	}
	Dictionary material_cache_result;
	bool bMaterialCacheCooked = true;
	if ( m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled() ) {
		const Ref<Terrain3DAssets> pAssets = m_pTerrain->getAssets();
		if ( pAssets.is_valid() ) {
			material_cache_result = pAssets->cookRuntimeMaterialCache();
			bMaterialCacheCooked = ( bool )( material_cache_result.get( "complete", false ) );
			if ( !bMaterialCacheCooked ) {
				errors.push_back( String( "Runtime material cache cook failed: " ) +
						String( material_cache_result.get( "error", "unknown error" ) ) );
			}
		}
	}
	Dictionary shader_pack_result;
	bool bShaderPackCooked = true;
	if ( m_pTerrain ) {
		const Ref<Terrain3DMaterial> material = m_pTerrain->getMaterial();
		if ( material.is_valid() && !material->isShaderOverrideEnabled() ) {
			shader_pack_result = material->cookRuntimeShaderPack();
			bShaderPackCooked = ( bool )shader_pack_result.get( "complete", false );
			if ( !bShaderPackCooked ) {
				errors.push_back( String( "Runtime shader pack cook failed: " ) +
						String( shader_pack_result.get( "error", "unknown error" ) ) );
			}
		}
	}
	const bool bClipmapPackCooked = m_pTerrain && m_pTerrain->cookRuntimeClipmapGeometryPack();
	if ( !bClipmapPackCooked ) {
		errors.push_back( "Runtime clipmap geometry pack cook failed" );
	}
	const bool bWorldManifestCooked = failed == 0 &&
			m_runtimeWorldManifest.write(
					data_dir, m_regionRepository, m_runtimePageStore, runtime_page_config, &errors );
	if ( !bWorldManifestCooked ) {
		errors.push_back( "Cooked world manifest generation failed" );
	}
	PackedStringArray strip_safe_sources;
	auto append_strip_source = [&]( const String &rPath ) {
		if ( rPath.is_empty() ) {
			return;
		}
		for ( const String &existing : strip_safe_sources ) {
			if ( existing == rPath ) {
				return;
			}
		}
		strip_safe_sources.push_back( rPath );
	};
	PackedStringArray region_strip_sources;
	for ( const String &source_file : source_files ) {
		const String path = data_dir.path_join( source_file );
		region_strip_sources.push_back( path );
		append_strip_source( path );
	}
	PackedStringArray material_strip_sources;
	if ( bMaterialCacheCooked && m_pTerrain && m_pTerrain->isRuntimeMaterialStreamingEnabled() ) {
		const Ref<Terrain3DAssets> pAssets = m_pTerrain->getAssets();
		if ( pAssets.is_valid() ) {
			const TypedArray<Terrain3DTextureAsset> textures = pAssets->getTextureList();
			for ( int index = 0; index < textures.size(); ++index ) {
				const Ref<Terrain3DTextureAsset> asset = textures[index];
				if ( asset.is_null() ) {
					continue;
				}
				const Ref<Texture2D> source_textures[] = { asset->getAlbedoTexture(),
					asset->getNormalTexture(),
					asset->getSurfaceTexture(),
					asset->getDisplacementTexture(),
					asset->getPbrTexture() };
				for ( const Ref<Texture2D> &texture : source_textures ) {
					if ( texture.is_null() || texture->get_path().is_empty() ) {
						continue;
					}
					bool duplicate = false;
					for ( const String &existing : material_strip_sources ) {
						if ( existing == texture->get_path() ) {
							duplicate = true;
							break;
						}
					}
					if ( !duplicate ) {
						material_strip_sources.push_back( texture->get_path() );
					}
					append_strip_source( texture->get_path() );
				}
			}
		}
	}
	const String strip_manifest_path = data_dir.path_join( "terrain3d_runtime_sources.txt" );
	if ( bWorldManifestCooked ) {
		Ref<FileAccess> pStripFile = FileAccess::open( strip_manifest_path, FileAccess::WRITE );
		if ( pStripFile.is_valid() ) {
			pStripFile->store_line( "# Terrain3D cooked-runtime source-strip manifest" );
			pStripFile->store_line( "# Runtime authority: terrain3d_world.t3dmanifest + .t3dpage region artifacts" );
			pStripFile->store_line( "[region_resources]" );
			for ( const String &path : region_strip_sources ) {
				pStripFile->store_line( path );
			}
			if ( !material_strip_sources.is_empty() ) {
				pStripFile->store_line( "[material_source_textures]" );
				for ( const String &path : material_strip_sources ) {
					pStripFile->store_line( path );
				}
			}
			pStripFile->store_line( "[runtime_artifact_authority]" );
			pStripFile->store_line( "shader_source=terrain3d_shaders.t3dshaderpack" );
			pStripFile->store_line( "clipmap_geometry=terrain3d_clipmap.t3dmeshpack" );
			pStripFile->store_line( "far_surface=terrain3d_far.t3dfar" );
			if ( bMaterialCacheCooked ) {
				pStripFile->store_line( "material_images=terrain3d_materials.t3dmatcache" );
			}
		}
	}
	const bool bCookDatabaseWritten = failed == 0 && orphan_failed == 0 && bFarAtlasCooked && bMaterialCacheCooked &&
			bShaderPackCooked && bClipmapPackCooked && bWorldManifestCooked &&
			cook_database.write(
					data_dir, m_regionRepository.getRegionSize(), runtime_page_config.m_nMaterialSourceSignature );
	if ( !bCookDatabaseWritten ) {
		errors.push_back(
				"Persistent cook database publish failed or was withheld because another cooked artifact failed" );
	}
	m_cpuRegionCache.releaseUntrackedMaps();
	const int usable = cooked + unchanged;
	result["data_directory"] = data_dir;
	result["far_atlas_cooked"] = bFarAtlasCooked;
	result["material_cache"] = material_cache_result;
	result["material_cache_cooked"] = bMaterialCacheCooked;
	result["shader_pack"] = shader_pack_result;
	result["shader_pack_cooked"] = bShaderPackCooked;
	result["clipmap_pack_cooked"] = bClipmapPackCooked;
	result["cook_database_loaded"] = bCookDatabaseLoaded;
	result["cook_database_written"] = bCookDatabaseWritten;
	result["cook_database_path"] = cook_database.getPath( data_dir );
	result["world_manifest_cooked"] = bWorldManifestCooked;
	result["world_manifest_path"] = m_runtimeWorldManifest.getPath( data_dir );
	result["strip_safe_sources"] = strip_safe_sources;
	result["strip_manifest_path"] = strip_manifest_path;
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
	result["complete"] = failed == 0 && usable == valid && valid == discovered && orphan_failed == 0 &&
			bFarAtlasCooked && bMaterialCacheCooked && bShaderPackCooked && bClipmapPackCooked &&
			bWorldManifestCooked && bCookDatabaseWritten;
	result["errors"] = errors;
	return result;
}

Error Terrain3DData::loadDirectory( const String &rDir ) {
	t3dStagedRepository_t staged;
	const bool bBoundedCpuLoad = m_cpuRegionCache.isEnabled();
	LOG( INFO,
			"[terrain-data] Candidate repository load; CPU cache ",
			bBoundedCpuLoad ? "bounded" : "authoring/unbounded",
			"; GPU cache ",
			m_gpuRegionCache.isEnabled() ? "enabled" : "disabled" );
	const t3dRuntimePageConfig_t runtime_page_config = getRuntimePageConfig();
	m_cpuRegionCache.setRuntimePageConfig( runtime_page_config );
	const bool bCookedRuntimeManifest =
			bBoundedCpuLoad && runtime_page_config.m_bCookedPagesEnabled && !runtime_page_config.m_bEditorAuthoring;
	Error stageError = OK;
	if ( bCookedRuntimeManifest ) {
		PackedStringArray manifest_errors;
		if ( !m_runtimeWorldManifest.load( rDir, &manifest_errors ) ||
				!m_runtimeWorldManifest.stageRepository( rDir, staged ) ) {
			for ( const String &error : manifest_errors ) {
				LOG( ERROR, error );
			}
			LOG( ERROR, "Cooked runtime profile requires a valid terrain3d_world.t3dmanifest." );
			return ERR_FILE_CORRUPT;
		}
		LOG( INFO,
				"Loaded cooked world manifest with ",
				staged.getRegionCount(),
				" regions; authored Terrain3DRegion resources were not deserialized." );
	} else {
		m_runtimeWorldManifest.reset();
		stageError = m_regionPersistence.stageDirectory( rDir, bBoundedCpuLoad, staged );
	}
	if ( stageError != OK ) {
		return stageError;
	}

	LOG( DEBUG, "[terrain-data] Phase 3/4: publishing active storage and region ownership" );
	m_regionPersistence.beginPublication();

	// Publish the complete validated repository in one state transition. Generated
	// textures and cache mirrors are rebuilt from this complete state afterward.
	m_samplingStorage.resetResidentPool();
	const int previousRegionSize = m_regionRepository.getRegionSize();
	bool activeStorageReady = false;
	if ( bBoundedCpuLoad ) {
		const uint8_t *pStagedHeightPages[s_nMaxRegionSlots] = {};
		int stagedHeightSlots[s_nMaxRegionSlots] = {};
		size_t stagedHeightCount = 0u;
		if ( !bCookedRuntimeManifest ) {
			for ( const t3dStagedRegion_t &entry : staged.m_regions ) {
				if ( entry.m_nActiveSlot < 0 || entry.m_stagedHeight.is_empty() ) {
					LOG( ERROR, "Missing staged height payload for region ", entry.m_location );
					m_regionPersistence.finishPublication( 0 );
					m_regionPersistence.finishDirectoryLoad( false );
					return ERR_FILE_CORRUPT;
				}
				pStagedHeightPages[stagedHeightCount] = entry.m_stagedHeight.ptr();
				stagedHeightSlots[stagedHeightCount] = entry.m_nActiveSlot;
				++stagedHeightCount;
			}
		}
		activeStorageReady = m_samplingStorage.replaceActiveHeightPages(
				staged.m_nTargetRegionSize, pStagedHeightPages, stagedHeightSlots, stagedHeightCount );
		if ( activeStorageReady ) {
			activeStorageReady = setRegionSizeInternal( staged.m_nTargetRegionSize );
		}
	} else {
		if ( !setRegionSizeInternal( staged.m_nTargetRegionSize ) ) {
			m_regionPersistence.finishPublication( 0 );
			m_regionPersistence.finishDirectoryLoad( false );
			return ERR_ALREADY_IN_USE;
		}
		// Same-size repository replacement does not pass through the reconfigure
		// clear path. Reset old slots before publishing the staged candidate.
		m_samplingStorage.resetActive();
		activeStorageReady = true;
		for ( const t3dStagedRegion_t &entry : staged.m_regions ) {
			if ( !syncActiveRegion( entry.m_location, entry.m_region.ptr() ) ) {
				activeStorageReady = false;
				break;
			}
		}
	}
	if ( !activeStorageReady ) {
		if ( !bBoundedCpuLoad ) {
			setRegionSizeInternal( previousRegionSize );
			Terrain3DRegion *const *const ppRegions = m_regionRepository.getRegionPtrMapData();
			for ( int i = 0; i < s_nMaxRegionSlots; ++i ) {
				Terrain3DRegion *const pRegion = ppRegions[i];
				if ( pRegion && !pRegion->isDeleted() ) {
					syncActiveRegion( pRegion->getLocation(), pRegion );
				}
			}
		}
		m_regionPersistence.finishPublication( 0 );
		m_regionPersistence.finishDirectoryLoad( false );
		LOG( ERROR, "Failed to populate process-static active terrain storage; previous repository restored" );
		return ERR_INVALID_DATA;
	}

	m_regionRepository.replaceAll( staged.m_resources,
			staged.m_pRegionPtrs.data(),
			staged.m_regionLocations.data(),
			staged.getRegionCount(),
			staged.m_logicalRegionMap.data() );
#ifdef DEBUG_ENABLED
	if ( !m_regionRepository.validateInvariants() ) {
		LOG( ERROR, "Region repository invariant failure after loadDirectory publication" );
	}
#endif
	m_heightMaps.clear();
	m_controlMaps.clear();
	m_colorMaps.clear();
	m_derivedMaps.clear();
	m_defaultHeightMap.unref();
	m_defaultControlMap.unref();
	m_defaultColorMap.unref();
	m_defaultDerivedMap.unref();
	m_bControlMapsImplicit = true;
	m_bColorMapsImplicit = true;
	m_mapEditService.reset();
	m_generatedHeightMaps.clear();
	m_generatedControlMaps.clear();
	m_generatedColorMaps.clear();
	m_generatedDerivedMaps.clear();

	// Path takeover happens only after the repository owns every staged resource.
	// Validation therefore cannot steal paths from the active repository.
	if ( !bCookedRuntimeManifest ) {
		m_regionPersistence.publishStagedPaths( staged );
	}
	m_regionPersistence.finishPublication( m_regionRepository.getActiveRegionCount() );
	LOG( DEBUG,
			"[terrain-data] Phase 3/4 complete: ",
			m_regionRepository.getActiveRegionCount(),
			" active regions published; rebuilding residency and renderer resources" );

	LOG( DEBUG, "[terrain-data] Phase 4/4: cache policy, GPU textures, and sampling state" );
	m_regionPersistence.beginFinalize();
	m_pTerrain->setRegionSize( ( t3dRegionSize_t )( staged.m_nTargetRegionSize ) );
	configureResidentPool( 64 );
	configureCpuRegionCache( m_pTerrain->isCpuRegionCacheEffective(),
			m_pTerrain->getEffectiveCpuRegionCacheBudgetMb(),
			m_pTerrain->getCpuRegionLoadBudgetLayers(),
			m_pTerrain->getCpuRegionLoadBudgetMb() );
	configureInstanceRegionCache( m_pTerrain->isInstanceRegionCacheEffective(),
			m_pTerrain->getEffectiveInstanceRegionCacheBudgetMb(),
			m_pTerrain->getInstanceRegionLoadBudgetPages(),
			m_pTerrain->getInstanceRegionLoadBudgetMb() );
	if ( m_cpuRegionCache.isEnabled() ) {
		updateCpuRegionResidency( Vector3(), true );
	}
	updateMaps( REGION_TYPE_MAX, true, false );
	invalidateSamplingCache();
	LOG( DEBUG, "[terrain-data] Phase 4/4 complete: terrain repository is ready for consumers" );
	if ( m_pTerrain->isInitialized() && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, s_v2iMax, true );
	}
	m_regionPersistence.finishDirectoryLoad( true );
	const t3dStartupLoadStats_t &loadStats = m_regionPersistence.getStartupLoadStats();
	const double elapsedMs = ( double )( loadStats.m_nTotalUsecLast ) / 1000.0;
	LOG( INFO,
			"Loaded ",
			m_regionRepository.getActiveRegionCount(),
			" terrain regions (",
			m_regionRepository.getSlotCount(),
			" texture layers) in ",
			vformat( "%.2f", elapsedMs ),
			" ms; synchronous Resource deserialize ",
			vformat( "%.2f", ( double )( loadStats.m_nDeserializeUsecLast ) / 1000.0 ),
			" ms",
			bCookedRuntimeManifest ? "; cooked manifest startup" : "" );
	return OK;
}

// Loads a region from an explicit directory through the persistence layer.
void Terrain3DData::loadRegion( const Vector2i &rRegionLoc, const String &rDir, const bool bUpdate ) {
	const t3dRegionLoadResult_t result = m_regionPersistence.loadRegion( rRegionLoc, rDir );
	if ( !result.succeeded() ) {
		return;
	}
	const Ref<Terrain3DRegion> pRegion = result.m_region;
	if ( m_regionRepository.isEmpty() ) {
		m_pTerrain->setRegionSize( ( t3dRegionSize_t )( pRegion->getRegionSize() ) );
	} else if ( m_pTerrain->getRegionSize() != ( t3dRegionSize_t )( pRegion->getRegionSize() ) ) {
		LOG( ERROR,
				"Region size mismatch. First loaded: ",
				m_pTerrain->getRegionSize(),
				" next: ",
				pRegion->getRegionSize(),
				" in file: ",
				result.m_path );
		return;
	}

	if ( addRegionInternal( pRegion, bUpdate, true ) == OK ) {
		m_regionPersistence.publishLoadedRegionPath( result );
		pRegion->setModified( result.m_bStorageUpgrade || pRegion->isModified() );
		pRegion->setEdited( false );
		m_cpuRegionCache.markSelectionDirty();
	}
}

TypedArray<Image> Terrain3DData::getMaps( const t3dMapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Specified map type out of range" );
		return TypedArray<Image>();
	}
	switch ( eMapType ) {
		case TYPE_HEIGHT:
			return getHeightMaps();
			break;
		case TYPE_CONTROL:
			return getControlMaps();
			break;
		case TYPE_COLOR:
			return getColorMaps();
			break;
		default:
			break;
	}
	return TypedArray<Image>();
}

void Terrain3DData::updateMaps( const t3dMapType_t eMapType, const bool bAllRegions, const bool bGenerateMipmaps ) {
	( void )bGenerateMipmaps; // GPU publication builds transient mip chains; CPU maps stay base-level only.
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
	const bool bSlotCapacityChanged =
			bAllRegions && !bSkipReserve && m_regionRepository.ensureSlotReserve( getRegionSlotReserve() );
	if ( bSkipReserve ) {
		m_bSkipSlotReserveOnce = false;
	}
	if ( bAllRegions ) {
		LOG( EXTREME, "Marking dirty maps of type: ", eMapType );
		if ( bSlotCapacityChanged || eMapType == REGION_TYPE_MAX ) {
			m_generatedHeightMaps.clear();
			m_generatedControlMaps.clear();
			m_generatedColorMaps.clear();
			m_generatedDerivedMaps.clear();
		} else {
			switch ( eMapType ) {
				case TYPE_HEIGHT:
					m_generatedHeightMaps.clear();
					m_generatedDerivedMaps.clear();
					break;
				case TYPE_CONTROL:
					m_generatedControlMaps.clear();
					break;
				case TYPE_COLOR:
					m_generatedColorMaps.clear();
					break;
				default:
					break;
			}
		}
	}

	bool bAnyChanged = bImplicitStateChanged;

	if ( m_regionRepository.isRegionMapDirty() ) {
		LOG( EXTREME,
				"Regenerating ",
				V2I( m_regionRepository.getRegionMapSize() ),
				" active region map from fixed slot storage" );
		m_regionRepository.rebuildRegionMapFromSlots();
		bAnyChanged = true;
		if ( !m_bStartupBulkLoad ) {
			LOG( DEBUG, "Emitting region_map_changed" );
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
			missing.reserve( ( size_t )( m_regionRepository.getActiveRegionCount() ) );
			for ( int slot = 0; slot < slot_count; ++slot ) {
				const Vector2i location = m_regionRepository.getSlotsData()[slot];
				Terrain3DRegion *const pRegion = location == s_v2iMax ? nullptr : getRegionPtr( location );
				if ( pRegion && !pRegion->isDeleted() && !pRegion->hasValidDerivedNormalSlopeMap() ) {
					missing.push_back( location );
				}
			}
			regenerateDerivedMaps( missing );
		}

		for ( int slot = 0; slot < slot_count; ++slot ) {
			const Vector2i location = m_regionRepository.getSlotsData()[slot];
			const Terrain3DRegion *pRegion = location == s_v2iMax ? nullptr : getRegionPtr( location );
			const bool bValid = pRegion && !pRegion->isDeleted();
			if ( bRebuildHeight ) {
				m_heightMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, TYPE_HEIGHT );
				if ( bValid ) {
					updateMasterHeights( pRegion->getHeightRange() );
				}
			}
			if ( bRebuildControl ) {
				m_controlMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, TYPE_CONTROL );
				gpu_control_maps[slot] = bValid ? makeGpuRenderControlImage( location ) : m_defaultControlMap;
			}
			if ( bRebuildColor ) {
				m_colorMaps[slot] = getRegionMapOrDefault( bValid ? pRegion : nullptr, TYPE_COLOR );
				gpu_color_maps[slot] = makeGpuColorImage( m_colorMaps[slot] );
			}
			if ( bRebuildDerived ) {
				const Ref<Image> pDerived = bValid ? pRegion->getDerivedNormalSlopeMap() : Ref<Image>();
				m_derivedMaps[slot] = pDerived.is_valid() ? pDerived : m_defaultDerivedMap;
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
				const TypedArray<Image> implicit_control = { m_defaultControlMap };
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
				const TypedArray<Image> implicit_color = { makeGpuColorImage( m_defaultColorMap ) };
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
					Terrain3DRegion *const pRegion = location == s_v2iMax ? nullptr : getRegionPtr( location );
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
			if ( region_loc == s_v2iMax ) {
				continue;
			}
			const Terrain3DRegion *pRegion = getRegionPtr( region_loc );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->isEdited() ) {
				continue;
			}
			switch ( eMapType ) {
				case TYPE_HEIGHT:
					m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), region_id );
					bHeightChanged = true;
					break;
				case TYPE_CONTROL:
					m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
					bControlChanged = true;
					break;
				case TYPE_COLOR:
					m_generatedColorMaps.update(
							makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ), region_id );
					bColorChanged = true;
					break;
				default:
					m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), region_id );
					m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), region_id );
					m_generatedColorMaps.update(
							makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ), region_id );
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
		LOG( DEBUG, "Emitting maps_changed" );
		emit_signal( "maps_changed" );
		m_pTerrain->snap();
	}
}

void Terrain3DData::updateMapRegions( const t3dMapType_t eMapType, const eastl::vector<Vector2i> &rRegionLocations ) {
	if ( rRegionLocations.empty() ) {
		return;
	}

	if ( refreshImplicitMapState() ) {
		updateMaps( eMapType, true, false );
		return;
	}

	eastl::vector<Vector2i> render_locations = rRegionLocations;
	if ( eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX ) {
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
					Terrain3DRegion *const pDependencyRegion = getRegionPtr( dependency_location );
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
		Terrain3DRegion *const pRegion = getRegionPtr( region_loc );
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
			bHeightChanged = bHeightChanged || eMapType == TYPE_HEIGHT || eMapType == REGION_TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == TYPE_CONTROL || eMapType == REGION_TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == TYPE_COLOR || eMapType == REGION_TYPE_MAX;
			bPhysicalMappingChanged =
					m_gpuRegionCache.republish( region_loc, getComputeBackendIfCreated() ) || bPhysicalMappingChanged;
			continue;
		}

		switch ( eMapType ) {
			case TYPE_HEIGHT:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), repository_slot );
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
			case TYPE_CONTROL:
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
				bControlChanged = true;
				break;
			case TYPE_COLOR:
				m_generatedColorMaps.update(
						makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ), repository_slot );
				bColorChanged = true;
				break;
			default:
				m_generatedHeightMaps.update( getRegionMapOrDefault( pRegion, TYPE_HEIGHT ), repository_slot );
				m_generatedControlMaps.update( makeGpuRenderControlImage( region_loc ), repository_slot );
				m_generatedColorMaps.update(
						makeGpuColorImage( getRegionMapOrDefault( pRegion, TYPE_COLOR ) ), repository_slot );
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

void Terrain3DData::publishDerivedMapUpdates( const eastl::vector<t3dDerivedMapUpdate_t> &rUpdates ) {
	for ( const t3dDerivedMapUpdate_t &update : rUpdates ) {
		if ( !update.m_bSuccess ) {
			continue;
		}
		Terrain3DRegion *const pRegion = getRegionPtr( update.m_location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		if ( m_gpuRegionCache.isEnabled() ) {
			if ( m_gpuRegionCache.isResident( update.m_location ) ) {
				m_gpuRegionCache.updateResidentLocation( update.m_location, TYPE_HEIGHT, getComputeBackendIfCreated() );
			} else {
				m_gpuRegionCache.markSelectionDirty();
			}
			continue;
		}

		const int repositorySlot = getRegionId( update.m_location );
		const Ref<Image> pDerivedMap = pRegion->getDerivedNormalSlopeMap();
		if ( repositorySlot >= 0 && pDerivedMap.is_valid() && !m_generatedDerivedMaps.isDirty() &&
				repositorySlot < m_derivedMaps.size() && repositorySlot < m_generatedDerivedMaps.size() ) {
			m_derivedMaps[repositorySlot] = pDerivedMap;
			m_generatedDerivedMaps.update( pDerivedMap, repositorySlot );
		}
	}
}

void Terrain3DData::regenerateDerivedMaps( const eastl::vector<Vector2i> &rRegionLocations, const AABB &rEditedArea ) {
	if ( rRegionLocations.empty() ) {
		return;
	}
	const eastl::vector<t3dDerivedMapUpdate_t> &updates =
			m_mapEditService.regenerateDerivedMaps( rRegionLocations, rEditedArea, getComputeBackend() );
	publishDerivedMapUpdates( updates );
}

void Terrain3DData::commitMapEdits( const t3dMapType_t eMapType,
		const eastl::vector<Vector2i> &rRegionLocations,
		const AABB &rEditedArea,
		const uint32_t nFlags ) {
	if ( eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX || rRegionLocations.empty() ) {
		return;
	}

	const bool bKeepEdited = ( nFlags & MAP_EDIT_KEEP_EDITED ) != 0u;
	Terrain3DCompute *const pCompute = eMapType == TYPE_HEIGHT ? getComputeBackend() : nullptr;
	t3dMapMutation_t mutation = m_mapEditService.commitMapEdits( eMapType, rRegionLocations, rEditedArea, pCompute );
	if ( !mutation.valid() ) {
		return;
	}

	const eastl::vector<Vector2i> &activeLocations = mutation.m_changedLocations;
	const eastl::vector<Vector2i> &derivedLocations = mutation.m_derivedLocations;
	publishDerivedMapUpdates( mutation.m_derivedUpdates );

	if ( m_gpuRegionCache.isEnabled() ) {
		for ( const Vector2i &location : activeLocations ) {
			markGpuFarRegionDirty( location, eMapType );
		}
	}
	if ( eMapType == TYPE_HEIGHT && isFarAtlasRequired() && m_pTerrain ) {
		const Ref<Terrain3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) {
			for ( const Vector2i &location : derivedLocations ) {
				markGpuFarRegionDirty( location, TYPE_COLOR );
			}
		}
	}

	updateMapRegions( eMapType, activeLocations );
	m_mapEditService.finalizeMapEdits( mutation, bKeepEdited );

	if ( mutation.m_editedArea.size.x > 0.0f && mutation.m_editedArea.size.z > 0.0f ) {
		LOG( DEBUG, "Emitting maps_edited" );
		emit_signal( "maps_edited", mutation.m_editedArea );
	}

	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_INSTANCES ) != 0u && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateTransforms( mutation.m_editedArea );
	}
	if ( m_pTerrain && ( nFlags & MAP_EDIT_UPDATE_COLLISION ) != 0u ) {
		Terrain3DCollision *const pCollision = m_pTerrain->getCollision();
		if ( pCollision && pCollision->isEnabled() ) {
			if ( pCollision->isDynamicMode() ) {
				pCollision->update( s_v2iMax, true );
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

void Terrain3DData::commitMapEdit( const t3dMapType_t eMapType, const Vector2i &rRegionLocation, const Rect2i &rDirtyRect ) {
	if ( eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Invalid map type: ", eMapType );
		return;
	}

	AABB editedArea;
	if ( !m_mapEditService.buildRegionEditArea( rRegionLocation, rDirtyRect, editedArea ) ) {
		LOG( ERROR, "No active region found at: ", rRegionLocation );
		return;
	}

	uint32_t flags = MAP_EDIT_SNAP;
	if ( eMapType == TYPE_HEIGHT || eMapType == TYPE_CONTROL ) {
		flags |= MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION;
	}
	commitMapEdits( eMapType, { rRegionLocation }, editedArea, flags );
}

Ref<Terrain3DErosionJob> Terrain3DData::createErosionJob( const Ref<Terrain3DErosionSettings> &rSettings ) {
	Ref<Terrain3DErosionJob> pJob;
	pJob.instantiate();
	if ( pJob->setup( this, rSettings ) != OK ) {
		return pJob;
	}
	return pJob;
}

void Terrain3DData::setPixel( const t3dMapType_t eMapType, const Vector3 &rGlobalPosition, const Color &rPixel ) {
	if ( eMapType < TYPE_HEIGHT || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Specified map type out of range" );
		return;
	}
	const Vector2i regionLocation = getRegionLocation( rGlobalPosition );
	if ( !ensureCpuRegionResident( regionLocation ) ) {
		LOG( ERROR, "Unable to load CPU map payload for region: ", regionLocation );
		return;
	}

	Vector2i writtenLocation;
	Rect2i dirtyRect;
	const Error writeError = m_mapEditService.setPixel( eMapType, rGlobalPosition, rPixel, writtenLocation, dirtyRect );
	if ( writeError != OK ) {
		LOG( ERROR, "Unable to edit terrain map at: ", rGlobalPosition, " error: ", writeError );
		return;
	}
	commitMapEdit( eMapType, writtenLocation, dirtyRect );
}

Color Terrain3DData::getPixel( const t3dMapType_t eMapType, const Vector3 &rGlobalPosition ) const {
	if ( eMapType < 0 || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Specified map type out of range" );
		return s_colorNan;
	}
	const Vector2i region_loc = getRegionLocation( rGlobalPosition );
	if ( eMapType != TYPE_HEIGHT && m_cpuRegionCache.isEnabled() &&
			!const_cast<Terrain3DData *>( this )->ensureCpuRegionResident( region_loc ) ) {
		return s_colorNan;
	}
	const Terrain3DRegion *pRegion = getRegionPtr( region_loc );
	if ( !pRegion ) {
		return s_colorNan;
	}
	if ( pRegion->isDeleted() ) {
		return s_colorNan;
	}
	const Vector2i global_offset = region_loc * m_regionRepository.getRegionSize();
	const Vector3 descaled_pos = rGlobalPosition / m_regionRepository.getVertexSpacing();
	Vector2i img_pos = Vector2i( descaled_pos.x - global_offset.x, descaled_pos.z - global_offset.y );
	img_pos = img_pos.clamp( s_v2iZero, V2I( m_regionRepository.getRegionSize() - 1 ) );
	const Terrain3DActiveMapLease active = m_samplingStorage.acquireActive( region_loc );
	if ( active.valid() ) {
		const t3dActiveMapView_t &view = active.view();
		const int index = img_pos.y * m_regionRepository.getRegionSize() + img_pos.x;
		switch ( eMapType ) {
			case TYPE_HEIGHT:
				return Color( t3dRegionCodecDecodeHeightU16( view.m_pHeight[index] ), 0.0f, 0.0f, 1.0f );
			default:
				break;
		}
	}
	Image *pMap = pRegion->getMapPtr( eMapType );
	if ( !pMap ) {
		if ( eMapType == TYPE_CONTROL ) {
			return COLOR_CONTROL;
		}
		if ( eMapType == TYPE_COLOR ) {
			return COLOR_ROUGHNESS;
		}
		return s_colorNan;
	}
	if ( eMapType == TYPE_HEIGHT ) {
		return Color( t3dRegionCodecReadHeightPixel( pMap, img_pos ), 0.0f, 0.0f, 1.0f );
	}
	return pMap->get_pixelv( img_pos );
}

real_t Terrain3DData::getHeight( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return NAN;
	}
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleHeight( rGlobalPosition );
}

uint32_t Terrain3DData::getControl( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 0 ) ) {
		return UINT32_MAX;
	}
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleControl( rGlobalPosition );
}

PackedFloat32Array Terrain3DData::getHeights( const PackedVector3Array &rGlobalPositions ) const {
	PackedFloat32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	Terrain3DSampler sampler( getSamplingSourceInternal() );
	if ( !m_cpuRegionCache.isEnabled() ) {
		sampler.sampleHeights( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	float *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( position, 1 )
				? sampler.sampleHeight( position )
				: NAN;
	}
	return result;
}

PackedInt32Array Terrain3DData::getControls( const PackedVector3Array &rGlobalPositions ) const {
	PackedInt32Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	Terrain3DSampler sampler( getSamplingSourceInternal() );
	if ( !m_cpuRegionCache.isEnabled() ) {
		sampler.sampleControls( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	int32_t *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( position, 0 )
				? ( int32_t )( sampler.sampleControl( position ) )
				: ( int32_t )( UINT32_MAX );
	}
	return result;
}

PackedVector3Array Terrain3DData::getNormals( const PackedVector3Array &rGlobalPositions ) const {
	PackedVector3Array result;
	result.resize( rGlobalPositions.size() );
	if ( rGlobalPositions.is_empty() ) {
		return result;
	}

	Terrain3DSampler sampler( getSamplingSourceInternal() );
	if ( !m_cpuRegionCache.isEnabled() ) {
		sampler.sampleNormals( rGlobalPositions.ptr(), result.ptrw(), rGlobalPositions.size() );
		return result;
	}
	Vector3 *pWrite = result.ptrw();
	for ( int index = 0; index < rGlobalPositions.size(); ++index ) {
		const Vector3 position = rGlobalPositions[index];
		pWrite[index] = const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( position, 1 )
				? sampler.sampleNormal( position )
				: s_v3Nan;
	}
	return result;
}

Vector3 Terrain3DData::getNormal( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return s_v3Nan;
	}
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	return sampler.sampleNormal( rGlobalPosition );
}

bool Terrain3DData::isInSlope( const Vector3 &rGlobalPosition, const Vector2 &rSlopeRange, const Vector3 &rNormal ) const {
	const Vector2 slope_range = CLAMP( rSlopeRange, s_v2Zero, V2( 90.f ) );
	if ( slope_range.y - slope_range.x > 89.99f ) {
		return true;
	}

	Vector3 slope_normal = rNormal;
	if ( !slope_normal.is_zero_approx() ) {
		slope_normal.normalize();
	} else {
		if ( !const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
			return false;
		}
		Terrain3DSampler sampler( getSamplingSourceInternal() );
		uint32_t control = UINT32_MAX;
		if ( !sampler.sampleControlAndSlopeNormal( rGlobalPosition, control, slope_normal ) ) {
			return false;
		}
	}

	const real_t fSlopeAngleDegrees =
			Math::rad_to_deg( Math::acos( CLAMP( slope_normal.dot( s_v3Up ), -1.0f, 1.0f ) ) );
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
Vector3 Terrain3DData::getTextureId( const Vector3 &rGlobalPosition ) const {
	if ( !const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, 1 ) ) {
		return s_v3Nan;
	}
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	const uint32_t control = sampler.sampleControl( rGlobalPosition );
	if ( control == UINT32_MAX || t3dIsHole( control ) ) {
		return s_v3Nan;
	}

	// If material available, autoshader enabled, and pixel set to auto
	if ( m_pTerrain ) {
		const Ref<Terrain3DMaterial> pTMaterial = m_pTerrain->getMaterial();
		const bool bControlAuto = t3dIsAuto( control );
		if ( pTMaterial.is_valid() && pTMaterial->getAutoShaderEnabled() && bControlAuto ) {
			const real_t fAutoSlope = ( real_t )( pTMaterial->getShaderParam( "auto_slope" ) );
			const real_t fAutoHeightReduction = ( real_t )( pTMaterial->getShaderParam( "auto_height_reduction" ) );
			float fHeight = NAN;
			Vector3 normal = s_v3Nan;
			if ( !sampler.sampleHeightAndNormal( rGlobalPosition, fHeight, normal ) ) {
				return s_v3Nan;
			}
			const uint32_t base_id = pTMaterial->getShaderParam( "auto_base_texture" );
			const uint32_t overlay_id = pTMaterial->getShaderParam( "auto_overlay_texture" );
			const real_t fBlend = CLAMP(
					( fAutoSlope * 2.f * ( normal.y - 1.f ) + 1.f ) - fAutoHeightReduction * .01f * fHeight, 0.f, 1.f );
			return Vector3( ( real_t )( base_id ), ( real_t )( overlay_id ), fBlend );
		}
	}

	// Else, just get textures from control map
	const uint32_t base_id = t3dGetBase( control );
	const uint32_t overlay_id = t3dGetOverlay( control );
	const real_t fBlend = ( real_t )( t3dGetBlend( control ) ) / s_fByteMaxFloat;
	return Vector3( ( real_t )( base_id ), ( real_t )( overlay_id ), fBlend );
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
Vector3
Terrain3DData::getMeshVertex( const int32_t nLod, const t3dHeightFilter_t filter, const Vector3 &rGlobalPosition ) const {
	const int32_t step = 1 << CLAMP( nLod, 0, 8 );
	if ( !const_cast<Terrain3DData *>( this )->ensureCpuSampleArea( rGlobalPosition, step / 2 + 1 ) ) {
		return Vector3( rGlobalPosition.x, NAN, rGlobalPosition.z );
	}
	real_t fHeight = 0.0f;
	Terrain3DSampler sampler( getSamplingSourceInternal() );

	switch ( filter ) {
		case HEIGHT_FILTER_NEAREST: {
			fHeight = sampler.sampleHeight( rGlobalPosition );
		} break;
		case HEIGHT_FILTER_MINIMUM: {
			const double fGridX = ( double )( rGlobalPosition.x ) / ( double )( m_regionRepository.getVertexSpacing() );
			const double fGridZ = ( double )( rGlobalPosition.z ) / ( double )( m_regionRepository.getVertexSpacing() );
			const int64_t rounded_x = ( int64_t )( Math::round( fGridX ) );
			const int64_t rounded_z = ( int64_t )( Math::round( fGridZ ) );
			const double fWorldDx =
					( fGridX - ( double )( rounded_x ) ) * ( double )( m_regionRepository.getVertexSpacing() );
			const double fWorldDz =
					( fGridZ - ( double )( rounded_z ) ) * ( double )( m_regionRepository.getVertexSpacing() );
			if ( fWorldDx * fWorldDx + fWorldDz * fWorldDz < 0.0001 ) {
				float fMinimum = NAN;
				fHeight = sampler.minimumHeightRect(
								  rounded_x - step / 2, rounded_z - step / 2, step, step, fMinimum, true )
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
					const Vector3 position =
							rGlobalPosition + Vector3( dx, 0.f, dz ) * m_regionRepository.getVertexSpacing();
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

void Terrain3DData::addEditedArea( const AABB &rArea ) {
	m_mapEditService.addEditedArea( rArea );
	LOG( DEBUG, "Emitting maps_edited" );
	emit_signal( "maps_edited", rArea );
}

void Terrain3DData::calcHeightRange( const bool bRecursive ) {
	m_mapEditService.calculateHeightRange(
			bRecursive,
			[]( void *const pContext, const Vector2i &rLocation ) -> bool {
				Terrain3DData *const pData = static_cast<Terrain3DData *>( pContext );
				if ( pData->ensureCpuRegionResident( rLocation ) ) {
					return true;
				}
				UtilityFunctions::push_error( "Terrain3DData:calcHeightRange: Unable to page region for recursive height-range calculation: ", rLocation );
				return false;
			},
			this );
	LOG( EXTREME, "Accumulated height range for all regions: ", m_mapEditService.getHeightRange() );
}

/**
 * Imports an Image set (Height, Control, Color) into Terrain3DData
 * It does NOT normalize values to 0-1. You must do that using get_min_max() and adjusting scale and offset.
 * Parameters:
 *	p_images - t3dMapType_t.TYPE_MAX sized array of Images for Height, Control, Color. Images can be blank or null
 *	p_global_position - X,0,Z location on the region map. Valid range is ~ (+/-8192, +/-8192)
 *	p_offset - Add this factor to all height values, can be negative
 *	p_scale - Scale all height values by this factor (applied after offset)
 */
void Terrain3DData::importImages( const TypedArray<Image> &rImages,
		const Vector3 &rGlobalPosition,
		const real_t fOffset,
		const real_t fScale ) {
	IS_INIT_MESG( "Data not initialized", VOID );

	bool bGenerateMipmaps = false;
	const Error error = m_mapEditService.importImages(
			rImages,
			rGlobalPosition,
			fOffset,
			fScale,
			[]( void *const pContext, const Vector2i &rLocation, Ref<Terrain3DRegion> &rRegion ) -> Error {
				Terrain3DData *const pData = static_cast<Terrain3DData *>( pContext );
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
						UtilityFunctions::push_error( "Terrain3DData:importImages: Unable to page existing CPU terrain maps for image import at ", rLocation );
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
			this,
			bGenerateMipmaps );
	if ( error != OK ) {
		LOG( ERROR, "Image import failed with error: ", error );
		return;
	}

	updateMaps( REGION_TYPE_MAX, true, bGenerateMipmaps );
}

/** Exports a specified map as one of r16/raw, exr, jpg, png, webp, res, tres
 * r16 or exr are recommended for roundtrip external editing
 * r16 can be edited by Krita, however you must know the dimensions and min/max before reimporting
 * res/tres stores in Godot's native format.
 */
Error Terrain3DData::exportImage( const String &rFileName, const t3dMapType_t eMapType ) const {
	if ( eMapType < 0 || eMapType >= REGION_TYPE_MAX ) {
		LOG( ERROR, "Invalid map type specified: ", eMapType, " max: ", REGION_TYPE_MAX - 1 );
		return FAILED;
	}

	if ( rFileName.is_empty() ) {
		LOG( ERROR, "No file specified. Nothing to export" );
		return FAILED;
	}

	if ( getRegionCount() == 0 ) {
		LOG( ERROR, "No valid regions. Nothing to export" );
		return FAILED;
	}

	// Simple file name validation
	static const String s_badChars = "?*|%<>\"";
	for ( int i = 0; i < s_badChars.length(); ++i ) {
		for ( int j = 0; j < rFileName.length(); ++j ) {
			if ( s_badChars[i] == rFileName[j] ) {
				LOG( ERROR, String( "Invalid file path '" ) + rFileName + String( "'" ) );
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
		file_name = String( "res://" ) + file_name;
	}

	// Check if the file can be opened for writing
	Ref<FileAccess> pFileRef = FileAccess::open( file_name, FileAccess::ModeFlags::WRITE );
	if ( pFileRef.is_null() ) {
		LOG( ERROR, String( "Cannot open file '" ) + file_name + String( "' for writing" ) );
		return FAILED;
	}
	pFileRef->close();

	// Filename is validated. Begin export image generation
	Ref<Image> pImg = layeredToImage( eMapType );
	if ( pImg.is_null() || pImg->is_empty() ) {
		LOG( ERROR, "Cannot create an export image for map type: ", s_pszRegionMapTypeNames[eMapType] );
		return FAILED;
	}

	String ext = file_name.get_extension().to_lower();
	LOG( MESG,
			"Saving ",
			pImg->get_size(),
			" sized ",
			s_pszRegionMapTypeNames[eMapType],
			" map in format ",
			pImg->get_format(),
			" as ",
			ext,
			" to: ",
			file_name );
	const Vector2i minmax = Terrain3DUtil::getMinMax( pImg );
	LOG( MESG, "Minimum height: ", minmax.x, ", Maximum height: ", minmax.y );
	if ( ext == "r16" || ext == "raw" ) {
		Ref<FileAccess> pFile = FileAccess::open( file_name, FileAccess::WRITE );
		const real_t fHeightMin = minmax.x;
		const real_t fHeightMax = minmax.y;
		const real_t fHscale = 65535.0 / ( fHeightMax - fHeightMin );
		for ( int y = 0; y < pImg->get_height(); y++ ) {
			for ( int x = 0; x < pImg->get_width(); x++ ) {
				int h = ( int )( ( pImg->get_pixel( x, y ).r - fHeightMin ) * fHscale );
				h = CLAMP( h, 0, 65535 );
				pFile->store_16( h );
			}
		}
		return pFile->get_error();
	} else if ( ext == "exr" ) {
		return pImg->save_exr( file_name, ( eMapType == TYPE_HEIGHT ) ? true : false );
	} else if ( ext == "png" ) {
		return pImg->save_png( file_name );
	} else if ( ext == "jpg" ) {
		return pImg->save_jpg( file_name );
	} else if ( ext == "webp" ) {
		return pImg->save_webp( file_name );
	} else if ( ( ext == "res" ) || ( ext == "tres" ) ) {
		return ResourceSaver::get_singleton()->save( pImg, file_name, ResourceSaver::FLAG_COMPRESS );
	}

	LOG( ERROR, "No recognized file type. See docs for valid extensions" );
	return FAILED;
}

Ref<Image> Terrain3DData::layeredToImage( const t3dMapType_t eMapType ) const {
	return m_mapEditService.layeredToImage(
			eMapType,
			[]( void *const pContext, const Vector2i &rLocation ) -> bool {
				Terrain3DData *const pData = static_cast<Terrain3DData *>( pContext );
				if ( pData->ensureCpuRegionResident( rLocation ) ) {
					return true;
				}
				UtilityFunctions::push_error( "Terrain3DData:layeredToImage: Unable to page region for image export: ", rLocation );
				return false;
			},
			const_cast<Terrain3DData *>( this ) );
}

void Terrain3DData::dump( const bool bVerbose ) const {
	const TypedArray<Vector2i> region_slots = getRegionSlots();
	LOG( MESG, "_region_locations (", m_regionRepository.getSlotCount(), "): ", region_slots );
	const Array keys = m_regionRepository.getRegionKeys();
	LOG( MESG, "_regions (", keys.size(), "):" );
	for ( const Vector2i region_loc : keys ) {
		const Terrain3DRegion *pRegion = getRegionPtr( region_loc );
		if ( !pRegion ) {
			LOG( WARN, "No region found at: ", region_loc );
			continue;
		}
		pRegion->dump( bVerbose );
	}
	if ( bVerbose ) {
		for ( int i = 0; i < s_nMaxRegionSlots; i++ ) {
			if ( m_regionRepository.getLogicalRegionMapData()[i] ) {
				LOG( MESG,
						"Region map array index: ",
						i,
						" / ",
						s_nMaxRegionSlots - 1,
						", Region id: ",
						m_regionRepository.getLogicalRegionMapData()[i] );
			}
		}
		Terrain3DUtil::dumpMaps( m_heightMaps, "Height maps" );
		Terrain3DUtil::dumpGentex( m_generatedHeightMaps, "height" );
		Terrain3DUtil::dumpMaps( m_controlMaps, "Control maps" );
		Terrain3DUtil::dumpGentex( m_generatedControlMaps, "control" );
		Terrain3DUtil::dumpMaps( m_colorMaps, "Color maps" );
		Terrain3DUtil::dumpGentex( m_generatedColorMaps, "color" );
	}
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DData::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_NEAREST );
	BIND_ENUM_CONSTANT( HEIGHT_FILTER_MINIMUM );

	ClassDB::bind_integer_constant( get_class_static(), "", "WORLD_TEXEL_SIZE", s_nActiveWorldTexels );
	ClassDB::bind_integer_constant( get_class_static(), "", "REGION_MAP_SIZE", s_nActiveMaxRegionMapSize );

	ClassDB::bind_method( D_METHOD( "get_region_count" ), &Terrain3DData::getRegionCount );
	ClassDB::bind_method( D_METHOD( "set_region_locations", "region_locations" ), &Terrain3DData::setRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_region_locations" ), &Terrain3DData::getRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_render_region_slots" ), &Terrain3DData::getRenderRegionSlots );
	ClassDB::bind_method( D_METHOD( "get_regions_active", "copy", "deep" ),
			&Terrain3DData::getRegionsActive,
			DEFVAL( false ),
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_regions_all" ), &Terrain3DData::getRegionsAll );
	ClassDB::bind_method( D_METHOD( "get_region_map" ), &Terrain3DData::getRegionMap );
	ClassDB::bind_method( D_METHOD( "get_region_map_size" ), &Terrain3DData::getRegionMapSize );
	ClassDB::bind_method( D_METHOD( "is_runtime_ready" ), &Terrain3DData::isRuntimeReady );
	ClassDB::bind_method( D_METHOD( "get_active_storage_generation" ), &Terrain3DData::getActiveStorageGeneration );
	ClassDB::bind_method( D_METHOD( "get_runtime_state" ), &Terrain3DData::getRuntimeState );
	ClassDB::bind_method( D_METHOD( "get_gpu_region_cache_stats" ), &Terrain3DData::getGpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_middle_height_cache_stats" ), &Terrain3DData::getMiddleHeightCacheStats );
	ClassDB::bind_method( D_METHOD( "get_startup_load_stats" ), &Terrain3DData::getStartupLoadStats );
	ClassDB::bind_method(
			D_METHOD( "get_runtime_residency_grid", "radius" ), &Terrain3DData::getRuntimeResidencyGrid, DEFVAL( 6 ) );
	ClassDB::bind_method( D_METHOD( "get_cpu_region_cache_stats" ), &Terrain3DData::getCpuRegionCacheStats );
	ClassDB::bind_method( D_METHOD( "get_instance_region_cache_stats" ), &Terrain3DData::getInstanceRegionCacheStats );
	ClassDB::bind_method(
			D_METHOD( "ensure_instance_region_resident", "location" ), &Terrain3DData::ensureInstanceRegionResident );
	ClassDB::bind_method(
			D_METHOD( "ensure_cpu_region_resident", "region_location" ), &Terrain3DData::ensureCpuRegionResident );
	ClassDB::bind_static_method(
			"Terrain3DData", D_METHOD( "get_region_map_index", "region_location" ), &Terrain3DData::getRegionMapIndex );

	ClassDB::bind_method( D_METHOD( "do_for_regions", "area", "callback" ), &Terrain3DData::doForRegions );
	ClassDB::bind_method( D_METHOD( "change_region_size", "region_size" ), &Terrain3DData::changeRegionSize );

	ClassDB::bind_method( D_METHOD( "get_region_location", "global_position" ), &Terrain3DData::getRegionLocation );
	ClassDB::bind_method( D_METHOD( "get_region_id", "region_location" ), &Terrain3DData::getRegionId );
	ClassDB::bind_method( D_METHOD( "get_region_idp", "global_position" ), &Terrain3DData::getRegionIdp );

	ClassDB::bind_method( D_METHOD( "has_region", "region_location" ), &Terrain3DData::hasRegion );
	ClassDB::bind_method( D_METHOD( "has_regionp", "global_position" ), &Terrain3DData::hasRegionp );
	ClassDB::bind_method( D_METHOD( "get_region", "region_location" ), &Terrain3DData::getRegion );
	ClassDB::bind_method( D_METHOD( "get_regionp", "global_position" ), &Terrain3DData::getRegionp );

	ClassDB::bind_method(
			D_METHOD( "set_region_modified", "region_location", "modified" ), &Terrain3DData::setRegionModified );
	ClassDB::bind_method( D_METHOD( "is_region_modified", "region_location" ), &Terrain3DData::isRegionModified );
	ClassDB::bind_method( D_METHOD( "set_region_deleted", "region_location", "deleted" ), &Terrain3DData::setRegionDeleted );
	ClassDB::bind_method( D_METHOD( "is_region_deleted", "region_location" ), &Terrain3DData::isRegionDeleted );

	ClassDB::bind_method(
			D_METHOD( "add_region_blankp", "global_position", "update" ), &Terrain3DData::addRegionBlankp, DEFVAL( true ) );
	ClassDB::bind_method(
			D_METHOD( "add_region_blank", "region_location", "update" ), &Terrain3DData::addRegionBlank, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "add_region", "region", "update" ), &Terrain3DData::addRegion, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "replace_region_data", "region_location", "data" ), &Terrain3DData::replaceRegionData );
	ClassDB::bind_method(
			D_METHOD( "remove_regionp", "global_position", "update" ), &Terrain3DData::removeRegionp, DEFVAL( true ) );
	ClassDB::bind_method(
			D_METHOD( "remove_regionl", "region_location", "update" ), &Terrain3DData::removeRegionl, DEFVAL( true ) );
	ClassDB::bind_method( D_METHOD( "remove_region", "region", "update" ), &Terrain3DData::removeRegion, DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "save_directory", "directory" ), &Terrain3DData::saveDirectory );
	ClassDB::bind_method( D_METHOD( "save_region", "region_location", "directory", "save_16_bit" ),
			&Terrain3DData::saveRegion,
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "inspect_gpu_region_pages" ), &Terrain3DData::inspectGpuRegionPages );
	ClassDB::bind_method( D_METHOD( "cook_gpu_region_pages" ), &Terrain3DData::cookGpuRegionPages );
	ClassDB::bind_method( D_METHOD( "load_directory", "directory" ), &Terrain3DData::loadDirectory );
	ClassDB::bind_method(
			D_METHOD( "load_region", "region_location", "directory", "update" ), &Terrain3DData::loadRegion, DEFVAL( true ) );

	ClassDB::bind_method( D_METHOD( "get_height_maps" ), &Terrain3DData::getHeightMaps );
	ClassDB::bind_method( D_METHOD( "get_control_maps" ), &Terrain3DData::getControlMaps );
	ClassDB::bind_method( D_METHOD( "get_color_maps" ), &Terrain3DData::getColorMaps );
	ClassDB::bind_method( D_METHOD( "get_maps", "map_type" ), &Terrain3DData::getMaps );
	ClassDB::bind_method( D_METHOD( "update_maps", "map_type", "all_regions", "generate_mipmaps" ),
			&Terrain3DData::updateMaps,
			DEFVAL( REGION_TYPE_MAX ),
			DEFVAL( true ),
			DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "create_erosion_job", "settings" ), &Terrain3DData::createErosionJob );
	ClassDB::bind_method( D_METHOD( "get_height_maps_rid" ), &Terrain3DData::getHeightMapsRid );
	ClassDB::bind_method( D_METHOD( "get_derived_maps_rid" ), &Terrain3DData::getDerivedMapsRid );
	ClassDB::bind_method( D_METHOD( "get_control_maps_rid" ), &Terrain3DData::getControlMapsRid );
	ClassDB::bind_method( D_METHOD( "get_color_maps_rid" ), &Terrain3DData::getColorMapsRid );
	ClassDB::bind_method( D_METHOD( "are_control_maps_implicit" ), &Terrain3DData::areControlMapsImplicit );
	ClassDB::bind_method( D_METHOD( "are_color_maps_implicit" ), &Terrain3DData::areColorMapsImplicit );

	ClassDB::bind_method( D_METHOD( "set_pixel", "map_type", "global_position", "pixel" ), &Terrain3DData::setPixel );
	ClassDB::bind_method( D_METHOD( "commit_map_edit", "map_type", "region_location", "dirty_rect" ),
			&Terrain3DData::commitMapEdit,
			DEFVAL( Rect2i() ) );
	ClassDB::bind_method( D_METHOD( "get_pixel", "map_type", "global_position" ), &Terrain3DData::getPixel );
	ClassDB::bind_method( D_METHOD( "set_height", "global_position", "height" ), &Terrain3DData::setHeight );
	ClassDB::bind_method( D_METHOD( "get_height", "global_position" ), &Terrain3DData::getHeight );
	ClassDB::bind_method( D_METHOD( "get_heights", "global_positions" ), &Terrain3DData::getHeights );
	ClassDB::bind_method( D_METHOD( "get_normals", "global_positions" ), &Terrain3DData::getNormals );
	ClassDB::bind_method( D_METHOD( "get_controls", "global_positions" ), &Terrain3DData::getControls );
	ClassDB::bind_method( D_METHOD( "set_color", "global_position", "color" ), &Terrain3DData::setColor );
	ClassDB::bind_method( D_METHOD( "get_color", "global_position" ), &Terrain3DData::getColor );
	ClassDB::bind_method( D_METHOD( "set_control", "global_position", "control" ), &Terrain3DData::setControl );
	ClassDB::bind_method( D_METHOD( "get_control", "global_position" ), &Terrain3DData::getControl );
	ClassDB::bind_method( D_METHOD( "set_roughness", "global_position", "roughness" ), &Terrain3DData::setRoughness );
	ClassDB::bind_method( D_METHOD( "get_roughness", "global_position" ), &Terrain3DData::getRoughness );

	ClassDB::bind_method(
			D_METHOD( "set_control_base_id", "global_position", "texture_id" ), &Terrain3DData::setControlBaseId );
	ClassDB::bind_method( D_METHOD( "get_control_base_id", "global_position" ), &Terrain3DData::getControlBaseId );
	ClassDB::bind_method(
			D_METHOD( "set_control_overlay_id", "global_position", "texture_id" ), &Terrain3DData::setControlOverlayId );
	ClassDB::bind_method( D_METHOD( "get_control_overlay_id", "global_position" ), &Terrain3DData::getControlOverlayId );
	ClassDB::bind_method(
			D_METHOD( "set_control_blend", "global_position", "blend_value" ), &Terrain3DData::setControlBlend );
	ClassDB::bind_method( D_METHOD( "get_control_blend", "global_position" ), &Terrain3DData::getControlBlend );
	ClassDB::bind_method( D_METHOD( "set_control_angle", "global_position", "degrees" ), &Terrain3DData::setControlAngle );
	ClassDB::bind_method( D_METHOD( "get_control_angle", "global_position" ), &Terrain3DData::getControlAngle );
	ClassDB::bind_method(
			D_METHOD( "set_control_scale", "global_position", "percentage_modifier" ), &Terrain3DData::setControlScale );
	ClassDB::bind_method( D_METHOD( "get_control_scale", "global_position" ), &Terrain3DData::getControlScale );
	ClassDB::bind_method( D_METHOD( "set_control_hole", "global_position", "enable" ), &Terrain3DData::setControlHole );
	ClassDB::bind_method( D_METHOD( "get_control_hole", "global_position" ), &Terrain3DData::getControlHole );
	ClassDB::bind_method(
			D_METHOD( "set_control_navigation", "global_position", "enable" ), &Terrain3DData::setControlNavigation );
	ClassDB::bind_method( D_METHOD( "get_control_navigation", "global_position" ), &Terrain3DData::getControlNavigation );
	ClassDB::bind_method( D_METHOD( "set_control_auto", "global_position", "enable" ), &Terrain3DData::setControlAuto );
	ClassDB::bind_method( D_METHOD( "get_control_auto", "global_position" ), &Terrain3DData::getControlAuto );

	ClassDB::bind_method( D_METHOD( "get_normal", "global_position" ), &Terrain3DData::getNormal );
	ClassDB::bind_method( D_METHOD( "is_in_slope", "global_position", "slope_range", "normal" ),
			&Terrain3DData::isInSlope,
			DEFVAL( s_v3Zero ) );
	ClassDB::bind_method( D_METHOD( "get_texture_id", "global_position" ), &Terrain3DData::getTextureId );
	ClassDB::bind_method( D_METHOD( "get_mesh_vertex", "lod", "filter", "global_position" ), &Terrain3DData::getMeshVertex );

	ClassDB::bind_method( D_METHOD( "get_height_range" ), &Terrain3DData::getHeightRange );
	ClassDB::bind_method( D_METHOD( "calc_height_range", "recursive" ), &Terrain3DData::calcHeightRange, DEFVAL( false ) );

	ClassDB::bind_method( D_METHOD( "import_images", "images", "global_position", "offset", "scale" ),
			&Terrain3DData::importImages,
			DEFVAL( s_v3Zero ),
			DEFVAL( 0.f ),
			DEFVAL( 1.f ) );
	ClassDB::bind_method( D_METHOD( "export_image", "file_name", "map_type" ), &Terrain3DData::exportImage );
	ClassDB::bind_method( D_METHOD( "layered_to_image", "map_type" ), &Terrain3DData::layeredToImage );
	ClassDB::bind_method( D_METHOD( "dump", "verbose" ), &Terrain3DData::dump, DEFVAL( false ) );

	const int ro_flags = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Runtime Repository Status", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "region_locations", PROPERTY_HINT_ARRAY_TYPE, "Vector2i", ro_flags ),
			"set_region_locations",
			"get_region_locations" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "height_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ),
			"",
			"get_height_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "control_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ),
			"",
			"get_control_maps" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "color_maps", PROPERTY_HINT_ARRAY_TYPE, "Image", ro_flags ),
			"",
			"get_color_maps" );

	ADD_SIGNAL( MethodInfo( "maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "gpu_region_page_table_changed" ) );
	ADD_SIGNAL( MethodInfo( "region_map_changed" ) );
	ADD_SIGNAL( MethodInfo( "height_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "control_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "color_maps_changed" ) );
	ADD_SIGNAL( MethodInfo( "maps_edited", PropertyInfo( Variant::AABB, "edited_area" ) ) );
}
