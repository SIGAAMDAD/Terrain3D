// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_CONTROLLER_H
#define TERRAIN3D_RENDER_WORLD_CONTROLLER_H

#include "authoring/render_world/terrain_3d_render_world_godot_converter.h"
#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_linear_arena.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3D;
class Terrain3DRenderCoordinator;

typedef bool ( *T3DRenderWorldPublishConvertedSceneColdFn )(
        void *pUser, const T3DRenderWorldSceneBuildBuffers &rBuffers, const T3DRenderWorldSceneBuildStats &rStats );

typedef void ( *T3DRenderWorldReleaseConvertedSceneColdFn )( void *pUser );

typedef struct t3dRenderWorldAuthoringBackend_s {
	T3DRenderWorldPublishConvertedSceneColdFn pPublishSceneCold = NULL;
	T3DRenderWorldReleaseConvertedSceneColdFn pReleaseSceneCold = NULL;
	void *pUser = NULL;
} T3DRenderWorldAuthoringBackend;

// Optional compatibility ingress for static Godot scene geometry; editor-facing and self-binding by default.
//
// Normal ownership model:
//   Terrain3D -> Terrain3DRenderCoordinator -> Terrain3DRenderWorldRuntime
//       ^
//       `-- Terrain3DRenderWorldController (child/descendant authoring boundary)
//
// The controller owns one fixed cold authoring image. It converts supported
// static MeshInstance3D data into compact RenderWorld records, publishes those
// records through the nearest Terrain3D coordinator, then hands visibility
// ownership to RenderWorld only after the render-thread upload completes.
class Terrain3DRenderWorldController : public godot::Node3D {
	GDCLASS( Terrain3DRenderWorldController, godot::Node3D );

private:
	static const uint32_t s_nScratchVertexCapacity = 262144u;
	static const uint32_t s_nScratchIndexCapacity = 786432u;
	static const uint32_t s_nScratchTriangleCapacity = 262144u;
	static const uint32_t s_nCellHashCapacity = 2048u;

	Terrain3DRenderWorldGodotConverter m_AutoConverter;
	Terrain3DLinearArena m_AuthoringArena;
	T3DGeometryDatabase m_AuthoringGeometry{};
	T3DRenderWorldOwnershipRegistry m_AuthoringOwnership{};
	T3DRenderWorldSceneBuildBuffers m_AuthoringBuffers{};
	Terrain3DRenderWorldGodotConverter *m_pConverter = NULL;
	Terrain3DRenderCoordinator *m_pCoordinator = NULL;
	T3DRenderWorldAuthoringBackend m_backend;
	godot::Ref<Terrain3DRenderWorldImportProfile> m_defaultProfile;
	T3DRenderWorldSceneBuildStats m_lastBuildStats{};
	uint64_t m_nAuthoringArenaBytes = 0u;
	int32_t m_iMode = ( int32_t )( T3D_RW_MODE_EXPLICIT );
	float m_fCellSize = 32.0f;
	bool m_bEnabled = true;
	bool m_bConvertMeshInstances = true;
	bool m_bConvertDynamicObjects = false;
	bool m_bConvertHiddenObjects = false;
	bool m_bOwnershipApplied = false;
	bool m_bAutoRebuildOnReady = true;
	bool m_bAutoConverterInitialized = false;

	T3DRenderWorldConversionPolicy buildPolicyCold( void ) const;
	bool bindNearestTerrainCold( void );
	bool initializeAuthoringImageCold( void );
	void releaseAuthoringImageCold( void );
	bool releasePublishedSceneCold( void );
	static bool publishAutoSceneCold(
	        void *pUser, const T3DRenderWorldSceneBuildBuffers &rBuffers, const T3DRenderWorldSceneBuildStats &rStats );
	static void releaseAutoSceneCold( void *pUser );

protected:
	static void _bind_methods( void );
	void _notification( int32_t nWhat );

public:
	Terrain3DRenderWorldController( void ) = default;
	~Terrain3DRenderWorldController( void );

	void setEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bEnabled;
	}
	void setMode( int32_t iMode );
	EA_FORCE_INLINE int32_t getMode( void ) const {
		return m_iMode;
	}
	void setConvertMeshInstances( bool bEnabled );
	EA_FORCE_INLINE bool isConvertMeshInstances( void ) const {
		return m_bConvertMeshInstances;
	}
	void setConvertDynamicObjects( bool bEnabled );
	EA_FORCE_INLINE bool isConvertDynamicObjects( void ) const {
		return m_bConvertDynamicObjects;
	}
	void setConvertHiddenObjects( bool bEnabled );
	EA_FORCE_INLINE bool isConvertHiddenObjects( void ) const {
		return m_bConvertHiddenObjects;
	}
	void setCellSize( float fCellSize );
	EA_FORCE_INLINE float getCellSize( void ) const {
		return m_fCellSize;
	}
	void setDefaultProfile( const godot::Ref<Terrain3DRenderWorldImportProfile> &rProfile );
	void setAutoRebuildOnReady( bool bEnabled );
	EA_FORCE_INLINE bool isAutoRebuildOnReady( void ) const {
		return m_bAutoRebuildOnReady;
	}
	EA_FORCE_INLINE godot::Ref<Terrain3DRenderWorldImportProfile> getDefaultProfile( void ) const {
		return m_defaultProfile;
	}

	bool requestColdRebuild( void );
	bool releaseRenderWorldOwnership( void );
	godot::Dictionary getLastBuildStatsReferenceDebug( void ) const;
	godot::Dictionary getBackendStatusReferenceDebug( void ) const;

	// Native override hooks retained for test/custom authoring hosts. Normal
	// editor use self-binds automatically and does not call these.
	void attachConverterCold( Terrain3DRenderWorldGodotConverter *pConverter );
	void attachBackendCold( const T3DRenderWorldAuthoringBackend &rBackend );
	EA_FORCE_INLINE Terrain3DRenderWorldGodotConverter *getConverter( void ) const {
		return m_pConverter;
	}
};

#endif // TERRAIN3D_RENDER_WORLD_CONTROLLER_H
