// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "data/terrain_3d_runtime_world_manifest.h"

#include "core/logger.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region.h"
#include "data/terrain_3d_region_persistence.h"
#include "data/terrain_3d_region_repository.h"
#include "lighting/terrain_3d_region_lighting.h"
#include "streaming/terrain_3d_runtime_page_store.h"

#include <string.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>


static void t3dRuntimeWorldManifestHashBytes( uint64_t &nHash, const void *pData, const size_t nBytes ) {
	const uint8_t *const pRead = ( const uint8_t * )( pData );
	for ( size_t i = 0; i < nBytes; ++i ) {
		nHash ^= pRead[i];
		nHash *= 1099511628211ull;
	}
}

EA_FORCE_INLINE static void t3dRuntimeWorldManifestHashU32( uint64_t &nHash, const uint32_t nValue ) {
	t3dRuntimeWorldManifestHashBytes( nHash, &nValue, sizeof( nValue ) );
}

static void t3dRuntimeWorldManifestStoreString( const Ref<FileAccess> &rFile, const String &rValue ) {
	const CharString utf8 = rValue.utf8();
	const uint32_t length = ( uint32_t )( MAX( 0, utf8.length() ) );
	rFile->store_32( length );
	if ( length > 0u ) {
		rFile->store_buffer( ( const uint8_t * )( utf8.get_data() ), length );
	}
}

static bool t3dRuntimeWorldManifestReadString( const Ref<FileAccess> &rFile, String &rValue ) {
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
	rValue = String::utf8( ( const char * )( bytes.ptr() ), ( int )( length ) );
	return true;
}

static String t3dRuntimeWorldManifestDescriptorScenePath( const t3dStreamedNodeRuntimeDescriptor_t &rDescriptor ) {
	if ( !rDescriptor.m_scenePath.is_empty() ) {
		return rDescriptor.m_scenePath;
	}
	return rDescriptor.m_scene.is_valid() ? rDescriptor.m_scene->get_path() : String();
}

Terrain3DRuntimeWorldManifest::Terrain3DRuntimeWorldManifest( void ) :
        m_regions( Terrain3DEastlAllocator::bootstrapAllocator() ),
        m_sceneDependencies( Terrain3DEastlAllocator::bootstrapAllocator() ),
        m_regionSceneDependencies( Terrain3DEastlAllocator::bootstrapAllocator() ) {
}

void Terrain3DRuntimeWorldManifest::reset( void ) {
	m_bLoaded = false;
	m_nRegionSize = 0;
	m_nRegionMapSize = 0;
	m_fVertexSpacing = 1.0;
	m_worldRegionMin = s_v2iZero;
	m_worldRegionMax = s_v2iZero;
	m_nWorldSignature = 0u;
	m_nMaterialSignature = 0u;
	m_worldHeightRange = s_v2Zero;
	m_regions.clear();
	m_sceneDependencies.clear();
	m_regionSceneDependencies.clear();
	eastl::fill_n( m_regionLookup, s_nMaxRegionSlots, ( int16_t )-1 );
	eastl::fill_n( m_topologyBits, s_nMaxRegionSlots / 64, 0u );
}

void Terrain3DRuntimeWorldManifest::releaseRuntimeAllocatorStorage( void ) {
	// Manifest storage is object-lifetime bootstrap ownership and never enters the runtime zone.
}

String Terrain3DRuntimeWorldManifest::getPath( const String &rDataDirectory ) const {
	return rDataDirectory.is_empty() ? String() : rDataDirectory.trim_suffix( "/" ).path_join( "terrain3d_world.t3dmanifest" );
}

int Terrain3DRuntimeWorldManifest::findSceneDependency( const String &rPath ) const {
	for ( int index = 0; index < ( int )( m_sceneDependencies.size() ); ++index ) {
		if ( m_sceneDependencies[( size_t )( index )] == rPath ) {
			return index;
		}
	}
	return -1;
}

const String &Terrain3DRuntimeWorldManifest::getSceneDependency( const uint32_t nIndex ) const {
	static const String s_empty;
	return nIndex < m_sceneDependencies.size() ? m_sceneDependencies[nIndex] : s_empty;
}

