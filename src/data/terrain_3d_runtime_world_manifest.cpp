// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "data/terrain_3d_runtime_world_manifest.h"

#include "core/logger.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_persistence.h"
#include "data/terrain_3d_region_repository.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "streaming/terrain_3d_runtime_page_store.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

#include <algorithm>
#include <cstring>

namespace {
static constexpr uint32_t MANIFEST_HEADER_BYTES = 112u;
static constexpr uint32_t MANIFEST_TOPOLOGY_BYTES = TERRAIN3D_MAX_REGION_SLOTS / 8u;
static constexpr uint32_t MANIFEST_REGION_BASE_BYTES = 112u;

static void hashBytes( uint64_t &nHash, const void *pData, const size_t nBytes ) {
	const uint8_t *const pRead = static_cast<const uint8_t *>( pData );
	for ( size_t i = 0; i < nBytes; ++i ) {
		nHash ^= pRead[i];
		nHash *= 1099511628211ull;
	}
}

static void hashU32( uint64_t &nHash, const uint32_t nValue ) {
	hashBytes( nHash, &nValue, sizeof( nValue ) );
}

static void storeString( const Ref<FileAccess> &rFile, const String &rValue ) {
	const CharString utf8 = rValue.utf8();
	const uint32_t length = ( uint32_t ) ( MAX( 0, utf8.length() ) );
	rFile->store_32( length );
	if ( length > 0u ) {
		rFile->store_buffer( reinterpret_cast<const uint8_t *>( utf8.get_data() ), length );
	}
}

static bool readString( const Ref<FileAccess> &rFile, String &rValue ) {
	if ( rFile.is_null() || rFile->get_position() + 4u > rFile->get_length() ) {
		return false;
	}
	const uint32_t length = rFile->get_32();
	if ( length > 1024u * 1024u || rFile->get_position() + length > rFile->get_length() ) {
		return false;
	}
	if ( length == 0u ) {
		rValue = String();
		return true;
	}
	const PackedByteArray bytes = rFile->get_buffer( length );
	if ( bytes.size() != length ) {
		return false;
	}
	rValue = String::utf8( reinterpret_cast<const char *>( bytes.ptr() ), ( int ) ( length ) );
	return true;
}

static String descriptorScenePath( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	if ( !rDescriptor.scenePath.is_empty() ) {
		return rDescriptor.scenePath;
	}
	return rDescriptor.pScene.is_valid() ? rDescriptor.pScene->get_path() : String();
}
}

void T3DRuntimeWorldManifest::reset( void ) {
	m_bLoaded = false;
	m_nRegionSize = 0;
	m_nRegionMapSize = 0;
	m_fVertexSpacing = 1.0;
	m_worldRegionMin = V2I_ZERO;
	m_worldRegionMax = V2I_ZERO;
	m_nWorldSignature = 0u;
	m_nMaterialSignature = 0u;
	m_worldHeightRange = V2_ZERO;
	m_regions.clear();
	m_sceneDependencies.clear();
	m_regionSceneDependencies.clear();
	m_regionLookup.fill( -1 );
	m_topologyBits.fill( 0u );
}

String T3DRuntimeWorldManifest::getPath( const String &rDataDirectory ) const {
	return rDataDirectory.is_empty() ? String() : rDataDirectory.trim_suffix( "/" ).path_join( "terrain3d_world.t3dmanifest" );
}

int T3DRuntimeWorldManifest::findSceneDependency( const String &rPath ) const {
	for ( int index = 0; index < ( int ) ( m_sceneDependencies.size() ); ++index ) {
		if ( m_sceneDependencies[( size_t ) ( index )] == rPath ) {
			return index;
		}
	}
	return -1;
}

const String &T3DRuntimeWorldManifest::getSceneDependency( const uint32_t nIndex ) const {
	static const String empty;
	return nIndex < m_sceneDependencies.size() ? m_sceneDependencies[nIndex] : empty;
}

