// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_SHADER_LIBRARY_H
#define TERRAIN3D_RD_SHADER_LIBRARY_H
#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_startup_state.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

// WG-C SH04/SH05: set-3 fragment feedback is deliberately not a production
// raster binding until the single VT owner publishes frame/reset ordering and
// feedback parameters. These mirrors make the disabled policy machine-checkable
// instead of relying on an accidentally absent shader version define.
#define T3D_RD_TERRAIN_FRAGMENT_FEEDBACK_PRODUCTION_DISABLED  1
#define T3D_RD_INSTANCE_FRAGMENT_FEEDBACK_PRODUCTION_DISABLED 0

using namespace godot;

class Terrain3DRdShaderLibrary
{
private:
	String					m_lastError;
	uint32_t				m_nShaderLoadAttemptCount = 0u;
	uint32_t				m_nShaderLoadSuccessCount = 0u;
	uint32_t				m_nShaderLoadFailureCount = 0u;
	uint32_t				m_nShaderLoadExpectedCount = 0u;
	uint64_t				m_nShaderLoadStartMsec = 0u;
	uint64_t				m_nShaderLoadStartUsec = 0u;
	t3dAtomicU32_t			m_nStartupGeneration{ 0u };
	t3dStartupDomainState_t m_startupState;
	uint64_t				m_nShaderLoadElapsedUsec = 0u;

	bool	 m_bPhysicalUser = false;
	uint64_t m_nPhysicalDeviceGeneration = 0u;
	uint8_t	 m_szSourceStages[T3D_RD_SHADER_SOURCE_CAPACITY]{};
	uint32_t m_nShaderSourceCount = 0u;
	uint32_t m_nShaderSourceRequestCount = 0u;
	uint32_t m_nShaderSourceReadyCount = 0u;
	uint32_t m_nShaderSourceFailureCount = 0u;
	bool	 m_bLoadRequested = false;
	bool	 m_bLoadCancelled = false;

	Ref< RDShaderFile >	 findSourceFile( const String &rPath ) const;
	Ref< RDShaderSPIRV > loadShader( const String &rPath, const StringName &rVersion = StringName() );
	Ref< RDShaderSPIRV > loadCatalogShader( t3dRdShaderId_t id );
	Ref< RDShaderSPIRV > getCatalogShader( t3dRdShaderId_t id ) const;
	void				 logShaderFailure( const String &rPath, const StringName &rVersion, uint64_t nStartedMsec );
	void				 logShaderReady( const String &rPath, const StringName &rVersion, uint64_t nStartedMsec );
	bool				 abortShaderLoad( void );
	bool				 resolveCatalogCold( void );

public:
	static Ref< RDShaderSPIRV > borrowSpirv( t3dRdShaderId_t id );
	static RID					borrowPhysical( t3dRdShaderId_t id );
	static RID					acquirePhysicalCold( Terrain3DRdContext &context, t3dRdShaderId_t id );
	static bool					initializeSharedState( void );
	static void					shutdownSharedState( void );
	static bool					physicalCacheReady( void );
	// Production WORLD/preview cull routing and page hash reduction are now
	// internal workload policies. Keep these cold query ABIs through SHD-05.
	static bool					isGroupedCullEnabledCold( void );
	// Project setting: terrain3d/rendering/compact_clusters (default false).
	static bool isCompactClustersEnabledCold( void );
	static bool isEnvironmentFieldsEnabledCold( void );
	static bool isTerrainGpuTimestampEnabledCold( void );
	static bool					isWorldSmallCullEnabledCold( void );
	static bool isWorldScanEnabledCold( void );
	static bool					isInstanceCommandOwnerEnabledCold( void );
	static bool					isInstanceExclusiveEnabledCold( void );
	static bool isPageReductionEnabledCold( void );
	static bool isMultiFeedbackExperimentEnabledCold( void );
	// 0 production, 1 opaque constant, 2 opacity only, 3 scalar-lit production.
	static uint32_t getInstanceGpuDiagnosticMode( void );
	bool			servicePhysicalPrewarm( Terrain3DRdContext &context, uint32_t budget );
	void			releasePhysical( Terrain3DRdContext &context );
	bool			isPhysicalReady( void ) const;
	// Renderer-owned counters; caller publishes into a POD snapshot on the render thread.
	void capturePhysicalCountersOnRenderThread( uint64_t &rReady, uint64_t &rExpected, uint64_t &rMisses ) const;
	void setStartupGeneration( const uint32_t nGeneration );
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getStartupSnapshot(
		void ) const {
		return t3dStartupDomainSnapshot( m_startupState );
	}
	void markStartupGpuPending( uint32_t nGeneration );
	void markStartupGpuReady( uint32_t nGeneration );
	void markStartupCommitted( uint32_t nGeneration );
	bool beginLoadCold( uint32_t nStartupGeneration );
	void serviceLoadCold( void );
	bool isCpuReady( uint32_t nStartupGeneration ) const;
	bool isGpuReady( uint32_t nStartupGeneration ) const;
	bool isFailed( uint32_t nStartupGeneration ) const;
	void cancelLoadCold( uint32_t nStartupGeneration );
	void clear( void );
	// Compute devices reuse catalog source/SPIR-V while owning device-specific RIDs.
	static Ref< RDShaderSPIRV > resolveComputeShader( t3dRdShaderId_t id );
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getShader(
		t3dRdShaderId_t id ) const {
		return getCatalogShader( id );
	}

