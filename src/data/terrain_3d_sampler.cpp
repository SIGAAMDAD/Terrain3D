// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
#include "data/terrain_3d_sampler.h"
#include "core/terrain_3d_cpp17_compat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <godot_cpp/classes/image.hpp>

#include "Terrain3DConstants.h"
#include "data/terrain_3d_region_repository.h"
#include "data/terrain_3d_sampling_storage.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_codec.h"

namespace {
static bool terrain3dSamplerIsExactVertex( const double fGridX, const double fGridZ, const float fVertexSpacing ) {
	const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
	const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
	const double fDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( fVertexSpacing );
	const double fDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( fVertexSpacing );
	return fDx * fDx + fDz * fDz < 0.0001;
}

EA_FORCE_INLINE static float terrain3dSamplerBilerp3x3( const float fHeights[3][3], const int iX, const int iZ, const real_t fFx, const real_t fFz ) { return Math::lerp( Math::lerp( fHeights[iZ][iX], fHeights[iZ][iX + 1], fFx ), Math::lerp( fHeights[iZ + 1][iX], fHeights[iZ + 1][iX + 1], fFx ), fFz ); }
} // namespace

void T3DSampler::t3dRegionCold_t::reset( void ) {
	activeLease.reset();
	lease.reset();
	heightData.clear();
	controlData.clear();
	derivedData.clear();
}

T3DSampler::T3DSampler( const T3DSamplingSource &rSource )
	: m_source( rSource ) {
	if ( m_source.valid() ) {
		m_nSamplingGeneration = m_source.pStorage->getSamplingGeneration();
		m_nRegionSize = m_source.pRepository->getRegionSize();
		m_nRegionShift = m_nRegionSize > 0 && ( m_nRegionSize & ( m_nRegionSize - 1 ) ) == 0
			? ( int ) ( terrain3d::countrZeroU32( ( uint32_t ) ( m_nRegionSize ) ) )
			: -1;
		m_nRegionMask = m_nRegionShift >= 0 ? ( int64_t ) ( m_nRegionSize - 1 ) : 0;
		m_fVertexSpacing = m_source.pRepository->getVertexSpacing();
		m_fInverseVertexSpacing = m_fVertexSpacing > 0.0f ? 1.0 / ( double ) ( m_fVertexSpacing ) : 0.0;
	}
}

void T3DSampler::resetCache( const uint64_t nGeneration ) {
	for ( int index = 0; index < TERRAIN3D_SAMPLER_CACHE_SIZE; ++index ) {
		m_szCacheCold[( size_t ) ( index )].reset();
		m_szCache[( size_t ) ( index )] = t3dRegionView_t{};
	}
	m_pLastView = nullptr;
	m_stamp = 1u;
	m_operation = 1u;
	m_nSamplingGeneration = nGeneration;
	m_nRegionSize = m_source.pRepository ? m_source.pRepository->getRegionSize() : 0;
	m_nRegionShift = m_nRegionSize > 0 && ( m_nRegionSize & ( m_nRegionSize - 1 ) ) == 0
		? ( int ) ( terrain3d::countrZeroU32( ( uint32_t ) ( m_nRegionSize ) ) )
		: -1;
	m_nRegionMask = m_nRegionShift >= 0 ? ( int64_t ) ( m_nRegionSize - 1 ) : 0;
	m_fVertexSpacing = m_source.pRepository ? m_source.pRepository->getVertexSpacing() : 0.0f;
	m_fInverseVertexSpacing = m_fVertexSpacing > 0.0f ? 1.0 / ( double ) ( m_fVertexSpacing ) : 0.0;
}

void T3DSampler::resetView( t3dRegionView_t *const pView ) {
	if ( !pView ) {
		return;
	}
	const ptrdiff_t index = pView - m_szCache.data();
	if ( index < 0 || index >= TERRAIN3D_SAMPLER_CACHE_SIZE ) {
		return;
	}
	m_szCacheCold[( size_t ) ( index )].reset();
	*pView = t3dRegionView_t{};
}

