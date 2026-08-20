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
#include <EASTL/bitset.h>
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

typedef struct t3dfarSurfaceBakeData_s {
	eastl::array<uint8_t, TERRAIN3D_MAX_TEXTURE_ASSETS> populated = {};
	int nCount = 0;
	int fallback_texture = 0;
} t3dfarSurfaceBakeData_t;

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

static uint16_t terrain3dReadU16Unaligned( const uint8_t *pBytes, const size_t nOffset ) {
	uint16_t value = 0u;
	if ( pBytes ) {
		std::memcpy( &value, pBytes + nOffset, sizeof( value ) );
	}
	return value;
}

static uint8_t terrain3dColorToByte( const float fValue ) {
	return ( uint8_t ) ( CLAMP( Math::round( fValue * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
}

static void terrain3dFillFarSurfaceEmpty( PackedByteArray &nBytes ) {
	uint8_t *pWrite = nBytes.ptrw();
	if ( !pWrite ) {
		return;
	}
	const size_t pixel_count = ( size_t ) ( nBytes.size() ) / 4u;
	for ( size_t index = 0u; index < pixel_count; ++index ) {
		const size_t offset = index * 4u;
		pWrite[offset] = 255u;
		pWrite[offset + 1u] = 255u;
		pWrite[offset + 2u] = 255u;
		pWrite[offset + 3u] = 0u;
	}
}

static uint8_t terrain3dFarSurfaceAlpha(
	const float fSolidCoverage, const float fColorAlpha,
	const int nMaterialId
) {
	if ( fSolidCoverage < 0.5f ) {
		return 0u;
	}
	// Bit 7 marks solid terrain. Bits 2-6 carry the dominant terrain texture
	// layer. Bits 0-1 retain coarse color-map alpha for wetness. This lets
	// the far shader recover one real terrain material sample without another
	// control/material atlas lookup.
	const uint32_t material = ( uint32_t ) ( CLAMP( nMaterialId, 0, 31 ) );
	const uint32_t surface_alpha = ( uint32_t ) ( CLAMP(
		Math::round( CLAMP( fColorAlpha, 0.0f, 1.0f ) * 3.0f ), 0.0f, 3.0f ) );
	return ( uint8_t ) ( 0x80u | ( material << 2u ) | surface_alpha );
}

static float terrain3dMaterialFloatParam(
	T3DMaterial *pMaterial, const char *pszName,
	const float fDefault
) {
	if ( !pMaterial ) {
		return fDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? fDefault : ( float ) ( value );
}

static int terrain3dMaterialIntParam(
	T3DMaterial *pMaterial, const char *pszName,
	const int nDefault
) {
	if ( !pMaterial ) {
		return nDefault;
	}
	const Variant value = pMaterial->getShaderParam( StringName( pszName ) );
	return value.get_type() == Variant::NIL ? nDefault : ( int ) ( value );
}

static int terrain3dFarResolveMaterialId(
	const t3dfarSurfaceBakeData_t &rBakeData,
	const int nTextureId
) {
	if ( nTextureId >= 0 && nTextureId < rBakeData.nCount &&
		rBakeData.populated[( size_t ) ( nTextureId )] != 0u ) {
		return nTextureId;
	}
	return rBakeData.fallback_texture;
}

static void terrain3dBuildFarSurfaceBakeData(
	T3DTerrain *pTerrain,
	t3dfarSurfaceBakeData_t &rBakeData
) {
	rBakeData = t3dfarSurfaceBakeData_s();
	if ( !pTerrain ) {
		return;
	}
	const Ref<T3DAssets> pAssets = pTerrain->getAssets();
	if ( pAssets.is_null() ) {
		return;
	}
	rBakeData.nCount = MIN( pAssets->getTextureCount(), TERRAIN3D_MAX_TEXTURE_ASSETS );
	bool bFoundFallback = false;
	for ( int index = 0; index < rBakeData.nCount; ++index ) {
		const bool bPopulated = pAssets->getTextureAsset( index ).is_valid();
		rBakeData.populated[( size_t ) ( index )] = bPopulated ? 1u : 0u;
		if ( bPopulated && !bFoundFallback ) {
			rBakeData.fallback_texture = index;
			bFoundFallback = true;
		}
	}
}

typedef enum t3dgpuRegionSlotState_e : uint8_t {
	TERRAIN3D_GPU_SLOT_FREE = 0u,
	TERRAIN3D_GPU_SLOT_STAGING = 1u,
	TERRAIN3D_GPU_SLOT_READY = 2u,
	TERRAIN3D_GPU_SLOT_MAPPED = 3u,
	TERRAIN3D_GPU_SLOT_RETIRED = 4u,
} t3dgpuRegionSlotState_t;

static size_t terrain3dGpuImageBytes( const Ref<Image> &rImage ) {
	return rImage.is_valid() ? ( size_t ) ( rImage->get_data().size() ) : 0u;
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
	const bool bStorageOk = m_samplingStorage.claimOrReconfigureActive( nRegionSize );
	if ( !bStorageOk ) {
		LOG( T3DTerrain::ERROR, "Unable to claim process-static active terrain storage. Only one Terrain3DData owner is legal." );
		return false;
	}
	m_regionRepository.configureGeometry( nRegionSize, m_regionRepository.getVertexSpacing() );
	m_activeHeightResidency.markDirty();
	if ( bRegionSizeChanged && m_bGpuRegionCacheEnabled ) {
		resetGpuRegionCache( true );
	}
	if ( bRegionSizeChanged && m_bCpuRegionCacheEnabled ) {
		resetCpuRegionCache( false );
		m_nCpuRegionCacheCapacity = calculateCpuRegionCacheCapacity();
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
	m_samplingStorage.resetActive();
	m_samplingStorage.resetResidentPool();
	resetCpuRegionCache( false );
	resetInstanceRegionCache( false );
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
	resetGpuRegionCache( false );
	m_generatedFarHeightMap.clear();
	m_generatedFarColorMap.clear();
	m_pGpuFarHeightImage.unref();
	m_pGpuFarColorImage.unref();
	m_gpuFarHeightDirtyRegions.reset();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarHeightDirty = true;
	m_bGpuFarColorDirty = true;
	m_bGpuFarHeightDirtyAll = true;
	m_bGpuFarColorDirtyAll = true;
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
	m_nGpuFarHeightDirtyTexelsLast = 0u;
	m_nGpuFarColorDirtyTexelsLast = 0u;
	m_nGpuFarHeightRebuiltBytesLast = 0u;
	m_nGpuFarColorRebuiltBytesLast = 0u;
	m_nGpuFarHeightUploadedBytesLast = 0u;
	m_nGpuFarColorUploadedBytesLast = 0u;
	m_nGpuFarHeightPublicationUsecLast = 0u;
	m_nGpuFarColorPublicationUsecLast = 0u;
	m_nGpuFarHeightPublicationCount = 0u;
	m_nGpuFarColorPublicationCount = 0u;
	m_nGpuFarHeightUploadedBytesTotal = 0u;
	m_nGpuFarColorUploadedBytesTotal = 0u;
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
	return bChanged;
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
	config.colorDivisor = MAX( 1, m_nGpuRegionColorDivisor );
	config.derivedDivisor = MAX( 1, m_nGpuRegionDerivedDivisor );
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
	frame.bCpuRegionCacheEnabled = m_bCpuRegionCacheEnabled;
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
	const T3DRuntimePageStats stats = m_runtimePageStore.getStats();
	m_nGpuRegionPreparedPagesLastTick = stats.nPreparedPages;
	m_nGpuRegionPrepareWorkersLastTick = stats.nPrepareWorkers;
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
	const T3DRuntimePageStats stats = m_runtimePageStore.getStats();
	m_nGpuRegionPreparedPagesLastTick = stats.nPreparedPages;
	m_nGpuRegionPrepareWorkersLastTick = stats.nPrepareWorkers;
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
	payload.pMiddleHeight = makeMiddleHeightImage( pRegion->getLocation(), true );
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
	if ( m_bGpuRegionCacheEnabled ) {
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

int T3DData::calculateAuthoringGpuRegionBudgetMb( void ) const {
	// m_nGpuRegionCacheBudgetMb is already the effective allocation produced by
	// Terrain3D's aggregate memory-budget coordinator. Authoring may use more
	// physical layers than runtime, but it must fit those layers inside that same
	// coordinated GPU allocation instead of manufacturing extra editor memory.
	return CLAMP( MAX( 1, m_nGpuRegionCacheBudgetMb ),
		TERRAIN3D_GPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_GPU_REGION_CACHE_MAX_BUDGET_MB );
}

int T3DData::calculateGpuRegionCapacityForLods( const int nDetailLods ) const {
	if ( !m_pTerrain || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return 0;
	}
	const int total_lods = MAX( 1, m_pTerrain->getMeshLods() + m_pTerrain->getTessellationLevel() );
	const int detail_lods = nDetailLods <= 0 ? total_lods : CLAMP( nDetailLods, 1, total_lods );
	const int required_core = calculateGpuRegionRequiredCorePagesForLods( detail_lods );
	// Allocation is budget-driven. The unified region policy determines which pages
	// are desirable; legacy GPU guard/prefetch controls must not define a second
	// spatial footprint or reduce the usable physical cache.
	const int requested = MIN( m_regionRepository.getActiveRegionCount(), TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	const int capacity = terrain3dGpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
		m_nGpuRegionCacheBudgetMb, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS, requested, m_bControlMapsImplicit,
		!isGpuRegionColorChannelEnabled(), isGpuRegionDerivedChannelEnabled(), m_pTerrain->getGpuRegionColorResolutionDivisor(),
		m_pTerrain->getGpuRegionDerivedResolutionDivisor(), m_pTerrain->getGpuFarQualityDivisor(),
		m_nGpuRegionSparePages, m_pTerrain->getGpuFarHeightAtlasSide() );
	return MAX( required_core > 0 ? 1 : 0, capacity );
}

int T3DData::calculateGpuRegionCacheCapacity( void ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		const int page_limit = CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16,
			TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS );
		return terrain3dGpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
			calculateAuthoringGpuRegionBudgetMb(), page_limit,
			MIN( m_regionRepository.getActiveRegionCount(), page_limit ), m_bControlMapsImplicit, !isGpuRegionColorChannelEnabled(),
			isGpuRegionDerivedChannelEnabled(),
			m_pTerrain->getGpuRegionColorResolutionDivisor(),
			m_pTerrain->getGpuRegionDerivedResolutionDivisor(),
			m_pTerrain->getGpuFarQualityDivisor(), m_nGpuRegionSparePages,
			m_pTerrain->getGpuFarHeightAtlasSide() );
	}
	const int allocation_request = MIN( m_regionRepository.getActiveRegionCount(), TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
	return terrain3dGpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_regionRepository.getRegionMapSize(),
		m_nGpuRegionCacheBudgetMb, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS,
		allocation_request, m_bControlMapsImplicit, !isGpuRegionColorChannelEnabled(),
		isGpuRegionDerivedChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1, m_nGpuRegionSparePages,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DData::calculateGpuRegionPhysicalLayers( const int nMappedCapacity ) const {
	if ( m_pTerrain && m_pTerrain->isEditorAuthoringMode() ) {
		return terrain3dGpuCacheCalculatePhysicalLayers( nMappedCapacity, m_nGpuRegionSparePages,
			CLAMP( m_pTerrain->getEditorGpuRegionCachePages(), 16, TERRAIN3D_GPU_CACHE_EDITOR_MAX_LAYERS ) );
	}
	return terrain3dGpuCacheCalculatePhysicalLayers( nMappedCapacity,
		m_nGpuRegionSparePages, TERRAIN3D_GPU_CACHE_RUNTIME_MAX_LAYERS );
}

int T3DData::calculateGpuRegionRequiredRadius( void ) const {
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

int T3DData::calculateGpuRegionRequestedPages( void ) const {
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

size_t T3DData::estimateGpuRegionLayerBytes( void ) const {
	return terrain3dGpuCacheEstimateLayerBytes( m_regionRepository.getRegionSize(), m_bControlMapsImplicit,
		!isGpuRegionColorChannelEnabled(), isGpuRegionDerivedChannelEnabled(),
		m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1 );
}

bool T3DData::isGpuRegionColorChannelEnabled( void ) const {
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

bool T3DData::isGpuRegionDerivedChannelEnabled( void ) const {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
	if ( pMaterial.is_null() ) {
		return true;
	}
	return ( m_pTerrain->getRuntimeNearShaderLods() > 0 &&
		pMaterial->getNearTerrainNormalQuality() == T3DMaterial::TERRAIN_NORMAL_QUALITY_DETAILED ) ||
		( m_pTerrain->getRuntimeMiddleShaderLods() > 0 &&
		pMaterial->getMiddleTerrainNormalQuality() == T3DMaterial::TERRAIN_NORMAL_QUALITY_DETAILED );
}

size_t T3DData::estimateGpuRegionSharedBytes( void ) const {
	return terrain3dGpuCacheEstimateSharedBytes( m_regionRepository.getRegionSize(), m_bControlMapsImplicit,
		!isGpuRegionColorChannelEnabled(), m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1,
		m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE );
}

int T3DData::getGpuFarHeightAtlasSide( void ) const {
	return m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
}

int T3DData::getGpuFarSurfaceAtlasSide( void ) const {
	return m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : TERRAIN3D_GPU_CACHE_FAR_HEIGHT_SIDE;
}

size_t T3DData::getGpuFarHeightBytes( void ) const {
	const size_t nSide = ( size_t ) ( getGpuFarHeightAtlasSide() );
	return nSide * nSide * 2u;
}

size_t T3DData::getGpuFarColorBytes( void ) const {
	const size_t nSide = ( size_t ) ( getGpuFarSurfaceAtlasSide() );
	return nSide * nSide * 4u;
}

int T3DData::estimateGpuRegionUploadCalls( const T3DRegion *pRegion, const int nLayer ) const {
	if ( !pRegion || nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ) {
		return 0;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	int calls = 1; // Height is always published.
	if ( isGpuRegionDerivedChannelEnabled() ) {
		++calls;
	}
	if ( !m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL ) ) ) {
		++calls;
	}
	if ( isGpuRegionColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_COLOR ) ) ) {
		++calls;
	}
	return calls;
}

size_t T3DData::estimateGpuRegionUploadBytes( const T3DRegion *pRegion, const int nLayer ) const {
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
	if ( isGpuRegionDerivedChannelEnabled() ) {
		bytes += derived_texels * 2u;
	}
	const uint8_t source_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t layer_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	if ( !m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL ) ) ) {
		bytes += full_texels * 4u;
	}
	if ( isGpuRegionColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
		source_flags, layer_flags, ( uint8_t ) ( T3DRegion::CHANNEL_COLOR ) ) ) {
		bytes += terrain3dGpuCacheColorBytes( m_regionRepository.getRegionSize(),
			m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1 );
	}
	return bytes;
}

int T3DData::getGpuRegionTextureUpdateCallsLastTick( void ) const {
	return m_nGpuRegionHeightUpdatesLastTick + m_nGpuRegionControlUpdatesLastTick +
		m_nGpuRegionColorUpdatesLastTick + m_nGpuRegionDerivedUpdatesLastTick +
		m_nGpuRegionPageTableUpdatesLastTick + m_nGpuFarHeightUpdatesLastTick +
		m_nGpuFarColorUpdatesLastTick;
}

void T3DData::resetGpuRegionCache( const bool bClearTextures ) {
	pollGpuRuntimePagePrepare( true );
	m_runtimePageStore.reset();
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
	m_nGpuFarHeightUpdatesLastTick = 0;
	m_nGpuFarColorUpdatesLastTick = 0;
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
	std::memset( m_szGpuRegionRepublishPending, 0, sizeof( m_szGpuRegionRepublishPending ) );
	m_nGpuRegionRepublishPendingCount = 0;
	m_nGpuRegionRepublishCursor = 0;
	m_bGpuFarHeightDirty = m_bGpuFarHeightDirty || m_pGpuFarHeightImage.is_null();
	m_bGpuFarColorDirty = m_bGpuFarColorDirty || m_pGpuFarColorImage.is_null();
	m_nGpuFarHeightUpdatesLastTick = 0;
	m_nGpuFarColorUpdatesLastTick = 0;
	m_nGpuFarHeightRegionsLastTick = 0;
	m_nGpuFarColorRegionsLastTick = 0;
	m_bGpuRegionCacheTexturesReady = false;
	m_bGpuRegionCacheSelectionDirty = true;
	if ( bClearTextures ) {
		m_generatedHeightMaps.clear();
		m_generatedControlMaps.clear();
		m_generatedColorMaps.clear();
		m_generatedDerivedMaps.clear();
		m_generatedMiddleCompositeMaps.clear();
		if ( !isFarAtlasRequired() ) {
			m_generatedFarHeightMap.clear();
			m_generatedFarColorMap.clear();
		}
	}
}

bool T3DData::rebuildGpuRegionCacheTextures( void ) {
	ensureDefaultMaps();
	const int capacity = calculateGpuRegionCacheCapacity();
	LOG( T3DTerrain::INFO, "[gpu-cache] Rebuild requested: mapped_capacity=", capacity,
		"; region_size=", m_regionRepository.getRegionSize(), "; implicit_control=", m_bControlMapsImplicit,
		"; implicit_color=", m_bColorMapsImplicit );
	if ( capacity <= 0 || m_pDefaultHeightMap.is_null() || m_pDefaultControlMap.is_null() ||
		m_pDefaultColorMap.is_null() || m_pDefaultDerivedMap.is_null() ) {
		return false;
	}

	const Vector2i previous_center = m_gpuRegionCacheCenter;
	resetGpuRegionCache( true );
	m_nGpuRegionCacheCapacity = capacity;
	m_nGpuRegionPhysicalLayerCount = calculateGpuRegionPhysicalLayers( capacity );
	m_nGpuRegionSpareLayerCount = MAX( 0, m_nGpuRegionPhysicalLayerCount - capacity );
	m_gpuRegionCacheCenter = previous_center;
	m_nGpuRegionColorDivisor = m_pTerrain ? m_pTerrain->getGpuRegionColorResolutionDivisor() : 1;
	m_nGpuRegionDerivedDivisor = m_pTerrain ? m_pTerrain->getGpuRegionDerivedResolutionDivisor() : 1;
	const bool bColorChannelEnabled = isGpuRegionColorChannelEnabled();
	const bool bDerivedChannelEnabled = isGpuRegionDerivedChannelEnabled();
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
	// RenderingServer owns the uploaded layers. Retaining these Image arrays would
	// duplicate the physical GPU cache on the CPU side and defeat bounded residency.
	m_gpuHeightMaps.clear();
	m_gpuControlMaps.clear();
	m_gpuColorMaps.clear();
	m_gpuDerivedMaps.clear();
	m_gpuMiddleCompositeMaps.clear();
	m_bGpuRegionCacheTexturesReady = bHeightOk && bControlOk && bColorOk && bDerivedOk && bMiddleCompositeOk;
	if ( m_bGpuRegionCacheTexturesReady ) {
		m_nGpuRegionLastPublicationTick = m_nGpuRegionTick;
	}
	LOG( T3DTerrain::DEBUG, "[gpu-cache] Physical arrays uploaded: height=", bHeightOk, "; control=", bControlOk,
		"; color=", bColorOk, "; derived=", bDerivedOk, "; middle_composite=", bMiddleCompositeOk,
		"; physical_layers=", m_nGpuRegionPhysicalLayerCount );
	if ( !m_bGpuRegionCacheTexturesReady ) {
		LOG( T3DTerrain::ERROR, "Failed to create bounded GPU region cache with ", capacity, " physical layers" );
		return false;
	}
	if ( ( m_pGpuFarHeightImage.is_null() || m_pGpuFarColorImage.is_null() ) &&
		!loadCookedGpuFarAtlases() ) {
		markAllGpuFarRegionsDirty( T3DRegion::TYPE_MAX );
	}
	if ( !m_bGpuFarHeightDirty && m_pGpuFarHeightImage.is_valid() ) {
		if ( !m_generatedFarHeightMap.create( m_pGpuFarHeightImage ).is_valid() ) {
			return false;
		}
	} else if ( !rebuildGpuFarHeightAtlas() ) {
		LOG( T3DTerrain::ERROR, "Failed to create the coarse whole-world terrain height atlas" );
		return false;
	}
	if ( !m_bGpuFarColorDirty && m_pGpuFarColorImage.is_valid() ) {
		if ( !m_generatedFarColorMap.create( m_pGpuFarColorImage ).is_valid() ) {
			return false;
		}
	} else if ( !rebuildGpuFarColorAtlas() ) {
		LOG( T3DTerrain::ERROR, "Failed to create the coarse whole-world terrain color atlas" );
		return false;
	}
	const size_t budget_bytes = ( size_t ) ( m_nGpuRegionCacheBudgetMb ) * TERRAIN3D_GPU_CACHE_MIB;
	const size_t layer_bytes = estimateGpuRegionLayerBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t estimated_bytes = shared_bytes + layer_bytes * ( size_t ) ( m_nGpuRegionPhysicalLayerCount );
	if ( m_pTerrain ) {
		const Ref<T3DMaterial> pMaterial = m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() && pMaterial->isInitializedFor( m_pTerrain ) ) {
			pMaterial->updateFarAtlasTextures();
		}
	}
	if ( estimated_bytes > budget_bytes ) {
		LOG( T3DTerrain::WARN, "One terrain region requires approximately ", layer_bytes,
			" GPU bytes and shared channels require ", shared_bytes,
			" bytes. The selected cache exceeds the configured budget of ", budget_bytes,
			" bytes because one physical layer is the minimum legal configuration." );
	}
	m_nGpuRegionRequestedPages = calculateGpuRegionRequestedPages();
	m_nGpuRegionRequiredRadius = calculateGpuRegionRequiredRadius();
	LOG( T3DTerrain::INFO, "[gpu-cache] Created footprint-driven GPU region cache: ", capacity, " mapped pages, ",
		m_nGpuRegionSpareLayerCount, " spare/retirement pages, ",
		m_nGpuRegionPhysicalLayerCount, " physical layers, requested ", m_nGpuRegionRequestedPages, ", required radius ", m_nGpuRegionRequiredRadius,
		", estimated bytes ", estimated_bytes, ", budget ", budget_bytes );
	return true;
}

String T3DData::getGpuFarAtlasPath( const String &rDirectory ) const {
	const String directory = !rDirectory.is_empty() ? rDirectory :
		( m_pTerrain ? m_pTerrain->getDataDirectory() : String() );
	if ( directory.is_empty() ) {
		return String();
	}
	return directory.path_join( "terrain3d_far_atlas.t3dfar" );
}

uint64_t T3DData::computeGpuFarAtlasSourceSignature( void ) const {
	uint64_t hash = 1469598103934665603ull;
	terrain3dFnv1a64AppendU32( hash, TERRAIN3D_GPU_FAR_ATLAS_VERSION );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_regionRepository.getRegionSize() ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_regionRepository.getRegionMapSize() ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( getGpuFarHeightAtlasSide() ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( getGpuFarSurfaceAtlasSide() ) );
	terrain3dFnv1a64AppendF32( hash, ( float ) ( m_regionRepository.getVertexSpacing() ) );

	// Iterate the fixed spatial table rather than region slot order so the same
	// persisted world produces the same signature regardless of load/allocation order.
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( m_regionRepository.getActiveRegionCount() ) );
	for ( int map_index = 0; map_index < TERRAIN3D_MAX_REGION_SLOTS; ++map_index ) {
		const Vector2i location(
			map_index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
			map_index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
		);
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( map_index ) );
		const String path = pRegion->get_path();
		const uint64_t modified = !path.is_empty() && FileAccess::file_exists( path ) ?
			FileAccess::get_modified_time( path ) : 0u;
		terrain3dFnv1a64AppendU64( hash, modified );
	}

	t3dfarSurfaceBakeData_t bake_data;
	terrain3dBuildFarSurfaceBakeData( m_pTerrain, bake_data );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( bake_data.nCount ) );
	terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( bake_data.fallback_texture ) );
	for ( int index = 0; index < bake_data.nCount; ++index ) {
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( index ) );
		terrain3dFnv1a64AppendU32( hash, ( uint32_t ) ( bake_data.populated[( size_t ) ( index )] ) );
	}

	const Ref<T3DMaterial> pMaterialRef = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	T3DMaterial *const pMaterial = pMaterialRef.ptr();
	terrain3dFnv1a64AppendU32( hash, pMaterial ? 1u : 0u );
	if ( pMaterial ) {
		terrain3dFnv1a64AppendU32( hash, pMaterial->getAutoShaderEnabled() ? 1u : 0u );
		terrain3dFnv1a64AppendF32( hash, terrain3dMaterialFloatParam( pMaterial, "auto_slope", 1.0f ) );
		terrain3dFnv1a64AppendF32( hash,
			terrain3dMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f ) );
		terrain3dFnv1a64AppendU32( hash,
			( uint32_t ) ( terrain3dMaterialIntParam( pMaterial, "auto_base_texture", 0 ) ) );
		terrain3dFnv1a64AppendU32( hash,
			( uint32_t ) ( terrain3dMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ) ) );
	}
	return hash;
}

