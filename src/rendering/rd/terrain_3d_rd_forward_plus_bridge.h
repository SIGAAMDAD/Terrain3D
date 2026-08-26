// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H
#define TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <mutex>
#include <type_traits>

using namespace godot;

#if defined( _WIN32 )
#define T3D_RD_FORWARD_BRIDGE_CALL __cdecl
#else
#define T3D_RD_FORWARD_BRIDGE_CALL
#endif

static constexpr uint32_t s_nT3dForwardBridgeAbi = 11u;
static constexpr uint32_t s_nT3dForwardBridgeMaxViews = 2u;

typedef enum t3dRdForwardBridgeCapability_e : uint64_t {
	T3D_RD_FORWARD_CAP_DIRECT_POD = uint64_t( 1u ) << 0u,
	T3D_RD_FORWARD_CAP_DEPTH = uint64_t( 1u ) << 1u,
	T3D_RD_FORWARD_CAP_OPAQUE = uint64_t( 1u ) << 2u,
	T3D_RD_FORWARD_CAP_SHADOW = uint64_t( 1u ) << 3u,
	T3D_RD_FORWARD_CAP_MOTION = uint64_t( 1u ) << 4u,
	T3D_RD_FORWARD_CAP_TRANSPARENT = uint64_t( 1u ) << 5u,
	T3D_RD_FORWARD_CAP_CLUSTERED_LIGHTING = uint64_t( 1u ) << 6u,
	T3D_RD_FORWARD_CAP_DECALS = uint64_t( 1u ) << 7u,
	T3D_RD_FORWARD_CAP_REFLECTIONS = uint64_t( 1u ) << 8u,
	T3D_RD_FORWARD_CAP_RESOURCE_GENERATIONS = uint64_t( 1u ) << 9u,
	T3D_RD_FORWARD_CAP_SPLIT_SHADOW_PACKET = uint64_t( 1u ) << 10u,
	T3D_RD_FORWARD_CAP_ENVIRONMENT_LIGHTING = uint64_t( 1u ) << 11u,
} t3dRdForwardBridgeCapability_t;

static constexpr uint64_t s_nT3dForwardBridgeRequiredCapabilities =
		T3D_RD_FORWARD_CAP_DIRECT_POD | T3D_RD_FORWARD_CAP_DEPTH | T3D_RD_FORWARD_CAP_OPAQUE |
		T3D_RD_FORWARD_CAP_SHADOW | T3D_RD_FORWARD_CAP_MOTION | T3D_RD_FORWARD_CAP_TRANSPARENT |
		T3D_RD_FORWARD_CAP_CLUSTERED_LIGHTING | T3D_RD_FORWARD_CAP_DECALS | T3D_RD_FORWARD_CAP_REFLECTIONS |
		T3D_RD_FORWARD_CAP_RESOURCE_GENERATIONS | T3D_RD_FORWARD_CAP_SPLIT_SHADOW_PACKET |
		T3D_RD_FORWARD_CAP_ENVIRONMENT_LIGHTING;

typedef enum t3dRdForwardBridgeEnvironmentFlag_e : uint32_t {
	T3D_RD_FORWARD_BRIDGE_ENV_USE_AMBIENT_LIGHT = 1u << 0u,
	T3D_RD_FORWARD_BRIDGE_ENV_USE_AMBIENT_CUBEMAP = 1u << 1u,
	T3D_RD_FORWARD_BRIDGE_ENV_USE_REFLECTION_CUBEMAP = 1u << 2u,
	T3D_RD_FORWARD_BRIDGE_ENV_RADIANCE_OCTMAP_ARRAY = 1u << 3u,
} t3dRdForwardBridgeEnvironmentFlag_t;

typedef enum t3dRdForwardBridgeFrameFlag_e : uint32_t {
	T3D_RD_FORWARD_FRAME_MOTION_VECTORS = 1u << 0u,
	T3D_RD_FORWARD_FRAME_DIRECTIONAL_SOFT_SHADOWS = 1u << 1u,
} t3dRdForwardBridgeFrameFlag_t;

typedef enum t3dRdForwardBridgePassFlag_e : uint32_t {
	T3D_RD_FORWARD_PASS_DEPTH_PREPASS_ACTIVE = 1u << 0u,
	T3D_RD_FORWARD_PASS_REVERSE_CULL = 1u << 1u,
} t3dRdForwardBridgePassFlag_t;

