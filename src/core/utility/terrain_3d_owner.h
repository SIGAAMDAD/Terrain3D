#ifndef TERRAIN3D_OWNER_H
#define TERRAIN3D_OWNER_H
#include "Terrain3DTypes.h"
#include "core/utility/terrain_3d_authoring_hooks.h"
#include "core/config/terrain_3d_compiler.h"
#include "data/runtime/terrain_3d_bake_source.h"
#include "instances/contracts/terrain_3d_instance_mutation.h"
#include "rendering/geometry/terrain_3d_clipmap_source.h"
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3DData;
class Terrain3DInstancer;
class Terrain3DCollision;
class Terrain3DHLODStreamer;
class Terrain3DNodeStreamer;
class Terrain3DAssets;
class Terrain3DMaterial;
class Terrain3DOceanSettings;
class Terrain3DMemoryAccount;

// Physics borrows only its scene, residency and collision policy capabilities.
class Terrain3DCollisionHost
{
protected:
	~Terrain3DCollisionHost( void ) = default;

public:
	virtual godot::Node3D				   &sceneNode( void ) const = 0;
	virtual Terrain3DData				   *dataObject( void ) const = 0;
	virtual godot::Ref< Terrain3DMaterial > materialResource( void ) const = 0;
	virtual Vector3							getClipmapTargetPosition( void ) const = 0;
	virtual t3dEditorExecutionMode_t		getEditorExecutionMode( void ) const = 0;
	virtual uint64_t						getRegionStreamingGeneration( void ) const = 0;
	virtual t3dRegionStreamingState_t		getRegionStreamingStateInternal( const Vector2i &vLocation ) const = 0;
	virtual bool							isGpuRegionCookedPagesEnabled( void ) const = 0;
	virtual bool							isInsideWorld( void ) const = 0;
};