bool T3DData::isFarAtlasRequired( void ) const {
	return m_bGpuRegionCacheEnabled || ( m_pTerrain && m_pTerrain->isRuntimeShaderTieringActive() );
}

bool T3DData::initializeGpuFarAtlasImages( void ) {
	const int nHeightAtlasSize = getGpuFarHeightAtlasSide();
	const int nSurfaceAtlasSize = getGpuFarSurfaceAtlasSide();
	const int nHeightTexelCount = nHeightAtlasSize * nHeightAtlasSize;
	const int nSurfaceTexelCount = nSurfaceAtlasSize * nSurfaceAtlasSize;
	if ( m_pGpuFarHeightImage.is_null() || m_pGpuFarHeightImage->get_width() != nHeightAtlasSize ||
		m_pGpuFarHeightImage->get_height() != nHeightAtlasSize ||
		m_pGpuFarHeightImage->get_format() != Image::FORMAT_R16 ) {
		const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
		PackedByteArray height_bytes;
		height_bytes.resize( ( int64_t ) ( nHeightTexelCount ) * 2 );
		uint8_t *const pWrite = height_bytes.ptrw();
		for ( int index = 0; index < nHeightTexelCount; ++index ) {
			std::memcpy( pWrite + ( size_t ) ( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		m_pGpuFarHeightImage = Image::create_from_data( nHeightAtlasSize, nHeightAtlasSize, false,
			Image::FORMAT_R16, height_bytes );
	}
	if ( m_pGpuFarColorImage.is_null() || m_pGpuFarColorImage->get_width() != nSurfaceAtlasSize ||
		m_pGpuFarColorImage->get_height() != nSurfaceAtlasSize ||
		m_pGpuFarColorImage->get_format() != Image::FORMAT_RGBA8 ) {
		PackedByteArray color_bytes;
		color_bytes.resize( ( int64_t ) ( nSurfaceTexelCount ) * 4 );
		terrain3dFillFarSurfaceEmpty( color_bytes );
		m_pGpuFarColorImage = Image::create_from_data( nSurfaceAtlasSize, nSurfaceAtlasSize, false,
			Image::FORMAT_RGBA8, color_bytes );
	}
	return m_pGpuFarHeightImage.is_valid() && !m_pGpuFarHeightImage->is_empty() &&
		m_pGpuFarColorImage.is_valid() && !m_pGpuFarColorImage->is_empty();
}

bool T3DData::loadCookedGpuFarAtlases( void ) {
	const String path = getGpuFarAtlasPath();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		return false;
	}
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	const Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() || pFile->get_length() < TERRAIN3D_GPU_FAR_ATLAS_HEADER_BYTES ) {
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
	const int nExpectedHeightAtlasSize = getGpuFarHeightAtlasSide();
	const int nExpectedSurfaceAtlasSize = getGpuFarSurfaceAtlasSide();
	const uint32_t expected_height_size = ( uint32_t ) ( nExpectedHeightAtlasSize * nExpectedHeightAtlasSize * 2 );
	const uint32_t expected_color_size = ( uint32_t ) ( nExpectedSurfaceAtlasSize * nExpectedSurfaceAtlasSize * 4 );
	if ( magic != TERRAIN3D_GPU_FAR_ATLAS_MAGIC || version != TERRAIN3D_GPU_FAR_ATLAS_VERSION ||
		region_size != ( uint32_t ) ( m_regionRepository.getRegionSize() ) || region_map_size != ( uint32_t ) ( m_regionRepository.getRegionMapSize() ) ||
		height_atlas_size != ( uint32_t ) ( nExpectedHeightAtlasSize ) ||
		surface_atlas_size != ( uint32_t ) ( nExpectedSurfaceAtlasSize ) ||
		height_size != expected_height_size || color_size != expected_color_size ||
		pFile->get_length() != TERRAIN3D_GPU_FAR_ATLAS_HEADER_BYTES + ( uint64_t ) ( height_size ) +
			( uint64_t ) ( color_size ) ) {
		return false;
	}
	const uint64_t expected_source_signature = computeGpuFarAtlasSourceSignature();
	if ( source_signature == 0u || source_signature != expected_source_signature ) {
		LOG( T3DTerrain::INFO, "[far-cache] Rejecting cooked atlas with stale source signature: cached=",
			source_signature, ", current=", expected_source_signature );
		return false;
	}
	const PackedByteArray height_bytes = pFile->get_buffer( height_size );
	const PackedByteArray color_bytes = pFile->get_buffer( color_size );
	if ( height_bytes.size() != height_size || color_bytes.size() != color_size ||
		terrain3dRuntimePageChecksum( height_bytes.ptr(), ( size_t ) ( height_bytes.size() ) ) != height_checksum ||
		terrain3dRuntimePageChecksum( color_bytes.ptr(), ( size_t ) ( color_bytes.size() ) ) != color_checksum ) {
		return false;
	}
	m_pGpuFarHeightImage = Image::create_from_data( nExpectedHeightAtlasSize,
		nExpectedHeightAtlasSize, false, Image::FORMAT_R16, height_bytes );
	m_pGpuFarColorImage = Image::create_from_data( nExpectedSurfaceAtlasSize,
		nExpectedSurfaceAtlasSize, false, Image::FORMAT_RGBA8, color_bytes );
	if ( m_pGpuFarHeightImage.is_null() || m_pGpuFarColorImage.is_null() ) {
		return false;
	}
	m_bGpuFarHeightDirty = false;
	m_bGpuFarColorDirty = false;
	m_bGpuFarHeightDirtyAll = false;
	m_bGpuFarColorDirtyAll = false;
	m_gpuFarHeightDirtyRegions.reset();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarCookedLoaded = true;
	m_bGpuFarCookedSaved = false;
	m_nGpuFarCookIoUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	return true;
}

bool T3DData::writeCookedGpuFarAtlases( const String &rDirectory ) {
	const String path = getGpuFarAtlasPath( rDirectory );
	if ( path.is_empty() ) {
		return false;
	}
	// A loaded/saved clean atlas is already authoritative. Do not rewrite a large
	// generated file merely because the scene itself was saved.
	if ( !m_bGpuFarHeightDirty && !m_bGpuFarColorDirty &&
		( m_bGpuFarCookedLoaded || m_bGpuFarCookedSaved ) && FileAccess::file_exists( path ) ) {
		return true;
	}
	if ( !initializeGpuFarAtlasImages() ) {
		return false;
	}
	if ( m_bGpuFarHeightDirty && !rebuildGpuFarHeightAtlas() ) {
		return false;
	}
	if ( m_bGpuFarColorDirty && !rebuildGpuFarColorAtlas() ) {
		return false;
	}
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	const PackedByteArray height_bytes = m_pGpuFarHeightImage->get_data();
	const PackedByteArray color_bytes = m_pGpuFarColorImage->get_data();
	const uint64_t source_signature = computeGpuFarAtlasSourceSignature();
	const String temporary_path = path + String( ".tmp" );
	const String backup_path = path + String( ".bak" );
	if ( FileAccess::file_exists( temporary_path ) ) {
		DirAccess::remove_absolute( temporary_path );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary_path, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		return false;
	}
	pFile->store_64( TERRAIN3D_GPU_FAR_ATLAS_MAGIC );
	pFile->store_32( TERRAIN3D_GPU_FAR_ATLAS_VERSION );
	pFile->store_32( ( uint32_t ) ( m_regionRepository.getRegionSize() ) );
	pFile->store_32( ( uint32_t ) ( m_regionRepository.getRegionMapSize() ) );
	pFile->store_32( ( uint32_t ) ( getGpuFarHeightAtlasSide() ) );
	pFile->store_32( ( uint32_t ) ( getGpuFarSurfaceAtlasSide() ) );
	pFile->store_32( ( uint32_t ) ( height_bytes.size() ) );
	pFile->store_32( ( uint32_t ) ( color_bytes.size() ) );
	pFile->store_32( terrain3dRuntimePageChecksum( height_bytes.ptr(), ( size_t ) ( height_bytes.size() ) ) );
	pFile->store_32( terrain3dRuntimePageChecksum( color_bytes.ptr(), ( size_t ) ( color_bytes.size() ) ) );
	pFile->store_64( source_signature );
	pFile->store_buffer( height_bytes );
	pFile->store_buffer( color_bytes );
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return false;
	}
	const bool bHadPrevious = FileAccess::file_exists( path );
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	if ( bHadPrevious && DirAccess::rename_absolute( path, backup_path ) != OK ) {
		DirAccess::remove_absolute( temporary_path );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary_path, path ) != OK ) {
		if ( bHadPrevious ) {
			DirAccess::rename_absolute( backup_path, path );
		}
		return false;
	}
	if ( FileAccess::file_exists( backup_path ) ) {
		DirAccess::remove_absolute( backup_path );
	}
	m_bGpuFarCookedSaved = true;
	m_bGpuFarCookedLoaded = false;
	if ( IS_EDITOR && path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( path );
		}
	}
	m_nGpuFarCookIoUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	return true;
}

void T3DData::markAllGpuFarRegionsDirty( const t3dmapType_t eMapType ) {
	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarHeightDirty = true;
		m_bGpuFarHeightDirtyAll = true;
		m_gpuFarHeightDirtyRegions.reset();
	}
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarColorDirty = true;
		m_bGpuFarColorDirtyAll = true;
		m_gpuFarColorDirtyRegions.reset();
	}
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
}

void T3DData::markGpuFarRegionDirty( const Vector2i &vLocation, const t3dmapType_t eMapType ) {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return;
	}
	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarHeightDirty = true;
		if ( !m_bGpuFarHeightDirtyAll ) {
			m_gpuFarHeightDirtyRegions.set( ( size_t ) ( map_index ) );
		}
	}
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) {
		m_bGpuFarColorDirty = true;
		if ( !m_bGpuFarColorDirtyAll ) {
			m_gpuFarColorDirtyRegions.set( ( size_t ) ( map_index ) );
		}
	}
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
}

bool T3DData::patchGpuFarHeightRegion(
	const Vector2i &vLocation,
	PackedByteArray &nAtlasBytes
) {
	const int nAtlasSize = getGpuFarHeightAtlasSide();
	if ( m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ||
		nAtlasSize % m_regionRepository.getRegionMapSize() != 0 ) {
		return false;
	}
	const int samples_per_region = nAtlasSize / m_regionRepository.getRegionMapSize();
	if ( samples_per_region <= 0 || m_regionRepository.getRegionSize() % samples_per_region != 0 ) {
		return false;
	}
	const int atlas_half = m_regionRepository.getRegionMapSize() / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( atlas_origin_x < 0 || atlas_origin_y < 0 ||
		atlas_origin_x + samples_per_region > nAtlasSize ||
		atlas_origin_y + samples_per_region > nAtlasSize ) {
		return false;
	}
	const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
	const uint16_t *height = nullptr;
	PackedByteArray region_height_bytes;
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	T3DActiveMapLease lease;
	if ( pRegion && !pRegion->isDeleted() ) {
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( vLocation, true ) ) {
			return false;
		}
		lease = m_samplingStorage.acquireActive( vLocation );
		if ( lease.valid() ) {
			height = lease.view().pHeight;
		} else {
			const Ref<Image> pHeight = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
			if ( pHeight.is_valid() && pHeight->get_format() == Image::FORMAT_R16 ) {
				region_height_bytes = pHeight->get_data();
				if ( region_height_bytes.size() >= ( int64_t ) ( m_regionRepository.getRegionSize() ) * m_regionRepository.getRegionSize() * 2 ) {
					height = ( const uint16_t * ) ( region_height_bytes.ptr() );
				}
			}
		}
	}
	const int source_step = m_regionRepository.getRegionSize() / samples_per_region;
	uint8_t *const pWrite = nAtlasBytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int y = 0; y < samples_per_region; ++y ) {
		const int source_y = MIN( m_regionRepository.getRegionSize() - 1, y * source_step );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int source_x = MIN( m_regionRepository.getRegionSize() - 1, x * source_step );
			const uint16_t encoded = height ? height[source_y * m_regionRepository.getRegionSize() + source_x] : default_height;
			const int atlas_index = ( atlas_origin_y + y ) * nAtlasSize + atlas_origin_x + x;
			std::memcpy( pWrite + ( size_t ) ( atlas_index ) * 2u, &encoded, sizeof( encoded ) );
		}
	}
	return true;
}

