// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_REGION_LIGHTING_H
#define TERRAIN3D_REGION_LIGHTING_H

#include <godot_cpp/classes/lightmap_gi_data.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include "Terrain3DConstants.h"

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
	Vector2i m_clusterLocation = V2I_ZERO;
	Vector2i m_regionMin = V2I_ZERO;
	Vector2i m_regionMax = V2I_ZERO;
	int m_nClusterSizeRegions = 1;
	int m_nTerrainProxyLod = 2;
	int m_nContextRadiusRegions = 1;
	int m_nShadowmaskMode = LightmapGIData::SHADOWMASK_MODE_OVERLAY;
	int64_t m_bakeSignature = 0;
	String m_directionalLightSignature;
	int64_t m_bakedUnix = 0;
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
	String getLightDataPath( void ) const { return m_lightDataPath; }
	bool hasLightData( void ) const { return !m_lightDataPath.is_empty(); }
	void setTerrainShadowMaskPath( const String &rPath );
	String getTerrainShadowMaskPath( void ) const { return m_terrainShadowMaskPath; }
	bool hasTerrainShadowMask( void ) const { return !m_terrainShadowMaskPath.is_empty(); }

	void setBakedUserData( const Array &rData );
	Array getBakedUserData( void ) const { return m_bakedUserData.duplicate( true ); }
	const Array &getBakedUserDataInternal( void ) const { return m_bakedUserData; }

	void setClusterLocation( const Vector2i &rLocation );
	Vector2i getClusterLocation( void ) const { return m_clusterLocation; }
	void setRegionMin( const Vector2i &rLocation );
	Vector2i getRegionMin( void ) const { return m_regionMin; }
	void setRegionMax( const Vector2i &rLocation );
	Vector2i getRegionMax( void ) const { return m_regionMax; }
	void setClusterSizeRegions( int nSize );
	int getClusterSizeRegions( void ) const { return m_nClusterSizeRegions; }
	void setTerrainProxyLod( int nLod );
	int getTerrainProxyLod( void ) const { return m_nTerrainProxyLod; }
	void setContextRadiusRegions( int nRadius );
	int getContextRadiusRegions( void ) const { return m_nContextRadiusRegions; }
	void setShadowmaskMode( int nMode );
	int getShadowmaskMode( void ) const { return m_nShadowmaskMode; }
	void setBakeSignature( int64_t nSignature );
	int64_t getBakeSignature( void ) const { return m_bakeSignature; }
	void setDirectionalLightSignature( const String &rSignature );
	String getDirectionalLightSignature( void ) const { return m_directionalLightSignature; }
	void setBakedUnix( int64_t nUnix );
	int64_t getBakedUnix( void ) const { return m_bakedUnix; }
	void setReceiverCount( int nCount );
	int getReceiverCount( void ) const { return m_nReceiverCount; }
	void setContributorCount( int nCount );
	int getContributorCount( void ) const { return m_nContributorCount; }
	void setLightmapTextureCount( int nCount );
	int getLightmapTextureCount( void ) const { return m_nLightmapTextureCount; }
	void setShadowmaskTextureCount( int nCount );
	int getShadowmaskTextureCount( void ) const { return m_nShadowmaskTextureCount; }
	void setHasProbeData( bool bHasProbes );
	bool hasProbeData( void ) const { return m_bHasProbeData; }
	void setDiskBytes( int64_t nBytes );
	int64_t getDiskBytes( void ) const { return m_nDiskBytes; }
	void setEstimatedGpuBytes( int64_t nBytes );
	int64_t getEstimatedGpuBytes( void ) const { return m_nEstimatedGpuBytes; }
	void setSourceScene( const String &rPath );
	String getSourceScene( void ) const { return m_sourceScene; }

	bool hasShadowmask( void ) const { return m_nShadowmaskTextureCount > 0; }
	int stripBakedUsersWithPathFragment( const String &rFragment );

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_REGION_LIGHTING_H
