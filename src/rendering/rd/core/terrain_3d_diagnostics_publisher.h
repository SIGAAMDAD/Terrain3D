// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_DIAGNOSTICS_PUBLISHER_H
#define TERRAIN3D_DIAGNOSTICS_PUBLISHER_H

#include "rendering/rd/core/terrain_3d_diagnostic_inputs.h"

#include <stddef.h>
#include <godot_cpp/variant/dictionary.hpp>


class Terrain3DDiagnosticsPublisher {
public:
	size_t getRetainedCpuBytes( const t3dDiagnosticInputs_t &rInputs ) const;
	godot::Dictionary getForwardLaneDiagnostics( const t3dDiagnosticInputs_t &rInputs ) const;
	godot::Dictionary getForwardDrawBaselineDiagnostics( const t3dDiagnosticInputs_t &rInputs ) const;
	godot::Dictionary getDiagnostics( const t3dDiagnosticInputs_t &rInputs ) const;
};

#endif // TERRAIN3D_DIAGNOSTICS_PUBLISHER_H
