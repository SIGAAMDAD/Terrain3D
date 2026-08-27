// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/rd/terrain_3d_rd_forward_plus_bridge.h"

#include "rendering/rd/terrain_3d_rd_context.h"
#include "rendering/rd/terrain_3d_render_coordinator.h"

#include <cmath>
#include <cstring>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#if defined( _MSC_VER )
#include <intrin.h>
#endif

Terrain3DRdForwardPlusBridgeAdapter *Terrain3DRdForwardPlusBridgeAdapter::s_pSingleton = nullptr;

static bool t3dRdForwardValidSampleCount( const uint32_t nSamples )
{
	return nSamples == 1u || nSamples == 2u || nSamples == 4u || nSamples == 8u;
}

static uint32_t t3dRdForwardCtz64( const uint64_t nMask )
{
#if defined( _MSC_VER )
	unsigned long index = 0u;
	_BitScanForward64( &index, nMask );
	return (uint32_t)( index );
#else
	return (uint32_t)( __builtin_ctzll( nMask ) );
#endif
}

static RID t3dRdForwardDecodeRid( const uint64_t nOpaque )
{
	RID result;
	std::memcpy( &result, &nOpaque, sizeof( result ) );
	return result;
}

static Projection t3dRdForwardDecodeProjection( const float *const pValues )
{
	Projection result;
	for ( uint32_t column = 0u; column < 4u; ++column ) {
		for ( uint32_t row = 0u; row < 4u; ++row ) {
			result.columns[column][row] = pValues[column * 4u + row];
		}
	}
	return result;
}

