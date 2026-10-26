// Copyright (c) 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_NATIVE_BUILD_H
#define TERRAIN3D_NATIVE_BUILD_H

#if defined( T3D_EDITOR_NATIVE ) && defined( T3D_RUNTIME_NATIVE )
#error Terrain3D editor and runtime native targets must be mutually exclusive
#endif
#if !defined( T3D_EDITOR_NATIVE ) && !defined( T3D_RUNTIME_NATIVE )
#error Terrain3D requires a selected native build flavor
#endif
#if defined( T3D_RUNTIME_NATIVE ) && defined( T3D_DOC_DATA_ENABLED )
#error Native documentation must not be linked into runtime templates
#endif
#if defined( T3D_RUNTIME_NATIVE ) && defined( T3D_TESTS_ENABLED ) && !defined( T3D_DEBUG_RUNTIME )
#error Native tests are forbidden in release templates
#endif
#if defined( T3D_DEBUG_RUNTIME ) && !defined( T3D_RUNTIME_NATIVE )
#error T3D_DEBUG_RUNTIME requires T3D_RUNTIME_NATIVE
#endif

inline const char *t3dGetNativeBuildFlavor(
	void ) {
#if defined( T3D_EDITOR_NATIVE )
	return "editor";
#elif defined( T3D_DEBUG_RUNTIME )
	return "template_debug";
#else
	return "template_release";
#endif
}
#endif // TERRAIN3D_NATIVE_BUILD_H