void T3DRuntimeWorldManifest::rebuildLookup( void ) {
	m_regionLookup.fill( -1 );
	m_topologyBits.fill( 0u );
	for ( int index = 0; index < ( int ) ( m_regions.size() ); ++index ) {
		const int map_index = T3DRegionRepository::getRegionMapIndex( m_regions[( size_t ) ( index )].location );
		if ( map_index >= 0 && map_index < TERRAIN3D_MAX_REGION_SLOTS ) {
			m_regionLookup[( size_t ) ( map_index )] = ( int16_t ) ( index );
			m_topologyBits[( size_t ) ( map_index >> 6 )] |= 1ull << ( map_index & 63 );
		}
	}
}

bool T3DRuntimeWorldManifest::hasRegion( const Vector2i &rLocation ) const {
	const int map_index = T3DRegionRepository::getRegionMapIndex( rLocation );
	return map_index >= 0 && map_index < TERRAIN3D_MAX_REGION_SLOTS &&
		( m_topologyBits[( size_t ) ( map_index >> 6 )] & ( 1ull << ( map_index & 63 ) ) ) != 0u;
}

const T3DRuntimeWorldRegionRecord *T3DRuntimeWorldManifest::findRegion( const Vector2i &rLocation ) const {
	const int map_index = T3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 || map_index >= TERRAIN3D_MAX_REGION_SLOTS ) {
		return nullptr;
	}
	const int index = m_regionLookup[( size_t ) ( map_index )];
	return index >= 0 && index < ( int ) ( m_regions.size() ) ? &m_regions[( size_t ) ( index )] : nullptr;
}

