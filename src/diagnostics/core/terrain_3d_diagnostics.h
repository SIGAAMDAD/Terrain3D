// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_DIAGNOSTICS_H
#define TERRAIN3D_DIAGNOSTICS_H

#include "Terrain3DConstants.h"
#include "core/config/terrain_3d_compiler.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

EA_FORCE_INLINE int64_t t3dDiagnosticsGetInt(
	const Dictionary &rValues,
	const StringName &rKey ) {
	return ( int64_t ) ( rValues.get( rKey, ( int64_t ) ( 0 ) ) );
}
EA_FORCE_INLINE double t3dDiagnosticsGetFloat(
	const Dictionary &rValues,
	const StringName &rKey ) {
	return ( double ) ( rValues.get( rKey, 0.0 ) );
}
EA_FORCE_INLINE bool t3dDiagnosticsGetBool(
	const Dictionary &rValues,
	const StringName &rKey ) {
	return ( bool ) ( rValues.get( rKey, false ) );
}
EA_FORCE_INLINE String t3dDiagnosticsGetString(
	const Dictionary &rValues,
	const StringName &rKey ) {
	return String( rValues.get( rKey, String() ) );
}

inline void t3dAppendRegionCacheActivity(
	godot::Dictionary &rStats,
	int				   nResident,
	int				   nDesired,
	int				   nEvictions ) {
	rStats["resident"] = nResident;
	rStats["desired_last_tick"] = nDesired;
	rStats["evictions_last_tick"] = nEvictions;
}

// CPU and instance load snapshots share this schema. Byte/time counters keep
// their existing signed Godot integer representation; specialized keys stay local.
template< typename T >
inline void t3dAppendRegionLoadCacheStats(
	godot::Dictionary &rStats,
	const T			  &rValues,
	int				   nBudgetMb,
	int				   nDemandLoads ) {
	t3dAppendRegionCacheActivity( rStats,
								  rValues.m_nResident,
								  rValues.m_nDesiredLastTick,
								  rValues.m_nEvictionsLastTick );
	rStats["enabled"] = rValues.m_bEnabled;
	rStats["budget_mb"] = nBudgetMb;
	rStats["capacity"] = rValues.m_nCapacity;
	rStats["loads_last_tick"] = rValues.m_nLoadsLastTick;
	rStats["demand_loads"] = nDemandLoads;
	rStats["load_bytes_last_tick"] = ( int64_t ) rValues.m_nLoadBytesLastTick;
	rStats["service_cpu_usec_last"] = ( int64_t ) rValues.m_nServiceUsecLast;
	rStats["selection_pending"] = rValues.m_bSelectionPending;
}

// Captured values stay private. Both input and exported dictionaries are deep
// copies so nested dictionaries/arrays cannot mutate a published snapshot.
class Terrain3DDiagnosticsSnapshot
{
	Dictionary m_values;

public:
	EA_FORCE_INLINE void setValues(
		const Dictionary &rValues ) {
		m_values = rValues.duplicate( true );
	}
	EA_FORCE_INLINE int getDiagnosticsVersion(
		void ) const {
		return T3D_DIAGNOSTICS_VERSION;
	}
	EA_FORCE_INLINE Dictionary getValues(
		void ) const {
		return m_values.duplicate( true );
	}
	EA_FORCE_INLINE int64_t getInt(
		const StringName &rKey ) const {
		return t3dDiagnosticsGetInt( m_values, rKey );
	}
	EA_FORCE_INLINE double getFloat(
		const StringName &rKey ) const {
		return t3dDiagnosticsGetFloat( m_values, rKey );
	}
	EA_FORCE_INLINE bool getBool(
		const StringName &rKey ) const {
		return t3dDiagnosticsGetBool( m_values, rKey );
	}
	EA_FORCE_INLINE String getString(
		const StringName &rKey ) const {
		return t3dDiagnosticsGetString( m_values, rKey );
	}
};

