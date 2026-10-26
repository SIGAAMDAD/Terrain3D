// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RUNTIME_ARTIFACT_SERVICE_H
#define TERRAIN3D_RUNTIME_ARTIFACT_SERVICE_H

#include "data/runtime/terrain_3d_runtime_world_manifest.h"
#include "streaming/runtime/terrain_3d_runtime_page_store.h"
#include "data/persistence/terrain_3d_region_persistence.h"
#include "data/persistence/terrain_3d_region_repository.h"

// Owns cooked/runtime terrain artifacts behind one narrow data-domain owner.
// Terrain3DData remains the public facade; streaming/residency owners borrow the
// page-store reference and never duplicate runtime page or manifest state.
class Terrain3DRuntimeArtifactService
{
public:
	Terrain3DRuntimeArtifactService( Terrain3DRegionRepository		  *pRepository,
									 const Terrain3DRegionPersistence *pPersistence );
	Terrain3DRuntimeArtifactService( const Terrain3DRuntimeArtifactService & ) = delete;
	Terrain3DRuntimeArtifactService &operator=( const Terrain3DRuntimeArtifactService & ) = delete;

	bool bindRuntimeAllocator( Terrain3DMemoryZone		   *pZone,
							   t3dAllocatorOverflowPolicy_t ePolicy = TERRAIN3D_ALLOCATOR_OVERFLOW_STRICT );
	void reset( void );
	void releaseRuntimeAllocatorStorage( void );

	EA_FORCE_INLINE Terrain3DRuntimePageStore &pageStore(
		void ) {
		return m_pageStore;
	}
	EA_FORCE_INLINE const Terrain3DRuntimePageStore &pageStore(
		void ) const {
		return m_pageStore;
	}
	EA_FORCE_INLINE Terrain3DRuntimeWorldManifest &worldManifest(
		void ) {
		return m_worldManifest;
	}
	EA_FORCE_INLINE const Terrain3DRuntimeWorldManifest &worldManifest(
		void ) const {
		return m_worldManifest;
	}

	uint64_t computeSourceSignature( const Terrain3DRegion *pRegion, const t3dRuntimePageConfig_s &rConfig ) const;
	bool	 readSourceSignature( const String &rPagePath, uint64_t &rSignature ) const;
	String	 getPagePath( const Terrain3DRegion *pRegion, const String &rDataDirectory ) const;
	bool	 probePage( const String &rPagePath, t3dRuntimePageProbe_t &rProbe ) const;
	uint32_t readMaterialMask( const String &rPagePath ) const;
	bool	 removePageForSource( const String &rSourcePath ) const;
	bool	 removePage( const String &rPagePath ) const;
	bool	 validatePageForRegion( const Terrain3DRegion *pRegion, const t3dRuntimePageConfig_s &rConfig ) const;
	bool	 collectPageSources( const String										&rDataDirectory,
								 PackedStringArray									&rSourceFiles,
								 eastl::vector< Vector2i, Terrain3DEastlAllocator > &rLocations,
								 PackedStringArray									&rErrors ) const;

	void poll( bool bWait );
	void request( const Vector2i &rLocation, uint16_t nConsumers, uint8_t nPriority, bool bEnabled );
	void service( const t3dRuntimePageServiceState_t &rState );
	bool isPending( const Vector2i &rLocation ) const;
	bool isPrepared( const Vector2i &rLocation ) const;
	bool takeHeight( const Vector2i	 &rLocation,
					 uint16_t		  nConsumer,
					 Ref< Image >	 &rHeight,
					 bool			  bMiddle,
					 PackedByteArray *pNativeHeightBytes = NULL );
	bool takeInstance( const Vector2i		  &rLocation,
					   Terrain3DInstanceStore &rStore,
					   PackedByteArray		  *pInstanceMetadata = NULL,
					   PackedByteArray		  *pMultimeshFastStart = NULL );
	bool takeCpuMaps( const Vector2i &rLocation,
					  Ref< Image >	 &rHeight,
					  Ref< Image >	 &rControl,
					  Ref< Image >	 &rColor,
					  Ref< Image >	 &rDerived );
	bool takeNodeDescriptors( const Vector2i &rLocation, PackedByteArray &rDescriptors );
	bool takeCollision( const Vector2i	&rLocation,
						PackedByteArray &rHeight,
						PackedByteArray &rBounds,
						PackedByteArray &rHoleMask );
	bool takeNavigation( const Vector2i &rLocation, PackedByteArray &rNavigation );
	bool takeTerrainShadowMask( const Vector2i &rLocation, Ref< Image > &rShadowMask );
	bool takeGpuPage( const Vector2i &rLocation, t3dRuntimeGpuPageResult_t &rResult );

	bool loadCpuMapsNow( const Vector2i				  &rLocation,
						 const t3dRuntimePageConfig_s &rConfig,
						 Ref< Image >				  &rHeight,
						 Ref< Image >				  &rControl,
						 Ref< Image >				  &rColor,
						 Ref< Image >				  &rDerived ) const;
	bool loadCollisionNow( const Vector2i				&rLocation,
						   const t3dRuntimePageConfig_s &rConfig,
						   PackedByteArray				&rHeight,
						   PackedByteArray				&rBounds,
						   PackedByteArray				&rHoleMask ) const;
	bool loadNavigationNow( const Vector2i				 &rLocation,
							const t3dRuntimePageConfig_s &rConfig,
							PackedByteArray				 &rNavigation,
							PackedByteArray				 *pCollisionHeight = NULL,
							PackedByteArray				 *pMinMaxHeight = NULL ) const;
	bool loadLightingReceiversNow( const Vector2i				&rLocation,
								   const t3dRuntimePageConfig_s &rConfig,
								   PackedByteArray				&rReceivers ) const;
	bool encodeBakedPage( Terrain3DRegion					*pRegion,
						  const t3dRuntimePageConfig_s		&rConfig,
						  const t3dRuntimePageCookPayload_t &rPayload,
						  PackedByteArray					&rPageBytes,
						  String							*pError = NULL );

	void				  setPreparedByteBudget( uint64_t nBytes );
	uint64_t			  getPreparedReservedBytes( void ) const;
	t3dRuntimePageStats_t getStats( void ) const;

	bool buildAuthoringIndex( void );
	bool writeWorldManifest( const String				  &rDataDirectory,
							 const t3dRuntimePageConfig_s &rConfig,
							 PackedStringArray			  *pErrors = NULL );
	bool loadWorldManifest( const String &rDataDirectory, PackedStringArray *pErrors = NULL );
	bool stageRepository( const String &rDataDirectory, t3dStagedRepository_s &rStaged ) const;

private:
	Terrain3DRegionRepository	 *m_pRepository = NULL;
	Terrain3DRuntimePageStore	  m_pageStore;
	Terrain3DRuntimeWorldManifest m_worldManifest;
};

#endif // TERRAIN3D_RUNTIME_ARTIFACT_SERVICE_H
