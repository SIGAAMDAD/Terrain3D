// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DTypeSupport.h"
#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "authoring/editor/terrain_3d_editor.h"
#include "core/utility/logger.h"
#include "core/utility/terrain_3d_integration_helpers.h"
#include "core/utility/terrain_3d_util.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "eastl_config.h"
#include "hlod/runtime/terrain_3d_hlod_streamer.h"
#include "lighting/policy/terrain_3d_lighting_policy.h"
#include "lighting/runtime/terrain_3d_lightmap_streamer.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "memory/core/terrain_3d_memory.h"
#include "memory/arena/terrain_3d_zone_allocator.h"
#include "rendering/compute/terrain_3d_compute.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "simd/core/terrain_3d_parallel.h"
#include "streaming/runtime/terrain_3d_node_streamer.h"
#include "streaming/policy/terrain_3d_performance_profile.h"
#include "streaming/policy/terrain_3d_streaming_policy.h"
#include "terrain_3d.h"

#include <string.h>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/surface_tool.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>

void Terrain3D::connectResourceSignals( const Ref<Terrain3DAssets> &rAssets, const Ref<Terrain3DMaterial> &rMaterial ) {
	if ( rMaterial.is_null() ) {
		return;
	}
	const Callable maps_callback = callable_mp( this, &Terrain3D::onDataMapsChanged );
	if ( m_pData && !m_pData->is_connected( "maps_changed", maps_callback ) ) {
		m_pData->connect( "maps_changed", maps_callback );
	}
	const Callable page_table_callback = callable_mp( this, &Terrain3D::onGpuRegionPageTableChanged );
	if ( m_pData && !m_pData->is_connected( "gpu_region_page_table_changed", page_table_callback ) ) {
		m_pData->connect( "gpu_region_page_table_changed", page_table_callback );
	}
	const Callable textures_callback = callable_mp( this, &Terrain3D::onAssetsTexturesChanged );
	if ( rAssets.is_valid() && !rAssets->is_connected( "textures_changed", textures_callback ) ) {
		rAssets->connect( "textures_changed", textures_callback );
	}
	const Callable material_callback = callable_mp( this, &Terrain3D::onProfileOwnedResourceChanged );
	if ( !rMaterial->is_connected( "changed", material_callback ) ) {
		rMaterial->connect( "changed", material_callback );
	}
}

void Terrain3D::disconnectResourceSignals( const Ref<Terrain3DAssets> &rAssets, const Ref<Terrain3DMaterial> &rMaterial ) {
	if ( rMaterial.is_null() ) {
		return;
	}
	const Callable maps_callback = callable_mp( this, &Terrain3D::onDataMapsChanged );
	if ( m_pData && m_pData->is_connected( "maps_changed", maps_callback ) ) {
		m_pData->disconnect( "maps_changed", maps_callback );
	}
	const Callable page_table_callback = callable_mp( this, &Terrain3D::onGpuRegionPageTableChanged );
	if ( m_pData && m_pData->is_connected( "gpu_region_page_table_changed", page_table_callback ) ) {
		m_pData->disconnect( "gpu_region_page_table_changed", page_table_callback );
	}
	const Callable textures_callback = callable_mp( this, &Terrain3D::onAssetsTexturesChanged );
	if ( rAssets.is_valid() && rAssets->is_connected( "textures_changed", textures_callback ) ) {
		rAssets->disconnect( "textures_changed", textures_callback );
	}
	const Callable material_callback = callable_mp( this, &Terrain3D::onProfileOwnedResourceChanged );
	if ( rMaterial->is_connected( "changed", material_callback ) ) {
		rMaterial->disconnect( "changed", material_callback );
	}
}

void Terrain3D::connectAutoShaderProfileSignal( const Ref<Resource> &rProfile ) {
	if ( rProfile.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &Terrain3D::onAutoShaderProfileChanged );
	if ( !rProfile->is_connected( "changed", callback ) ) {
		rProfile->connect( "changed", callback );
	}
}

void Terrain3D::disconnectAutoShaderProfileSignal( const Ref<Resource> &rProfile ) {
	if ( rProfile.is_null() ) {
		return;
	}
	const Callable callback = callable_mp( this, &Terrain3D::onAutoShaderProfileChanged );
	if ( rProfile->is_connected( "changed", callback ) ) {
		rProfile->disconnect( "changed", callback );
	}
}