class Terrain3DMemoryStats : public RefCounted
{
	GDCLASS( Terrain3DMemoryStats, RefCounted );
	CLASS_NAME();

	Terrain3DDiagnosticsSnapshot m_snapshot;

public:
	EA_FORCE_INLINE void setValues(
		const Dictionary &rValues ) {
		m_snapshot.setValues( rValues );
	}
	EA_FORCE_INLINE int getDiagnosticsVersion(
		void ) const {
		return m_snapshot.getDiagnosticsVersion();
	}
	EA_FORCE_INLINE int64_t getConfiguredBudgetBytes(
		void ) const {
		return m_snapshot.getInt( "configured_budget_bytes" );
	}
	EA_FORCE_INLINE int64_t getCurrentKnownBytes(
		void ) const {
		return m_snapshot.getInt( "current_known_bytes" );
	}
	EA_FORCE_INLINE int64_t getPeakKnownBytes(
		void ) const {
		return m_snapshot.getInt( "peak_known_bytes" );
	}
	EA_FORCE_INLINE int64_t getFixedBytes(
		void ) const {
		return m_snapshot.getInt( "fixed_bytes" );
	}
	EA_FORCE_INLINE int64_t getManagedResidentBytes(
		void ) const {
		return m_snapshot.getInt( "managed_resident_bytes" );
	}
	EA_FORCE_INLINE int64_t getTransientBytes(
		void ) const {
		return m_snapshot.getInt( "transient_bytes" );
	}
	EA_FORCE_INLINE int64_t getKnownHeadroomBytes(
		void ) const {
		return m_snapshot.getInt( "known_headroom_bytes" );
	}
	EA_FORCE_INLINE bool isOvercommitted(
		void ) const {
		return m_snapshot.getBool( "overcommitted" );
	}
	EA_FORCE_INLINE String getPressureState(
		void ) const {
		return m_snapshot.getString( "pressure_state" );
	}
	EA_FORCE_INLINE Dictionary getValues(
		void ) const {
		return m_snapshot.getValues();
	}
	EA_FORCE_INLINE Dictionary toDictionary(
		void ) const {
		return getValues();
	}

protected:
	static void _bind_methods( void );
};

class Terrain3DShaderTierStats : public RefCounted
{
	GDCLASS( Terrain3DShaderTierStats, RefCounted );
	CLASS_NAME();

	Terrain3DDiagnosticsSnapshot m_snapshot;

public:
	EA_FORCE_INLINE void setValues(
		const Dictionary &rValues ) {
		m_snapshot.setValues( rValues );
	}
	EA_FORCE_INLINE int getDiagnosticsVersion(
		void ) const {
		return m_snapshot.getDiagnosticsVersion();
	}
	EA_FORCE_INLINE int getTier(
		void ) const {
		return ( int ) ( m_snapshot.getInt( "tier" ) );
	}
	EA_FORCE_INLINE String getVariantSignature(
		void ) const {
		return m_snapshot.getString( "variant_signature" );
	}
	EA_FORCE_INLINE int64_t getShaderRid(
		void ) const {
		return m_snapshot.getInt( "shader_rid" );
	}
	EA_FORCE_INLINE bool isCompileValid(
		void ) const {
		return m_snapshot.getBool( "compile_valid" );
	}
	EA_FORCE_INLINE int getReflectedParameterCount(
		void ) const {
		return ( int ) ( m_snapshot.getInt( "reflected_parameter_count" ) );
	}
	EA_FORCE_INLINE bool isFallbackToNear(
		void ) const {
		return m_snapshot.getBool( "fallback_to_near" );
	}
	EA_FORCE_INLINE double getRangeStartM(
		void ) const {
		return m_snapshot.getFloat( "range_start_m" );
	}
	EA_FORCE_INLINE double getRangeEndM(
		void ) const {
		return m_snapshot.getFloat( "range_end_m" );
	}
	EA_FORCE_INLINE bool isRangeUnbounded(
		void ) const {
		return m_snapshot.getBool( "range_unbounded" );
	}
	EA_FORCE_INLINE int getModeledMajorTextureFetches(
		void ) const {
		return ( int ) ( m_snapshot.getInt( "modeled_major_texture_fetches" ) );
	}
	EA_FORCE_INLINE Dictionary getValues(
		void ) const {
		return m_snapshot.getValues();
	}
	EA_FORCE_INLINE Dictionary toDictionary(
		void ) const {
		return getValues();
	}

protected:
	static void _bind_methods( void );
};

