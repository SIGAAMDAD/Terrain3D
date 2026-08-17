// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGION_CLASS_H
#define TERRAIN3D_REGION_CLASS_H

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

#include "eastl_config.h"
#include <EASTL/vector.h>

#include "Terrain3DConstants.h"
#include "terrain_3d_util.h"
#include "terrain_3d_region_codec.h"
#include "terrain_3d_instance_store.h"
#include "terrain_3d_assets.h"
#include "terrain_3d_region_lighting.h"

class T3DRegion : public Resource {
	using Terrain3DRegion = T3DRegion;
	GDCLASS( Terrain3DRegion, Resource );
	friend class T3DData;
	CLASS_NAME();

public: // Constants
	typedef enum t3dmapType_e {
		TYPE_HEIGHT,
		TYPE_CONTROL,
		TYPE_COLOR,
		TYPE_MAX,
	} t3dmapType_t;

	typedef enum t3dchannelFlag_e : uint8_t {
		CHANNEL_CONTROL = 1u << 0,
		CHANNEL_COLOR = 1u << 1,
		CHANNEL_DERIVED = 1u << 2,
		CHANNEL_MASK = CHANNEL_CONTROL | CHANNEL_COLOR | CHANNEL_DERIVED,
	} t3dchannelFlag_t;

	typedef enum t3dstreamedNodeBoundaryPolicy_e : int {
		STREAMED_NODE_BOUNDARY_ANCHOR = 0,
		STREAMED_NODE_BOUNDARY_COVERAGE = 1,
	} t3dstreamedNodeBoundaryPolicy_t;

	typedef enum t3denvironmentLightingClass_e : int {
		LIGHTING_CLASS_TERRAIN = 0,
		LIGHTING_CLASS_FOLIAGE_CLUTTER = 1,
		LIGHTING_CLASS_MEDIUM_OBSTACLE = 2,
		LIGHTING_CLASS_LARGE_OCCLUDER = 3,
		LIGHTING_CLASS_REGIONAL_OCCLUDER = 4,
	} t3denvironmentLightingClass_t;

	typedef enum t3dbakedLightingMode_e : int {
		BAKED_LIGHTING_DISABLED = 0,
		BAKED_LIGHTING_LIGHTMAP = 1,
		BAKED_LIGHTING_PROBE = 2,
		BAKED_LIGHTING_MATERIAL_ONLY = 3,
	} t3dbakedLightingMode_t;

	// Runtime representation of the serialized streamed-node descriptor. The
	// persisted Array<Dictionary> remains the editor/resource ABI, while the
	// streamer consumes this typed cache so activation does not repeatedly hash
	// Dictionary keys or convert Variants in hot traversal paths.
	typedef struct t3dstreamedNodeRuntimeDescriptor_s {
		int64_t id = 0;
		Ref<PackedScene> pScene;
		String scenePath;
		Transform3D transform;
		Dictionary state;
		int flags = 0;
		int boundaryPolicy = STREAMED_NODE_BOUNDARY_ANCHOR;
		Vector2i coverageMin = V2I_ZERO;
		Vector2i coverageMax = V2I_ZERO;
		int lightingClass = LIGHTING_CLASS_MEDIUM_OBSTACLE;
		int bakedLightingMode = BAKED_LIGHTING_LIGHTMAP;
		bool bContributeBakedGi = true;
		bool bReceiveDynamicDirectional = true;
		bool bDynamicShadowEligible = true;
		Ref<PackedScene> pShadowProxyScene;
		String shadowProxyScenePath;
	} t3dstreamedNodeRuntimeDescriptor_t;

private:
	// Import without persistence side effects. This is intentionally not bound to
	// ClassDB; only trusted loading/reconstruction code may bypass mutation flags.
	void importInstancesUnmodified( const Dictionary &rInstances );
	void importRuntimeMapsUnmodified( const T3DRegion *pSource );
	void releaseRuntimeMapsUnmodified( void );
	void markRuntimeMapsClean( void );
	bool hasRuntimeMapPayload( void ) const;
	bool runtimeMapsEvictable( void ) const;
	bool encodeInstancePage( void ) const;
	void clearInstanceMetadata( void ) const;
	void refreshStreamedNodeId( void );
	void rebuildStreamedNodeRuntime( void );
	void refreshStreamedNodeCoverage( void );
	bool streamedNodeIdExists( uint64_t nId ) const;
	int64_t allocateStreamedNodeId( int64_t nRequestedId );
	Dictionary getInstancesLegacyStorage( void ) const;

