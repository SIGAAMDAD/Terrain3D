// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/sampling/terrain_3d_sampler.h"
#include "core/utility/terrain_3d_binary_io.h"

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/config/terrain_3d_cpp17_compat.h"
#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"

#include <EASTL/numeric_limits.h>
#include <math.h>
#include <string.h>
#include <godot_cpp/classes/image.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

static bool t3dSamplerSamplerIsExactVertex(
	const double fGridX,
	const double fGridZ,
	const float	 fVertexSpacing ) {
	const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
	const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
	const double  fDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( fVertexSpacing );
	const double  fDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( fVertexSpacing );
	return fDx * fDx + fDz * fDz < 0.0001;
}

EA_FORCE_INLINE static float t3dSamplerSamplerBilerp3x3(
	const float	 fHeights[3][3],
	const int	 iX,
	const int	 iZ,
	const real_t fFx,
	const real_t fFz ) {
	return Math::lerp( Math::lerp( fHeights[iZ][iX], fHeights[iZ][iX + 1], fFx ),
					   Math::lerp( fHeights[iZ + 1][iX], fHeights[iZ + 1][iX + 1], fFx ),
					   fFz );
}

void t3dRegionCold_t::reset(
	void ) {
	m_activeLease.reset();
	m_lease.reset();
	m_heightData.clear();
	m_controlData.clear();
	m_derivedData.clear();
}

Terrain3DSampler::Terrain3DSampler(
	const t3dSamplingSource_t &rSource ) :
	m_source( rSource ) {
	if ( m_source.valid() ) {
		m_nSamplingGeneration = m_source.m_pStorage->getSamplingGeneration();
		m_fVertexSpacing = s_fRegionVertexSpacing;
		m_fInverseVertexSpacing = m_fVertexSpacing > 0.0f ? 1.0 / ( double ) ( m_fVertexSpacing ) : 0.0;
	}
}

void Terrain3DSampler::resetCache(
	const uint64_t nGeneration ) {
	for ( int index = 0; index < T3D_SAMPLER_CACHE_SIZE; ++index ) {
		m_szCacheCold[( size_t ) ( index )].reset();
		m_szCache[( size_t ) ( index )] = t3dRegionView_t{};
	}
	m_pLastView = NULL;
	m_nStamp = 1u;
	m_nOperation = 1u;
	m_nSamplingGeneration = nGeneration;
	m_fVertexSpacing = m_source.m_pRepository ? s_fRegionVertexSpacing : 0.0f;
	m_fInverseVertexSpacing = m_fVertexSpacing > 0.0f ? 1.0 / ( double ) ( m_fVertexSpacing ) : 0.0;
}

void Terrain3DSampler::resetView(
	t3dRegionView_t *const pView ) {
	if ( !pView ) {
		return;
	}
	const ptrdiff_t index = pView - m_szCache;
	if ( index < 0 || index >= T3D_SAMPLER_CACHE_SIZE ) {
		return;
	}
	m_szCacheCold[( size_t ) ( index )].reset();
	*pView = t3dRegionView_t{};
}

bool Terrain3DSampler::validateEpoch(
	void ) {
	if ( !m_source.valid() || m_fVertexSpacing <= 0.0f ) {
		return false;
	}
	const uint64_t current_generation = m_source.m_pStorage->getSamplingGeneration();
	if ( current_generation != m_nSamplingGeneration ) {
		resetCache( current_generation );
	}
	if ( ++m_nOperation == 0u ) {
		m_nOperation = 1u;
		for ( size_t iCacheEntry = 0u; iCacheEntry < T3D_ARRAY_COUNT( m_szCache ); iCacheEntry++ ) {
			t3dRegionView_t &entry = ( m_szCache )[iCacheEntry];
			entry.m_nValidatedOperation = 0u;
		}
	}
	return m_fVertexSpacing > 0.0f;
}

