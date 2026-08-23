// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_culling.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static Vector4 t3dRdTransformPoint( const Projection &rProjection, const float x, const float y, const float z ) {
	const Vector4 c0 = rProjection[0];
	const Vector4 c1 = rProjection[1];
	const Vector4 c2 = rProjection[2];
	const Vector4 c3 = rProjection[3];
	return Vector4( c0.x * x + c1.x * y + c2.x * z + c3.x,
			c0.y * x + c1.y * y + c2.y * z + c3.y,
			c0.z * x + c1.z * y + c2.z * z + c3.z,
			c0.w * x + c1.w * y + c2.w * z + c3.w );
}

uint32_t Terrain3DRdCpuCuller::getDrawGroup( const t3dRdPatch_t &rPatch ) {
	if ( rPatch.m_nLod == 0u ) return T3D_RD_DRAW_GROUP_LOD0_NEAR;
	if ( rPatch.m_nTier == 0u ) return T3D_RD_DRAW_GROUP_OUTER_NEAR;
	if ( rPatch.m_nTier == 1u ) return T3D_RD_DRAW_GROUP_OUTER_MIDDLE;
	return T3D_RD_DRAW_GROUP_OUTER_FAR;
}

bool Terrain3DRdCpuCuller::isPatchValid( const t3dRdPatch_t &rPatch ) {
	return std::isfinite( rPatch.m_fOriginX ) && std::isfinite( rPatch.m_fOriginZ ) &&
			std::isfinite( rPatch.m_fScale ) && rPatch.m_fScale > 0.0f &&
			rPatch.m_nLod < s_nMaxClipmapLevels && rPatch.m_nTier <= 2u &&
			rPatch.m_nStitchX <= 2u && rPatch.m_nStitchZ <= 2u &&
			std::isfinite( rPatch.m_fBoundsMinY ) && std::isfinite( rPatch.m_fBoundsMaxY ) &&
			std::isfinite( rPatch.m_fBoundsHalfExtent ) &&
			rPatch.m_fBoundsMinY <= rPatch.m_fBoundsMaxY && rPatch.m_fBoundsHalfExtent > 0.0f &&
			std::isfinite( rPatch.m_fPreviousOriginX ) && std::isfinite( rPatch.m_fPreviousOriginZ ) &&
			std::isfinite( rPatch.m_fPreviousScale ) && rPatch.m_fPreviousScale > 0.0f;
}

bool Terrain3DRdCpuCuller::isPatchVisible( const t3dRdPatch_t &rPatch, const Projection &rViewProjection ) {
	if ( !isPatchValid( rPatch ) ) return false;
	const float x0 = rPatch.m_fOriginX - rPatch.m_fBoundsHalfExtent;
	const float x1 = rPatch.m_fOriginX + rPatch.m_fBoundsHalfExtent;
	const float z0 = rPatch.m_fOriginZ - rPatch.m_fBoundsHalfExtent;
	const float z1 = rPatch.m_fOriginZ + rPatch.m_fBoundsHalfExtent;
	const float y0 = rPatch.m_fBoundsMinY;
	const float y1 = rPatch.m_fBoundsMaxY;
	const Vector4 corners[8] = {
		t3dRdTransformPoint( rViewProjection, x0, y0, z0 ),
		t3dRdTransformPoint( rViewProjection, x1, y0, z0 ),
		t3dRdTransformPoint( rViewProjection, x0, y1, z0 ),
		t3dRdTransformPoint( rViewProjection, x1, y1, z0 ),
		t3dRdTransformPoint( rViewProjection, x0, y0, z1 ),
		t3dRdTransformPoint( rViewProjection, x1, y0, z1 ),
		t3dRdTransformPoint( rViewProjection, x0, y1, z1 ),
		t3dRdTransformPoint( rViewProjection, x1, y1, z1 ),
	};
	bool outside_left = true;
	bool outside_right = true;
	bool outside_bottom = true;
	bool outside_top = true;
	bool outside_near = true;
	bool outside_far = true;
	for ( const Vector4 &corner : corners ) {
		outside_left = outside_left && corner.x < -corner.w;
		outside_right = outside_right && corner.x > corner.w;
		outside_bottom = outside_bottom && corner.y < -corner.w;
		outside_top = outside_top && corner.y > corner.w;
		outside_near = outside_near && corner.z > corner.w;
		outside_far = outside_far && corner.z < 0.0f;
	}
	return !( outside_left || outside_right || outside_bottom || outside_top || outside_near || outside_far );
}