	// Saved data
	real_t m_fVersion = 0.8f; // Set to first version to ensure we always upgrades this
	int m_nRegionSize = 0;
	Vector2 m_heightRange = V2_ZERO;
	// Maps
	Ref<Image> m_pHeightMap;
	Ref<Image> m_pControlMap;
	Ref<Image> m_pColorMap;
	// Runtime-only normal cache regenerated from the height map. RG stores
	// encoded normal X/Z; normal Y and slope are reconstructed by consumers. It
	// is excluded from serialization because height remains authoritative.
	Ref<Image> m_pDerivedNormalSlopeMap;
	bool m_bDerivedNormalSlopeValid = false;
	// Persistent channel metadata survives bounded CPU eviction. Runtime code must
	// never infer world-wide channel presence from currently resident Image refs.
	uint8_t m_nChannelFlags = 0u;
	bool m_bRuntimeMapsModified = false;
	// Instancer. Expanded native stores are pageable; the compact binary page remains
	// serialized with the region and is sufficient for dormant regions.
	mutable T3DInstanceStore m_instanceStore;
	mutable PackedByteArray m_instancePage;
	mutable bool m_bInstanceStoreResident = true;
	mutable bool m_bInstanceStoreModified = false;
	mutable uint32_t m_szInstanceMeshCounts[TERRAIN3D_MAX_MESH_ASSETS] = {};
	mutable uint32_t m_nInstanceCount = 0u;
	mutable uint16_t m_nInstanceMeshCount = 0u;

	// Dynamic nodes remain as compact spawn/state descriptors while their owning
	// region is dormant. Terrain3DNodeStreamer is the only runtime publisher.
	Array m_streamedNodes;
	eastl::vector<t3dstreamedNodeRuntimeDescriptor_t> m_streamedNodeRuntime;
	uint64_t m_nNextStreamedNodeId = 1u;
	Vector2i m_streamedNodeCoverageMin = V2I_ZERO;
	Vector2i m_streamedNodeCoverageMax = V2I_ZERO;
	bool m_bStreamedNodeHasExtendedCoverage = false;
	Ref<T3DRegionLighting> m_pLighting;
	real_t m_fVertexSpacing = 1.f; // Spacing that instancer transforms are currently scaled by.

	// Working data not saved to disk
	bool m_bDeleted = false;	// Marked for deletion on save
	bool m_bEdited = false;	// Marked for undo/redo storage
	bool m_bModified = false; // Marked for saving
	Vector2i m_location = V2I_MAX;

public:
	T3DRegion( void );
	~T3DRegion( void );

	void clear( void );
	void setVersion( const real_t fVersion );
	real_t getVersion( void ) const {
		return m_fVersion;
	}
	void setRegionSize( const int nRegionSize );
	int getRegionSize( void ) const {
		return m_nRegionSize;
	}