void Terrain3DRuntimeWorldManifest::rebuildLookup( void ) {
	eastl::fill_n( m_regionLookup, s_nMaxRegionSlots, ( int16_t )-1 );
	eastl::fill_n( m_topologyBits, s_nMaxRegionSlots / 64, 0u );
	for ( int index = 0; index < ( int )( m_regions.size() ); ++index ) {
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( m_regions[( size_t )( index )].m_location );
		if ( map_index >= 0 && map_index < s_nMaxRegionSlots ) {
			m_regionLookup[( size_t )( map_index )] = ( int16_t )( index );
			m_topologyBits[( size_t )( map_index >> 6 )] |= 1ull << ( map_index & 63 );
		}
	}
}

bool Terrain3DRuntimeWorldManifest::hasRegion( const Vector2i &rLocation ) const {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	return map_index >= 0 && map_index < s_nMaxRegionSlots
	        && ( m_topologyBits[( size_t )( map_index >> 6 )] & ( 1ull << ( map_index & 63 ) ) ) != 0u;
}

const t3dRuntimeWorldRegionRecord_t *Terrain3DRuntimeWorldManifest::findRegion( const Vector2i &rLocation ) const {
	const int map_index = Terrain3DRegionRepository::getRegionMapIndex( rLocation );
	if ( map_index < 0 || map_index >= s_nMaxRegionSlots ) {
		return NULL;
	}
	const int index = m_regionLookup[( size_t )( map_index )];
	return index >= 0 && index < ( int )( m_regions.size() ) ? &m_regions[( size_t )( index )] : NULL;
}

