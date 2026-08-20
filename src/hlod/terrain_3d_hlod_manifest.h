// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once
#ifndef TERRAIN3D_HLOD_MANIFEST_H
#define TERRAIN3D_HLOD_MANIFEST_H

#include <godot_cpp/classes/resource.hpp>
#include "Terrain3DConstants.h"
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class T3DHLODManifest : public Resource {
	GDCLASS( T3DHLODManifest, Resource );
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
	void setFormat( int nValue ); int getFormat( void ) const { return m_nFormat; }
	void setRegionMapSize( int nValue ); int getRegionMapSize( void ) const { return m_nRegionMapSize; }
	void setRegionSize( int nValue ); int getRegionSize( void ) const { return m_nRegionSize; }
	void setVertexSpacing( real_t fValue ); real_t getVertexSpacing( void ) const { return m_fVertexSpacing; }
	void setMaxLevel( int nValue ); int getMaxLevel( void ) const { return m_nMaxLevel; }
	void setBuildSignature( int64_t nValue ); int64_t getBuildSignature( void ) const { return m_nBuildSignature; }
	void setBuiltUnix( int64_t nValue ); int64_t getBuiltUnix( void ) const { return m_nBuiltUnix; }
	void setEntries( const Array &rValue ); Array getEntries( void ) const { return m_entries.duplicate( true ); }
	const Array &getEntriesInternal( void ) const { return m_entries; }
	void setInstancerMinLevels( const PackedByteArray &rValue ); PackedByteArray getInstancerMinLevels( void ) const { return m_instancerMinLevels; }
	void setNodeRegionIndices( const PackedInt32Array &rValue ); PackedInt32Array getNodeRegionIndices( void ) const { return m_nodeRegionIndices; }
	void setNodeSpawnIds( const PackedInt64Array &rValue ); PackedInt64Array getNodeSpawnIds( void ) const { return m_nodeSpawnIds; }
	void setNodeMinLevels( const PackedByteArray &rValue ); PackedByteArray getNodeMinLevels( void ) const { return m_nodeMinLevels; }
	void setRuntimeBlob( const PackedByteArray &rValue ); PackedByteArray getRuntimeBlob( void ) const { return m_runtimeBlob; }
	bool compileRuntimeBlob( void );
	int getEntryCount( void ) const { return m_entries.size(); }

protected:
	static void _bind_methods( void );
};
#endif
