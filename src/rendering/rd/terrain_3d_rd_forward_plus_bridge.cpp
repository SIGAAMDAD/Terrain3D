// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_forward_plus_bridge.h"

#include "core/logger.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

Terrain3DRdForwardPlusBridgeAdapter *Terrain3DRdForwardPlusBridgeAdapter::s_pSingleton = nullptr;

static bool t3dRdForwardReadRequiredInt( const Dictionary &rPacket,
		const StringName &rKey,
		int64_t &rValue,
		String &rError ) {
	if ( !rPacket.has( rKey ) ) {
		rError = String( "Forward+ bridge packet missing required field: " ) + String( rKey );
		return false;
	}
	const Variant value = rPacket[rKey];
	if ( value.get_type() != Variant::INT ) {
		rError = String( "Forward+ bridge packet field is not an integer: " ) + String( rKey );
		return false;
	}
	rValue = ( int64_t )( value );
	return true;
}

static RID t3dRdForwardReadRid( const Dictionary &rPacket, const StringName &rKey ) {
	if ( !rPacket.has( rKey ) ) return RID();
	const Variant value = rPacket[rKey];
	return value.get_type() == Variant::RID ? ( RID )( value ) : RID();
}

Terrain3DRdForwardPlusBridgeAdapter::Terrain3DRdForwardPlusBridgeAdapter( void ) {
	for ( std::atomic<Terrain3DRenderCoordinator *> &slot : m_szCoordinators ) {
		slot.store( nullptr, std::memory_order_relaxed );
	}
}

void Terrain3DRdForwardPlusBridgeAdapter::initializeSingleton( void ) {
	if ( s_pSingleton ) return;
	s_pSingleton = memnew( Terrain3DRdForwardPlusBridgeAdapter );
	s_pSingleton->connectEngineBridge();
}

void Terrain3DRdForwardPlusBridgeAdapter::shutdownSingleton( void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const pAdapter = s_pSingleton;
	if ( !pAdapter ) return;
	pAdapter->disconnectEngineBridge();
	for ( std::atomic<Terrain3DRenderCoordinator *> &slot : pAdapter->m_szCoordinators ) {
		slot.store( nullptr, std::memory_order_release );
	}
	s_pSingleton = nullptr;
	memdelete( pAdapter );
}

void Terrain3DRdForwardPlusBridgeAdapter::setError( const String &rError ) {
	std::lock_guard<std::mutex> lock( m_errorMutex );
	m_lastError = rError;
}

String Terrain3DRdForwardPlusBridgeAdapter::getLastError( void ) const {
	std::lock_guard<std::mutex> lock( m_errorMutex );
	return m_lastError;
}