bool T3DData::patchGpuFarColorRegion(
	const Vector2i &vLocation,
	PackedByteArray &nAtlasBytes, const t3dfarSurfaceBakeData_t &rBakeData
) {
	const int nAtlasSize = getGpuFarSurfaceAtlasSide();
	if ( m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ||
		nAtlasSize % m_regionRepository.getRegionMapSize() != 0 ) {
		return false;
	}
	const int samples_per_region = nAtlasSize / m_regionRepository.getRegionMapSize();
	const int atlas_half = m_regionRepository.getRegionMapSize() / 2;
	const int atlas_origin_x = ( vLocation.x + atlas_half ) * samples_per_region;
	const int atlas_origin_y = ( vLocation.y + atlas_half ) * samples_per_region;
	if ( samples_per_region <= 0 || atlas_origin_x < 0 || atlas_origin_y < 0 ||
		atlas_origin_x + samples_per_region > nAtlasSize ||
		atlas_origin_y + samples_per_region > nAtlasSize ) {
		return false;
	}
	uint8_t *pAtlasWrite = nAtlasBytes.ptrw();
	if ( !pAtlasWrite ) {
		return false;
	}
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		for ( int y = 0; y < samples_per_region; ++y ) {
			for ( int x = 0; x < samples_per_region; ++x ) {
				const int atlas_index = ( atlas_origin_y + y ) * nAtlasSize + atlas_origin_x + x;
				uint8_t *pixel = pAtlasWrite + ( size_t ) ( atlas_index ) * 4u;
				pixel[0] = 255u;
				pixel[1] = 255u;
				pixel[2] = 255u;
				pixel[3] = 0u;
			}
		}
		return true;
	}
	if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( vLocation, true ) ) {
		return false;
	}
	const Ref<Image> pSourceColor = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR );
	const Ref<Image> pSourceControl = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_CONTROL );
	const Ref<Image> pSourceHeight = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
	const Ref<Image> pSourceDerived = pRegion->getDerivedNormalSlopeMap();
	if ( pSourceColor.is_null() || pSourceControl.is_null() ||
		pSourceColor->get_format() != Image::FORMAT_RGBA8 ||
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
	const bool bHeightValid = pSourceHeight.is_valid() && pSourceHeight->get_format() == Image::FORMAT_R16 &&
		height_bytes.size() >= ( int64_t ) ( m_regionRepository.getRegionSize() ) * m_regionRepository.getRegionSize() * 2;
	const int derived_width = pSourceDerived.is_valid() ? pSourceDerived->get_width() : 0;
	const int derived_height = pSourceDerived.is_valid() ? pSourceDerived->get_height() : 0;
	const bool bDerivedValid = pSourceDerived.is_valid() && pSourceDerived->get_format() == Image::FORMAT_RG8 &&
		derived_width > 0 && derived_height > 0 &&
		derived_bytes.size() >= ( int64_t ) ( derived_width ) * derived_height * 2;
	if ( !pColorRead || !pControlRead || color_width <= 0 || color_height <= 0 ||
		control_bytes.size() < ( int64_t ) ( m_regionRepository.getRegionSize() ) * m_regionRepository.getRegionSize() * 4 ) {
		return false;
	}

	const Ref<T3DMaterial> pMaterialRef = m_pTerrain ? m_pTerrain->getMaterial() : Ref<T3DMaterial>();
	T3DMaterial *const pMaterial = pMaterialRef.ptr();
	const bool bAutoShaderEnabled = pMaterial && pMaterial->getAutoShaderEnabled();
	const float fAutoSlope = terrain3dMaterialFloatParam( pMaterial, "auto_slope", 1.0f );
	const float fAutoHeightReduction = terrain3dMaterialFloatParam( pMaterial, "auto_height_reduction", 0.1f );
	const int auto_layer_max = MAX( rBakeData.nCount - 1, 0 );
	const int auto_base_texture = CLAMP( terrain3dMaterialIntParam( pMaterial, "auto_base_texture", 0 ), 0, auto_layer_max );
	const int auto_overlay_texture = CLAMP( terrain3dMaterialIntParam( pMaterial, "auto_overlay_texture", 1 ), 0, auto_layer_max );
	const int source_step = MAX( 1, m_regionRepository.getRegionSize() / samples_per_region );

	for ( int y = 0; y < samples_per_region; ++y ) {
		const int region_y0 = y * m_regionRepository.getRegionSize() / samples_per_region;
		const int region_y1 = MAX( region_y0 + 1, ( y + 1 ) * m_regionRepository.getRegionSize() / samples_per_region );
		const int color_y0 = y * color_height / samples_per_region;
		const int color_y1 = MAX( color_y0 + 1, ( y + 1 ) * color_height / samples_per_region );
		for ( int x = 0; x < samples_per_region; ++x ) {
			const int region_x0 = x * m_regionRepository.getRegionSize() / samples_per_region;
			const int region_x1 = MAX( region_x0 + 1, ( x + 1 ) * m_regionRepository.getRegionSize() / samples_per_region );
			const int color_x0 = x * color_width / samples_per_region;
			const int color_x1 = MAX( color_x0 + 1, ( x + 1 ) * color_width / samples_per_region );
			uint64_t sum_r = 0u;
			uint64_t sum_g = 0u;
			uint64_t sum_b = 0u;
			uint64_t sum_a = 0u;
			int color_samples = 0;
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
			const int center_x = MIN( m_regionRepository.getRegionSize() - 1, region_x0 + ( region_x1 - region_x0 ) / 2 );
			const int center_y = MIN( m_regionRepository.getRegionSize() - 1, region_y0 + ( region_y1 - region_y0 ) / 2 );
			const int center_index = center_y * m_regionRepository.getRegionSize() + center_x;
			const uint32_t control = terrain3dReadU32Unaligned( pControlRead, ( size_t ) ( center_index ) * 4u );
			float fSolidCoverage = 0.0f;
			for ( int probe_y = 0; probe_y < 2; ++probe_y ) {
				for ( int probe_x = 0; probe_x < 2; ++probe_x ) {
					const int px = MIN( m_regionRepository.getRegionSize() - 1, region_x0 + ( ( probe_x * 2 + 1 ) * source_step ) / 4 );
					const int py = MIN( m_regionRepository.getRegionSize() - 1, region_y0 + ( ( probe_y * 2 + 1 ) * source_step ) / 4 );
					const uint32_t probe_control = terrain3dReadU32Unaligned( pControlRead,
						( size_t ) ( py * m_regionRepository.getRegionSize() + px ) * 4u );
					fSolidCoverage += isHole( probe_control ) ? 0.0f : 0.25f;
				}
			}

			int base_id = ( int ) ( getBase( control ) );
			int overlay_id = ( int ) ( getOverlay( control ) );
			float fBlend = ( float ) ( getBlend( control ) ) / TERRAIN3D_BYTE_MAX_FLOAT;
			if ( bAutoShaderEnabled && isAuto( control ) ) {
				float fNormalY = 1.0f;
				float fHeight = 0.0f;
				if ( bHeightValid ) {
					fHeight = Terrain3DRegionCodec::decodeHeightU16( terrain3dReadU16Unaligned(
						pHeightRead, ( size_t ) ( center_index ) * 2u ) );
				}
				if ( bDerivedValid ) {
					const int derived_x = MIN( derived_width - 1, center_x * derived_width / m_regionRepository.getRegionSize() );
					const int derived_y = MIN( derived_height - 1, center_y * derived_height / m_regionRepository.getRegionSize() );
					const size_t derived_offset = ( size_t ) ( derived_y * derived_width + derived_x ) * 2u;
					const float fNormalX = ( float ) ( pDerivedRead[derived_offset] ) / TERRAIN3D_BYTE_MAX_FLOAT * 2.0f - 1.0f;
					const float fNormalZ = ( float ) ( pDerivedRead[derived_offset + 1u] ) / TERRAIN3D_BYTE_MAX_FLOAT * 2.0f - 1.0f;
					fNormalY = Math::sqrt( MAX( 1.0f - fNormalX * fNormalX - fNormalZ * fNormalZ, 0.0f ) );
				} else if ( bHeightValid ) {
					const int sample_x = center_y * m_regionRepository.getRegionSize() + MIN( m_regionRepository.getRegionSize() - 1, center_x + 1 );
					const int sample_z = MIN( m_regionRepository.getRegionSize() - 1, center_y + 1 ) * m_regionRepository.getRegionSize() + center_x;
					const float fHeightX = Terrain3DRegionCodec::decodeHeightU16( terrain3dReadU16Unaligned(
						pHeightRead, ( size_t ) ( sample_x ) * 2u ) );
					const float fHeightZ = Terrain3DRegionCodec::decodeHeightU16( terrain3dReadU16Unaligned(
						pHeightRead, ( size_t ) ( sample_z ) * 2u ) );
					const float fSpacing = MAX( ( float ) ( m_regionRepository.getVertexSpacing() ), 0.0001f );
					const float fDx = fHeightX - fHeight;
					const float fDz = fHeightZ - fHeight;
					fNormalY = fSpacing / Math::sqrt( fDx * fDx + fSpacing * fSpacing + fDz * fDz );
				}
				base_id = auto_base_texture;
				overlay_id = auto_overlay_texture;
				fBlend = CLAMP( fAutoSlope * 2.0f * ( fNormalY - 1.0f ) + 1.0f -
					fAutoHeightReduction * 0.01f * fHeight, 0.0f, 1.0f );
			}
			base_id = terrain3dFarResolveMaterialId( rBakeData, base_id );
			overlay_id = terrain3dFarResolveMaterialId( rBakeData, overlay_id );
			const int dominant_material = fBlend >= 0.5f ? overlay_id : base_id;
			const float fInvSampleCount = color_samples > 0 ? 1.0f / ( float ) ( color_samples ) : 0.0f;
			const float fSurfaceR = ( float ) ( sum_r ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT;
			const float fSurfaceG = ( float ) ( sum_g ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT;
			const float fSurfaceB = ( float ) ( sum_b ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT;
			const float fSurfaceAlpha = color_samples > 0 ? ( float ) ( sum_a ) * fInvSampleCount / TERRAIN3D_BYTE_MAX_FLOAT : 0.5f;
			const int atlas_index = ( atlas_origin_y + y ) * nAtlasSize + atlas_origin_x + x;
			const size_t atlas_offset = ( size_t ) ( atlas_index ) * 4u;
			pAtlasWrite[atlas_offset] = terrain3dColorToByte( fSurfaceR );
			pAtlasWrite[atlas_offset + 1u] = terrain3dColorToByte( fSurfaceG );
			pAtlasWrite[atlas_offset + 2u] = terrain3dColorToByte( fSurfaceB );
			pAtlasWrite[atlas_offset + 3u] = terrain3dFarSurfaceAlpha(
				fSolidCoverage, fSurfaceAlpha, dominant_material );
		}
	}
	return true;
}

bool T3DData::rebuildGpuFarHeightAtlas( void ) {
	if ( !isFarAtlasRequired() || !m_samplingStorage.isActiveOwner() || !initializeGpuFarAtlasImages() ) {
		return false;
	}
	const int nAtlasSize = getGpuFarHeightAtlasSide();
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	PackedByteArray atlas_bytes = m_pGpuFarHeightImage->get_data();
	m_nGpuFarHeightRegionsLastTick = 0;
	m_nGpuFarHeightDirtyTexelsLast = 0u;
	m_nGpuFarHeightRebuiltBytesLast = 0u;
	m_nGpuFarHeightUploadedBytesLast = 0u;
	m_nGpuFarHeightPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t ) ( nAtlasSize ) * ( uint64_t ) ( nAtlasSize );
	const int samples_per_region = m_regionRepository.getRegionMapSize() > 0 ? nAtlasSize / m_regionRepository.getRegionMapSize() : 0;
	const uint64_t region_texels = samples_per_region > 0 ? ( uint64_t ) ( samples_per_region ) * ( uint64_t ) ( samples_per_region ) : 0u;
	if ( m_bGpuFarHeightDirtyAll ) {
		m_nGpuFarHeightDirtyTexelsLast = atlas_texels;
		const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
		uint8_t *const pWrite = atlas_bytes.ptrw();
		for ( int index = 0; index < nAtlasSize * nAtlasSize; ++index ) {
			std::memcpy( pWrite + ( size_t ) ( index ) * 2u, &default_height, sizeof( default_height ) );
		}
		for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
			const Vector2i location = m_regionRepository.getSlotsData()[slot];
			if ( location != V2I_MAX && patchGpuFarHeightRegion( location, atlas_bytes ) ) {
				++m_nGpuFarHeightRegionsLastTick;
			}
		}
	} else {
		m_nGpuFarHeightDirtyTexelsLast = ( uint64_t ) ( m_gpuFarHeightDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
			if ( !m_gpuFarHeightDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location(
				( int ) ( index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
				( int ) ( index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
			);
			if ( patchGpuFarHeightRegion( location, atlas_bytes ) ) {
				++m_nGpuFarHeightRegionsLastTick;
			}
		}
	}
	m_nGpuFarHeightRebuiltBytesLast = m_bGpuFarHeightDirtyAll ? ( uint64_t ) ( atlas_bytes.size() ) :
		( uint64_t ) ( m_nGpuFarHeightRegionsLastTick ) * region_texels * 2u;
	m_pGpuFarHeightImage->set_data( nAtlasSize, nAtlasSize,
		false, Image::FORMAT_R16, atlas_bytes );
	m_nGpuFarHeightProcessingUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	const uint64_t publication_started_usec = Time::get_singleton()->get_ticks_usec();
	const bool bUploaded = m_generatedFarHeightMap.getRid().is_valid() ?
		m_generatedFarHeightMap.update2d( m_pGpuFarHeightImage ) :
		m_generatedFarHeightMap.create( m_pGpuFarHeightImage ).is_valid();
	m_nGpuFarHeightPublicationUsecLast = Time::get_singleton()->get_ticks_usec() - publication_started_usec;
	if ( !bUploaded ) {
		return false;
	}
	m_nGpuFarHeightUploadedBytesLast = ( uint64_t ) ( atlas_bytes.size() );
	m_nGpuFarHeightUploadedBytesTotal += m_nGpuFarHeightUploadedBytesLast;
	++m_nGpuFarHeightPublicationCount;
	m_bGpuFarHeightDirty = false;
	m_bGpuFarHeightDirtyAll = false;
	m_gpuFarHeightDirtyRegions.reset();
	++m_nGpuFarHeightUpdatesLastTick;
	return true;
}

bool T3DData::rebuildGpuFarColorAtlas( void ) {
	if ( !isFarAtlasRequired() || !initializeGpuFarAtlasImages() ) {
		return false;
	}
	const int nAtlasSize = getGpuFarSurfaceAtlasSide();
	const uint64_t started_usec = Time::get_singleton()->get_ticks_usec();
	PackedByteArray atlas_bytes = m_pGpuFarColorImage->get_data();
	t3dfarSurfaceBakeData_t bake_data;
	terrain3dBuildFarSurfaceBakeData( m_pTerrain, bake_data );
	m_nGpuFarColorRegionsLastTick = 0;
	m_nGpuFarColorDirtyTexelsLast = 0u;
	m_nGpuFarColorRebuiltBytesLast = 0u;
	m_nGpuFarColorUploadedBytesLast = 0u;
	m_nGpuFarColorPublicationUsecLast = 0u;
	const uint64_t atlas_texels = ( uint64_t ) ( nAtlasSize ) * ( uint64_t ) ( nAtlasSize );
	const int samples_per_region = m_regionRepository.getRegionMapSize() > 0 ? nAtlasSize / m_regionRepository.getRegionMapSize() : 0;
	const uint64_t region_texels = samples_per_region > 0 ? ( uint64_t ) ( samples_per_region ) * ( uint64_t ) ( samples_per_region ) : 0u;
	if ( m_bGpuFarColorDirtyAll ) {
		m_nGpuFarColorDirtyTexelsLast = atlas_texels;
		terrain3dFillFarSurfaceEmpty( atlas_bytes );
		for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
			const Vector2i location = m_regionRepository.getSlotsData()[slot];
			if ( location != V2I_MAX && patchGpuFarColorRegion( location, atlas_bytes, bake_data ) ) {
				++m_nGpuFarColorRegionsLastTick;
			}
		}
	} else {
		m_nGpuFarColorDirtyTexelsLast = ( uint64_t ) ( m_gpuFarColorDirtyRegions.count() ) * region_texels;
		for ( size_t index = 0; index < TERRAIN3D_MAX_REGION_SLOTS; ++index ) {
			if ( !m_gpuFarColorDirtyRegions.test( index ) ) {
				continue;
			}
			const Vector2i location(
				( int ) ( index % TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2,
				( int ) ( index / TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE ) - TERRAIN3D_ACTIVE_MAX_REGION_MAP_SIZE / 2
			);
			if ( patchGpuFarColorRegion( location, atlas_bytes, bake_data ) ) {
				++m_nGpuFarColorRegionsLastTick;
			}
		}
	}
	m_nGpuFarColorRebuiltBytesLast = m_bGpuFarColorDirtyAll ? ( uint64_t ) ( atlas_bytes.size() ) :
		( uint64_t ) ( m_nGpuFarColorRegionsLastTick ) * region_texels * 4u;
	m_pGpuFarColorImage->set_data( nAtlasSize, nAtlasSize,
		false, Image::FORMAT_RGBA8, atlas_bytes );
	m_nGpuFarColorProcessingUsecLast = Time::get_singleton()->get_ticks_usec() - started_usec;
	const uint64_t publication_started_usec = Time::get_singleton()->get_ticks_usec();
	const bool bUploaded = m_generatedFarColorMap.getRid().is_valid() ?
		m_generatedFarColorMap.update2d( m_pGpuFarColorImage ) :
		m_generatedFarColorMap.create( m_pGpuFarColorImage ).is_valid();
	m_nGpuFarColorPublicationUsecLast = Time::get_singleton()->get_ticks_usec() - publication_started_usec;
	if ( !bUploaded ) {
		return false;
	}
	m_nGpuFarColorUploadedBytesLast = ( uint64_t ) ( atlas_bytes.size() );
	m_nGpuFarColorUploadedBytesTotal += m_nGpuFarColorUploadedBytesLast;
	++m_nGpuFarColorPublicationCount;
	m_bGpuFarColorDirty = false;
	m_bGpuFarColorDirtyAll = false;
	m_gpuFarColorDirtyRegions.reset();
	++m_nGpuFarColorUpdatesLastTick;
	return true;
}

int T3DData::findGpuRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
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

int T3DData::findFreeGpuRegionSlot( void ) const {
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		if ( m_szGpuRegionLocations[layer] == V2I_MAX &&
			m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE ) {
			return layer;
		}
	}
	return -1;
}

void T3DData::recordGpuRegionDeparture( const int nLayer, const bool bCountUnusedPrefetch ) {
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

void T3DData::retireGpuRegionSlot( const int nLayer ) {
	if ( nLayer < 0 || nLayer >= m_nGpuRegionPhysicalLayerCount ||
		m_szGpuRegionSlotStates[nLayer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
		return;
	}
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_RETIRED;
	m_szGpuRegionRetireAfterFrame[nLayer] = std::numeric_limits<uint64_t>::max();
	++m_nGpuRegionRetiredCount;
}

void T3DData::armPendingGpuRegionRetirements( void ) {
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

void T3DData::reclaimRetiredGpuRegionSlots( void ) {
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

void T3DData::invalidateGpuRegionLocation( const Vector2i &vLocation ) {
	if ( !m_bGpuRegionCacheEnabled ) {
		return;
	}
	const int layer = findGpuRegionSlot( vLocation );
	if ( layer < 0 ) {
		return;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szGpuRegionMap[map_index] = 0u;
	}
	recordGpuRegionDeparture( layer, true );
	retireGpuRegionSlot( layer );
	m_nGpuRegionResidentCount = MAX( 0, m_nGpuRegionResidentCount - 1 );
	m_bGpuRegionCacheSelectionDirty = true;
}

bool T3DData::uploadGpuRegionLayer(
	const int nLayer, const Vector2i &vLocation,
	const t3dmapType_t eMapType
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
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	ensureDefaultMaps();

	const uint8_t source_channel_flags = ( uint8_t ) ( pRegion->getChannelFlags() );
	const uint8_t previous_channel_flags = m_szGpuRegionLayerChannelFlags[nLayer];
	const bool bUploadHeight = eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
	// Optional channels are updated only when either the incoming page or the
	// previous physical contents are explicit. Reusing default-on-default layers
	// requires no texture update, while explicit-to-default transitions still clear
	// stale physical contents before publication.
	const bool bUploadControl = ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX ) &&
		!m_bControlMapsImplicit && terrain3dGpuCacheOptionalChannelNeedsUpdate(
			source_channel_flags, previous_channel_flags, ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL ) );
	const bool bUploadColor = ( eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) &&
		isGpuRegionColorChannelEnabled() && terrain3dGpuCacheOptionalChannelNeedsUpdate(
			source_channel_flags, previous_channel_flags, ( uint8_t ) ( T3DRegion::CHANNEL_COLOR ) );
	Ref<Image> pHeight;
	Ref<Image> pDerived;
	Ref<Image> pControl;
	Ref<Image> pColor;
	Ref<Image> pMiddleComposite;
	const uint8_t previous_state = m_szGpuRegionSlotStates[nLayer];
	m_szGpuRegionSlotStates[nLayer] = TERRAIN3D_GPU_SLOT_STAGING;

	bool bCookedPage = false;
	uint32_t cooked_material_mask = 1u;
	if ( eMapType == T3DRegion::TYPE_MAX && pRegion->runtimeMapsEvictable() && m_pTerrain &&
		m_pTerrain->isGpuRegionCookedPagesEnabled() ) {
		bool bPreparedAttempted = false;
		bCookedPage = takePreparedGpuRuntimePage( vLocation, pHeight, pControl, pColor, pDerived, pMiddleComposite,
			cooked_material_mask, bPreparedAttempted, &prepared_request_usec );
		if ( !bPreparedAttempted && isGpuRuntimePagePreparePending( vLocation ) ) {
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
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( vLocation, true ) ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		++m_nGpuRegionRuntimePreparesLastTick;
	}

	// Resolve the complete publication set before overwriting a physical layer.
	// The page table continues to reference the previous logical region until all
	// source images are known to be valid.
	const bool bDerivedChannelEnabled = isGpuRegionDerivedChannelEnabled();
	if ( bUploadHeight && !bCookedPage ) {
		pHeight = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_HEIGHT );
		if ( pHeight.is_null() ) {
			m_szGpuRegionSlotStates[nLayer] = previous_state;
			return false;
		}
		if ( bDerivedChannelEnabled ) {
			pDerived = pRegion->getDerivedNormalSlopeMap();
			if ( pDerived.is_null() ) {
				++m_nGpuRegionRuntimeDerivedBuildsLastTick;
				regenerateDerivedMaps( {vLocation} );
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
			const Ref<Image> pSourceColor = getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR );
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
	if ( eMapType == T3DRegion::TYPE_MAX && pMiddleComposite.is_null() ) {
		Ref<Image> pCompositeControl = pControl;
		if ( pCompositeControl.is_null() ) {
			pCompositeControl = makeGpuRenderControlImage( vLocation );
		}
		Ref<Image> pCompositeColor = pColor;
		if ( pCompositeColor.is_null() ) {
			pCompositeColor = makeGpuColorImage( getRegionMapOrDefault( pRegion, T3DRegion::TYPE_COLOR ) );
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
	if ( bUploadOk && eMapType == T3DRegion::TYPE_MAX ) {
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
	if ( eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX ) {
		published_flags &= ( uint8_t ) ( ~T3DRegion::CHANNEL_CONTROL );
		published_flags |= source_channel_flags & ( uint8_t ) ( T3DRegion::CHANNEL_CONTROL );
	}
	if ( eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX ) {
		published_flags &= ( uint8_t ) ( ~T3DRegion::CHANNEL_COLOR );
		published_flags |= source_channel_flags & ( uint8_t ) ( T3DRegion::CHANNEL_COLOR );
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

void T3DData::queueGpuRegionRepublish( const Vector2i &vLocation ) {
	const int map_index = getRegionMapIndex( vLocation );
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

bool T3DData::republishGpuRegionLocation( const Vector2i &vLocation ) {
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
		queueGpuRegionRepublish( vLocation );
		return false;
	}
	// A staging slot starts from default channel contents, so publish the entire
	// page even when only one source channel changed. The old mapped page remains
	// valid until the replacement contains a coherent cross-channel snapshot.
	if ( !uploadGpuRegionLayer( staging_layer, vLocation, T3DRegion::TYPE_MAX ) ) {
		return false;
	}
	const int map_index = getRegionMapIndex( vLocation );
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

int T3DData::processPendingGpuRegionRepublishes( const int nLimit ) {
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
		if ( republishGpuRegionLocation( location ) ) {
			++published;
		}
	}
	m_nGpuRegionRepublishCursor = cursor;
	return published;
}

int T3DData::collectDesiredGpuRegions(
	const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity, t3dgpuCacheSelectionStats_t &rStats
) {
	rStats = t3dgpuCacheSelectionStats_s();
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_GPU_CACHE_MAX_LAYERS );
	int output_count = 0;
	uint8_t selected_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	auto append_location = [&]( const Vector2i &location, const t3dregionStreamingState_t wanted ) {
		if ( output_count >= capacity ) {
			return false;
		}
		if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
			rFrame.streamingState( location ) != wanted ) {
			return true;
		}
		const int map_index = getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u ) {
			return true;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
		return output_count < capacity;
	};
	auto append_state = [&]( const t3dregionStreamingState_t wanted ) {
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
		const int map_index = location == V2I_MAX ? -1 : getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || !hasRegion( location ) ||
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

void T3DData::resetMiddleHeightCache( void ) {
	m_generatedMiddleHeightMaps.clear();
	m_generatedMiddleHeightRegionMap.clear();
	std::fill_n( m_szMiddleHeightRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightLocations, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szMiddleHeightLastUseTick, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightRetireAfterFrame, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, ( uint64_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightSlotStates, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS,
		( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
	m_nMiddleHeightPhysicalLayerCount = 0;
	m_nMiddleHeightMappedCapacity = 0;
	m_nMiddleHeightResidentCount = 0;
	m_nMiddleHeightDesiredLastTick = 0;
	m_nMiddleHeightMissingRequiredLastTick = 0;
	m_nMiddleHeightUploadsLastTick = 0;
	m_nMiddleHeightEvictionsLastTick = 0;
	m_nMiddleHeightTick = 0u;
	m_middleHeightCenter = V2I_MAX;
	m_bMiddleHeightTexturesReady = false;
	m_bMiddleHeightPageTableDirty = true;
	m_bMiddleHeightSelectionDirty = true;
	m_nMiddleHeightStreamingGenerationLast = 0u;
}

bool T3DData::configureMiddleHeightCache( void ) {
	if ( !m_pTerrain || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 || m_regionRepository.getActiveRegionCount() <= 0 ) {
		return false;
	}
	ensureDefaultMaps();
	const bool authoring = m_pTerrain->isEditorAuthoringMode();
	const int hot_radius = authoring ? m_regionRepository.getRegionMapSize() : m_pTerrain->getStreamingHotRadius();
	const int diameter = hot_radius * 2 + 1;
	const int required_ceiling = authoring ? m_regionRepository.getActiveRegionCount() :
		MIN( m_regionRepository.getActiveRegionCount(), diameter * diameter );
	const int boundary_spares = authoring ? 0 : MAX( TERRAIN3D_GPU_HEIGHT_CACHE_MIN_SPARE_LAYERS,
		4 * hot_radius + 4 );
	m_nMiddleHeightMappedCapacity = MIN( required_ceiling, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS );
	m_nMiddleHeightPhysicalLayerCount = MIN( m_regionRepository.getActiveRegionCount(),
		MIN( TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, m_nMiddleHeightMappedCapacity + boundary_spares ) );
	if ( m_nMiddleHeightPhysicalLayerCount <= 0 || m_nMiddleHeightMappedCapacity < required_ceiling ) {
		UtilityFunctions::push_error( "Terrain3D: Middle height cache cannot satisfy the configured Active + Hot footprint." );
		return false;
	}

	const int divisor = authoring ? 1 : MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() );
	const int height_side = MAX( 1, ( m_regionRepository.getRegionSize() + divisor - 1 ) / divisor );
	PackedByteArray default_height_bytes;
	default_height_bytes.resize( ( int64_t ) ( height_side ) * ( int64_t ) ( height_side ) * 2 );
	const uint16_t default_height = Terrain3DRegionCodec::encodeHeightU16( 0.0f );
	for ( int index = 0; index < height_side * height_side; ++index ) {
		std::memcpy( default_height_bytes.ptrw() + ( size_t ) ( index ) * 2u,
			&default_height, sizeof( default_height ) );
	}
	const Ref<Image> pMiddleDefault = Image::create_from_data( height_side, height_side, false,
		Image::FORMAT_R16, default_height_bytes );
	if ( pMiddleDefault.is_null() ) {
		return false;
	}
	TypedArray<Image> layers;
	layers.resize( m_nMiddleHeightPhysicalLayerCount );
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		layers[layer] = pMiddleDefault;
	}
	if ( !m_generatedMiddleHeightMaps.create( layers ).is_valid() ) {
		UtilityFunctions::push_error( "Terrain3D: Failed to allocate the Middle height-only texture array." );
		resetMiddleHeightCache();
		return false;
	}
	std::fill_n( m_szMiddleHeightRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
	std::fill_n( m_szMiddleHeightLocations, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, V2I_MAX );
	std::fill_n( m_szMiddleHeightSlotStates, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS,
		( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
	m_bMiddleHeightTexturesReady = true;
	m_bMiddleHeightPageTableDirty = true;
	return updateMiddleHeightPageTableTexture();
}

int T3DData::findMiddleHeightSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int layer = ( int ) ( m_szMiddleHeightRegionMap[map_index] ) - 1;
	if ( layer < 0 || layer >= m_nMiddleHeightPhysicalLayerCount ||
		m_szMiddleHeightSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ||
		m_szMiddleHeightLocations[layer] != vLocation ) {
		return -1;
	}
	return layer;
}

Ref<Image> T3DData::makeMiddleHeightImage( const Vector2i &vLocation, const bool bRuntimeResolution ) const {
	T3DActiveMapLease lease = m_samplingStorage.acquireActive( vLocation );
	const uint16_t *pActiveHeight = nullptr;
	PackedByteArray source_bytes;
	const uint8_t *pSourceBytes = nullptr;
	if ( lease.valid() && lease.view().nRegionSize == m_regionRepository.getRegionSize() && lease.view().pHeight ) {
		pActiveHeight = lease.view().pHeight;
	} else {
		const T3DRegion *const pRegion = getRegionPtr( vLocation );
		const Ref<Image> pHeight = pRegion && !pRegion->isDeleted() ? pRegion->getHeightMap() : Ref<Image>();
		if ( pHeight.is_null() || pHeight->get_width() != m_regionRepository.getRegionSize() || pHeight->get_height() != m_regionRepository.getRegionSize() ||
			pHeight->get_format() != Image::FORMAT_R16 || pHeight->has_mipmaps() ) {
			return Ref<Image>();
		}
		source_bytes = pHeight->get_data();
		const size_t expected_source_bytes = ( size_t ) ( m_regionRepository.getRegionSize() ) * ( size_t ) ( m_regionRepository.getRegionSize() ) * sizeof( uint16_t );
		if ( ( size_t ) ( source_bytes.size() ) != expected_source_bytes ) {
			return Ref<Image>();
		}
		pSourceBytes = source_bytes.ptr();
	}
	const bool use_runtime_resolution = bRuntimeResolution || ( m_pTerrain && !m_pTerrain->isEditorAuthoringMode() );
	const int divisor = m_pTerrain && use_runtime_resolution ?
		MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() ) : 1;
	const int side = MAX( 1, ( m_regionRepository.getRegionSize() + divisor - 1 ) / divisor );
	const size_t byte_count = ( size_t ) ( side ) * ( size_t ) ( side ) * sizeof( uint16_t );
	PackedByteArray bytes;
	bytes.resize( ( int64_t ) ( byte_count ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return Ref<Image>();
	}
	if ( divisor == 1 ) {
		if ( pActiveHeight ) {
			std::memcpy( pWrite, pActiveHeight, byte_count );
		} else {
			std::memcpy( pWrite, pSourceBytes, byte_count );
		}
	} else {
		for ( int y = 0; y < side; ++y ) {
			const int source_y = MIN( y * divisor, m_regionRepository.getRegionSize() - 1 );
			for ( int x = 0; x < side; ++x ) {
				const int source_x = MIN( x * divisor, m_regionRepository.getRegionSize() - 1 );
				const size_t source_offset = ( size_t ) ( source_y * m_regionRepository.getRegionSize() + source_x ) * sizeof( uint16_t );
				uint16_t value = 0u;
				if ( pActiveHeight ) {
					value = pActiveHeight[source_y * m_regionRepository.getRegionSize() + source_x];
				} else {
					std::memcpy( &value, pSourceBytes + source_offset, sizeof( value ) );
				}
				std::memcpy( pWrite + ( size_t ) ( y * side + x ) * sizeof( uint16_t ),
					&value, sizeof( value ) );
			}
		}
	}
	return Image::create_from_data( side, side, false, Image::FORMAT_R16, bytes );
}

bool T3DData::uploadMiddleHeightLayer(
	const int nLayer, const Vector2i &vLocation, const Ref<Image> &rPreparedHeight
) {
	if ( !m_bMiddleHeightTexturesReady || nLayer < 0 || nLayer >= m_nMiddleHeightPhysicalLayerCount ) {
		return false;
	}
	Ref<Image> pHeight = rPreparedHeight;
	if ( pHeight.is_null() ) {
		pHeight = makeMiddleHeightImage( vLocation );
	}
	if ( pHeight.is_null() || pHeight->is_empty() || !m_generatedMiddleHeightMaps.update( pHeight, nLayer ) ) {
		return false;
	}
	++m_nMiddleHeightUploadsLastTick;
	return true;
}


int T3DData::collectDesiredMiddleHeightRegions(
	const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity, int &rRequiredCount
) const {
	rRequiredCount = 0;
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS );
	int output_count = 0;
	uint8_t selected_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int state_value = ( int ) ( TERRAIN3D_REGION_STREAMING_ACTIVE );
		state_value >= ( int ) ( TERRAIN3D_REGION_STREAMING_HOT ) && output_count < capacity; --state_value ) {
		const t3dregionStreamingState_t wanted = ( t3dregionStreamingState_t ) ( state_value );
		terrain3dForEachSquareRing( rCenter, m_regionRepository.getRegionMapSize(), [&]( const Vector2i &location ) {
			if ( output_count >= capacity ) {
				return false;
			}
			if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
				rFrame.streamingState( location ) != wanted ) {
				return true;
			}
			const int map_index = getRegionMapIndex( location );
			if ( map_index < 0 || selected_map[map_index] != 0u ) {
				return true;
			}
			pLocations[output_count++] = location;
			selected_map[map_index] = 1u;
			return output_count < capacity;
		} );
	}
	rRequiredCount = output_count;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount() && output_count < capacity; ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		const int map_index = location == V2I_MAX ? -1 : getRegionMapIndex( location );
		if ( map_index < 0 || selected_map[map_index] != 0u || !hasRegion( location ) ||
			!rFrame.isPrefetch( location ) ) {
			continue;
		}
		pLocations[output_count++] = location;
		selected_map[map_index] = 1u;
	}
	return output_count;
}

bool T3DData::updateMiddleHeightPageTableTexture( void ) {
	if ( m_regionRepository.getRegionMapSize() <= 0 ) {
		return false;
	}
	PackedByteArray bytes;
	bytes.resize( m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() * ( int ) ( sizeof( uint16_t ) ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	std::memset( pWrite, 0, ( size_t ) ( bytes.size() ) );
	const int half = m_regionRepository.getRegionMapSize() / 2;
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szMiddleHeightLocations[layer];
		const int x = location.x + half;
		const int y = location.y + half;
		if ( ( uint32_t ) ( x ) >= ( uint32_t ) ( m_regionRepository.getRegionMapSize() ) ||
			( uint32_t ) ( y ) >= ( uint32_t ) ( m_regionRepository.getRegionMapSize() ) ) {
			continue;
		}
		const uint16_t value = ( uint16_t ) ( layer + 1 );
		std::memcpy( pWrite + ( size_t ) ( y * m_regionRepository.getRegionMapSize() + x ) * sizeof( uint16_t ),
			&value, sizeof( uint16_t ) );
	}
	const Ref<Image> pImage = Image::create_from_data( m_regionRepository.getRegionMapSize(), m_regionRepository.getRegionMapSize(), false,
		Image::FORMAT_R16, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bExisting = m_generatedMiddleHeightRegionMap.getRid().is_valid();
	bool ok = bExisting ? m_generatedMiddleHeightRegionMap.update2d( pImage ) :
		m_generatedMiddleHeightRegionMap.create( pImage ).is_valid();
	if ( !ok && bExisting ) {
		m_generatedMiddleHeightRegionMap.clear();
		ok = m_generatedMiddleHeightRegionMap.create( pImage ).is_valid();
	}
	m_bMiddleHeightPageTableDirty = !ok;
	return ok;
}

void T3DData::updateMiddleHeightResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	const Vector2i &rCenter = rFrame.residencyCenter;
	m_nMiddleHeightUploadsLastTick = 0;
	m_nMiddleHeightEvictionsLastTick = 0;
	m_nMiddleHeightMissingRequiredLastTick = 0;
	if ( !rFrame.bRuntimeTiering || m_regionRepository.getActiveRegionCount() <= 0 ) {
		return;
	}
	++m_nMiddleHeightTick;
	const uint64_t streaming_generation = rFrame.streamingGeneration;
	bool bRetirementPending = false;
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED ) {
			bRetirementPending = true;
			break;
		}
	}
	if ( !bForce && m_bMiddleHeightTexturesReady && rCenter == m_middleHeightCenter &&
		streaming_generation == m_nMiddleHeightStreamingGenerationLast &&
		!m_bMiddleHeightSelectionDirty && !m_bMiddleHeightPageTableDirty && !bRetirementPending ) {
		return;
	}
	const bool large_jump = m_middleHeightCenter != V2I_MAX &&
		MAX( Math::abs( rCenter.x - m_middleHeightCenter.x ), Math::abs( rCenter.y - m_middleHeightCenter.y ) ) > 1;
	if ( !m_bMiddleHeightTexturesReady ) {
		if ( !configureMiddleHeightCache() ) {
			return;
		}
	} else if ( large_jump ) {
		std::fill_n( m_szMiddleHeightRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint16_t ) ( 0 ) );
		std::fill_n( m_szMiddleHeightLocations, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, V2I_MAX );
		std::fill_n( m_szMiddleHeightSlotStates, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS,
			( uint8_t ) ( TERRAIN3D_GPU_SLOT_FREE ) );
		std::fill_n( m_szMiddleHeightRetireAfterFrame, TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS, 0u );
		m_nMiddleHeightResidentCount = 0;
		m_bMiddleHeightPageTableDirty = true;
	}
	m_middleHeightCenter = rCenter;
	m_nMiddleHeightStreamingGenerationLast = streaming_generation;

	Engine *const pEngine = Engine::get_singleton();
	const uint64_t frame = pEngine ? ( uint64_t ) ( pEngine->get_frames_drawn() ) : m_nMiddleHeightTick;
	bRetirementPending = false;
	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] == TERRAIN3D_GPU_SLOT_RETIRED ) {
			if ( frame >= m_szMiddleHeightRetireAfterFrame[layer] ) {
				m_szMiddleHeightSlotStates[layer] = TERRAIN3D_GPU_SLOT_FREE;
				m_szMiddleHeightLocations[layer] = V2I_MAX;
				m_szMiddleHeightRetireAfterFrame[layer] = 0u;
			} else {
				bRetirementPending = true;
			}
		}
	}

	Vector2i desired[TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS];
	int required_count = 0;
	const int desired_count = collectDesiredMiddleHeightRegions( rFrame, rCenter, desired,
		m_nMiddleHeightMappedCapacity, required_count );
	m_nMiddleHeightDesiredLastTick = desired_count;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}

	for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
		if ( m_szMiddleHeightSlotStates[layer] != TERRAIN3D_GPU_SLOT_MAPPED ) {
			continue;
		}
		const Vector2i location = m_szMiddleHeightLocations[layer];
		const int map_index = getRegionMapIndex( location );
		if ( map_index >= 0 && desired_map[map_index] != 0u ) {
			m_szMiddleHeightLastUseTick[layer] = m_nMiddleHeightTick;
			continue;
		}
		if ( map_index >= 0 ) {
			m_szMiddleHeightRegionMap[map_index] = 0u;
		}
		m_szMiddleHeightSlotStates[layer] = TERRAIN3D_GPU_SLOT_RETIRED;
		m_szMiddleHeightRetireAfterFrame[layer] = frame + ( uint64_t ) ( MAX( 1, m_nGpuRegionRetireFrames ) );
		m_nMiddleHeightResidentCount = MAX( 0, m_nMiddleHeightResidentCount - 1 );
		++m_nMiddleHeightEvictionsLastTick;
		m_bMiddleHeightPageTableDirty = true;
		bRetirementPending = true;
	}

	const int upload_budget = bForce ? TERRAIN3D_GPU_HEIGHT_CACHE_MAX_LAYERS :
		MAX( 1, getEffectiveGpuRegionUploadBudgetLayers() );
	const bool cooked_runtime = rFrame.bCookedPagesEnabled;
	bool bAllDesiredResident = true;
	for ( int index = 0; index < desired_count; ++index ) {
		const Vector2i location = desired[index];
		if ( findMiddleHeightSlot( location ) >= 0 ) {
			continue;
		}
		bAllDesiredResident = false;
		const bool required = index < required_count;
		if ( cooked_runtime ) {
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, required ? 2u : 3u );
		}
		if ( !bForce && m_nMiddleHeightUploadsLastTick >= upload_budget ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}
		int free_layer = -1;
		for ( int layer = 0; layer < m_nMiddleHeightPhysicalLayerCount; ++layer ) {
			if ( m_szMiddleHeightSlotStates[layer] == TERRAIN3D_GPU_SLOT_FREE ) {
				free_layer = layer;
				break;
			}
		}
		if ( free_layer < 0 ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}

		Ref<Image> pPreparedHeight;
		if ( cooked_runtime && !takePreparedRuntimeHeight( location,
			RUNTIME_PAGE_CONSUMER_MIDDLE_HEIGHT, pPreparedHeight, true ) ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}
		if ( !uploadMiddleHeightLayer( free_layer, location, pPreparedHeight ) ) {
			if ( required ) {
				++m_nMiddleHeightMissingRequiredLastTick;
			}
			continue;
		}
		m_szMiddleHeightLocations[free_layer] = location;
		m_szMiddleHeightLastUseTick[free_layer] = m_nMiddleHeightTick;
		m_szMiddleHeightSlotStates[free_layer] = TERRAIN3D_GPU_SLOT_MAPPED;
		const int map_index = getRegionMapIndex( location );
		if ( map_index >= 0 ) {
			m_szMiddleHeightRegionMap[map_index] = ( uint16_t ) ( free_layer + 1 );
		}
		++m_nMiddleHeightResidentCount;
		m_bMiddleHeightPageTableDirty = true;
	}
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findMiddleHeightSlot( desired[index] ) < 0 ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	if ( m_bMiddleHeightPageTableDirty || bForce ) {
		updateMiddleHeightPageTableTexture();
	}
	m_bMiddleHeightSelectionDirty = !bAllDesiredResident || bRetirementPending || m_bMiddleHeightPageTableDirty;
}


Dictionary T3DData::getMiddleHeightCacheStats( void ) const {
	Dictionary stats;
	stats["resident"] = m_nMiddleHeightResidentCount;
	stats["mapped_capacity"] = m_nMiddleHeightMappedCapacity;
	stats["physical_layers"] = m_nMiddleHeightPhysicalLayerCount;
	stats["desired_last_tick"] = m_nMiddleHeightDesiredLastTick;
	stats["missing_required_last_tick"] = m_nMiddleHeightMissingRequiredLastTick;
	stats["uploads_last_tick"] = m_nMiddleHeightUploadsLastTick;
	stats["evictions_last_tick"] = m_nMiddleHeightEvictionsLastTick;
	stats["page_table_valid"] = m_generatedMiddleHeightRegionMap.getRid().is_valid();
	stats["height_array_valid"] = m_generatedMiddleHeightMaps.getRid().is_valid();
	stats["resolution_divisor"] = m_pTerrain && !m_pTerrain->isEditorAuthoringMode() ?
		m_pTerrain->getGpuMiddleHeightResolutionDivisor() : 1;
	stats["reserved_bytes"] = ( int64_t ) ( getMiddleHeightCacheReservedBytes() );
	return stats;
}

int T3DData::fillGpuMaterialPriorityIds( uint8_t *const pIds, const int nCapacity ) const {
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

size_t T3DData::getMiddleHeightCacheReservedBytes( void ) const {
	if ( m_nMiddleHeightPhysicalLayerCount <= 0 || m_regionRepository.getRegionSize() <= 0 ) {
		return 0u;
	}
	const int divisor = m_pTerrain && !m_pTerrain->isEditorAuthoringMode() ?
		MAX( 1, m_pTerrain->getGpuMiddleHeightResolutionDivisor() ) : 1;
	const size_t side = ( size_t ) ( MAX( 1, ( m_regionRepository.getRegionSize() + divisor - 1 ) / divisor ) );
	const size_t page_table = ( size_t ) ( MAX( 0, m_regionRepository.getRegionMapSize() ) ) *
		( size_t ) ( MAX( 0, m_regionRepository.getRegionMapSize() ) ) * sizeof( uint16_t );
	return side * side * sizeof( uint16_t ) * ( size_t ) ( m_nMiddleHeightPhysicalLayerCount ) + page_table;
}

int T3DData::calculateCpuRegionCacheCapacity( void ) const {
	return terrain3dCpuCacheCalculateCapacity( m_regionRepository.getRegionSize(), m_nCpuRegionCacheBudgetMb,
		TERRAIN3D_CPU_CACHE_MAX_LAYERS );
}

size_t T3DData::estimateCpuRegionLayerBytes( void ) const {
	return terrain3dCpuCacheEstimateLayerBytes( m_regionRepository.getRegionSize() );
}

void T3DData::resetCpuRegionCache( const bool bReleaseCleanMaps ) {
	if ( bReleaseCleanMaps ) {
		for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
			evictCpuRegionSlot( slot );
		}
	}
	std::fill_n( m_szCpuRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szCpuRegionLocations, TERRAIN3D_CPU_CACHE_MAX_LAYERS, V2I_MAX );
	m_nCpuRegionCacheCapacity = 0;
	m_nCpuRegionResidentCount = 0;
	m_nCpuRegionDesiredLastTick = 0;
	m_nCpuRegionLoadsLastTick = 0;
	m_nCpuRegionLoadsSinceReset = 0u;
	m_nCpuRegionEvictionsLastTick = 0;
	m_nCpuRegionPinnedLastTick = 0;
	m_nCpuRegionLoadBytesLastTick = 0u;
	m_nCpuRegionAsyncRequestsLastTick = 0;
	m_nCpuRegionAsyncReadyLastTick = 0;
	m_nCpuRegionAsyncPendingLastTick = 0;
	m_nCpuRegionAsyncOutstanding = 0;
	m_nCpuRegionPrefetchStreamingGenerationLast = 0u;
	std::fill_n( m_szCpuRegionAsyncState, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	m_cpuRegionCacheCenter = V2I_MAX;
	m_bCpuRegionCacheSelectionDirty = true;
}

int T3DData::findCpuRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int slot = ( int ) ( m_szCpuRegionMap[map_index] ) - 1;
	if ( slot < 0 || slot >= m_nCpuRegionCacheCapacity || m_szCpuRegionLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void T3DData::invalidateCpuRegionLocation( const Vector2i &vLocation ) {
	const int slot = findCpuRegionSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szCpuRegionMap[map_index] = 0u;
	}
	m_szCpuRegionLocations[slot] = V2I_MAX;
	m_nCpuRegionResidentCount = MAX( 0, m_nCpuRegionResidentCount - 1 );
	m_bCpuRegionCacheSelectionDirty = true;
}

bool T3DData::loadCpuRegionSource( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) const {
	rSource.unref();
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayload() ) {
		rSource = Ref<T3DRegion>( pRegion );
		return true;
	}
	return m_regionPersistence.loadRuntimeRegionSource(
		pRegion->get_path(), m_regionRepository.getRegionSize(), rSource );
}

bool T3DData::requestCpuRegionSourceAsync( const Vector2i &vLocation ) {
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 || !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayload() ||
		m_szCpuRegionAsyncState[map_index] != 0u ) {
		return pRegion && !pRegion->isDeleted();
	}
	const String path = pRegion->get_path();
	if ( path.is_empty() || !FileAccess::file_exists( path ) ) {
		m_szCpuRegionAsyncState[map_index] = 2u;
		return false;
	}
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return false;
	}
	const Error error = loader->load_threaded_request( path, "Terrain3DRegion", true,
		ResourceLoader::CACHE_MODE_IGNORE );
	if ( error != OK && error != ERR_BUSY ) {
		m_szCpuRegionAsyncState[map_index] = 2u;
		return false;
	}
	m_szCpuRegionAsyncState[map_index] = 1u;
	++m_nCpuRegionAsyncOutstanding;
	++m_nCpuRegionAsyncRequestsLastTick;
	return true;
}

bool T3DData::takeCpuRegionSourceAsync( const Vector2i &vLocation, Ref<T3DRegion> &rSource ) {
	rSource.unref();
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 || !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( pRegion->hasRuntimeMapPayload() ) {
		rSource = Ref<T3DRegion>( pRegion );
		return true;
	}
	if ( m_szCpuRegionAsyncState[map_index] != 1u ) {
		requestCpuRegionSourceAsync( vLocation );
		return false;
	}
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return false;
	}
	const String path = pRegion->get_path();
	const ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( path );
	if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
		++m_nCpuRegionAsyncPendingLastTick;
		return false;
	}
	if ( status != ResourceLoader::THREAD_LOAD_LOADED ) {
		m_szCpuRegionAsyncState[map_index] = 2u;
		m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
		return false;
	}
	const Ref<Resource> pResource = loader->load_threaded_get( path );
	const Ref<T3DRegion> pLoaded = pResource;
	m_szCpuRegionAsyncState[map_index] = 0u;
	m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
	if ( !m_regionPersistence.prepareRuntimeRegionSource(
		pLoaded, m_regionRepository.getRegionSize(), rSource ) ) {
		return false;
	}
	++m_nCpuRegionAsyncReadyLastTick;
	return true;
}