typedef struct alignas( 8 ) t3dRdForwardBridgeFramePacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint64_t m_nGeneration;
	uint64_t m_nScenePass;
	uint64_t m_nRenderFrame;
	uint64_t m_nViewportKey;
	uint32_t m_nViewCount;
	uint32_t m_nSampleCount;
	uint32_t m_nCameraVisibleLayers;
	uint32_t m_nFlags;
	int32_t m_nRenderRegion[4];
	int32_t m_nRenderSize[2];
	uint32_t m_nClusterMaxElements;
	uint32_t m_nClusterShift;
	uint32_t m_nClusterWidth;
	uint32_t m_nClusterTypeSize;
	uint32_t m_nClusterMaxElementsDiv32;
	uint32_t m_nDirectionalLightCount;
	float m_fCameraZNear;
	float m_fCameraZFar;
	float m_fReflectionAtlasBorderSize;
	float m_mCameraTransform[12];
	float m_mViewProjection[s_nT3dForwardBridgeMaxViews][16];
	float m_mPreviousViewProjection[s_nT3dForwardBridgeMaxViews][16];
	float m_vEyeOffset[s_nT3dForwardBridgeMaxViews][4];
	float m_vMotionJitter[4];
	uint32_t m_nAbi10ReservedTail;
	uint32_t m_nEnvironmentFlags;
	uint32_t m_nEnvironmentReserved0;
	float m_szAmbientLightColorEnergy[4];
	float m_fAmbientColorSkyMix;
	float m_fEmissiveExposureNormalization;
	float m_fIblExposureNormalization;
	float m_fRadiancePixelSize;
	float m_fRadianceBorderSize;
	float m_fEnvironmentReserved1;
	float m_szRadianceInverseXform[12];
} t3dRdForwardBridgeFramePacket_t;

typedef struct alignas( 8 ) t3dRdForwardBridgeResourcePacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nGeneration;
	uint32_t m_nReserved0;
	uint64_t m_hDirectionalLightBuffer;
	uint64_t m_hOmniLightBuffer;
	uint64_t m_hSpotLightBuffer;
	uint64_t m_hAreaLightBuffer;
	uint64_t m_hClusterBuffer;
	uint64_t m_hShadowAtlas;
	uint64_t m_hDirectionalShadowAtlas;
	uint64_t m_hShadowSampler;
	uint64_t m_hReflectionBuffer;
	uint64_t m_hReflectionAtlas;
	uint64_t m_hDecalBuffer;
	uint64_t m_hDecalAtlas;
	uint64_t m_hDecalAtlasSrgb;
	uint64_t m_hRadianceTexture;
} t3dRdForwardBridgeResourcePacket_t;

typedef struct alignas( 8 ) t3dRdForwardBridgePassPacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nPassKind;
	uint32_t m_nNativePassMode;
	uint32_t m_nColorFlags;
	uint32_t m_nFlags;
	int64_t m_nFramebufferFormat;
	uint64_t m_nSerial;
	uint64_t m_hNativeFramebuffer;
} t3dRdForwardBridgePassPacket_t;

typedef struct alignas( 8 ) t3dRdForwardBridgeShadowPacket_s {
	uint32_t m_nAbi;
	uint32_t m_nStructSize;
	uint32_t m_nNativePassMode;
	uint32_t m_nFlags;
	uint32_t m_nSampleCount;
	uint32_t m_nShadowCasterMask;
	uint32_t m_nShadowLightCullMask;
	uint32_t m_nReserved0;
	int64_t m_nFramebufferFormat;
	uint64_t m_nSerial;
	uint64_t m_nScenePass;
	uint64_t m_nRenderFrame;
	uint64_t m_nViewportKey;
	float m_mCameraTransform[12];
	float m_mViewProjection[16];
	float m_vEyeOffset[4];
} t3dRdForwardBridgeShadowPacket_t;

static_assert( sizeof( void * ) == 8u, "Terrain3D ABI 11 requires 64-bit pointers" );
static_assert( sizeof( RID ) == sizeof( uint64_t ), "Terrain3D ABI 11 requires 64-bit RID storage" );
static_assert( std::is_standard_layout_v<t3dRdForwardBridgeFramePacket_t> &&
		std::is_trivially_copyable_v<t3dRdForwardBridgeFramePacket_t>, "ABI 11 frame packet must remain POD" );
