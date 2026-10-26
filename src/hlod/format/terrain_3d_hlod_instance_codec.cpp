// Copyright (c) 2026 Terrain3D contributors.
#include "Terrain3DTypes.h"
#include "hlod/format/terrain_3d_hlod_instance_codec.h"
#include "core/utility/terrain_3d_binary_io.h"
#include <math.h>
#include <string.h>

#define T3D_HLOD_GROUP_HEADER_BYTES ( 20u )
#define T3D_HLOD_GROUP_PROTOTYPE_BYTES ( 52u )
#define T3D_HLOD_GROUP_INSTANCE_BYTES ( 56u )
#define T3D_HLOD_GROUP_MAX_PROTOTYPES ( 8192u )
#define T3D_HLOD_GROUP_MAX_INSTANCES ( 1048576u )

static uint32_t t3dHlodGroupChecksum( const uint8_t *pBytes, size_t nBytes )
{
    uint32_t nHash = 2166136261u;
    for ( size_t i = 0u; i < nBytes; ++i ) {
        nHash = ( nHash ^ pBytes[i] ) * 16777619u;
    }
    return nHash;
}

static bool t3dHlodGroupFinite( const float *pValues, uint32_t nCount )
{
    for ( uint32_t i = 0u; i < nCount; ++i ) {
        if ( !isfinite( pValues[i] ) ) { return false; }
    }
    return true;
}

static bool t3dHlodGroupValid( const t3dHLODGroupData_t &rGroup )
{
    if ( rGroup.m_prototypes.empty() || rGroup.m_instances.empty() ||
        rGroup.m_prototypes.size() > T3D_HLOD_GROUP_MAX_PROTOTYPES ||
        rGroup.m_instances.size() > T3D_HLOD_GROUP_MAX_INSTANCES ) { return false; }
    for ( size_t i = 0; i < rGroup.m_prototypes.size(); ++i ) {
        const t3dHLODGroupPrototype_t &p = rGroup.m_prototypes[i];
        if ( !p.m_nGeometryKey || !p.m_nMaterialKey || !isfinite( p.m_fGeometricError ) ||
            p.m_fGeometricError < 0.0f || !t3dHlodGroupFinite( p.m_fBounds, 6u ) ||
            p.m_fBounds[3] < 0.0f || p.m_fBounds[4] < 0.0f || p.m_fBounds[5] < 0.0f ) { return false; }
    }
    for ( size_t i = 0; i < rGroup.m_instances.size(); ++i ) {
        const t3dHLODGroupInstance_t &p = rGroup.m_instances[i];
        if ( p.m_nPrototype >= rGroup.m_prototypes.size() || !t3dHlodGroupFinite( p.m_fAffine, 12u ) ) {
            return false;
        }
    }
    return true;
}

PackedByteArray Terrain3DHLODInstanceCodec::encode( const t3dHLODGroupData_t &rSource )
{
    PackedByteArray output;
    if ( !t3dHlodGroupValid( rSource ) ) { return output; }
    const size_t nBytes = T3D_HLOD_GROUP_HEADER_BYTES +
        rSource.m_prototypes.size() * T3D_HLOD_GROUP_PROTOTYPE_BYTES +
        rSource.m_instances.size() * T3D_HLOD_GROUP_INSTANCE_BYTES;
    output.resize( (int64_t) nBytes );
    if ( (size_t) output.size() != nBytes ) { return PackedByteArray(); }
    t3dByteWriter_t w( output.ptrw(), nBytes );
    w.writeU32( T3D_HLOD_GROUP_MAGIC );
    w.writeU16( T3D_HLOD_GROUP_VERSION );
    w.writeU16( 0u );
    w.writeU32( (uint32_t) rSource.m_prototypes.size() );
    w.writeU32( (uint32_t) rSource.m_instances.size() );
    w.writeU32( 0u ); // payload checksum set after all writes
    for ( size_t i = 0; i < rSource.m_prototypes.size(); ++i ) {
        const t3dHLODGroupPrototype_t &p = rSource.m_prototypes[i];
        w.writeU64( p.m_nGeometryKey ); w.writeU64( p.m_nMaterialKey );
        w.writeU32( p.m_nVertexBytes ); w.writeU32( p.m_nIndexBytes );
        w.writeF32( p.m_fGeometricError );
        for ( size_t k = 0u; k < 6u; ++k ) { w.writeF32( p.m_fBounds[k] ); }
    }
    for ( size_t i = 0; i < rSource.m_instances.size(); ++i ) {
        const t3dHLODGroupInstance_t &p = rSource.m_instances[i];
        w.writeU32( p.m_nPrototype ); w.writeU32( p.m_nSourceId );
        for ( size_t k = 0u; k < 12u; ++k ) { w.writeF32( p.m_fAffine[k] ); }
    }
    if ( !w.complete() ) { return PackedByteArray(); }
    uint8_t *pBytes = output.ptrw();
    t3dWriteLittleUnsigned< uint32_t >( pBytes + 16u,
        t3dHlodGroupChecksum( pBytes + T3D_HLOD_GROUP_HEADER_BYTES, nBytes - T3D_HLOD_GROUP_HEADER_BYTES ) );
    return output;
}