bool T3DSampler::validateEpoch( void ) {
	if ( !m_source.valid() || m_nRegionSize <= 0 || m_fVertexSpacing <= 0.0f ) {
		return false;
	}
	const uint64_t current_generation = m_source.pStorage->getSamplingGeneration();
	if ( current_generation != m_nSamplingGeneration ) {
		resetCache( current_generation );
	}
	if ( ++m_operation == 0u ) {
		m_operation = 1u;
		for ( t3dRegionView_t &entry : m_szCache ) {
			entry.nValidatedOperation = 0u;
		}
	}
	return m_nRegionSize > 0 && m_fVertexSpacing > 0.0f;
}

int64_t T3DSampler::floorDiv( const int64_t nValue, const int64_t nDivisor ) {
	const int64_t quotient = nValue / nDivisor;
	const int64_t remainder = nValue % nDivisor;
	return quotient - ( ( remainder != 0 && ( ( remainder < 0 ) != ( nDivisor < 0 ) ) ) ? 1 : 0 );
}

void T3DSampler::resolveAxis( const int64_t nGrid, int64_t &nRegion, int &nLocal ) const {
	if ( m_nRegionShift >= 0 ) {
		if ( nGrid >= 0 ) {
			nRegion = nGrid >> m_nRegionShift;
			nLocal = ( int ) ( nGrid & m_nRegionMask );
			return;
		}
		// -(p_grid + 1) is defined even at INT64_MIN. This produces mathematical
		// floor division and a positive local coordinate without idiv.
		const int64_t adjusted = -( nGrid + 1 );
		nRegion = -1 - ( adjusted >> m_nRegionShift );
		nLocal = ( int ) ( nGrid - nRegion * ( int64_t ) ( m_nRegionSize ) );
		return;
	}
	nRegion = floorDiv( nGrid, m_nRegionSize );
	nLocal = ( int ) ( nGrid - nRegion * ( int64_t ) ( m_nRegionSize ) );
}

float T3DSampler::readHeight( const uint8_t *pBytes, const int iIndex ) {
	return T3DRegionCodec::readHeightBytes( pBytes, iIndex, Image::FORMAT_R16 );
}

uint32_t T3DSampler::readUint( const uint8_t *pBytes, const int iIndex ) {
	if ( !pBytes || iIndex < 0 ) {
		return UINT32_MAX;
	}
	uint32_t value = UINT32_MAX;
	std::memcpy( &value, pBytes + ( size_t ) ( iIndex ) * sizeof( uint32_t ), sizeof( uint32_t ) );
	return value;
}

uint32_t T3DSampler::readControl( const t3dRegionView_t *pView, const int iIndex ) {
	if ( !pView || iIndex < 0 || !pView->bControlResolved ) {
		return UINT32_MAX;
	}
	return pView->pControlBytes ? readUint( pView->pControlBytes, iIndex ) : TERRAIN3D_REGION_DEFAULT_CONTROL;
}

T3DSampler::t3dRegionView_t *T3DSampler::validateCachedView(
	t3dRegionView_t *pView,
	const Vector2i &rLocation
) {
	if ( !pView || !pView->bLoaded || !pView->bValid || pView->location != rLocation ) {
		return nullptr;
	}
	if ( pView->nValidatedOperation != m_operation ) {
		const uint32_t generation = m_source.pStorage->getRegionGeneration( rLocation );
		if ( generation == 0u || generation != pView->nRegionGeneration ) {
			resetView( pView );
			if ( m_pLastView == pView ) {
				m_pLastView = nullptr;
			}
			return nullptr;
		}
		pView->nValidatedOperation = m_operation;
	}
	pView->nStamp = ++m_stamp;
	return pView;
}