bool Terrain3DRuntimeWorldManifest::buildAuthoringIndex( const Terrain3DRegionRepository &rRepository ) {
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
		const Terrain3DRegion *const pRegion = rRepository.getRegionPtr( location );
		if ( !pRegion || pRegion->isDeleted() ) {
			continue;
		}
		t3dRuntimeWorldRegionRecord_t record;
		record.m_location = location;
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
		record.m_heightRange = pRegion->getHeightRange();
		record.m_nChannelFlags = ( uint32_t )( pRegion->getChannelFlags() );
		record.m_nInstanceCount = pRegion->getInstanceCount();
		record.m_nInstanceMeshCount = pRegion->getInstanceMeshCount();
		for ( int mesh = 0; mesh < s_nMaxMeshAssets; ++mesh ) {
			record.m_szInstanceMeshCounts[( size_t )( mesh )] = pRegion->getInstanceCount( mesh );
		}
		const eastl::vector<t3dStreamedNodeRuntimeDescriptor_t> &nodes = pRegion->getStreamedNodesRuntimeInternal();
		record.m_nStreamedNodeCount = ( uint16_t )( MIN( nodes.size(), ( size_t )( UINT16_MAX ) ) );
		record.m_nodeCoverageMin = s_v2iZero;
		record.m_nodeCoverageMax = s_v2iZero;
		bool first_coverage = true;
		record.m_nSceneDependencyFirst = ( uint32_t )( m_regionSceneDependencies.size() );
		for ( size_t iNode = 0u; iNode < ( size_t )( ( nodes ).size() ); iNode++ ) {
			const t3dStreamedNodeRuntimeDescriptor_t &descriptor = ( nodes )[iNode];
			if ( descriptor.m_nBoundaryPolicy == STREAMED_NODE_BOUNDARY_COVERAGE ) {
				if ( first_coverage ) {
					record.m_nodeCoverageMin = descriptor.m_coverageMin;
					record.m_nodeCoverageMax = descriptor.m_coverageMax;
					first_coverage = false;
				} else {
					record.m_nodeCoverageMin.x = MIN( record.m_nodeCoverageMin.x, descriptor.m_coverageMin.x );
					record.m_nodeCoverageMin.y = MIN( record.m_nodeCoverageMin.y, descriptor.m_coverageMin.y );
					record.m_nodeCoverageMax.x = MAX( record.m_nodeCoverageMax.x, descriptor.m_coverageMax.x );
					record.m_nodeCoverageMax.y = MAX( record.m_nodeCoverageMax.y, descriptor.m_coverageMax.y );
				}
			}
			const String scene_path = t3dRuntimeWorldManifestDescriptorScenePath( descriptor );
			if ( scene_path.is_empty() ) {
				continue;
			}
			int dependency = findSceneDependency( scene_path );
			if ( dependency < 0 ) {
				dependency = ( int )( m_sceneDependencies.size() );
				m_sceneDependencies.push_back( scene_path );
			}
			bool duplicate = false;
			for ( uint32_t d = record.m_nSceneDependencyFirst; d < m_regionSceneDependencies.size(); ++d ) {
				if ( m_regionSceneDependencies[d] == ( uint16_t )( dependency ) ) {
					duplicate = true;
					break;
				}
			}
			if ( !duplicate && dependency <= UINT16_MAX ) {
				m_regionSceneDependencies.push_back( ( uint16_t )( dependency ) );
			}
		}
		record.m_nSceneDependencyCount = ( uint16_t )( m_regionSceneDependencies.size() - record.m_nSceneDependencyFirst );
		const Ref<Terrain3DRegionLighting> pLighting = pRegion->getLighting();
		if ( pLighting.is_valid() && !pLighting->getLightDataPath().is_empty() ) {
			int dependency = findSceneDependency( pLighting->getLightDataPath() );
			if ( dependency < 0 ) {
				dependency = ( int )m_sceneDependencies.size();
				m_sceneDependencies.push_back( pLighting->getLightDataPath() );
			}
			record.m_nLightmapDependency = ( uint32_t )dependency;
			const String directional_signature = pLighting->getDirectionalLightSignature();
			if ( !directional_signature.is_empty() ) {
				int directional = findSceneDependency( directional_signature );
				if ( directional < 0 ) {
					directional = ( int )m_sceneDependencies.size();
					m_sceneDependencies.push_back( directional_signature );
				}
				record.m_nLightmapDirectionalSignatureDependency = ( uint32_t )directional;
			}
			record.m_nLightmapReceiverCount = ( uint32_t )MAX( pLighting->getReceiverCount(), 0 );
			record.m_nLightmapContributorCount = ( uint32_t )MAX( pLighting->getContributorCount(), 0 );
			record.m_nLightmapEstimatedGpuBytes = ( uint64_t )MAX<int64_t>( pLighting->getEstimatedGpuBytes(), 0 );
			record.m_nLightmapBakeSignature = pLighting->getBakeSignature();
			record.m_nLightmapShadowmaskMode = ( uint16_t )MAX( pLighting->getShadowmaskMode(), 0 );
			record.m_nLightmapFlags = pLighting->hasProbeData() ? 1u : 0u;
			record.m_nLightmapTextureCount = ( uint16_t )MIN( MAX( pLighting->getLightmapTextureCount(), 0 ), ( int )UINT16_MAX );
			record.m_nLightmapShadowmaskTextureCount =
			        ( uint16_t )MIN( MAX( pLighting->getShadowmaskTextureCount(), 0 ), ( int )UINT16_MAX );
			record.m_nLightmapClusterSizeRegions = ( uint16_t )pLighting->getClusterSizeRegions();
			record.m_nLightmapContextRadiusRegions = ( uint16_t )pLighting->getContextRadiusRegions();
			record.m_nLightmapTerrainProxyLod = ( uint16_t )pLighting->getTerrainProxyLod();
		}
		if ( first_height ) {
			m_worldHeightRange = record.m_heightRange;
			first_height = false;
		} else {
			m_worldHeightRange.x = MIN( m_worldHeightRange.x, record.m_heightRange.x );
			m_worldHeightRange.y = MAX( m_worldHeightRange.y, record.m_heightRange.y );
		}
		m_regions.push_back( record );
	}
	rebuildLookup();
	static const Vector2i s_szNeighbors[8] = { Vector2i( 0, -1 ),
		Vector2i( 1, -1 ),
		Vector2i( 1, 0 ),
		Vector2i( 1, 1 ),
		Vector2i( 0, 1 ),
		Vector2i( -1, 1 ),
		Vector2i( -1, 0 ),
		Vector2i( -1, -1 ) };
	for ( size_t iRegion = 0u; iRegion < ( size_t )( ( m_regions ).size() ); iRegion++ ) {
		t3dRuntimeWorldRegionRecord_t &record = ( m_regions )[iRegion];
		uint16_t adjacency = 0u;
		for ( uint16_t bit = 0u; bit < 8u; ++bit ) {
			if ( hasRegion( record.m_location + s_szNeighbors[bit] ) ) {
				adjacency |= ( uint16_t )( 1u << bit );
			}
		}
		record.m_nFlags = ( uint16_t )( ( record.m_nFlags & 0xff00u ) | adjacency );
	}
	return !m_regions.empty();
}

