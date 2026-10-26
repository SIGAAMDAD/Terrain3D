// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_SHADER_LIBRARY_H
#define TERRAIN3D_RD_SHADER_LIBRARY_H
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_startup_state.h"
#include "instances/terrain_3d_instancer_bringup.h"
#include "rendering/rd/terrain_3d_rd_forward_shader_bundle.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <godot_cpp/classes/rd_shader_file.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

// WG-C SH04/SH05: set-3 fragment feedback is deliberately not a production
// raster binding until the single VT owner publishes frame/reset ordering and
// feedback parameters. These mirrors make the disabled policy machine-checkable
// instead of relying on an accidentally absent shader version define.
#define T3D_RD_TERRAIN_FRAGMENT_FEEDBACK_PRODUCTION_DISABLED 1
#define T3D_RD_INSTANCE_FRAGMENT_FEEDBACK_PRODUCTION_DISABLED 1

using namespace godot;

class Terrain3DRdShaderLibrary {
private:
	Ref<RDShaderSPIRV> m_near;
	Ref<RDShaderSPIRV> m_middle;
	Ref<RDShaderSPIRV> m_far;
	Ref<RDShaderSPIRV> m_cull;
	Ref<RDShaderSPIRV> m_instances;
	Ref<RDShaderSPIRV> m_hlodCull;
	Ref<RDShaderSPIRV> m_instanceCull;
	Ref<RDShaderSPIRV> m_instanceHiZReduce;
	Ref<RDShaderSPIRV> m_instanceOccluderDepth;
	Ref<RDShaderSPIRV> m_worldHiZReduce;
	Ref<RDShaderSPIRV> m_worldVisibilityReset;
	Ref<RDShaderSPIRV> m_worldVisibilityCull;
	Ref<RDShaderSPIRV> m_worldVisibilityFinalize;
	Ref<RDShaderSPIRV> m_worldRasterTerrainStrip[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Ref<RDShaderSPIRV> m_worldRasterTerrainStripShadow;
	Ref<RDShaderSPIRV> m_worldRasterMesh[T3D_RD_FORWARD_SHADER_VARIANT_MAX];
	Ref<RDShaderSPIRV> m_worldRasterMeshShadow;
	Ref<RDShaderSPIRV> m_oceanColor;
	Ref<RDShaderSPIRV> m_oceanShadow;
	t3dRdForwardShaderBundle_t m_forward;
	String m_lastError;
	uint32_t m_nShaderLoadAttemptCount = 0u;
	uint32_t m_nShaderLoadSuccessCount = 0u;
	uint32_t m_nShaderLoadFailureCount = 0u;
	uint32_t m_nShaderLoadExpectedCount = 0u;
	uint64_t m_nShaderLoadStartMsec = 0u;
	uint64_t m_nShaderLoadStartUsec = 0u;
	t3dAtomicU32_t m_nStartupGeneration{ 0u };
	t3dStartupDomainState_t m_startupState;
	uint64_t m_nShaderLoadElapsedUsec = 0u;
	static const uint32_t s_nShaderSourceCapacity = 25u;
	Ref<RDShaderFile> m_szSourceFiles[s_nShaderSourceCapacity];
	uint8_t m_szSourceStages[s_nShaderSourceCapacity]{};
	uint32_t m_nShaderSourceCount = 0u;
	uint32_t m_nShaderSourceRequestCount = 0u;
	uint32_t m_nShaderSourceReadyCount = 0u;
	uint32_t m_nShaderSourceFailureCount = 0u;
	bool m_bLoadRequested = false;
	bool m_bLoadCancelled = false;

	Ref<RDShaderFile> findSourceFile( const String &rPath ) const;
	Ref<RDShaderSPIRV> loadShader( const String &rPath, const StringName &rVersion = StringName() );
	void logShaderFailure( const String &rPath, const StringName &rVersion, uint64_t nStartedMsec );
	void logShaderReady( const String &rPath, const StringName &rVersion, uint64_t nStartedMsec );
	bool abortShaderLoad( void );
	bool resolveShaderBundleCold( void );

public:
	EA_FORCE_INLINE void setStartupGeneration( const uint32_t nGeneration ) {
		m_nStartupGeneration = nGeneration;
		t3dStartupDomainRequest( m_startupState, nGeneration, 0u, 0u );
	}
	EA_FORCE_INLINE t3dStartupDomainSnapshot_t getStartupSnapshot( void ) const {
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

	EA_FORCE_INLINE bool isReady( void ) const {
#if T3D_INSTANCER_MINIMAL_RENDER_PATH
		return m_near.is_valid() && m_middle.is_valid() && m_far.is_valid() && m_cull.is_valid() && m_instances.is_valid()
		        && m_instanceOccluderDepth.is_valid() && m_oceanColor.is_valid() && m_oceanShadow.is_valid() && m_worldHiZReduce.is_valid()
		        && m_worldVisibilityReset.is_valid() && m_worldVisibilityCull.is_valid() && m_worldVisibilityFinalize.is_valid()
		        && m_worldRasterTerrainStrip[0].is_valid() && m_worldRasterTerrainStripShadow.is_valid() && m_worldRasterMesh[0].is_valid()
		        && m_worldRasterMeshShadow.is_valid();
#else
		return m_near.is_valid() && m_middle.is_valid() && m_far.is_valid() && m_cull.is_valid() && m_instances.is_valid()
		        && m_instanceOccluderDepth.is_valid() && m_hlodCull.is_valid() && m_instanceCull.is_valid()
		        && m_instanceHiZReduce.is_valid() && m_oceanColor.is_valid() && m_oceanShadow.is_valid() && m_worldHiZReduce.is_valid()
		        && m_worldVisibilityReset.is_valid() && m_worldVisibilityCull.is_valid() && m_worldVisibilityFinalize.is_valid()
		        && m_worldRasterTerrainStrip[0].is_valid() && m_worldRasterTerrainStripShadow.is_valid() && m_worldRasterMesh[0].is_valid()
		        && m_worldRasterMeshShadow.is_valid();
#endif
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getNear( void ) const {
		return m_near;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getMiddle( void ) const {
		return m_middle;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getFar( void ) const {
		return m_far;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getCull( void ) const {
		return m_cull;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstances( void ) const {
		return m_instances;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getHlodCull( void ) const {
		return m_hlodCull;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstanceCull( void ) const {
		return m_instanceCull;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstanceHiZReduce( void ) const {
		return m_instanceHiZReduce;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstanceOccluderDepth( void ) const {
		return m_instanceOccluderDepth;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldHiZReduce( void ) const {
		return m_worldHiZReduce;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldVisibilityReset( void ) const {
		return m_worldVisibilityReset;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldVisibilityCull( void ) const {
		return m_worldVisibilityCull;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldVisibilityFinalize( void ) const {
		return m_worldVisibilityFinalize;
	}

	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldRasterTerrainStrip( const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX ? m_worldRasterTerrainStrip[variant] : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldRasterTerrainStripShadow( void ) const {
		return m_worldRasterTerrainStripShadow;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldRasterMesh( const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX ? m_worldRasterMesh[variant] : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getWorldRasterMeshShadow( void ) const {
		return m_worldRasterMeshShadow;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getOceanColor( void ) const {
		return m_oceanColor;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getOceanShadow( void ) const {
		return m_oceanShadow;
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getTerrainForward(
	        const t3dTerrainShaderPath_t path, const uint32_t nTier, const t3dRdForwardShaderVariant_t variant ) const {
		return path < T3D_TERRAIN_SHADER_PATH_COUNT && nTier < 3u && variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX
		        ? m_forward.m_szTerrain[path][nTier][variant]
		        : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getTerrainForwardOneSun(
	        const t3dTerrainShaderPath_t path, const uint32_t nTier, const t3dRdForwardShaderVariant_t variant ) const {
		return path < T3D_TERRAIN_SHADER_PATH_COUNT && nTier < 3u && variant < T3D_RD_FORWARD_COLOR_VARIANT_COUNT
		        ? m_forward.m_szTerrainOneSun[path][nTier][variant]
		        : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE Ref<RDShaderSPIRV> getInstanceForward( const t3dRdForwardShaderVariant_t variant ) const {
		return variant < T3D_RD_FORWARD_SHADER_VARIANT_MAX ? m_forward.m_szInstances[variant] : Ref<RDShaderSPIRV>();
	}
	EA_FORCE_INLINE const t3dRdForwardShaderBundle_t &getForwardBundle( void ) const {
		return m_forward;
	}

	EA_FORCE_INLINE String getLastError( void ) const {
		return m_lastError;
	}
	Dictionary getDiagnostics( void ) const;
};

#endif // TERRAIN3D_RD_SHADER_LIBRARY_H
