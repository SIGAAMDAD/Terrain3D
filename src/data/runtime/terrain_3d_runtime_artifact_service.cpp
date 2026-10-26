// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/runtime/terrain_3d_runtime_artifact_service.h"

#include "data/persistence/terrain_3d_region_persistence.h"
#include "data/persistence/terrain_3d_region_repository.h"

Terrain3DRuntimeArtifactService::Terrain3DRuntimeArtifactService(
	Terrain3DRegionRepository *const		pRepository,
	const Terrain3DRegionPersistence *const pPersistence ) :
	m_pRepository( pRepository ),
	m_pageStore( pRepository, pPersistence ) {
}

bool Terrain3DRuntimeArtifactService::bindRuntimeAllocator(
	Terrain3DMemoryZone *const		   pZone,
	const t3dAllocatorOverflowPolicy_t ePolicy ) {
	return m_pageStore.bindRuntimeAllocator( pZone, ePolicy );
}

void Terrain3DRuntimeArtifactService::reset(
	void ) {
	m_pageStore.reset();
	m_worldManifest.reset();
}

void Terrain3DRuntimeArtifactService::releaseRuntimeAllocatorStorage(
	void ) {
	m_pageStore.reset();
	m_worldManifest.releaseRuntimeAllocatorStorage();
}

uint64_t Terrain3DRuntimeArtifactService::computeSourceSignature(
	const Terrain3DRegion *const  pRegion,
	const t3dRuntimePageConfig_s &rConfig ) const {
	return m_pageStore.computeSourceSignature( pRegion, rConfig );
}

bool Terrain3DRuntimeArtifactService::readSourceSignature(
	const String &rPagePath,
	uint64_t	 &rSignature ) const {
	return m_pageStore.readSourceSignature( rPagePath, rSignature );
}

String Terrain3DRuntimeArtifactService::getPagePath(
	const Terrain3DRegion *const pRegion,
	const String				&rDataDirectory ) const {
	return m_pageStore.getPagePath( pRegion, rDataDirectory );
}

bool Terrain3DRuntimeArtifactService::probePage(
	const String		  &rPagePath,
	t3dRuntimePageProbe_t &rProbe ) const {
	return m_pageStore.probePage( rPagePath, rProbe );
}

uint32_t Terrain3DRuntimeArtifactService::readMaterialMask(
	const String &rPagePath ) const {
	return m_pageStore.readMaterialMask( rPagePath );
}

bool Terrain3DRuntimeArtifactService::removePageForSource(
	const String &rSourcePath ) const {
	return m_pageStore.removePageForSource( rSourcePath );
}

bool Terrain3DRuntimeArtifactService::removePage(
	const String &rPagePath ) const {
	return m_pageStore.removePage( rPagePath );
}

bool Terrain3DRuntimeArtifactService::validatePageForRegion(
	const Terrain3DRegion *const  pRegion,
	const t3dRuntimePageConfig_s &rConfig ) const {
	return m_pageStore.validatePageForRegion( pRegion, rConfig );
}

bool Terrain3DRuntimeArtifactService::collectPageSources(
	const String									   &rDataDirectory,
	PackedStringArray								   &rSourceFiles,
	eastl::vector< Vector2i, Terrain3DEastlAllocator > &rLocations,
	PackedStringArray								   &rErrors ) const {
	return m_pageStore.collectPageSources( rDataDirectory, rSourceFiles, rLocations, rErrors );
}

void Terrain3DRuntimeArtifactService::poll(
	const bool bWait ) {
	m_pageStore.poll( bWait );
}

void Terrain3DRuntimeArtifactService::request(
	const Vector2i &rLocation,
	const uint16_t	nConsumers,
	const uint8_t	nPriority,
	const bool		bEnabled ) {
	m_pageStore.request( rLocation, nConsumers, nPriority, bEnabled );
}

void Terrain3DRuntimeArtifactService::service(
	const t3dRuntimePageServiceState_t &rState ) {
	m_pageStore.service( rState );
}

bool Terrain3DRuntimeArtifactService::isPending(
	const Vector2i &rLocation ) const {
	return m_pageStore.isPending( rLocation );
}

bool Terrain3DRuntimeArtifactService::isPrepared(
	const Vector2i &rLocation ) const {
	return m_pageStore.isPrepared( rLocation );
}

bool Terrain3DRuntimeArtifactService::takeHeight(
	const Vector2i		  &rLocation,
	const uint16_t		   nConsumer,
	Ref< Image >		  &rHeight,
	const bool			   bMiddle,
	PackedByteArray *const pNativeHeightBytes ) {
	return m_pageStore.takeHeight( rLocation, nConsumer, rHeight, bMiddle, pNativeHeightBytes );
}

bool Terrain3DRuntimeArtifactService::takeInstance(
	const Vector2i		   &rLocation,
	Terrain3DInstanceStore &rStore,
	PackedByteArray *const	pInstanceMetadata,
	PackedByteArray *const	pMultimeshFastStart ) {
	return m_pageStore.takeInstance( rLocation, rStore, pInstanceMetadata, pMultimeshFastStart );
}

