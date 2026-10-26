// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/forward/terrain_3d_rd_forward_plus_bridge.h"
#include "core/utility/terrain_3d_sync.h"
#include "diagnostics/core/terrain_3d_streaming_profiler.h"

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/core/terrain_3d_render_coordinator.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#if defined( _MSC_VER )
#include <intrin.h>
#endif

Terrain3DRdForwardPlusBridgeAdapter *Terrain3DRdForwardPlusBridgeAdapter::s_pSingleton = NULL;

static bool t3dRdForwardValidSampleCount(
	const uint32_t nSamples ) {
	return nSamples == 1u || nSamples == 2u || nSamples == 4u || nSamples == 8u;
}

static uint32_t t3dRdForwardCtz64(
	const uint64_t nMask ) {
#if defined( _MSC_VER )
	unsigned long index = 0u;
	_BitScanForward64( &index, nMask );
	return ( uint32_t ) ( index );
#else
	return ( uint32_t ) ( __builtin_ctzll( nMask ) );
#endif
}

static RID t3dRdForwardDecodeRid(
	const uint64_t nOpaque ) {
	static_assert( sizeof( RID ) == sizeof( uint64_t ), "Pinned godot-cpp RID native ABI changed" );
	static_assert( alignof( RID ) >= alignof( uint64_t ), "Pinned godot-cpp RID native alignment changed" );
	RID result;
	// Godot-cpp exposes the native representation through _native_ptr().
	// Do not overwrite the C++ RID object, whose copy operations are nontrivial.
	memcpy( result._native_ptr(), &nOpaque, sizeof( nOpaque ) );
	return result;
}

static Projection t3dRdForwardDecodeProjection(
	const float *const pValues ) {
	Projection result;
	for ( uint32_t column = 0u; column < 4u; ++column ) {
		for ( uint32_t row = 0u; row < 4u; ++row ) {
			result.columns[column][row] = pValues[column * 4u + row];
		}
	}
	return result;
}

static Transform3D t3dRdForwardDecodeTransform(
	const float *const pValues ) {
	const Basis basis( pValues[0],
					   pValues[1],
					   pValues[2],
					   pValues[4],
					   pValues[5],
					   pValues[6],
					   pValues[8],
					   pValues[9],
					   pValues[10] );
	return Transform3D( basis, Vector3( pValues[3], pValues[7], pValues[11] ) );
}

