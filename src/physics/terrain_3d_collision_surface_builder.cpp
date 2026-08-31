// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "physics/terrain_3d_collision_surface_builder.h"

#include "core/logger.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_data.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_sampler.h"
#include "memory/terrain_3d_memory.h"
#include "rendering/terrain_3d_material.h"
#include "terrain_3d.h"

#include <float.h>
#include <math.h>

size_t Terrain3DCollisionSurfaceBuilder::getRetainedBufferBytes( void ) const {
	return m_fullHeightRowScratch.residentBytes();
}

bool Terrain3DCollisionSurfaceBuilder::prepareRuntimeScratch( const int nAxisSamples ) {
	Terrain3DColdAllocationScope allocation_scope;
	if ( nAxisSamples <= 0 || nAxisSamples > s_nMaxCollisionAxisSamples ) {
		return false;
	}
	if ( nAxisSamples <= s_nDynamicScratchAxis ) {
		return true;
	}
	const size_t sample_count = ( size_t )( nAxisSamples ) * ( size_t )( nAxisSamples );
	if ( sample_count <= m_fullHeightRowScratch.capacity() ) {
		return true;
	}
	Terrain3DMemoryArenaLayout layout( sample_count * sizeof( float ) + s_nArenaDefaultBaseAlignment );
	t3dMemoryArenaSlice_t scratch_slice;
	Terrain3DMemoryArena next_arena;
	if ( !layout.reserveLow( sample_count * sizeof( float ), alignof( float ), scratch_slice ) || !layout.seal()
	        || !next_arena.initialize( layout ) || !next_arena.commit() || !next_arena.seal() ) {
		return false;
	}
	const t3dMemoryArenaSpan_t span = next_arena.resolve( scratch_slice );
	if ( !span.valid() || !m_fullHeightRowScratch.bind( ( float * )( span.m_pData ), sample_count ) ) {
		return false;
	}
	m_fullHeightScratchArena.swap( next_arena );
	return true;
}

void Terrain3DCollisionSurfaceBuilder::refreshRegionBlendCurve( const int nRegionSize, const real_t fRegionBlend ) {
	if ( nRegionSize <= 0 ) {
		eastl::fill_n( m_fRegionBlendCurve, s_nMaxRegionBlendSamples, 0.0f );
		m_nRegionBlendCurveSize = 0;
		m_fRegionBlendCurveSetting = -1.0f;
		return;
	}
	if ( m_nRegionBlendCurveSize == nRegionSize && Math::is_equal_approx( m_fRegionBlendCurveSetting, fRegionBlend ) ) {
		return;
	}
	if ( nRegionSize > s_nMaxRegionBlendSamples ) {
		m_nRegionBlendCurveSize = 0;
		return;
	}
	const real_t blend_factor = 2.0f + 126.0f * ( 1.0f - fRegionBlend );
	for ( int local = 0; local < nRegionSize; ++local ) {
		const real_t biased = ( real_t )( local ) / ( real_t )( nRegionSize )-0.5011f;
		real_t fraction = biased - Math::floor( biased );
		fraction = Math::clamp( fraction, ( real_t )( 1e-8f ), ( real_t )( 1.0f - 1e-8f ) );
		m_fRegionBlendCurve[( size_t )( local )] =
		        1.0f / ( 1.0f + Math::exp( blend_factor * Math::log( ( 1.0f - fraction ) / fraction ) ) );
	}
	m_nRegionBlendCurveSize = nRegionSize;
	m_fRegionBlendCurveSetting = fRegionBlend;
}

real_t Terrain3DCollisionSurfaceBuilder::getRegionBlendWeight( const int64_t nGridCoordinate ) const {
	if ( m_nRegionBlendCurveSize <= 0 ) {
		return 0.0f;
	}
	int64_t local = nGridCoordinate % ( int64_t )( m_nRegionBlendCurveSize );
	if ( local < 0 ) {
		local += m_nRegionBlendCurveSize;
	}
	return m_fRegionBlendCurve[( size_t )( local )];
}

