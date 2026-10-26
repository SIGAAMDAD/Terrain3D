// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

// EASTL is mostly template/header based, but its assertion dispatch and prime
// hash-table rehash policy require one linked runtime translation unit. Keep
// these definitions in Terrain3D so they are built with the same custom
// allocator/platform configuration as every EASTL container in the extension.
#include "eastl_config.h"

#include <EASTL/internal/config.h>
#include <stdio.h>
#include <stdlib.h>

namespace eastl
{
EA_FORCE_INLINE static void t3dEastlRuntimeEastlAssertDefault(
	void	   *pInstructionPointer,
	const char *pszExpression,
	void	   *pContext ) {
	EA_UNUSED( pInstructionPointer );
	EA_UNUSED( pContext );
	AssertionFailureFunctionDefault( pszExpression, NULL );
}

EASTL_AssertionFailureFunctionEx s_pfnAssertionFailure = t3dEastlRuntimeEastlAssertDefault;
void							*s_pAssertionFailureContext = NULL;
EASTL_AssertionFailureFunction	 s_pfnLegacyAssertionFailure = NULL;

static void t3dEastlRuntimeEastlAssertLegacyAdapter(
	void	   *pInstructionPointer,
	const char *pszExpression,
	void	   *pContext ) {
	EA_UNUSED( pInstructionPointer );
	EA_UNUSED( pContext );
	if ( s_pfnLegacyAssertionFailure ) {
		s_pfnLegacyAssertionFailure( pszExpression, s_pAssertionFailureContext );
	}
}

void SetAssertionFailureFunction(
	EASTL_AssertionFailureFunction function,
	void						  *pContext ) {
	s_pfnLegacyAssertionFailure = function;
	s_pAssertionFailureContext = pContext;
	s_pfnAssertionFailure = function ? t3dEastlRuntimeEastlAssertLegacyAdapter : t3dEastlRuntimeEastlAssertDefault;
}

void SetAssertionFailureFunction(
	EASTL_AssertionFailureFunctionEx function,
	void							*pContext ) {
	s_pfnLegacyAssertionFailure = NULL;
	s_pAssertionFailureContext = pContext;
	s_pfnAssertionFailure = function ? function : t3dEastlRuntimeEastlAssertDefault;
}

void AssertionFailureFunctionDefault(
	const char *pszExpression,
	void	   *pContext ) {
	EA_UNUSED( pContext );
#if EASTL_ASSERT_ENABLED
	fprintf( stderr, "Terrain3D EASTL assertion failed: %s\n", pszExpression ? pszExpression : "<null>" );
	fflush( stderr );
	abort();
#else
	EA_UNUSED( pszExpression );
#endif
}

void AssertionFailure(
	const char *pszExpression ) {
	AssertionFailure( NULL, pszExpression );
}

void AssertionFailure(
	void	   *pInstructionPointer,
	const char *pszExpression ) {
	if ( s_pfnAssertionFailure ) {
		s_pfnAssertionFailure( pInstructionPointer, pszExpression, s_pAssertionFailureContext );
	}
}

} // namespace eastl

// EASTL's prime rehash policy and shared empty bucket array are implemented in
// its runtime source rather than headers. Import only that implementation here;
// compiling it separately as well would create duplicate definitions.
#include "../../thirdparty/EASTL/source/hashtable.cpp"