void Terrain3DSampler::resolveAxis(
	const int64_t nGrid,
	int64_t		 &nRegion,
	int			 &nLocal ) const {
	if ( nGrid >= 0 ) {
		nRegion = nGrid >> T3D_REGION_SHIFT;
	} else {
		const uint64_t magnitude = ( uint64_t ) ( -( nGrid + 1 ) ) + 1u;
		nRegion = -( int64_t ) ( ( magnitude + ( uint64_t ) ( T3D_REGION_MASK ) ) >> T3D_REGION_SHIFT );
	}
	nLocal = ( int ) ( nGrid - nRegion * ( int64_t ) ( T3D_REGION_SIZE ) );
}

float Terrain3DSampler::readHeight(
	const uint8_t *pBytes,
	const int	   iIndex ) {
	return t3dRegionCodecReadHeightBytes( pBytes, iIndex, Image::FORMAT_R16 );
}

uint32_t Terrain3DSampler::readUint(
	const uint8_t *pBytes,
	const int	   iIndex ) {
	if ( !pBytes || iIndex < 0 ) {
		return UINT32_MAX;
	}
	return t3dReadNativeUnaligned< uint32_t >( pBytes + ( size_t ) ( iIndex ) * sizeof( uint32_t ) );
}

uint32_t Terrain3DSampler::readControl(
	const t3dRegionView_t *pView,
	const int			   iIndex ) {
	if ( !pView || iIndex < 0 || !pView->m_bControlResolved ) {
		return UINT32_MAX;
	}
	return pView->m_pControlBytes ? readUint( pView->m_pControlBytes, iIndex ) : T3D_REGION_DEFAULT_CONTROL;
}

t3dRegionView_t *Terrain3DSampler::validateCachedView(
	t3dRegionView_t *pView,
	const Vector2i	&rLocation ) {
	if ( !pView || !pView->m_bLoaded || !pView->m_bValid || pView->m_location != rLocation ) {
		return NULL;
	}
	if ( pView->m_nValidatedOperation != m_nOperation ) {
		const uint32_t generation = m_source.m_pStorage->getRegionGeneration( rLocation );
		if ( generation == 0u || generation != pView->m_nRegionGeneration ) {
			resetView( pView );
			if ( m_pLastView == pView ) {
				m_pLastView = NULL;
			}
			return NULL;
		}
		pView->m_nValidatedOperation = m_nOperation;
	}
	pView->m_nStamp = ++m_nStamp;
	return pView;
}