bool T3DRuntimeWorldManifest::buildAuthoringIndex( const T3DRegionRepository &rRepository ) {
	reset();
	m_nRegionSize = rRepository.getRegionSize();
	m_nRegionMapSize = rRepository.getRegionMapSize();
	m_fVertexSpacing = rRepository.getVertexSpacing();
	if ( m_nRegionSize <= 0 || m_nRegionMapSize <= 0 ) {
		return false;
	}
	bool first_height = true;
	bool first_region = true;
	for ( int slot = 0; slot < rRepository.getSlotCount(); ++slot ) {
		const Vector2i location = rRepository.getSlotsData()[slot];
		const T3DRegion *const pRegion = rRepository.getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		T3DRuntimeWorldRegionRecord record;
		record.location = location;
		if ( first_region ) {
			m_worldRegionMin = location;
			m_worldRegionMax = location;
			first_region = false;
		} else {
			m_worldRegionMin.x = MIN( m_worldRegionMin.x, location.x );
			m_worldRegionMin.y = MIN( m_worldRegionMin.y, location.y );
			m_worldRegionMax.x = MAX( m_worldRegionMax.x, location.x );
			m_worldRegionMax.y = MAX( m_worldRegionMax.y, location.y );
		}
		record.heightRange = pRegion->getHeightRange();
		record.channelFlags = ( uint32_t ) ( pRegion->getChannelFlags() );
		record.instanceCount = pRegion->getInstanceCount();
		record.instanceMeshCount = pRegion->getInstanceMeshCount();
		for ( int mesh = 0; mesh < TERRAIN3D_MAX_MESH_ASSETS; ++mesh ) {
			record.instanceMeshCounts[( size_t ) ( mesh )] = pRegion->getInstanceCount( mesh );
		}
		const eastl::vector<T3DRegion::t3dstreamedNodeRuntimeDescriptor_t> &nodes = pRegion->getStreamedNodesRuntimeInternal();
		record.streamedNodeCount = ( uint16_t ) ( MIN( nodes.size(), ( size_t ) ( UINT16_MAX ) ) );
		record.nodeCoverageMin = V2I_ZERO;
		record.nodeCoverageMax = V2I_ZERO;
		bool first_coverage = true;
		record.sceneDependencyFirst = ( uint32_t ) ( m_regionSceneDependencies.size() );
		for ( const T3DRegion::t3dstreamedNodeRuntimeDescriptor_t &descriptor : nodes ) {
			if ( descriptor.boundaryPolicy == T3DRegion::STREAMED_NODE_BOUNDARY_COVERAGE ) {
				if ( first_coverage ) {
					record.nodeCoverageMin = descriptor.coverageMin;
					record.nodeCoverageMax = descriptor.coverageMax;
					first_coverage = false;
				} else {
					record.nodeCoverageMin.x = MIN( record.nodeCoverageMin.x, descriptor.coverageMin.x );
					record.nodeCoverageMin.y = MIN( record.nodeCoverageMin.y, descriptor.coverageMin.y );
					record.nodeCoverageMax.x = MAX( record.nodeCoverageMax.x, descriptor.coverageMax.x );
					record.nodeCoverageMax.y = MAX( record.nodeCoverageMax.y, descriptor.coverageMax.y );
				}
			}
			const String scene_path = descriptorScenePath( descriptor );
			if ( scene_path.is_empty() ) {
				continue;
			}
			int dependency = findSceneDependency( scene_path );
			if ( dependency < 0 ) {
				dependency = ( int ) ( m_sceneDependencies.size() );
				m_sceneDependencies.push_back( scene_path );
			}
			bool duplicate = false;
			for ( uint32_t d = record.sceneDependencyFirst; d < m_regionSceneDependencies.size(); ++d ) {
				if ( m_regionSceneDependencies[d] == ( uint16_t ) ( dependency ) ) {
					duplicate = true;
					break;
				}
			}
			if ( !duplicate && dependency <= UINT16_MAX ) {
				m_regionSceneDependencies.push_back( ( uint16_t ) ( dependency ) );
			}
		}
		record.sceneDependencyCount = ( uint16_t ) ( m_regionSceneDependencies.size() - record.sceneDependencyFirst );
		const Ref<T3DRegionLighting> pLighting = pRegion->getLighting();
		if ( pLighting.is_valid() && !pLighting->getLightDataPath().is_empty() ) {
			int dependency = findSceneDependency( pLighting->getLightDataPath() );
			if ( dependency < 0 ) { dependency = ( int ) m_sceneDependencies.size(); m_sceneDependencies.push_back( pLighting->getLightDataPath() ); }
			record.lightmapDependency = ( uint32_t ) dependency;
			const String directional_signature = pLighting->getDirectionalLightSignature();
			if ( !directional_signature.is_empty() ) {
				int directional = findSceneDependency( directional_signature );
				if ( directional < 0 ) { directional = ( int ) m_sceneDependencies.size(); m_sceneDependencies.push_back( directional_signature ); }
				record.lightmapDirectionalSignatureDependency = ( uint32_t ) directional;
			}
			record.lightmapReceiverCount = ( uint32_t ) MAX( pLighting->getReceiverCount(), 0 );
			record.lightmapContributorCount = ( uint32_t ) MAX( pLighting->getContributorCount(), 0 );
			record.lightmapEstimatedGpuBytes = ( uint64_t ) MAX<int64_t>( pLighting->getEstimatedGpuBytes(), 0 );
			record.lightmapBakeSignature = pLighting->getBakeSignature();
			record.lightmapShadowmaskMode = ( uint16_t ) MAX( pLighting->getShadowmaskMode(), 0 );
			record.lightmapFlags = pLighting->hasProbeData() ? 1u : 0u;
			record.lightmapTextureCount = ( uint16_t ) MIN( MAX( pLighting->getLightmapTextureCount(), 0 ), ( int ) UINT16_MAX );
			record.lightmapShadowmaskTextureCount = ( uint16_t ) MIN( MAX( pLighting->getShadowmaskTextureCount(), 0 ), ( int ) UINT16_MAX );
			record.lightmapClusterSizeRegions = ( uint16_t ) pLighting->getClusterSizeRegions();
			record.lightmapContextRadiusRegions = ( uint16_t ) pLighting->getContextRadiusRegions();
			record.lightmapTerrainProxyLod = ( uint16_t ) pLighting->getTerrainProxyLod();
		}
		if ( first_height ) {
			m_worldHeightRange = record.heightRange;
			first_height = false;
		} else {
			m_worldHeightRange.x = MIN( m_worldHeightRange.x, record.heightRange.x );
			m_worldHeightRange.y = MAX( m_worldHeightRange.y, record.heightRange.y );
		}
		m_regions.push_back( record );
	}
	rebuildLookup();
	static const Vector2i neighbors[8] = {
		Vector2i( 0, -1 ), Vector2i( 1, -1 ), Vector2i( 1, 0 ), Vector2i( 1, 1 ),
		Vector2i( 0, 1 ), Vector2i( -1, 1 ), Vector2i( -1, 0 ), Vector2i( -1, -1 )
	};
	for ( T3DRuntimeWorldRegionRecord &record : m_regions ) {
		uint16_t adjacency = 0u;
		for ( uint16_t bit = 0u; bit < 8u; ++bit ) {
			if ( hasRegion( record.location + neighbors[bit] ) ) {
				adjacency |= ( uint16_t ) ( 1u << bit );
			}
		}
		record.flags = ( uint16_t ) ( ( record.flags & 0xff00u ) | adjacency );
	}
	return !m_regions.empty();
}

