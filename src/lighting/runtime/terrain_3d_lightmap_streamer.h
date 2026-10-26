// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_LIGHTMAP_STREAMER_H
#define TERRAIN3D_LIGHTMAP_STREAMER_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "lighting/runtime/terrain_3d_region_lighting.h"

#include <godot_cpp/classes/lightmap_gi.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

// Streams external LightmapGIData packages with terrain-region residency.
// Region resources retain only package metadata; the heavy .lmbake payload and
// its lightmap/shadowmask textures are requested when the cluster is desired.

typedef struct t3dActivePackage_s
{
	Ref< Terrain3DRegionLighting >	 m_ackage;
	Ref< LightmapGIData >			 m_sourceData;
	Ref< LightmapGIData >			 m_runtimeData;
	LightmapGI						*m_pNode = NULL;
	uint64_t						 m_nReceiverSignature = 0u;
	int								 m_nReceiverCount = 0;
	bool							 m_bDesired = false;
	Terrain3DSharedMemoryReservation m_memory;
} t3dActivePackage_t;

class Terrain3DLightmapStreamer : public Object
{
	GDCLASS( Terrain3DLightmapStreamer, Object );
	CLASS_NAME();

	Terrain3DOwner																		*m_pTerrain = NULL;
	eastl::fixed_vector< t3dActivePackage_t, T3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT, false > m_activePackages;
	struct PendingPackage
	{
		Ref< Terrain3DRegionLighting >	 package;
		Ref< LightmapGIData >			 data;
		Terrain3DSharedMemoryReservation memory;
	};
	eastl::fixed_vector< PendingPackage, T3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT, false > m_pendingPackages;
	void																			 drainPendingLoads();
	eastl::fixed_vector< LightmapGI *, T3D_LIGHTMAP_ACTIVE_INSTANCE_LIMIT, false >	 m_freeNodes;
	bool																			 m_bEnabled = true;
	int																				 m_nRadiusRegions = 1;
	int																				 m_nLoadBudget = 2;
	int																				 m_nUnloadBudget = 4;
	int																				 m_nResidencyBudgetMb = 16;
	Vector2i																		 m_center = s_v2iMax;
	int																				 m_nLoadsLastTick = 0;
	int																				 m_nUnloadsLastTick = 0;
	int																				 m_nRefreshesLastTick = 0;
	int																				 m_nResourceRequestsLastTick = 0;
	int																				 m_nResourceFailuresLastTick = 0;
	uint64_t																		 m_nLoadsTotal = 0u;
	uint64_t																		 m_nUnloadsTotal = 0u;
	uint64_t																		 m_nRefreshesTotal = 0u;
	uint64_t																		 m_nResourceRequestsTotal = 0u;
	uint64_t																		 m_nResourceFailuresTotal = 0u;
	uint64_t																		 m_nNodeRuntimeGeneration = 0u;
	bool																			 m_bHasPendingActivation = false;
	int																				 m_nPendingPackagesLastTick = 0;
	int																				 m_nCapacityDeferredLastTick = 0;
	int		 m_nCompatibilityFailuresLastTick = 0;
	uint64_t m_nCompatibilityFailuresTotal = 0u;
	size_t	 m_nResidentEstimatedBytes = 0u;

	t3dActivePackage_t		 *findActive( const Ref< Terrain3DRegionLighting > &rPackage );
	const t3dActivePackage_t *findActive( const Ref< Terrain3DRegionLighting > &rPackage ) const;
	t3dSourceLoadResult_t	  requestSourceData( const Ref< Terrain3DRegionLighting > &rPackage,
												 Ref< LightmapGIData >				  &rData );
	bool					  collectReceiverState( const Ref< Terrain3DRegionLighting > &rPackage,
													uint64_t							 &nSignature,
													int									 &nReceiverCount ) const;
	bool					  buildRuntimeData( const Ref< Terrain3DRegionLighting > &rPackage,
												const Ref< LightmapGIData >			 &rSource,
												Ref< LightmapGIData >				 &rData,
												uint64_t							 &nSignature,
												int									 &nReceiverCount ) const;
	bool					  activate( const Ref< Terrain3DRegionLighting > &rPackage,
										const Ref< LightmapGIData >			 &rSourceData,
										const Ref< LightmapGIData >			 &rRuntimeData,
										uint64_t							  nSignature,
										int									  nReceiverCount );
	bool					  refreshActive( t3dActivePackage_t			 &rActive,
											 const Ref< LightmapGIData > &rRuntimeData,
											 uint64_t					  nSignature,
											 int						  nReceiverCount );
	bool					  isPackageCompatibleWithActive( const Ref< Terrain3DRegionLighting > &rPackage ) const;
	LightmapGI				 *acquireNode( void );
	void					  releaseNode( LightmapGI *pNode );
	void					  deactivate( size_t iIndex );
	void					  markDesired( const Vector2i &rCenter );
	int						  getEffectiveResidencyBudgetMb( void ) const;

public:
	Terrain3DLightmapStreamer( void ) = default;
	~Terrain3DLightmapStreamer( void );

	void initialize( Object *pTerrain );
	void destroy( void );
	void updateResidency( const Vector3 &rTargetPosition, bool bForce = false );
	void suspend( void );

	void				 setEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isEnabled(
		void ) const {
		return m_bEnabled;
	}
	void				setRadiusRegions( int nRadius );
	EA_FORCE_INLINE int getRadiusRegions(
		void ) const {
		return m_nRadiusRegions;
	}
	EA_FORCE_INLINE void setLoadBudget(
		const int nBudget ) {
		m_nLoadBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getLoadBudget(
		void ) const {
		return m_nLoadBudget;
	}
	EA_FORCE_INLINE void setUnloadBudget(
		const int nBudget ) {
		m_nUnloadBudget = CLAMP( nBudget, 0, 64 );
	}
	EA_FORCE_INLINE int getUnloadBudget(
		void ) const {
		return m_nUnloadBudget;
	}
	void				setResidencyBudgetMb( int nBudgetMb );
	EA_FORCE_INLINE int getResidencyBudgetMb(
		void ) const {
		return m_nResidencyBudgetMb;
	}
	EA_FORCE_INLINE int getEffectiveResidencyBudgetMbForDiagnostics(
		void ) const {
		return getEffectiveResidencyBudgetMb();
	}
	EA_FORCE_INLINE size_t getResidentEstimatedBytes(
		void ) const {
		return m_nResidentEstimatedBytes;
	}
	EA_FORCE_INLINE bool isResidencyIdle(
		void ) const {
		return m_nLoadsLastTick == 0 && m_nRefreshesLastTick == 0;
	}
	Dictionary getStatsFast( void ) const;

	EA_FORCE_INLINE int getActivePackageCount(
		void ) const {
		return ( int ) m_activePackages.size();
	}
	Dictionary getStats( void ) const;

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_LIGHTMAP_STREAMER_H
