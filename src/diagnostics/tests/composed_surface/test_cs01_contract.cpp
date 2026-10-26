// CS00–CS02 standalone contract/budget tests. Build without Godot dependencies:
// clang++ -std=c++17 -DT3D_RUNTIME_NATIVE -Isrc -Wall -Wextra -Werror -pthread ...
#include "data/runtime/terrain_3d_composed_surface_sidecar.h"
#include "memory/policy/terrain_3d_composed_surface_budget.h"

#include <assert.h>
#include <stdio.h>
#include <vector>

static std::vector< uint8_t > makeSidecar( const uint32_t count )
{
    t3dComposedSurfaceFileHeader_t h{};
    h.m_nMagic = T3D_CS_FILE_MAGIC;
    h.m_nVersion = T3D_CS_FILE_VERSION;
    h.m_nHeaderBytes = sizeof( h );
    h.m_nRecordBytes = sizeof( t3dComposedSurfacePageRecord_t );
    h.m_nCompositionAbi = T3D_CS_COMPOSITION_ABI;
    h.m_nChannelMask = T3D_CS_CHANNEL_MASK_ALL;
    h.m_nPageInterior = T3D_CS_PAGE_INTERIOR;
    h.m_nPageGutter = T3D_CS_PAGE_GUTTER;
    h.m_nPhysicalTile = T3D_CS_PHYSICAL_TILE;
    h.m_nPageCount = count;
    h.m_nRecordOffset = sizeof( h );
    h.m_nDataOffset = h.m_nRecordOffset + uint64_t( count ) * sizeof( t3dComposedSurfacePageRecord_t );
    h.m_nFileBytes = h.m_nDataOffset + uint64_t( count ) * T3D_CS_PAGE_PAYLOAD_BYTES;
    h.m_nSourceFingerprint = 0x98765432u;
    std::vector< uint8_t > file( size_t( h.m_nFileBytes ), 0u );
    memcpy( file.data(), &h, sizeof( h ) );
    uint64_t offset = h.m_nDataOffset;
    for ( uint32_t page = 0u; page < count; ++page ) {
        t3dComposedSurfacePageRecord_t r{};
        r.m_nRegionKey = 0x5445525245470001ull;
        r.m_nSourceFingerprint = h.m_nSourceFingerprint;
        r.m_nContentFingerprint = 0xabcdu + page;
        r.m_nCompositionRevision = 1u;
        r.m_nGeneration = 1u;
        r.m_nPageX = page;
        r.m_nChannelMask = T3D_CS_CHANNEL_MASK_ALL;
        for ( uint32_t channel = 0u; channel < T3D_CS_CHANNEL_COUNT; ++channel ) {
            t3dComposedSurfaceChannelRecord_t &b = r.m_channels[channel];
            b.m_nFileOffset = offset;
            b.m_nByteCount = t3d_cs_channel_bytes( channel );
            for ( uint32_t i = 0u; i < b.m_nByteCount; ++i ) {
                file[size_t( offset + i )] = uint8_t( ( i * 7u + page * 3u + channel ) & 255u );
            }
            b.m_nCrc32 = t3d_cs_crc32( file.data() + offset, b.m_nByteCount );
            offset += b.m_nByteCount;
        }
        memcpy( file.data() + h.m_nRecordOffset + uint64_t( page ) * sizeof( r ), &r, sizeof( r ) );
    }
    assert( offset == h.m_nFileBytes );
    return file;
}

