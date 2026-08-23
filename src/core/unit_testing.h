// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef UNIT_TESTING_H
#define UNIT_TESTING_H

#define EXPECT_FALSE( cond ) \
	do { \
		if ( cond ) { \
			UtilityFunctions::print( "FAILED: ", #cond ); \
		} else { \
			UtilityFunctions::print( "PASSED: ", #cond ); \
		} \
	} while ( 0 )

#define EXPECT_TRUE( cond ) \
	do { \
		if ( cond ) { \
			UtilityFunctions::print( "PASSED: ", #cond ); \
		} else { \
			UtilityFunctions::print( "FAILED: ", #cond ); \
		} \
	} while ( 0 )

void t3dTestDiffers( void );
bool t3dTestRdTopology( void );
bool t3dTestRdAbiAndPublication( void );
bool t3dTestRdCullingAndIndirect( void );
bool t3dTestRdContextDiagnostics( void );
bool t3dTestRdTextureLayouts( void );
bool t3dTestRdMaterialAbi( void );
bool t3dRunRdCpuTests( void );

#endif // UNIT_TESTING_H