	// Maps
	void setMap( const t3dmapType_t mapType, const Ref<Image> &rImage );
	Ref<Image> getMap( const t3dmapType_t mapType ) const;
	Image *getMapPtr( const t3dmapType_t mapType ) const;
	void setMaps( const TypedArray<Image> &rMaps );
	TypedArray<Image> getMaps( void ) const;
	void setHeightMap( const Ref<Image> &rMap );
	Ref<Image> getHeightMap( void ) const {
		return m_pHeightMap;
	}
	void setControlMap( const Ref<Image> &rMap );
	Ref<Image> getControlMap( void ) const {
		return m_pControlMap;
	}
	void setColorMap( const Ref<Image> &rMap );
	Ref<Image> getColorMap( void ) const {
		return m_pColorMap;
	}
	Image *getMapPtrForWrite( const t3dmapType_t mapType );
	Ref<Image> materializeMap( const t3dmapType_t mapType );
	bool hasMap( const t3dmapType_t mapType ) const {
		return getMap( mapType ).is_valid();
	}
	void setChannelFlags( const int nFlags );
	int getChannelFlags( void ) const {
		return ( int ) ( m_nChannelFlags );
	}
	bool hasChannel( const t3dmapType_t mapType ) const {
		if ( mapType == TYPE_HEIGHT ) {
			return true;
		}
		if ( mapType == TYPE_CONTROL ) {
			return ( m_nChannelFlags & CHANNEL_CONTROL ) != 0u;
		}
		if ( mapType == TYPE_COLOR ) {
			return ( m_nChannelFlags & CHANNEL_COLOR ) != 0u;
		}
		return false;
	}
	bool hasDerivedChannel( void ) const {
		return ( m_nChannelFlags & CHANNEL_DERIVED ) != 0u;
	}
	Ref<Image> getDerivedNormalSlopeMap( void ) const {
		return m_pDerivedNormalSlopeMap;
	}
	bool hasValidDerivedNormalSlopeMap( void ) const {
		return m_bDerivedNormalSlopeValid && m_pDerivedNormalSlopeMap.is_valid();
	}
	void setDerivedNormalSlopeMapInternal( const Ref<Image> &rMap );
	void invalidateDerivedNormalSlopeMap( void ) {
		m_bDerivedNormalSlopeValid = false;
	}
	void sanitizeMaps( void );
	bool isMapDefault( const t3dmapType_t mapType ) const;
	bool compactMapIfDefault( const t3dmapType_t mapType, const bool bMarkModified = true );
	void compactDefaultMaps( void );
	// Commit direct pixel edits performed through a mutable Image reference.
	// Callers that mutate a map in place must invoke this once after the edit.
	void markMapEdited( const t3dmapType_t mapType, const bool bRecalculateHeightRange = true );
	Ref<Image> sanitizeMap( const t3dmapType_t mapType, const Ref<Image> &rMap ) const;
	bool validateMapSize( const Ref<Image> &rMap ) const;

	void setHeightRange( const Vector2 &rRange );
	Vector2 getHeightRange( void ) const {
		return m_heightRange;
	}
	void updateHeight( const real_t fHeight );
	void updateHeights( const Vector2 &rLowHigh );
	void calcHeightRange( void );

	// Instancer
	// Public replacement API. Unlike resource deserialization, changing this
	// data is an edit and marks the region for saving.
	void setInstances( const Dictionary &rInstances );
	Dictionary getInstances( void ) const;
	void setInstancePage( const PackedByteArray &rPage );
	PackedByteArray getInstancePage( void ) const;
	bool ensureInstanceStoreResident( void ) const;
	bool evictInstanceStore( void );
	bool isInstanceStoreResident( void ) const {
		return m_bInstanceStoreResident;
	}
	size_t getInstanceResidentBytes( void ) const {
		return m_bInstanceStoreResident ? m_instanceStore.residentBytes() : 0u;
	}
	size_t getInstancePageBytes( void ) const {
		return ( size_t ) ( m_instancePage.size() );
	}
	uint32_t getInstanceCount( const int nMeshId = -1 ) const {
		if ( m_bInstanceStoreResident ) {
			const size_t count = m_instanceStore.instanceCount( nMeshId );
			return count > ( size_t ) ( UINT32_MAX ) ? UINT32_MAX : ( uint32_t ) ( count );
		}
		return nMeshId >= 0 && nMeshId < TERRAIN3D_MAX_MESH_ASSETS ?
			m_szInstanceMeshCounts[nMeshId] : m_nInstanceCount;
	}
	uint16_t getInstanceMeshCount( void ) const {
		if ( m_bInstanceStoreResident ) {
			return m_instanceStore.meshCount() > ( size_t ) ( UINT16_MAX ) ? UINT16_MAX : ( uint16_t ) ( m_instanceStore.meshCount() );
		}
		return m_nInstanceMeshCount;
	}
	const T3DInstanceStore &getInstanceStore( void ) const {
		ensureInstanceStoreResident();
		return m_instanceStore;
	}
	T3DInstanceStore &getInstanceStoreMut( void ) {
		ensureInstanceStoreResident();
		m_bModified = true;
		m_bInstanceStoreModified = true;
		return m_instanceStore;
	}
	T3DInstanceStore &getInstanceStoreInternal( void ) {
		ensureInstanceStoreResident();
		return m_instanceStore;
	}
	bool hasInstances( void ) const {
		return m_bInstanceStoreResident ? !m_instanceStore.empty() : m_nInstanceCount > 0u;
	}