t3dRegionView_t *Terrain3DSampler::getRegionViewUnchecked(
	const Vector2i &rLocation ) {
	if ( !m_source.valid() ) {
		return NULL;
	}
	if ( t3dRegionView_t *hit = validateCachedView( m_pLastView, rLocation ) ) {
		return hit;
	}

	t3dRegionView_t *pReplacement = &m_szCache[0];
	for ( size_t iCacheEntry = 0u; iCacheEntry < T3D_ARRAY_COUNT( m_szCache ); iCacheEntry++ ) {
		t3dRegionView_t &entry = ( m_szCache )[iCacheEntry];
		if ( entry.m_bLoaded && entry.m_location == rLocation ) {
			if ( t3dRegionView_t *hit = validateCachedView( &entry, rLocation ) ) {
				m_pLastView = hit;
				return hit;
			}
		}
		if ( !entry.m_bLoaded ) {
			pReplacement = &entry;
			break;
		}
		if ( entry.m_nStamp < pReplacement->m_nStamp ) {
			pReplacement = &entry;
		}
	}

	Terrain3DRegion *const pRegion = m_source.m_pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return NULL;
	}

	resetView( pReplacement );
	const size_t	 replacement_index = ( size_t ) ( pReplacement - m_szCache );
	t3dRegionCold_t &rCold = m_szCacheCold[replacement_index];
	pReplacement->m_location = rLocation;
	pReplacement->m_nStamp = ++m_nStamp;
	pReplacement->m_bLoaded = true;
	pReplacement->m_nRegionGeneration = m_source.m_pStorage->getRegionGeneration( rLocation );
	pReplacement->m_nValidatedOperation = m_nOperation;
	pReplacement->m_bControlResolved =
		!pRegion->hasChannel( TYPE_CONTROL ) || pRegion->getMapPtr( TYPE_CONTROL ) != NULL;

	Terrain3DActiveMapLease active = m_source.m_pStorage->acquireActive( rLocation );
	if ( active.valid() ) {
		const t3dActiveMapView_t &view = active.view();
		pReplacement->m_pHeightBytes = ( const uint8_t * ) ( view.m_pHeight );
		rCold.m_activeLease = eastl::move( active );
	} else {
		Terrain3DResidentMapLease pooled = m_source.m_pStorage->acquireResident( rLocation, pRegion );
		if ( pooled.valid() ) {
			const t3dResidentMapView_t &view = pooled.getView();
			pReplacement->m_pHeightBytes = ( const uint8_t * ) ( view.m_pHeight );
			pReplacement->m_pControlBytes = ( const uint8_t * ) ( view.m_pControl );
			pReplacement->m_pDerivedBytes = ( const uint8_t * ) ( view.m_pDerived );
			rCold.m_lease = eastl::move( pooled );
		} else {
			Image *const height = pRegion->getMapPtr( TYPE_HEIGHT );
			Image *const control = pRegion->getMapPtr( TYPE_CONTROL );
			if ( !height || height->get_format() != Image::FORMAT_R16 ) {
				return NULL;
			}
			rCold.m_heightData = height->get_data();
			const int texels = T3D_REGION_TEXELS;
			if ( rCold.m_heightData.size() < texels * ( int ) ( sizeof( uint16_t ) ) ) {
				rCold.m_heightData.clear();
				return NULL;
			}
			pReplacement->m_pHeightBytes = rCold.m_heightData.ptr();
			if ( control ) {
				if ( control->get_format() != Image::FORMAT_RF ) {
					return NULL;
				}
				rCold.m_controlData = control->get_data();
				if ( rCold.m_controlData.size() < texels * ( int ) ( sizeof( uint32_t ) ) ) {
					rCold.m_controlData.clear();
					return NULL;
				}
				pReplacement->m_pControlBytes = rCold.m_controlData.ptr();
			}
		}
	}

	if ( !pReplacement->m_pControlBytes ) {
		Image *const control = pRegion->getMapPtr( TYPE_CONTROL );
		if ( control ) {
			if ( control->get_format() != Image::FORMAT_RF ) {
				return NULL;
			}
			rCold.m_controlData = control->get_data();
			const int texels = T3D_REGION_TEXELS;
			if ( rCold.m_controlData.size() < texels * ( int ) ( sizeof( uint32_t ) ) ) {
				rCold.m_controlData.clear();
				return NULL;
			}
			pReplacement->m_pControlBytes = rCold.m_controlData.ptr();
		}
	}

	if ( !pReplacement->m_pDerivedBytes && pRegion->hasValidDerivedNormalSlopeMap() ) {
		const Ref< Image > derived = pRegion->getDerivedNormalSlopeMap();
		if ( derived.is_valid() && derived->get_format() == Image::FORMAT_RG8 ) {
			rCold.m_derivedData = derived->get_data();
		}
	}
	const int expected_derived_bytes = T3D_REGION_TEXELS * 2;
	if ( !pReplacement->m_pDerivedBytes && rCold.m_derivedData.size() >= expected_derived_bytes ) {
		pReplacement->m_pDerivedBytes = rCold.m_derivedData.ptr();
	}
	pReplacement->m_bValid = true;
	m_pLastView = pReplacement;
	return pReplacement;
}