real_t Terrain3DCollisionSurfaceBuilder::getRegionBlend(
        const int iX, const int iZ, const int iMinRegionX, const int iMinRegionZ, const int nPresenceWidth ) const {
	const int32_t x0 = m_szBlendRegionX0[( size_t )( iX )] - iMinRegionX;
	const int32_t x1 = m_szBlendRegionX1[( size_t )( iX )] - iMinRegionX;
	const int32_t z0 = m_szBlendRegionZ0[( size_t )( iZ )] - iMinRegionZ;
	const int32_t z1 = m_szBlendRegionZ1[( size_t )( iZ )] - iMinRegionZ;
	const uint8_t *const presence = m_regionPresenceScratch;
	const real_t d = ( real_t )( presence[( size_t )( z0 ) * ( size_t )( nPresenceWidth ) + ( size_t )( x0 )] );
	const real_t c = ( real_t )( presence[( size_t )( z0 ) * ( size_t )( nPresenceWidth ) + ( size_t )( x1 )] );
	const real_t a = ( real_t )( presence[( size_t )( z1 ) * ( size_t )( nPresenceWidth ) + ( size_t )( x0 )] );
	const real_t b = ( real_t )( presence[( size_t )( z1 ) * ( size_t )( nPresenceWidth ) + ( size_t )( x1 )] );
	const real_t wx = m_fBlendWeightX[( size_t )( iX )];
	const real_t wz = m_fBlendWeightZ[( size_t )( iZ )];
	const real_t blend = Math::lerp( Math::lerp( d, c, wx ), Math::lerp( a, b, wx ), wz );
	return ( 1.0f - blend ) * 2.0f;
}