bool Terrain3DRdCpuCuller::isCommandRangeValid( const uint32_t nGroup,
		const uint32_t nBufferBytes,
		const uint32_t nStride,
		const uint32_t nDrawCount ) {
	constexpr size_t command_bytes = sizeof( t3dRdIndexedIndirectCommand_t );
	if ( nGroup >= s_nRdDrawGroupCount || nDrawCount == 0u || nStride < command_bytes || ( nStride & 3u ) != 0u )
		return false;
	const size_t offset = ( size_t )( nGroup ) * nStride;
	const size_t last_offset = offset + ( size_t )( nDrawCount - 1u ) * nStride;
	return last_offset >= offset && last_offset <= nBufferBytes && command_bytes <= nBufferBytes - last_offset;
}

bool Terrain3DRdCpuCuller::build( const t3dRdFrameSnapshot_t &rSnapshot,
		const Projection &rViewProjection,
		const uint32_t nLod0IndexCount,
		const uint32_t nOuterIndexCount,
		t3dRdCpuCullResult_t &rResult ) {
	rResult = t3dRdCpuCullResult_t{};
	if ( rSnapshot.m_nPatchCount > s_nMaxClipmapLevels || nLod0IndexCount == 0u || nOuterIndexCount == 0u ) return false;
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		t3dRdIndexedIndirectCommand_t &command = rResult.m_szCommands[group];
		command.m_nIndexCount = group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? nLod0IndexCount : nOuterIndexCount;
		command.m_nFirstInstance = group * s_nMaxClipmapLevels;
	}
	for ( uint32_t patch_index = 0u; patch_index < rSnapshot.m_nPatchCount; ++patch_index ) {
		const t3dRdPatch_t &patch = rSnapshot.m_szPatches[patch_index];
		if ( !isPatchValid( patch ) ) {
			++rResult.m_nInvalidPatchCount;
			continue;
		}
		if ( !isPatchVisible( patch, rViewProjection ) ) continue;
		const uint32_t group = getDrawGroup( patch );
		t3dRdIndexedIndirectCommand_t &command = rResult.m_szCommands[group];
		if ( command.m_nInstanceCount >= s_nMaxClipmapLevels ) return false;
		const uint32_t slot = command.m_nInstanceCount++;
		rResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot] = patch_index;
		++rResult.m_nVisibleCount;
	}
	return rResult.m_nInvalidPatchCount == 0u &&
			validate( rSnapshot, rViewProjection, nLod0IndexCount, nOuterIndexCount, rResult );
}

bool Terrain3DRdCpuCuller::validate( const t3dRdFrameSnapshot_t &rSnapshot,
		const Projection &rViewProjection,
		const uint32_t nLod0IndexCount,
		const uint32_t nOuterIndexCount,
		const t3dRdCpuCullResult_t &rResult ) {
	if ( rSnapshot.m_nPatchCount > s_nMaxClipmapLevels || rResult.m_nInvalidPatchCount != 0u ) return false;
	uint32_t visible_count = 0u;
	bool seen[s_nMaxClipmapLevels]{};
	constexpr uint32_t buffer_bytes = sizeof( t3dRdIndexedIndirectCommand_t ) * s_nRdDrawGroupCount;
	for ( uint32_t group = 0u; group < s_nRdDrawGroupCount; ++group ) {
		if ( !isCommandRangeValid( group, buffer_bytes ) ) return false;
		const t3dRdIndexedIndirectCommand_t &command = rResult.m_szCommands[group];
		const uint32_t expected_indices = group == T3D_RD_DRAW_GROUP_LOD0_NEAR ? nLod0IndexCount : nOuterIndexCount;
		if ( command.m_nIndexCount != expected_indices || command.m_nInstanceCount > s_nMaxClipmapLevels ||
				command.m_nFirstIndex != 0u || command.m_nVertexOffset != 0 ||
				command.m_nFirstInstance != group * s_nMaxClipmapLevels ) return false;
		for ( uint32_t slot = 0u; slot < command.m_nInstanceCount; ++slot ) {
			const uint32_t patch_index = rResult.m_szVisibleIndices[group * s_nMaxClipmapLevels + slot];
			if ( patch_index >= rSnapshot.m_nPatchCount ) return false;
			if ( seen[patch_index] ) return false;
			seen[patch_index] = true;
			const t3dRdPatch_t &patch = rSnapshot.m_szPatches[patch_index];
			if ( getDrawGroup( patch ) != group || !isPatchVisible( patch, rViewProjection ) ) return false;
		}
		visible_count += command.m_nInstanceCount;
	}
	return visible_count == rResult.m_nVisibleCount;
}