int main()
{
    assert( t3d_cs_host_little_endian() );
    assert( t3d_cs_crc32( reinterpret_cast< const uint8_t * >( "123456789" ), 9u ) == 0xcbf43926u );
    assert( t3d_cs_channel_bytes( T3D_CS_CHANNEL_ALBEDO ) == 9248u );
    assert( t3d_cs_channel_bytes( T3D_CS_CHANNEL_NORMAL ) == 18496u );
    assert( t3d_cs_channel_bytes( T3D_CS_CHANNEL_SURFACE ) == 18496u );
    auto file = makeSidecar( 2u );
    assert( t3d_cs_validate_sidecar_bytes( file.data(), file.size(), true ) );
    assert( !t3d_cs_validate_sidecar_bytes( file.data(), file.size() - 1u, false ) );
    t3dComposedSurfaceFileHeader_t header{};
    memcpy( &header, file.data(), sizeof( header ) );
    header.m_nRecordBytes = 0u;
    memcpy( file.data(), &header, sizeof( header ) );
    assert( !t3d_cs_validate_sidecar_bytes( file.data(), file.size(), false ) );
    file = makeSidecar( 2u );
    t3dComposedSurfacePageRecord_t record{};
    memcpy( &record, file.data() + sizeof( header ), sizeof( record ) );
    record.m_channels[0].m_nFileOffset = UINT64_MAX;
    memcpy( file.data() + sizeof( header ), &record, sizeof( record ) );
    assert( !t3d_cs_validate_sidecar_bytes( file.data(), file.size(), false ) );
    file = makeSidecar( 2u );
    file.back() ^= 1u;
    assert( t3d_cs_validate_sidecar_bytes( file.data(), file.size(), false ) );
    assert( !t3d_cs_validate_sidecar_bytes( file.data(), file.size(), true ) );
    file = makeSidecar( 1u );
    assert( t3d_cs_validate_sidecar_bytes( file.data(), file.size(), true ) );
    // Byte reader is aligned-independent; cast of unaligned mapped records is forbidden.
    std::vector< uint8_t > unaligned( file.size() + 1u );
    memcpy( unaligned.data() + 1u, file.data(), file.size() );
    assert( t3d_cs_validate_sidecar_bytes( unaligned.data() + 1u, file.size(), true ) );

    t3dComposedSurfacePageMapping_t mapping{};
    mapping.m_nRegionKey = 21u;
    mapping.m_nPhysicalSlot = 5u;
    mapping.m_nState = T3D_CS_PAGE_RESIDENT;
    mapping.m_nResidentChannels = T3D_CS_CHANNEL_MASK_ALL;
    mapping.m_nGeneration = 1u;
    mapping.m_nDeviceGeneration = 3u;
    mapping.m_nCompositionRevision = 8u;
    assert( t3d_cs_validate_mapping( mapping, 3u, 8u ) );
    assert( !t3d_cs_validate_mapping( mapping, 4u, 8u ) );
    assert( !t3d_cs_validate_mapping( mapping, 3u, 9u ) );
    mapping.m_nResidentChannels = 3u;
    assert( !t3d_cs_validate_mapping( mapping, 3u, 8u ) );
    mapping.m_nResidentChannels = T3D_CS_CHANNEL_MASK_ALL;
    mapping.m_nPhysicalSlot = T3D_CS_INVALID_SLOT;
    assert( !t3d_cs_validate_mapping( mapping, 3u, 8u ) );

    const uint32_t limits[] = { 256u, 512u, 1024u, 2048u };
    const uint32_t expected[] = { 128u, 256u, 512u, 1024u };
    for ( uint32_t i = 0u; i < 4u; ++i ) {
        const auto budget = t3d_cs_plan_budget( true, limits[i], false, 1024ull * 1024ull * 1024ull );
        assert( budget.m_nPages == expected[i] );
        assert( budget.m_nGpuBytes == uint64_t( expected[i] ) *
                                            ( T3D_CS_PAGE_PAYLOAD_BYTES + T3D_CS_PAGE_METADATA_BYTES ) );
        assert( budget.m_nTotalBytes == budget.m_nGpuBytes + budget.m_nCpuBytes );
        const auto disabled = t3d_cs_plan_budget( false, limits[i], false, UINT64_MAX );
        assert( disabled.m_nPages == 0u && disabled.m_nTotalBytes == 0u );
    }
    assert( t3d_cs_plan_budget( true, 256u, false, 0u ).m_nPages == 0u );
    assert( t3d_cs_plan_budget( true, 512u, false, 4ull * 1024u * 1024u ).m_nPages == 64u );
    assert( t3d_cs_plan_budget( true, 2048u, true, UINT64_MAX ).m_nPages == 2048u );
    auto grant = t3d_cs_plan_budget( true, 256u, false, UINT64_MAX );
    Terrain3DMemoryAccount account( grant.m_nTotalBytes + 1024u );
    assert( t3d_cs_reserve_budget( account, grant ) );
    const auto after = account.stats();
    assert( after.committed == grant.m_nTotalBytes );
    assert( after.domains[Terrain3DMemoryAccount::COMPOSED_SURFACE].committed == grant.m_nTotalBytes );
    assert( after.bytes[Terrain3DMemoryAccount::GPU] == grant.m_nGpuBytes );
    assert( !t3d_cs_reserve_budget( account, grant ) );
    assert( account.stats().committed == grant.m_nTotalBytes );
    t3d_cs_release_budget( account, grant );
    assert( account.stats().committed == 0u );
    assert( account.setDomainLimit( Terrain3DMemoryAccount::COMPOSED_SURFACE, grant.m_nGpuBytes ) );
    assert( !t3d_cs_reserve_budget( account, grant ) );
    assert( account.stats().committed == 0u );
    puts( "CS01: sidecar schema, CRC, bounded budget, mapping generation, rollback: PASS" );
    return 0;
}