static_assert( std::is_standard_layout_v<t3dRdForwardBridgeResourcePacket_t> &&
		std::is_trivially_copyable_v<t3dRdForwardBridgeResourcePacket_t>, "ABI 11 resource packet must remain POD" );
static_assert( std::is_standard_layout_v<t3dRdForwardBridgePassPacket_t> &&
		std::is_trivially_copyable_v<t3dRdForwardBridgePassPacket_t>, "ABI 11 pass packet must remain POD" );
static_assert( std::is_standard_layout_v<t3dRdForwardBridgeShadowPacket_t> &&
		std::is_trivially_copyable_v<t3dRdForwardBridgeShadowPacket_t>, "ABI 11 shadow packet must remain POD" );
static_assert( alignof( t3dRdForwardBridgeFramePacket_t ) == 8u && sizeof( t3dRdForwardBridgeFramePacket_t ) == 568u,
		"ABI 11 frame packet layout changed" );
static_assert( offsetof( t3dRdForwardBridgeFramePacket_t, m_nAbi10ReservedTail ) == 468u, "ABI 10 tail padding position changed" );
static_assert( offsetof( t3dRdForwardBridgeFramePacket_t, m_nEnvironmentFlags ) == 472u, "ABI 11 Environment payload is not append-only" );
static_assert( alignof( t3dRdForwardBridgeResourcePacket_t ) == 8u && sizeof( t3dRdForwardBridgeResourcePacket_t ) == 128u,
		"ABI 11 resource packet layout changed" );
static_assert( offsetof( t3dRdForwardBridgeResourcePacket_t, m_hRadianceTexture ) == 120u, "ABI 11 radiance RID is not append-only" );
static_assert( alignof( t3dRdForwardBridgePassPacket_t ) == 8u && sizeof( t3dRdForwardBridgePassPacket_t ) == 48u,
		"ABI 11 pass packet layout changed" );
static_assert( alignof( t3dRdForwardBridgeShadowPacket_t ) == 8u && sizeof( t3dRdForwardBridgeShadowPacket_t ) == 200u,
		"ABI 11 shadow packet layout changed" );

typedef uint64_t( T3D_RD_FORWARD_BRIDGE_CALL *t3dRdForwardBridgePrepareFn )(
		void *, const t3dRdForwardBridgeFramePacket_t *, const t3dRdForwardBridgeResourcePacket_t *,
		const t3dRdForwardBridgePassPacket_t * ) noexcept;
typedef uint64_t( T3D_RD_FORWARD_BRIDGE_CALL *t3dRdForwardBridgeShadowPrepareFn )(
		void *, const t3dRdForwardBridgeShadowPacket_t * ) noexcept;
typedef uint32_t( T3D_RD_FORWARD_BRIDGE_CALL *t3dRdForwardBridgeSubmitFn )(
		void *, uint64_t, int64_t ) noexcept;

class Terrain3DRenderCoordinator;

class Terrain3DRdForwardPlusBridgeAdapter : public Object {
	GDCLASS( Terrain3DRdForwardPlusBridgeAdapter, Object );
	CLASS_NAME_STATIC( "Terrain3DRdForwardPlusBridgeAdapter" );

public:
	static constexpr uint32_t s_nBridgeAbiVersion = s_nT3dForwardBridgeAbi;
	static constexpr uint32_t s_nMaxCoordinators = 64u;
	static constexpr const char *s_pszEngineSingletonName = "Terrain3DForwardPlusBridge";

private:
	static Terrain3DRdForwardPlusBridgeAdapter *s_pSingleton;