bool Terrain3DSampler::resolveGridUnchecked(
	const int64_t	  iGridX,
	const int64_t	  iGridZ,
	t3dRegionView_t *&pView,
	int				 &iIndex ) {
	int64_t region_x = 0;
	int64_t region_z = 0;
	int		local_x = 0;
	int		local_z = 0;
	resolveAxis( iGridX, region_x, local_x );
	resolveAxis( iGridZ, region_z, local_z );
	if ( region_x < INT32_MIN || region_x > INT32_MAX || region_z < INT32_MIN || region_z > INT32_MAX ) {
		return false;
	}
	pView = getRegionViewUnchecked( Vector2i( ( int32_t ) ( region_x ), ( int32_t ) ( region_z ) ) );
	if ( !pView ) {
		return false;
	}
	if ( ( uint32_t ) ( local_x ) >= ( uint32_t ) ( T3D_REGION_SIZE ) ||
		 ( uint32_t ) ( local_z ) >= ( uint32_t ) ( T3D_REGION_SIZE ) ) {
		return false;
	}
	iIndex = local_z * T3D_REGION_SIZE + local_x;
	return true;
}

float Terrain3DSampler::sampleHeightGridUnchecked(
	const int64_t iGridX,
	const int64_t iGridZ,
	const bool	  bRespectHoles ) {
	t3dRegionView_t *pView = NULL;
	int				 index = 0;
	if ( !resolveGridUnchecked( iGridX, iGridZ, pView, index ) ) {
		return NAN;
	}
	if ( bRespectHoles && t3dIsHole( readControl( pView, index ) ) ) {
		return NAN;
	}
	return readHeight( pView->m_pHeightBytes, index );
}

uint32_t Terrain3DSampler::sampleControlGridUnchecked(
	const int64_t iGridX,
	const int64_t iGridZ ) {
	t3dRegionView_t *pView = NULL;
	int				 index = 0;
	if ( !resolveGridUnchecked( iGridX, iGridZ, pView, index ) ) {
		return UINT32_MAX;
	}
	return readControl( pView, index );
}

float Terrain3DSampler::sampleHeightGrid(
	const int64_t iGridX,
	const int64_t iGridZ,
	const bool	  bRespectHoles ) {
	return validateEpoch() ? sampleHeightGridUnchecked( iGridX, iGridZ, bRespectHoles ) : NAN;
}

uint32_t Terrain3DSampler::sampleControlGrid(
	const int64_t iGridX,
	const int64_t iGridZ ) {
	return validateEpoch() ? sampleControlGridUnchecked( iGridX, iGridZ ) : UINT32_MAX;
}

uint32_t Terrain3DSampler::sampleControl(
	const Vector3 &rGlobalPosition ) {
	if ( !validateEpoch() ) {
		return UINT32_MAX;
	}
	return sampleControlGridUnchecked(
		( int64_t ) ( Math::floor( ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing ) ),
		( int64_t ) ( Math::floor( ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing ) ) );
}

float Terrain3DSampler::sampleHeightUnchecked(
	const Vector3 &rGlobalPosition ) {
	const double	 fGridX = ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing;
	const double	 fGridZ = ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing;
	const int64_t	 x0 = ( int64_t ) ( Math::floor( fGridX ) );
	const int64_t	 z0 = ( int64_t ) ( Math::floor( fGridZ ) );
	t3dRegionView_t *pView = NULL;
	int				 index = 0;
	if ( !resolveGridUnchecked( x0, z0, pView, index ) ) {
		return NAN;
	}
	const uint32_t control = readControl( pView, index );
	if ( control == UINT32_MAX || t3dIsHole( control ) ) {
		return NAN;
	}

	const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
	const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
	const double  fDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( m_fVertexSpacing );
	const double  fDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( m_fVertexSpacing );
	if ( fDx * fDx + fDz * fDz < 0.0001 ) {
		return readHeight( pView->m_pHeightBytes, index );
	}

	const int local_x = ( int ) ( index & T3D_REGION_MASK );
	const int local_z = index >> T3D_REGION_SHIFT;
	float	  fH00;
	float	  fH10;
	float	  fH01;
	float	  fH11;
	if ( local_x + 1 < T3D_REGION_SIZE && local_z + 1 < T3D_REGION_SIZE ) {
		// The overwhelmingly common path resolves one region and consumes two
		// adjacent R16 pairs. No cache scan, coordinate decomposition or metadata
		// load occurs for the remaining three samples.
		fH00 = readHeight( pView->m_pHeightBytes, index );
		fH10 = readHeight( pView->m_pHeightBytes, index + 1 );
		fH01 = readHeight( pView->m_pHeightBytes, index + T3D_REGION_SIZE );
		fH11 = readHeight( pView->m_pHeightBytes, index + T3D_REGION_SIZE + 1 );
	} else {
		fH00 = sampleHeightGridUnchecked( x0, z0, false );
		fH10 = sampleHeightGridUnchecked( x0 + 1, z0, false );
		fH01 = sampleHeightGridUnchecked( x0, z0 + 1, false );
		fH11 = sampleHeightGridUnchecked( x0 + 1, z0 + 1, false );
	}
	if ( isnan( fH00 ) || isnan( fH10 ) || isnan( fH01 ) || isnan( fH11 ) ) {
		return NAN;
	}
	const real_t fFx = ( real_t ) ( fGridX - ( double ) ( x0 ) );
	const real_t fFz = ( real_t ) ( fGridZ - ( double ) ( z0 ) );
	return Math::lerp( Math::lerp( fH00, fH10, fFx ), Math::lerp( fH01, fH11, fFx ), fFz );
}