	// Dynamic streamed-node descriptors. Each entry stores id, PackedScene,
	// region-local transform, state Dictionary, and flags.
	void setStreamedNodes( const Array &rNodes );
	Array getStreamedNodes( void ) const {
		return m_streamedNodes.duplicate( true );
	}
	const Array &getStreamedNodesInternal( void ) const {
		return m_streamedNodes;
	}
	const eastl::vector<t3dstreamedNodeRuntimeDescriptor_t> &getStreamedNodesRuntimeInternal( void ) const {
		return m_streamedNodeRuntime;
	}
	const t3dstreamedNodeRuntimeDescriptor_t *findStreamedNodeRuntimeInternal( const int64_t nId ) const;
	int64_t addStreamedNode(
		const Ref<PackedScene> &rScene, const Transform3D &rLocalTransform,
		const Dictionary &rState = Dictionary(), const int nFlags = 0, const int64_t nId = 0,
		const int nBoundaryPolicy = STREAMED_NODE_BOUNDARY_ANCHOR,
		const Vector2i &rCoverageMin = V2I_ZERO, const Vector2i &rCoverageMax = V2I_ZERO
	);
	int64_t addStreamedNodePath(
		const String &rScenePath, const Transform3D &rLocalTransform,
		const Dictionary &rState = Dictionary(), const int nFlags = 0, const int64_t nId = 0,
		const int nBoundaryPolicy = STREAMED_NODE_BOUNDARY_ANCHOR,
		const Vector2i &rCoverageMin = V2I_ZERO, const Vector2i &rCoverageMax = V2I_ZERO
	);
	bool removeStreamedNode( const int64_t nId );
	bool updateStreamedNodeState(
		const int64_t nId, const Transform3D &rLocalTransform,
		const Dictionary &rState, const int nFlags
	);
	int getStreamedNodeCount( void ) const {
		return ( int ) ( m_streamedNodeRuntime.size() );
	}
	bool streamedNodeCoverageIntersects(
		const Vector2i &rOwnerLocation,
		const Vector2i &rTargetMin, const Vector2i &rTargetMax
	) const;
	bool hasExtendedStreamedNodeCoverage( void ) const {
		return m_bStreamedNodeHasExtendedCoverage;
	}

	void setLighting( const Ref<T3DRegionLighting> &rLighting );
	Ref<T3DRegionLighting> getLighting( void ) const { return m_pLighting; }
	bool hasLighting( void ) const { return m_pLighting.is_valid() && m_pLighting->hasLightData(); }
	void setVertexSpacing( const real_t fVertexSpacing ) {
		m_fVertexSpacing = CLAMP( fVertexSpacing, 0.25f, 100.f );
	}
	real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}

	// Working Data
	void setDeleted( const bool bDeleted ) {
		m_bDeleted = bDeleted;
	}
	bool isDeleted( void ) const {
		return m_bDeleted;
	}
	void setEdited( const bool bEdited ) {
		m_bEdited = bEdited;
	}
	bool isEdited( void ) const {
		return m_bEdited;
	}
	void setModified( const bool bModified ) {
		m_bModified = bModified;
		if ( !bModified ) {
			m_bRuntimeMapsModified = false;
		}
	}
	bool isModified( void ) const {
		return m_bModified;
	}
	void setLocation( const Vector2i &rLocation );
	Vector2i getLocation( void ) const {
		return m_location;
	}

	// File I/O
	Error save( const String &rPath = "", const bool b16Bit = false );

	// Utility
	// Detached-resource helper used by Terrain3DData's replacement transaction.
	// Deliberately not bound to scripts because attached regions require repository,
	// renderer, collision, and persistence reconciliation.
	void setData( const Dictionary &rData );
	Dictionary getData( void ) const;
	Ref<T3DRegion> duplicate( const bool bDeep = false );
	void dump( const bool bVerbose = false ) const;

protected:
	static void _bind_methods( void );
};

using t3dmapType_t = T3DRegion::t3dmapType_t;
VARIANT_ENUM_CAST( T3DRegion::t3dmapType_t );
VARIANT_ENUM_CAST( T3DRegion::t3dchannelFlag_t );
VARIANT_ENUM_CAST( T3DRegion::t3dstreamedNodeBoundaryPolicy_t );
VARIANT_ENUM_CAST( T3DRegion::t3denvironmentLightingClass_t );
VARIANT_ENUM_CAST( T3DRegion::t3dbakedLightingMode_t );

using Terrain3DRegion = T3DRegion;

#endif // TERRAIN3D_REGION_CLASS_H
