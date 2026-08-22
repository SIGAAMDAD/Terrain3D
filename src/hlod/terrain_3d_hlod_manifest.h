// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once
#ifndef TERRAIN3D_HLOD_MANIFEST_H
#define TERRAIN3D_HLOD_MANIFEST_H

#include "Terrain3DConstants.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class Terrain3DHLODManifest : public Resource {
	GDCLASS( Terrain3DHLODManifest, Resource );
	CLASS_NAME();

	int m_nFormat = 1;
	int m_nRegionMapSize = 64;
	int m_nRegionSize = 256;
	real_t m_fVertexSpacing = 1.0f;
	int m_nMaxLevel = 6;
	int64_t m_nBuildSignature = 0;
	int64_t m_nBuiltUnix = 0;
	Array m_entries;
	PackedByteArray m_instancerMinLevels;
	PackedInt32Array m_nodeRegionIndices;
	PackedInt64Array m_nodeSpawnIds;
	PackedByteArray m_nodeMinLevels;
	PackedByteArray m_runtimeBlob;

public:
	EA_FORCE_INLINE void setFormat( int nValue ) {
		const int value = MAX( nValue, 1 );
		if ( m_nFormat == value ) {
			return;
		}
		m_nFormat = value;
		emit_changed();
	}
	EA_FORCE_INLINE int getFormat( void ) const {
		return m_nFormat;
	}
	EA_FORCE_INLINE void setRegionMapSize( int nValue ) {
		const int value = MAX( nValue, 1 );
		if ( m_nRegionMapSize == value ) {
			return;
		}
		m_nRegionMapSize = value;
		emit_changed();
	}
	EA_FORCE_INLINE int getRegionMapSize( void ) const {
		return m_nRegionMapSize;
	}
	EA_FORCE_INLINE void setRegionSize( int nValue ) {
		const int value = MAX( nValue, 1 );
		if ( m_nRegionSize == value ) {
			return;
		}
		m_nRegionSize = value;
		emit_changed();
	}
	EA_FORCE_INLINE int getRegionSize( void ) const {
		return m_nRegionSize;
	}
	EA_FORCE_INLINE void setVertexSpacing( real_t fValue ) {
		const real_t value = MAX( fValue, 0.001f );
		if ( m_fVertexSpacing == value ) {
			return;
		}
		m_fVertexSpacing = value;
		emit_changed();
	}
	EA_FORCE_INLINE real_t getVertexSpacing( void ) const {
		return m_fVertexSpacing;
	}
	EA_FORCE_INLINE void setMaxLevel( int nValue ) {
		const int value = CLAMP( nValue, 1, 6 );
		if ( m_nMaxLevel == value ) {
			return;
		}
		m_nMaxLevel = value;
		emit_changed();
	}
	EA_FORCE_INLINE int getMaxLevel( void ) const {
		return m_nMaxLevel;
	}
	EA_FORCE_INLINE void setBuildSignature( int64_t nValue ) {
		if ( m_nBuildSignature == nValue ) {
			return;
		}
		m_nBuildSignature = nValue;
		emit_changed();
	}
	EA_FORCE_INLINE int64_t getBuildSignature( void ) const {
		return m_nBuildSignature;
	}
	EA_FORCE_INLINE void setBuiltUnix( int64_t nValue ) {
		const int64_t value = MAX<int64_t>( nValue, 0 );
		if ( m_nBuiltUnix == value ) {
			return;
		}
		m_nBuiltUnix = value;
		emit_changed();
	}
	EA_FORCE_INLINE int64_t getBuiltUnix( void ) const {
		return m_nBuiltUnix;
	}
	EA_FORCE_INLINE void setEntries( const Array &rValue ) {
		m_entries = rValue.duplicate( true );
		m_runtimeBlob.clear();
		emit_changed();
	}
	EA_FORCE_INLINE Array getEntries( void ) const {
		return m_entries.duplicate( true );
	}
	EA_FORCE_INLINE const Array &getEntriesInternal( void ) const {
		return m_entries;
	}
	EA_FORCE_INLINE void setInstancerMinLevels( const PackedByteArray &rValue ) {
		m_instancerMinLevels = rValue;
		m_runtimeBlob.clear();
		emit_changed();
	}
	EA_FORCE_INLINE PackedByteArray getInstancerMinLevels( void ) const {
		return m_instancerMinLevels;
	}
	EA_FORCE_INLINE void setNodeRegionIndices( const PackedInt32Array &rValue ) {
		m_nodeRegionIndices = rValue;
		m_runtimeBlob.clear();
		emit_changed();
	}
	EA_FORCE_INLINE PackedInt32Array getNodeRegionIndices( void ) const {
		return m_nodeRegionIndices;
	}
	EA_FORCE_INLINE void setNodeSpawnIds( const PackedInt64Array &rValue ) {
		m_nodeSpawnIds = rValue;
		m_runtimeBlob.clear();
		emit_changed();
	}
	EA_FORCE_INLINE PackedInt64Array getNodeSpawnIds( void ) const {
		return m_nodeSpawnIds;
	}
	EA_FORCE_INLINE void setNodeMinLevels( const PackedByteArray &rValue ) {
		m_nodeMinLevels = rValue;
		m_runtimeBlob.clear();
		emit_changed();
	}
	EA_FORCE_INLINE PackedByteArray getNodeMinLevels( void ) const {
		return m_nodeMinLevels;
	}
	EA_FORCE_INLINE void setRuntimeBlob( const PackedByteArray &rValue ) {
		m_runtimeBlob = rValue;
		emit_changed();
	}
	EA_FORCE_INLINE PackedByteArray getRuntimeBlob( void ) const {
		return m_runtimeBlob;
	}
	bool compileRuntimeBlob( void );
	EA_FORCE_INLINE int getEntryCount( void ) const {
		return m_entries.size();
	}

protected:
	static void _bind_methods( void );
};
#endif