static bool t3dRdForwardFiniteEnvironmentFrame(
	const t3dRdForwardBridgeFramePacket_t &rFrame ) {
	if ( rFrame.m_nAbi10ReservedTail != 0u || rFrame.m_nEnvironmentReserved0 != 0u ||
		 rFrame.m_fEnvironmentReserved1 != 0.0f || ( rFrame.m_nEnvironmentFlags & ~uint32_t( 0x0fu ) ) != 0u ||
		 !isfinite( rFrame.m_fAmbientColorSkyMix ) || rFrame.m_fAmbientColorSkyMix < 0.0f ||
		 rFrame.m_fAmbientColorSkyMix > 1.0f || !isfinite( rFrame.m_fEmissiveExposureNormalization ) ||
		 !isfinite( rFrame.m_fIblExposureNormalization ) || !isfinite( rFrame.m_fRadiancePixelSize ) ||
		 rFrame.m_fRadiancePixelSize < 0.0f || !isfinite( rFrame.m_fRadianceBorderSize ) ||
		 rFrame.m_fRadianceBorderSize < 0.0f ) {
		return false;
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		if ( !isfinite( rFrame.m_szAmbientLightColorEnergy[i] ) ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < 12u; ++i ) {
		if ( !isfinite( rFrame.m_szRadianceInverseXform[i] ) ) {
			return false;
		}
	}
	return true;
}

static bool t3dRdForwardOrdinaryPassKind(
	const uint32_t nPassKind ) {
	return nPassKind == T3D_RD_FORWARD_PASS_DEPTH || nPassKind == T3D_RD_FORWARD_PASS_OPAQUE ||
		   nPassKind == T3D_RD_FORWARD_PASS_MOTION || nPassKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
}

Terrain3DRdForwardPlusBridgeAdapter::Terrain3DRdForwardPlusBridgeAdapter(
	void ) {
	memset( m_szCoordinators, 0, sizeof( m_szCoordinators ) );
	resetConnectionState();
}

void Terrain3DRdForwardPlusBridgeAdapter::initializeSingleton(
	void ) {
	if ( s_pSingleton ) {
		return;
	}
	// Scene-extension registration precedes register_server_singletons().
	// Defer RD discovery and staging setup until the first terrain attaches.
	s_pSingleton = memnew( Terrain3DRdForwardPlusBridgeAdapter );
}

void Terrain3DRdForwardPlusBridgeAdapter::shutdownSingleton(
	void ) {
	Terrain3DRdForwardPlusBridgeAdapter *const adapter = s_pSingleton;
	if ( !adapter ) {
		return;
	}
	// disconnectEngineBridge() unregisters the callback and synchronizes Godot's
	// render thread before retired engine callback storage is collected. No second
	// force_sync is required before clearing the cold coordinator table.
	adapter->disconnectEngineBridge();
	ERR_FAIL_COND_MSG( Terrain3DRdContext::getShared().hotPending(),
					   "Cannot destroy bridge while its HOT callback is pending." );
	adapter->m_nCoordinatorMask.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	{
		Terrain3DScopedLock lock( adapter->m_coordinatorMutex );
		memset( adapter->m_szCoordinators, 0, sizeof( adapter->m_szCoordinators ) );
	}
	adapter->m_nPreparedCoordinatorMask = 0u;
	s_pSingleton = NULL;
	memdelete( adapter );
}

void Terrain3DRdForwardPlusBridgeAdapter::resetConnectionState(
	void ) {
	m_bBridgePresent.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bBridgeReady.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_bCallbackRegistered.store( false, T3D_MEMORY_ORDER_RELEASE );
	m_nObservedAbiVersion.store( 0u, T3D_MEMORY_ORDER_RELEASE );
	m_nObservedCapabilities.store( 0u, T3D_MEMORY_ORDER_RELEASE );
    m_br24FragmentSubgroup = t3dBr24SubgroupCapabilities_t{};
    m_bBr24SubgroupRequested = false;

	m_pEngineBridge = NULL;
	m_nPrepareAddress = ( uintptr_t ) ( &Terrain3DRdForwardPlusBridgeAdapter::prepareThunk );
	m_nShadowPrepareAddress = ( uintptr_t ) ( &Terrain3DRdForwardPlusBridgeAdapter::prepareShadowThunk );
	m_nSubmitAddress = ( uintptr_t ) ( &Terrain3DRdForwardPlusBridgeAdapter::submitThunk );
	m_nUserdataAddress = ( uintptr_t ) ( this );
	m_nCachedFrameGeneration = 0u;
	m_nCachedResourceGeneration = 0u;
	m_nCachedPassSerial = 0u;
	m_nPreparedCoordinatorMask = 0u;
	m_mainCallbackContext = t3dRdForwardPassContext_t{};
	m_shadowCallbackContext = t3dRdForwardPassContext_t{};
	m_pPreparedContext = NULL;
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	m_nPacketsReceived = 0u;
	m_nPacketsAccepted = 0u;
	m_nPacketsRejected = 0u;
	m_nLastScenePass = 0u;
	m_nLastDrawList = -1;
	m_nLastPassKind = T3D_RD_FORWARD_PASS_INVALID;
#endif
}

void Terrain3DRdForwardPlusBridgeAdapter::setError(
	const String &rError ) {
	Terrain3DScopedLock lock( m_errorMutex );
	m_lastError = rError;
}

bool Terrain3DRdForwardPlusBridgeAdapter::failConnection(
	const String &rError ) {
	setError( rError );
	UtilityFunctions::push_error( "Terrain3D Forward+ bridge: ", rError );
	return false;
}

String Terrain3DRdForwardPlusBridgeAdapter::getLastError(
	void ) const {
	Terrain3DScopedLock lock( m_errorMutex );
	return m_lastError;
}

bool Terrain3DRdForwardPlusBridgeAdapter::connectEngineBridge(
	void ) {
	resetConnectionState();
	Engine *const engine = Engine::get_singleton();
	if ( !engine || !engine->has_singleton( StringName( "RenderingServer" ) ) ) {
		setError( "Terrain3D Forward+ setup is waiting for RenderingServer registration" );
		return false;
	}
	const StringName singleton_name( s_pszEngineSingletonName );
	if ( !engine || !engine->has_singleton( singleton_name ) ) {
		return failConnection( "Godot 4.7.2 Terrain3DForwardPlusBridge singleton is missing; production "
							   "RenderingDevice rendering is disabled" );
	}
	m_bBridgePresent.store( true, T3D_MEMORY_ORDER_RELEASE );
	m_pEngineBridge = engine->get_singleton( singleton_name );
	static const char *const required_methods[] = { "get_abi_version",
													"get_native_capabilities",
													"register_native_callback",
													"unregister_native_callback",
													"has_native_callback",
													"collect_retired_callbacks",
													"get_native_device_frame",
													"complete_native_hot_frame",
													"get_deferred_upload_abi_version",
													"defer_native_buffer_update" };
	for ( size_t iMethod = 0u; iMethod < T3D_ARRAY_COUNT( required_methods ); iMethod++ ) {
		const char *const method = ( required_methods )[iMethod];
		if ( !m_pEngineBridge || !m_pEngineBridge->has_method( StringName( method ) ) ) {
			return failConnection(
				String( "Terrain3DForwardPlusBridge singleton is missing required ABI 12 method: " ) + method );
		}
	}

	const Variant abi_value = m_pEngineBridge->call( StringName( "get_abi_version" ) );
	if ( abi_value.get_type() != Variant::INT ) {
		return failConnection( "Terrain3DForwardPlusBridge returned a non-integer ABI version" );
	}
	const int64_t  abi_signed = ( int64_t ) ( abi_value );
	const uint32_t abi = abi_signed >= 0 ? ( uint32_t ) ( abi_signed ) : 0u;
	m_nObservedAbiVersion.store( abi, T3D_MEMORY_ORDER_RELEASE );
	if ( abi != T3D_BRIDGE_ABI_VERSION ) {
		return failConnection( vformat( "Terrain3DForwardPlusBridge ABI mismatch: expected %d, received %d",
										T3D_BRIDGE_ABI_VERSION,
										abi ) );
	}

	const Variant capabilities_value = m_pEngineBridge->call( StringName( "get_native_capabilities" ) );
	if ( capabilities_value.get_type() != Variant::INT ) {
		return failConnection( "Terrain3DForwardPlusBridge returned a non-integer capability mask" );
	}
	const uint64_t capabilities = ( uint64_t ) ( ( int64_t ) ( capabilities_value ) );
	m_nObservedCapabilities.store( capabilities, T3D_MEMORY_ORDER_RELEASE );
    // BR18: optional cold Vulkan subgroup-properties handshake. Godot's
    // RenderingDevice API does not expose supported subgroup operations;
    // an engine bridge must explicitly report the Vulkan properties.
    // Missing/partial capability evidence keeps the experiment disabled.
    OS *const pOS = OS::get_singleton();
    m_bBr24SubgroupRequested = pOS &&
        pOS->get_environment( "TERRAIN3D_FRAGMENT_SUBGROUP_EXPERIMENT" ) == "1";
    if ( m_bBr24SubgroupRequested &&
         m_pEngineBridge->has_method( StringName( "get_fragment_subgroup_properties" ) ) ) {
        const Variant properties = m_pEngineBridge->call( StringName( "get_fragment_subgroup_properties" ) );
        if ( properties.get_type() == Variant::DICTIONARY ) {
            const Dictionary values = ( Dictionary ) properties;
            if ( values.has( "supported_stages" ) && values.has( "supported_operations" ) &&
                 values.has( "subgroup_size" ) &&
                 values["supported_stages"].get_type() == Variant::INT &&
                 values["supported_operations"].get_type() == Variant::INT &&
                 values["subgroup_size"].get_type() == Variant::INT ) {
                const int64_t stages = ( int64_t ) ( values["supported_stages"] );
                const int64_t operations = ( int64_t ) ( values["supported_operations"] );
                const int64_t size = ( int64_t ) ( values["subgroup_size"] );
                if ( stages >= 0 && stages <= UINT32_MAX &&
                     operations >= 0 && operations <= UINT32_MAX &&
                     size > 0 && size <= 128 ) {
                    m_br24FragmentSubgroup.m_nStageMask = ( uint32_t ) stages;
                    m_br24FragmentSubgroup.m_nOperationMask = ( uint32_t ) operations;
                    m_br24FragmentSubgroup.m_nSubgroupSize = ( uint32_t ) size;
                    m_br24FragmentSubgroup.m_nPropertiesValid = 1u;
                }
            }
        }
    }

	if ( ( capabilities & T3D_FORWARD_BRIDGE_REQUIRED_CAPABILITIES ) != T3D_FORWARD_BRIDGE_REQUIRED_CAPABILITIES ) {
		return failConnection(
			vformat( "Terrain3DForwardPlusBridge capability mismatch: required 0x%llx, received 0x%llx",
					 T3D_FORWARD_BRIDGE_REQUIRED_CAPABILITIES,
					 capabilities ) );
	}

	// Ordinary discovery/configuration allocates the fixed staging pool and
	// therefore must precede registration. Persistent ABI 2 allocates its CPU
	// dirty-range mirror in the context instead of an engine staging pool.
	RenderingDevice *const rd = Terrain3DRdContext::getGlobalDevice();
	const int64_t		   persistent_abi = m_pEngineBridge->has_method( "get_persistent_hot_abi_version" )
												? int64_t( m_pEngineBridge->call( "get_persistent_hot_abi_version" ) )
												: 0;
	const bool			   persistent_hot =
		rd && Terrain3DRdGpuBuffer::supportsPersistentMapping( rd ) &&
		m_pEngineBridge->has_method( "get_native_hot_backing_size" ) &&
		( ( persistent_abi == 1 && m_pEngineBridge->has_method( "defer_native_buffer_flush" ) ) ||
		  ( persistent_abi == 2 && m_pEngineBridge->has_method( "defer_native_buffer_flush_ranges" ) &&
			m_pEngineBridge->has_method( "get_persistent_hot_frame_slot" ) ) );
	if ( !persistent_hot ) {
		if ( m_pEngineBridge->has_method( "configure_native_hot_staging" ) &&
			 !bool( m_pEngineBridge->call( "configure_native_hot_staging",
										   int64_t( Terrain3DRdContext::HOT_FRAME_BYTES ) ) ) ) {
			return failConnection(
				"Godot could not configure the fixed HOT staging pool before callback registration" );
		}
		const Variant deferred_abi = m_pEngineBridge->call( "get_deferred_upload_abi_version" );
		if ( deferred_abi.get_type() != Variant::INT ||
			 ( int64_t( deferred_abi ) != 1 &&
			   !( int64_t( deferred_abi ) == 2 &&
				  m_pEngineBridge->has_method( "defer_native_buffer_update_ranges" ) ) ) ) {
			return failConnection( "Godot deferred HOT upload ABI 1 or range-upload ABI 2 is required" );
		}
	}

	const Variant registered_value =
		m_pEngineBridge->call( StringName( "register_native_callback" ),
							   ( int64_t ) ( m_nPrepareAddress ),
							   ( int64_t ) ( m_nShadowPrepareAddress ),
							   ( int64_t ) ( m_nSubmitAddress ),
							   ( int64_t ) ( m_nUserdataAddress ),
							   ( int64_t ) ( sizeof( t3dRdForwardBridgeFramePacket_t ) ),
							   ( int64_t ) ( sizeof( t3dRdForwardBridgeResourcePacket_t ) ),
							   ( int64_t ) ( sizeof( t3dRdForwardBridgePassPacket_t ) ),
							   ( int64_t ) ( sizeof( t3dRdForwardBridgeShadowPacket_t ) ) );
	const bool registered = registered_value.get_type() == Variant::BOOL && ( bool ) ( registered_value );
	m_bCallbackRegistered.store( registered, T3D_MEMORY_ORDER_RELEASE );
	if ( !registered ) {
		return failConnection( "Terrain3DForwardPlusBridge rejected the ABI 12 native callback registration" );
	}

	const Variant has_callback_value = m_pEngineBridge->call( StringName( "has_native_callback" ) );
	if ( has_callback_value.get_type() != Variant::BOOL || !( bool ) ( has_callback_value ) ) {
		disconnectEngineBridge();
		return failConnection(
			"Terrain3DForwardPlusBridge did not report the registered native callback as available" );
	}
	m_bBridgeReady.store( true, T3D_MEMORY_ORDER_RELEASE );
	setError( String() );
	return true;
}

void Terrain3DRdForwardPlusBridgeAdapter::disconnectEngineBridge(
	void ) {
	if ( Terrain3DRdContext::getShared().hotPending() && m_pEngineBridge &&
		 m_pEngineBridge->has_method( "complete_native_hot_frame" ) ) {
		m_pEngineBridge->call( "complete_native_hot_frame" );
	}
	ERR_FAIL_COND_MSG( Terrain3DRdContext::getShared().hotPending(),
					   "Pending HOT finalizer must complete before bridge detach." );
	m_bBridgeReady.store( false, T3D_MEMORY_ORDER_RELEASE );
	Object *const bridge = m_pEngineBridge;
	if ( bridge && m_bCallbackRegistered.exchange( false, T3D_MEMORY_ORDER_ACQ_REL ) ) {
		bridge->call( StringName( "unregister_native_callback" ),
					  ( int64_t ) ( m_nPrepareAddress ),
					  ( int64_t ) ( m_nShadowPrepareAddress ),
					  ( int64_t ) ( m_nSubmitAddress ),
					  ( int64_t ) ( m_nUserdataAddress ) );
		Terrain3DRdContext::forceSync();
		bridge->call( StringName( "collect_retired_callbacks" ) );
	}
	m_pPreparedContext = NULL;
	m_nCachedPassSerial = 0u;
	m_nPreparedCoordinatorMask = 0u;
	m_pEngineBridge = NULL;
	m_bBridgePresent.store( false, T3D_MEMORY_ORDER_RELEASE );
}

bool Terrain3DRdForwardPlusBridgeAdapter::attachCoordinator(
	Terrain3DRenderCoordinator *const pCoordinator ) {
	if ( !pCoordinator ) {
		return false;
	}
	Terrain3DScopedLock lock( m_coordinatorMutex );
	if ( !isBridgeReady() && !connectEngineBridge() ) {
		return false;
	}
	for ( uint32_t i = 0u; i < T3D_MAX_COORDINATORS; ++i ) {
		if ( m_szCoordinators[i] == pCoordinator ) {
			return true;
		}
	}
	for ( uint32_t i = 0u; i < T3D_MAX_COORDINATORS; ++i ) {
		if ( !m_szCoordinators[i] ) {
			m_szCoordinators[i] = pCoordinator;
			// Publish the fully initialized pointer with one release operation. The
			// render thread performs one acquire load of this mask per PREPARE.
			m_nCoordinatorMask.fetch_or( uint64_t( 1u ) << i, T3D_MEMORY_ORDER_RELEASE );
			return true;
		}
	}
	setError( "Terrain3D Forward+ bridge coordinator slot capacity exhausted" );
	return false;
}

void Terrain3DRdForwardPlusBridgeAdapter::detachCoordinator(
	Terrain3DRenderCoordinator *const pCoordinator ) {
	if ( !pCoordinator ) {
		return;
	}
	uint64_t removed_mask = 0u;
	{
		Terrain3DScopedLock lock( m_coordinatorMutex );
		for ( uint32_t i = 0u; i < T3D_MAX_COORDINATORS; ++i ) {
			if ( m_szCoordinators[i] == pCoordinator ) {
				removed_mask |= uint64_t( 1u ) << i;
			}
		}
		if ( removed_mask == 0u ) {
			return;
		}
		// Stop publishing the coordinator before waiting for the render thread.
		m_nCoordinatorMask.fetch_and( ~removed_mask, T3D_MEMORY_ORDER_ACQ_REL );
	}

	// A PREPARE that acquired the old mask may still be in flight and its paired
	// SUBMIT intentionally reuses that same mask. Synchronize before clearing the
	// pointer or allowing the coordinator to be destroyed.
	Terrain3DRdContext::forceSync();
	{
		Terrain3DScopedLock lock( m_coordinatorMutex );
		while ( removed_mask != 0u ) {
			const uint32_t index = t3dRdForwardCtz64( removed_mask );
			removed_mask &= removed_mask - 1u;
			if ( m_szCoordinators[index] == pCoordinator ) {
				m_szCoordinators[index] = NULL;
			}
		}
	}
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertFrame(
	const t3dRdForwardBridgeFramePacket_t &rFrame ) {
	if ( rFrame.m_nAbi != T3D_BRIDGE_ABI_VERSION || rFrame.m_nStructSize != sizeof( rFrame ) ||
		 rFrame.m_nGeneration == 0u ) {
		setError( "Terrain3D ABI 12 frame packet validation failed" );
		return false;
	}
	if ( rFrame.m_nGeneration == m_nCachedFrameGeneration ) {
		return true;
	}
	if ( rFrame.m_nViewCount == 0u || rFrame.m_nViewCount > T3D_FORWARD_BRIDGE_MAX_VIEWS ||
		 !t3dRdForwardValidSampleCount( rFrame.m_nSampleCount ) || !t3dRdForwardFiniteEnvironmentFrame( rFrame ) ) {
		setError( "Terrain3D ABI 12 frame packet validation failed" );
		return false;
	}

	t3dRdForwardPassContext_t &context = m_mainCallbackContext;
	context.m_nBridgeAbiVersion = T3D_BRIDGE_ABI_VERSION;
	context.m_nFrameGeneration = rFrame.m_nGeneration;
	context.m_nScenePass = rFrame.m_nScenePass;
	context.m_nRenderFrame = rFrame.m_nRenderFrame;
	context.m_nViewportKey = rFrame.m_nViewportKey;
	context.m_nViewCount = rFrame.m_nViewCount;
	context.m_nSampleCount = rFrame.m_nSampleCount;
	context.m_nCameraVisibleLayers = rFrame.m_nCameraVisibleLayers;
	context.m_renderRegion = Rect2i( rFrame.m_nRenderRegion[0],
									 rFrame.m_nRenderRegion[1],
									 rFrame.m_nRenderRegion[2],
									 rFrame.m_nRenderRegion[3] );
	context.m_renderSize = Vector2i( rFrame.m_nRenderSize[0], rFrame.m_nRenderSize[1] );
	context.m_nClusterMaxElements = rFrame.m_nClusterMaxElements;
	context.m_nClusterShift = rFrame.m_nClusterShift;
	context.m_nClusterWidth = rFrame.m_nClusterWidth;
	context.m_nClusterTypeSize = rFrame.m_nClusterTypeSize;
	context.m_nClusterMaxElementsDiv32 = rFrame.m_nClusterMaxElementsDiv32;
	context.m_nDirectionalLightCount = rFrame.m_nDirectionalLightCount;
	context.m_fCameraZNear = rFrame.m_fCameraZNear;
	context.m_fCameraZFar = rFrame.m_fCameraZFar;
	context.m_fReflectionAtlasBorderSize = rFrame.m_fReflectionAtlasBorderSize;
	context.m_cameraTransform = t3dRdForwardDecodeTransform( rFrame.m_mCameraTransform );
	context.m_previousCameraTransform = context.m_cameraTransform;
	for ( uint32_t view = 0u; view < rFrame.m_nViewCount; ++view ) {
		context.m_szViewProjection[view] = t3dRdForwardDecodeProjection( rFrame.m_mViewProjection[view] );
		context.m_szPreviousViewProjection[view] =
			t3dRdForwardDecodeProjection( rFrame.m_mPreviousViewProjection[view] );
		context.m_szEyeOffset[view] =
			Vector3( rFrame.m_vEyeOffset[view][0], rFrame.m_vEyeOffset[view][1], rFrame.m_vEyeOffset[view][2] );
	}
	context.m_taaJitter = Vector2( rFrame.m_vMotionJitter[0], rFrame.m_vMotionJitter[1] );
	context.m_previousTaaJitter = Vector2( rFrame.m_vMotionJitter[2], rFrame.m_vMotionJitter[3] );
	context.m_bMotionVectorsEnabled = ( rFrame.m_nFlags & T3D_RD_FORWARD_FRAME_MOTION_VECTORS ) != 0u;
	context.m_bDirectionalLightSoftShadows = ( rFrame.m_nFlags & T3D_RD_FORWARD_FRAME_DIRECTIONAL_SOFT_SHADOWS ) != 0u;
	context.m_nEnvironmentFlags = ( ( rFrame.m_nEnvironmentFlags & T3D_RD_FORWARD_BRIDGE_ENV_USE_AMBIENT_LIGHT )
										? T3D_RD_FORWARD_ENV_USE_AMBIENT_LIGHT
										: 0u ) |
								  ( ( rFrame.m_nEnvironmentFlags & T3D_RD_FORWARD_BRIDGE_ENV_USE_AMBIENT_CUBEMAP )
										? T3D_RD_FORWARD_ENV_USE_AMBIENT_CUBEMAP
										: 0u ) |
								  ( ( rFrame.m_nEnvironmentFlags & T3D_RD_FORWARD_BRIDGE_ENV_USE_REFLECTION_CUBEMAP )
										? T3D_RD_FORWARD_ENV_USE_REFLECTION_CUBEMAP
										: 0u ) |
								  ( ( rFrame.m_nEnvironmentFlags & T3D_RD_FORWARD_BRIDGE_ENV_RADIANCE_OCTMAP_ARRAY )
										? T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY
										: 0u );
	memcpy( context.m_szAmbientLightColorEnergy,
			rFrame.m_szAmbientLightColorEnergy,
			sizeof( context.m_szAmbientLightColorEnergy ) );
	context.m_fAmbientColorSkyMix = rFrame.m_fAmbientColorSkyMix;
	context.m_fEmissiveExposureNormalization = rFrame.m_fEmissiveExposureNormalization;
	context.m_fIblExposureNormalization = rFrame.m_fIblExposureNormalization;
	context.m_fRadiancePixelSize = rFrame.m_fRadiancePixelSize;
	context.m_fRadianceBorderSize = rFrame.m_fRadianceBorderSize;
	memcpy( context.m_szRadianceInverseXform,
			rFrame.m_szRadianceInverseXform,
			sizeof( context.m_szRadianceInverseXform ) );
	m_nCachedFrameGeneration = rFrame.m_nGeneration;
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertResources(
	const t3dRdForwardBridgeResourcePacket_t &rResources ) {
	if ( rResources.m_nAbi != T3D_BRIDGE_ABI_VERSION || rResources.m_nStructSize != sizeof( rResources ) ||
		 rResources.m_nGeneration == 0u || rResources.m_nReserved0 != 0u ) {
		setError( "Terrain3D ABI 12 borrowed-resource packet validation failed" );
		return false;
	}
	if ( rResources.m_nGeneration == m_nCachedResourceGeneration ) {
		return true;
	}
	t3dRdForwardPassContext_t &context = m_mainCallbackContext;
	context.m_nResourceGeneration = rResources.m_nGeneration;
	context.m_hDirectionalLightBuffer = t3dRdForwardDecodeRid( rResources.m_hDirectionalLightBuffer );
	context.m_hOmniLightBuffer = t3dRdForwardDecodeRid( rResources.m_hOmniLightBuffer );
	context.m_hSpotLightBuffer = t3dRdForwardDecodeRid( rResources.m_hSpotLightBuffer );
	context.m_hAreaLightBuffer = t3dRdForwardDecodeRid( rResources.m_hAreaLightBuffer );
	context.m_hClusterBuffer = t3dRdForwardDecodeRid( rResources.m_hClusterBuffer );
	context.m_hShadowAtlasTexture = t3dRdForwardDecodeRid( rResources.m_hShadowAtlas );
	context.m_hDirectionalShadowAtlasTexture = t3dRdForwardDecodeRid( rResources.m_hDirectionalShadowAtlas );
	context.m_hNativeShadowSampler = t3dRdForwardDecodeRid( rResources.m_hShadowSampler );
	context.m_hReflectionProbeBuffer = t3dRdForwardDecodeRid( rResources.m_hReflectionBuffer );
	context.m_hReflectionAtlasTexture = t3dRdForwardDecodeRid( rResources.m_hReflectionAtlas );
	context.m_hDecalBuffer = t3dRdForwardDecodeRid( rResources.m_hDecalBuffer );
	context.m_hDecalAtlasTexture = t3dRdForwardDecodeRid( rResources.m_hDecalAtlas );
	context.m_hDecalAtlasSrgbTexture = t3dRdForwardDecodeRid( rResources.m_hDecalAtlasSrgb );
	context.m_hRadianceTexture = t3dRdForwardDecodeRid( rResources.m_hRadianceTexture );
	context.m_hPreviousDepthTexture = t3dRdForwardDecodeRid( rResources.m_hPreviousDepthTexture );
	context.m_bTemporalHistoryValid = context.m_hPreviousDepthTexture.is_valid();
	m_nCachedResourceGeneration = rResources.m_nGeneration;
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertPass(
	const t3dRdForwardBridgePassPacket_t &rPass ) {
	if ( rPass.m_nAbi != T3D_BRIDGE_ABI_VERSION || rPass.m_nStructSize != sizeof( rPass ) ||
		 !t3dRdForwardOrdinaryPassKind( rPass.m_nPassKind ) ||
		 rPass.m_nNativePassMode > T3D_RD_FORWARD_NATIVE_PASS_SDF || rPass.m_nSerial == 0u ||
		 rPass.m_nFramebufferFormat < 0 || rPass.m_hNativeFramebuffer == 0u ) {
		setError( "Terrain3D ABI 12 ordinary-pass packet validation failed" );
		return false;
	}
	t3dRdForwardPassContext_t &context = m_mainCallbackContext;
	context.m_phase = T3D_RD_FORWARD_PHASE_PREPARE;
	context.m_passKind = ( t3dRdForwardPassKind_t ) ( rPass.m_nPassKind );
	context.m_nativePassMode = ( t3dRdForwardNativePassMode_t ) ( rPass.m_nNativePassMode );
	context.m_nColorPassFlags = rPass.m_nColorFlags;
	context.m_nPassFlags = rPass.m_nFlags;
	context.m_bDepthPrepassActive = ( rPass.m_nFlags & T3D_RD_FORWARD_PASS_DEPTH_PREPASS_ACTIVE ) != 0u;
	context.m_bReverseCull = ( rPass.m_nFlags & T3D_RD_FORWARD_PASS_REVERSE_CULL ) != 0u;
	context.m_nFramebufferFormat = rPass.m_nFramebufferFormat;
	context.m_nPreparedSerial = rPass.m_nSerial;
	context.m_hNativeFramebuffer = t3dRdForwardDecodeRid( rPass.m_hNativeFramebuffer );
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertShadow(
	const t3dRdForwardBridgeShadowPacket_t &rShadow ) {
	if ( rShadow.m_nAbi != T3D_BRIDGE_ABI_VERSION || rShadow.m_nStructSize != sizeof( rShadow ) ||
		 rShadow.m_nNativePassMode > T3D_RD_FORWARD_NATIVE_PASS_SDF ||
		 !t3dRdForwardValidSampleCount( rShadow.m_nSampleCount ) || rShadow.m_nReserved0 != 0u ||
		 rShadow.m_nSerial == 0u || rShadow.m_nFramebufferFormat < 0 ) {
		setError( "Terrain3D ABI 12 shadow packet validation failed" );
		return false;
	}
	m_shadowCallbackContext = t3dRdForwardPassContext_t{};
	t3dRdForwardPassContext_t &context = m_shadowCallbackContext;
	context.m_nBridgeAbiVersion = T3D_BRIDGE_ABI_VERSION;
	context.m_phase = T3D_RD_FORWARD_PHASE_PREPARE;
	context.m_passKind = T3D_RD_FORWARD_PASS_SHADOW;
	context.m_nativePassMode = ( t3dRdForwardNativePassMode_t ) ( rShadow.m_nNativePassMode );
	context.m_nPassFlags = rShadow.m_nFlags;
	context.m_bReverseCull = ( rShadow.m_nFlags & T3D_RD_FORWARD_PASS_REVERSE_CULL ) != 0u;
	context.m_nViewCount = 1u;
	context.m_nSampleCount = rShadow.m_nSampleCount;
	context.m_nShadowCasterMask = rShadow.m_nShadowCasterMask;
	context.m_nShadowLightCullMask = rShadow.m_nShadowLightCullMask;
	context.m_nFramebufferFormat = rShadow.m_nFramebufferFormat;
	context.m_nPreparedSerial = rShadow.m_nSerial;
	context.m_nScenePass = rShadow.m_nScenePass;
	context.m_nRenderFrame = rShadow.m_nRenderFrame;
	context.m_nViewportKey = rShadow.m_nViewportKey;
	context.m_cameraTransform = t3dRdForwardDecodeTransform( rShadow.m_mCameraTransform );
	context.m_previousCameraTransform = context.m_cameraTransform;
	context.m_szViewProjection[0] = t3dRdForwardDecodeProjection( rShadow.m_mViewProjection );
	context.m_szPreviousViewProjection[0] = context.m_szViewProjection[0];
	context.m_szEyeOffset[0] = Vector3( rShadow.m_vEyeOffset[0], rShadow.m_vEyeOffset[1], rShadow.m_vEyeOffset[2] );
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::dispatchToCoordinators(
	const t3dRdForwardPassContext_t &rContext,
	uint64_t						 nCoordinatorMask ) {
	bool dispatched = false;
	bool accepted = true;
	while ( nCoordinatorMask != 0u ) {
		const uint32_t index = t3dRdForwardCtz64( nCoordinatorMask );
		nCoordinatorMask &= nCoordinatorMask - 1u;
		Terrain3DRenderCoordinator *const coordinator = m_szCoordinators[index];
		if ( coordinator ) {
			if ( rContext.m_phase == T3D_RD_FORWARD_PHASE_PREPARE && !Terrain3DRdContext::getShared().beginHotPass() ) {
				return false;
			}
			dispatched = true;
			accepted = coordinator->renderForwardPass( rContext ) && accepted;
		}
	}
	return dispatched && accepted;
}

static bool t3dRdForwardPassRequiresBorrowedResources(
	const t3dRdForwardPassKind_t passKind ) noexcept {
	return passKind == T3D_RD_FORWARD_PASS_OPAQUE || passKind == T3D_RD_FORWARD_PASS_MOTION ||
		   passKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepare(
	const t3dRdForwardBridgeFramePacket_t *const	pFrame,
	const t3dRdForwardBridgeResourcePacket_t *const pResources,
	const t3dRdForwardBridgePassPacket_t *const		pPass ) noexcept {
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	++m_nPacketsReceived;
#endif
	m_pPreparedContext = NULL;
	m_nCachedPassSerial = 0u;
	m_nPreparedCoordinatorMask = 0u;
	const uint64_t coordinator_mask = m_nCoordinatorMask.load( T3D_MEMORY_ORDER_ACQUIRE );
	bool		   accepted = false;
	{
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_FORWARD_BRIDGE_PACKET );
		accepted = m_bBridgeReady.load( T3D_MEMORY_ORDER_ACQUIRE ) && coordinator_mask != 0u && pPass &&
				   ( pFrame ? convertFrame( *pFrame ) : m_nCachedFrameGeneration != 0u ) && convertPass( *pPass );
		if ( accepted && pResources ) {
			accepted = convertResources( *pResources );
		}
		if ( accepted && !pResources &&
			 t3dRdForwardPassRequiresBorrowedResources( m_mainCallbackContext.m_passKind ) ) {
			accepted = m_nCachedResourceGeneration != 0u;
		}
		if ( accepted ) {
			accepted = Terrain3DRdContext::getShared().beginHotFrame( m_mainCallbackContext.m_nRenderFrame );
		}
	}
	if ( accepted ) {
		accepted = dispatchToCoordinators( m_mainCallbackContext, coordinator_mask );
	}
	if ( !accepted ) {
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
		++m_nPacketsRejected;
#endif
		return 0u;
	}
	m_nCachedPassSerial = pPass->m_nSerial;
	m_nPreparedCoordinatorMask = coordinator_mask;
	m_pPreparedContext = &m_mainCallbackContext;
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	++m_nPacketsAccepted;
	m_nLastScenePass = m_mainCallbackContext.m_nScenePass;
	m_nLastPassKind = ( uint32_t ) ( m_mainCallbackContext.m_passKind );
#endif
	return pPass->m_nSerial;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareShadow(
	const t3dRdForwardBridgeShadowPacket_t *const pShadow ) noexcept {
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	++m_nPacketsReceived;
#endif
	m_pPreparedContext = NULL;
	m_nCachedPassSerial = 0u;
	m_nPreparedCoordinatorMask = 0u;
	const uint64_t coordinator_mask = m_nCoordinatorMask.load( T3D_MEMORY_ORDER_ACQUIRE );
	bool		   accepted = false;
	{
		T3D_STREAM_PROFILE_SCOPE( T3D_SP_FORWARD_BRIDGE_PACKET );
		accepted = m_bBridgeReady.load( T3D_MEMORY_ORDER_ACQUIRE ) && coordinator_mask != 0u && pShadow &&
				   convertShadow( *pShadow ) &&
				   Terrain3DRdContext::getShared().beginHotFrame( m_shadowCallbackContext.m_nRenderFrame );
	}
	if ( accepted ) {
		accepted = dispatchToCoordinators( m_shadowCallbackContext, coordinator_mask );
	}
	if ( !accepted ) {
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
		++m_nPacketsRejected;
#endif
		return 0u;
	}
	m_nCachedPassSerial = pShadow->m_nSerial;
	m_nPreparedCoordinatorMask = coordinator_mask;
	m_pPreparedContext = &m_shadowCallbackContext;
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	++m_nPacketsAccepted;
	m_nLastScenePass = m_shadowCallbackContext.m_nScenePass;
	m_nLastPassKind = T3D_RD_FORWARD_PASS_SHADOW;
#endif
	return pShadow->m_nSerial;
}

uint32_t Terrain3DRdForwardPlusBridgeAdapter::submit(
	const uint64_t nPreparedSerial,
	const int64_t  nDrawList ) noexcept {
	if ( nPreparedSerial == 0u || nPreparedSerial != m_nCachedPassSerial || nDrawList < 0 || !m_pPreparedContext ) {
		return 0u;
	}
	m_pPreparedContext->m_phase = T3D_RD_FORWARD_PHASE_SUBMIT;
	m_pPreparedContext->m_nDrawList = nDrawList;
	dispatchToCoordinators( *m_pPreparedContext, m_nPreparedCoordinatorMask );
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	m_nLastDrawList = nDrawList;
#endif
	m_pPreparedContext = NULL;
	m_nCachedPassSerial = 0u;
	m_nPreparedCoordinatorMask = 0u;
	return 1u;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareThunk(
	void *const										pUserdata,
	const t3dRdForwardBridgeFramePacket_t *const	pFrame,
	const t3dRdForwardBridgeResourcePacket_t *const pResources,
	const t3dRdForwardBridgePassPacket_t *const		pPass ) noexcept {
	Terrain3DRdForwardPlusBridgeAdapter *const adapter = ( Terrain3DRdForwardPlusBridgeAdapter * ) ( pUserdata );
	return adapter ? adapter->prepare( pFrame, pResources, pPass ) : 0u;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareShadowThunk(
	void *const									  pUserdata,
	const t3dRdForwardBridgeShadowPacket_t *const pShadow ) noexcept {
	Terrain3DRdForwardPlusBridgeAdapter *const adapter = ( Terrain3DRdForwardPlusBridgeAdapter * ) ( pUserdata );
	return adapter ? adapter->prepareShadow( pShadow ) : 0u;
}

uint32_t Terrain3DRdForwardPlusBridgeAdapter::submitThunk(
	void *const	   pUserdata,
	const uint64_t nPreparedSerial,
	const int64_t  nDrawList ) noexcept {
	Terrain3DRdForwardPlusBridgeAdapter *const adapter = ( Terrain3DRdForwardPlusBridgeAdapter * ) ( pUserdata );
	return adapter ? adapter->submit( nPreparedSerial, nDrawList ) : 0u;
}

Dictionary Terrain3DRdForwardPlusBridgeAdapter::getDiagnostics(
	void ) {
	Terrain3DRdContext::forceSync();
	Dictionary result;
	result["singleton_name"] = s_pszEngineSingletonName;
	result["required_abi"] = ( int64_t ) ( T3D_BRIDGE_ABI_VERSION );
	result["observed_abi"] = ( int64_t ) ( getObservedAbiVersion() );
	result["required_capabilities"] = ( int64_t ) ( T3D_FORWARD_BRIDGE_REQUIRED_CAPABILITIES );
	result["observed_capabilities"] = ( int64_t ) ( getObservedCapabilities() );
    result["br24_fragment_subgroup_requested"] = m_bBr24SubgroupRequested;
    result["br24_fragment_subgroup_eligible"] = isBr24FragmentSubgroupEligibleCold();
    result["br24_fragment_subgroup_stages"] = ( int64_t ) m_br24FragmentSubgroup.m_nStageMask;
    result["br24_fragment_subgroup_operations"] = ( int64_t ) m_br24FragmentSubgroup.m_nOperationMask;
    result["br24_fragment_subgroup_size"] = ( int64_t ) m_br24FragmentSubgroup.m_nSubgroupSize;

	result["bridge_present"] = isBridgePresent();
	result["bridge_ready"] = isBridgeReady();
	result["callback_registered"] = isCallbackRegistered();
	const uint64_t coordinator_mask = m_nCoordinatorMask.load( T3D_MEMORY_ORDER_ACQUIRE );
	result["active_coordinator_mask"] = ( int64_t ) ( coordinator_mask );
	Array coordinator_lanes;
	{
		Terrain3DScopedLock lock( m_coordinatorMutex );
		uint64_t			lane_mask = coordinator_mask;
		while ( lane_mask != 0u ) {
			const uint32_t index = t3dRdForwardCtz64( lane_mask );
			lane_mask &= lane_mask - 1u;
			Terrain3DRenderCoordinator *const coordinator = m_szCoordinators[index];
			if ( coordinator ) {
				Dictionary lane = coordinator->getForwardLaneDiagnostics();
				lane["coordinator_slot"] = ( int64_t ) ( index );
				coordinator_lanes.push_back( lane );
			}
		}
	}
	result["coordinator_lanes"] = coordinator_lanes;
	result["cached_frame_generation"] = ( int64_t ) ( m_nCachedFrameGeneration );
	result["cached_resource_generation"] = ( int64_t ) ( m_nCachedResourceGeneration );
	result["environment_flags"] = ( int64_t ) ( m_mainCallbackContext.m_nEnvironmentFlags );
	result["environment_radiance_valid"] = m_mainCallbackContext.m_hRadianceTexture.is_valid();
	result["environment_radiance_array"] =
		( m_mainCallbackContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	result["packets_received"] = ( int64_t ) ( m_nPacketsReceived );
	result["packets_accepted"] = ( int64_t ) ( m_nPacketsAccepted );
	result["packets_rejected"] = ( int64_t ) ( m_nPacketsRejected );
	result["last_scene_pass"] = ( int64_t ) ( m_nLastScenePass );
	result["last_draw_list"] = m_nLastDrawList;
	result["last_pass_kind"] = ( int64_t ) ( m_nLastPassKind );
#else
	result["diagnostics_compiled"] = false;
#endif
	result["last_error"] = getLastError();
	return result;
}
