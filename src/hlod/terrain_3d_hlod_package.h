// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once
#ifndef TERRAIN3D_HLOD_PACKAGE_H
#define TERRAIN3D_HLOD_PACKAGE_H

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/resource.hpp>
#include "Terrain3DConstants.h"
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#if defined( GDEXTENSION ) && !defined( GODOT_MODULE )
using namespace godot;
#endif

class Terrain3DHLODPackage : public Resource {
	GDCLASS( Terrain3DHLODPackage, Resource );
	CLASS_NAME();

	int m_nLevel = 1;
	Vector2i m_clusterLocation = V2I_ZERO;
	Vector2i m_regionMin = V2I_ZERO;
	Vector2i m_regionMax = V2I_ZERO;
	AABB m_localBounds;
	Ref<ArrayMesh> m_pMesh;
	real_t m_fGeometricError = 1.0f;
	int64_t m_nSourceSignature = 0;
	int64_t m_nBuildSignature = 0;
	int64_t m_nSourceTriangles = 0;
	int64_t m_nProxyTriangles = 0;
	int64_t m_nEstimatedGpuBytes = 0;
	int64_t m_nBuiltUnix = 0;

public:
	void setLevel( int nLevel );
	int getLevel( void ) const { return m_nLevel; }
	void setClusterLocation( const Vector2i &rLocation );
	Vector2i getClusterLocation( void ) const { return m_clusterLocation; }
	void setRegionMin( const Vector2i &rLocation );
	Vector2i getRegionMin( void ) const { return m_regionMin; }
	void setRegionMax( const Vector2i &rLocation );
	Vector2i getRegionMax( void ) const { return m_regionMax; }
	void setLocalBounds( const AABB &rBounds );
	AABB getLocalBounds( void ) const { return m_localBounds; }
	void setMesh( const Ref<ArrayMesh> &rMesh );
	Ref<ArrayMesh> getMesh( void ) const { return m_pMesh; }
	void setGeometricError( real_t fError );
	real_t getGeometricError( void ) const { return m_fGeometricError; }
	void setSourceSignature( int64_t nSignature );
	int64_t getSourceSignature( void ) const { return m_nSourceSignature; }
	void setBuildSignature( int64_t nSignature );
	int64_t getBuildSignature( void ) const { return m_nBuildSignature; }
	void setSourceTriangles( int64_t nCount );
	int64_t getSourceTriangles( void ) const { return m_nSourceTriangles; }
	void setProxyTriangles( int64_t nCount );
	int64_t getProxyTriangles( void ) const { return m_nProxyTriangles; }
	void setEstimatedGpuBytes( int64_t nBytes );
	int64_t getEstimatedGpuBytes( void ) const { return m_nEstimatedGpuBytes; }
	void setBuiltUnix( int64_t nUnix );
	int64_t getBuiltUnix( void ) const { return m_nBuiltUnix; }

protected:
	static void _bind_methods( void );
};
#endif