void Terrain3D::applyAutoShaderProfileMode( void ) {
	if ( m_material.is_null() ) {
		return;
	}
	const bool strict_cooked = m_bGpuRegionCookedPagesEnabled && !isEditorAuthoringModeInternal();
	if ( m_autoShaderProfile.is_null() ) {
		if ( strict_cooked ) {
			m_material->setAutoShaderEnabled( false );
		}
		return;
	}
	const Variant mode_value = m_autoShaderProfile->get( "mode" );
	if ( mode_value.get_type() != Variant::INT ) {
		return;
	}
	const int mode = ( int )mode_value;
	if ( strict_cooked ) {
		// Production cooked pages already contain resolved material classification.
		// Never reactivate fragment-time procedural classification in this profile.
		if ( mode == T3D_AUTOSHADER_MODE_LEGACY_RUNTIME || mode == T3D_AUTOSHADER_MODE_EDITOR_PREVIEW ) {
			ERR_PRINT_ONCE( "Terrain3D strict cooked runtime ignores live/legacy autoshader classification; bake the "
			                "profile into control maps before export." );
		}
		m_material->setAutoShaderEnabled( false );
		return;
	}
	m_material->setAutoShaderEnabled( mode == T3D_AUTOSHADER_MODE_LEGACY_RUNTIME );
}

void Terrain3D::onAutoShaderProfileChanged( void ) {
	applyAutoShaderProfileMode();
}

void Terrain3D::onDataMapsChanged( void ) {
	// Signals can arrive from an inactive editor scene whose resource objects
	// have already transferred to the newly active Terrain3D. Only the current
	// owner may rebind shared material state.
	if ( m_material.is_valid() && m_material->isInitializedFor( this ) ) {
		m_material->update( REGION_ARRAYS );
	}
}

void Terrain3D::onGpuRegionPageTableChanged( void ) {
	if ( m_material.is_valid() && m_material->isInitializedFor( this ) ) {
		m_material->updateRegionPageTable();
	}
	// The compute displacement request captures the physical GPU page table.
	// Page publication can complete after the initial request, so invalidate the
	// displacement state independently of clipmap movement and retry next tick.
	if ( m_nTessellationLevel > 0 ) {
		m_bComputeDisplacementDirty.store( true, T3D_MEMORY_ORDER_RELEASE );
	}
}

void Terrain3D::onAssetsTexturesChanged( void ) {
	if ( m_assets.is_valid() && m_assets->isInitializedFor( this ) && m_material.is_valid() && m_material->isInitializedFor( this ) ) {
		m_material->update( TEXTURE_ARRAYS );
		if ( m_pData ) {
			m_pData->invalidateGpuFarSurface();
		}
	}
}

void Terrain3D::refreshPerformanceProfileConformity( void ) {
	if ( m_bApplyingPerformanceTarget ) {
		return;
	}

	t3dPerformanceProfileStatus_t next_status = PERFORMANCE_PROFILE_CUSTOM;
	if ( m_performanceTarget == PERFORMANCE_TARGET_XBOX360_EXPERIMENTAL ) {
		next_status = PERFORMANCE_PROFILE_EXACT;
		for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
			const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
			Object *pOwner = this;
			if ( setting.m_owner == T3D_PROFILE_OWNER_MATERIAL ) {
				if ( m_material.is_null() ) {
					next_status = PERFORMANCE_PROFILE_MODIFIED;
					break;
				}
				pOwner = m_material.ptr();
			}
			const Variant current = pOwner->get( StringName( setting.m_pszProperty ) );
			if ( !t3dPerformanceProfileValueMatches( current, setting ) ) {
				next_status = PERFORMANCE_PROFILE_MODIFIED;
				break;
			}
		}
	}

	if ( m_performanceProfileStatus != next_status ) {
		m_performanceProfileStatus = next_status;
		notify_property_list_changed();
		emit_signal( "performance_profile_state_changed", ( int )( m_performanceProfileStatus ) );
	}
}

void Terrain3D::onProfileOwnedResourceChanged( void ) {
	markMemoryBudgetDirty();
	refreshPerformanceProfileConformity();
}

