// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H
#define TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"
#include "rendering/rd/terrain_3d_rd_types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <mutex>

using namespace godot;

class Terrain3DRenderCoordinator;

// One extension-side endpoint is registered with the patched engine. The
// endpoint translates the dynamic engine packet exactly once, then fans the
// typed state out to active Terrain3D coordinators without a render-thread
// mutex or a per-frame registration/lookup path.
class Terrain3DRdForwardPlusBridgeAdapter : public Object {
	GDCLASS( Terrain3DRdForwardPlusBridgeAdapter, Object );
	CLASS_NAME_STATIC( "Terrain3DRdForwardPlusBridgeAdapter" );

public:
	static constexpr uint32_t s_nBridgeAbiVersion = 4u;
	static constexpr uint32_t s_nMaxCoordinators = 64u;
	static constexpr const char *s_pszEngineSingletonName = "Terrain3DForwardPlusBridge";

private:
	static Terrain3DRdForwardPlusBridgeAdapter *s_pSingleton;

	Object *m_pEngineBridge = nullptr;
	Callable m_callback;
	std::array<std::atomic<Terrain3DRenderCoordinator *>, s_nMaxCoordinators> m_szCoordinators{};
	std::atomic_bool m_bBridgePresent{ false };
	std::atomic_bool m_bBridgeReady{ false };
	std::atomic_bool m_bCallbackRegistered{ false };
	std::atomic_uint32_t m_nObservedAbiVersion{ 0u };
	std::atomic_uint64_t m_nPacketsReceived{ 0u };
	std::atomic_uint64_t m_nPacketsAccepted{ 0u };
	std::atomic_uint64_t m_nPacketsRejected{ 0u };
	std::atomic_uint64_t m_nLastScenePass{ 0u };
	std::atomic_int64_t m_nLastDrawList{ -1 };
	std::atomic_uint32_t m_nLastPassKind{ T3D_RD_FORWARD_PASS_INVALID };
	mutable std::mutex m_errorMutex;
	String m_lastError;

	bool connectEngineBridge( void );
	void disconnectEngineBridge( void );
	bool forwardPassCallback( const Dictionary &rPacket );
	void setError( const String &rError );

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
	String getLastError( void ) const;
	Dictionary getDiagnostics( void ) const;

	// Exposed for deterministic CPU/static packet tests without requiring a
	// patched engine singleton.
	static bool parsePacket( const Dictionary &rPacket, t3dRdForwardPassContext_t &rContext, String &rError );
};

#endif // TERRAIN3D_RD_FORWARD_PLUS_BRIDGE_H
