// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_LIGHTMAP_STREAMER_H
#define TERRAIN3D_LIGHTMAP_STREAMER_H

#include "Terrain3DConstants.h"
#include <godot_cpp/classes/lightmap_gi.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "lighting/terrain_3d_region_lighting.h"
#include "data/terrain_3d_runtime_bake_codec.h"

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DTerrain;

// Streams external LightmapGIData packages with terrain-region residency.
// Region resources retain only package metadata; the heavy .lmbake payload and
// its lightmap/shadowmask textures are requested when the cluster is desired.
class T3DLightmapStreamer : public Object {
	GDCLASS( T3DLightmapStreamer, Object );
	CLASS_NAME();

	typedef enum t3dsourceLoadResult_e {
		SOURCE_LOAD_PENDING,
		SOURCE_LOAD_READY,
		SOURCE_LOAD_FAILED,
	} t3dsourceLoadResult_t;

	typedef struct t3dactivePackage_s {
		Ref<T3DRegionLighting> pPackage;
		Ref<LightmapGIData> pSourceData;
		Ref<LightmapGIData> pRuntimeData;
		LightmapGI *pNode = nullptr;
		uint64_t receiverSignature = 0u;
		int nReceiverCount = 0;
		bool bDesired = false;
	} t3dactivePackage_t;

	T3DTerrain *m_pTerrain = nullptr;
	t3dactivePackage_t m_szActive[TERRAIN3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT];
	uint8_t m_nActiveCount = 0u;
	bool m_bEnabled = true;
	int m_nRadiusRegions = 1;
	int m_nLoadBudget = 2;
	int m_nUnloadBudget = 4;
	int m_nResidencyBudgetMb = 16;
	Vector2i m_center = V2I_MAX;
	int m_nLoadsLastTick = 0;
	int m_nUnloadsLastTick = 0;
	int m_nRefreshesLastTick = 0;
	int m_nResourceRequestsLastTick = 0;
	int m_nResourceFailuresLastTick = 0;
	uint64_t m_nLoadsTotal = 0u;
	uint64_t m_nUnloadsTotal = 0u;
	uint64_t m_nRefreshesTotal = 0u;
	uint64_t m_nResourceRequestsTotal = 0u;
	uint64_t m_nResourceFailuresTotal = 0u;
	uint64_t m_nNodeRuntimeGeneration = 0u;
	bool m_bHasPendingActivation = false;
	int m_nPendingPackagesLastTick = 0;
	int m_nCapacityDeferredLastTick = 0;
	int m_nCompatibilityFailuresLastTick = 0;
	uint64_t m_nCompatibilityFailuresTotal = 0u;
	size_t m_nResidentEstimatedBytes = 0u;

	t3dactivePackage_t *findActive( const Ref<T3DRegionLighting> &rPackage );
	const t3dactivePackage_t *findActive( const Ref<T3DRegionLighting> &rPackage ) const;
	t3dsourceLoadResult_t requestSourceData(
		const Ref<T3DRegionLighting> &rPackage,
		Ref<LightmapGIData> &rData
	);
	bool collectReceiverState(
		const Ref<T3DRegionLighting> &rPackage, uint64_t &nSignature, int &nReceiverCount
	) const;
	bool buildRuntimeData(
		const Ref<T3DRegionLighting> &rPackage,
		const Ref<LightmapGIData> &rSource, Ref<LightmapGIData> &rData,
		uint64_t &nSignature, int &nReceiverCount
	) const;
	bool activate(
		const Ref<T3DRegionLighting> &rPackage, const Ref<LightmapGIData> &rSourceData,
		const Ref<LightmapGIData> &rRuntimeData, uint64_t nSignature, int nReceiverCount
	);
	bool refreshActive(
		t3dactivePackage_t &rActive, const Ref<LightmapGIData> &rRuntimeData,
		uint64_t nSignature, int nReceiverCount
	);
	bool isPackageCompatibleWithActive( const Ref<T3DRegionLighting> &rPackage ) const;
	void deactivate( size_t iIndex );
	void markDesired( const Vector2i &rCenter );
	int getEffectiveResidencyBudgetMb( void ) const;

public:
	T3DLightmapStreamer( void ) = default;
	~T3DLightmapStreamer( void );

	void initialize( T3DTerrain *pTerrain );
	void destroy( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void suspend( void );

	void setEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isEnabled( void ) const { return m_bEnabled; }
	void setRadiusRegions( int nRadius );
	EA_FORCE_INLINE int getRadiusRegions( void ) const { return m_nRadiusRegions; }
	EA_FORCE_INLINE void setLoadBudget( const int nBudget ) { m_nLoadBudget = CLAMP( nBudget, 0, 64 ); }
	EA_FORCE_INLINE int getLoadBudget( void ) const { return m_nLoadBudget; }
	EA_FORCE_INLINE void setUnloadBudget( const int nBudget ) { m_nUnloadBudget = CLAMP( nBudget, 0, 64 ); }
	EA_FORCE_INLINE int getUnloadBudget( void ) const { return m_nUnloadBudget; }
	void setResidencyBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getResidencyBudgetMb( void ) const { return m_nResidencyBudgetMb; }
	EA_FORCE_INLINE int getEffectiveResidencyBudgetMbForDiagnostics( void ) const { return getEffectiveResidencyBudgetMb(); }
	EA_FORCE_INLINE size_t getResidentEstimatedBytes( void ) const { return m_nResidentEstimatedBytes; }
	EA_FORCE_INLINE bool isResidencyIdle( void ) const { return m_nLoadsLastTick == 0 && m_nRefreshesLastTick == 0; }
	Dictionary getStatsFast( void ) const;

	EA_FORCE_INLINE int getActivePackageCount( void ) const { return ( int ) m_nActiveCount; }
	Dictionary getStats( void ) const;

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_LIGHTMAP_STREAMER_H