	bool			isReady( void ) const;
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getNear(
		void ) const {
		return getShader( T3D_RD_SHADER_NEAR_ALIAS );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getMiddle(
		void ) const {
		return getShader( T3D_RD_SHADER_MIDDLE_ALIAS );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getFar(
		void ) const {
		return getShader( T3D_RD_SHADER_FAR_ALIAS );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getCull(
		void ) const {
		return getShader( T3D_RD_SHADER_TERRAIN_CULL );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getInstances(
		void ) const {
		return getShader( T3D_RD_SHADER_INSTANCE_ALIAS );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getHlodCull(
		void ) const {
		return getShader( T3D_RD_SHADER_HLOD_CULL );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getInstanceCull(
		void ) const {
		return getShader( T3D_RD_SHADER_INSTANCE_CULL );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getInstanceOccluderDepth(
		void ) const {
		return getShader( T3D_RD_SHADER_INSTANCE_OCCLUDER_DEPTH );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldHiZReduce(
		void ) const {
		return getShader( T3D_RD_SHADER_WORLD_HIZ_REDUCE );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldVisibilityReset(
		void ) const {
		return getShader( T3D_RD_SHADER_WORLD_VISIBILITY_RESET );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldVisibilityCull(
		void ) const {
		return getShader( T3D_RD_SHADER_WORLD_VISIBILITY_CULL );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldVisibilityFinalize(
		void ) const {
		return getShader( T3D_RD_SHADER_WORLD_VISIBILITY_FINALIZE );
	}

	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldRasterTerrainStrip(
		const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX
				   ? getShader( ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_RASTER_STRIP_BASE + variant ) )
				   : Ref< RDShaderSPIRV >();
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldRasterTerrainStripShadow(
		void ) const {
		return getShader( T3D_RD_SHADER_WORLD_RASTER_STRIP_SHADOW );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldRasterMesh(
		const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX
				   ? getShader( ( t3dRdShaderId_t ) ( T3D_RD_SHADER_WORLD_RASTER_MESH_BASE + variant ) )
				   : Ref< RDShaderSPIRV >();
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getWorldRasterMeshShadow(
		void ) const {
		return getShader( T3D_RD_SHADER_WORLD_RASTER_MESH_SHADOW );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getOceanColor(
		void ) const {
		return getShader( T3D_RD_SHADER_OCEAN_COLOR );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getOceanShadow(
		void ) const {
		return getShader( T3D_RD_SHADER_OCEAN_SHADOW );
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getTerrainForward(
		const t3dTerrainShaderPath_t	  path,
		const uint32_t					  nTier,
		const t3dRdForwardShaderVariant_t variant ) const {
		return path < T3D_TERRAIN_SHADER_PATH_COUNT && nTier < 3u && variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX
				   ? getShader( t3dRdTerrainForwardId( path, nTier, variant ) )
				   : Ref< RDShaderSPIRV >();
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getTerrainForwardOneSun(
		const t3dTerrainShaderPath_t	  path,
		const uint32_t					  nTier,
		const t3dRdForwardShaderVariant_t variant ) const {
		return path < T3D_TERRAIN_SHADER_PATH_COUNT && nTier < 3u && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT
				   ? getShader( t3dRdTerrainOneSunId( path, nTier, variant ) )
				   : Ref< RDShaderSPIRV >();
	}
	EA_FORCE_INLINE Ref< RDShaderSPIRV > getInstanceForward(
		const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX
				   ? getShader( ( t3dRdShaderId_t ) ( T3D_RD_SHADER_INSTANCE_FORWARD_BASE + variant ) )
				   : Ref< RDShaderSPIRV >();
	}

	EA_FORCE_INLINE String getLastError(
		void ) const {
		return m_lastError;
	}
	Dictionary getDiagnostics( void ) const;
};

#endif // TERRAIN3D_RD_SHADER_LIBRARY_H
