// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_DIAGNOSTICS_PUBLISHER_H
#define TERRAIN3D_DIAGNOSTICS_PUBLISHER_H

#include <stddef.h>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3DRenderCoordinator;

class Terrain3DDiagnosticsPublisher {
public:
	size_t getRetainedCpuBytes( const Terrain3DRenderCoordinator &rCoordinator ) const;
	godot::Dictionary getForwardLaneDiagnostics( const Terrain3DRenderCoordinator &rCoordinator ) const;
	godot::Dictionary getForwardDrawBaselineDiagnostics( const Terrain3DRenderCoordinator &rCoordinator ) const;
	godot::Dictionary getDiagnostics( const Terrain3DRenderCoordinator &rCoordinator ) const;
};

#endif // TERRAIN3D_DIAGNOSTICS_PUBLISHER_H