T3DSampler::t3dRegionView_t *T3DSampler::getRegionViewUnchecked( const Vector2i &rLocation ) {
	if ( !m_source.valid() ) {
		return nullptr;
	}
	if ( t3dRegionView_t *hit = validateCachedView( m_pLastView, rLocation ) ) {
		return hit;
	}

	t3dRegionView_t *pReplacement = &m_szCache[0];
	for ( t3dRegionView_t &entry : m_szCache ) {
		if ( entry.bLoaded && entry.location == rLocation ) {
			if ( t3dRegionView_t *hit = validateCachedView( &entry, rLocation ) ) {
				m_pLastView = hit;
				return hit;
			}
		}
		if ( !entry.bLoaded ) {
			pReplacement = &entry;
			break;
		}
		if ( entry.nStamp < pReplacement->nStamp ) {
			pReplacement = &entry;
		}
	}

	T3DRegion *const pRegion = m_source.pRepository->getRegionPtr( rLocation );
	if ( !pRegion || pRegion->isDeleted() ) {
		return nullptr;
	}

	resetView( pReplacement );
	const size_t replacement_index = ( size_t ) ( pReplacement - m_szCache.data() );
	t3dRegionCold_t &rCold = m_szCacheCold[replacement_index];
	pReplacement->location = rLocation;
	pReplacement->nStamp = ++m_stamp;
	pReplacement->bLoaded = true;
	pReplacement->nRegionGeneration = m_source.pStorage->getRegionGeneration( rLocation );
	pReplacement->nValidatedOperation = m_operation;
	pReplacement->bControlResolved = !pRegion->hasChannel( T3DRegion::TYPE_CONTROL ) ||
		pRegion->getMapPtr( T3DRegion::TYPE_CONTROL ) != nullptr;

	T3DActiveMapLease active = m_source.pStorage->acquireActive( rLocation );
	if ( active.valid() ) {
		const t3dActiveMapView_t &view = active.view();
		pReplacement->pHeightBytes = ( const uint8_t * ) ( view.pHeight );
		rCold.activeLease = std::move( active );
	} else {
		T3DResidentMapLease pooled = m_source.pStorage->acquireResident( rLocation, pRegion );
		if ( pooled.valid() ) {
			const t3dResidentMapView_t &view = pooled.getView();
			pReplacement->pHeightBytes = ( const uint8_t * ) ( view.pHeight );
			pReplacement->pControlBytes = ( const uint8_t * ) ( view.pControl );
			pReplacement->pDerivedBytes = ( const uint8_t * ) ( view.pDerived );
			rCold.lease = std::move( pooled );
		} else {
		Image *const height = pRegion->getMapPtr( T3DRegion::TYPE_HEIGHT );
		Image *const control = pRegion->getMapPtr( T3DRegion::TYPE_CONTROL );
		if ( !height || height->get_format() != Image::FORMAT_R16 ) {
			return nullptr;
		}
			rCold.heightData = height->get_data();
			const int texels = m_nRegionSize * m_nRegionSize;
			if ( rCold.heightData.size() < texels * ( int ) ( sizeof( uint16_t ) ) ) {
				rCold.heightData.clear();
				return nullptr;
			}
			pReplacement->pHeightBytes = rCold.heightData.ptr();
			if ( control ) {
				if ( control->get_format() != Image::FORMAT_RF ) {
					return nullptr;
				}
				rCold.controlData = control->get_data();
				if ( rCold.controlData.size() < texels * ( int ) ( sizeof( uint32_t ) ) ) {
					rCold.controlData.clear();
					return nullptr;
				}
				pReplacement->pControlBytes = rCold.controlData.ptr();
			}
		}
	}

	if ( !pReplacement->pControlBytes ) {
		Image *const control = pRegion->getMapPtr( T3DRegion::TYPE_CONTROL );
		if ( control ) {
			if ( control->get_format() != Image::FORMAT_RF ) {
				return nullptr;
			}
			rCold.controlData = control->get_data();
			const int texels = m_nRegionSize * m_nRegionSize;
			if ( rCold.controlData.size() < texels * ( int ) ( sizeof( uint32_t ) ) ) {
				rCold.controlData.clear();
				return nullptr;
			}
			pReplacement->pControlBytes = rCold.controlData.ptr();
		}
	}

	if ( !pReplacement->pDerivedBytes && pRegion->hasValidDerivedNormalSlopeMap() ) {
		const Ref<Image> derived = pRegion->getDerivedNormalSlopeMap();
		if ( derived.is_valid() && derived->get_format() == Image::FORMAT_RG8 ) {
			rCold.derivedData = derived->get_data();
		}
	}
	const int expected_derived_bytes = m_nRegionSize * m_nRegionSize * 2;
	if ( !pReplacement->pDerivedBytes && rCold.derivedData.size() >= expected_derived_bytes ) {
		pReplacement->pDerivedBytes = rCold.derivedData.ptr();
	}
	pReplacement->bValid = true;
	m_pLastView = pReplacement;
	return pReplacement;
}