float Terrain3DSampler::sampleHeight(
	const Vector3 &rGlobalPosition ) {
	return validateEpoch() ? sampleHeightUnchecked( rGlobalPosition ) : NAN;
}

Vector3 Terrain3DSampler::sampleNormalUnchecked(
	const Vector3 &rGlobalPosition,
	float		  *pHeight ) {
	if ( pHeight ) {
		*pHeight = NAN;
	}
	const double  fGridX = ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing;
	const double  fGridZ = ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing;
	const int64_t x0 = ( int64_t ) ( Math::floor( fGridX ) );
	const int64_t z0 = ( int64_t ) ( Math::floor( fGridZ ) );
	const bool	  bExactVertex = t3dSamplerSamplerIsExactVertex( fGridX, fGridZ, m_fVertexSpacing );

	float			 fHeights[3][3];
	const int		 needed = bExactVertex ? 2 : 3;
	t3dRegionView_t *pView = NULL;
	int				 index = 0;
	if ( !resolveGridUnchecked( x0, z0, pView, index ) ) {
		return s_v3Nan;
	}
	const int  local_x = ( int ) ( index & T3D_REGION_MASK );
	const int  local_z = index >> T3D_REGION_SHIFT;
	const bool bInterior = local_x + needed <= T3D_REGION_SIZE && local_z + needed <= T3D_REGION_SIZE;
	if ( bInterior ) {
		const uint32_t c0 = readControl( pView, index );
		const uint32_t cx = readControl( pView, index + 1 );
		const uint32_t cz = readControl( pView, index + T3D_REGION_SIZE );
		if ( c0 == UINT32_MAX || cx == UINT32_MAX || cz == UINT32_MAX || t3dIsHole( c0 ) || t3dIsHole( cx ) ||
			 t3dIsHole( cz ) ) {
			return s_v3Nan;
		}
		for ( int z = 0; z < needed; ++z ) {
			const int row = index + z * T3D_REGION_SIZE;
			for ( int x = 0; x < needed; ++x ) {
				fHeights[z][x] = readHeight( pView->m_pHeightBytes, row + x );
			}
		}
	} else {
		const uint32_t c0 = sampleControlGridUnchecked( x0, z0 );
		const uint32_t cx = sampleControlGridUnchecked( x0 + 1, z0 );
		const uint32_t cz = sampleControlGridUnchecked( x0, z0 + 1 );
		if ( c0 == UINT32_MAX || cx == UINT32_MAX || cz == UINT32_MAX || t3dIsHole( c0 ) || t3dIsHole( cx ) ||
			 t3dIsHole( cz ) ) {
			return s_v3Nan;
		}
		for ( int z = 0; z < needed; ++z ) {
			for ( int x = 0; x < needed; ++x ) {
				fHeights[z][x] = sampleHeightGridUnchecked( x0 + x, z0 + z, false );
				if ( isnan( fHeights[z][x] ) ) {
					return s_v3Nan;
				}
			}
		}
	}

	float fHeight = fHeights[0][0];
	float fHeightX = fHeights[0][1];
	float fHeightZ = fHeights[1][0];
	if ( !bExactVertex ) {
		const real_t fFx = ( real_t ) ( fGridX - ( double ) ( x0 ) );
		const real_t fFz = ( real_t ) ( fGridZ - ( double ) ( z0 ) );
		fHeight = t3dSamplerSamplerBilerp3x3( fHeights, 0, 0, fFx, fFz );
		fHeightX = t3dSamplerSamplerBilerp3x3( fHeights, 1, 0, fFx, fFz );
		fHeightZ = t3dSamplerSamplerBilerp3x3( fHeights, 0, 1, fFx, fFz );
	}
	if ( pHeight ) {
		*pHeight = fHeight;
	}
	return Vector3( fHeight - fHeightX, m_fVertexSpacing, fHeight - fHeightZ ).normalized();
}