// Composite attachment contract at integration boundaries. Physics and region
// authoring consumers borrow their narrow capability directly.
class Terrain3DOwner : public Terrain3DCollisionHost
{
protected:
	~Terrain3DOwner( void ) = default;

public:
	virtual Terrain3DInstancer	   *instancerObject( void ) const = 0;
	virtual Terrain3DMemoryAccount *getMemoryAccountInternal(
		void ) const {
		return nullptr;
	}
	virtual uint32_t getMegaVTTexelsPerMeterInternal(
		void ) const {
		return 2u;
	}
	virtual Terrain3DCollision						*collisionObject( void ) const = 0;
	virtual Terrain3DHLODStreamer					*hlodObject( void ) const = 0;
	// The WORLD render thread publishes the exact prepared HLOD source generation.
	virtual uint64_t getPreparedHlodSourceGenerationCold( void ) const = 0;
	virtual Terrain3DNodeStreamer					*nodeStreamerObject( void ) const = 0;
	virtual Terrain3DAuthoringHooks					*authoringHooks( void ) const = 0;
	virtual godot::Ref< Terrain3DAssets >			 assetsResource( void ) const = 0;
	virtual godot::Ref< Terrain3DOceanSettings >	 oceanSettingsResource( void ) const = 0;
	virtual Terrain3DClipmapSource					*getMesher( void ) const = 0;
	virtual Terrain3DClipmapSource					*getOceanMesher( void ) const = 0;
	virtual uint64_t								 instanceOcclusionPublicationGeneration( void ) const = 0;
	virtual const t3dRuntimePageNumericConfig_t		&runtimePageNumericConfig( void ) const = 0;
	virtual void									 notifyTerrainMapsEdited( const godot::AABB &rArea ) = 0;
	virtual void									 generateTrianglePair( t3dTriangleBuffer_t			  &rBuffer,
																		   const t3dTrianglePairRequest_t &rRequest ) const = 0;
	virtual Camera3D								*getCamera( void ) const = 0;
	virtual GeometryInstance3D::ShadowCastingSetting getCastShadows( void ) const = 0;
	virtual uint32_t								 getCollisionLayer( void ) const = 0;
	virtual uint32_t								 getCollisionMask( void ) const = 0;
	virtual t3dCollisionMode_t						 getCollisionMode( void ) const = 0;
	virtual real_t									 getCollisionPriority( void ) const = 0;
	virtual uint16_t								 getCollisionRadius( void ) const = 0;
	virtual RID										 getComputeDisplacementTextureRid( void ) const = 0;
	virtual int										 getCpuRegionLoadBudgetLayers( void ) const = 0;
	virtual int										 getCpuRegionLoadBudgetMb( void ) const = 0;
	virtual real_t									 getCullMargin( void ) const = 0;
	virtual String									 getDataDirectory( void ) const = 0;
	virtual real_t									 getDisplacementScale( void ) const = 0;
	virtual int										 getEditorGpuRegionCachePages( void ) const = 0;
	virtual int										 getEffectiveCpuRegionCacheBudgetMb( void ) const = 0;
	virtual uint64_t								 getEffectiveGpuRegionCacheBudgetBytes( void ) const = 0;
	virtual int										 getEffectiveInstanceRegionCacheBudgetMb( void ) const = 0;
	virtual int										 getFarMaterialResolution( void ) const = 0;
	virtual bool									 getFarShadowEnabled( void ) const = 0;
	virtual int										 getFarShadowLodCount( void ) const = 0;
	virtual int										 getFarVtHeightSourceSide( void ) const = 0;
	virtual int										 getGpuFarQualityDivisor( void ) const = 0;
	virtual int										 getFarVtSurfaceSourceSide( void ) const = 0;
	virtual int										 getGpuRegionCachePageLimit( void ) const = 0;
	virtual int										 getGpuRegionColorResolutionDivisor( void ) const = 0;
	virtual int										 getGpuRegionDerivedResolutionDivisor( void ) const = 0;
	virtual int										 getGpuRegionDetailLods( void ) const = 0;
	virtual int										 getGpuRegionHysteresisTicks( void ) const = 0;
	virtual int										 getGpuRegionRetireFrames( void ) const = 0;
	virtual uint8_t	 getHlodInstancerMinimumLevel( const Vector2i &rRegion, int nMeshId ) const = 0;
	virtual uint8_t	 getHlodNodeMinimumLevel( const Vector2i &rRegion, int64_t nSpawnId ) const = 0;
	virtual int		 getInstanceRegionLoadBudgetMb( void ) const = 0;
	virtual int		 getInstanceRegionLoadBudgetPages( void ) const = 0;
	virtual float	 getClutterTextureMipBias( void ) const = 0;
	virtual float	 getGroundCoverTextureMipBias( void ) const = 0;
	virtual float	 getOccluderTextureMipBias( void ) const = 0;
	virtual uint32_t getInstanceTextureFilterOverrides( void ) const = 0;
	virtual Node3D	*getLightTarget( void ) const = 0;
	virtual int		 getMeshLods( void ) const = 0;
	virtual int		 getMeshSize( void ) const = 0;
	virtual bool	 getMiddleShadowEnabled( void ) const = 0;
	virtual int		 getMiddleShadowLodCount( void ) const = 0;
	virtual bool	 getNearShadowEnabled( void ) const = 0;
	virtual int		 getNearShadowLodCount( void ) const = 0;
	virtual GeometryInstance3D::ShadowCastingSetting getOceanCastShadows( void ) const = 0;
	virtual real_t									 getOceanCullMargin( void ) const = 0;
	virtual t3dOceanFoamMode_t						 getOceanFoamMode( void ) const = 0;
	virtual int										 getOceanFragmentWaveIterations( void ) const = 0;
	virtual int										 getOceanMeshSize( void ) const = 0;
	virtual uint64_t								 getOceanPublicationGeneration( void ) const = 0;
	virtual t3dOceanRefractionMode_t				 getOceanRefractionMode( void ) const = 0;
	virtual uint32_t								 getOceanRenderLayers( void ) const = 0;
	virtual t3dOceanScatteringMode_t				 getOceanScatteringMode( void ) const = 0;
	virtual t3dOceanShadingMode_t					 getOceanShadingMode( void ) const = 0;
	virtual int										 getOceanTessellationLevel( void ) const = 0;
	virtual real_t									 getOceanVertexSpacing( void ) const = 0;
	virtual int										 getOceanVertexWaveIterations( void ) const = 0;
	virtual Object									*getPlugin( void ) const = 0;
	virtual Dictionary								 getRaycastResult( const Vector3 &rSrcPos,
																	   const Vector3 &rDirection,
																	   const uint32_t nColMask = 0xFFFFFFFF,
																	   const bool	  bExcludeSelf = false ) const = 0;
	virtual Vector2i								 getRegionStreamingCenterInternal( void ) const = 0;
	virtual Vector2i								 getRegionStreamingPredictedCenterInternal( void ) const = 0;
	virtual const uint8_t							*getRegionStreamingPrefetchInternal( void ) const = 0;
	virtual const uint8_t							*getRegionStreamingStatesInternal( void ) const = 0;
	virtual Dictionary								 getRegionStreamingStats( void ) const = 0;
	virtual uint32_t								 getRenderLayers( void ) const = 0;
	virtual real_t									 getRuntimeMiddleRangeEndM( void ) const = 0;
	virtual int										 getRuntimeMiddleShaderLods( void ) const = 0;
	virtual real_t									 getRuntimeNearRangeEndM( void ) const = 0;
	virtual int										 getRuntimeNearShaderLods( void ) const = 0;
	virtual real_t									 getRuntimeShadowRangeEndM( void ) const = 0;
	virtual bool									 getSave16Bit( void ) const = 0;
	virtual int										 getShadowLodCount( void ) const = 0;
	virtual uint32_t								 getStartupGenerationInternal( void ) const = 0;
	virtual int										 getStreamingActiveRadius( void ) const = 0;
	virtual int										 getStreamingColdRadius( void ) const = 0;
	virtual int										 getStreamingHotRadius( void ) const = 0;
	virtual int										 getTessellationLevel( void ) const = 0;
	virtual real_t									 getVertexSpacing( void ) const = 0;
	virtual int										 getVirtualTextureQualityProfile( void ) const = 0;
	virtual int										 getVtAdmissionBudget( void ) const = 0;
	virtual int										 getVtRequestBudget( void ) const = 0;
	virtual uint8_t									 getWarnings( void ) const = 0;
	virtual int										 getWorldClusterUploadBudget( void ) const = 0;
	virtual t3dWorldResourceLimits_t				 getWorldResourceLimitsInternal( void ) const = 0;
	virtual int										 getWorldUploadBudgetKb( void ) const = 0;
	virtual bool									 isComputeDisplacementActive( void ) const = 0;
	virtual bool									 isCpuRegionCacheEffective( void ) const = 0;
	virtual bool									 isDominantSunOnly( void ) const = 0;
	virtual bool									 isEditorAuthoringMode( void ) const = 0;
	virtual bool									 isEditorNodeStreamingPreviewActive( void ) const = 0;
	virtual bool									 isGpuRegionCookOnSave( void ) const = 0;
	virtual bool isHlodDetailReady( const Vector2i &rRegionMin, const Vector2i &rRegionMax ) const = 0;
	virtual bool isHlodRegionSuppressed( const Vector2i &rRegion, int nMinimumLevel = 1 ) const = 0;
    virtual bool isHlodNodeVisualSuppressed( const Vector2i &rRegion, int64_t nSpawnId, int nMinimumLevel ) const = 0;
	virtual bool isInitialized( void ) const = 0;
	virtual bool isInstanceRegionCacheEffective( void ) const = 0;
	virtual bool isOceanEnabled( void ) const = 0;
	virtual bool isRegionStreamingPrefetchInternal( const Vector2i &vLocation ) const = 0;
	virtual bool isRuntimeProfilingEnabled( void ) const = 0;
	virtual bool isRuntimeShaderTieringActive( void ) const = 0;
	virtual void onHlodRegionRepresentationChanged( const Vector2i &rRegion ) = 0;
	virtual void onInstanceAssetPolicyChanged( int nMeshId ) = 0;
	virtual void onInstanceDerivedMutation( const t3dInstanceMutationEvent_t &rEvent ) = 0;
	virtual void reconfigureGpuRegionCache( void ) = 0;
	virtual void reconfigureGpuRegionCache( bool bEnabled ) = 0;
	virtual bool requestRdMeshPreviewAsync( uint16_t			   nAssetId,
											const Vector2i		  &rSize,
											uint64_t			   nCompletionOwnerId,
											uint64_t			   nCompletionGeneration,
											t3dRdPreviewFailure_t *pFailure = NULL ) = 0;
	virtual void setWarning( const uint8_t nWarning, const bool bEnabled ) = 0;
	virtual void snap( void ) = 0;
};

template< typename T >
EA_FORCE_INLINE uint64_t t3dOwnerDiagnosticIdentity(
	const T *const pOwner ) {
	// Native capability pointers are not Godot Objects. Passing one directly to
	// Variant selects its raw-Variant-pointer constructor and reads invalid memory.
	// An integer identity also avoids dereferencing an owner during teardown.
	return ( uint64_t ) ( uintptr_t ) pOwner;
}

template< typename T >
EA_FORCE_INLINE T *t3dTransferOwner(
	T *&pOwner,
	T  *pNextOwner ) {
	T *pPreviousOwner = pOwner;
	pOwner = pNextOwner;
	return pPreviousOwner;
}

template< typename T >
EA_FORCE_INLINE bool t3dReleaseOwner(
	T *&pOwner,
	T  *pReleasingOwner ) {
	if ( !pReleasingOwner || pOwner != pReleasingOwner ) {
		return false;
	}
	pOwner = NULL;
	return true;
}

#endif // TERRAIN3D_OWNER_H