static Transform3D t3dRdForwardDecodeTransform( const float *const pValues )
{
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

static bool t3dRdForwardFiniteEnvironmentFrame( const t3dRdForwardBridgeFramePacket_t& rFrame )
{
	if ( rFrame.m_nAbi10ReservedTail != 0u || rFrame.m_nEnvironmentReserved0 != 0u ||
			rFrame.m_fEnvironmentReserved1 != 0.0f || ( rFrame.m_nEnvironmentFlags & ~uint32_t( 0x0fu ) ) != 0u ||
			!std::isfinite( rFrame.m_fAmbientColorSkyMix ) || rFrame.m_fAmbientColorSkyMix < 0.0f ||
			rFrame.m_fAmbientColorSkyMix > 1.0f || !std::isfinite( rFrame.m_fEmissiveExposureNormalization ) ||
			!std::isfinite( rFrame.m_fIblExposureNormalization ) || !std::isfinite( rFrame.m_fRadiancePixelSize ) ||
			rFrame.m_fRadiancePixelSize < 0.0f || !std::isfinite( rFrame.m_fRadianceBorderSize ) ||
			rFrame.m_fRadianceBorderSize < 0.0f ) {
		return false;
	}
	for ( uint32_t i = 0u; i < 4u; ++i ) {
		if ( !std::isfinite( rFrame.m_szAmbientLightColorEnergy[i] ) ) {
			return false;
		}
	}
	for ( uint32_t i = 0u; i < 12u; ++i ) {
		if ( !std::isfinite( rFrame.m_szRadianceInverseXform[i] ) ) {
			return false;
		}
	}
	return true;
}

static bool t3dRdForwardOrdinaryPassKind( const uint32_t nPassKind )
{
	return nPassKind == T3D_RD_FORWARD_PASS_DEPTH || nPassKind == T3D_RD_FORWARD_PASS_OPAQUE ||
			nPassKind == T3D_RD_FORWARD_PASS_MOTION || nPassKind == T3D_RD_FORWARD_PASS_TRANSPARENT;
}

Terrain3DRdForwardPlusBridgeAdapter::Terrain3DRdForwardPlusBridgeAdapter( void )
{
	for ( std::atomic<Terrain3DRenderCoordinator *>& slot : m_szCoordinators ) {
		slot.store( nullptr, std::memory_order_relaxed );
	}
	resetConnectionState();
}

void Terrain3DRdForwardPlusBridgeAdapter::initializeSingleton( void )
{
	if ( s_pSingleton ) {
		return;
	}
	s_pSingleton = memnew( Terrain3DRdForwardPlusBridgeAdapter );
	s_pSingleton->connectEngineBridge();
}

void Terrain3DRdForwardPlusBridgeAdapter::shutdownSingleton( void )
{
	Terrain3DRdForwardPlusBridgeAdapter *const adapter = s_pSingleton;
	if ( !adapter ) {
		return;
	}
	adapter->disconnectEngineBridge();
	adapter->m_nCoordinatorMask.store( 0u, std::memory_order_release );
	Terrain3DRdContext::forceSync();
	for ( std::atomic<Terrain3DRenderCoordinator *>& slot : adapter->m_szCoordinators ) {
		slot.store( nullptr, std::memory_order_release );
	}
	s_pSingleton = nullptr;
	memdelete( adapter );
}

void Terrain3DRdForwardPlusBridgeAdapter::resetConnectionState( void )
{
	m_bBridgePresent.store( false, std::memory_order_release );
	m_bBridgeReady.store( false, std::memory_order_release );
	m_bCallbackRegistered.store( false, std::memory_order_release );
	m_nObservedAbiVersion.store( 0u, std::memory_order_release );
	m_nObservedCapabilities.store( 0u, std::memory_order_release );
	m_pEngineBridge = nullptr;
	m_nPrepareAddress = reinterpret_cast<uintptr_t>( &Terrain3DRdForwardPlusBridgeAdapter::prepareThunk );
	m_nShadowPrepareAddress = reinterpret_cast<uintptr_t>( &Terrain3DRdForwardPlusBridgeAdapter::prepareShadowThunk );
	m_nSubmitAddress = reinterpret_cast<uintptr_t>( &Terrain3DRdForwardPlusBridgeAdapter::submitThunk );
	m_nUserdataAddress = reinterpret_cast<uintptr_t>( this );
	m_nCachedFrameGeneration = 0u;
	m_nCachedResourceGeneration = 0u;
	m_nCachedPassSerial = 0u;
	m_mainCallbackContext = t3dRdForwardPassContext_t{};
	m_shadowCallbackContext = t3dRdForwardPassContext_t{};
	m_pPreparedContext = nullptr;
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	m_nPacketsReceived = 0u;
	m_nPacketsAccepted = 0u;
	m_nPacketsRejected = 0u;
	m_nLastScenePass = 0u;
	m_nLastDrawList = -1;
	m_nLastPassKind = T3D_RD_FORWARD_PASS_INVALID;
#endif
}

void Terrain3DRdForwardPlusBridgeAdapter::setError( const String& rError )
{
	std::lock_guard<std::mutex> lock( m_errorMutex );
	m_lastError = rError;
}

String Terrain3DRdForwardPlusBridgeAdapter::getLastError( void ) const
{
	std::lock_guard<std::mutex> lock( m_errorMutex );
	return m_lastError;
}

bool Terrain3DRdForwardPlusBridgeAdapter::connectEngineBridge( void )
{
	resetConnectionState();
	const auto failConnection = [this]( const String& rError ) {
		setError( rError );
		UtilityFunctions::push_error( "Terrain3D Forward+ bridge: ", rError );
		return false;
	};

	Engine *const engine = Engine::get_singleton();
	const StringName singleton_name( s_pszEngineSingletonName );
	if ( !engine || !engine->has_singleton( singleton_name ) ) {
		return failConnection( "Godot 4.7.2 Terrain3DForwardPlusBridge singleton is missing; production "
							   "RenderingDevice rendering is disabled" );
	}
	m_bBridgePresent.store( true, std::memory_order_release );
	m_pEngineBridge = engine->get_singleton( singleton_name );
	static const char *const required_methods[] = { "get_abi_version",
		"get_native_capabilities",
		"register_native_callback",
		"unregister_native_callback",
		"has_native_callback",
		"collect_retired_callbacks" };
	for ( const char *const method : required_methods ) {
		if ( !m_pEngineBridge || !m_pEngineBridge->has_method( StringName( method ) ) ) {
			return failConnection(
					String( "Terrain3DForwardPlusBridge singleton is missing required ABI 11 method: " ) + method );
		}
	}

	const Variant abi_value = m_pEngineBridge->call( StringName( "get_abi_version" ) );
	if ( abi_value.get_type() != Variant::INT ) {
		return failConnection( "Terrain3DForwardPlusBridge returned a non-integer ABI version" );
	}
	const int64_t abi_signed = (int64_t)( abi_value );
	const uint32_t abi = abi_signed >= 0 ? (uint32_t)( abi_signed ) : 0u;
	m_nObservedAbiVersion.store( abi, std::memory_order_release );
	if ( abi != s_nBridgeAbiVersion ) {
		return failConnection( vformat(
				"Terrain3DForwardPlusBridge ABI mismatch: expected %d, received %d", s_nBridgeAbiVersion, abi ) );
	}

	const Variant capabilities_value = m_pEngineBridge->call( StringName( "get_native_capabilities" ) );
	if ( capabilities_value.get_type() != Variant::INT ) {
		return failConnection( "Terrain3DForwardPlusBridge returned a non-integer capability mask" );
	}
	const uint64_t capabilities = (uint64_t)( (int64_t)( capabilities_value ) );
	m_nObservedCapabilities.store( capabilities, std::memory_order_release );
	if ( ( capabilities & s_nT3dForwardBridgeRequiredCapabilities ) != s_nT3dForwardBridgeRequiredCapabilities ) {
		return failConnection(
				vformat( "Terrain3DForwardPlusBridge capability mismatch: required 0x%llx, received 0x%llx",
						s_nT3dForwardBridgeRequiredCapabilities,
						capabilities ) );
	}

	const Variant registered_value = m_pEngineBridge->call( StringName( "register_native_callback" ),
			(int64_t)( m_nPrepareAddress ),
			(int64_t)( m_nShadowPrepareAddress ),
			(int64_t)( m_nSubmitAddress ),
			(int64_t)( m_nUserdataAddress ),
			(int64_t)( sizeof( t3dRdForwardBridgeFramePacket_t ) ),
			(int64_t)( sizeof( t3dRdForwardBridgeResourcePacket_t ) ),
			(int64_t)( sizeof( t3dRdForwardBridgePassPacket_t ) ),
			(int64_t)( sizeof( t3dRdForwardBridgeShadowPacket_t ) ) );
	const bool registered = registered_value.get_type() == Variant::BOOL && (bool)( registered_value );
	m_bCallbackRegistered.store( registered, std::memory_order_release );
	if ( !registered ) {
		return failConnection( "Terrain3DForwardPlusBridge rejected the ABI 11 native callback registration" );
	}

	const Variant has_callback_value = m_pEngineBridge->call( StringName( "has_native_callback" ) );
	if ( has_callback_value.get_type() != Variant::BOOL || !(bool)( has_callback_value ) ) {
		disconnectEngineBridge();
		return failConnection(
				"Terrain3DForwardPlusBridge did not report the registered native callback as available" );
	}
	m_bBridgeReady.store( true, std::memory_order_release );
	setError( String() );
	return true;
}

void Terrain3DRdForwardPlusBridgeAdapter::disconnectEngineBridge( void )
{
	m_bBridgeReady.store( false, std::memory_order_release );
	Object *const bridge = m_pEngineBridge;
	if ( bridge && m_bCallbackRegistered.exchange( false, std::memory_order_acq_rel ) ) {
		bridge->call( StringName( "unregister_native_callback" ),
				(int64_t)( m_nPrepareAddress ),
				(int64_t)( m_nShadowPrepareAddress ),
				(int64_t)( m_nSubmitAddress ),
				(int64_t)( m_nUserdataAddress ) );
		Terrain3DRdContext::forceSync();
		bridge->call( StringName( "collect_retired_callbacks" ) );
	}
	m_pPreparedContext = nullptr;
	m_nCachedPassSerial = 0u;
	m_pEngineBridge = nullptr;
	m_bBridgePresent.store( false, std::memory_order_release );
}

bool Terrain3DRdForwardPlusBridgeAdapter::attachCoordinator( Terrain3DRenderCoordinator *const pCoordinator )
{
	if ( !pCoordinator ) {
		return false;
	}
	for ( uint32_t i = 0u; i < s_nMaxCoordinators; ++i ) {
		if ( m_szCoordinators[i].load( std::memory_order_acquire ) == pCoordinator ) {
			return true;
		}
	}
	for ( uint32_t i = 0u; i < s_nMaxCoordinators; ++i ) {
		Terrain3DRenderCoordinator *expected = nullptr;
		if ( m_szCoordinators[i].compare_exchange_strong(
					 expected, pCoordinator, std::memory_order_acq_rel, std::memory_order_acquire ) ) {
			m_nCoordinatorMask.fetch_or( uint64_t( 1u ) << i, std::memory_order_release );
			return true;
		}
	}
	setError( "Terrain3D Forward+ bridge coordinator slot capacity exhausted" );
	return false;
}

void Terrain3DRdForwardPlusBridgeAdapter::detachCoordinator( Terrain3DRenderCoordinator *const pCoordinator )
{
	if ( !pCoordinator ) {
		return;
	}
	uint64_t removed_mask = 0u;
	for ( uint32_t i = 0u; i < s_nMaxCoordinators; ++i ) {
		if ( m_szCoordinators[i].load( std::memory_order_acquire ) == pCoordinator ) {
			removed_mask |= uint64_t( 1u ) << i;
		}
	}
	if ( removed_mask == 0u ) {
		return;
	}
	m_nCoordinatorMask.fetch_and( ~removed_mask, std::memory_order_acq_rel );
	Terrain3DRdContext::forceSync();
	while ( removed_mask != 0u ) {
		const uint32_t index = t3dRdForwardCtz64( removed_mask );
		removed_mask &= removed_mask - 1u;
		Terrain3DRenderCoordinator *expected = pCoordinator;
		m_szCoordinators[index].compare_exchange_strong(
				expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire );
	}
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertFrame( const t3dRdForwardBridgeFramePacket_t& rFrame )
{
	if ( rFrame.m_nAbi != s_nBridgeAbiVersion || rFrame.m_nStructSize != sizeof( rFrame ) ||
			rFrame.m_nGeneration == 0u || rFrame.m_nViewCount == 0u ||
			rFrame.m_nViewCount > s_nT3dForwardBridgeMaxViews ||
			!t3dRdForwardValidSampleCount( rFrame.m_nSampleCount ) || !t3dRdForwardFiniteEnvironmentFrame( rFrame ) ) {
		setError( "Terrain3D ABI 11 frame packet validation failed" );
		return false;
	}
	if ( rFrame.m_nGeneration == m_nCachedFrameGeneration ) {
		return true;
	}

	t3dRdForwardPassContext_t& context = m_mainCallbackContext;
	context.m_nBridgeAbiVersion = s_nBridgeAbiVersion;
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
	std::memcpy( context.m_szAmbientLightColorEnergy,
			rFrame.m_szAmbientLightColorEnergy,
			sizeof( context.m_szAmbientLightColorEnergy ) );
	context.m_fAmbientColorSkyMix = rFrame.m_fAmbientColorSkyMix;
	context.m_fEmissiveExposureNormalization = rFrame.m_fEmissiveExposureNormalization;
	context.m_fIblExposureNormalization = rFrame.m_fIblExposureNormalization;
	context.m_fRadiancePixelSize = rFrame.m_fRadiancePixelSize;
	context.m_fRadianceBorderSize = rFrame.m_fRadianceBorderSize;
	std::memcpy( context.m_szRadianceInverseXform,
			rFrame.m_szRadianceInverseXform,
			sizeof( context.m_szRadianceInverseXform ) );
	m_nCachedFrameGeneration = rFrame.m_nGeneration;
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertResources( const t3dRdForwardBridgeResourcePacket_t& rResources )
{
	if ( rResources.m_nAbi != s_nBridgeAbiVersion || rResources.m_nStructSize != sizeof( rResources ) ||
			rResources.m_nGeneration == 0u || rResources.m_nReserved0 != 0u ) {
		setError( "Terrain3D ABI 11 borrowed-resource packet validation failed" );
		return false;
	}
	if ( rResources.m_nGeneration == m_nCachedResourceGeneration ) {
		return true;
	}
	t3dRdForwardPassContext_t& context = m_mainCallbackContext;
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
	m_nCachedResourceGeneration = rResources.m_nGeneration;
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertPass( const t3dRdForwardBridgePassPacket_t& rPass )
{
	if ( rPass.m_nAbi != s_nBridgeAbiVersion || rPass.m_nStructSize != sizeof( rPass ) ||
			!t3dRdForwardOrdinaryPassKind( rPass.m_nPassKind ) ||
			rPass.m_nNativePassMode > T3D_RD_FORWARD_NATIVE_PASS_SDF || rPass.m_nSerial == 0u ||
			rPass.m_nFramebufferFormat < 0 || rPass.m_hNativeFramebuffer == 0u ) {
		setError( "Terrain3D ABI 11 ordinary-pass packet validation failed" );
		return false;
	}
	t3dRdForwardPassContext_t& context = m_mainCallbackContext;
	context.m_phase = T3D_RD_FORWARD_PHASE_PREPARE;
	context.m_passKind = (t3dRdForwardPassKind_t)( rPass.m_nPassKind );
	context.m_nativePassMode = (t3dRdForwardNativePassMode_t)( rPass.m_nNativePassMode );
	context.m_nColorPassFlags = rPass.m_nColorFlags;
	context.m_nPassFlags = rPass.m_nFlags;
	context.m_bDepthPrepassActive = ( rPass.m_nFlags & T3D_RD_FORWARD_PASS_DEPTH_PREPASS_ACTIVE ) != 0u;
	context.m_bReverseCull = ( rPass.m_nFlags & T3D_RD_FORWARD_PASS_REVERSE_CULL ) != 0u;
	context.m_nFramebufferFormat = rPass.m_nFramebufferFormat;
	context.m_nPreparedSerial = rPass.m_nSerial;
	context.m_hNativeFramebuffer = t3dRdForwardDecodeRid( rPass.m_hNativeFramebuffer );
	return true;
}

bool Terrain3DRdForwardPlusBridgeAdapter::convertShadow( const t3dRdForwardBridgeShadowPacket_t& rShadow )
{
	if ( rShadow.m_nAbi != s_nBridgeAbiVersion || rShadow.m_nStructSize != sizeof( rShadow ) ||
			rShadow.m_nNativePassMode > T3D_RD_FORWARD_NATIVE_PASS_SDF ||
			!t3dRdForwardValidSampleCount( rShadow.m_nSampleCount ) || rShadow.m_nReserved0 != 0u ||
			rShadow.m_nSerial == 0u || rShadow.m_nFramebufferFormat < 0 ) {
		setError( "Terrain3D ABI 11 shadow packet validation failed" );
		return false;
	}
	m_shadowCallbackContext = t3dRdForwardPassContext_t{};
	t3dRdForwardPassContext_t& context = m_shadowCallbackContext;
	context.m_nBridgeAbiVersion = s_nBridgeAbiVersion;
	context.m_phase = T3D_RD_FORWARD_PHASE_PREPARE;
	context.m_passKind = T3D_RD_FORWARD_PASS_SHADOW;
	context.m_nativePassMode = (t3dRdForwardNativePassMode_t)( rShadow.m_nNativePassMode );
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

bool Terrain3DRdForwardPlusBridgeAdapter::dispatchToCoordinators( const t3dRdForwardPassContext_t& rContext )
{
	uint64_t mask = m_nCoordinatorMask.load( std::memory_order_acquire );
	bool dispatched = false;
	bool accepted = true;
	while ( mask != 0u ) {
		const uint32_t index = t3dRdForwardCtz64( mask );
		mask &= mask - 1u;
		Terrain3DRenderCoordinator *const coordinator = m_szCoordinators[index].load( std::memory_order_acquire );
		if ( coordinator ) {
			dispatched = true;
			accepted = coordinator->renderForwardPass( rContext ) && accepted;
		}
	}
	return dispatched && accepted;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepare( const t3dRdForwardBridgeFramePacket_t *const pFrame,
		const t3dRdForwardBridgeResourcePacket_t *const pResources,
		const t3dRdForwardBridgePassPacket_t *const pPass ) noexcept
{
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	++m_nPacketsReceived;
#endif
	m_pPreparedContext = nullptr;
	m_nCachedPassSerial = 0u;
	if ( !m_bBridgeReady.load( std::memory_order_acquire ) || !pPass ||
			( pFrame ? !convertFrame( *pFrame ) : m_nCachedFrameGeneration == 0u ) ||
			( pResources ? !convertResources( *pResources ) : m_nCachedResourceGeneration == 0u ) ||
			!convertPass( *pPass ) || !dispatchToCoordinators( m_mainCallbackContext ) ) {
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
		++m_nPacketsRejected;
#endif
		return 0u;
	}
	m_nCachedPassSerial = pPass->m_nSerial;
	m_pPreparedContext = &m_mainCallbackContext;
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	++m_nPacketsAccepted;
	m_nLastScenePass = m_mainCallbackContext.m_nScenePass;
	m_nLastPassKind = (uint32_t)( m_mainCallbackContext.m_passKind );
#endif
	return pPass->m_nSerial;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareShadow(
		const t3dRdForwardBridgeShadowPacket_t *const pShadow ) noexcept
{
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	++m_nPacketsReceived;
#endif
	m_pPreparedContext = nullptr;
	m_nCachedPassSerial = 0u;
	if ( !m_bBridgeReady.load( std::memory_order_acquire ) || !pShadow || !convertShadow( *pShadow ) ||
			!dispatchToCoordinators( m_shadowCallbackContext ) ) {
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
		++m_nPacketsRejected;
#endif
		return 0u;
	}
	m_nCachedPassSerial = pShadow->m_nSerial;
	m_pPreparedContext = &m_shadowCallbackContext;
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	++m_nPacketsAccepted;
	m_nLastScenePass = m_shadowCallbackContext.m_nScenePass;
	m_nLastPassKind = T3D_RD_FORWARD_PASS_SHADOW;
#endif
	return pShadow->m_nSerial;
}

uint32_t Terrain3DRdForwardPlusBridgeAdapter::submit( const uint64_t nPreparedSerial, const int64_t nDrawList ) noexcept
{
	if ( nPreparedSerial == 0u || nPreparedSerial != m_nCachedPassSerial || nDrawList < 0 || !m_pPreparedContext ) {
		return 0u;
	}
	m_pPreparedContext->m_phase = T3D_RD_FORWARD_PHASE_SUBMIT;
	m_pPreparedContext->m_nDrawList = nDrawList;
	dispatchToCoordinators( *m_pPreparedContext );
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	m_nLastDrawList = nDrawList;
#endif
	m_pPreparedContext = nullptr;
	m_nCachedPassSerial = 0u;
	return 1u;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareThunk( void *const pUserdata,
		const t3dRdForwardBridgeFramePacket_t *const pFrame,
		const t3dRdForwardBridgeResourcePacket_t *const pResources,
		const t3dRdForwardBridgePassPacket_t *const pPass ) noexcept
{
	Terrain3DRdForwardPlusBridgeAdapter *const adapter =
			static_cast<Terrain3DRdForwardPlusBridgeAdapter *>( pUserdata );
	return adapter ? adapter->prepare( pFrame, pResources, pPass ) : 0u;
}

uint64_t Terrain3DRdForwardPlusBridgeAdapter::prepareShadowThunk( void *const pUserdata,
		const t3dRdForwardBridgeShadowPacket_t *const pShadow ) noexcept
{
	Terrain3DRdForwardPlusBridgeAdapter *const adapter =
			static_cast<Terrain3DRdForwardPlusBridgeAdapter *>( pUserdata );
	return adapter ? adapter->prepareShadow( pShadow ) : 0u;
}

uint32_t Terrain3DRdForwardPlusBridgeAdapter::submitThunk( void *const pUserdata,
		const uint64_t nPreparedSerial,
		const int64_t nDrawList ) noexcept
{
	Terrain3DRdForwardPlusBridgeAdapter *const adapter =
			static_cast<Terrain3DRdForwardPlusBridgeAdapter *>( pUserdata );
	return adapter ? adapter->submit( nPreparedSerial, nDrawList ) : 0u;
}

Dictionary Terrain3DRdForwardPlusBridgeAdapter::getDiagnostics( void )
{
	Terrain3DRdContext::forceSync();
	Dictionary result;
	result["singleton_name"] = s_pszEngineSingletonName;
	result["required_abi"] = (int64_t)( s_nBridgeAbiVersion );
	result["observed_abi"] = (int64_t)( getObservedAbiVersion() );
	result["required_capabilities"] = (int64_t)( s_nT3dForwardBridgeRequiredCapabilities );
	result["observed_capabilities"] = (int64_t)( getObservedCapabilities() );
	result["bridge_present"] = isBridgePresent();
	result["bridge_ready"] = isBridgeReady();
	result["callback_registered"] = isCallbackRegistered();
	result["active_coordinator_mask"] = (int64_t)( m_nCoordinatorMask.load( std::memory_order_acquire ) );
	result["cached_frame_generation"] = (int64_t)( m_nCachedFrameGeneration );
	result["cached_resource_generation"] = (int64_t)( m_nCachedResourceGeneration );
	result["environment_flags"] = (int64_t)( m_mainCallbackContext.m_nEnvironmentFlags );
	result["environment_radiance_valid"] = m_mainCallbackContext.m_hRadianceTexture.is_valid();
	result["environment_radiance_array"] =
			( m_mainCallbackContext.m_nEnvironmentFlags & T3D_RD_FORWARD_ENV_RADIANCE_OCTMAP_ARRAY ) != 0u;
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	result["packets_received"] = (int64_t)( m_nPacketsReceived );
	result["packets_accepted"] = (int64_t)( m_nPacketsAccepted );
	result["packets_rejected"] = (int64_t)( m_nPacketsRejected );
	result["last_scene_pass"] = (int64_t)( m_nLastScenePass );
	result["last_draw_list"] = m_nLastDrawList;
	result["last_pass_kind"] = (int64_t)( m_nLastPassKind );
#else
	result["diagnostics_compiled"] = false;
#endif
	result["last_error"] = getLastError();
	return result;
}
