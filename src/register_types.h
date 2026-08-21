// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_REGISTER_TYPES_H
#define TERRAIN3D_REGISTER_TYPES_H

#ifdef GDEXTENSION
#include <godot_cpp/godot.hpp>
using namespace godot;
#else
#include "modules/register_module_types.h"
#endif

// NOTE: These have module ending for custom module build compatibility.
void t3dInitializeTerrain3DModule( ModuleInitializationLevel level );
void t3dUninitializeTerrain3DModule( ModuleInitializationLevel level );

#endif // TERRAIN3D_REGISTER_TYPES_H
