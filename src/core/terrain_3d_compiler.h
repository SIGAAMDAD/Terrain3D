// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef EA_FORCE_INLINE
#if defined( _MSC_VER )
#define EA_FORCE_INLINE __forceinline
#elif defined( __GNUC__ ) || defined( __clang__ )
#define EA_FORCE_INLINE inline __attribute__( ( always_inline ) )
#else
#define EA_FORCE_INLINE inline
#endif
#endif