bool Terrain3DRuntimeWorldManifest::write( const String &rDataDirectory,
        const Terrain3DRegionRepository &rRepository,
        const Terrain3DRuntimePageStore &rPageStore,
        const t3dRuntimePageConfig_t &rConfig,
        PackedStringArray *const pErrors ) {
	if ( !buildAuthoringIndex( rRepository ) ) {
		if ( pErrors ) {
			pErrors->push_back( "World manifest has no regions to cook." );
		}
		return false;
	}
	m_nMaterialSignature = rConfig.m_nMaterialSourceSignature;
	uint64_t world_signature = 1469598103934665603ull;
	t3dRuntimeWorldManifestHashU32( world_signature, ( uint32_t )( m_nRegionSize ) );
	t3dRuntimeWorldManifestHashU32( world_signature, ( uint32_t )( m_regions.size() ) );
	for ( size_t iRegion = 0u; iRegion < ( size_t )( ( m_regions ).size() ); iRegion++ ) {
		t3dRuntimeWorldRegionRecord_t &record = ( m_regions )[iRegion];
		const Terrain3DRegion *const pRegion = rRepository.getRegionPtr( record.m_location );
		uint64_t page_signature = 0u;
		const String page_path = rPageStore.getPagePath( pRegion, rDataDirectory );
		if ( !rPageStore.readSourceSignature( page_path, page_signature ) || page_signature == 0u ) {
			if ( pErrors ) {
				pErrors->push_back( String( "Missing/invalid runtime page for manifest region " ) + String( record.m_location ) );
			}
			return false;
		}
		record.m_nPageSourceSignature = page_signature;
		record.m_nMaterialMask = rPageStore.readMaterialMask( page_path );
		if ( record.m_nMaterialMask == 0u ) {
			record.m_nMaterialMask = 1u;
		}
		t3dRuntimeWorldManifestHashBytes( world_signature, &record.m_location.x, sizeof( record.m_location.x ) );
		t3dRuntimeWorldManifestHashBytes( world_signature, &record.m_location.y, sizeof( record.m_location.y ) );
		t3dRuntimeWorldManifestHashBytes( world_signature, &record.m_nPageSourceSignature, sizeof( record.m_nPageSourceSignature ) );
		t3dRuntimeWorldManifestHashBytes( world_signature, &record.m_nMaterialMask, sizeof( record.m_nMaterialMask ) );
		t3dRuntimeWorldManifestHashBytes( world_signature, &record.m_nFlags, sizeof( record.m_nFlags ) );
	}
	m_nWorldSignature = world_signature;

	const String path = getPath( rDataDirectory );
	if ( path.is_empty() ) {
		return false;
	}
	const String temporary = path + String( ".tmp" );
	const String backup = path + String( ".bak" );
	if ( FileAccess::file_exists( temporary ) ) {
		DirAccess::remove_absolute( temporary );
	}
	Ref<FileAccess> pFile = FileAccess::open( temporary, FileAccess::WRITE );
	if ( pFile.is_null() ) {
		if ( pErrors ) {
			pErrors->push_back( "Cannot open world manifest temporary file." );
		}
		return false;
	}
	pFile->store_64( s_nRuntimeWorldManifestMagic );
	pFile->store_32( s_nRuntimeWorldManifestVersion );
	pFile->store_32( s_nManifestHeaderBytes );
	pFile->store_32( ( uint32_t )( m_nRegionSize ) );
	pFile->store_32( ( uint32_t )( m_nRegionMapSize ) );
	pFile->store_32( ( uint32_t )( m_regions.size() ) );
	pFile->store_32( ( uint32_t )( m_sceneDependencies.size() ) );
	pFile->store_32( ( uint32_t )( m_regionSceneDependencies.size() ) );
	pFile->store_32( s_nManifestRegionBaseBytes + ( uint32_t )( s_nMaxMeshAssets * sizeof( uint32_t ) ) );
	pFile->store_64( m_nWorldSignature );
	pFile->store_64( m_nMaterialSignature );
	pFile->store_float( ( float )( m_fVertexSpacing ) );
	pFile->store_float( ( float )( m_worldHeightRange.x ) );
	pFile->store_float( ( float )( m_worldHeightRange.y ) );
	pFile->store_32( ( uint32_t )( m_worldRegionMin.x ) );
	pFile->store_32( ( uint32_t )( m_worldRegionMin.y ) );
	pFile->store_32( ( uint32_t )( m_worldRegionMax.x ) );
	pFile->store_32( ( uint32_t )( m_worldRegionMax.y ) );
	pFile->store_32( s_nManifestTopologyBytes );
	pFile->store_32( 0u );
	while ( pFile->get_position() < s_nManifestHeaderBytes ) {
		pFile->store_8( 0u );
	}
	for ( size_t iTopology = 0u; iTopology < T3D_ARRAY_COUNT( m_topologyBits ); iTopology++ ) {
		uint64_t bits = ( m_topologyBits )[iTopology];
		pFile->store_64( bits );
	}
	for ( size_t iRegion = 0u; iRegion < ( size_t )( ( m_regions ).size() ); iRegion++ ) {
		const t3dRuntimeWorldRegionRecord_t &record = ( m_regions )[iRegion];
		pFile->store_32( ( uint32_t )( record.m_location.x ) );
		pFile->store_32( ( uint32_t )( record.m_location.y ) );
		pFile->store_float( ( float )( record.m_heightRange.x ) );
		pFile->store_float( ( float )( record.m_heightRange.y ) );
		pFile->store_32( record.m_nChannelFlags );
		pFile->store_32( record.m_nMaterialMask );
		pFile->store_64( record.m_nPageSourceSignature );
		pFile->store_32( record.m_nInstanceCount );
		pFile->store_16( record.m_nInstanceMeshCount );
		pFile->store_16( record.m_nStreamedNodeCount );
		pFile->store_32( ( uint32_t )( record.m_nodeCoverageMin.x ) );
		pFile->store_32( ( uint32_t )( record.m_nodeCoverageMin.y ) );
		pFile->store_32( ( uint32_t )( record.m_nodeCoverageMax.x ) );
		pFile->store_32( ( uint32_t )( record.m_nodeCoverageMax.y ) );
		pFile->store_32( record.m_nSceneDependencyFirst );
		pFile->store_16( record.m_nSceneDependencyCount );
		pFile->store_16( record.m_nFlags );
		pFile->store_32( record.m_nLightmapDependency );
		pFile->store_32( record.m_nLightmapDirectionalSignatureDependency );
		pFile->store_32( record.m_nLightmapReceiverCount );
		pFile->store_32( record.m_nLightmapContributorCount );
		pFile->store_64( record.m_nLightmapEstimatedGpuBytes );
		pFile->store_64( ( uint64_t )record.m_nLightmapBakeSignature );
		pFile->store_16( record.m_nLightmapShadowmaskMode );
		pFile->store_16( record.m_nLightmapFlags );
		pFile->store_16( record.m_nLightmapTextureCount );
		pFile->store_16( record.m_nLightmapShadowmaskTextureCount );
		pFile->store_16( record.m_nLightmapClusterSizeRegions );
		pFile->store_16( record.m_nLightmapContextRadiusRegions );
		pFile->store_16( record.m_nLightmapTerrainProxyLod );
		pFile->store_16( 0u );
		for ( size_t iMesh = 0u; iMesh < T3D_ARRAY_COUNT( record.m_szInstanceMeshCounts ); iMesh++ ) {
			uint32_t count = ( record.m_szInstanceMeshCounts )[iMesh];
			pFile->store_32( count );
		}
	}
	for ( size_t iDependency = 0u; iDependency < ( size_t )( ( m_regionSceneDependencies ).size() ); iDependency++ ) {
		uint16_t dependency = ( m_regionSceneDependencies )[iDependency];
		pFile->store_16( dependency );
	}
	for ( size_t iDependency = 0u; iDependency < ( size_t )( ( m_sceneDependencies ).size() ); iDependency++ ) {
		const String &dependency = ( m_sceneDependencies )[iDependency];
		t3dRuntimeWorldManifestStoreString( pFile, dependency );
	}
	const Error write_error = pFile->get_error();
	pFile->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary );
		if ( pErrors ) {
			pErrors->push_back( "World manifest write failed." );
		}
		return false;
	}
	if ( FileAccess::file_exists( backup ) ) {
		DirAccess::remove_absolute( backup );
	}
	const bool had_previous = FileAccess::file_exists( path );
	if ( had_previous && DirAccess::rename_absolute( path, backup ) != OK ) {
		DirAccess::remove_absolute( temporary );
		return false;
	}
	if ( DirAccess::rename_absolute( temporary, path ) != OK ) {
		if ( had_previous ) {
			DirAccess::rename_absolute( backup, path );
		}
		return false;
	}
	if ( FileAccess::file_exists( backup ) ) {
		DirAccess::remove_absolute( backup );
	}
	m_bLoaded = true;
	return true;
}

