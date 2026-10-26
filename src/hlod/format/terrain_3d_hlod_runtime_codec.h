// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_HLOD_RUNTIME_CODEC_H
#define TERRAIN3D_HLOD_RUNTIME_CODEC_H

#include "Terrain3DTypeSupport.h"
#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "eastl_config.h"
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/variant.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

typedef struct t3dHLODEntryDescriptor_s
{
	int						 m_nLevel = 1;
	t3dRegionCoord_t		 m_cluster = t3dPackRegionCoord( s_v2iZero );
	t3dRegionCoord_t		 m_regionMin = t3dPackRegionCoord( s_v2iZero );
	t3dRegionCoord_t		 m_regionMax = t3dPackRegionCoord( s_v2iZero );
	EA_FORCE_INLINE Vector2i getCluster(
		void ) const {
		return t3dUnpackRegionCoord( m_cluster );
	}
	EA_FORCE_INLINE Vector2i getRegionMin(
		void ) const {
		return t3dUnpackRegionCoord( m_regionMin );
	}
	EA_FORCE_INLINE Vector2i getRegionMax(
		void ) const {
		return t3dUnpackRegionCoord( m_regionMax );
	}
	EA_FORCE_INLINE void setCluster(
		const Vector2i &rValue ) {
		m_cluster = t3dPackRegionCoord( rValue );
	}
	EA_FORCE_INLINE void setRegionMin(
		const Vector2i &rValue ) {
		m_regionMin = t3dPackRegionCoord( rValue );
	}
	EA_FORCE_INLINE void setRegionMax(
		const Vector2i &rValue ) {
		m_regionMax = t3dPackRegionCoord( rValue );
	}
	AABB	m_localBounds;
	String	m_packagePath;
	real_t	m_fGeometricError = 1.0f;
	int64_t m_nSourceSignature = 0;
	int64_t m_nBuildSignature = 0;
	int64_t m_nEstimatedGpuBytes = 0;
	int		m_nParent = -1;
	int		m_szChildren[4] = { -1, -1, -1, -1 };
} t3dHLODEntryDescriptor_t;

typedef struct t3dHLODRuntimeData_s
{
	eastl::vector< t3dHLODEntryDescriptor_t, Terrain3DEastlAllocator >		   m_entries;
	eastl::vector< int, Terrain3DEastlAllocator >							   m_roots;
	eastl::vector< uint8_t, Terrain3DEastlAllocator >						   m_instancerMinLevels;
	eastl::vector< uint8_t, Terrain3DEastlAllocator > m_instancerSubcellMasks;
	eastl::vector< t3dHLODRuntimeNodePolicyRecord_t, Terrain3DEastlAllocator > m_nodePolicies;
	uint32_t m_szNodePolicyOffsets[T3D_REGION_SLOTS + 1]{};
} t3dHLODRuntimeData_t;

class Terrain3DHLODRuntimeCodec
{
public:
	static bool			   decodeEntryDictionary( const Variant			   &rValue,
												  t3dHLODEntryDescriptor_t &rEntry,
												  bool						bRequireNonEmptyPackagePath = true );
	static PackedByteArray encode( const Array			  &rEntries,
								   const PackedByteArray  &rInstancerMinLevels,
								   const PackedInt32Array &rNodeRegionIndices,
								   const PackedInt64Array &rNodeSpawnIds,
								   const PackedByteArray  &rNodeMinLevels,
                                   const PackedByteArray  &rInstancerSubcellMasks,
                                   const PackedByteArray  &rNodeSubcellMasks );
	static bool			   decode( const PackedByteArray &rBlob, t3dHLODRuntimeData_t &rData );
};

#endif // TERRAIN3D_HLOD_RUNTIME_CODEC_H
