// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_INSTANCE_RECORD_H
#define TERRAIN3D_RD_INSTANCE_RECORD_H

#include "data/terrain_3d_half_buffer.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <math.h>

EA_FORCE_INLINE uint32_t t3dRdPackInstanceSnormPair( const float x, const float y ) {
	const int16_t px = ( int16_t )( lround( CLAMP( x, -1.0f, 1.0f ) * 32767.0f ) );
	const int16_t py = ( int16_t )( lround( CLAMP( y, -1.0f, 1.0f ) * 32767.0f ) );
	return ( uint32_t )( ( uint16_t )( px ) ) | ( ( uint32_t )( ( uint16_t )( py ) ) << 16u );
}

EA_FORCE_INLINE t3dRdInstanceRecord_t t3dRdPackInstanceRecord(
        const Transform3D &rTransform, const uint32_t nColorRgba32 = 0xffffffffu ) {
	t3dRdInstanceRecord_t record;
	record.m_szPosition[0] = ( float )( rTransform.origin.x );
	record.m_szPosition[1] = ( float )( rTransform.origin.y );
	record.m_szPosition[2] = ( float )( rTransform.origin.z );
	const Vector3 scale = rTransform.basis.get_scale();
	const Quaternion rotation = rTransform.basis.orthonormalized().get_rotation_quaternion().normalized();
	record.m_nQuaternion01 = t3dRdPackInstanceSnormPair( ( float )( rotation.x ), ( float )( rotation.y ) );
	record.m_nQuaternion23 = t3dRdPackInstanceSnormPair( ( float )( rotation.z ), ( float )( rotation.w ) );
	record.m_nScale01 =
	        ( uint32_t )( t3dHalfEncode( ( float )( scale.x ) ) ) | ( ( uint32_t )( t3dHalfEncode( ( float )( scale.y ) ) ) << 16u );
	record.m_nScale2Flags = ( uint32_t )( t3dHalfEncode( ( float )( scale.z ) ) );
	record.m_nColorRgba8 = t3dRdPackGodotRgba32ForGpu( nColorRgba32 );
	return record;
}

#endif // TERRAIN3D_RD_INSTANCE_RECORD_H