bool T3DRuntimeWorldManifest::write(
	const String &rDataDirectory, const T3DRegionRepository &rRepository,
	const T3DRuntimePageStore &rPageStore, const T3DRuntimePageConfig &rConfig,
	PackedStringArray *const pErrors
) {
	if ( !buildAuthoringIndex( rRepository ) ) {
		if ( pErrors ) pErrors->push_back( "World manifest has no regions to cook." );
		return false;
	}
	m_nMaterialSignature = rConfig.materialSourceSignature;
	uint64_t world_signature = 1469598103934665603ull;
	hashU32( world_signature, ( uint32_t ) ( m_nRegionSize ) );
	hashU32( world_signature, ( uint32_t ) ( m_regions.size() ) );
	for ( T3DRuntimeWorldRegionRecord &record : m_regions ) {
		const T3DRegion *const pRegion = rRepository.getRegionPtr( record.location );
		uint64_t page_signature = 0u;
		const String page_path = rPageStore.getPagePath( pRegion, rDataDirectory );
		if ( !rPageStore.readSourceSignature( page_path, page_signature ) || page_signature == 0u ) {
			if ( pErrors ) pErrors->push_back( "Missing/invalid runtime page for manifest region " + String( record.location ) );
			return false;
		}
		record.pageSourceSignature = page_signature;
		record.materialMask = rPageStore.readMaterialMask( page_path );
		if ( record.materialMask == 0u ) record.materialMask = 1u;
		hashBytes( world_signature, &record.location.x, sizeof( record.location.x ) );
		hashBytes( world_signature, &record.location.y, sizeof( record.location.y ) );
		hashBytes( world_signature, &record.pageSourceSignature, sizeof( record.pageSourceSignature ) );
		hashBytes( world_signature, &record.materialMask, sizeof( record.materialMask ) );
		hashBytes( world_signature, &record.flags, sizeof( record.flags ) );
	}
	m_nWorldSignature = world_signature;

	const String path = getPath( rDataDirectory );
	if ( path.is_empty() ) return false;
	const String temporary = path + ".tmp";
	const String backup = path + ".bak";
	if ( FileAccess::file_exists( temporary ) ) DirAccess::remove_absolute( temporary );
	Ref<FileAccess> pFile = FileAccess::open( temporary, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		if ( pErrors ) pErrors->push_back( "Cannot open world manifest temporary file." );
		return false;
	}
	pFile->store_64( TERRAIN3D_RUNTIME_WORLD_MANIFEST_MAGIC );
	pFile->store_32( TERRAIN3D_RUNTIME_WORLD_MANIFEST_VERSION );
	pFile->store_32( MANIFEST_HEADER_BYTES );
	pFile->store_32( ( uint32_t ) ( m_nRegionSize ) );
	pFile->store_32( ( uint32_t ) ( m_nRegionMapSize ) );
	pFile->store_32( ( uint32_t ) ( m_regions.size() ) );
	pFile->store_32( ( uint32_t ) ( m_sceneDependencies.size() ) );
	pFile->store_32( ( uint32_t ) ( m_regionSceneDependencies.size() ) );
	pFile->store_32( MANIFEST_REGION_BASE_BYTES + ( uint32_t ) ( TERRAIN3D_MAX_MESH_ASSETS * sizeof( uint32_t ) ) );
	pFile->store_64( m_nWorldSignature );
	pFile->store_64( m_nMaterialSignature );
	pFile->store_float( ( float ) ( m_fVertexSpacing ) );
	pFile->store_float( ( float ) ( m_worldHeightRange.x ) );
	pFile->store_float( ( float ) ( m_worldHeightRange.y ) );
	pFile->store_32( ( uint32_t ) ( m_worldRegionMin.x ) );
	pFile->store_32( ( uint32_t ) ( m_worldRegionMin.y ) );
	pFile->store_32( ( uint32_t ) ( m_worldRegionMax.x ) );
	pFile->store_32( ( uint32_t ) ( m_worldRegionMax.y ) );
	pFile->store_32( MANIFEST_TOPOLOGY_BYTES );
	pFile->store_32( 0u );
	while ( pFile->get_position() < MANIFEST_HEADER_BYTES ) pFile->store_8( 0u );
	for ( uint64_t bits : m_topologyBits ) pFile->store_64( bits );
	for ( const T3DRuntimeWorldRegionRecord &record : m_regions ) {
		pFile->store_32( ( uint32_t ) ( record.location.x ) );
		pFile->store_32( ( uint32_t ) ( record.location.y ) );
		pFile->store_float( ( float ) ( record.heightRange.x ) );
		pFile->store_float( ( float ) ( record.heightRange.y ) );
		pFile->store_32( record.channelFlags );
		pFile->store_32( record.materialMask );
		pFile->store_64( record.pageSourceSignature );
		pFile->store_32( record.instanceCount );
		pFile->store_16( record.instanceMeshCount );
		pFile->store_16( record.streamedNodeCount );
		pFile->store_32( ( uint32_t ) ( record.nodeCoverageMin.x ) );
		pFile->store_32( ( uint32_t ) ( record.nodeCoverageMin.y ) );
		pFile->store_32( ( uint32_t ) ( record.nodeCoverageMax.x ) );
		pFile->store_32( ( uint32_t ) ( record.nodeCoverageMax.y ) );
		pFile->store_32( record.sceneDependencyFirst );
		pFile->store_16( record.sceneDependencyCount );
		pFile->store_16( record.flags );
		pFile->store_32( record.lightmapDependency );
		pFile->store_32( record.lightmapDirectionalSignatureDependency );
		pFile->store_32( record.lightmapReceiverCount );
		pFile->store_32( record.lightmapContributorCount );
		pFile->store_64( record.lightmapEstimatedGpuBytes );
		pFile->store_64( ( uint64_t ) record.lightmapBakeSignature );
		pFile->store_16( record.lightmapShadowmaskMode );
		pFile->store_16( record.lightmapFlags );
		pFile->store_16( record.lightmapTextureCount );
		pFile->store_16( record.lightmapShadowmaskTextureCount );
		pFile->store_16( record.lightmapClusterSizeRegions );
		pFile->store_16( record.lightmapContextRadiusRegions );
		pFile->store_16( record.lightmapTerrainProxyLod );
		pFile->store_16( 0u );
		for ( uint32_t count : record.instanceMeshCounts ) pFile->store_32( count );
	}
	for ( uint16_t dependency : m_regionSceneDependencies ) pFile->store_16( dependency );
	for ( const String &dependency : m_sceneDependencies ) storeString( pFile, dependency );
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary );
		if ( pErrors ) pErrors->push_back( "World manifest write failed." );
		return false;
	}
	if ( FileAccess::file_exists( backup ) ) DirAccess::remove_absolute( backup );
	const bool had_previous = FileAccess::file_exists( path );
	if ( had_previous && DirAccess::rename_absolute( path, backup ) != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary, path ) != OK ) {
		if ( had_previous ) DirAccess::rename_absolute( backup, path );
		return false;
	}
	if ( FileAccess::file_exists( backup ) ) DirAccess::remove_absolute( backup );
	m_bLoaded = true;
	return true;
}