void T3DData::prewarmCpuRegionSources( const T3DResidencyFrameState &rFrame ) {
	if ( !m_bCpuRegionCacheEnabled || rFrame.bAuthoring ) {
		return;
	}
	const Vector2i center = rFrame.predictedCenter;
	const int radius = rFrame.nStreamingActiveRadius;
	for ( int y = -radius; y <= radius; ++y ) {
		for ( int x = -radius; x <= radius; ++x ) {
			const Vector2i location = center + Vector2i( x, y );
			if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
				findCpuRegionSlot( location ) >= 0 ) {
				continue;
			}
			T3DRegion *const pRegion = getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && !pRegion->hasRuntimeMapPayload() ) {
				requestCpuRegionSourceAsync( location );
			}
		}
	}
}

void T3DData::reapCpuRegionSourceAsync( const T3DResidencyFrameState &rFrame ) {
	if ( m_nCpuRegionAsyncOutstanding <= 0 || rFrame.bAuthoring ) {
		return;
	}
	ResourceLoader *const loader = ResourceLoader::get_singleton();
	if ( !loader ) {
		return;
	}
	const Vector2i predicted_center = rFrame.predictedCenter;
	const int active_radius = rFrame.nStreamingActiveRadius;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount() && m_nCpuRegionAsyncOutstanding > 0; ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		const int map_index = location == V2I_MAX ? -1 : getRegionMapIndex( location );
		if ( map_index < 0 || m_szCpuRegionAsyncState[map_index] == 0u ) {
			continue;
		}
		const bool active_now = rFrame.streamingState( location ) == TERRAIN3D_REGION_STREAMING_ACTIVE;
		const bool predicted_active = MAX( Math::abs( location.x - predicted_center.x ),
			Math::abs( location.y - predicted_center.y ) ) <= active_radius;
		if ( active_now || predicted_active ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->get_path().is_empty() ) {
			m_szCpuRegionAsyncState[map_index] = 0u;
			m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
			continue;
		}
		const String path = pRegion->get_path();
		const ResourceLoader::ThreadLoadStatus status = loader->load_threaded_get_status( path );
		if ( status == ResourceLoader::THREAD_LOAD_IN_PROGRESS ) {
			continue;
		}
		if ( status == ResourceLoader::THREAD_LOAD_LOADED ) {
			// Collect and immediately release stale completed requests.
			const Ref<Resource> discard = loader->load_threaded_get( path );
			( void ) discard;
		}
		m_szCpuRegionAsyncState[map_index] = 0u;
		m_nCpuRegionAsyncOutstanding = MAX( 0, m_nCpuRegionAsyncOutstanding - 1 );
	}
}


