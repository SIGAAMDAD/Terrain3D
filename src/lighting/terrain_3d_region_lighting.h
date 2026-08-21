// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_REGION_LIGHTING_H
#define TERRAIN3D_REGION_LIGHTING_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/lightmap_gi_data.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

class T3DRegionLighting : public Resource {
	GDCLASS( T3DRegionLighting, Resource );
	CLASS_NAME();

private:
	// Keep the heavy LightmapGIData external. Holding a serialized Ref here would
	// eagerly retain its atlas and shadowmask textures whenever terrain regions
	// are loaded, defeating the residency streamer.
	String m_lightDataPath;
	String m_terrainShadowMaskPath;
	Array m_bakedUserData;
	Vector2i m_clusterLocation = s_v2iZero;
	Vector2i m_regionMin = s_v2iZero;
	Vector2i m_regionMax = s_v2iZero;
	int m_nClusterSizeRegions = 1;
	int m_nTerrainProxyLod = 2;
	int m_nContextRadiusRegions = 1;
	int m_nShadowmaskMode = LightmapGIData::SHADOWMASK_MODE_OVERLAY;
	int64_t m_nBakeSignature = 0;
	String m_directionalLightSignature;
	int64_t m_nBakedUnix = 0;
	int m_nReceiverCount = 0;
	int m_nContributorCount = 0;
	int m_nLightmapTextureCount = 0;
	int m_nShadowmaskTextureCount = 0;
	bool m_bHasProbeData = false;
	int64_t m_nDiskBytes = 0;
	int64_t m_nEstimatedGpuBytes = 0;
	String m_sourceScene;

public:
	T3DRegionLighting( void ) = default;
	~T3DRegionLighting( void ) = default;

	void setLightDataPath( const String &rPath );
	EA_FORCE_INLINE String getLightDataPath( void ) const {
		return m_lightDataPath;
	}
	EA_FORCE_INLINE bool hasLightData( void ) const {
		return !m_lightDataPath.is_empty();
	}
	void setTerrainShadowMaskPath( const String &rPath );
	EA_FORCE_INLINE String getTerrainShadowMaskPath( void ) const {
		return m_terrainShadowMaskPath;
	}
	EA_FORCE_INLINE bool hasTerrainShadowMask( void ) const {
		return !m_terrainShadowMaskPath.is_empty();
	}

	void setBakedUserData( const Array &rData );
	EA_FORCE_INLINE Array getBakedUserData( void ) const {
		return m_bakedUserData.duplicate( true );
	}
	EA_FORCE_INLINE const Array &getBakedUserDataInternal( void ) const {
		return m_bakedUserData;
	}

	void setClusterLocation( const Vector2i &rLocation );
	EA_FORCE_INLINE Vector2i getClusterLocation( void ) const {
		return m_clusterLocation;
	}
	void setRegionMin( const Vector2i &rLocation );
	EA_FORCE_INLINE Vector2i getRegionMin( void ) const {
		return m_regionMin;
	}
	void setRegionMax( const Vector2i &rLocation );
	EA_FORCE_INLINE Vector2i getRegionMax( void ) const {
		return m_regionMax;
	}
	void setClusterSizeRegions( int nSize );
	EA_FORCE_INLINE int getClusterSizeRegions( void ) const {
		return m_nClusterSizeRegions;
	}
	void setTerrainProxyLod( int nLod );
	EA_FORCE_INLINE int getTerrainProxyLod( void ) const {
		return m_nTerrainProxyLod;
	}
	void setContextRadiusRegions( int nRadius );
	EA_FORCE_INLINE int getContextRadiusRegions( void ) const {
		return m_nContextRadiusRegions;
	}
	void setShadowmaskMode( int nMode );
	EA_FORCE_INLINE int getShadowmaskMode( void ) const {
		return m_nShadowmaskMode;
	}
	void setBakeSignature( int64_t nSignature );
	EA_FORCE_INLINE int64_t getBakeSignature( void ) const {
		return m_nBakeSignature;
	}
	void setDirectionalLightSignature( const String &rSignature );
	EA_FORCE_INLINE String getDirectionalLightSignature( void ) const {
		return m_directionalLightSignature;
	}
	void setBakedUnix( int64_t nUnix );
	EA_FORCE_INLINE int64_t getBakedUnix( void ) const {
		return m_nBakedUnix;
	}
	void setReceiverCount( int nCount );
	EA_FORCE_INLINE int getReceiverCount( void ) const {
		return m_nReceiverCount;
	}
	void setContributorCount( int nCount );
	EA_FORCE_INLINE int getContributorCount( void ) const {
		return m_nContributorCount;
	}
	void setLightmapTextureCount( int nCount );
	EA_FORCE_INLINE int getLightmapTextureCount( void ) const {
		return m_nLightmapTextureCount;
	}
	void setShadowmaskTextureCount( int nCount );
	EA_FORCE_INLINE int getShadowmaskTextureCount( void ) const {
		return m_nShadowmaskTextureCount;
	}
	void setHasProbeData( bool bHasProbes );
	EA_FORCE_INLINE bool hasProbeData( void ) const {
		return m_bHasProbeData;
	}
	void setDiskBytes( int64_t nBytes );
	EA_FORCE_INLINE int64_t getDiskBytes( void ) const {
		return m_nDiskBytes;
	}
	void setEstimatedGpuBytes( int64_t nBytes );
	EA_FORCE_INLINE int64_t getEstimatedGpuBytes( void ) const {
		return m_nEstimatedGpuBytes;
	}
	void setSourceScene( const String &rPath );
	EA_FORCE_INLINE String getSourceScene( void ) const {
		return m_sourceScene;
	}

	EA_FORCE_INLINE bool hasShadowmask( void ) const {
		return m_nShadowmaskTextureCount > 0;
	}
	int stripBakedUsersWithPathFragment( const String &rFragment );

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_REGION_LIGHTING_H