bool T3DSampler::resolveGridUnchecked(
	const int64_t iGridX, const int64_t iGridZ, t3dRegionView_t *&pView,
	int &iIndex
) {
	int64_t region_x = 0;
	int64_t region_z = 0;
	int local_x = 0;
	int local_z = 0;
	resolveAxis( iGridX, region_x, local_x );
	resolveAxis( iGridZ, region_z, local_z );
	if ( region_x < INT32_MIN || region_x > INT32_MAX || region_z < INT32_MIN || region_z > INT32_MAX ) {
		return false;
	}
	pView = getRegionViewUnchecked( Vector2i( ( int32_t ) ( region_x ), ( int32_t ) ( region_z ) ) );
	if ( !pView ) {
		return false;
	}
	if ( ( uint32_t ) ( local_x ) >= ( uint32_t ) ( m_nRegionSize ) || ( uint32_t ) ( local_z ) >= ( uint32_t ) ( m_nRegionSize ) ) {
		return false;
	}
	iIndex = local_z * m_nRegionSize + local_x;
	return true;
}

float T3DSampler::sampleHeightGridUnchecked(
	const int64_t iGridX, const int64_t iGridZ,
	const bool bRespectHoles
) {
	t3dRegionView_t *pView = nullptr;
	int index = 0;
	if ( !resolveGridUnchecked( iGridX, iGridZ, pView, index ) ) {
		return NAN;
	}
	if ( bRespectHoles &&
		isHole( readControl( pView, index ) ) ) {
		return NAN;
	}
	return readHeight( pView->pHeightBytes, index );
}

uint32_t T3DSampler::sampleControlGridUnchecked( const int64_t iGridX, const int64_t iGridZ ) {
	t3dRegionView_t *pView = nullptr;
	int index = 0;
	if ( !resolveGridUnchecked( iGridX, iGridZ, pView, index ) ) {
		return UINT32_MAX;
	}
	return readControl( pView, index );
}

float T3DSampler::sampleHeightGrid(
	const int64_t iGridX, const int64_t iGridZ,
	const bool bRespectHoles
) {
	return validateEpoch() ? sampleHeightGridUnchecked( iGridX, iGridZ, bRespectHoles ) : NAN;
}

uint32_t T3DSampler::sampleControlGrid( const int64_t iGridX, const int64_t iGridZ ) {
	return validateEpoch() ? sampleControlGridUnchecked( iGridX, iGridZ ) : UINT32_MAX;
}

uint32_t T3DSampler::sampleControl( const Vector3 &rGlobalPosition ) {
	if ( !validateEpoch() ) {
		return UINT32_MAX;
	}
	return sampleControlGridUnchecked( ( int64_t ) ( Math::floor( ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing ) ),
		( int64_t ) ( Math::floor( ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing ) ) );
}

