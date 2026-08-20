// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_DIAGNOSTICS_H
#define TERRAIN3D_DIAGNOSTICS_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "core/terrain_3d_compiler.h"
#include "Terrain3DConstants.h"


namespace terrain3d_diagnostics_detail {
EA_FORCE_INLINE int64_t getInt( const Dictionary &rValues, const StringName &rKey ) { return ( int64_t ) ( rValues.get( rKey, ( int64_t ) ( 0 ) ) ); }
EA_FORCE_INLINE double getFloat( const Dictionary &rValues, const StringName &rKey ) { return ( double ) ( rValues.get( rKey, 0.0 ) ); }
EA_FORCE_INLINE bool getBool( const Dictionary &rValues, const StringName &rKey ) { return ( bool ) ( rValues.get( rKey, false ) ); }
EA_FORCE_INLINE String getString( const Dictionary &rValues, const StringName &rKey ) { return String( rValues.get( rKey, String() ) ); }
}

class T3DMemoryStats : public RefCounted {
	GDCLASS( T3DMemoryStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	EA_FORCE_INLINE void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	EA_FORCE_INLINE int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	EA_FORCE_INLINE int64_t getConfiguredBudgetBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "configured_budget_bytes" ); }
	EA_FORCE_INLINE int64_t getCurrentKnownBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "current_known_bytes" ); }
	EA_FORCE_INLINE int64_t getPeakKnownBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "peak_known_bytes" ); }
	EA_FORCE_INLINE int64_t getFixedBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "fixed_bytes" ); }
	EA_FORCE_INLINE int64_t getManagedResidentBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "managed_resident_bytes" ); }
	EA_FORCE_INLINE int64_t getTransientBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "transient_bytes" ); }
	EA_FORCE_INLINE int64_t getKnownHeadroomBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "known_headroom_bytes" ); }
	EA_FORCE_INLINE bool isOvercommitted( void ) const { return terrain3d_diagnostics_detail::getBool( m_values, "overcommitted" ); }
	EA_FORCE_INLINE String getPressureState( void ) const { return terrain3d_diagnostics_detail::getString( m_values, "pressure_state" ); }
	EA_FORCE_INLINE Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	EA_FORCE_INLINE Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

class T3DShaderTierStats : public RefCounted {
	GDCLASS( T3DShaderTierStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	EA_FORCE_INLINE void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	EA_FORCE_INLINE int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	EA_FORCE_INLINE int getTier( void ) const { return ( int ) ( terrain3d_diagnostics_detail::getInt( m_values, "tier" ) ); }
	EA_FORCE_INLINE String getVariantSignature( void ) const { return terrain3d_diagnostics_detail::getString( m_values, "variant_signature" ); }
	EA_FORCE_INLINE int64_t getShaderRid( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "shader_rid" ); }
	EA_FORCE_INLINE bool isCompileValid( void ) const { return terrain3d_diagnostics_detail::getBool( m_values, "compile_valid" ); }
	EA_FORCE_INLINE int getReflectedParameterCount( void ) const { return ( int ) ( terrain3d_diagnostics_detail::getInt( m_values, "reflected_parameter_count" ) ); }
	EA_FORCE_INLINE bool isFallbackToNear( void ) const { return terrain3d_diagnostics_detail::getBool( m_values, "fallback_to_near" ); }
	EA_FORCE_INLINE double getRangeStartM( void ) const { return terrain3d_diagnostics_detail::getFloat( m_values, "range_start_m" ); }
	EA_FORCE_INLINE double getRangeEndM( void ) const { return terrain3d_diagnostics_detail::getFloat( m_values, "range_end_m" ); }
	EA_FORCE_INLINE bool isRangeUnbounded( void ) const { return terrain3d_diagnostics_detail::getBool( m_values, "range_unbounded" ); }
	EA_FORCE_INLINE int getModeledMajorTextureFetches( void ) const { return ( int ) ( terrain3d_diagnostics_detail::getInt( m_values, "modeled_major_texture_fetches" ) ); }
	EA_FORCE_INLINE Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	EA_FORCE_INLINE Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

class T3DCacheStats : public RefCounted {
	GDCLASS( T3DCacheStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	EA_FORCE_INLINE void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	EA_FORCE_INLINE int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	EA_FORCE_INLINE String getCacheName( void ) const { return terrain3d_diagnostics_detail::getString( m_values, "cache_name" ); }
	EA_FORCE_INLINE int64_t getResident( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "resident" ); }
	EA_FORCE_INLINE int64_t getEffectiveCapacity( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "effective_capacity" ); }
	EA_FORCE_INLINE int64_t getCurrentBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "current_bytes" ); }
	EA_FORCE_INLINE int64_t getPeakBytes( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "peak_bytes" ); }
	EA_FORCE_INLINE int64_t getHits( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "hits" ); }
	EA_FORCE_INLINE int64_t getMisses( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "misses" ); }
	EA_FORCE_INLINE double getHitRate( void ) const { return terrain3d_diagnostics_detail::getFloat( m_values, "hit_rate" ); }
	EA_FORCE_INLINE String getLimitingConstraint( void ) const { return terrain3d_diagnostics_detail::getString( m_values, "limiting_constraint" ); }
	EA_FORCE_INLINE Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	EA_FORCE_INLINE Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

class T3DFrameStats : public RefCounted {
	GDCLASS( T3DFrameStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	EA_FORCE_INLINE void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	EA_FORCE_INLINE int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	EA_FORCE_INLINE int64_t getSequence( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "sequence" ); }
	EA_FORCE_INLINE int64_t getPhysicsTotalUsec( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "physics_total_usec" ); }
	EA_FORCE_INLINE int64_t getProfilerCollectionUsec( void ) const { return terrain3d_diagnostics_detail::getInt( m_values, "profiler_collection_usec" ); }
	EA_FORCE_INLINE Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	EA_FORCE_INLINE Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_DIAGNOSTICS_H