Vector3 Terrain3DSampler::sampleNormal(
	const Vector3 &rGlobalPosition ) {
	return validateEpoch() ? sampleNormalUnchecked( rGlobalPosition ) : s_v3Nan;
}

bool Terrain3DSampler::sampleHeightAndNormal(
	const Vector3 &rGlobalPosition,
	float		  &fHeight,
	Vector3		  &rNormal ) {
	fHeight = NAN;
	rNormal = s_v3Nan;
	if ( !validateEpoch() ) {
		return false;
	}
	rNormal = sampleNormalUnchecked( rGlobalPosition, &fHeight );
	return isfinite( fHeight ) && rNormal.is_finite();
}

Vector3 Terrain3DSampler::sampleSlopeNormalUnchecked(
	const Vector3 &rGlobalPosition ) {
	const int64_t	 x = ( int64_t ) ( Math::round( ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing ) );
	const int64_t	 z = ( int64_t ) ( Math::round( ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing ) );
	t3dRegionView_t *pView = NULL;
	int				 index = 0;
	if ( !resolveGridUnchecked( x, z, pView, index ) ) {
		return s_v3Nan;
	}
	if ( pView->m_pDerivedBytes ) {
		const size_t offset = ( size_t ) ( index ) * 2u;
		const float	 fNx = ( float ) ( pView->m_pDerivedBytes[offset + 0u] ) / s_fByteMaxFloat * 2.0f - 1.0f;
		const float	 fNz = ( float ) ( pView->m_pDerivedBytes[offset + 1u] ) / s_fByteMaxFloat * 2.0f - 1.0f;
		const float	 fNy = Math::sqrt( MAX( 1.0f - fNx * fNx - fNz * fNz, 0.0f ) );
		Vector3		 normal( fNx, fNy, fNz );
		if ( normal.length_squared() > CMP_EPSILON ) {
			return normal.normalized();
		}
	}
	float	  fHeight;
	float	  fHeightX;
	float	  fHeightZ;
	const int local_x = ( int ) ( index & T3D_REGION_MASK );
	const int local_z = index >> T3D_REGION_SHIFT;
	if ( local_x + 1 < T3D_REGION_SIZE && local_z + 1 < T3D_REGION_SIZE ) {
		fHeight = readHeight( pView->m_pHeightBytes, index );
		fHeightX = readHeight( pView->m_pHeightBytes, index + 1 );
		fHeightZ = readHeight( pView->m_pHeightBytes, index + T3D_REGION_SIZE );
	} else {
		fHeight = sampleHeightGridUnchecked( x, z, false );
		fHeightX = sampleHeightGridUnchecked( x + 1, z, false );
		fHeightZ = sampleHeightGridUnchecked( x, z + 1, false );
	}
	fHeight = isnan( fHeight ) ? 0.0f : fHeight;
	fHeightX = isnan( fHeightX ) ? 0.0f : fHeightX;
	fHeightZ = isnan( fHeightZ ) ? 0.0f : fHeightZ;
	return Vector3( fHeight - fHeightX, m_fVertexSpacing, fHeight - fHeightZ ).normalized();
}