bool Terrain3DRdForwardPlusBridgeAdapter::connectEngineBridge( void ) {
	const auto fail_connection = [this]( const String &rError ) {
		setError( rError );
		UtilityFunctions::push_error( "Terrain3D Forward+ bridge: ", rError );
		return false;
	};

	m_bBridgePresent.store( false, std::memory_order_release );
	m_bBridgeReady.store( false, std::memory_order_release );
	m_bCallbackRegistered.store( false, std::memory_order_release );
	m_nObservedAbiVersion.store( 0u, std::memory_order_release );
	m_pEngineBridge = nullptr;
	m_callback = Callable();

	Engine *const pEngine = Engine::get_singleton();
	const StringName singleton_name( s_pszEngineSingletonName );
	if ( !pEngine || !pEngine->has_singleton( singleton_name ) ) {
		return fail_connection( "Godot 4.7.1 Terrain3DForwardPlusBridge singleton is missing; production RenderingDevice rendering is disabled" );
	}
	m_bBridgePresent.store( true, std::memory_order_release );
	m_pEngineBridge = pEngine->get_singleton( singleton_name );
	if ( !m_pEngineBridge || !m_pEngineBridge->has_method( StringName( "get_abi_version" ) ) ||
			!m_pEngineBridge->has_method( StringName( "register_callback" ) ) ||
			!m_pEngineBridge->has_method( StringName( "unregister_callback" ) ) ||
			!m_pEngineBridge->has_method( StringName( "has_callback" ) ) ||
			!m_pEngineBridge->has_method( StringName( "collect_retired_callbacks" ) ) ) {
		return fail_connection( "Terrain3DForwardPlusBridge singleton does not expose the required F1 ABI methods" );
	}
	const Variant abi_value = m_pEngineBridge->call( StringName( "get_abi_version" ) );
	if ( abi_value.get_type() != Variant::INT ) {
		return fail_connection( "Terrain3DForwardPlusBridge returned a non-integer ABI version" );
	}
	const uint32_t abi = ( uint32_t )( MAX( ( int64_t )( abi_value ), int64_t( 0 ) ) );
	m_nObservedAbiVersion.store( abi, std::memory_order_release );
	if ( abi != s_nBridgeAbiVersion ) {
		return fail_connection( vformat( "Terrain3DForwardPlusBridge ABI mismatch: expected %d, received %d", s_nBridgeAbiVersion, abi ) );
	}
	m_callback = callable_mp( this, &Terrain3DRdForwardPlusBridgeAdapter::forwardPassCallback );
	const Variant registered_value = m_pEngineBridge->call( StringName( "register_callback" ), m_callback );
	const bool registered = registered_value.get_type() == Variant::BOOL && ( bool )( registered_value );
	m_bCallbackRegistered.store( registered, std::memory_order_release );
	if ( !registered ) {
		m_callback = Callable();
		return fail_connection( "Terrain3DForwardPlusBridge rejected the Terrain3D callback registration" );
	}
	const Variant has_callback_value = m_pEngineBridge->call( StringName( "has_callback" ) );
	if ( has_callback_value.get_type() != Variant::BOOL || !( bool )( has_callback_value ) ) {
		disconnectEngineBridge();
		return fail_connection( "Terrain3DForwardPlusBridge did not report the registered callback as available" );
	}
	m_bBridgeReady.store( true, std::memory_order_release );
	setError( String() );
	return true;
}

void Terrain3DRdForwardPlusBridgeAdapter::disconnectEngineBridge( void ) {
	m_bBridgeReady.store( false, std::memory_order_release );
	Object *const pBridge = m_pEngineBridge;
	const Callable callback = m_callback;
	if ( pBridge && m_bCallbackRegistered.exchange( false, std::memory_order_acq_rel ) && callback.is_valid() ) {
		pBridge->call( StringName( "unregister_callback" ), callback );
		RenderingServer *const pServer = RenderingServer::get_singleton();
		if ( pServer && !pServer->is_on_render_thread() ) pServer->force_sync();
		if ( pBridge->has_method( StringName( "collect_retired_callbacks" ) ) ) {
			pBridge->call( StringName( "collect_retired_callbacks" ) );
		}
	}
	m_callback = Callable();
	m_pEngineBridge = nullptr;
}

