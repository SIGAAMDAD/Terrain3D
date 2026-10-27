// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H
#define TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"
#include "core/utility/terrain_3d_sync.h"
#include "core/config/terrain_3d_compiler.h"
#include "rendering/rd/core/terrain_3d_rd_types.h"
#include "rendering/rd/forward/terrain_3d_fragment_subgroup_policy.h"

#include <EASTL/type_traits.h>
#include <stddef.h>
#include <stdint.h>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include "rendering/rd/core/terrain_3d_render_coordinator.h"

using namespace godot;

#if defined( _WIN32 )
#define T3D_RD_FORWARD_BRIDGE_CALL __cdecl
#else
#define T3D_RD_FORWARD_BRIDGE_CALL
#endif

static_assert( sizeof( void * ) == 8u, "Terrain3D ABI 12 requires 64-bit pointers" );
static_assert( sizeof( RID ) == sizeof( uint64_t ), "Terrain3D ABI 12 requires 64-bit RID storage" );
static_assert( eastl::is_standard_layout_v< t3dRdForwardBridgeFramePacket_t > &&
				   eastl::is_trivially_copyable_v< t3dRdForwardBridgeFramePacket_t >,
			   "ABI 12 frame packet must remain POD" );
static_assert( eastl::is_standard_layout_v< t3dRdForwardBridgeResourcePacket_t > &&
				   eastl::is_trivially_copyable_v< t3dRdForwardBridgeResourcePacket_t >,
			   "ABI 12 resource packet must remain POD" );
static_assert( eastl::is_standard_layout_v< t3dRdForwardBridgePassPacket_t > &&
				   eastl::is_trivially_copyable_v< t3dRdForwardBridgePassPacket_t >,
			   "ABI 12 pass packet must remain POD" );
static_assert( eastl::is_standard_layout_v< t3dRdForwardBridgeShadowPacket_t > &&
				   eastl::is_trivially_copyable_v< t3dRdForwardBridgeShadowPacket_t >,
			   "ABI 12 shadow packet must remain POD" );
static_assert( alignof( t3dRdForwardBridgeFramePacket_t ) == 8u && sizeof( t3dRdForwardBridgeFramePacket_t ) == 568u,
			   "ABI 11 frame packet layout changed" );
static_assert( offsetof( t3dRdForwardBridgeFramePacket_t, m_nAbi10ReservedTail ) == 468u,
			   "ABI 10 tail padding position changed" );
static_assert( offsetof( t3dRdForwardBridgeFramePacket_t, m_nEnvironmentFlags ) == 472u,
			   "ABI 11 Environment payload is not append-only" );
static_assert( alignof( t3dRdForwardBridgeResourcePacket_t ) == 8u &&
				   sizeof( t3dRdForwardBridgeResourcePacket_t ) == 136u,
			   "ABI 12 resource packet layout changed" );
static_assert( offsetof( t3dRdForwardBridgeResourcePacket_t, m_hRadianceTexture ) == 120u,
			   "ABI 12 inherited radiance RID moved" );
static_assert( offsetof( t3dRdForwardBridgeResourcePacket_t, m_hPreviousDepthTexture ) == 128u,
			   "ABI 12 previous-depth RID is not append-only" );
static_assert( alignof( t3dRdForwardBridgePassPacket_t ) == 8u && sizeof( t3dRdForwardBridgePassPacket_t ) == 48u,
			   "ABI 11 pass packet layout changed" );
static_assert( alignof( t3dRdForwardBridgeShadowPacket_t ) == 8u && sizeof( t3dRdForwardBridgeShadowPacket_t ) == 200u,
			   "ABI 11 shadow packet layout changed" );

typedef uint64_t( T3D_RD_FORWARD_BRIDGE_CALL *t3dRdForwardBridgePrepareFn )(
	void *,
	const t3dRdForwardBridgeFramePacket_t *,
	const t3dRdForwardBridgeResourcePacket_t *,
	const t3dRdForwardBridgePassPacket_t * ) noexcept;
typedef uint64_t( T3D_RD_FORWARD_BRIDGE_CALL *t3dRdForwardBridgeShadowPrepareFn )(
	void *,
	const t3dRdForwardBridgeShadowPacket_t * ) noexcept;
typedef uint32_t( T3D_RD_FORWARD_BRIDGE_CALL *t3dRdForwardBridgeSubmitFn )( void *, uint64_t, int64_t ) noexcept;

class Terrain3DRdForwardPlusBridgeAdapter : public Object
{
	GDCLASS( Terrain3DRdForwardPlusBridgeAdapter, Object );
	CLASS_NAME_STATIC( "Terrain3DRdForwardPlusBridgeAdapter" );

public:
private:
	static Terrain3DRdForwardPlusBridgeAdapter *s_pSingleton;

	Object *m_pEngineBridge = NULL;
	// Coordinator mutation is cold/main-thread work. The release-published mask is
	// the sole hot-path synchronization point; slots are immutable while their bit
	// is visible to the render thread. Detach clears the bit, synchronizes Godot's
	// render thread, then clears the pointer.
	Terrain3DRenderCoordinator *m_szCoordinators[T3D_MAX_COORDINATORS]{};
	mutable Terrain3DMutex		m_coordinatorMutex;
	t3dAtomicU64_t				m_nCoordinatorMask{ 0u };
	uint64_t					m_nPreparedCoordinatorMask = 0u;
	t3dAtomicBool_t				m_bBridgePresent{ false };
	t3dAtomicBool_t				m_bBridgeReady{ false };
	t3dAtomicBool_t				m_bCallbackRegistered{ false };
	t3dAtomicU32_t				m_nObservedAbiVersion{ 0u };
	t3dAtomicU64_t				m_nObservedCapabilities{ 0u };
    // BR18: cold optional engine-backend query, never inferred from vendor.
    t3dBr24SubgroupCapabilities_t m_br24FragmentSubgroup{};
    bool m_bBr24SubgroupRequested = false;

