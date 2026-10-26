// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/cooking/terrain_3d_control_map_cooker.h"

#include "core/utility/terrain_3d_util.h"

#include <string.h>

bool t3dControlCookSetExplicitSource( t3dControlCookSource_t &rSource, const Ref<Image> &rControl ) {
	if ( rControl.is_null() || rControl->get_width() != T3D_REGION_SIZE || rControl->get_height() != T3D_REGION_SIZE
	        || rControl->get_format() != Image::FORMAT_RF || rControl->has_mipmaps() ) {
		return false;
	}
	rSource.m_bytes = rControl->get_data();
	if ( rSource.m_bytes.size() != T3D_REGION_SIZE * T3D_REGION_SIZE * ( int )( sizeof( uint32_t ) ) ) {
		return false;
	}
	rSource.m_bPresent = true;
	rSource.m_bImplicit = false;
	return true;
}



uint32_t t3dControlCookMaterialMaskFromControlImage( const Ref<Image> &rControl ) {
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
		const uint32_t control = t3dControlCookReadU32Unaligned( pRead, index * 4u );
		mask |= 1u << ( uint32_t )( t3dGetBase( control ) & 31u );
		mask |= 1u << ( uint32_t )( t3dGetOverlay( control ) & 31u );
	}
	return mask != 0u ? mask : 1u;
}



static bool t3dControlCookReadControl(
        const t3dControlCookNeighborhood_t &rNeighborhood, const int iX, const int iZ, uint32_t &nControl ) {
	int region_dx = 0;
	int region_dz = 0;
	int local_x = iX;
	int local_z = iZ;
	if ( local_x < 0 ) {
		region_dx = -1;
		local_x += T3D_REGION_SIZE;
	} else if ( local_x >= T3D_REGION_SIZE ) {
		region_dx = 1;
		local_x -= T3D_REGION_SIZE;
	}
	if ( local_z < 0 ) {
		region_dz = -1;
		local_z += T3D_REGION_SIZE;
	} else if ( local_z >= T3D_REGION_SIZE ) {
		region_dz = 1;
		local_z -= T3D_REGION_SIZE;
	}
	const t3dControlCookSource_t &source = rNeighborhood.m_sources[( size_t )( ( region_dz + 1 ) * 3 + ( region_dx + 1 ) )];
	if ( !source.m_bPresent ) {
		return false;
	}
	if ( source.m_bImplicit ) {
		nControl = T3D_REGION_DEFAULT_CONTROL;
		return true;
	}
	const size_t offset = ( size_t )( local_z * T3D_REGION_SIZE + local_x ) * sizeof( uint32_t );
	memcpy( &nControl, source.m_bytes.ptr() + offset, sizeof( uint32_t ) );
	return true;
}

Ref<Image> t3dCookGpuRenderControlImage( const t3dControlCookNeighborhood_t &rNeighborhood ) {
	PackedByteArray output;
	output.resize( T3D_REGION_SIZE * T3D_REGION_SIZE * ( int )( sizeof( uint32_t ) ) );
	uint8_t *const pWrite = output.ptrw();
	for ( int z = 0; z < T3D_REGION_SIZE; ++z ) {
		for ( int x = 0; x < T3D_REGION_SIZE; ++x ) {
			uint32_t center = T3D_REGION_DEFAULT_CONTROL;
			if ( !t3dControlCookReadControl( rNeighborhood, x, z, center ) ) {
				memcpy( pWrite + ( size_t )( z * T3D_REGION_SIZE + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}
			if ( t3dIsAuto( center ) || t3dIsHole( center ) ) {
				memcpy( pWrite + ( size_t )( z * T3D_REGION_SIZE + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
				continue;
			}

			uint64_t material_weights[T3D_MAX_TEXTURE_ASSETS];
			memset( material_weights, 0, sizeof( material_weights ) );
			for ( int dz = -T3D_RENDER_CONTROL_BLEND_RADIUS; dz <= T3D_RENDER_CONTROL_BLEND_RADIUS; ++dz ) {
				for ( int dx = -T3D_RENDER_CONTROL_BLEND_RADIUS; dx <= T3D_RENDER_CONTROL_BLEND_RADIUS; ++dx ) {
					uint32_t pControl = T3D_REGION_DEFAULT_CONTROL;
					if ( !t3dControlCookReadControl( rNeighborhood, x + dx, z + dz, pControl ) || t3dIsAuto( pControl ) || t3dIsHole( pControl ) ) {
						continue;
					}
					const uint64_t spatial_weight = ( uint64_t )( s_szRenderControlKernel[dx + T3D_RENDER_CONTROL_BLEND_RADIUS]
					        * s_szRenderControlKernel[dz + T3D_RENDER_CONTROL_BLEND_RADIUS] );
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
			for ( int material = 0; material < T3D_MAX_TEXTURE_ASSETS; ++material ) {
				if ( strongest < 0 || material_weights[( size_t )( material )] > material_weights[( size_t )( strongest )] ) {
					second = strongest;
					strongest = material;
				} else if ( second < 0 || material_weights[( size_t )( material )] > material_weights[( size_t )( second )] ) {
					second = material;
				}
			}
			if ( strongest < 0 || material_weights[( size_t )( strongest )] == 0u ) {
				memcpy( pWrite + ( size_t )( z * T3D_REGION_SIZE + x ) * sizeof( uint32_t ), &center, sizeof( uint32_t ) );
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
				const int temporary = base;
				base = overlay;
				overlay = temporary;
			}
			const uint64_t base_weight = material_weights[( size_t )( base )];
			const uint64_t overlay_weight = material_weights[( size_t )( overlay )];
			const uint64_t pair_weight = base_weight + overlay_weight;
			uint8_t blend = 0u;
			if ( base != overlay && pair_weight > 0u ) {
				blend = ( uint8_t )( CLAMP( ( int )( ( overlay_weight * 255u + pair_weight / 2u ) / pair_weight ), 0, 255 ) );
			}
			const uint32_t cooked = ( center & ~T3D_RENDER_CONTROL_MATERIAL_MASK ) | t3dEncBase( ( uint8_t )( base ) )
			        | t3dEncOverlay( ( uint8_t )( overlay ) ) | t3dEncBlend( blend );
			memcpy( pWrite + ( size_t )( z * T3D_REGION_SIZE + x ) * sizeof( uint32_t ), &cooked, sizeof( uint32_t ) );
		}
	}
	return Image::create_from_data( T3D_REGION_SIZE, T3D_REGION_SIZE, false, Image::FORMAT_RF, output );
}