class Terrain3DCacheStats : public RefCounted
{
	GDCLASS( Terrain3DCacheStats, RefCounted );
	CLASS_NAME();

	Terrain3DDiagnosticsSnapshot m_snapshot;

public:
	EA_FORCE_INLINE void setValues(
		const Dictionary &rValues ) {
		m_snapshot.setValues( rValues );
	}
	EA_FORCE_INLINE int getDiagnosticsVersion(
		void ) const {
		return m_snapshot.getDiagnosticsVersion();
	}
	EA_FORCE_INLINE String getCacheName(
		void ) const {
		return m_snapshot.getString( "cache_name" );
	}
	EA_FORCE_INLINE int64_t getResident(
		void ) const {
		return m_snapshot.getInt( "resident" );
	}
	EA_FORCE_INLINE int64_t getEffectiveCapacity(
		void ) const {
		return m_snapshot.getInt( "effective_capacity" );
	}
	EA_FORCE_INLINE int64_t getCurrentBytes(
		void ) const {
		return m_snapshot.getInt( "current_bytes" );
	}
	EA_FORCE_INLINE int64_t getPeakBytes(
		void ) const {
		return m_snapshot.getInt( "peak_bytes" );
	}
	EA_FORCE_INLINE int64_t getHits(
		void ) const {
		return m_snapshot.getInt( "hits" );
	}
	EA_FORCE_INLINE int64_t getMisses(
		void ) const {
		return m_snapshot.getInt( "misses" );
	}
	EA_FORCE_INLINE double getHitRate(
		void ) const {
		return m_snapshot.getFloat( "hit_rate" );
	}
	EA_FORCE_INLINE String getLimitingConstraint(
		void ) const {
		return m_snapshot.getString( "limiting_constraint" );
	}
	EA_FORCE_INLINE Dictionary getValues(
		void ) const {
		return m_snapshot.getValues();
	}
	EA_FORCE_INLINE Dictionary toDictionary(
		void ) const {
		return getValues();
	}

protected:
	static void _bind_methods( void );
};

class Terrain3DFrameStats : public RefCounted
{
	GDCLASS( Terrain3DFrameStats, RefCounted );
	CLASS_NAME();

	Terrain3DDiagnosticsSnapshot m_snapshot;

public:
	EA_FORCE_INLINE void setValues(
		const Dictionary &rValues ) {
		m_snapshot.setValues( rValues );
	}
	EA_FORCE_INLINE int getDiagnosticsVersion(
		void ) const {
		return m_snapshot.getDiagnosticsVersion();
	}
	EA_FORCE_INLINE int64_t getSequence(
		void ) const {
		return m_snapshot.getInt( "sequence" );
	}
	EA_FORCE_INLINE int64_t getPhysicsTotalUsec(
		void ) const {
		return m_snapshot.getInt( "physics_total_usec" );
	}
	EA_FORCE_INLINE int64_t getProfilerCollectionUsec(
		void ) const {
		return m_snapshot.getInt( "profiler_collection_usec" );
	}
	EA_FORCE_INLINE Dictionary getValues(
		void ) const {
		return m_snapshot.getValues();
	}
	EA_FORCE_INLINE Dictionary toDictionary(
		void ) const {
		return getValues();
	}

protected:
	static void _bind_methods( void );
};
#endif // TERRAIN3D_DIAGNOSTICS_H