Vector3 Terrain3DSampler::sampleSlopeNormal(
	const Vector3 &rGlobalPosition ) {
	return validateEpoch() ? sampleSlopeNormalUnchecked( rGlobalPosition ) : s_v3Nan;
}

bool Terrain3DSampler::sampleControlAndSlopeNormal(
	const Vector3 &rGlobalPosition,
	uint32_t	  &nControl,
	Vector3		  &rNormal ) {
	nControl = UINT32_MAX;
	rNormal = s_v3Nan;
	if ( !validateEpoch() ) {
		return false;
	}
	nControl = sampleControlGridUnchecked(
		( int64_t ) ( Math::floor( ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing ) ),
		( int64_t ) ( Math::floor( ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing ) ) );
	if ( nControl == UINT32_MAX ) {
		return false;
	}
	rNormal = sampleSlopeNormalUnchecked( rGlobalPosition );
	return rNormal.is_finite();
}

bool Terrain3DSampler::sampleHeights(
	const Vector3 *pPositions,
	float		  *pHeights,
	const int	   nCount ) {
	if ( !pPositions || !pHeights || nCount < 0 ) {
		return false;
	}
	if ( !validateEpoch() ) {
		eastl::fill_n( pHeights, nCount, NAN );
		return false;
	}
	for ( int index = 0; index < nCount; ++index ) {
		pHeights[index] = sampleHeightUnchecked( pPositions[index] );
	}
	return true;
}

bool Terrain3DSampler::sampleControls(
	const Vector3 *pPositions,
	int32_t		  *pControls,
	const int	   nCount ) {
	if ( !pPositions || !pControls || nCount < 0 ) {
		return false;
	}
	if ( !validateEpoch() ) {
		eastl::fill_n( pControls, nCount, ( int32_t ) ( UINT32_MAX ) );
		return false;
	}
	for ( int index = 0; index < nCount; ++index ) {
		const Vector3 &position = pPositions[index];
		pControls[index] = ( int32_t ) ( sampleControlGridUnchecked(
			( int64_t ) ( Math::floor( ( double ) ( position.x ) * m_fInverseVertexSpacing ) ),
			( int64_t ) ( Math::floor( ( double ) ( position.z ) * m_fInverseVertexSpacing ) ) ) );
	}
	return true;
}

bool Terrain3DSampler::sampleNormals(
	const Vector3 *pPositions,
	Vector3		  *pNormals,
	const int	   nCount ) {
	if ( !pPositions || !pNormals || nCount < 0 ) {
		return false;
	}
	if ( !validateEpoch() ) {
		eastl::fill_n( pNormals, nCount, s_v3Nan );
		return false;
	}
	for ( int index = 0; index < nCount; ++index ) {
		pNormals[index] = sampleNormalUnchecked( pPositions[index] );
	}
	return true;
}