bool T3DData::publishCpuRegionSlot(
	const int iSlot, const Vector2i &vLocation,
	const Ref<T3DRegion> &rSource
) {
	if ( iSlot < 0 || iSlot >= m_nCpuRegionCacheCapacity || rSource.is_null() ) {
		return false;
	}
	T3DRegion *const pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	pRegion->importRuntimeMapsUnmodified( rSource.ptr() );
	if ( !pRegion->hasRuntimeMapPayload() ) {
		return false;
	}
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return false;
	}
	m_szCpuRegionLocations[iSlot] = vLocation;
	m_szCpuRegionMap[map_index] = ( uint8_t ) ( iSlot + 1 );
	++m_nCpuRegionResidentCount;
	publishRegionSamplingGeneration( vLocation );
	return true;
}

bool T3DData::evictCpuRegionSlot( const int iSlot ) {
	if ( iSlot < 0 || iSlot >= m_nCpuRegionCacheCapacity ) {
		return false;
	}
	const Vector2i location = m_szCpuRegionLocations[iSlot];
	if ( location == V2I_MAX ) {
		return true;
	}
	T3DRegion *const pRegion = getRegionPtr( location );
	if ( pRegion && pRegion->hasRuntimeMapPayload() && !pRegion->runtimeMapsEvictable() ) {
		return false;
	}
	m_samplingStorage.invalidateResident( location );
	if ( pRegion ) {
		pRegion->releaseRuntimeMapsUnmodified();
	}
	const int map_index = getRegionMapIndex( location );
	if ( map_index >= 0 ) {
		m_szCpuRegionMap[map_index] = 0u;
	}
	m_szCpuRegionLocations[iSlot] = V2I_MAX;
	m_nCpuRegionResidentCount = MAX( 0, m_nCpuRegionResidentCount - 1 );
	publishRegionSamplingGeneration( location );
	return true;
}

bool T3DData::ensureCpuRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	if ( !m_bCpuRegionCacheEnabled ) {
		return true;
	}
	const int existing = findCpuRegionSlot( vLocation );
	if ( existing >= 0 ) {
		return true;
	}
	T3DRegion *const pExistingRegion = getRegionPtr( vLocation );
	if ( pExistingRegion && pExistingRegion->hasRuntimeMapPayload() &&
		!pExistingRegion->runtimeMapsEvictable() ) {
		// Dirty, undo-owned, deleted, and pathless resources are deliberately
		// outside the bounded clean cache. Their payload is already available and
		// remains pinned until persistence/lifecycle state makes it evictable.
		return true;
	}
	Ref<T3DRegion> pSource;
	if ( !loadCpuRegionSource( vLocation, pSource ) ) {
		return false;
	}
	int target_slot = -1;
	for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
		if ( m_szCpuRegionLocations[slot] == V2I_MAX ) {
			target_slot = slot;
			break;
		}
	}
	if ( target_slot < 0 ) {
		int farthest_distance = -1;
		const Vector2i eviction_center = m_cpuRegionCacheCenter == V2I_MAX ? vLocation : m_cpuRegionCacheCenter;
		const t3dcpuCacheCoord_t center = {eviction_center.x, eviction_center.y};
		for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
			const Vector2i old_location = m_szCpuRegionLocations[slot];
			T3DRegion *const pOldRegion = getRegionPtr( old_location );
			if ( !pOldRegion || !pOldRegion->runtimeMapsEvictable() ) {
				continue;
			}
			const int distance = terrain3dCpuCacheDistance(
				{old_location.x, old_location.y}, center );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				target_slot = slot;
			}
		}
	}
	if ( target_slot < 0 ) {
		return false;
	}
	if ( m_szCpuRegionLocations[target_slot] != V2I_MAX ) {
		if ( !evictCpuRegionSlot( target_slot ) ) {
			return false;
		}
		++m_nCpuRegionEvictionsLastTick;
	}
	if ( !publishCpuRegionSlot( target_slot, vLocation, pSource ) ) {
		return false;
	}
	if ( bDemandLoad ) {
		++m_nCpuRegionDemandLoads;
	}
	return true;
}

void T3DData::releaseUntrackedCpuMaps( void ) {
	if ( !m_bCpuRegionCacheEnabled ) {
		return;
	}
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		if ( location == V2I_MAX || findCpuRegionSlot( location ) >= 0 ) {
			continue;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		if ( !pRegion || !pRegion->hasRuntimeMapPayload() || !pRegion->runtimeMapsEvictable() ) {
			continue;
		}
		m_samplingStorage.invalidateResident( location );
		pRegion->releaseRuntimeMapsUnmodified();
		publishRegionSamplingGeneration( location );
	}
}

bool T3DData::restoreAllCpuMaps( void ) {
	bool bSuccess = true;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || pRegion->hasRuntimeMapPayload() ) {
			continue;
		}
		Ref<T3DRegion> pSource;
		if ( !loadCpuRegionSource( location, pSource ) ) {
			bSuccess = false;
			continue;
		}
		pRegion->importRuntimeMapsUnmodified( pSource.ptr() );
		publishRegionSamplingGeneration( location );
	}
	return bSuccess;
}

int T3DData::collectDesiredCpuRegions(
	const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity
) const {
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_CPU_CACHE_MAX_LAYERS );
	int output_count = 0;
	terrain3dForEachSquareRing( rCenter, m_regionRepository.getRegionMapSize(), [&]( const Vector2i &location ) {
		if ( output_count >= capacity ) {
			return false;
		}
		if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
			rFrame.streamingState( location ) != TERRAIN3D_REGION_STREAMING_ACTIVE ) {
			return true;
		}
		pLocations[output_count++] = location;
		return output_count < capacity;
	} );
	return output_count;
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
	return m_regionResidency.effectiveGpuUploadLayers( m_nGpuRegionUploadBudgetLayers );
}

int T3DData::getEffectiveCpuRegionLoadBudgetLayers( void ) const {
	return m_regionResidency.effectiveCpuLoadLayers( m_nCpuRegionLoadBudgetLayers );
}

int T3DData::getEffectiveInstanceRegionLoadBudgetPages( void ) const {
	return m_regionResidency.effectiveInstanceLoadPages( m_nInstanceRegionLoadBudgetPages );
}


void T3DData::configureCpuRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadLayers, const int nLoadBudgetMb
) {
	const bool bOldEnabled = m_bCpuRegionCacheEnabled;
	const int old_budget = m_nCpuRegionCacheBudgetMb;
	m_bCpuRegionCacheEnabled = bEnabled;
	m_nCpuRegionCacheBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_CPU_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_CPU_REGION_CACHE_MAX_BUDGET_MB );
	m_nCpuRegionLoadBudgetLayers = CLAMP( nLoadLayers, 1, TERRAIN3D_CPU_REGION_LOAD_MAX_LAYERS );
	m_nCpuRegionLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, TERRAIN3D_CPU_REGION_LOAD_MAX_BUDGET_MB );
	if ( !bOldEnabled && !m_bCpuRegionCacheEnabled ) {
		return;
	}
	if ( bOldEnabled && !m_bCpuRegionCacheEnabled ) {
		if ( !restoreAllCpuMaps() ) {
			m_bCpuRegionCacheEnabled = true;
			m_bCpuRegionCacheSelectionDirty = true;
			LOG( T3DTerrain::ERROR, "CPU region cache remains enabled because one or more region payloads could not be restored" );
			return;
		}
		resetCpuRegionCache( false );
		return;
	}
	const int capacity = calculateCpuRegionCacheCapacity();
	if ( !bOldEnabled || old_budget != m_nCpuRegionCacheBudgetMb || capacity != m_nCpuRegionCacheCapacity ) {
		resetCpuRegionCache( false );
		m_nCpuRegionCacheCapacity = capacity;
		m_bCpuRegionCacheSelectionDirty = true;
	}
}

bool T3DData::ensureCpuRegionResident( const Vector2i &vLocation ) {
	if ( !m_bCpuRegionCacheEnabled ) {
		return hasRegion( vLocation );
	}
	if ( !hasRegion( vLocation ) ) {
		return false;
	}
	return ensureCpuRegionResidentInternal( vLocation, true );
}


bool T3DData::ensureCpuSampleArea( const Vector3 &rWorldPosition, const int nRadiusTexels ) {
	if ( !m_bCpuRegionCacheEnabled || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getVertexSpacing() <= 0.0f ) {
		return true;
	}
	const int radius = MAX( 0, nRadiusTexels ) + 1;
	const Vector2i center(
		( int ) ( Math::floor( rWorldPosition.x / m_regionRepository.getVertexSpacing() ) ),
		( int ) ( Math::floor( rWorldPosition.z / m_regionRepository.getVertexSpacing() ) )
	);
	const Vector2i extent( radius * 2 + 1, radius * 2 + 1 );
	const Rect2i bounds = getRegionBoundsForArea( Rect2i( center - Vector2i( radius, radius ), extent ), m_regionRepository.getRegionSize() );
	bool bSuccess = true;
	for ( int y = bounds.position.y; y < bounds.get_end().y; ++y ) {
		for ( int x = bounds.position.x; x < bounds.get_end().x; ++x ) {
			const Vector2i location( x, y );
			if ( hasRegion( location ) && !ensureCpuRegionResident( location ) ) {
				bSuccess = false;
			}
		}
	}
	return bSuccess;
}

void T3DData::updateCpuRegionResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	const T3DLastScopeTimer serviceTimer( m_nCpuRegionServiceUsecLast, rFrame.bProfilingEnabled );
	m_nCpuRegionLoadsLastTick = 0;
	m_nCpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nCpuRegionEvictionsLastTick = 0;
	m_nCpuRegionPinnedLastTick = 0;
	m_nCpuRegionLoadBytesLastTick = 0u;
	m_nCpuRegionAsyncRequestsLastTick = 0;
	m_nCpuRegionAsyncReadyLastTick = 0;
	m_nCpuRegionAsyncPendingLastTick = 0;
	if ( !m_bCpuRegionCacheEnabled || m_nCpuRegionCacheCapacity <= 0 || m_regionRepository.getRegionSize() <= 0 ||
		m_regionRepository.getVertexSpacing() <= 0.0f ) {
		return;
	}

	// Predictive CPU requests only need to be reconsidered when the shared streaming
	// policy changes. Crossing a boundary should normally promote an already-loaded
	// Terrain3DRegion resource instead of invoking ResourceLoader::load() in physics.
	const uint64_t streaming_generation = rFrame.streamingGeneration;
	if ( bForce || streaming_generation != m_nCpuRegionPrefetchStreamingGenerationLast ) {
		prewarmCpuRegionSources( rFrame );
		m_nCpuRegionPrefetchStreamingGenerationLast = streaming_generation;
	}
	reapCpuRegionSourceAsync( rFrame );

	const Vector2i center = rFrame.residencyCenter;
	if ( !bForce && center == m_cpuRegionCacheCenter && !m_bCpuRegionCacheSelectionDirty ) {
		return;
	}
	Vector2i desired[TERRAIN3D_CPU_CACHE_MAX_LAYERS];
	const int desired_count = collectDesiredCpuRegions( rFrame, center, desired, m_nCpuRegionCacheCapacity );
	m_nCpuRegionDesiredLastTick = desired_count;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}
	const size_t layer_bytes = estimateCpuRegionLayerBytes();
	const size_t byte_budget = ( size_t ) ( m_nCpuRegionLoadBudgetMb ) * TERRAIN3D_CPU_CACHE_MIB;
	const int load_layer_budget = getEffectiveCpuRegionLoadBudgetLayers();
	bool bAllDesiredResident = true;
	for ( int desired_index = 0; desired_index < desired_count; ++desired_index ) {
		const Vector2i location = desired[desired_index];
		if ( findCpuRegionSlot( location ) >= 0 ) {
			continue;
		}
		T3DRegion *const pAvailableRegion = getRegionPtr( location );
		if ( pAvailableRegion && pAvailableRegion->hasRuntimeMapPayload() &&
			!pAvailableRegion->runtimeMapsEvictable() ) {
			continue;
		}
		bAllDesiredResident = false;
		if ( !bForce && m_nCpuRegionLoadsLastTick >= load_layer_budget ) {
			m_nCpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_LOAD_BUDGET;
			break;
		}
		if ( !bForce && m_nCpuRegionLoadsLastTick > 0 &&
			m_nCpuRegionLoadBytesLastTick + layer_bytes > byte_budget ) {
			m_nCpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_LOAD_BYTE_BUDGET;
			break;
		}
		Ref<T3DRegion> pSource;
		const bool bSourceReady = bForce ? loadCpuRegionSource( location, pSource ) :
			takeCpuRegionSourceAsync( location, pSource );
		if ( !bSourceReady ) {
			if ( !bForce ) {
				requestCpuRegionSourceAsync( location );
			}
			continue;
		}
		int target_slot = -1;
		for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
			if ( m_szCpuRegionLocations[slot] == V2I_MAX ) {
				target_slot = slot;
				break;
			}
		}
		if ( target_slot < 0 ) {
			int farthest_distance = -1;
			for ( int slot = 0; slot < m_nCpuRegionCacheCapacity; ++slot ) {
				const Vector2i old_location = m_szCpuRegionLocations[slot];
				const int old_map_index = getRegionMapIndex( old_location );
				if ( old_map_index >= 0 && desired_map[old_map_index] != 0u ) {
					continue;
				}
				T3DRegion *const pOldRegion = getRegionPtr( old_location );
				if ( !pOldRegion || !pOldRegion->runtimeMapsEvictable() ) {
					++m_nCpuRegionPinnedLastTick;
					continue;
				}
				const int distance = MAX( std::abs( old_location.x - center.x ),
					std::abs( old_location.y - center.y ) );
				if ( distance > farthest_distance ) {
					farthest_distance = distance;
					target_slot = slot;
				}
			}
		}
		if ( target_slot < 0 ) {
			m_nCpuRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_CPU_PINNED_CAPACITY;
			continue;
		}
		if ( m_szCpuRegionLocations[target_slot] != V2I_MAX ) {
			if ( !evictCpuRegionSlot( target_slot ) ) {
				continue;
			}
			++m_nCpuRegionEvictionsLastTick;
		}
		if ( !publishCpuRegionSlot( target_slot, location, pSource ) ) {
			continue;
		}
		++m_nCpuRegionLoadsLastTick;
		++m_nCpuRegionLoadsSinceReset;
		m_nCpuRegionLoadBytesLastTick += layer_bytes;
	}
	if ( !bAllDesiredResident ) {
		bAllDesiredResident = true;
		for ( int index = 0; index < desired_count; ++index ) {
			const Vector2i location = desired[index];
			T3DRegion *const pAvailableRegion = getRegionPtr( location );
			const bool bPinnedAvailable = pAvailableRegion && pAvailableRegion->hasRuntimeMapPayload() &&
				!pAvailableRegion->runtimeMapsEvictable();
			if ( findCpuRegionSlot( location ) < 0 && !bPinnedAvailable ) {
				bAllDesiredResident = false;
				break;
			}
		}
	}
	m_cpuRegionCacheCenter = center;
	m_bCpuRegionCacheSelectionDirty = !bAllDesiredResident;
	releaseUntrackedCpuMaps();
}


