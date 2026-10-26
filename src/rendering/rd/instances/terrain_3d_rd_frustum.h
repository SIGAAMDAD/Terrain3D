// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_FRUSTUM_H
#define TERRAIN3D_RD_FRUSTUM_H

#include "Terrain3DTypes.h"

#include "core/config/terrain_3d_compiler.h"
#include <math.h>
#include <stdint.h>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/vector4.hpp>

using namespace godot;

// t3dRdFrustum_s centralized in Terrain3DTypes.h.


EA_FORCE_INLINE static Vector4 t3dRdNormalizeFrustumPlane( const Vector4 &rPlane ) {
	const double length_sq = rPlane.x * rPlane.x + rPlane.y * rPlane.y + rPlane.z * rPlane.z;
	if ( length_sq <= 1.0e-20 ) {
		return rPlane;
	}
	return rPlane * ( 1.0 / sqrt( length_sq ) );
}

EA_FORCE_INLINE static t3dRdFrustum_t t3dRdExtractFrustum( const Projection &rProjection ) {
	const Vector4 c0 = rProjection[0];
	const Vector4 c1 = rProjection[1];
	const Vector4 c2 = rProjection[2];
	const Vector4 c3 = rProjection[3];
	const Vector4 row0( c0.x, c1.x, c2.x, c3.x );
	const Vector4 row1( c0.y, c1.y, c2.y, c3.y );
	const Vector4 row2( c0.z, c1.z, c2.z, c3.z );
	const Vector4 row3( c0.w, c1.w, c2.w, c3.w );
	t3dRdFrustum_t result;
	result.m_szPlanes[0] = t3dRdNormalizeFrustumPlane( row3 + row0 );
	result.m_szPlanes[1] = t3dRdNormalizeFrustumPlane( row3 - row0 );
	result.m_szPlanes[2] = t3dRdNormalizeFrustumPlane( row3 + row1 );
	result.m_szPlanes[3] = t3dRdNormalizeFrustumPlane( row3 - row1 );
	result.m_szPlanes[4] = t3dRdNormalizeFrustumPlane( row3 - row2 );
	result.m_szPlanes[5] = t3dRdNormalizeFrustumPlane( row2 );
	return result;
}

EA_FORCE_INLINE static void t3dRdStoreFrustumPlanes( const t3dRdFrustum_t &rFrustum, float *const pPlanes ) {
	for ( uint32_t i = 0u; i < 6u; ++i ) {
		pPlanes[i * 4u + 0u] = ( float )rFrustum.m_szPlanes[i].x;
		pPlanes[i * 4u + 1u] = ( float )rFrustum.m_szPlanes[i].y;
		pPlanes[i * 4u + 2u] = ( float )rFrustum.m_szPlanes[i].z;
		pPlanes[i * 4u + 3u] = ( float )rFrustum.m_szPlanes[i].w;
	}
}

#endif // TERRAIN3D_RD_FRUSTUM_H