bool Terrain3DSampler::copyHeightRect(
	const int64_t iGridX,
	const int64_t iGridZ,
	const int	  nWidth,
	const int	  nHeight,
	float *const  pDestination,
	const int	  nDestinationStride,
	const bool	  bRespectHoles,
	const bool	  bRequireCompleteCoverage ) {
	if ( !validateEpoch() || !pDestination || nWidth <= 0 || nHeight <= 0 || nDestinationStride < nWidth ) {
		return false;
	}
	bool bComplete = true;
	bool bHardFailure = false;
	for ( int row = 0; row < nHeight; ++row ) {
		float		 *destination = pDestination + ( size_t ) ( row ) * ( size_t ) ( nDestinationStride );
		const int64_t grid_z = iGridZ + row;
		int64_t		  region_z = 0;
		int			  local_z = 0;
		resolveAxis( grid_z, region_z, local_z );
		int copied = 0;
		while ( copied < nWidth ) {
			const int64_t grid_x = iGridX + copied;
			int64_t		  region_x = 0;
			int			  local_x = 0;
			resolveAxis( grid_x, region_x, local_x );
			const int run = MIN( nWidth - copied, T3D_REGION_SIZE - local_x );
			if ( region_x < INT32_MIN || region_x > INT32_MAX || region_z < INT32_MIN || region_z > INT32_MAX ) {
				eastl::fill_n( destination + copied, run, NAN );
				bComplete = false;
				copied += run;
				continue;
			}
			const Vector2i		   region_location( ( int32_t ) ( region_x ), ( int32_t ) ( region_z ) );
			t3dRegionView_t *const pView = getRegionViewUnchecked( region_location );
			if ( !pView ) {
				eastl::fill_n( destination + copied, run, NAN );
				bComplete = false;
				const Terrain3DRegion *const pRegion =
					m_source.m_pRepository ? m_source.m_pRepository->getRegionPtr( region_location ) : NULL;
				bHardFailure = bHardFailure || ( pRegion && !pRegion->isDeleted() );
				copied += run;
				continue;
			}
			const int		index = local_z * T3D_REGION_SIZE + local_x;
			const uint8_t *const pHeight = pView->m_pHeightBytes + ( size_t ) index * sizeof( uint16_t );
			if ( !bRespectHoles ) {
				for ( int i = 0; i < run; ++i ) {
					destination[copied + i] = t3dRegionCodecReadHeightBytes( pHeight, i, Image::FORMAT_R16 );
				}
			} else {
				for ( int i = 0; i < run; ++i ) {
					const uint32_t control = readControl( pView, index + i );
					destination[copied + i] = t3dIsHole( control ) ? NAN : t3dRegionCodecReadHeightBytes( pHeight, i, Image::FORMAT_R16 );
				}
			}
			copied += run;
		}
	}
	return !bHardFailure && ( bComplete || !bRequireCompleteCoverage );
}

bool Terrain3DSampler::minimumHeightRect(
	const int64_t iGridX,
	const int64_t iGridZ,
	const int	  nWidth,
	const int	  nHeight,
	float		 &fHeight,
	const bool	  bRespectHoles ) {
	if ( !validateEpoch() || nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	float fMinimum = eastl::numeric_limits< float >::infinity();
	for ( int row = 0; row < nHeight; ++row ) {
		const int64_t grid_z = iGridZ + row;
		int64_t		  region_z = 0;
		int			  local_z = 0;
		resolveAxis( grid_z, region_z, local_z );
		if ( region_z < INT32_MIN || region_z > INT32_MAX ) {
			return false;
		}
		int copied = 0;
		while ( copied < nWidth ) {
			const int64_t grid_x = iGridX + copied;
			int64_t		  region_x = 0;
			int			  local_x = 0;
			resolveAxis( grid_x, region_x, local_x );
			if ( region_x < INT32_MIN || region_x > INT32_MAX ) {
				return false;
			}
			const int			   run = MIN( nWidth - copied, T3D_REGION_SIZE - local_x );
			t3dRegionView_t *const pView =
				getRegionViewUnchecked( Vector2i( ( int32_t ) ( region_x ), ( int32_t ) ( region_z ) ) );
			if ( !pView ) {
				return false;
			}
			const int		index = local_z * T3D_REGION_SIZE + local_x;
			const uint8_t *const pHeight = pView->m_pHeightBytes + ( size_t ) index * sizeof( uint16_t );
			for ( int i = 0; i < run; ++i ) {
				if ( bRespectHoles && t3dIsHole( readControl( pView, index + i ) ) ) {
					return false;
				}
				fMinimum = MIN( fMinimum, t3dRegionCodecReadHeightBytes( pHeight, i, Image::FORMAT_R16 ) );
			}
			copied += run;
		}
	}
	if ( !isfinite( fMinimum ) ) {
		return false;
	}
	fHeight = fMinimum;
	return true;
}
