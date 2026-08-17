// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_DIAGNOSTICS_H
#define TERRAIN3D_DIAGNOSTICS_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "Terrain3DConstants.h"


class T3DMemoryStats : public RefCounted {
	using Terrain3DMemoryStats = T3DMemoryStats;
	GDCLASS( Terrain3DMemoryStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	int64_t getConfiguredBudgetBytes( void ) const;
	int64_t getCurrentKnownBytes( void ) const;
	int64_t getPeakKnownBytes( void ) const;
	int64_t getFixedBytes( void ) const;
	int64_t getManagedResidentBytes( void ) const;
	int64_t getTransientBytes( void ) const;
	int64_t getKnownHeadroomBytes( void ) const;
	bool isOvercommitted( void ) const;
	String getPressureState( void ) const;
	Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

class T3DShaderTierStats : public RefCounted {
	using Terrain3DShaderTierStats = T3DShaderTierStats;
	GDCLASS( Terrain3DShaderTierStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	int getTier( void ) const;
	String getVariantSignature( void ) const;
	int64_t getShaderRid( void ) const;
	bool isCompileValid( void ) const;
	int getReflectedParameterCount( void ) const;
	bool isFallbackToNear( void ) const;
	double getRangeStartM( void ) const;
	double getRangeEndM( void ) const;
	bool isRangeUnbounded( void ) const;
	int getModeledMajorTextureFetches( void ) const;
	Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

class T3DCacheStats : public RefCounted {
	using Terrain3DCacheStats = T3DCacheStats;
	GDCLASS( Terrain3DCacheStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	String getCacheName( void ) const;
	int64_t getResident( void ) const;
	int64_t getEffectiveCapacity( void ) const;
	int64_t getCurrentBytes( void ) const;
	int64_t getPeakBytes( void ) const;
	int64_t getHits( void ) const;
	int64_t getMisses( void ) const;
	double getHitRate( void ) const;
	String getLimitingConstraint( void ) const;
	Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

class T3DFrameStats : public RefCounted {
	using Terrain3DFrameStats = T3DFrameStats;
	GDCLASS( Terrain3DFrameStats, RefCounted );
	CLASS_NAME();

	Dictionary m_values;

public:
	void setValues( const Dictionary &rValues ) { m_values = rValues.duplicate( true ); }
	int getDiagnosticsVersion( void ) const { return TERRAIN3D_DIAGNOSTICS_VERSION; }
	int64_t getSequence( void ) const;
	int64_t getPhysicsTotalUsec( void ) const;
	int64_t getProfilerCollectionUsec( void ) const;
	Dictionary getValues( void ) const { return m_values.duplicate( true ); }
	Dictionary toDictionary( void ) const { return getValues(); }

protected:
	static void _bind_methods( void );
};

using Terrain3DMemoryStats = T3DMemoryStats;
using Terrain3DShaderTierStats = T3DShaderTierStats;
using Terrain3DCacheStats = T3DCacheStats;
using Terrain3DFrameStats = T3DFrameStats;

#endif // TERRAIN3D_DIAGNOSTICS_H