bool Terrain3DHLODInstanceCodec::decode( const PackedByteArray &rPayload, t3dHLODGroupData_t &rOutput )
{
    rOutput.m_prototypes.clear();
    rOutput.m_instances.clear();
    if ( rPayload.size() < (int64_t) T3D_HLOD_GROUP_HEADER_BYTES ) { return false; }
    const uint8_t *pBytes = rPayload.ptr();
    t3dByteReader_t r( pBytes, (size_t) rPayload.size() );
    const uint32_t nMagic = r.readU32();
    const uint16_t nVersion = r.readU16();
    const uint16_t nFlags = r.readU16();
    const uint32_t nPrototypes = r.readU32();
    const uint32_t nInstances = r.readU32();
    const uint32_t nChecksum = r.readU32();
    if ( nMagic != T3D_HLOD_GROUP_MAGIC || nVersion != T3D_HLOD_GROUP_VERSION || nFlags != 0u ||
        nPrototypes == 0u || nPrototypes > T3D_HLOD_GROUP_MAX_PROTOTYPES ||
        nInstances == 0u || nInstances > T3D_HLOD_GROUP_MAX_INSTANCES ) { return false; }
    const size_t nBytes = T3D_HLOD_GROUP_HEADER_BYTES +
        (size_t) nPrototypes * T3D_HLOD_GROUP_PROTOTYPE_BYTES +
        (size_t) nInstances * T3D_HLOD_GROUP_INSTANCE_BYTES;
    if ( (size_t) rPayload.size() != nBytes ||
        nChecksum != t3dHlodGroupChecksum( pBytes + T3D_HLOD_GROUP_HEADER_BYTES,
            nBytes - T3D_HLOD_GROUP_HEADER_BYTES ) ) { return false; }
    t3dHLODGroupData_t decoded;
    decoded.m_prototypes.resize( nPrototypes );
    decoded.m_instances.resize( nInstances );
    for ( uint32_t i = 0u; i < nPrototypes; ++i ) {
        t3dHLODGroupPrototype_t &p = decoded.m_prototypes[i];
        p.m_nGeometryKey = r.readU64(); p.m_nMaterialKey = r.readU64();
        p.m_nVertexBytes = r.readU32(); p.m_nIndexBytes = r.readU32();
        p.m_fGeometricError = r.readF32();
        for ( size_t k = 0u; k < 6u; ++k ) { p.m_fBounds[k] = r.readF32(); }
    }
    for ( uint32_t i = 0u; i < nInstances; ++i ) {
        t3dHLODGroupInstance_t &p = decoded.m_instances[i];
        p.m_nPrototype = r.readU32(); p.m_nSourceId = r.readU32();
        for ( size_t k = 0u; k < 12u; ++k ) { p.m_fAffine[k] = r.readF32(); }
    }
    if ( !r.complete() || !t3dHlodGroupValid( decoded ) ) { return false; }
    rOutput.m_prototypes.swap( decoded.m_prototypes );
    rOutput.m_instances.swap( decoded.m_instances );
    return true;
}