bool Terrain3DRuntimeArtifactService::takeCpuMaps(
	const Vector2i &rLocation,
	Ref< Image >   &rHeight,
	Ref< Image >   &rControl,
	Ref< Image >   &rColor,
	Ref< Image >   &rDerived ) {
	return m_pageStore.takeCpuMaps( rLocation, rHeight, rControl, rColor, rDerived );
}

bool Terrain3DRuntimeArtifactService::takeNodeDescriptors(
	const Vector2i	&rLocation,
	PackedByteArray &rDescriptors ) {
	return m_pageStore.takeNodeDescriptors( rLocation, rDescriptors );
}

bool Terrain3DRuntimeArtifactService::takeCollision(
	const Vector2i	&rLocation,
	PackedByteArray &rHeight,
	PackedByteArray &rBounds,
	PackedByteArray &rHoleMask ) {
	return m_pageStore.takeCollision( rLocation, rHeight, rBounds, rHoleMask );
}

bool Terrain3DRuntimeArtifactService::takeNavigation(
	const Vector2i	&rLocation,
	PackedByteArray &rNavigation ) {
	return m_pageStore.takeNavigation( rLocation, rNavigation );
}

bool Terrain3DRuntimeArtifactService::takeTerrainShadowMask(
	const Vector2i &rLocation,
	Ref< Image >   &rShadowMask ) {
	return m_pageStore.takeTerrainShadowMask( rLocation, rShadowMask );
}

bool Terrain3DRuntimeArtifactService::takeGpuPage(
	const Vector2i			  &rLocation,
	t3dRuntimeGpuPageResult_t &rResult ) {
	return m_pageStore.takeGpuPage( rLocation, rResult );
}

bool Terrain3DRuntimeArtifactService::loadCpuMapsNow(
	const Vector2i				 &rLocation,
	const t3dRuntimePageConfig_s &rConfig,
	Ref< Image >				 &rHeight,
	Ref< Image >				 &rControl,
	Ref< Image >				 &rColor,
	Ref< Image >				 &rDerived ) const {
	return m_pageStore.loadCpuMapsNow( rLocation, rConfig, rHeight, rControl, rColor, rDerived );
}

bool Terrain3DRuntimeArtifactService::loadCollisionNow(
	const Vector2i				 &rLocation,
	const t3dRuntimePageConfig_s &rConfig,
	PackedByteArray				 &rHeight,
	PackedByteArray				 &rBounds,
	PackedByteArray				 &rHoleMask ) const {
	return m_pageStore.loadCollisionNow( rLocation, rConfig, rHeight, rBounds, rHoleMask );
}

bool Terrain3DRuntimeArtifactService::loadNavigationNow(
	const Vector2i				 &rLocation,
	const t3dRuntimePageConfig_s &rConfig,
	PackedByteArray				 &rNavigation,
	PackedByteArray *const		  pCollisionHeight,
	PackedByteArray *const		  pMinMaxHeight ) const {
	return m_pageStore.loadNavigationNow( rLocation, rConfig, rNavigation, pCollisionHeight, pMinMaxHeight );
}

bool Terrain3DRuntimeArtifactService::loadLightingReceiversNow(
	const Vector2i				 &rLocation,
	const t3dRuntimePageConfig_s &rConfig,
	PackedByteArray				 &rReceivers ) const {
	return m_pageStore.loadLightingReceiversNow( rLocation, rConfig, rReceivers );
}

bool Terrain3DRuntimeArtifactService::encodeBakedPage(
	Terrain3DRegion *const			   pRegion,
	const t3dRuntimePageConfig_s	  &rConfig,
	const t3dRuntimePageCookPayload_t &rPayload,
	PackedByteArray					  &rPageBytes,
	String *const					   pError ) {
	return m_pageStore.encodeBakedPage( pRegion, rConfig, rPayload, rPageBytes, pError );
}

void Terrain3DRuntimeArtifactService::setPreparedByteBudget(
	const uint64_t nBytes ) {
	m_pageStore.setPreparedByteBudget( nBytes );
}

uint64_t Terrain3DRuntimeArtifactService::getPreparedReservedBytes(
	void ) const {
	return m_pageStore.getPreparedReservedBytes();
}

t3dRuntimePageStats_t Terrain3DRuntimeArtifactService::getStats(
	void ) const {
	return m_pageStore.getStats();
}

bool Terrain3DRuntimeArtifactService::buildAuthoringIndex(
	void ) {
	return m_pRepository && m_worldManifest.buildAuthoringIndex( *m_pRepository );
}

bool Terrain3DRuntimeArtifactService::writeWorldManifest(
	const String				 &rDataDirectory,
	const t3dRuntimePageConfig_s &rConfig,
	PackedStringArray *const	  pErrors ) {
	return m_pRepository && m_worldManifest.write( rDataDirectory, *m_pRepository, m_pageStore, rConfig, pErrors );
}

bool Terrain3DRuntimeArtifactService::loadWorldManifest(
	const String			&rDataDirectory,
	PackedStringArray *const pErrors ) {
	return m_worldManifest.load( rDataDirectory, pErrors );
}

bool Terrain3DRuntimeArtifactService::stageRepository(
	const String		  &rDataDirectory,
	t3dStagedRepository_s &rStaged ) const {
	return m_worldManifest.stageRepository( rDataDirectory, rStaged );
}