void Terrain3D::applyPerformanceProfileToTerrain( void ) {
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		if ( setting.m_owner != T3D_PROFILE_OWNER_TERRAIN ) {
			continue;
		}
		set( StringName( setting.m_pszProperty ), t3dPerformanceProfileValue( setting ) );
	}
}

void Terrain3D::setDataDirectory( const String dir ) {
	if ( m_dataDirectory == dir ) {
		if ( m_regionLightmapOutputDirectory.is_empty() && !dir.is_empty() ) {
			m_regionLightmapOutputDirectory = t3dDefaultRegionLightmapOutputDirectory( dir );
		}
		return;
	}

	// Replacing the repository can enumerate files, stage EASTL storage, load
	// Resources, tear down consumers, and rebuild fixed runtime state. Mark the
	// whole owner transition as a cold-rebuild phase for telemetry; backing-growth
	// legality remains with the repository and subsystem owners.
	Terrain3DColdPhaseScope cold_scope;
	const String old_dir = m_dataDirectory;
	const String old_lightmap_default = t3dDefaultRegionLightmapOutputDirectory( old_dir );
	const bool bFollowLightmapDefault =
	        m_regionLightmapOutputDirectory.is_empty() || m_regionLightmapOutputDirectory == old_lightmap_default;
	LOG( INFO,
	        "[data-transition] Requested data directory change: ",
	        old_dir,
	        " -> ",
	        dir,
	        "; initialized=",
	        m_bInitialized,
	        "; inside_tree=",
	        is_inside_tree(),
	        "; inside_world=",
	        m_bIsInsideWorld );

	if ( !m_pData ) {
		m_dataDirectory = dir;
		if ( bFollowLightmapDefault ) {
			m_regionLightmapOutputDirectory = t3dDefaultRegionLightmapOutputDirectory( dir );
		}
		initializeInternal();
		update_configuration_warnings();
		return;
	}

	const bool bHasRegionFiles = !dir.is_empty() && !Terrain3DUtil::getFiles( dir, "terrain3d*.res" ).is_empty();
	const bool bClearsRepository = !old_dir.is_empty() && dir.is_empty();
	const bool bReplacesRepository = bHasRegionFiles;
	if ( !bClearsRepository && !bReplacesRepository ) {
		// Assigning a new empty directory is a save-target change. Preserve the
		// current in-memory world instead of destroying it.
		m_dataDirectory = dir;
		if ( bFollowLightmapDefault ) {
			m_regionLightmapOutputDirectory = t3dDefaultRegionLightmapOutputDirectory( dir );
		}
		LOG( INFO, "[data-transition] Updated save target without replacing terrain data" );
		update_configuration_warnings();
		return;
	}

	const bool bWasInitialized = m_bInitialized;
	const bool bWasPhysicsProcessing = is_physics_processing();
	if ( bWasInitialized ) {
		// A directory replacement invalidates generated texture RIDs, region slot
		// mappings, collision shapes, streamed nodes, and instance buffers. None of
		// their live consumers may observe the repository half way through that
		// transition. In particular, the displacement compute path can have a queued
		// render-thread request that contains the old terrain texture RIDs.
		LOG( INFO, "[data-transition] Quiescing renderer, physics, streaming, and resource callbacks" );
		set_physics_process( false );
		m_bInitialized = false;
		destroyTerrainMesher();
		destroyOceanMesher();
		destroyHlodStreamer();
		destroyLightmapStreamer();
		destroyNodeStreamer( false );
		destroyInstancer();
		destroyCollision();
		destroyLabels();
		destroyMousePicking();
		destroyDisplacementBuffer();
		disconnectResourceSignals( m_assets, m_material );
		if ( m_assets.is_valid() ) {
			m_assets->uninitialize( this );
		}
		if ( m_material.is_valid() ) {
			m_material->uninitialize( this );
		}

		// Terrain3DGeneratedTexture replacement frees old renderer RIDs. Flush teardown
		// commands first so Direct3D/Vulkan backends cannot execute an old draw or
		// compute dispatch after the data object starts replacing those RIDs.
		Terrain3DRdContext::forceSync();
		LOG( INFO, "[data-transition] Runtime consumers quiesced; repository mutation may begin" );
	}

	Error transition_error = OK;
	if ( bClearsRepository ) {
		LOG( INFO, "[data-transition] Clearing active terrain repository" );
		m_pData->clear();
	} else {
		transition_error = m_pData->loadDirectory( dir );
	}

	if ( transition_error == OK ) {
		m_dataDirectory = dir;
		if ( bFollowLightmapDefault ) {
			m_regionLightmapOutputDirectory = t3dDefaultRegionLightmapOutputDirectory( dir );
		}
		LOG( INFO, "[data-transition] Repository mutation committed for ", dir );
	} else {
		LOG( ERROR, "[data-transition] Data directory change rejected; restoring previous terrain. Error: ", transition_error );
	}

	if ( bWasInitialized ) {
		LOG( INFO, "[data-transition] Recreating editor/runtime consumers from the committed repository" );
		initializeInternal();
		setupMousePicking();
		setupDisplacementBuffer();
		updateDisplacementBuffer();
		set_physics_process( bWasPhysicsProcessing );
		LOG( INFO,
		        "[data-transition] Consumer restore complete; initialized=",
		        m_bInitialized,
		        "; physics_process=",
		        is_physics_processing() );
	} else if ( transition_error == OK ) {
		// Preserve the old setter behavior for pre-tree/property-deserialization
		// calls: build whatever can be initialized at the current lifecycle stage.
		initializeInternal();
	}

	update_configuration_warnings();
}

