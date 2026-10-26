// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "diagnostics/core/terrain_3d_diagnostics.h"

#include <godot_cpp/core/class_db.hpp>

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
