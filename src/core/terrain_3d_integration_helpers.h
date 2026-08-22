// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#include "../Terrain3DConstants.h"
#include "../streaming/terrain_3d_performance_profile.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

static inline int32_t t3dDivideFloor( const int32_t nValue, const int32_t nDivisor ) {
	int32_t nQuotient = nValue / nDivisor;
	if ( nValue % nDivisor < 0 ) {
		--nQuotient;
	}
	return nQuotient;
}

static inline void t3dAppendRuntimeProblem( Array &rProblems,
		const String &rSeverity,
		const String &rCode,
		const String &rSubsystem,
		const String &rMessage,
		const int64_t nValue ) {
	Dictionary problem;
	problem["severity"] = rSeverity;
	problem["code"] = rCode;
	problem["subsystem"] = rSubsystem;
	problem["message"] = rMessage;
	problem["value"] = nValue;
	rProblems.push_back( problem );
}

static inline Variant t3dPerformanceProfileValue( const t3dPerformanceProfileSetting_t &rSetting ) {
	switch ( rSetting.m_kind ) {
		case T3D_PROFILE_VALUE_INT:
			return ( int64_t )( rSetting.m_nIntValue );
		case T3D_PROFILE_VALUE_FLOAT:
			return rSetting.m_fFloatValue;
		case T3D_PROFILE_VALUE_BOOL:
			return rSetting.m_bBoolValue;
	}
	return Variant();
}

static inline bool t3dPerformanceProfileValueMatches( const Variant &rCurrent,
		const t3dPerformanceProfileSetting_t &rSetting ) {
	switch ( rSetting.m_kind ) {
		case T3D_PROFILE_VALUE_INT:
			return ( int64_t )( rCurrent ) == rSetting.m_nIntValue;
		case T3D_PROFILE_VALUE_FLOAT:
			return Math::is_equal_approx( ( double )( rCurrent ), rSetting.m_fFloatValue );
		case T3D_PROFILE_VALUE_BOOL:
			return ( bool )( rCurrent ) == rSetting.m_bBoolValue;
	}
	return false;
}

static inline String t3dPerformanceProfileOwnerName( const t3dPerformanceProfileOwner_t owner ) {
	return owner == T3D_PROFILE_OWNER_MATERIAL ? "Terrain3DMaterial" : "Terrain3D";
}

#define TERRAIN3D_ALIGN_BAKE_TASK_OFFSET( value ) \
	( ( ( value ) + s_nBakeTaskAlignmentElements - 1u ) & ~( s_nBakeTaskAlignmentElements - 1u ) )
