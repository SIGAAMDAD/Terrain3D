// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_REGISTER_TYPES_H
#define TERRAIN3D_REGISTER_TYPES_H

#ifdef GDEXTENSION
#include <godot_cpp/godot.hpp>
using namespace godot;
#else
#include "modules/register_module_types.h"
#endif

// One shared ClassDB registry; the editor adds authoring-only classes.
void t3dRegisterRuntimeTypes( void );
#if defined( T3D_EDITOR_NATIVE )
void t3dRegisterEditorTypes( void );
#endif

// Compile-time flavor contract shared by Terrain3D and Terrain3DUtil.
// Each process loads one complete GDExtension binary.
#include "core/config/terrain_3d_native_build.h"

// Generated documentation participates in the same module initialization contract.
void t3dRegisterGeneratedDocData( void );

// NOTE: These have module ending for custom module build compatibility.
void t3dInitializeTerrain3DModule( ModuleInitializationLevel level );
void t3dUninitializeTerrain3DModule( ModuleInitializationLevel level );

#endif // TERRAIN3D_REGISTER_TYPES_H