	Object *m_pEngineBridge = nullptr;
	std::atomic<Terrain3DRenderCoordinator *> m_szCoordinators[s_nMaxCoordinators]{};
	std::atomic_uint64_t m_nCoordinatorMask{ 0u };
	std::atomic_bool m_bBridgePresent{ false };
	std::atomic_bool m_bBridgeReady{ false };
	std::atomic_bool m_bCallbackRegistered{ false };
	std::atomic_uint32_t m_nObservedAbiVersion{ 0u };
	std::atomic_uint64_t m_nObservedCapabilities{ 0u };
	uintptr_t m_nPrepareAddress = 0u;
	uintptr_t m_nShadowPrepareAddress = 0u;
	uintptr_t m_nSubmitAddress = 0u;
	uintptr_t m_nUserdataAddress = 0u;
	uint64_t m_nCachedFrameGeneration = 0u;
	uint32_t m_nCachedResourceGeneration = 0u;
	uint64_t m_nCachedPassSerial = 0u;
	t3dRdForwardPassContext_t m_mainCallbackContext;
	t3dRdForwardPassContext_t m_shadowCallbackContext;
	t3dRdForwardPassContext_t *m_pPreparedContext = nullptr;
#if defined( TOOLS_ENABLED ) || defined( DEBUG_ENABLED )
	uint64_t m_nPacketsReceived = 0u;
	uint64_t m_nPacketsAccepted = 0u;
	uint64_t m_nPacketsRejected = 0u;
	uint64_t m_nLastScenePass = 0u;
	int64_t m_nLastDrawList = -1;
	uint32_t m_nLastPassKind = T3D_RD_FORWARD_PASS_INVALID;
#endif
	mutable std::mutex m_errorMutex;
	String m_lastError;

	void resetConnectionState( void );
	bool connectEngineBridge( void );
	void disconnectEngineBridge( void );
	void setError( const String &rError );
	bool convertFrame( const t3dRdForwardBridgeFramePacket_t &rFrame );
	bool convertResources( const t3dRdForwardBridgeResourcePacket_t &rResources );
	bool convertPass( const t3dRdForwardBridgePassPacket_t &rPass );
	bool convertShadow( const t3dRdForwardBridgeShadowPacket_t &rShadow );
	bool dispatchToCoordinators( const t3dRdForwardPassContext_t &rContext );
	uint64_t prepare( const t3dRdForwardBridgeFramePacket_t *pFrame,
			const t3dRdForwardBridgeResourcePacket_t *pResources,
			const t3dRdForwardBridgePassPacket_t *pPass ) noexcept;
	uint64_t prepareShadow( const t3dRdForwardBridgeShadowPacket_t *pShadow ) noexcept;
	uint32_t submit( uint64_t nPreparedSerial, int64_t nDrawList ) noexcept;
	static uint64_t T3D_RD_FORWARD_BRIDGE_CALL prepareThunk( void *pUserdata,
			const t3dRdForwardBridgeFramePacket_t *pFrame,
			const t3dRdForwardBridgeResourcePacket_t *pResources,
			const t3dRdForwardBridgePassPacket_t *pPass ) noexcept;
	static uint64_t T3D_RD_FORWARD_BRIDGE_CALL prepareShadowThunk(
			void *pUserdata, const t3dRdForwardBridgeShadowPacket_t *pShadow ) noexcept;
	static uint32_t T3D_RD_FORWARD_BRIDGE_CALL submitThunk(
			void *pUserdata, uint64_t nPreparedSerial, int64_t nDrawList ) noexcept;

protected:
	static void _bind_methods( void ) {}

public:
	Terrain3DRdForwardPlusBridgeAdapter( void );

	static void initializeSingleton( void );
	static void shutdownSingleton( void );
	static Terrain3DRdForwardPlusBridgeAdapter *getSingleton( void ) { return s_pSingleton; }

	bool attachCoordinator( Terrain3DRenderCoordinator *pCoordinator );
	void detachCoordinator( Terrain3DRenderCoordinator *pCoordinator );
	bool isBridgePresent( void ) const { return m_bBridgePresent.load( std::memory_order_acquire ); }
	bool isBridgeReady( void ) const { return m_bBridgeReady.load( std::memory_order_acquire ); }
	bool isCallbackRegistered( void ) const { return m_bCallbackRegistered.load( std::memory_order_acquire ); }
	uint32_t getObservedAbiVersion( void ) const { return m_nObservedAbiVersion.load( std::memory_order_acquire ); }
	uint64_t getObservedCapabilities( void ) const { return m_nObservedCapabilities.load( std::memory_order_acquire ); }
	String getLastError( void ) const;
	Dictionary getDiagnostics( void );
};

#undef T3D_RD_FORWARD_BRIDGE_CALL

#endif // TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H