bool T3DRuntimeWorldManifest::load( const String &rDataDirectory, PackedStringArray *const pErrors ) {
	reset();
	const String path = getPath( rDataDirectory );
	Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() ) {
		if ( pErrors ) pErrors->push_back( "Cooked world manifest is missing: " + path );
		return false;
	}
	if ( pFile->get_length() < MANIFEST_HEADER_BYTES || pFile->get_64() != TERRAIN3D_RUNTIME_WORLD_MANIFEST_MAGIC ||
		pFile->get_32() != TERRAIN3D_RUNTIME_WORLD_MANIFEST_VERSION || pFile->get_32() != MANIFEST_HEADER_BYTES ) {
		if ( pErrors ) pErrors->push_back( "Cooked world manifest header is invalid." );
		return false;
	}
	m_nRegionSize = ( int ) ( pFile->get_32() );
	m_nRegionMapSize = ( int ) ( pFile->get_32() );
	const uint32_t region_count = pFile->get_32();
	const uint32_t dependency_count = pFile->get_32();
	const uint32_t region_dependency_count = pFile->get_32();
	const uint32_t record_bytes = pFile->get_32();
	m_nWorldSignature = pFile->get_64();
	m_nMaterialSignature = pFile->get_64();
	m_fVertexSpacing = pFile->get_float();
	m_worldHeightRange.x = pFile->get_float();
	m_worldHeightRange.y = pFile->get_float();
	m_worldRegionMin.x = ( int32_t ) ( pFile->get_32() );
	m_worldRegionMin.y = ( int32_t ) ( pFile->get_32() );
	m_worldRegionMax.x = ( int32_t ) ( pFile->get_32() );
	m_worldRegionMax.y = ( int32_t ) ( pFile->get_32() );
	const uint32_t topology_bytes = pFile->get_32();
	pFile->get_32();
	if ( !isValidRegionSize( m_nRegionSize ) || m_nRegionMapSize != TERRAIN3D_ACTIVE_WORLD_TEXELS / m_nRegionSize ||
		region_count == 0u || region_count > TERRAIN3D_MAX_REGION_SLOTS || dependency_count > UINT16_MAX ||
		record_bytes != MANIFEST_REGION_BASE_BYTES + TERRAIN3D_MAX_MESH_ASSETS * sizeof( uint32_t ) ||
		topology_bytes != MANIFEST_TOPOLOGY_BYTES || m_fVertexSpacing < 0.25f || m_fVertexSpacing > 100.0f ) {
		if ( pErrors ) pErrors->push_back( "Cooked world manifest dimensions are invalid." );
		return false;
	}
	pFile->seek( MANIFEST_HEADER_BYTES );
	for ( uint64_t &bits : m_topologyBits ) bits = pFile->get_64();
	m_regions.resize( region_count );
	for ( uint32_t index = 0u; index < region_count; ++index ) {
		T3DRuntimeWorldRegionRecord &record = m_regions[index];
		record.location.x = ( int32_t ) ( pFile->get_32() );
		record.location.y = ( int32_t ) ( pFile->get_32() );
		record.heightRange.x = pFile->get_float();
		record.heightRange.y = pFile->get_float();
		record.channelFlags = pFile->get_32();
		record.materialMask = pFile->get_32();
		record.pageSourceSignature = pFile->get_64();
		record.instanceCount = pFile->get_32();
		record.instanceMeshCount = pFile->get_16();
		record.streamedNodeCount = pFile->get_16();
		record.nodeCoverageMin.x = ( int32_t ) ( pFile->get_32() );
		record.nodeCoverageMin.y = ( int32_t ) ( pFile->get_32() );
		record.nodeCoverageMax.x = ( int32_t ) ( pFile->get_32() );
		record.nodeCoverageMax.y = ( int32_t ) ( pFile->get_32() );
		record.sceneDependencyFirst = pFile->get_32();
		record.sceneDependencyCount = pFile->get_16();
		record.flags = pFile->get_16();
		record.lightmapDependency = pFile->get_32();
		record.lightmapDirectionalSignatureDependency = pFile->get_32();
		record.lightmapReceiverCount = pFile->get_32();
		record.lightmapContributorCount = pFile->get_32();
		record.lightmapEstimatedGpuBytes = pFile->get_64();
		record.lightmapBakeSignature = ( int64_t ) pFile->get_64();
		record.lightmapShadowmaskMode = pFile->get_16();
		record.lightmapFlags = pFile->get_16();
		record.lightmapTextureCount = pFile->get_16();
		record.lightmapShadowmaskTextureCount = pFile->get_16();
		record.lightmapClusterSizeRegions = pFile->get_16();
		record.lightmapContextRadiusRegions = pFile->get_16();
		record.lightmapTerrainProxyLod = pFile->get_16();
		pFile->get_16();
		if ( !terrain3dRegionLocationValid( record.location, m_nRegionMapSize ) || record.materialMask == 0u ||
			record.pageSourceSignature == 0u || record.sceneDependencyFirst + record.sceneDependencyCount > region_dependency_count ) {
			if ( pErrors ) pErrors->push_back( "Cooked world manifest contains an invalid region record." );
			reset();
			return false;
		}
		for ( uint32_t &count : record.instanceMeshCounts ) count = pFile->get_32();
	}
	m_regionSceneDependencies.resize( region_dependency_count );
	for ( uint32_t index = 0u; index < region_dependency_count; ++index ) m_regionSceneDependencies[index] = pFile->get_16();
	m_sceneDependencies.resize( dependency_count );
	for ( uint32_t index = 0u; index < dependency_count; ++index ) {
		if ( !readString( pFile, m_sceneDependencies[index] ) ) {
			if ( pErrors ) pErrors->push_back( "Cooked world manifest dependency table is truncated." );
			reset();
			return false;
		}
	}
	for ( const uint16_t dependency : m_regionSceneDependencies ) {
		if ( dependency >= dependency_count ) {
			if ( pErrors ) pErrors->push_back( "Cooked world manifest contains an invalid region dependency index." );
			reset();
			return false;
		}
	}
	for ( const T3DRuntimeWorldRegionRecord &record : m_regions ) {
		if ( ( record.lightmapDependency != UINT32_MAX && record.lightmapDependency >= dependency_count ) ||
			( record.lightmapDirectionalSignatureDependency != UINT32_MAX &&
				record.lightmapDirectionalSignatureDependency >= dependency_count ) ) {
			if ( pErrors ) pErrors->push_back( "Cooked world manifest contains an invalid lightmap dependency index." );
			reset();
			return false;
		}
		const String source_path = rDataDirectory.trim_suffix( "/" ) + String( "/" ) +
			Util::locationToFilename( record.location );
		const String page_path = source_path.get_basename() + String( ".t3dpage" );
		if ( !FileAccess::file_exists( page_path ) ) {
			if ( pErrors ) pErrors->push_back( "Cooked runtime page is missing: " + page_path );
			reset();
			return false;
		}
	}
	if ( pFile->get_error() != OK && pFile->get_error() != ERR_FILE_EOF ) {
		reset();
		return false;
	}
	rebuildLookup();
	m_bLoaded = true;
	return true;
}

