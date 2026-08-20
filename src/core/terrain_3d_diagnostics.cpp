// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_diagnostics.h"

#include <godot_cpp/core/class_db.hpp>

namespace {
int64_t t3dDiagInt( const Dictionary &rValues, const StringName &rKey ) {
	return ( int64_t ) ( rValues.get( rKey, ( int64_t ) ( 0 ) ) );
}
double t3dDiagFloat( const Dictionary &rValues, const StringName &rKey ) {
	return ( double ) ( rValues.get( rKey, 0.0 ) );
}
bool t3dDiagBool( const Dictionary &rValues, const StringName &rKey ) {
	return ( bool ) ( rValues.get( rKey, false ) );
}
String t3dDiagString( const Dictionary &rValues, const StringName &rKey ) {
	return String( rValues.get( rKey, String() ) );
}
}

int64_t Terrain3DMemoryStats::getConfiguredBudgetBytes( void ) const { return t3dDiagInt( m_values, "configured_budget_bytes" ); }
int64_t Terrain3DMemoryStats::getCurrentKnownBytes( void ) const { return t3dDiagInt( m_values, "current_known_bytes" ); }
int64_t Terrain3DMemoryStats::getPeakKnownBytes( void ) const { return t3dDiagInt( m_values, "peak_known_bytes" ); }
int64_t Terrain3DMemoryStats::getFixedBytes( void ) const { return t3dDiagInt( m_values, "fixed_bytes" ); }
int64_t Terrain3DMemoryStats::getManagedResidentBytes( void ) const { return t3dDiagInt( m_values, "managed_resident_bytes" ); }
int64_t Terrain3DMemoryStats::getTransientBytes( void ) const { return t3dDiagInt( m_values, "transient_bytes" ); }
int64_t Terrain3DMemoryStats::getKnownHeadroomBytes( void ) const { return t3dDiagInt( m_values, "known_headroom_bytes" ); }
bool Terrain3DMemoryStats::isOvercommitted( void ) const { return t3dDiagBool( m_values, "overcommitted" ); }
String Terrain3DMemoryStats::getPressureState( void ) const { return t3dDiagString( m_values, "pressure_state" ); }

int Terrain3DShaderTierStats::getTier( void ) const { return ( int ) ( t3dDiagInt( m_values, "tier" ) ); }
String Terrain3DShaderTierStats::getVariantSignature( void ) const { return t3dDiagString( m_values, "variant_signature" ); }
int64_t Terrain3DShaderTierStats::getShaderRid( void ) const { return t3dDiagInt( m_values, "shader_rid" ); }
bool Terrain3DShaderTierStats::isCompileValid( void ) const { return t3dDiagBool( m_values, "compile_valid" ); }
int Terrain3DShaderTierStats::getReflectedParameterCount( void ) const { return ( int ) ( t3dDiagInt( m_values, "reflected_parameter_count" ) ); }
bool Terrain3DShaderTierStats::isFallbackToNear( void ) const { return t3dDiagBool( m_values, "fallback_to_near" ); }
double Terrain3DShaderTierStats::getRangeStartM( void ) const { return t3dDiagFloat( m_values, "range_start_m" ); }
double Terrain3DShaderTierStats::getRangeEndM( void ) const { return t3dDiagFloat( m_values, "range_end_m" ); }
bool Terrain3DShaderTierStats::isRangeUnbounded( void ) const { return t3dDiagBool( m_values, "range_unbounded" ); }
int Terrain3DShaderTierStats::getModeledMajorTextureFetches( void ) const { return ( int ) ( t3dDiagInt( m_values, "modeled_major_texture_fetches" ) ); }

String Terrain3DCacheStats::getCacheName( void ) const { return t3dDiagString( m_values, "cache_name" ); }
int64_t Terrain3DCacheStats::getResident( void ) const { return t3dDiagInt( m_values, "resident" ); }
int64_t Terrain3DCacheStats::getEffectiveCapacity( void ) const { return t3dDiagInt( m_values, "effective_capacity" ); }
int64_t Terrain3DCacheStats::getCurrentBytes( void ) const { return t3dDiagInt( m_values, "current_bytes" ); }
int64_t Terrain3DCacheStats::getPeakBytes( void ) const { return t3dDiagInt( m_values, "peak_bytes" ); }
int64_t Terrain3DCacheStats::getHits( void ) const { return t3dDiagInt( m_values, "hits" ); }
int64_t Terrain3DCacheStats::getMisses( void ) const { return t3dDiagInt( m_values, "misses" ); }
double Terrain3DCacheStats::getHitRate( void ) const { return t3dDiagFloat( m_values, "hit_rate" ); }
String Terrain3DCacheStats::getLimitingConstraint( void ) const { return t3dDiagString( m_values, "limiting_constraint" ); }

int64_t Terrain3DFrameStats::getSequence( void ) const { return t3dDiagInt( m_values, "sequence" ); }
int64_t Terrain3DFrameStats::getPhysicsTotalUsec( void ) const { return t3dDiagInt( m_values, "physics_total_usec" ); }
int64_t Terrain3DFrameStats::getProfilerCollectionUsec( void ) const { return t3dDiagInt( m_values, "profiler_collection_usec" ); }

#define T3D_BIND_DIAG_METHOD( cls, method ) ClassDB::bind_method( D_METHOD( #method ), &cls::method )

void Terrain3DMemoryStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getConfiguredBudgetBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getCurrentKnownBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getPeakKnownBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getFixedBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getManagedResidentBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getTransientBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getKnownHeadroomBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, isOvercommitted );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, getPressureState );
	T3D_BIND_DIAG_METHOD( Terrain3DMemoryStats, toDictionary );
}

void Terrain3DShaderTierStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getTier );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getVariantSignature );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getShaderRid );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, isCompileValid );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getReflectedParameterCount );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, isFallbackToNear );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getRangeStartM );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getRangeEndM );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, isRangeUnbounded );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, getModeledMajorTextureFetches );
	T3D_BIND_DIAG_METHOD( Terrain3DShaderTierStats, toDictionary );
}

void Terrain3DCacheStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getCacheName );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getResident );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getEffectiveCapacity );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getCurrentBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getPeakBytes );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getHits );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getMisses );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getHitRate );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, getLimitingConstraint );
	T3D_BIND_DIAG_METHOD( Terrain3DCacheStats, toDictionary );
}

void Terrain3DFrameStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( Terrain3DFrameStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( Terrain3DFrameStats, getSequence );
	T3D_BIND_DIAG_METHOD( Terrain3DFrameStats, getPhysicsTotalUsec );
	T3D_BIND_DIAG_METHOD( Terrain3DFrameStats, getProfilerCollectionUsec );
	T3D_BIND_DIAG_METHOD( Terrain3DFrameStats, toDictionary );
}

#undef T3D_BIND_DIAG_METHOD