void Terrain3D::setAutoShaderProfile( const Ref<Resource> &rProfile ) {
	if ( m_autoShaderProfile == rProfile ) {
		connectAutoShaderProfileSignal( m_autoShaderProfile );
		applyAutoShaderProfileMode();
		return;
	}

	disconnectAutoShaderProfileSignal( m_autoShaderProfile );
	m_autoShaderProfile = rProfile;
	connectAutoShaderProfileSignal( m_autoShaderProfile );
	applyAutoShaderProfileMode();
}

void Terrain3D::setAssets( const Ref<Terrain3DAssets> &rAssets ) {
	if ( !t3dDiffers( m_assets, rAssets ) ) {
		return;
	}
	LOG( INFO, "Replacing asset registry" );
	const bool bWasInitialized = m_bInitialized;
	const Ref<Terrain3DAssets> pOldAssets = m_assets;
	disconnectResourceSignals( pOldAssets, m_material );
	if ( bWasInitialized && m_pInstancer ) {
		// Clear render records while the old registry still owns their counts.
		m_pInstancer->destroy();
	}
	if ( pOldAssets.is_valid() ) {
		pOldAssets->uninitialize( this );
	}
	m_assets = rAssets;
	if ( m_assets.is_null() ) {
		m_assets.instantiate();
	}
	connectResourceSignals( m_assets, m_material );
	if ( bWasInitialized ) {
		m_assets->initialize( this );
		if ( m_pInstancer ) {
			m_pInstancer->initialize( this );
		}
		if ( m_material.is_valid() ) {
			m_material->update( FULL_REBUILD );
		}
	} else {
		initializeInternal();
	}
	emit_signal( "assets_changed" );
}

void Terrain3D::setEditor( Terrain3DEditor *const pEditor ) {
	if ( pEditor && pEditor->is_queued_for_deletion() ) {
		LOG( ERROR, "Attempted to set a node queued for deletion" );
		return;
	}
	SET_IF_DIFF( m_pEditor, pEditor );
	LOG( INFO, "Setting Terrain3DEditor: ", m_pEditor );
	if ( m_material.is_valid() ) {
		m_material->update( FULL_REBUILD );
	}
}

void Terrain3D::setPlugin( Object *const pPlugin ) {
	if ( pPlugin && pPlugin->is_queued_for_deletion() ) {
		LOG( ERROR, "Attempted to set a node queued for deletion" );
		return;
	}
	SET_IF_DIFF( m_pEditorPlugin, pPlugin );
	LOG( INFO, "Setting Editor Plugin: ", m_pEditorPlugin );
}

void Terrain3D::setSave16Bit( const bool bEnabled ) {
	SET_IF_DIFF( m_bSave16Bit, bEnabled );
	// Region persistence is now unconditionally compact R16. Retain the property
	// for scene compatibility, but do not dirty every region: bounded CPU residency
	// deliberately permits clean region resources to exist without map payloads.
	LOG( INFO, "Save heightmaps as 16-bit: ", m_bSave16Bit );
}