bool Terrain3DCollisionSurfaceBuilder::build( const Terrain3DData *const pData,
        const Ref<Terrain3DMaterial> &rMaterial,
        const Vector2i &rPosition,
        const int nSize,
        PackedFloat32Array &rHeights,
        float &rMinHeight,
        float &rMaxHeight ) {
	if ( !pData || !rMaterial.is_valid() || nSize < 1 ) {
		return false;
	}
	const int region_size = pData->getRegionSizeInternal();
	if ( region_size <= 0 ) {
		return false;
	}
	const Vector2i origin_location = V2I_DIVIDE_FLOOR( rPosition, region_size );
	const Terrain3DRegion *const pOriginRegion = pData->getRegionPtr( origin_location );
	if ( !pOriginRegion || pOriginRegion->isDeleted() ) {
		return false;
	}

	const t3dWorldBackground_t background_mode = rMaterial->getWorldBackground();
	const bool blend_background = background_mode == t3dWorldBackground_t::FLAT || background_mode == t3dWorldBackground_t::NOISE;
	const real_t ground_level = rMaterial->get( "ground_level" );
	const real_t region_blend = rMaterial->get( "region_blend" );
	const real_t region_texel_size = 1.0f / ( real_t )( region_size );
	if ( blend_background ) {
		refreshRegionBlendCurve( region_size, region_blend );
	}

	const int width = nSize + 1;
	if ( width > s_nMaxCollisionAxisSamples ) {
		return false;
	}
	int32_t min_blend_region_x = INT32_MAX;
	int32_t max_blend_region_x = INT32_MIN;
	int32_t min_blend_region_z = INT32_MAX;
	int32_t max_blend_region_z = INT32_MIN;
	if ( blend_background ) {
		for ( int i = 0; i < width; ++i ) {
			const int64_t sample_x = ( int64_t )( rPosition.x ) + i;
			const int64_t sample_z = ( int64_t )( rPosition.y ) + i;
			const int32_t x0 = ( int32_t )( Math::floor( ( double )( sample_x ) * ( double )( region_texel_size )-0.5011 ) );
			const int32_t z0 = ( int32_t )( Math::floor( ( double )( sample_z ) * ( double )( region_texel_size )-0.5011 ) );
			m_szBlendRegionX0[( size_t )( i )] = x0;
			m_szBlendRegionX1[( size_t )( i )] = x0 + 1;
			m_szBlendRegionZ0[( size_t )( i )] = z0;
			m_szBlendRegionZ1[( size_t )( i )] = z0 + 1;
			m_fBlendWeightX[( size_t )( i )] = getRegionBlendWeight( sample_x );
			m_fBlendWeightZ[( size_t )( i )] = getRegionBlendWeight( sample_z );
			min_blend_region_x = MIN( min_blend_region_x, x0 );
			max_blend_region_x = MAX( max_blend_region_x, x0 + 1 );
			min_blend_region_z = MIN( min_blend_region_z, z0 );
			max_blend_region_z = MAX( max_blend_region_z, z0 + 1 );
		}
		const int presence_width = max_blend_region_x - min_blend_region_x + 1;
		const int presence_height = max_blend_region_z - min_blend_region_z + 1;
		if ( presence_width <= 0 || presence_height <= 0 || presence_width * presence_height > s_nRegionPresenceSamples ) {
			return false;
		}
		for ( int z = 0; z < presence_height; ++z ) {
			for ( int x = 0; x < presence_width; ++x ) {
				const Terrain3DRegion *const pRegion = pData->getRegionPtr( Vector2i( min_blend_region_x + x, min_blend_region_z + z ) );
				m_regionPresenceScratch[( size_t )( z ) * ( size_t )( presence_width ) + ( size_t )( x )] =
				        pRegion && !pRegion->isDeleted() ? ( uint8_t )( 1 ) : ( uint8_t )( 0 );
			}
		}
	}

	const int sample_count = width * width;
	float *pHeightScratch = NULL;
	if ( width <= s_nDynamicScratchAxis ) {
		pHeightScratch = m_dynamicHeightScratch;
	} else {
		if ( m_fullHeightRowScratch.capacity() < ( size_t )sample_count ) {
			UtilityFunctions::push_error( "Collision surface scratch capacity exceeded; collision rebuild required" );
			return false;
		}
		pHeightScratch = m_fullHeightRowScratch.data();
	}
	Terrain3DSampler sampler( pData->getSamplingSourceInternal() );
	if ( !sampler.copyHeightRect( rPosition.x, rPosition.y, width, width, pHeightScratch, width, true ) ) {
		return false;
	}
	if ( rHeights.size() != sample_count ) {
		UtilityFunctions::push_error( "Collision output height capacity mismatch; collision rebuild required" );
		return false;
	}
	float *const output = rHeights.ptrw();
	rMinHeight = FLT_MAX;
	rMaxHeight = -FLT_MAX;
	for ( int x = 0; x < width; ++x ) {
		const int destination_base = x * width;
		for ( int z = width - 1, destination = 0; z >= 0; --z, ++destination ) {
			float height = pHeightScratch[( size_t )( z ) * ( size_t )( width ) + ( size_t )( x )];
			if ( !isnan( height ) && blend_background ) {
				height = ( float )Math::lerp( ( real_t )( height ),
				        ground_level,
				        t3dSmoothstep( 0.0f,
				                1.0f,
				                getRegionBlend(
				                        x, z, min_blend_region_x, min_blend_region_z, max_blend_region_x - min_blend_region_x + 1 ) ) );
			}
			output[destination_base + destination] = height;
			if ( !isnan( height ) ) {
				rMinHeight = MIN( rMinHeight, height );
				rMaxHeight = MAX( rMaxHeight, height );
			}
		}
	}
	if ( rMinHeight == FLT_MAX ) {
		// Preserve an all-hole surface in cooked pages. Runtime collision callers
		// can still treat it as an inactive shape, while the page remains valid.
		rMinHeight = 0.0f;
		rMaxHeight = 0.0f;
	}
	return true;
}