bool T3DRuntimeWorldManifest::stageRepository(
	const String &rDataDirectory, T3DStagedRepository &rStaged
) const {
	if ( !m_bLoaded || m_nRegionSize <= 0 || m_regions.empty() ) return false;
	rStaged.reset();
	rStaged.bBoundedCpuLoad = true;
	rStaged.nTargetRegionSize = m_nRegionSize;
	rStaged.regions.reserve( m_regions.size() );
	for ( int slot = 0; slot < ( int ) ( m_regions.size() ); ++slot ) {
		const T3DRuntimeWorldRegionRecord &record = m_regions[( size_t ) ( slot )];
		Ref<T3DRegion> pRegion;
		pRegion.instantiate();
		pRegion->setLocation( record.location );
		pRegion->setRegionSize( m_nRegionSize );
		pRegion->setVertexSpacing( m_fVertexSpacing );
		pRegion->setHeightRange( record.heightRange );
		pRegion->setChannelFlags( ( int ) ( record.channelFlags ) );
		pRegion->setCookedPageSourceSignatureInternal( record.pageSourceSignature );
		pRegion->setCookedInstanceMetadataInternal( record.instanceMeshCounts.data(), record.instanceCount, record.instanceMeshCount );
		pRegion->setCookedStreamedNodeMetadataInternal(
			record.streamedNodeCount, record.nodeCoverageMin, record.nodeCoverageMax );
		if ( record.lightmapDependency != UINT32_MAX && record.lightmapDependency < m_sceneDependencies.size() ) {
			Ref<T3DRegionLighting> lighting; lighting.instantiate();
			lighting->setLightDataPath( m_sceneDependencies[record.lightmapDependency] );
			lighting->setClusterLocation( record.location ); lighting->setRegionMin( record.location ); lighting->setRegionMax( record.location );
			lighting->setClusterSizeRegions( record.lightmapClusterSizeRegions ); lighting->setTerrainProxyLod( record.lightmapTerrainProxyLod );
			lighting->setContextRadiusRegions( record.lightmapContextRadiusRegions ); lighting->setShadowmaskMode( record.lightmapShadowmaskMode );
			lighting->setBakeSignature( record.lightmapBakeSignature );
			if ( record.lightmapDirectionalSignatureDependency != UINT32_MAX && record.lightmapDirectionalSignatureDependency < m_sceneDependencies.size() )
				lighting->setDirectionalLightSignature( m_sceneDependencies[record.lightmapDirectionalSignatureDependency] );
			lighting->setReceiverCount( ( int ) record.lightmapReceiverCount ); lighting->setContributorCount( ( int ) record.lightmapContributorCount );
			lighting->setLightmapTextureCount( record.lightmapTextureCount ); lighting->setShadowmaskTextureCount( record.lightmapShadowmaskTextureCount );
			lighting->setHasProbeData( ( record.lightmapFlags & 1u ) != 0u ); lighting->setEstimatedGpuBytes( ( int64_t ) record.lightmapEstimatedGpuBytes );
			pRegion->setLighting( lighting );
		}
		pRegion->setModified( false );
		pRegion->markRuntimeMapsCleanInternal();
		T3DStagedRegion entry;
		entry.location = record.location;
		entry.path = rDataDirectory.path_join( Util::locationToFilename( record.location ) );
		entry.pRegion = pRegion;
		const int half_map = m_nRegionMapSize / 2;
		entry.nActiveSlot = ( record.location.y + half_map ) * m_nRegionMapSize + record.location.x + half_map;
		rStaged.regions.push_back( entry );
		const int map_index = T3DRegionRepository::getRegionMapIndex( record.location );
		rStaged.resources[record.location] = pRegion;
		rStaged.regionPtrs[( size_t ) ( map_index )] = pRegion.ptr();
		rStaged.regionLocations[( size_t ) ( slot )] = record.location;
		rStaged.logicalRegionMap[( size_t ) ( map_index )] = ( uint16_t ) ( slot + 1 );
	}
	return true;
}