bool Terrain3DRdForwardPlusBridgeAdapter::attachCoordinator( Terrain3DRenderCoordinator *const pCoordinator ) {
	if ( !pCoordinator ) return false;
	for ( std::atomic<Terrain3DRenderCoordinator *> &slot : m_szCoordinators ) {
		if ( slot.load( std::memory_order_acquire ) == pCoordinator ) return true;
	}
	for ( std::atomic<Terrain3DRenderCoordinator *> &slot : m_szCoordinators ) {
		Terrain3DRenderCoordinator *pExpected = nullptr;
		if ( slot.compare_exchange_strong( pExpected, pCoordinator, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
			return true;
		}
	}
	setError( "Terrain3D Forward+ bridge coordinator slot capacity exhausted" );
	return false;
}

void Terrain3DRdForwardPlusBridgeAdapter::detachCoordinator( Terrain3DRenderCoordinator *const pCoordinator ) {
	if ( !pCoordinator ) return;
	for ( std::atomic<Terrain3DRenderCoordinator *> &slot : m_szCoordinators ) {
		Terrain3DRenderCoordinator *pExpected = pCoordinator;
		slot.compare_exchange_strong( pExpected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire );
	}
	// A callback that loaded the coordinator pointer before the exchange must
	// finish before the coordinator can be destroyed.
	RenderingServer *const pServer = RenderingServer::get_singleton();
	if ( pServer && !pServer->is_on_render_thread() ) pServer->force_sync();
}

bool Terrain3DRdForwardPlusBridgeAdapter::parsePacket(
		const Dictionary &rPacket, t3dRdForwardPassContext_t &rContext, String &rError ) {
	rContext = t3dRdForwardPassContext_t{};
	rError = String();
	int64_t value = 0;
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "bridge_abi" ), value, rError ) ) return false;
	if ( value != ( int64_t )( s_nBridgeAbiVersion ) ) {
		rError = vformat( "Forward+ bridge packet ABI mismatch: expected %d, received %d", s_nBridgeAbiVersion, value );
		return false;
	}
	rContext.m_nBridgeAbiVersion = ( uint32_t )( value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "phase" ), value, rError ) ||
			value < T3D_RD_FORWARD_PHASE_PREPARE || value > T3D_RD_FORWARD_PHASE_SUBMIT ) {
		if ( rError.is_empty() ) rError = "Forward+ bridge packet has an invalid callback phase";
		return false;
	}
	rContext.m_phase = ( t3dRdForwardCallbackPhase_t )( value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "pass_kind" ), value, rError ) ) return false;
	if ( value <= T3D_RD_FORWARD_PASS_INVALID || value >= T3D_RD_FORWARD_PASS_MAX ) {
		rError = vformat( "Forward+ bridge packet has unsupported pass kind: %d", value );
		return false;
	}
	rContext.m_passKind = ( t3dRdForwardPassKind_t )( value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "native_pass_mode" ), value, rError ) ||
			value < T3D_RD_FORWARD_NATIVE_PASS_COLOR || value > T3D_RD_FORWARD_NATIVE_PASS_SDF ) {
		if ( rError.is_empty() ) rError = "Forward+ bridge packet has an invalid native pass mode";
		return false;
	}
	rContext.m_nativePassMode = ( t3dRdForwardNativePassMode_t )( value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "draw_list" ), value, rError ) ||
			( rContext.m_phase == T3D_RD_FORWARD_PHASE_SUBMIT && value < 0 ) ) {
		if ( rError.is_empty() ) rError = "Forward+ bridge packet has an invalid native draw-list ID";
		return false;
	}
	rContext.m_nDrawList = value;
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "framebuffer_format" ), value, rError ) || value < 0 ) {
		if ( rError.is_empty() ) rError = "Forward+ bridge packet has an invalid framebuffer format ID";
		return false;
	}
	rContext.m_nFramebufferFormat = value;
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "view_count" ), value, rError ) || value < 1 ||
			value > ( int64_t )( s_nRdForwardMaxViews ) ) {
		if ( rError.is_empty() ) rError = vformat( "Forward+ bridge packet view count is outside [1, %d]", s_nRdForwardMaxViews );
		return false;
	}
	rContext.m_nViewCount = ( uint32_t )( value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "scene_pass" ), value, rError ) || value < 0 ) return false;
	rContext.m_nScenePass = ( uint64_t )( value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "color_pass_flags" ), value, rError ) || value < 0 ) return false;
	rContext.m_nColorPassFlags = ( uint32_t )( value );
	const Variant depth_prepass_value = rPacket.get( StringName( "depth_prepass_active" ), Variant() );
	if ( depth_prepass_value.get_type() != Variant::BOOL ) {
		rError = "Forward+ bridge packet depth_prepass_active is missing or malformed";
		return false;
	}
	rContext.m_bDepthPrepassActive = ( bool )( depth_prepass_value );
	if ( !t3dRdForwardReadRequiredInt( rPacket, StringName( "camera_visible_layers" ), value, rError ) || value < 0 ) return false;
	rContext.m_nCameraVisibleLayers = ( uint32_t )( value );

	const Variant region_value = rPacket.get( StringName( "render_region" ), Variant() );
	if ( region_value.get_type() != Variant::RECT2I ) {
		rError = "Forward+ bridge packet render_region is missing or malformed";
		return false;
	}
	rContext.m_renderRegion = ( Rect2i )( region_value );
	const Variant size_value = rPacket.get( StringName( "render_size" ), Variant() );
	if ( size_value.get_type() != Variant::VECTOR2I ) {
		rError = "Forward+ bridge packet render_size is missing or malformed";
		return false;
	}
	rContext.m_renderSize = ( Vector2i )( size_value );
	const Variant reverse_cull_value = rPacket.get( StringName( "reverse_cull" ), Variant() );
	const Variant reflection_value = rPacket.get( StringName( "reflection_probe_render" ), Variant() );
	if ( reverse_cull_value.get_type() != Variant::BOOL || reflection_value.get_type() != Variant::BOOL ) {
		rError = "Forward+ bridge packet pass-state booleans are missing or malformed";
		return false;
	}
	rContext.m_bReverseCull = ( bool )( reverse_cull_value );
	rContext.m_bReflectionProbeRender = ( bool )( reflection_value );

	const Variant camera_value = rPacket.get( StringName( "camera_transform" ), Variant() );
	const Variant previous_camera_value = rPacket.get( StringName( "previous_camera_transform" ), Variant() );
	if ( camera_value.get_type() != Variant::TRANSFORM3D || previous_camera_value.get_type() != Variant::TRANSFORM3D ) {
		rError = "Forward+ bridge packet camera transforms are missing or malformed";
		return false;
	}
	rContext.m_cameraTransform = ( Transform3D )( camera_value );
	rContext.m_previousCameraTransform = ( Transform3D )( previous_camera_value );

	const Variant current_views_value = rPacket.get( StringName( "view_projections" ), Variant() );
	const Variant previous_views_value = rPacket.get( StringName( "previous_view_projections" ), Variant() );
	const Variant eye_offsets_value = rPacket.get( StringName( "eye_offsets" ), Variant() );
	if ( current_views_value.get_type() != Variant::ARRAY || previous_views_value.get_type() != Variant::ARRAY ||
			eye_offsets_value.get_type() != Variant::ARRAY ) {
		rError = "Forward+ bridge packet view arrays are missing or malformed";
		return false;
	}
	const Array current_views = current_views_value;
	const Array previous_views = previous_views_value;
	const Array eye_offsets = eye_offsets_value;
	if ( current_views.size() < ( int64_t )( rContext.m_nViewCount ) ||
			previous_views.size() < ( int64_t )( rContext.m_nViewCount ) ||
			eye_offsets.size() < ( int64_t )( rContext.m_nViewCount ) ) {
		rError = "Forward+ bridge packet view arrays are shorter than view_count";
		return false;
	}
	for ( uint32_t i = 0u; i < rContext.m_nViewCount; ++i ) {
		if ( current_views[( int64_t )( i )].get_type() != Variant::PROJECTION ||
				previous_views[( int64_t )( i )].get_type() != Variant::PROJECTION ||
				eye_offsets[( int64_t )( i )].get_type() != Variant::VECTOR3 ) {
			rError = vformat( "Forward+ bridge packet view %d has malformed projection/stereo state", i );
			return false;
		}
		rContext.m_szViewProjection[i] = ( Projection )( current_views[( int64_t )( i )] );
		rContext.m_szPreviousViewProjection[i] = ( Projection )( previous_views[( int64_t )( i )] );
		rContext.m_szEyeOffset[i] = ( Vector3 )( eye_offsets[( int64_t )( i )] );
	}

	if ( t3dRdForwardReadRequiredInt( rPacket, StringName( "cluster_size" ), value, rError ) && value >= 0 ) {
		rContext.m_nClusterSize = ( uint32_t )( value );
	} else {
		return false;
	}
	if ( t3dRdForwardReadRequiredInt( rPacket, StringName( "cluster_max_elements" ), value, rError ) && value >= 0 ) {
		rContext.m_nClusterMaxElements = ( uint32_t )( value );
	} else {
		return false;
	}

	rContext.m_hNativeFramebuffer = t3dRdForwardReadRid( rPacket, StringName( "native_framebuffer" ) );
	rContext.m_hDirectionalLightBuffer = t3dRdForwardReadRid( rPacket, StringName( "directional_light_buffer" ) );
	rContext.m_hOmniLightBuffer = t3dRdForwardReadRid( rPacket, StringName( "omni_light_buffer" ) );
	rContext.m_hSpotLightBuffer = t3dRdForwardReadRid( rPacket, StringName( "spot_light_buffer" ) );
	rContext.m_hAreaLightBuffer = t3dRdForwardReadRid( rPacket, StringName( "area_light_buffer" ) );
	rContext.m_hClusterBuffer = t3dRdForwardReadRid( rPacket, StringName( "cluster_buffer" ) );
	rContext.m_hShadowAtlasTexture = t3dRdForwardReadRid( rPacket, StringName( "shadow_atlas_texture" ) );
	rContext.m_hDirectionalShadowAtlasTexture = t3dRdForwardReadRid( rPacket, StringName( "directional_shadow_atlas_texture" ) );
	rContext.m_hNativeShadowSampler = t3dRdForwardReadRid( rPacket, StringName( "shadow_sampler" ) );
	rContext.m_hLightmapBuffer = t3dRdForwardReadRid( rPacket, StringName( "lightmap_buffer" ) );
	rContext.m_hLightmapCaptureBuffer = t3dRdForwardReadRid( rPacket, StringName( "lightmap_capture_buffer" ) );
	rContext.m_hReflectionProbeBuffer = t3dRdForwardReadRid( rPacket, StringName( "reflection_probe_buffer" ) );
	rContext.m_hReflectionAtlasTexture = t3dRdForwardReadRid( rPacket, StringName( "reflection_atlas_texture" ) );
	rContext.m_hRadianceTexture = t3dRdForwardReadRid( rPacket, StringName( "radiance_texture" ) );
	rContext.m_hDecalBuffer = t3dRdForwardReadRid( rPacket, StringName( "decal_buffer" ) );
	rContext.m_hDecalAtlasTexture = t3dRdForwardReadRid( rPacket, StringName( "decal_atlas_texture" ) );
	rContext.m_hDecalAtlasSrgbTexture = t3dRdForwardReadRid( rPacket, StringName( "decal_atlas_srgb_texture" ) );
	rContext.m_hAreaLightAtlasTexture = t3dRdForwardReadRid( rPacket, StringName( "area_light_atlas_texture" ) );
	rContext.m_hBaseUniformSet = t3dRdForwardReadRid( rPacket, StringName( "base_uniform_set" ) );
	rContext.m_hRenderPassUniformSet = t3dRdForwardReadRid( rPacket, StringName( "render_pass_uniform_set" ) );
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::forwardPassCallback( const Dictionary &rPacket ) {
	m_nPacketsReceived.fetch_add( 1u, std::memory_order_relaxed );
	t3dRdForwardPassContext_t context;
	String error;
	if ( !parsePacket( rPacket, context, error ) ) {
		m_nPacketsRejected.fetch_add( 1u, std::memory_order_relaxed );
		setError( error );
		return false;
	}
	m_nPacketsAccepted.fetch_add( 1u, std::memory_order_relaxed );
	m_nLastScenePass.store( context.m_nScenePass, std::memory_order_release );
	m_nLastDrawList.store( context.m_nDrawList, std::memory_order_release );
	m_nLastPassKind.store( ( uint32_t )( context.m_passKind ), std::memory_order_release );
	for ( std::atomic<Terrain3DRenderCoordinator *> &slot : m_szCoordinators ) {
		Terrain3DRenderCoordinator *const pCoordinator = slot.load( std::memory_order_acquire );
		if ( pCoordinator ) pCoordinator->renderForwardPass( context );
	}
	return true;
}

Dictionary Terrain3DRdForwardPlusBridgeAdapter::getDiagnostics( void ) const {
	Dictionary result;
	result["singleton_name"] = s_pszEngineSingletonName;
	result["required_abi"] = ( int64_t )( s_nBridgeAbiVersion );
	result["observed_abi"] = ( int64_t )( getObservedAbiVersion() );
	result["bridge_present"] = isBridgePresent();
	result["bridge_ready"] = isBridgeReady();
	result["callback_registered"] = isCallbackRegistered();
	result["packets_received"] = ( int64_t )( m_nPacketsReceived.load( std::memory_order_acquire ) );
	result["packets_accepted"] = ( int64_t )( m_nPacketsAccepted.load( std::memory_order_acquire ) );
	result["packets_rejected"] = ( int64_t )( m_nPacketsRejected.load( std::memory_order_acquire ) );
	result["last_scene_pass"] = ( int64_t )( m_nLastScenePass.load( std::memory_order_acquire ) );
	result["last_draw_list"] = m_nLastDrawList.load( std::memory_order_acquire );
	result["last_pass_kind"] = ( int64_t )( m_nLastPassKind.load( std::memory_order_acquire ) );
	result["last_error"] = getLastError();
	return result;
}
