// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// EASTL is mostly template/header based, but its assertion dispatch and prime
// hash-table rehash policy require one linked runtime translation unit. Keep
// these definitions in Terrain3D so they are built with the same custom
// allocator/platform configuration as every EASTL container in the extension.
#include "eastl_config.h"

#include <cstdio>
#include <cstdlib>

#include <EASTL/internal/config.h>

namespace eastl {
namespace {

void terrain3dEastlAssertDefault( void *pInstructionPointer, const char *pszExpression, void *pContext );

EASTL_AssertionFailureFunctionEx assertion_failure_function = terrain3dEastlAssertDefault;
void *assertion_failure_context = nullptr;
EASTL_AssertionFailureFunction assertion_failure_legacy_function = nullptr;

void terrain3dEastlAssertLegacyAdapter( void *pInstructionPointer, const char *pszExpression, void *pContext ) {
	EA_UNUSED( pInstructionPointer );
	EA_UNUSED( pContext );
	if ( assertion_failure_legacy_function ) {
		assertion_failure_legacy_function( pszExpression, assertion_failure_context );
	}
}

void terrain3dEastlAssertDefault( void *pInstructionPointer, const char *pszExpression, void *pContext ) {
	EA_UNUSED( pInstructionPointer );
	EA_UNUSED( pContext );
	AssertionFailureFunctionDefault( pszExpression, nullptr );
}

} // namespace

void SetAssertionFailureFunction( EASTL_AssertionFailureFunction function, void *pContext ) {
	assertion_failure_legacy_function = function;
	assertion_failure_context = pContext;
	assertion_failure_function = function ? terrain3dEastlAssertLegacyAdapter : terrain3dEastlAssertDefault;
}

void SetAssertionFailureFunction( EASTL_AssertionFailureFunctionEx function, void *pContext ) {
	assertion_failure_legacy_function = nullptr;
	assertion_failure_context = pContext;
	assertion_failure_function = function ? function : terrain3dEastlAssertDefault;
}

void AssertionFailureFunctionDefault( const char *pszExpression, void *pContext ) {
	EA_UNUSED( pContext );
#if EASTL_ASSERT_ENABLED
	std::fprintf( stderr, "Terrain3D EASTL assertion failed: %s\n", pszExpression ? pszExpression : "<null>" );
	std::fflush( stderr );
	std::abort();
#else
	EA_UNUSED( pszExpression );
#endif
}

void AssertionFailure( const char *pszExpression ) {
	AssertionFailure( nullptr, pszExpression );
}

void AssertionFailure( void *pInstructionPointer, const char *pszExpression ) {
	if ( assertion_failure_function ) {
		assertion_failure_function( pInstructionPointer, pszExpression, assertion_failure_context );
	}
}

} // namespace eastl

// EASTL's prime rehash policy and shared empty bucket array are implemented in
// its runtime source rather than headers. Import only that implementation here;
// compiling it separately as well would create duplicate definitions.
#include "../thirdparty/EASTL/source/hashtable.cpp"