	uintptr_t					m_nPrepareAddress = 0u;
	uintptr_t					m_nShadowPrepareAddress = 0u;
	uintptr_t					m_nSubmitAddress = 0u;
	uintptr_t					m_nUserdataAddress = 0u;
	uint64_t					m_nCachedFrameGeneration = 0u;
	uint32_t					m_nCachedResourceGeneration = 0u;
	uint64_t					m_nCachedPassSerial = 0u;
	t3dRdForwardPassContext_t	m_mainCallbackContext;
	t3dRdForwardPassContext_t	m_shadowCallbackContext;
	t3dRdForwardPassContext_t  *m_pPreparedContext = NULL;
#if defined( T3D_EDITOR_NATIVE ) || defined( T3D_DEBUG_RUNTIME )
	uint64_t m_nPacketsReceived = 0u;
	uint64_t m_nPacketsAccepted = 0u;
	uint64_t m_nPacketsRejected = 0u;
	uint64_t m_nLastScenePass = 0u;
	int64_t	 m_nLastDrawList = -1;
	uint32_t m_nLastPassKind = T3D_RD_FORWARD_PASS_INVALID;
	uint64_t m_szFailureReports[T3D_RD_FORWARD_PASS_MAX][16]{};
	uint64_t m_szAcceptedByPass[T3D_RD_FORWARD_PASS_MAX]{};
	uint64_t m_szSubmittedByPass[T3D_RD_FORWARD_PASS_MAX]{};
#endif
	uint32_t m_nLastDispatchFailureStage = 0u;
	mutable Terrain3DMutex m_errorMutex;
	String				   m_lastError;

	void	 resetConnectionState( void );
	bool	 connectEngineBridge( void );
	void	 disconnectEngineBridge( void );
	void	 logPacketFailureHot( const char *pPhase, const char *pReason, uint32_t nReasonCode,
		uint32_t nPassKind, uint64_t nFrame, uint64_t nScenePass, uint64_t nSerial,
		uint64_t nCoordinatorMask, bool bHasFrame, bool bHasResources ) noexcept;
	void	 setError( const String &rError );
	bool	 failConnection( const String &rError );
	bool	 convertFrame( const t3dRdForwardBridgeFramePacket_t &rFrame );
	bool	 convertResources( const t3dRdForwardBridgeResourcePacket_t &rResources );
	bool	 convertPass( const t3dRdForwardBridgePassPacket_t &rPass );
	bool	 convertShadow( const t3dRdForwardBridgeShadowPacket_t &rShadow );
	bool	 dispatchToCoordinators( const t3dRdForwardPassContext_t &rContext, uint64_t nCoordinatorMask );
	uint64_t prepare( const t3dRdForwardBridgeFramePacket_t	   *pFrame,
					  const t3dRdForwardBridgeResourcePacket_t *pResources,
					  const t3dRdForwardBridgePassPacket_t	   *pPass ) noexcept;
	uint64_t prepareShadow( const t3dRdForwardBridgeShadowPacket_t *pShadow ) noexcept;
	uint32_t submit( uint64_t nPreparedSerial, int64_t nDrawList ) noexcept;
	static uint64_t T3D_RD_FORWARD_BRIDGE_CALL prepareThunk( void									  *pUserdata,
															 const t3dRdForwardBridgeFramePacket_t	  *pFrame,
															 const t3dRdForwardBridgeResourcePacket_t *pResources,
															 const t3dRdForwardBridgePassPacket_t	  *pPass ) noexcept;
	static uint64_t T3D_RD_FORWARD_BRIDGE_CALL
	prepareShadowThunk( void *pUserdata, const t3dRdForwardBridgeShadowPacket_t *pShadow ) noexcept;
	static uint32_t T3D_RD_FORWARD_BRIDGE_CALL submitThunk( void	*pUserdata,
															uint64_t nPreparedSerial,
															int64_t	 nDrawList ) noexcept;

protected:
	static void _bind_methods(
		void ) {
	}

public:
	Terrain3DRdForwardPlusBridgeAdapter( void );

	static void									initializeSingleton( void );
	static void									shutdownSingleton( void );
	static Terrain3DRdForwardPlusBridgeAdapter *getSingleton(
		void ) {
		return s_pSingleton;
	}

	bool attachCoordinator( Terrain3DRenderCoordinator *pCoordinator );
	void detachCoordinator( Terrain3DRenderCoordinator *pCoordinator );
	bool isBridgePresent(
		void ) const {
		return m_bBridgePresent.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	bool isBridgeReady(
		void ) const {
		return m_bBridgeReady.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	bool isCallbackRegistered(
		void ) const {
		return m_bCallbackRegistered.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint32_t getObservedAbiVersion(
		void ) const {
		return m_nObservedAbiVersion.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
	uint64_t getObservedCapabilities(
		void ) const {
		return m_nObservedCapabilities.load( T3D_MEMORY_ORDER_ACQUIRE );
	}
    bool isBr24FragmentSubgroupEligibleCold( void ) const {
        return t3dBr24FragmentSubgroupEligible( m_bBr24SubgroupRequested, m_br24FragmentSubgroup );
    }
	String	   getLastError( void ) const;
	Dictionary getDiagnostics( void );
};

#undef T3D_RD_FORWARD_BRIDGE_CALL

#endif // TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H
