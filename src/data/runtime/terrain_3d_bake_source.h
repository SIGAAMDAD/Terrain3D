#ifndef TERRAIN3D_BAKE_SOURCE_H
#define TERRAIN3D_BAKE_SOURCE_H
#include "Terrain3DTypes.h"
#include "eastl_config.h"
#include <EASTL/vector.h>
typedef struct t3dTriangleBuffer_s
{
	eastl::vector< Vector3, Terrain3DEastlAllocator > m_ownedVertices;
	eastl::vector< Vector2, Terrain3DEastlAllocator > m_ownedUvs;
	Vector3											 *m_pExternalVertices = NULL;
	Vector2											 *m_pExternalUvs = NULL;
	size_t											  m_nExternalCapacity = 0u;
	size_t											  m_nExternalSize = 0u;
	bool											  m_bOverflowed = false;

	void bindExternal( Vector3 *const pVertices, Vector2 *const pUvs, const size_t nCapacity );
	void reserve( const size_t nCapacity, const bool bUvs );
	void appendTriangle( const Vector3 &rA, const Vector3 &rB, const Vector3 &rC, const bool bUvs );

	EA_FORCE_INLINE size_t size(
		void ) const {
		return m_pExternalVertices ? m_nExternalSize : m_ownedVertices.size();
	}

	EA_FORCE_INLINE size_t uvSize(
		void ) const {
		return m_pExternalVertices ? ( m_pExternalUvs ? m_nExternalSize : 0u ) : m_ownedUvs.size();
	}

	EA_FORCE_INLINE const Vector3 *vertexData(
		void ) const {
		return m_pExternalVertices ? m_pExternalVertices : m_ownedVertices.data();
	}

	EA_FORCE_INLINE const Vector2 *uvData(
		void ) const {
		return m_pExternalVertices ? m_pExternalUvs : m_ownedUvs.data();
	}
} t3dTriangleBuffer_t;

typedef struct t3dBakeRegionSnapshot_s
{
	Vector2i		m_location;
	PackedByteArray m_heightData;
	PackedByteArray m_controlData;
	const uint8_t  *m_pHeightBytes = NULL;
	const uint8_t  *m_pControlBytes = NULL;
} t3dBakeRegionSnapshot_t;

typedef struct t3dBakeSnapshot_s
{
	real_t	m_fVertexSpacing = 1.f;
	int32_t m_szRegionLookup[T3D_REGION_MAP_SIZE * T3D_REGION_MAP_SIZE] = {};
	eastl::vector< t3dBakeRegionSnapshot_t, Terrain3DEastlAllocator > m_regions;

	const t3dBakeRegionSnapshot_t *getRegion( const int32_t iX, const int32_t iZ ) const;
	void	getSample( const int32_t iX, const int32_t iZ, uint32_t &nControl, real_t &fHeight ) const;
	bool	getMinimumRect( int32_t iX, int32_t iZ, int32_t nWidth, int32_t nHeight, real_t &fHeight ) const;
	Vector3 getMeshVertex( const int32_t		   nLod,
						   const t3dHeightFilter_t filter,
						   const int32_t		   iX,
						   const int32_t		   iZ,
						   uint32_t				  &nControl ) const;
} t3dBakeSnapshot_t;

struct t3dTrianglePairRequest_t
{
	bool					 m_bGenerateUvs = false;
	int32_t					 m_nLod = 0;
	t3dHeightFilter_t		 m_filter = HEIGHT_FILTER_NEAREST;
	bool					 m_bRequireNav = false;
	int32_t					 m_nX = 0;
	int32_t					 m_nZ = 0;
	const t3dBakeSnapshot_t *m_pSnapshot = NULL;
};

#endif // TERRAIN3D_BAKE_SOURCE_H
