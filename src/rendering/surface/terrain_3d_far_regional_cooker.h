// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_FAR_REGIONAL_COOKER_H
#define TERRAIN3D_FAR_REGIONAL_COOKER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "data/model/terrain_3d_region.h"
#include "eastl_config.h"

#include <EASTL/bitset.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include "assets/catalog/terrain_3d_assets.h"
#include "data/persistence/terrain_3d_region_repository.h"
#include "data/sampling/terrain_3d_sampling_storage.h"
#include "rendering/material/terrain_3d_material.h"
#include "streaming/cache/terrain_3d_cpu_region_cache.h"

class Terrain3DData;

typedef struct t3dFarRegionalCookConfig_s
{
	int						 m_nHeightSourceSide = T3D_FAR_VT_MAX_SOURCE_HEIGHT_SIDE;
	int						 m_nSurfaceSourceSide = T3D_FAR_VT_MAX_SOURCE_HEIGHT_SIDE;
	String					 m_dataDirectory;
	Ref< Image >			 m_defaultHeightMap;
	Ref< Image >			 m_defaultControlMap;
	Ref< Image >			 m_defaultColorMap;
	const Terrain3DData		*m_pData = NULL;
	Ref< Terrain3DMaterial > m_material;
	Ref< Terrain3DAssets >	 m_assets;
} t3dFarRegionalCookConfig_t;

// Saved-source fingerprints are independent of V1 GPU residency.
// Revisions are local dirty notifications; only content hashes are serialized
// Authoritative decoded regions and source files supply deterministic hashes.
typedef struct t3dFarRegionSourceFingerprint_s
{
	String m_path;
	uint64_t m_nHash = 0u;
	uint32_t m_nRevision = 0u;
	bool m_bValid = false;
	bool m_bFileSource = false;
} t3dFarRegionSourceFingerprint_t;

// Source-only regional FAR V1 cooker. GPU residency, page tables, budgets and
// publication belong exclusively to Terrain3DVirtualTexture.
class Terrain3DFarRegionalCooker
{
public:
    Terrain3DFarRegionalCooker( Terrain3DRegionRepository *pRepository,
        Terrain3DSamplingStorage *pSamplingStorage, Terrain3DCpuRegionCache *pCpuRegionCache );
    void setConfig( const t3dFarRegionalCookConfig_t &rConfig );
    void reset( void );
    void invalidateSurface( void );
    void invalidateLayout( void );
    void markAllDirty( t3dMapType_t eMapType );
    void markRegionDirty( const Vector2i &vLocation, t3dMapType_t eMapType );
    void noteRegionSaved( const Vector2i &vLocation, const String &rPath, bool bDeleted );
    bool writeRegionalAvtCooked( const String &rDirectory = String() );
    uint64_t getRegionalAvtPublicationRevision() const { return m_nAvtPublicationRevision; }
    bool isRegionalAvtCookDirty( const Vector2i &vLocation ) const;
    uint32_t getRegionalAvtCookedPackageGeneration( const Vector2i &vLocation ) const;
    bool isRegionHeightDirty( const Vector2i &vLocation ) const;
    bool isRegionColorDirty( const Vector2i &vLocation ) const;
    int getDirtyRegionCount( void ) const;
    t3dFarRegionalCookStats_t getStats( void ) const;
    Dictionary getStatsDictionary( void ) const;

private:
    Terrain3DRegionRepository *m_pRepository = NULL;
    Terrain3DSamplingStorage *m_pSamplingStorage = NULL;
    Terrain3DCpuRegionCache *m_pCpuRegionCache = NULL;
    t3dFarRegionalCookConfig_t m_config;
    eastl::bitset< T3D_REGION_SLOTS > m_avtCookDirtyRegions;
    uint32_t m_nAvtCookedPackageGenerations[T3D_REGION_SLOTS]{};
    bool m_bAvtCookDirtyAll = true;
    uint64_t m_nAvtPublicationRevision = 1u;
    t3dFarRegionSourceFingerprint_t m_sourceFingerprints[T3D_REGION_SLOTS]{};
    uint64_t m_nSourceRevision = 0u;
    uint64_t m_nSourceFileHashBytesTotal = 0u;
    uint32_t m_nSourceFileRehashCount = 0u;
    uint32_t m_nAvtLastCookWrittenRegions = 0u;
    uint32_t m_nAvtLastCookReusedRegions = 0u;
    uint64_t m_nAvtLastCookWrittenBytes = 0u;

    void invalidateSourceFingerprints( void );
    bool updateSourceFingerprint( int nIndex, const Terrain3DRegion *pRegion,
        const String &rSavedPath = String() );
    void appendGlobalBakeSignature( uint64_t &rHash ) const;
    Ref< Image > getRegionMapOrDefault( const Terrain3DRegion *pRegion, t3dMapType_t eMapType ) const;
    bool patchHeightRegionAt( const Vector2i &vLocation, PackedByteArray &rBytes,
        int nDestinationSide, int nDestinationX, int nDestinationY );
    bool patchColorRegionAt( const Vector2i &vLocation, PackedByteArray &rBytes,
        int nDestinationSide, int nDestinationX, int nDestinationY );
    bool writeOneRegionalAvtCold( const Vector2i &vLocation, const String &rPath,
        uint64_t nMaterialSignature, bool &rWritten );
};

#endif // TERRAIN3D_FAR_REGIONAL_COOKER_H