float T3DSampler::sampleHeightUnchecked( const Vector3 &rGlobalPosition ) {
	const double fGridX = ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing;
	const double fGridZ = ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing;
	const int64_t x0 = ( int64_t ) ( Math::floor( fGridX ) );
	const int64_t z0 = ( int64_t ) ( Math::floor( fGridZ ) );
	t3dRegionView_t *pView = nullptr;
	int index = 0;
	if ( !resolveGridUnchecked( x0, z0, pView, index ) ) {
		return NAN;
	}
	const uint32_t control = readControl( pView, index );
	if ( control == UINT32_MAX || isHole( control ) ) {
		return NAN;
	}

	const int64_t rounded_x = ( int64_t ) ( Math::round( fGridX ) );
	const int64_t rounded_z = ( int64_t ) ( Math::round( fGridZ ) );
	const double fDx = ( fGridX - ( double ) ( rounded_x ) ) * ( double ) ( m_fVertexSpacing );
	const double fDz = ( fGridZ - ( double ) ( rounded_z ) ) * ( double ) ( m_fVertexSpacing );
	if ( fDx * fDx + fDz * fDz < 0.0001 ) {
		return readHeight( pView->pHeightBytes, index );
	}

	const int local_x = m_nRegionShift >= 0 ? ( int ) ( index & ( int ) ( m_nRegionMask ) ) : index % m_nRegionSize;
	const int local_z = m_nRegionShift >= 0 ? index >> m_nRegionShift : index / m_nRegionSize;
	float fH00;
	float fH10;
	float fH01;
	float fH11;
	if ( local_x + 1 < m_nRegionSize && local_z + 1 < m_nRegionSize ) {
		// The overwhelmingly common path resolves one region and consumes two
		// adjacent R16 pairs. No cache scan, coordinate decomposition or metadata
		// load occurs for the remaining three samples.
		fH00 = readHeight( pView->pHeightBytes, index );
		fH10 = readHeight( pView->pHeightBytes, index + 1 );
		fH01 = readHeight( pView->pHeightBytes, index + m_nRegionSize );
		fH11 = readHeight( pView->pHeightBytes, index + m_nRegionSize + 1 );
	} else {
		fH00 = sampleHeightGridUnchecked( x0, z0, false );
		fH10 = sampleHeightGridUnchecked( x0 + 1, z0, false );
		fH01 = sampleHeightGridUnchecked( x0, z0 + 1, false );
		fH11 = sampleHeightGridUnchecked( x0 + 1, z0 + 1, false );
	}
	if ( std::isnan( fH00 ) || std::isnan( fH10 ) || std::isnan( fH01 ) || std::isnan( fH11 ) ) {
		return NAN;
	}
	const real_t fFx = ( real_t ) ( fGridX - ( double ) ( x0 ) );
	const real_t fFz = ( real_t ) ( fGridZ - ( double ) ( z0 ) );
	return Math::lerp( Math::lerp( fH00, fH10, fFx ), Math::lerp( fH01, fH11, fFx ), fFz );
}

float T3DSampler::sampleHeight( const Vector3 &rGlobalPosition ) {
	return validateEpoch() ? sampleHeightUnchecked( rGlobalPosition ) : NAN;
}

