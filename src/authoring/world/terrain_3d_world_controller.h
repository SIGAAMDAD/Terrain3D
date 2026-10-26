// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_CONTROLLER_H
#define TERRAIN3D_WORLD_CONTROLLER_H

#include "authoring/world/terrain_3d_world_godot_converter.h"
#include "core/terrain_3d_compiler.h"
#include "memory/terrain_3d_linear_arena.h"
#include "rendering/rd/world/terrain_3d_world_publication.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3D;
class Terrain3DRenderCoordinator;

typedef bool ( *T3DWorldPublishConvertedSceneColdFn )(
        void *pUser, const T3DWorldSceneBuildBuffers &rBuffers, const T3DWorldSceneBuildStats &rStats );

typedef void ( *T3DWorldReleaseConvertedSceneColdFn )( void *pUser );

typedef struct t3dWorldAuthoringBackend_s {
	T3DWorldPublishConvertedSceneColdFn pPublishSceneCold = NULL;
	T3DWorldReleaseConvertedSceneColdFn pReleaseSceneCold = NULL;
	void *pUser = NULL;
} T3DWorldAuthoringBackend;

// Canonical Terrain3D World authoring ingress for static Godot scene geometry; editor-facing and self-binding by default.
//
// Ownership model:
//   Terrain3D -> Terrain3DRenderCoordinator -> Unified World publication
//       ^
//       `-- Terrain3DWorldController (child/descendant authoring boundary)
//
// The controller owns one fixed cold authoring image. It converts supported
// static MeshInstance3D data directly into World geometry/publication records,
// publishes them through the nearest Terrain3D coordinator, then hands visibility
// ownership to World only after the render-thread upload completes.
class Terrain3DWorldController : public godot::Node3D {
	GDCLASS( Terrain3DWorldController, godot::Node3D );

private:
	static const uint32_t s_nScratchVertexCapacity = 262144u;
	static const uint32_t s_nScratchIndexCapacity = 786432u;
	static const uint32_t s_nScratchTriangleCapacity = 262144u;
	static const uint32_t s_nCellHashCapacity = 2048u;

	Terrain3DWorldGodotConverter m_AutoConverter;
	Terrain3DLinearArena m_AuthoringArena;
	T3DWorldGeometryDatabase m_AuthoringGeometry{};
	T3DWorldOwnershipRegistry m_AuthoringOwnership{};
	T3DWorldSceneBuildBuffers m_AuthoringBuffers{};
	Terrain3DWorldPublication m_AuthoringWorldPublication;
	Terrain3DWorldGodotConverter *m_pConverter = NULL;
	Terrain3DRenderCoordinator *m_pCoordinator = NULL;
	T3DWorldAuthoringBackend m_backend;
	godot::Ref<Terrain3DWorldImportProfile> m_defaultProfile;
	T3DWorldSceneBuildStats m_lastBuildStats{};
	uint64_t m_nAuthoringArenaBytes = 0u;
	int32_t m_iMode = ( int32_t )( T3D_WORLD_MODE_EXPLICIT );
	float m_fCellSize = 32.0f;
	bool m_bEnabled = true;
	bool m_bConvertMeshInstances = true;
	bool m_bConvertDynamicObjects = false;
	bool m_bConvertHiddenObjects = false;
	bool m_bOwnershipApplied = false;
	bool m_bAutoRebuildOnReady = true;
	bool m_bAutoConverterInitialized = false;

	T3DWorldConversionPolicy buildPolicyCold( void ) const;
	bool bindNearestTerrainCold( void );
	bool initializeAuthoringImageCold( void );
	void releaseAuthoringImageCold( void );
	bool releasePublishedSceneCold( void );
	static bool publishAutoSceneCold(
	        void *pUser, const T3DWorldSceneBuildBuffers &rBuffers, const T3DWorldSceneBuildStats &rStats );
	static void releaseAutoSceneCold( void *pUser );
	static bool resolveAutoMaterialVtCold( void *pUser, uint64_t nStableMaterialKey, uint32_t &rAllocationId );

protected:
	static void _bind_methods( void );
	void _notification( int32_t nWhat );

public:
	Terrain3DWorldController( void ) = default;
	~Terrain3DWorldController( void );

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
	void setDefaultProfile( const godot::Ref<Terrain3DWorldImportProfile> &rProfile );
	void setAutoRebuildOnReady( bool bEnabled );
	EA_FORCE_INLINE bool isAutoRebuildOnReady( void ) const {
		return m_bAutoRebuildOnReady;
	}
	EA_FORCE_INLINE godot::Ref<Terrain3DWorldImportProfile> getDefaultProfile( void ) const {
		return m_defaultProfile;
	}

	bool requestColdRebuild( void );
	bool releaseWorldOwnership( void );
	godot::Dictionary getLastBuildStatsReferenceDebug( void ) const;
	godot::Dictionary getBackendStatusReferenceDebug( void ) const;

	// Native override hooks retained for test/custom authoring hosts. Normal
	// editor use self-binds automatically and does not call these.
	void attachConverterCold( Terrain3DWorldGodotConverter *pConverter );
	void attachBackendCold( const T3DWorldAuthoringBackend &rBackend );
	EA_FORCE_INLINE Terrain3DWorldGodotConverter *getConverter( void ) const {
		return m_pConverter;
	}
};

#endif // TERRAIN3D_WORLD_CONTROLLER_H
