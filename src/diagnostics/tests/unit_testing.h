// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_UNIT_TESTING_H
#define TERRAIN3D_UNIT_TESTING_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#define EXPECT_FALSE( cond )                                                                                           \
	do {                                                                                                               \
		if ( cond ) {                                                                                                  \
			UtilityFunctions::print( "FAILED: ", #cond );                                                              \
		} else {                                                                                                       \
			UtilityFunctions::print( "PASSED: ", #cond );                                                              \
		}                                                                                                              \
	} while ( 0 )

#define EXPECT_TRUE( cond )                                                                                            \
	do {                                                                                                               \
		if ( cond ) {                                                                                                  \
			UtilityFunctions::print( "PASSED: ", #cond );                                                              \
		} else {                                                                                                       \
			UtilityFunctions::print( "FAILED: ", #cond );                                                              \
		}                                                                                                              \
	} while ( 0 )

void t3dTestDiffers( void );
bool t3dTestRdTopology( void );
bool t3dTestRdAbiAndPublication( void );
bool t3dTestRdCullingAndIndirect( void );
bool t3dTestRdContextDiagnostics( void );
bool t3dTestRdTextureLayouts( void );
bool t3dTestRdMaterialAbi( void );
bool t3dTestRdInstanceAbi( void );
bool t3dRunRdCpuTests( void );
bool t3dRunCentralizationTests( void );
bool t3dRunMegaVTCacheTests( void );
bool t3dRunVTWarmRepublishTests( void );
bool t3dRunVTWarmGpuTests( void );

godot::Dictionary
t3dRunSystemUnitTests( const godot::String &suiteFilter, const godot::String &caseFilter, bool listOnly );

#endif // TERRAIN3D_UNIT_TESTING_H