Dictionary T3DData::getCpuRegionCacheStatsFast( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bCpuRegionCacheEnabled;
	stats["budget_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["capacity"] = m_nCpuRegionCacheCapacity;
	stats["resident"] = m_nCpuRegionResidentCount;
	stats["desired_last_tick"] = m_nCpuRegionDesiredLastTick;
	stats["loads_last_tick"] = m_nCpuRegionLoadsLastTick;
	stats["loads_since_reset"] = ( int64_t ) ( m_nCpuRegionLoadsSinceReset );
	stats["evictions_last_tick"] = m_nCpuRegionEvictionsLastTick;
	stats["pinned_last_tick"] = m_nCpuRegionPinnedLastTick;
	stats["demand_loads"] = m_nCpuRegionDemandLoads;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nCpuRegionLoadBytesLastTick );
	stats["async_requests_last_tick"] = m_nCpuRegionAsyncRequestsLastTick;
	stats["async_ready_last_tick"] = m_nCpuRegionAsyncReadyLastTick;
	stats["async_pending_last_tick"] = m_nCpuRegionAsyncPendingLastTick;
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nCpuRegionServiceUsecLast );
	stats["selection_pending"] = m_bCpuRegionCacheSelectionDirty;
	stats["reserved_bytes"] = ( int64_t ) ( getCpuRegionCacheReservedBytes() );
	stats["active_height_bytes"] = ( int64_t ) ( m_samplingStorage.getActiveReservedBytes() );
	stats["active_height_physical_capacity"] = m_samplingStorage.getActivePhysicalCapacity();
	const T3DActiveHeightResidencyStats &activeHeightStats = m_activeHeightResidency.getStats();
	stats["active_height_reconciliations_last_tick"] = activeHeightStats.nReconciliationsLastTick;
	stats["active_height_slots_examined_last_tick"] = activeHeightStats.nSlotsExaminedLastTick;
	stats["active_height_required_last_tick"] = activeHeightStats.nRequiredLastTick;
	stats["active_height_missing_required_last_tick"] = activeHeightStats.nMissingRequiredLastTick;
	stats["active_height_published_last_tick"] = activeHeightStats.nPublishedLastTick;
	stats["active_height_evicted_last_tick"] = activeHeightStats.nEvictedLastTick;
	stats["active_height_predicted_requests_last_tick"] = activeHeightStats.nPredictedRequestsLastTick;
	stats["active_height_reader_retries"] = ( int64_t ) ( m_samplingStorage.getActiveReaderRetryCount() );
	stats["active_height_reader_mutexes"] = ( int64_t ) ( m_samplingStorage.getActiveReaderMutexCount() );
	stats["active_height_publications"] = ( int64_t ) ( m_samplingStorage.getActivePublicationCount() );
	return stats;
}

Dictionary T3DData::getCpuRegionCacheStats( void ) const {
	Dictionary stats;
	int pinned_payload_count = 0;
	int total_payload_count = 0;
	for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
		const Vector2i location = m_regionRepository.getSlotsData()[slot];
		const T3DRegion *pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() || !pRegion->hasRuntimeMapPayload() ) {
			continue;
		}
		++total_payload_count;
		if ( findCpuRegionSlot( location ) < 0 ) {
			++pinned_payload_count;
		}
	}
	stats["enabled"] = m_bCpuRegionCacheEnabled;
	stats["budget_mb"] = m_nCpuRegionCacheBudgetMb;
	stats["capacity"] = m_nCpuRegionCacheCapacity;
	stats["resident"] = m_nCpuRegionResidentCount;
	stats["desired_last_tick"] = m_nCpuRegionDesiredLastTick;
	stats["load_layer_budget"] = m_nCpuRegionLoadBudgetLayers;
	stats["effective_load_layer_budget"] = getEffectiveCpuRegionLoadBudgetLayers();
	stats["load_budget_mb"] = m_nCpuRegionLoadBudgetMb;
	stats["loads_last_tick"] = m_nCpuRegionLoadsLastTick;
	stats["loads_since_reset"] = ( int64_t ) ( m_nCpuRegionLoadsSinceReset );
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nCpuRegionStallFlagsLastTick );
	stats["evictions_last_tick"] = m_nCpuRegionEvictionsLastTick;
	stats["pinned_last_tick"] = m_nCpuRegionPinnedLastTick;
	stats["pinned_payloads"] = pinned_payload_count;
	stats["total_payloads"] = total_payload_count;
	stats["demand_loads"] = m_nCpuRegionDemandLoads;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nCpuRegionLoadBytesLastTick );
	stats["async_requests_last_tick"] = m_nCpuRegionAsyncRequestsLastTick;
	stats["async_ready_last_tick"] = m_nCpuRegionAsyncReadyLastTick;
	stats["async_pending_last_tick"] = m_nCpuRegionAsyncPendingLastTick;
	const size_t layer_bytes = estimateCpuRegionLayerBytes();
	const size_t worst_case_bytes = layer_bytes * ( size_t ) ( MAX( 0, m_nCpuRegionCacheCapacity ) );
	const size_t budget_bytes = ( size_t ) ( m_nCpuRegionCacheBudgetMb ) * TERRAIN3D_CPU_CACHE_MIB;
	const size_t active_height_bytes = m_samplingStorage.getActiveReservedBytes();
	const size_t default_map_bytes = m_regionRepository.getRegionSize() > 0 ? layer_bytes : 0u;
	const size_t payload_bytes = layer_bytes * ( size_t ) ( MAX( 0, total_payload_count ) );
	const size_t pinned_bytes = layer_bytes * ( size_t ) ( MAX( 0, pinned_payload_count ) );
	const size_t total_map_bytes = active_height_bytes + default_map_bytes + payload_bytes;
	stats["estimated_layer_bytes"] = ( int64_t ) ( layer_bytes );
	stats["estimated_worst_case_bytes"] = ( int64_t ) ( worst_case_bytes );
	stats["estimated_payload_bytes"] = ( int64_t ) ( payload_bytes );
	stats["estimated_pinned_bytes"] = ( int64_t ) ( pinned_bytes );
	stats["active_height_bytes"] = ( int64_t ) ( active_height_bytes );
	stats["active_height_physical_capacity"] = m_samplingStorage.getActivePhysicalCapacity();
	const T3DActiveHeightResidencyStats &activeHeightStats = m_activeHeightResidency.getStats();
	stats["active_height_reconciliations_last_tick"] = activeHeightStats.nReconciliationsLastTick;
	stats["active_height_slots_examined_last_tick"] = activeHeightStats.nSlotsExaminedLastTick;
	stats["active_height_required_last_tick"] = activeHeightStats.nRequiredLastTick;
	stats["active_height_missing_required_last_tick"] = activeHeightStats.nMissingRequiredLastTick;
	stats["active_height_published_last_tick"] = activeHeightStats.nPublishedLastTick;
	stats["active_height_evicted_last_tick"] = activeHeightStats.nEvictedLastTick;
	stats["active_height_predicted_requests_last_tick"] = activeHeightStats.nPredictedRequestsLastTick;
	stats["active_height_reader_retries"] = ( int64_t ) ( m_samplingStorage.getActiveReaderRetryCount() );
	stats["active_height_reader_mutexes"] = ( int64_t ) ( m_samplingStorage.getActiveReaderMutexCount() );
	stats["active_height_publications"] = ( int64_t ) ( m_samplingStorage.getActivePublicationCount() );
	stats["default_map_bytes"] = ( int64_t ) ( default_map_bytes );
	stats["estimated_total_map_bytes"] = ( int64_t ) ( total_map_bytes );
	stats["target_map_ceiling_bytes"] = ( int64_t ) ( TERRAIN3D_XBOX360_CPU_MAP_CEILING_BYTES );
	stats["budget_overcommitted"] = worst_case_bytes > budget_bytes;
	stats["runtime_overcommitted"] = total_payload_count > m_nCpuRegionCacheCapacity;
	stats["target_map_ceiling_exceeded"] = total_map_bytes > TERRAIN3D_XBOX360_CPU_MAP_CEILING_BYTES;
	stats["center"] = m_cpuRegionCacheCenter;
	stats["selection_pending"] = m_bCpuRegionCacheSelectionDirty;
	return stats;
}

void T3DData::emitGpuRegionMapChanged( void ) {
	if ( m_bStartupBulkLoad ) {
		return;
	}
	++m_nGpuRegionPageTableUpdatesLastTick;
	emit_signal( "gpu_region_page_table_changed" );
	armPendingGpuRegionRetirements();
}


void T3DData::resetInstanceRegionCache( const bool bRestoreAll ) {
	if ( bRestoreAll ) {
		for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
			const Vector2i location = m_regionRepository.getSlotsData()[slot];
			T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
			if ( pRegion && !pRegion->isDeleted() && pRegion->hasInstances() ) {
				pRegion->ensureInstanceStoreResident();
			}
		}
	} else {
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			evictInstanceRegionSlot( slot );
		}
	}
	std::fill_n( m_szInstanceRegionMap, TERRAIN3D_MAX_REGION_SLOTS, ( uint8_t ) ( 0 ) );
	std::fill_n( m_szInstanceRegionLocations, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES, V2I_MAX );
	m_nInstanceRegionCacheCapacity = TERRAIN3D_INSTANCE_CACHE_MAX_PAGES;
	m_nInstanceRegionResidentCount = 0;
	m_nInstanceRegionDesiredLastTick = 0;
	m_nInstanceRegionLoadsLastTick = 0;
	m_nInstanceRegionEvictionsLastTick = 0;
	m_nInstanceRegionLoadBytesLastTick = 0u;
	m_nInstanceRegionResidentBytes = 0u;
	m_instanceRegionCacheCenter = V2I_MAX;
	m_nInstancePrefetchStreamingGenerationLast = 0u;
	m_bInstanceRegionCacheSelectionDirty = true;
}

int T3DData::findInstanceRegionSlot( const Vector2i &vLocation ) const {
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index < 0 ) {
		return -1;
	}
	const int slot = ( int ) ( m_szInstanceRegionMap[map_index] ) - 1;
	if ( slot < 0 || slot >= m_nInstanceRegionCacheCapacity || m_szInstanceRegionLocations[slot] != vLocation ) {
		return -1;
	}
	return slot;
}

void T3DData::invalidateInstanceRegionLocation( const Vector2i &vLocation ) {
	const int slot = findInstanceRegionSlot( vLocation );
	if ( slot < 0 ) {
		return;
	}
	evictInstanceRegionSlot( slot );
	m_bInstanceRegionCacheSelectionDirty = true;
}

bool T3DData::evictInstanceRegionSlot( const int iSlot ) {
	if ( !terrain3dInstanceCacheIndexValid( iSlot, m_nInstanceRegionCacheCapacity ) ) {
		return false;
	}
	const Vector2i location = m_szInstanceRegionLocations[iSlot];
	if ( location == V2I_MAX ) {
		return true;
	}
	T3DRegion *const pRegion = getRegionPtr( location );
	size_t resident_bytes = 0u;
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		resident_bytes = pRegion->getInstanceResidentBytes();
		const PackedByteArray page = pRegion->getInstancePage();
		if ( page.is_empty() && pRegion->hasInstances() ) {
			return false;
		}
	}
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->destroyRegionMmis( location );
	}
	if ( pRegion && pRegion->isInstanceStoreResident() ) {
		if ( !pRegion->evictInstanceStore() ) {
			return false;
		}
		m_nInstanceRegionResidentBytes = resident_bytes <= m_nInstanceRegionResidentBytes ?
			m_nInstanceRegionResidentBytes - resident_bytes : 0u;
	}
	const int map_index = getRegionMapIndex( location );
	if ( map_index >= 0 ) {
		m_szInstanceRegionMap[map_index] = 0u;
	}
	m_szInstanceRegionLocations[iSlot] = V2I_MAX;
	m_nInstanceRegionResidentCount = MAX( 0, m_nInstanceRegionResidentCount - 1 );
	return true;
}

bool T3DData::ensureInstanceRegionResidentInternal( const Vector2i &vLocation, const bool bDemandLoad ) {
	T3DRegion *pRegion = getRegionPtr( vLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return false;
	}
	if ( !m_bInstanceRegionCacheEnabled ) {
		return pRegion->ensureInstanceStoreResident();
	}
	if ( findInstanceRegionSlot( vLocation ) >= 0 ) {
		return true;
	}
	if ( !pRegion->hasInstances() ) {
		return true;
	}
	int target_slot = -1;
	for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
		if ( m_szInstanceRegionLocations[slot] == V2I_MAX ) {
			target_slot = slot;
			break;
		}
	}
	if ( target_slot < 0 ) {
		int farthest_distance = -1;
		const Vector2i center = m_instanceRegionCacheCenter == V2I_MAX ? vLocation : m_instanceRegionCacheCenter;
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			const Vector2i old_location = m_szInstanceRegionLocations[slot];
			const int distance = terrain3dInstanceCacheDistance(
				{old_location.x, old_location.y}, {center.x, center.y} );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				target_slot = slot;
			}
		}
	}
	if ( target_slot < 0 ) {
		return false;
	}
	if ( m_szInstanceRegionLocations[target_slot] != V2I_MAX && !evictInstanceRegionSlot( target_slot ) ) {
		return false;
	}
	if ( !pRegion->ensureInstanceStoreResident() ) {
		return false;
	}
	const size_t bytes = pRegion->getInstanceResidentBytes();
	const size_t budget = ( size_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	while ( bDemandLoad && m_nInstanceRegionResidentCount > 0 &&
		m_nInstanceRegionResidentBytes + bytes > budget ) {
		int farthest_slot = -1;
		int farthest_distance = -1;
		const Vector2i center = m_instanceRegionCacheCenter == V2I_MAX ? vLocation : m_instanceRegionCacheCenter;
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			const Vector2i old_location = m_szInstanceRegionLocations[slot];
			if ( old_location == V2I_MAX || old_location == vLocation ) {
				continue;
			}
			const int distance = terrain3dInstanceCacheDistance(
				{old_location.x, old_location.y}, {center.x, center.y} );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				farthest_slot = slot;
			}
		}
		if ( farthest_slot < 0 || !evictInstanceRegionSlot( farthest_slot ) ) {
			break;
		}
		++m_nInstanceRegionEvictionsLastTick;
	}
	if ( !bDemandLoad && !terrain3dInstanceCacheCanStream( m_nInstanceRegionResidentBytes, bytes, budget,
		m_nInstanceRegionResidentCount ) ) {
		pRegion->evictInstanceStore();
		return false;
	}
	m_szInstanceRegionLocations[target_slot] = vLocation;
	const int map_index = getRegionMapIndex( vLocation );
	if ( map_index >= 0 ) {
		m_szInstanceRegionMap[map_index] = ( uint8_t ) ( target_slot + 1 );
	}
	++m_nInstanceRegionResidentCount;
	m_nInstanceRegionResidentBytes += bytes;
	if ( bDemandLoad ) {
		++m_nInstanceRegionDemandLoads;
	}
	if ( m_pTerrain && m_pTerrain->getInstancer() ) {
		m_pTerrain->getInstancer()->updateMmis( -1, vLocation, false );
	}
	return true;
}

int T3DData::collectDesiredInstanceRegions(
	const T3DResidencyFrameState &rFrame, const Vector2i &rCenter, Vector2i *pLocations,
	const int nCapacity
) const {
	if ( !pLocations || nCapacity <= 0 ) {
		return 0;
	}
	const int capacity = MIN( nCapacity, TERRAIN3D_INSTANCE_CACHE_MAX_PAGES );
	int output_count = 0;
	for ( int state_value = ( int ) ( TERRAIN3D_REGION_STREAMING_ACTIVE );
		state_value >= ( int ) ( TERRAIN3D_REGION_STREAMING_HOT ) && output_count < capacity; --state_value ) {
		const t3dregionStreamingState_t wanted = ( t3dregionStreamingState_t ) ( state_value );
		terrain3dForEachSquareRing( rCenter, m_regionRepository.getRegionMapSize(), [&]( const Vector2i &location ) {
			if ( output_count >= capacity ) {
				return false;
			}
			if ( !isRegionLocationValidInternal( location ) || !hasRegion( location ) ||
				rFrame.streamingState( location ) != wanted ) {
				return true;
			}
			const T3DRegion *pRegion = getRegionPtr( location );
			if ( pRegion && pRegion->hasInstances() ) {
				pLocations[output_count++] = location;
			}
			return output_count < capacity;
		} );
	}
	return output_count;
}

void T3DData::configureInstanceRegionCache(
	const bool bEnabled, const int nBudgetMb,
	const int nLoadPages, const int nLoadBudgetMb
) {
	const bool bWasEnabled = m_bInstanceRegionCacheEnabled;
	m_bInstanceRegionCacheEnabled = bEnabled;
	m_nInstanceRegionCacheBudgetMb = CLAMP( nBudgetMb, TERRAIN3D_INSTANCE_REGION_CACHE_MIN_BUDGET_MB,
		TERRAIN3D_INSTANCE_REGION_CACHE_MAX_BUDGET_MB );
	m_nInstanceRegionLoadBudgetPages = CLAMP( nLoadPages, 1, TERRAIN3D_INSTANCE_REGION_LOAD_MAX_PAGES );
	m_nInstanceRegionLoadBudgetMb = CLAMP( nLoadBudgetMb, 1, TERRAIN3D_INSTANCE_REGION_LOAD_MAX_BUDGET_MB );
	if ( bWasEnabled && !bEnabled ) {
		resetInstanceRegionCache( true );
		return;
	}
	if ( !bWasEnabled && bEnabled ) {
		resetInstanceRegionCache( false );
	}
	m_bInstanceRegionCacheSelectionDirty = true;
}

void T3DData::updateInstanceRegionResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	const T3DLastScopeTimer serviceTimer( m_nInstanceRegionServiceUsecLast, rFrame.bProfilingEnabled );
	m_nInstanceRegionLoadsLastTick = 0;
	m_nInstanceRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_nInstanceRegionEvictionsLastTick = 0;
	m_nInstanceRegionLoadBytesLastTick = 0u;
	if ( !m_bInstanceRegionCacheEnabled || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getVertexSpacing() <= 0.0f ) {
		return;
	}
	const bool async_runtime = !rFrame.bAuthoring && rFrame.bCookedPagesEnabled;
	const uint64_t streaming_generation = rFrame.streamingGeneration;
	if ( async_runtime && ( bForce || streaming_generation != m_nInstancePrefetchStreamingGenerationLast ) ) {
		// Predictive instance demand changes only when the streaming generation changes.
		// Avoid walking the authored region table on stationary frames.
		for ( int slot = 0; slot < m_regionRepository.getSlotCount(); ++slot ) {
			const Vector2i location = m_regionRepository.getSlotsData()[slot];
			T3DRegion *const pRegion = location == V2I_MAX ? nullptr : getRegionPtr( location );
			if ( !pRegion || pRegion->isDeleted() || !pRegion->hasInstances() ||
				pRegion->isInstanceStoreResident() || findInstanceRegionSlot( location ) >= 0 ||
				!rFrame.isPrefetch( location ) ) {
				continue;
			}
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, 3u );
		}
		m_nInstancePrefetchStreamingGenerationLast = streaming_generation;
	}
	const Vector2i center = rFrame.residencyCenter;
	if ( !bForce && center == m_instanceRegionCacheCenter && !m_bInstanceRegionCacheSelectionDirty ) {
		return;
	}
	Vector2i desired[TERRAIN3D_INSTANCE_CACHE_MAX_PAGES];
	const int desired_count = collectDesiredInstanceRegions( rFrame, center, desired, m_nInstanceRegionCacheCapacity );
	m_nInstanceRegionDesiredLastTick = desired_count;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
		if ( map_index >= 0 ) {
			desired_map[map_index] = 1u;
		}
	}
	const size_t resident_budget = ( size_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	while ( m_nInstanceRegionResidentCount > 1 && m_nInstanceRegionResidentBytes > resident_budget ) {
		int farthest_slot = -1;
		int farthest_distance = -1;
		for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
			const Vector2i location = m_szInstanceRegionLocations[slot];
			if ( location == V2I_MAX ) {
				continue;
			}
			const int distance = terrain3dInstanceCacheDistance(
				{location.x, location.y}, {center.x, center.y} );
			if ( distance > farthest_distance ) {
				farthest_distance = distance;
				farthest_slot = slot;
			}
		}
		if ( farthest_slot < 0 || !evictInstanceRegionSlot( farthest_slot ) ) {
			break;
		}
		++m_nInstanceRegionEvictionsLastTick;
	}
	for ( int slot = 0; slot < m_nInstanceRegionCacheCapacity; ++slot ) {
		const Vector2i location = m_szInstanceRegionLocations[slot];
		const int map_index = getRegionMapIndex( location );
		if ( location != V2I_MAX && ( map_index < 0 || desired_map[map_index] == 0u ) ) {
			if ( evictInstanceRegionSlot( slot ) ) {
				++m_nInstanceRegionEvictionsLastTick;
			}
		}
	}
	const size_t load_budget = ( size_t ) ( m_nInstanceRegionLoadBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	const int load_page_budget = getEffectiveInstanceRegionLoadBudgetPages();
	bool bComplete = true;
	for ( int index = 0; index < desired_count; ++index ) {
		const Vector2i location = desired[index];
		if ( findInstanceRegionSlot( location ) >= 0 ) {
			continue;
		}
		bComplete = false;
		if ( !bForce && m_nInstanceRegionLoadsLastTick >= load_page_budget ) {
			m_nInstanceRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BUDGET;
			break;
		}
		T3DRegion *const pRegion = getRegionPtr( location );
		const size_t estimated = pRegion ? MAX<size_t>( pRegion->getInstancePageBytes(), 1u ) : 0u;
		if ( !bForce && m_nInstanceRegionLoadsLastTick > 0 &&
			m_nInstanceRegionLoadBytesLastTick + estimated > load_budget ) {
			m_nInstanceRegionStallFlagsLastTick |= TERRAIN3D_PROFILE_STALL_INSTANCE_LOAD_BYTE_BUDGET;
			break;
		}
		if ( async_runtime && pRegion && !pRegion->isInstanceStoreResident() ) {
			const uint8_t priority = rFrame.streamingState( location ) ==
				TERRAIN3D_REGION_STREAMING_ACTIVE ? 1u : 2u;
			requestGpuRuntimePage( location, RUNTIME_PAGE_CONSUMER_INSTANCE_STORE, priority );
			T3DInstanceStore prepared_store;
			if ( !takePreparedRuntimeInstance( location, prepared_store ) ) {
				continue;
			}
			pRegion->m_instanceStore = eastl::move( prepared_store );
			pRegion->m_bInstanceStoreResident = true;
			pRegion->m_bInstanceStoreModified = false;
		}
		if ( ensureInstanceRegionResidentInternal( location, false ) ) {
			++m_nInstanceRegionLoadsLastTick;
			m_nInstanceRegionLoadBytesLastTick += estimated;
		}
	}
	if ( !bComplete ) {
		bComplete = true;
		for ( int index = 0; index < desired_count; ++index ) {
			if ( findInstanceRegionSlot( desired[index] ) < 0 ) {
				bComplete = false;
				break;
			}
		}
	}
	m_instanceRegionCacheCenter = center;
	m_bInstanceRegionCacheSelectionDirty = !bComplete;
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
	Dictionary stats;
	stats["enabled"] = m_bInstanceRegionCacheEnabled;
	stats["budget_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["capacity"] = m_nInstanceRegionCacheCapacity;
	stats["resident"] = m_nInstanceRegionResidentCount;
	stats["resident_bytes"] = ( int64_t ) ( m_nInstanceRegionResidentBytes );
	stats["desired_last_tick"] = m_nInstanceRegionDesiredLastTick;
	stats["loads_last_tick"] = m_nInstanceRegionLoadsLastTick;
	stats["evictions_last_tick"] = m_nInstanceRegionEvictionsLastTick;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nInstanceRegionLoadBytesLastTick );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nInstanceRegionServiceUsecLast );
	stats["demand_loads"] = m_nInstanceRegionDemandLoads;
	stats["selection_pending"] = m_bInstanceRegionCacheSelectionDirty;
	return stats;
}

