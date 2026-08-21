// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_SAMPLER_H
#define TERRAIN3D_SAMPLER_H

#include "Terrain3DConstants.h"

#include "eastl_config.h"
#include <EASTL/array.h>
#include <cstdint>

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "data/terrain_3d_sampling_storage.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DRegionRepository;

// Low-overhead terrain query context. A sampler owns leases on a compact set of
// resident region planes, so cached raw pointers remain valid while the global
// resident pool continues to replace unrelated slots. Public operations check
// repository topology once, then all sub-samples use unchecked in-cache access.

typedef struct alignas( 64 ) t3dRegionView_s {
	Vector2i location = Vector2i( INT32_MAX, INT32_MAX );
	uint32_t nStamp = 0;
	uint32_t nRegionGeneration = 0u;
	uint32_t nValidatedOperation = 0u;
	bool bLoaded = false;
	bool bValid = false;
	bool bControlResolved = false;
	const uint8_t *pHeightBytes = nullptr;
	const uint8_t *pControlBytes = nullptr;
	const uint8_t *pDerivedBytes = nullptr;
} t3dRegionView_t;

typedef struct t3dRegionCold_s {
	T3DActiveMapLease activeLease;
	T3DResidentMapLease lease;
	PackedByteArray heightData;
	PackedByteArray controlData;
	PackedByteArray derivedData;

	void reset( void );
} t3dRegionCold_t;

class T3DSampler {
public:
	explicit T3DSampler( const T3DSamplingSource &rSource );

	float sampleHeight( const Vector3 &rGlobalPosition );
	float sampleHeightGrid( int64_t iGridX, int64_t iGridZ, bool bRespectHoles = false );
	uint32_t sampleControl( const Vector3 &rGlobalPosition );
	uint32_t sampleControlGrid( int64_t iGridX, int64_t iGridZ );
	Vector3 sampleNormal( const Vector3 &rGlobalPosition );
	Vector3 sampleSlopeNormal( const Vector3 &rGlobalPosition );
	bool sampleHeightAndNormal( const Vector3 &rGlobalPosition, float &fHeight, Vector3 &rNormal );
	bool sampleControlAndSlopeNormal( const Vector3 &rGlobalPosition, uint32_t &nControl, Vector3 &rNormal );
	bool sampleHeights( const Vector3 *pPositions, float *pHeights, int nCount );
	bool sampleControls( const Vector3 *pPositions, int32_t *pControls, int nCount );
	bool sampleNormals( const Vector3 *pPositions, Vector3 *pNormals, int nCount );

	// Copies a grid-aligned rectangle in row-major order. Region metadata is
	// resolved once per contiguous row span rather than once per texel.
	bool copyHeightRect(
		int64_t iGridX, int64_t iGridZ, int nWidth, int nHeight, float *pDestination,
		int nDestinationStride, bool bRespectHoles = false
	);
	bool minimumHeightRect(
		int64_t iGridX, int64_t iGridZ, int nWidth, int nHeight, float &fHeight,
		bool bRespectHoles = true
	);

private:
	// Cache probes and successful samples touch only this cacheline-aligned hot
	// descriptor. Ownership and fallback containers live in the parallel cold bank.




	T3DSamplingSource m_source;
	eastl::array<t3dRegionView_t, TERRAIN3D_SAMPLER_CACHE_SIZE> m_szCache{};
	eastl::array<t3dRegionCold_t, TERRAIN3D_SAMPLER_CACHE_SIZE> m_szCacheCold{};
	t3dRegionView_t *m_pLastView = nullptr;
	uint32_t m_stamp = 1;
	uint32_t m_operation = 1u;
	uint64_t m_nSamplingGeneration = 0u;
	int m_nRegionSize = 0;
	int m_nRegionShift = -1;
	int64_t m_nRegionMask = 0;
	real_t m_fVertexSpacing = 0.0f;
	double m_fInverseVertexSpacing = 0.0;

	bool validateEpoch( void );
	t3dRegionView_t *validateCachedView( t3dRegionView_t *pView, const Vector2i &rLocation );
	t3dRegionView_t *getRegionViewUnchecked( const Vector2i &rLocation );
	bool resolveGridUnchecked( int64_t iGridX, int64_t iGridZ, t3dRegionView_t *&pView, int &iIndex );
	float sampleHeightGridUnchecked( int64_t iGridX, int64_t iGridZ, bool bRespectHoles );
	uint32_t sampleControlGridUnchecked( int64_t iGridX, int64_t iGridZ );
	float sampleHeightUnchecked( const Vector3 &rGlobalPosition );
	Vector3 sampleNormalUnchecked( const Vector3 &rGlobalPosition, float *pHeight = nullptr );
	Vector3 sampleSlopeNormalUnchecked( const Vector3 &rGlobalPosition );
	void resetCache( uint64_t nGeneration );
	void resetView( t3dRegionView_t *pView );
	static int64_t floorDiv( int64_t nValue, int64_t nDivisor );
	void resolveAxis( int64_t nGrid, int64_t &nRegion, int &nLocal ) const;
	static float readHeight( const uint8_t *pBytes, int iIndex );
	static uint32_t readUint( const uint8_t *pBytes, int iIndex );
	static uint32_t readControl( const t3dRegionView_t *pView, int iIndex );
};

#endif // TERRAIN3D_SAMPLER_H
