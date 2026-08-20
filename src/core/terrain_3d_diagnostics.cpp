// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_diagnostics.h"

#include <godot_cpp/core/class_db.hpp>

#define T3D_BIND_DIAG_METHOD( cls, method ) ClassDB::bind_method( D_METHOD( #method ), &cls::method )

void T3DMemoryStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getConfiguredBudgetBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getCurrentKnownBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getPeakKnownBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getFixedBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getManagedResidentBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getTransientBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getKnownHeadroomBytes );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, isOvercommitted );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, getPressureState );
	T3D_BIND_DIAG_METHOD( T3DMemoryStats, toDictionary );
}

void T3DShaderTierStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getTier );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getVariantSignature );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getShaderRid );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, isCompileValid );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getReflectedParameterCount );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, isFallbackToNear );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getRangeStartM );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getRangeEndM );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, isRangeUnbounded );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, getModeledMajorTextureFetches );
	T3D_BIND_DIAG_METHOD( T3DShaderTierStats, toDictionary );
}

void T3DCacheStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getCacheName );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getResident );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getEffectiveCapacity );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getCurrentBytes );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getPeakBytes );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getHits );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getMisses );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getHitRate );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, getLimitingConstraint );
	T3D_BIND_DIAG_METHOD( T3DCacheStats, toDictionary );
}

void T3DFrameStats::_bind_methods( void ) {
	T3D_BIND_DIAG_METHOD( T3DFrameStats, getDiagnosticsVersion );
	T3D_BIND_DIAG_METHOD( T3DFrameStats, getSequence );
	T3D_BIND_DIAG_METHOD( T3DFrameStats, getPhysicsTotalUsec );
	T3D_BIND_DIAG_METHOD( T3DFrameStats, getProfilerCollectionUsec );
	T3D_BIND_DIAG_METHOD( T3DFrameStats, toDictionary );
}

#undef T3D_BIND_DIAG_METHOD