Dictionary T3DData::getInstanceRegionCacheStats( void ) const {
	Dictionary stats;
	stats["enabled"] = m_bInstanceRegionCacheEnabled;
	stats["budget_mb"] = m_nInstanceRegionCacheBudgetMb;
	stats["capacity"] = m_nInstanceRegionCacheCapacity;
	stats["resident"] = m_nInstanceRegionResidentCount;
	stats["resident_bytes"] = ( int64_t ) ( m_nInstanceRegionResidentBytes );
	stats["desired_last_tick"] = m_nInstanceRegionDesiredLastTick;
	stats["load_page_budget"] = m_nInstanceRegionLoadBudgetPages;
	stats["effective_load_page_budget"] = getEffectiveInstanceRegionLoadBudgetPages();
	stats["load_budget_mb"] = m_nInstanceRegionLoadBudgetMb;
	stats["loads_last_tick"] = m_nInstanceRegionLoadsLastTick;
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nInstanceRegionStallFlagsLastTick );
	stats["evictions_last_tick"] = m_nInstanceRegionEvictionsLastTick;
	stats["load_bytes_last_tick"] = ( int64_t ) ( m_nInstanceRegionLoadBytesLastTick );
	stats["demand_loads"] = m_nInstanceRegionDemandLoads;
	stats["center"] = m_instanceRegionCacheCenter;
	stats["selection_pending"] = m_bInstanceRegionCacheSelectionDirty;
	stats["budget_overcommitted"] = m_nInstanceRegionResidentBytes >
		( size_t ) ( m_nInstanceRegionCacheBudgetMb ) * TERRAIN3D_INSTANCE_CACHE_MIB;
	return stats;
}

void T3DData::updateMapsBounded( const t3dmapType_t eMapType, const bool bAllRegions ) {
	const bool bImplicitStateChanged = refreshImplicitMapState();
	bool bTopologyChanged = false;
	if ( m_regionRepository.isRegionMapDirty() ) {
		m_regionRepository.rebuildRegionMapFromSlots();
		bTopologyChanged = true;
		m_bGpuRegionCacheSelectionDirty = true;
		if ( !m_bStartupBulkLoad ) {
			emit_signal( "region_map_changed" );
		}
	}

	const int required_capacity = calculateGpuRegionCacheCapacity();
	const bool bRebuild = !m_bGpuRegionCacheTexturesReady || bImplicitStateChanged ||
		required_capacity != m_nGpuRegionCacheCapacity || m_generatedHeightMaps.isDirty() ||
		m_generatedControlMaps.isDirty() || m_generatedColorMaps.isDirty() ||
		m_generatedDerivedMaps.isDirty();
	if ( bRebuild && !rebuildGpuRegionCacheTextures() ) {
		return;
	}

	bool bHeightChanged = false;
	bool bControlChanged = false;
	bool bColorChanged = false;
	bool bPhysicalMappingChanged = false;
	if ( bAllRegions && !bRebuild ) {
		Vector2i resident_locations[TERRAIN3D_GPU_CACHE_MAX_LAYERS];
		int resident_location_count = 0;
		for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
			if ( m_szGpuRegionLocations[layer] != V2I_MAX &&
				m_szGpuRegionSlotStates[layer] == TERRAIN3D_GPU_SLOT_MAPPED ) {
				resident_locations[resident_location_count++] = m_szGpuRegionLocations[layer];
			}
		}
		for ( int index = 0; index < resident_location_count; ++index ) {
			bHeightChanged = bHeightChanged || eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX;
			if ( republishGpuRegionLocation( resident_locations[index] ) ) {
				bPhysicalMappingChanged = true;
			}
		}
	}

	if ( eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX ) {
		calcHeightRange();
	}
	if ( bRebuild || bTopologyChanged ) {
		const Vector2i center = m_gpuRegionCacheCenter == V2I_MAX ? V2I_ZERO : m_gpuRegionCacheCenter;
		const real_t fWorldRegionSize = ( real_t ) ( m_regionRepository.getRegionSize() ) * m_regionRepository.getVertexSpacing();
		const Vector3 centerPosition(
			( ( real_t ) ( center.x ) + 0.5f ) * fWorldRegionSize, 0.0f,
			( ( real_t ) ( center.y ) + 0.5f ) * fWorldRegionSize
		);
		// Initial population obeys the same upload ceiling as ordinary traversal.
		// Rebuilding the physical arrays must not synchronously refill the cache.
		updateGpuRegionResidency( centerPosition, false );
	}

	if ( bPhysicalMappingChanged ) {
		emitGpuRegionMapChanged();
	}
	if ( !m_bStartupBulkLoad ) {
		if ( bHeightChanged || bRebuild ) {
			emit_signal( "height_maps_changed" );
		}
		if ( bControlChanged || bRebuild || bImplicitStateChanged ) {
			emit_signal( "control_maps_changed" );
		}
		if ( bColorChanged || bRebuild || bImplicitStateChanged ) {
			emit_signal( "color_maps_changed" );
		}
		if ( bRebuild || bImplicitStateChanged ) {
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
	if ( m_bCpuRegionCacheEnabled ) {
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
		m_activeHeightResidency( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore ),
		m_regionResidency( &m_regionRepository, &m_samplingStorage, &m_runtimePageStore, &m_activeHeightResidency ) {
	std::fill_n( m_szGpuRegionLocations, TERRAIN3D_GPU_CACHE_MAX_LAYERS, V2I_MAX );
}

T3DData::~T3DData( void ) {
	clear();
	// The far-field atlas is intentionally retained across normal data reloads,
	// but destruction must release its RenderingServer RIDs and retained Images.
	m_generatedFarHeightMap.clear();
	m_generatedFarColorMap.clear();
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
	const bool color_channel_enabled = isGpuRegionColorChannelEnabled();
	const bool derived_channel_enabled = isGpuRegionDerivedChannelEnabled();
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
		return;
	}
	m_nGpuRegionRequestedPages = requested_pages;
	m_nGpuRegionRequiredRadius = required_radius;
	m_bGpuRegionCacheSelectionDirty = m_bGpuRegionCacheSelectionDirty || bSelectionChanged;
	if ( !bOldEnabled && !m_bGpuRegionCacheEnabled ) {
		return;
	}
	if ( bStorageChanged ) {
		resetGpuRegionCache( true );
		if ( m_pTerrain && m_regionRepository.getRegionSize() > 0 ) {
			updateMaps( T3DRegion::TYPE_MAX, true, false );
		}
	}
}

void T3DData::updateGpuRegionResidencyInternal( const T3DResidencyFrameState &rFrame, const bool bForce ) {
	const T3DLastScopeTimer serviceTimer( m_nGpuRegionServiceUsecLast, rFrame.bProfilingEnabled );
	m_nGpuRegionStallFlagsLastTick = TERRAIN3D_PROFILE_STALL_NONE;
	m_runtimePageStore.resetFrameStats();
	m_nGpuPreparePublicationUsecLast = 0u;
	m_nGpuPrepareEndToEndUsecLast = 0u;
	pollGpuRuntimePagePrepare( false );
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
	m_nGpuFarHeightUpdatesLastTick = 0;
	m_nGpuFarColorUpdatesLastTick = 0;
	m_nGpuRegionUploadBytesLastTick = 0u;
	m_nGpuRegionHeightBytesLastTick = 0u;
	m_nGpuRegionControlBytesLastTick = 0u;
	m_nGpuRegionColorBytesLastTick = 0u;
	m_nGpuRegionDerivedBytesLastTick = 0u;
	m_nGpuRegionHysteresisLastTick = 0;
	m_nGpuRegionMissingRequiredLastTick = 0;
	if ( !m_bGpuRegionCacheEnabled || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getVertexSpacing() <= 0.0f ) {
		return;
	}
	++m_nGpuRegionTick;
	reclaimRetiredGpuRegionSlots();
	const uint64_t streaming_generation = rFrame.streamingGeneration;
	const Vector2i prediction_delta = rFrame.predictedCenter - rFrame.streamingCenter;
	m_gpuRegionPredictionDirection = Vector2i( SIGN( prediction_delta.x ), SIGN( prediction_delta.y ) );
	if ( !m_bGpuRegionCacheTexturesReady && !rebuildGpuRegionCacheTextures() ) {
		return;
	}
	const T3DEditor *pTerrainEditor = m_pTerrain ? m_pTerrain->getEditor() : nullptr;
	const bool bDeferFarAtlasPublish = rFrame.bAuthoring &&
		pTerrainEditor && pTerrainEditor->isOperating();
	// Brush motion can commit several stamps per frame. Keep the resident page
	// updates immediate, but publish the full far atlases once after the stroke.
	if ( !bDeferFarAtlasPublish ) {
		if ( m_bGpuFarHeightDirty && !rebuildGpuFarHeightAtlas() ) {
			return;
		}
		if ( m_bGpuFarColorDirty && !rebuildGpuFarColorAtlas() ) {
			return;
		}
	}

	const Vector2i center = rFrame.residencyCenter;
	m_bGpuRegionCameraMovedLastTick = m_gpuRegionCacheCenter != V2I_MAX &&
		center != m_gpuRegionCacheCenter;
	const int runtime_upload_budget = getEffectiveGpuRegionUploadBudgetLayers();
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
			emitGpuRegionMapChanged();
		}
		return;
	}

	Vector2i desired[TERRAIN3D_GPU_CACHE_MAX_LAYERS];
	t3dgpuCacheSelectionStats_t selection_stats;
	int desired_count = collectDesiredGpuRegions( rFrame, center, desired, m_nGpuRegionCacheCapacity, selection_stats );
	const int footprint_count = desired_count;
	m_nGpuRegionRequiredLastTick = selection_stats.required;
	m_nGpuRegionGuardLastTick = selection_stats.guard;
	m_nGpuRegionPrefetchLastTick = selection_stats.prefetch;
	uint8_t desired_map[TERRAIN3D_MAX_REGION_SLOTS] = {};
	for ( int index = 0; index < desired_count; ++index ) {
		const int map_index = getRegionMapIndex( desired[index] );
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
		const int map_index = getRegionMapIndex( location );
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
			T3DRegion *const pRegion = getRegionPtr( desired[index] );
			if ( pRegion && !pRegion->isDeleted() && pRegion->runtimeMapsEvictable() ) {
				const uint8_t priority = index < m_nGpuRegionPriorityCount ?
					m_szGpuRegionPriorityClasses[index] : 4u;
				requestGpuRuntimePage( desired[index], RUNTIME_PAGE_CONSUMER_GPU_DETAIL, priority );
			}
		}
		serviceGpuRuntimePagePrepare( rFrame );
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
		T3DRegion *const pUploadRegion = getRegionPtr( location );
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
				const int old_map_index = getRegionMapIndex( old_location );
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
		if ( !uploadGpuRegionLayer( target_layer, location, T3DRegion::TYPE_MAX ) ) {
			continue;
		}

		if ( victim_layer >= 0 ) {
			const Vector2i old_location = m_szGpuRegionLocations[victim_layer];
			const int old_map_index = getRegionMapIndex( old_location );
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
		const int new_map_index = getRegionMapIndex( location );
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
		emitGpuRegionMapChanged();
	}
}

void T3DData::invalidateGpuFarSurface( void ) {
	if ( !isFarAtlasRequired() ) {
		return;
	}
	// Surface quality may change the atlas dimensions. Recreate only the surface
	// representation; the Far height lattice is geometry-owned and stays intact.
	m_generatedFarColorMap.clear();
	m_pGpuFarColorImage.unref();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarColorDirty = true;
	m_bGpuFarColorDirtyAll = true;
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
	if ( !m_bGpuRegionCacheEnabled ) {
		ensureFarAtlases();
	}
}

void T3DData::invalidateGpuFarAtlasLayout( void ) {
	m_generatedFarHeightMap.clear();
	m_generatedFarColorMap.clear();
	m_pGpuFarHeightImage.unref();
	m_pGpuFarColorImage.unref();
	m_gpuFarHeightDirtyRegions.reset();
	m_gpuFarColorDirtyRegions.reset();
	m_bGpuFarHeightDirty = true;
	m_bGpuFarColorDirty = true;
	m_bGpuFarHeightDirtyAll = true;
	m_bGpuFarColorDirtyAll = true;
	m_bGpuFarCookedLoaded = false;
	m_bGpuFarCookedSaved = false;
	if ( isFarAtlasRequired() && m_samplingStorage.isActiveOwner() && m_regionRepository.getRegionSize() > 0 && m_regionRepository.getRegionMapSize() > 0 ) {
		ensureFarAtlases();
	}
}