bool Terrain3DRuntimeWorldManifest::load( const String &rDataDirectory, PackedStringArray *const pErrors ) {
	reset();
	const String path = getPath( rDataDirectory );
	Ref<FileAccess> pFile = FileAccess::open( path, FileAccess::READ );
	if ( pFile.is_null() ) {
		if ( pErrors ) {
			pErrors->push_back( String( "Cooked world manifest is missing: " ) + path );
		}
		return false;
	}
	if ( pFile->get_length() < s_nManifestHeaderBytes || pFile->get_64() != s_nRuntimeWorldManifestMagic
	        || pFile->get_32() != s_nRuntimeWorldManifestVersion || pFile->get_32() != s_nManifestHeaderBytes ) {
		if ( pErrors ) {
			pErrors->push_back( "Cooked world manifest header is invalid." );
		}
		return false;
	}
	m_nRegionSize = ( int )( pFile->get_32() );
	m_nRegionMapSize = ( int )( pFile->get_32() );
	const uint32_t region_count = pFile->get_32();
	const uint32_t dependency_count = pFile->get_32();
	const uint32_t region_dependency_count = pFile->get_32();
	const uint32_t record_bytes = pFile->get_32();
	m_nWorldSignature = pFile->get_64();
	m_nMaterialSignature = pFile->get_64();
	m_fVertexSpacing = pFile->get_float();
	m_worldHeightRange.x = pFile->get_float();
	m_worldHeightRange.y = pFile->get_float();
	m_worldRegionMin.x = ( int32_t )( pFile->get_32() );
	m_worldRegionMin.y = ( int32_t )( pFile->get_32() );
	m_worldRegionMax.x = ( int32_t )( pFile->get_32() );
	m_worldRegionMax.y = ( int32_t )( pFile->get_32() );
	const uint32_t topology_bytes = pFile->get_32();
	pFile->get_32();
	if ( !t3dIsValidRegionSize( m_nRegionSize ) || m_nRegionMapSize != s_nActiveWorldTexels / m_nRegionSize || region_count == 0u
	        || region_count > s_nMaxRegionSlots || dependency_count > UINT16_MAX
	        || record_bytes != s_nManifestRegionBaseBytes + s_nMaxMeshAssets * sizeof( uint32_t )
	        || topology_bytes != s_nManifestTopologyBytes || m_fVertexSpacing < 0.25f || m_fVertexSpacing > 100.0f ) {
		if ( pErrors ) {
			pErrors->push_back( "Cooked world manifest dimensions are invalid." );
		}
		return false;
	}
	pFile->seek( s_nManifestHeaderBytes );
	for ( size_t iTopology = 0u; iTopology < T3D_ARRAY_COUNT( m_topologyBits ); iTopology++ ) {
		uint64_t &bits = ( m_topologyBits )[iTopology];
		bits = pFile->get_64();
	}
	m_regions.resize( region_count );
	for ( uint32_t index = 0u; index < region_count; ++index ) {
		t3dRuntimeWorldRegionRecord_t &record = m_regions[index];
		record.m_location.x = ( int32_t )( pFile->get_32() );
		record.m_location.y = ( int32_t )( pFile->get_32() );
		record.m_heightRange.x = pFile->get_float();
		record.m_heightRange.y = pFile->get_float();
		record.m_nChannelFlags = pFile->get_32();
		record.m_nMaterialMask = pFile->get_32();
		record.m_nPageSourceSignature = pFile->get_64();
		record.m_nInstanceCount = pFile->get_32();
		record.m_nInstanceMeshCount = pFile->get_16();
		record.m_nStreamedNodeCount = pFile->get_16();
		record.m_nodeCoverageMin.x = ( int32_t )( pFile->get_32() );
		record.m_nodeCoverageMin.y = ( int32_t )( pFile->get_32() );
		record.m_nodeCoverageMax.x = ( int32_t )( pFile->get_32() );
		record.m_nodeCoverageMax.y = ( int32_t )( pFile->get_32() );
		record.m_nSceneDependencyFirst = pFile->get_32();
		record.m_nSceneDependencyCount = pFile->get_16();
		record.m_nFlags = pFile->get_16();
		record.m_nLightmapDependency = pFile->get_32();
		record.m_nLightmapDirectionalSignatureDependency = pFile->get_32();
		record.m_nLightmapReceiverCount = pFile->get_32();
		record.m_nLightmapContributorCount = pFile->get_32();
		record.m_nLightmapEstimatedGpuBytes = pFile->get_64();
		record.m_nLightmapBakeSignature = ( int64_t )pFile->get_64();
		record.m_nLightmapShadowmaskMode = pFile->get_16();
		record.m_nLightmapFlags = pFile->get_16();
		record.m_nLightmapTextureCount = pFile->get_16();
		record.m_nLightmapShadowmaskTextureCount = pFile->get_16();
		record.m_nLightmapClusterSizeRegions = pFile->get_16();
		record.m_nLightmapContextRadiusRegions = pFile->get_16();
		record.m_nLightmapTerrainProxyLod = pFile->get_16();
		pFile->get_16();
		if ( !t3dRegionLocationValid( record.m_location, m_nRegionMapSize ) || record.m_nMaterialMask == 0u
		        || record.m_nPageSourceSignature == 0u
		        || record.m_nSceneDependencyFirst + record.m_nSceneDependencyCount > region_dependency_count ) {
			if ( pErrors ) {
				pErrors->push_back( "Cooked world manifest contains an invalid region record." );
			}
			reset();
			return false;
		}
		for ( size_t iMesh = 0u; iMesh < T3D_ARRAY_COUNT( record.m_szInstanceMeshCounts ); iMesh++ ) {
			uint32_t &count = ( record.m_szInstanceMeshCounts )[iMesh];
			count = pFile->get_32();
		}
	}
	m_regionSceneDependencies.resize( region_dependency_count );
	for ( uint32_t index = 0u; index < region_dependency_count; ++index ) {
		m_regionSceneDependencies[index] = pFile->get_16();
	}
	m_sceneDependencies.resize( dependency_count );
	for ( uint32_t index = 0u; index < dependency_count; ++index ) {
		if ( !t3dRuntimeWorldManifestReadString( pFile, m_sceneDependencies[index] ) ) {
			if ( pErrors ) {
				pErrors->push_back( "Cooked world manifest dependency table is truncated." );
			}
			reset();
			return false;
		}
	}
	for ( size_t iDependency = 0u; iDependency < ( size_t )( ( m_regionSceneDependencies ).size() ); iDependency++ ) {
		const uint16_t dependency = ( m_regionSceneDependencies )[iDependency];
		if ( dependency >= dependency_count ) {
			if ( pErrors ) {
				pErrors->push_back( "Cooked world manifest contains an invalid region dependency index." );
			}
			reset();
			return false;
		}
	}
	for ( size_t iRegion = 0u; iRegion < ( size_t )( ( m_regions ).size() ); iRegion++ ) {
		const t3dRuntimeWorldRegionRecord_t &record = ( m_regions )[iRegion];
		if ( ( record.m_nLightmapDependency != UINT32_MAX && record.m_nLightmapDependency >= dependency_count )
		        || ( record.m_nLightmapDirectionalSignatureDependency != UINT32_MAX
		                && record.m_nLightmapDirectionalSignatureDependency >= dependency_count ) ) {
			if ( pErrors ) {
				pErrors->push_back( "Cooked world manifest contains an invalid lightmap dependency index." );
			}
			reset();
			return false;
		}
		const String source_path =
		        rDataDirectory.trim_suffix( "/" ) + String( "/" ) + Terrain3DUtil::locationToFilename( record.m_location );
		const String page_path = source_path.get_basename() + String( ".t3dpage" );
		if ( !FileAccess::file_exists( page_path ) ) {
			if ( pErrors ) {
				pErrors->push_back( String( "Cooked runtime page is missing: " ) + page_path );
			}
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

bool Terrain3DRuntimeWorldManifest::stageRepository( const String &rDataDirectory, t3dStagedRepository_t &rStaged ) const {
	if ( !m_bLoaded || m_nRegionSize <= 0 || m_regions.empty() ) {
		return false;
	}
	rStaged.reset();
	rStaged.m_bBoundedCpuLoad = true;
	rStaged.m_nTargetRegionSize = m_nRegionSize;
	rStaged.m_regions.reserve( m_regions.size() );
	for ( int slot = 0; slot < ( int )( m_regions.size() ); ++slot ) {
		const t3dRuntimeWorldRegionRecord_t &record = m_regions[( size_t )( slot )];
		Ref<Terrain3DRegion> pRegion;
		pRegion.instantiate();
		pRegion->setLocation( record.m_location );
		pRegion->setRegionSize( m_nRegionSize );
		pRegion->setVertexSpacing( m_fVertexSpacing );
		pRegion->setHeightRange( record.m_heightRange );
		pRegion->setChannelFlags( ( int )( record.m_nChannelFlags ) );
		pRegion->setCookedPageSourceSignatureInternal( record.m_nPageSourceSignature );
		pRegion->setCookedInstanceMetadataInternal( record.m_szInstanceMeshCounts, record.m_nInstanceCount, record.m_nInstanceMeshCount );
		pRegion->setCookedStreamedNodeMetadataInternal( record.m_nStreamedNodeCount, record.m_nodeCoverageMin, record.m_nodeCoverageMax );
		if ( record.m_nLightmapDependency != UINT32_MAX && record.m_nLightmapDependency < m_sceneDependencies.size() ) {
			Ref<Terrain3DRegionLighting> lighting;
			lighting.instantiate();
			lighting->setLightDataPath( m_sceneDependencies[record.m_nLightmapDependency] );
			lighting->setClusterLocation( record.m_location );
			lighting->setRegionMin( record.m_location );
			lighting->setRegionMax( record.m_location );
			lighting->setClusterSizeRegions( record.m_nLightmapClusterSizeRegions );
			lighting->setTerrainProxyLod( record.m_nLightmapTerrainProxyLod );
			lighting->setContextRadiusRegions( record.m_nLightmapContextRadiusRegions );
			lighting->setShadowmaskMode( record.m_nLightmapShadowmaskMode );
			lighting->setBakeSignature( record.m_nLightmapBakeSignature );
			if ( record.m_nLightmapDirectionalSignatureDependency != UINT32_MAX
			        && record.m_nLightmapDirectionalSignatureDependency < m_sceneDependencies.size() ) {
				lighting->setDirectionalLightSignature( m_sceneDependencies[record.m_nLightmapDirectionalSignatureDependency] );
			}
			lighting->setReceiverCount( ( int )record.m_nLightmapReceiverCount );
			lighting->setContributorCount( ( int )record.m_nLightmapContributorCount );
			lighting->setLightmapTextureCount( record.m_nLightmapTextureCount );
			lighting->setShadowmaskTextureCount( record.m_nLightmapShadowmaskTextureCount );
			lighting->setHasProbeData( ( record.m_nLightmapFlags & 1u ) != 0u );
			lighting->setEstimatedGpuBytes( ( int64_t )record.m_nLightmapEstimatedGpuBytes );
			pRegion->setLighting( lighting );
		}
		pRegion->setModified( false );
		pRegion->markRuntimeMapsCleanInternal();
		t3dStagedRegion_t entry;
		entry.m_location = record.m_location;
		entry.m_path = rDataDirectory.path_join( Terrain3DUtil::locationToFilename( record.m_location ) );
		entry.m_region = pRegion;
		const int half_map = m_nRegionMapSize / 2;
		entry.m_nActiveSlot = ( record.m_location.y + half_map ) * m_nRegionMapSize + record.m_location.x + half_map;
		rStaged.m_regions.push_back( entry );
		const int map_index = Terrain3DRegionRepository::getRegionMapIndex( record.m_location );
		rStaged.m_resources[record.m_location] = pRegion;
		rStaged.m_pRegionPtrs[( size_t )( map_index )] = pRegion.ptr();
		rStaged.m_regionLocations[( size_t )( slot )] = record.m_location;
		rStaged.m_logicalRegionMap[( size_t )( map_index )] = ( uint16_t )( slot + 1 );
	}
	return true;
}