Vector3 T3DSampler::sampleNormalUnchecked( const Vector3 &rGlobalPosition, float *pHeight ) {
	if ( pHeight ) {
		*pHeight = NAN;
	}
	const double fGridX = ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing;
	const double fGridZ = ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing;
	const int64_t x0 = ( int64_t ) ( Math::floor( fGridX ) );
	const int64_t z0 = ( int64_t ) ( Math::floor( fGridZ ) );
	const bool bExactVertex = terrain3dSamplerIsExactVertex( fGridX, fGridZ, m_fVertexSpacing );

	float fHeights[3][3];
	const int needed = bExactVertex ? 2 : 3;
	t3dRegionView_t *pView = nullptr;
	int index = 0;
	if ( !resolveGridUnchecked( x0, z0, pView, index ) ) {
		return V3_NAN;
	}
	const int local_x = m_nRegionShift >= 0 ? ( int ) ( index & ( int ) ( m_nRegionMask ) ) : index % m_nRegionSize;
	const int local_z = m_nRegionShift >= 0 ? index >> m_nRegionShift : index / m_nRegionSize;
	const bool bInterior = local_x + needed <= m_nRegionSize && local_z + needed <= m_nRegionSize;
	if ( bInterior ) {
		const uint32_t c0 = readControl( pView, index );
		const uint32_t cx = readControl( pView, index + 1 );
		const uint32_t cz = readControl( pView, index + m_nRegionSize );
		if ( c0 == UINT32_MAX || cx == UINT32_MAX || cz == UINT32_MAX || isHole( c0 ) || isHole( cx ) || isHole( cz ) ) {
			return V3_NAN;
		}
		for ( int z = 0; z < needed; ++z ) {
			const int row = index + z * m_nRegionSize;
			for ( int x = 0; x < needed; ++x ) {
				fHeights[z][x] = readHeight( pView->pHeightBytes, row + x );
			}
		}
	} else {
		const uint32_t c0 = sampleControlGridUnchecked( x0, z0 );
		const uint32_t cx = sampleControlGridUnchecked( x0 + 1, z0 );
		const uint32_t cz = sampleControlGridUnchecked( x0, z0 + 1 );
		if ( c0 == UINT32_MAX || cx == UINT32_MAX || cz == UINT32_MAX || isHole( c0 ) || isHole( cx ) || isHole( cz ) ) {
			return V3_NAN;
		}
		for ( int z = 0; z < needed; ++z ) {
			for ( int x = 0; x < needed; ++x ) {
				fHeights[z][x] = sampleHeightGridUnchecked( x0 + x, z0 + z, false );
				if ( std::isnan( fHeights[z][x] ) ) {
					return V3_NAN;
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
		fHeight = terrain3dSamplerBilerp3x3( fHeights, 0, 0, fFx, fFz );
		fHeightX = terrain3dSamplerBilerp3x3( fHeights, 1, 0, fFx, fFz );
		fHeightZ = terrain3dSamplerBilerp3x3( fHeights, 0, 1, fFx, fFz );
	}
	if ( pHeight ) {
		*pHeight = fHeight;
	}
	return Vector3( fHeight - fHeightX, m_fVertexSpacing, fHeight - fHeightZ ).normalized();
}

Vector3 T3DSampler::sampleNormal( const Vector3 &rGlobalPosition ) {
	return validateEpoch() ? sampleNormalUnchecked( rGlobalPosition ) : V3_NAN;
}

bool T3DSampler::sampleHeightAndNormal(
	const Vector3 &rGlobalPosition, float &fHeight,
	Vector3 &rNormal
) {
	fHeight = NAN;
	rNormal = V3_NAN;
	if ( !validateEpoch() ) {
		return false;
	}
	rNormal = sampleNormalUnchecked( rGlobalPosition, &fHeight );
	return std::isfinite( fHeight ) && rNormal.is_finite();
}

Vector3 T3DSampler::sampleSlopeNormalUnchecked( const Vector3 &rGlobalPosition ) {
	const int64_t x = ( int64_t ) ( Math::round( ( double ) ( rGlobalPosition.x ) * m_fInverseVertexSpacing ) );
	const int64_t z = ( int64_t ) ( Math::round( ( double ) ( rGlobalPosition.z ) * m_fInverseVertexSpacing ) );
	t3dRegionView_t *pView = nullptr;
	int index = 0;
	if ( !resolveGridUnchecked( x, z, pView, index ) ) {
		return V3_NAN;
	}
	if ( pView->pDerivedBytes ) {
		const size_t offset = ( size_t ) ( index ) * 2u;
		const float fNx = ( float ) ( pView->pDerivedBytes[offset + 0u] ) / TERRAIN3D_BYTE_MAX_FLOAT * 2.0f - 1.0f;
		const float fNz = ( float ) ( pView->pDerivedBytes[offset + 1u] ) / TERRAIN3D_BYTE_MAX_FLOAT * 2.0f - 1.0f;
		const float fNy = Math::sqrt( MAX( 1.0f - fNx * fNx - fNz * fNz, 0.0f ) );
		Vector3 normal( fNx, fNy, fNz );
		if ( normal.length_squared() > CMP_EPSILON ) {
			return normal.normalized();
		}
	}
	float fHeight;
	float fHeightX;
	float fHeightZ;
	const int local_x = m_nRegionShift >= 0 ? ( int ) ( index & ( int ) ( m_nRegionMask ) ) : index % m_nRegionSize;
	const int local_z = m_nRegionShift >= 0 ? index >> m_nRegionShift : index / m_nRegionSize;
	if ( local_x + 1 < m_nRegionSize && local_z + 1 < m_nRegionSize ) {
		fHeight = readHeight( pView->pHeightBytes, index );
		fHeightX = readHeight( pView->pHeightBytes, index + 1 );
		fHeightZ = readHeight( pView->pHeightBytes, index + m_nRegionSize );
	} else {
		fHeight = sampleHeightGridUnchecked( x, z, false );
		fHeightX = sampleHeightGridUnchecked( x + 1, z, false );
		fHeightZ = sampleHeightGridUnchecked( x, z + 1, false );
	}
	fHeight = std::isnan( fHeight ) ? 0.0f : fHeight;
	fHeightX = std::isnan( fHeightX ) ? 0.0f : fHeightX;
	fHeightZ = std::isnan( fHeightZ ) ? 0.0f : fHeightZ;
	return Vector3( fHeight - fHeightX, m_fVertexSpacing, fHeight - fHeightZ ).normalized();
}

Vector3 T3DSampler::sampleSlopeNormal( const Vector3 &rGlobalPosition ) {
	return validateEpoch() ? sampleSlopeNormalUnchecked( rGlobalPosition ) : V3_NAN;
}

bool T3DSampler::sampleControlAndSlopeNormal(
	const Vector3 &rGlobalPosition, uint32_t &nControl,
	Vector3 &rNormal
) {
	nControl = UINT32_MAX;
	rNormal = V3_NAN;
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

bool T3DSampler::sampleHeights( const Vector3 *pPositions, float *pHeights, const int nCount ) {
	if ( !pPositions || !pHeights || nCount < 0 ) {
		return false;
	}
	if ( !validateEpoch() ) {
		std::fill_n( pHeights, nCount, NAN );
		return false;
	}
	for ( int index = 0; index < nCount; ++index ) {
		pHeights[index] = sampleHeightUnchecked( pPositions[index] );
	}
	return true;
}

bool T3DSampler::sampleControls( const Vector3 *pPositions, int32_t *pControls, const int nCount ) {
	if ( !pPositions || !pControls || nCount < 0 ) {
		return false;
	}
	if ( !validateEpoch() ) {
		std::fill_n( pControls, nCount, ( int32_t ) ( UINT32_MAX ) );
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

bool T3DSampler::sampleNormals( const Vector3 *pPositions, Vector3 *pNormals, const int nCount ) {
	if ( !pPositions || !pNormals || nCount < 0 ) {
		return false;
	}
	if ( !validateEpoch() ) {
		std::fill_n( pNormals, nCount, V3_NAN );
		return false;
	}
	for ( int index = 0; index < nCount; ++index ) {
		pNormals[index] = sampleNormalUnchecked( pPositions[index] );
	}
	return true;
}

bool T3DSampler::copyHeightRect(
	const int64_t iGridX, const int64_t iGridZ, const int nWidth,
	const int nHeight, float *const pDestination, const int nDestinationStride, const bool bRespectHoles
) {
	if ( !validateEpoch() || !pDestination || nWidth <= 0 || nHeight <= 0 || nDestinationStride < nWidth ) {
		return false;
	}
	bool bComplete = true;
	for ( int row = 0; row < nHeight; ++row ) {
		float *destination = pDestination + ( size_t ) ( row ) * ( size_t ) ( nDestinationStride );
		const int64_t grid_z = iGridZ + row;
		int64_t region_z = 0;
		int local_z = 0;
		resolveAxis( grid_z, region_z, local_z );
		int copied = 0;
		while ( copied < nWidth ) {
			const int64_t grid_x = iGridX + copied;
			int64_t region_x = 0;
			int local_x = 0;
			resolveAxis( grid_x, region_x, local_x );
			const int run = MIN( nWidth - copied, m_nRegionSize - local_x );
			if ( region_x < INT32_MIN || region_x > INT32_MAX || region_z < INT32_MIN || region_z > INT32_MAX ) {
				std::fill_n( destination + copied, run, NAN );
				bComplete = false;
				copied += run;
				continue;
			}
			t3dRegionView_t *const pView = getRegionViewUnchecked( Vector2i( ( int32_t ) ( region_x ), ( int32_t ) ( region_z ) ) );
			if ( !pView ) {
				std::fill_n( destination + copied, run, NAN );
				bComplete = false;
				copied += run;
				continue;
			}
			const int index = local_z * m_nRegionSize + local_x;
			const uint16_t *pHeight = ( const uint16_t * ) ( pView->pHeightBytes ) + index;
			if ( !bRespectHoles ) {
				for ( int i = 0; i < run; ++i ) {
					destination[copied + i] = T3DRegionCodec::decodeHeightU16( pHeight[i] );
				}
			} else {
				for ( int i = 0; i < run; ++i ) {
					const uint32_t control = readControl( pView, index + i );
					destination[copied + i] = isHole( control )
						? NAN
						: T3DRegionCodec::decodeHeightU16( pHeight[i] );
				}
			}
			copied += run;
		}
	}
	return bComplete;
}

bool T3DSampler::minimumHeightRect(
	const int64_t iGridX, const int64_t iGridZ, const int nWidth,
	const int nHeight, float &fHeight, const bool bRespectHoles
) {
	if ( !validateEpoch() || nWidth <= 0 || nHeight <= 0 ) {
		return false;
	}
	float fMinimum = std::numeric_limits<float>::infinity();
	for ( int row = 0; row < nHeight; ++row ) {
		const int64_t grid_z = iGridZ + row;
		int64_t region_z = 0;
		int local_z = 0;
		resolveAxis( grid_z, region_z, local_z );
		if ( region_z < INT32_MIN || region_z > INT32_MAX ) {
			return false;
		}
		int copied = 0;
		while ( copied < nWidth ) {
			const int64_t grid_x = iGridX + copied;
			int64_t region_x = 0;
			int local_x = 0;
			resolveAxis( grid_x, region_x, local_x );
			if ( region_x < INT32_MIN || region_x > INT32_MAX ) {
				return false;
			}
			const int run = MIN( nWidth - copied, m_nRegionSize - local_x );
			t3dRegionView_t *const pView = getRegionViewUnchecked( Vector2i( ( int32_t ) ( region_x ), ( int32_t ) ( region_z ) ) );
			if ( !pView ) {
				return false;
			}
			const int index = local_z * m_nRegionSize + local_x;
			const uint16_t *pHeight = ( const uint16_t * ) ( pView->pHeightBytes ) + index;
			for ( int i = 0; i < run; ++i ) {
				if ( bRespectHoles && isHole( readControl( pView, index + i ) ) ) {
					return false;
				}
				fMinimum = MIN( fMinimum, T3DRegionCodec::decodeHeightU16( pHeight[i] ) );
			}
			copied += run;
		}
	}
	if ( !std::isfinite( fMinimum ) ) {
		return false;
	}
	fHeight = fMinimum;
	return true;
}