bool T3DData::ensureFarAtlases( void ) {
	if ( !isFarAtlasRequired() || !m_samplingStorage.isActiveOwner() || m_regionRepository.getRegionSize() <= 0 || m_regionRepository.getRegionMapSize() <= 0 ) {
		return false;
	}
	if ( ( m_pGpuFarHeightImage.is_null() || m_pGpuFarColorImage.is_null() ) &&
		!loadCookedGpuFarAtlases() ) {
		markAllGpuFarRegionsDirty( T3DRegion::TYPE_MAX );
	}
	if ( m_bGpuFarHeightDirty || !m_generatedFarHeightMap.getRid().is_valid() ) {
		if ( !rebuildGpuFarHeightAtlas() ) {
			return false;
		}
	}
	if ( m_bGpuFarColorDirty || !m_generatedFarColorMap.getRid().is_valid() ) {
		if ( !rebuildGpuFarColorAtlas() ) {
			return false;
		}
	}
	const bool bReady = m_generatedFarHeightMap.getRid().is_valid() &&
		m_generatedFarColorMap.getRid().is_valid();
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
			const int map_index = getRegionMapIndex( location );
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
			// Compatibility field for older profiler consumers: 1/2/3 remain the
			// high-to-low surface-page priority classes.
			cell["desired_class"] = state == TERRAIN3D_REGION_STREAMING_ACTIVE ? 1 :
				( state == TERRAIN3D_REGION_STREAMING_HOT ? 2 : ( predicted ? 3 : 0 ) );
			const int gpu_slot = findGpuRegionSlot( location );
			cell["gpu_resident"] = gpu_slot >= 0;
			cell["gpu_layer"] = gpu_slot;
			cell["middle_height_resident"] = findMiddleHeightSlot( location ) >= 0;
			cell["cpu_resident"] = findCpuRegionSlot( location ) >= 0;
			cell["instance_resident"] = findInstanceRegionSlot( location ) >= 0;
			const bool bPrepared = m_runtimePageStore.isPrepared( location );
			cell["prepared"] = bPrepared;
			cell["prepare_pending"] = isGpuRuntimePagePreparePending( location );
			int slot_state = TERRAIN3D_GPU_SLOT_FREE;
			for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
				if ( m_szGpuRegionLocations[layer] == location ) {
					slot_state = ( int ) ( m_szGpuRegionSlotStates[layer] );
					break;
				}
			}
			cell["gpu_slot_state"] = slot_state;
			cell["far_height_dirty"] = map_index >= 0 &&
				( m_bGpuFarHeightDirtyAll || m_gpuFarHeightDirtyRegions.test( ( size_t ) ( map_index ) ) );
			cell["far_color_dirty"] = map_index >= 0 &&
				( m_bGpuFarColorDirtyAll || m_gpuFarColorDirtyRegions.test( ( size_t ) ( map_index ) ) );
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
	Dictionary stats;
	const T3DRuntimePageStats runtime_page_stats = m_runtimePageStore.getStats();
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["budget_mb"] = m_nGpuRegionCacheBudgetMb;
	stats["allocation_budget_mb"] = m_pTerrain && m_pTerrain->isEditorAuthoringMode() ?
		calculateAuthoringGpuRegionBudgetMb() : m_nGpuRegionCacheBudgetMb;
	stats["capacity"] = m_nGpuRegionCacheCapacity;
	stats["physical_layers"] = m_nGpuRegionPhysicalLayerCount;
	stats["resident"] = m_nGpuRegionResidentCount;
	stats["republish_pending"] = m_nGpuRegionRepublishPendingCount;
	stats["requested_pages"] = m_nGpuRegionRequestedPages;
	stats["required_last_tick"] = m_nGpuRegionRequiredLastTick;
	stats["missing_required_last_tick"] = m_nGpuRegionMissingRequiredLastTick;
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t ) ( m_nGpuRegionUploadsSinceReset );
	stats["evictions_last_tick"] = m_nGpuRegionEvictionsLastTick;
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	const int cooked_attempts = m_nGpuRegionCookedHitsLastTick + m_nGpuRegionCookedMissesLastTick;
	stats["cooked_hit_ratio_last_tick"] = cooked_attempts > 0 ?
		( double ) ( m_nGpuRegionCookedHitsLastTick ) / ( double ) ( cooked_attempts ) : -1.0;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["prepare_job_active"] = runtime_page_stats.bPrepareJobActive;
	stats["prepare_job_pending_pages"] = runtime_page_stats.nPendingPages;
	stats["prepare_ready_pages"] = runtime_page_stats.nPreparedPages;
	stats["prepare_publication_usec_last"] = ( int64_t ) ( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t ) ( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nGpuRegionServiceUsecLast );
	stats["far_height_dirty_regions"] = m_bGpuFarHeightDirtyAll ? m_regionRepository.getActiveRegionCount() : ( int ) ( m_gpuFarHeightDirtyRegions.count() );
	stats["far_color_dirty_regions"] = m_bGpuFarColorDirtyAll ? m_regionRepository.getActiveRegionCount() : ( int ) ( m_gpuFarColorDirtyRegions.count() );
	stats["far_height_processing_usec_last"] = ( int64_t ) ( m_nGpuFarHeightProcessingUsecLast );
	stats["far_color_processing_usec_last"] = ( int64_t ) ( m_nGpuFarColorProcessingUsecLast );
	stats["far_height_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarHeightDirtyTexelsLast );
	stats["far_color_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarColorDirtyTexelsLast );
	stats["far_height_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightRebuiltBytesLast );
	stats["far_color_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarColorRebuiltBytesLast );
	stats["far_height_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesLast );
	stats["far_color_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesLast );
	stats["far_height_publication_usec_last"] = ( int64_t ) ( m_nGpuFarHeightPublicationUsecLast );
	stats["far_color_publication_usec_last"] = ( int64_t ) ( m_nGpuFarColorPublicationUsecLast );
	stats["far_height_publication_count"] = ( int64_t ) ( m_nGpuFarHeightPublicationCount );
	stats["far_color_publication_count"] = ( int64_t ) ( m_nGpuFarColorPublicationCount );
	stats["far_height_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesTotal );
	stats["far_color_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesTotal );
	stats["far_partial_publication_available"] = false;
	stats["far_publication_path"] = "RenderingServer full Texture2D update";
	stats["upload_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionUploadBytesLastTick );
	stats["reserved_bytes"] = ( int64_t ) ( getGpuRegionCacheReservedBytes() );
	stats["authoring_bounded_pool_covers_all_regions"] = isAuthoringGpuFullResidencyPossible();
	stats["authoring_readback_safe"] = isAuthoringGpuReadbackSafe();
	return stats;
}

bool T3DData::isAuthoringGpuFullResidencyPossible( void ) const {
	return !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ||
		calculateGpuRegionCacheCapacity() >= m_regionRepository.getActiveRegionCount();
}

bool T3DData::isAuthoringGpuReadbackSafe( void ) const {
	if ( !m_pTerrain || !m_pTerrain->isEditorAuthoringMode() ) {
		return true;
	}
	if ( !m_bGpuRegionCacheEnabled ) {
		return true;
	}
	if ( !m_bGpuRegionCacheTexturesReady || m_runtimePageStore.isPrepareJobActive() ||
		m_bGpuRegionCacheSelectionDirty || m_nGpuRegionUploadsLastTick > 0 ) {
		return false;
	}
	return m_nGpuRegionTick >= m_nGpuRegionLastPublicationTick + 2u;
}

Dictionary T3DData::getStartupLoadStats( void ) const {
	return m_regionPersistence.getStartupLoadStatsDictionary();
}

Dictionary T3DData::getGpuRegionCacheStats( void ) const {
	Dictionary stats;
	const T3DRuntimePageStats runtime_page_stats = m_runtimePageStore.getStats();
	stats["enabled"] = m_bGpuRegionCacheEnabled;
	stats["budget_mb"] = m_nGpuRegionCacheBudgetMb;
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
	const int required_resident = MAX( 0, m_nGpuRegionRequiredLastTick - m_nGpuRegionMissingRequiredLastTick );
	stats["required_resident_last_tick"] = required_resident;
	stats["required_residency_ratio"] = m_nGpuRegionRequiredLastTick > 0 ?
		( double ) ( required_resident ) / ( double ) ( m_nGpuRegionRequiredLastTick ) : 1.0;
	stats["stall_flags_last_tick"] = ( int64_t ) ( m_nGpuRegionStallFlagsLastTick );
	stats["prediction_direction"] = m_gpuRegionPredictionDirection;
	stats["streaming_generation_last"] = ( int64_t ) ( m_nGpuRegionStreamingGenerationLast );
	stats["upload_layer_budget"] = m_nGpuRegionUploadBudgetLayers;
	stats["runtime_upload_layer_budget"] = getEffectiveGpuRegionUploadBudgetLayers();
	stats["upload_budget_mb"] = m_nGpuRegionUploadBudgetMb;
	stats["upload_update_budget"] = MAX( 5, getEffectiveGpuRegionUploadBudgetLayers() * 3 + 1 );
	stats["uploads_last_tick"] = m_nGpuRegionUploadsLastTick;
	stats["uploads_since_reset"] = ( int64_t ) ( m_nGpuRegionUploadsSinceReset );
	stats["evictions_last_tick"] = m_nGpuRegionEvictionsLastTick;
	stats["cooked_hits_last_tick"] = m_nGpuRegionCookedHitsLastTick;
	stats["cooked_misses_last_tick"] = m_nGpuRegionCookedMissesLastTick;
	const int cooked_attempts = m_nGpuRegionCookedHitsLastTick + m_nGpuRegionCookedMissesLastTick;
	stats["cooked_hit_ratio_last_tick"] = cooked_attempts > 0 ?
		( double ) ( m_nGpuRegionCookedHitsLastTick ) / ( double ) ( cooked_attempts ) : -1.0;
	stats["prefetch_used_total"] = ( int64_t ) ( m_nGpuRegionPrefetchUsedTotal );
	stats["prefetch_discarded_total"] = ( int64_t ) ( m_nGpuRegionPrefetchDiscardedTotal );
	const uint64_t completed_prefetches = m_nGpuRegionPrefetchUsedTotal + m_nGpuRegionPrefetchDiscardedTotal;
	stats["prefetch_accuracy"] = completed_prefetches > 0u ?
		( double ) ( m_nGpuRegionPrefetchUsedTotal ) / ( double ) ( completed_prefetches ) : -1.0;
	int pending_prefetches = 0;
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		pending_prefetches += m_szGpuRegionPrefetchOrigin[layer] != 0u ? 1 : 0;
	}
	stats["prefetch_pending"] = pending_prefetches;
	Array pending_requests;
	const int pending_request_limit = 16;
	int pending_request_count = 0;
	for ( int index = 0; index < m_nGpuRegionPriorityCount; ++index ) {
		const Vector2i location = m_szGpuRegionPriorityLocations[index];
		if ( location == V2I_MAX || findGpuRegionSlot( location ) >= 0 ) {
			continue;
		}
		++pending_request_count;
		if ( pending_requests.size() >= pending_request_limit ) {
			continue;
		}
		Dictionary request;
		request["priority_rank"] = index;
		request["location"] = location;
		request["offset"] = location - m_gpuRegionCacheCenter;
		request["distance_regions"] = MAX( std::abs( location.x - m_gpuRegionCacheCenter.x ),
			std::abs( location.y - m_gpuRegionCacheCenter.y ) );
		request["prediction_alignment"] = ( location.x - m_gpuRegionCacheCenter.x ) * m_gpuRegionPredictionDirection.x +
			( location.y - m_gpuRegionCacheCenter.y ) * m_gpuRegionPredictionDirection.y;
		const uint8_t request_class = m_szGpuRegionPriorityClasses[index];
		request["class_id"] = ( int ) ( request_class );
		request["class"] = request_class == 1u ? "required" :
			request_class == 2u ? "guard" : request_class == 3u ? "prefetch" : "hysteresis";
		const bool bPrepared = m_runtimePageStore.isPrepared( location );
		request["prepared"] = bPrepared;
		request["prepare_pending"] = isGpuRuntimePagePreparePending( location );
		const T3DRegion *pRegion = getRegionPtr( location );
		request["active"] = pRegion && !pRegion->isDeleted();
		request["runtime_maps_evictable"] = pRegion && pRegion->runtimeMapsEvictable();
		pending_requests.push_back( request );
	}
	stats["pending_requests"] = pending_requests;
	stats["pending_requests_reported"] = pending_requests.size();
	stats["pending_requests_total"] = pending_request_count;
	stats["departed_pages_total"] = ( int64_t ) ( m_nGpuRegionDepartedPagesTotal );
	stats["average_resident_ticks"] = m_nGpuRegionDepartedPagesTotal > 0u ?
		( double ) ( m_nGpuRegionResidentTicksTotal ) / ( double ) ( m_nGpuRegionDepartedPagesTotal ) : 0.0;
	stats["runtime_prepares_last_tick"] = m_nGpuRegionRuntimePreparesLastTick;
	stats["reused_layers_last_tick"] = m_nGpuRegionReusedLayersLastTick;
	stats["page_table_updates_last_tick"] = m_nGpuRegionPageTableUpdatesLastTick;
	stats["height_updates_last_tick"] = m_nGpuRegionHeightUpdatesLastTick;
	stats["control_updates_last_tick"] = m_nGpuRegionControlUpdatesLastTick;
	stats["color_updates_last_tick"] = m_nGpuRegionColorUpdatesLastTick;
	stats["derived_updates_last_tick"] = m_nGpuRegionDerivedUpdatesLastTick;
	stats["runtime_color_mips_last_tick"] = m_nGpuRegionRuntimeColorMipsLastTick;
	stats["runtime_derived_builds_last_tick"] = m_nGpuRegionRuntimeDerivedBuildsLastTick;
	stats["prepared_pages_last_tick"] = m_nGpuRegionPreparedPagesLastTick;
	stats["prepare_workers_last_tick"] = m_nGpuRegionPrepareWorkersLastTick;
	stats["prepare_job_active"] = runtime_page_stats.bPrepareJobActive;
	stats["prepare_job_pending_pages"] = runtime_page_stats.nPendingPages;
	stats["prepare_ready_pages"] = runtime_page_stats.nPreparedPages;
	stats["prepare_latency_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareLatencyUsecLast );
	stats["prepare_queue_wait_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareQueueWaitUsecLast );
	stats["prepare_file_open_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareFileOpenUsecLast );
	stats["prepare_file_read_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareFileReadUsecLast );
	stats["prepare_validation_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareValidationUsecLast );
	stats["prepare_materialize_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareMaterializeUsecLast );
	stats["prepare_worker_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareWorkerUsecLast );
	stats["prepare_ready_wait_usec_last"] = ( int64_t ) ( runtime_page_stats.nPrepareReadyWaitUsecLast );
	stats["prepare_publication_usec_last"] = ( int64_t ) ( m_nGpuPreparePublicationUsecLast );
	stats["prepare_end_to_end_usec_last"] = ( int64_t ) ( m_nGpuPrepareEndToEndUsecLast );
	stats["service_cpu_usec_last"] = ( int64_t ) ( m_nGpuRegionServiceUsecLast );
	stats["effective_upload_limit_last_tick"] = m_nGpuRegionEffectiveUploadLimitLastTick;
	stats["camera_moved_last_tick"] = m_bGpuRegionCameraMovedLastTick;
	stats["far_height_updates_last_tick"] = m_nGpuFarHeightUpdatesLastTick;
	stats["far_color_updates_last_tick"] = m_nGpuFarColorUpdatesLastTick;
	stats["far_height_regions_processed_last_tick"] = m_nGpuFarHeightRegionsLastTick;
	stats["far_color_regions_processed_last_tick"] = m_nGpuFarColorRegionsLastTick;
	stats["far_height_dirty_regions"] = m_bGpuFarHeightDirtyAll ? m_regionRepository.getActiveRegionCount() :
		( int ) ( m_gpuFarHeightDirtyRegions.count() );
	stats["far_color_dirty_regions"] = m_bGpuFarColorDirtyAll ? m_regionRepository.getActiveRegionCount() :
		( int ) ( m_gpuFarColorDirtyRegions.count() );
	stats["far_height_processing_usec_last"] = ( int64_t ) ( m_nGpuFarHeightProcessingUsecLast );
	stats["far_color_processing_usec_last"] = ( int64_t ) ( m_nGpuFarColorProcessingUsecLast );
	stats["far_height_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarHeightDirtyTexelsLast );
	stats["far_color_dirty_texels_last"] = ( int64_t ) ( m_nGpuFarColorDirtyTexelsLast );
	stats["far_height_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightRebuiltBytesLast );
	stats["far_color_rebuilt_bytes_last"] = ( int64_t ) ( m_nGpuFarColorRebuiltBytesLast );
	stats["far_height_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesLast );
	stats["far_color_uploaded_bytes_last"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesLast );
	stats["far_height_publication_usec_last"] = ( int64_t ) ( m_nGpuFarHeightPublicationUsecLast );
	stats["far_color_publication_usec_last"] = ( int64_t ) ( m_nGpuFarColorPublicationUsecLast );
	stats["far_height_publication_count"] = ( int64_t ) ( m_nGpuFarHeightPublicationCount );
	stats["far_color_publication_count"] = ( int64_t ) ( m_nGpuFarColorPublicationCount );
	stats["far_height_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarHeightUploadedBytesTotal );
	stats["far_color_uploaded_bytes_total"] = ( int64_t ) ( m_nGpuFarColorUploadedBytesTotal );
	stats["far_partial_publication_available"] = false;
	stats["far_publication_path"] = "RenderingServer full Texture2D update";
	stats["far_cook_io_usec_last"] = ( int64_t ) ( m_nGpuFarCookIoUsecLast );
	stats["far_cooked_loaded"] = m_bGpuFarCookedLoaded;
	stats["far_cooked_saved"] = m_bGpuFarCookedSaved;
	stats["far_height_bytes"] = ( int64_t ) ( getGpuFarHeightBytes() );
	stats["far_color_bytes"] = ( int64_t ) ( getGpuFarColorBytes() );
	stats["far_quality_divisor"] = m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1;
	stats["far_height_atlas_side"] = getGpuFarHeightAtlasSide();
	stats["far_surface_atlas_side"] = getGpuFarSurfaceAtlasSide();
	stats["texture_update_calls_last_tick"] = getGpuRegionTextureUpdateCallsLastTick();
	stats["upload_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionUploadBytesLastTick );
	stats["height_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionHeightBytesLastTick );
	stats["control_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionControlBytesLastTick );
	stats["color_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionColorBytesLastTick );
	stats["derived_bytes_last_tick"] = ( int64_t ) ( m_nGpuRegionDerivedBytesLastTick );
	const size_t layer_bytes = estimateGpuRegionLayerBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t worst_case_bytes = shared_bytes + layer_bytes * ( size_t ) ( MAX( 0, m_nGpuRegionPhysicalLayerCount ) );
	const int allocation_budget_mb = m_pTerrain && m_pTerrain->isEditorAuthoringMode() ?
		calculateAuthoringGpuRegionBudgetMb() : m_nGpuRegionCacheBudgetMb;
	const size_t budget_bytes = ( size_t ) ( allocation_budget_mb ) * TERRAIN3D_GPU_CACHE_MIB;
	stats["estimated_layer_bytes"] = ( int64_t ) ( layer_bytes );
	stats["estimated_shared_bytes"] = ( int64_t ) ( shared_bytes );
	stats["estimated_worst_case_bytes"] = ( int64_t ) ( worst_case_bytes );
	stats["allocation_budget_mb"] = allocation_budget_mb;
	stats["budget_overcommitted"] = worst_case_bytes > budget_bytes;
	stats["authoring_bounded_pool_covers_all_regions"] = isAuthoringGpuFullResidencyPossible();
	stats["authoring_readback_safe"] = isAuthoringGpuReadbackSafe();
	stats["control_implicit"] = m_bControlMapsImplicit;
	stats["color_implicit"] = m_bColorMapsImplicit;
	stats["color_resolution_divisor"] = m_nGpuRegionColorDivisor;
	stats["derived_resolution_divisor"] = m_nGpuRegionDerivedDivisor;
	stats["cooked_pages_enabled"] = m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled();
	stats["cook_on_save"] = m_pTerrain && m_pTerrain->isGpuRegionCookOnSave();
	stats["center"] = m_gpuRegionCacheCenter;
	stats["selection_pending"] = m_bGpuRegionCacheSelectionDirty;
	return stats;
}

size_t T3DData::getGpuRegionCacheReservedBytes( void ) const {
	const size_t fixed_far_bytes = getGpuFarHeightBytes() + getGpuFarColorBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	return ( shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u ) +
		estimateGpuRegionLayerBytes() * ( size_t ) ( MAX( 0, m_nGpuRegionPhysicalLayerCount ) );
}

size_t T3DData::getGpuRegionCacheMinimumBytes( void ) const {
	const size_t fixed_far_bytes = getGpuFarHeightBytes() + getGpuFarColorBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateGpuRegionLayerBytes() *
		( size_t ) ( TERRAIN3D_GPU_CACHE_MIN_SPARE_LAYERS + 1 );
}

size_t T3DData::getGpuRegionCacheHardCapBytes( void ) const {
	const size_t fixed_far_bytes = getGpuFarHeightBytes() + getGpuFarColorBytes();
	const size_t shared_bytes = estimateGpuRegionSharedBytes();
	const size_t managed_shared = shared_bytes > fixed_far_bytes ? shared_bytes - fixed_far_bytes : 0u;
	return managed_shared + estimateGpuRegionLayerBytes() * ( size_t ) ( TERRAIN3D_GPU_CACHE_MAX_LAYERS );
}

size_t T3DData::getCpuRegionCacheReservedBytes( void ) const {
	return estimateCpuRegionLayerBytes() * ( size_t ) ( MAX( 0, m_nCpuRegionCacheCapacity ) );
}

size_t T3DData::getCpuRegionCacheMinimumBytes( void ) const {
	return estimateCpuRegionLayerBytes() * ( size_t ) ( TERRAIN3D_CPU_CACHE_MIN_SAMPLE_LAYERS );
}

size_t T3DData::getCpuRegionCacheHardCapBytes( void ) const {
	return estimateCpuRegionLayerBytes() * ( size_t ) ( TERRAIN3D_CPU_CACHE_MAX_LAYERS );
}

size_t T3DData::getMemoryTransientStagingBytes( void ) const {
	const size_t prepared_page_bytes = estimateCpuRegionLayerBytes();
	size_t bytes = prepared_page_bytes * ( size_t ) ( MAX( 0, m_runtimePageStore.getTransientPageCount() ) );
	bytes += m_mapEditService.getTransientScratchBytes();
	if ( m_pGpuFarHeightImage.is_valid() ) {
		bytes += getGpuFarHeightBytes();
	}
	if ( m_pGpuFarColorImage.is_valid() ) {
		bytes += getGpuFarColorBytes();
	}
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
	state["gpu_resident_page_count"] = m_nGpuRegionResidentCount;
	state["middle_height_resident_page_count"] = m_nMiddleHeightResidentCount;
	state["middle_height_page_table_valid"] = m_generatedMiddleHeightRegionMap.getRid().is_valid();
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
	if ( !m_bGpuRegionCacheEnabled ) {
		return getRegionSlots();
	}
	TypedArray<Vector2i> slots;
	slots.resize( m_nGpuRegionPhysicalLayerCount );
	for ( int layer = 0; layer < m_nGpuRegionPhysicalLayerCount; ++layer ) {
		slots[layer] = m_szGpuRegionLocations[layer];
	}
	return slots;
}

PackedInt32Array T3DData::getRegionMap( void ) const {
	PackedInt32Array region_map;
	if ( m_regionRepository.getRegionMapSize() <= 0 ) {
		return region_map;
	}
	region_map.resize( m_regionRepository.getRegionMapSize() * m_regionRepository.getRegionMapSize() );
	int32_t *pWrite = region_map.ptrw();
	const int half = m_regionRepository.getRegionMapSize() / 2;
	const uint16_t *pSourceMap = m_bGpuRegionCacheEnabled ? m_szGpuRegionMap : m_regionRepository.getLogicalRegionMapData();
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
	if ( bCopy && m_bCpuRegionCacheEnabled ) {
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
				if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResident( current_region_loc ) ) {
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
			if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( cookLocation, true ) ) {
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
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( location, true ) ) {
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
		if ( m_bCpuRegionCacheEnabled && !ensureCpuRegionResidentInternal( location, true ) ) {
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
	const bool bBoundedCpuLoad = m_bCpuRegionCacheEnabled;
	LOG( T3DTerrain::INFO, "[terrain-data] Candidate repository load; CPU cache ",
		bBoundedCpuLoad ? "bounded" : "authoring/unbounded", "; GPU cache ",
		m_bGpuRegionCacheEnabled ? "enabled" : "disabled" );
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
	if ( m_bCpuRegionCacheEnabled ) {
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
		m_bCpuRegionCacheSelectionDirty = true;
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
	if ( m_bGpuRegionCacheEnabled ) {
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

	if ( !m_bGpuRegionCacheEnabled && isFarAtlasRequired() ) {
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
		if ( m_bGpuRegionCacheEnabled ) {
			const int render_layer = findGpuRegionSlot( region_loc );
			if ( render_layer < 0 ) {
				m_bGpuRegionCacheSelectionDirty = true;
				continue;
			}
			bHeightChanged = bHeightChanged || eMapType == T3DRegion::TYPE_HEIGHT || eMapType == T3DRegion::TYPE_MAX;
			bControlChanged = bControlChanged || eMapType == T3DRegion::TYPE_CONTROL || eMapType == T3DRegion::TYPE_MAX;
			bColorChanged = bColorChanged || eMapType == T3DRegion::TYPE_COLOR || eMapType == T3DRegion::TYPE_MAX;
			if ( republishGpuRegionLocation( region_loc ) ) {
				bPhysicalMappingChanged = true;
			}
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
	if ( !m_bGpuRegionCacheEnabled && isFarAtlasRequired() ) {
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
		const int repositorySlot = getRegionId( update.location );
		const int renderLayer = m_bGpuRegionCacheEnabled ? findGpuRegionSlot( update.location ) : repositorySlot;
		const Ref<Image> pDerivedMap = pRegion->getDerivedNormalSlopeMap();
		TypedArray<Image> &derivedLayers = m_bGpuRegionCacheEnabled ? m_gpuDerivedMaps : m_derivedMaps;
		if ( renderLayer >= 0 && pDerivedMap.is_valid() && !m_generatedDerivedMaps.isDirty() &&
			renderLayer < derivedLayers.size() && renderLayer < m_generatedDerivedMaps.size() ) {
			derivedLayers[renderLayer] = pDerivedMap;
			m_generatedDerivedMaps.update( pDerivedMap, renderLayer );
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

	if ( m_bGpuRegionCacheEnabled ) {
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
	if ( eMapType != T3DRegion::TYPE_HEIGHT && m_bCpuRegionCacheEnabled &&
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
	if ( !m_bCpuRegionCacheEnabled ) {
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
	if ( !m_bCpuRegionCacheEnabled ) {
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
	if ( !m_bCpuRegionCacheEnabled ) {
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
