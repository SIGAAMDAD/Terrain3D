// Startup cache restoration exercises real cooked fragments and file I/O without a GPU.
#include "authoring/vt/build/terrain_3d_incremental_megavt_cooker.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "diagnostics/tests/unit_testing.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace {
class CacheTestSource final : public Terrain3DRegionSource {
public:
	Ref<Terrain3DRegion> regions[2];
	int residentCalls = 0;
	CacheTestSource() {
		for ( auto &region : regions ) { region.instantiate(); region->setMegaVTSourceGeneration( 7 ); }
	}
	int getRegionCount() const override { return 2; }
	bool hasRegion( const Vector2i &location ) const override {
		return location.y == 0 && location.x >= 0 && location.x < 2;
	}
	TypedArray<Vector2i> getRegionLocations() const override {
		TypedArray<Vector2i> locations;
		locations.push_back( Vector2i( 0, 0 ) ); locations.push_back( Vector2i( 1, 0 ) ); return locations;
	}
	Ref<Terrain3DRegion> getRegion( const Vector2i &location ) const override {
		return hasRegion( location ) ? regions[location.x] : Ref<Terrain3DRegion>();
	}
	bool ensureCpuRegionResident( const Vector2i &location ) override { ++residentCalls; return hasRegion( location ); }
};

bool writeBytes( const String &path, const PackedByteArray &bytes ) {
	const Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() ) { return false; }
	file->store_buffer( bytes ); return file->get_error() == OK;
}

bool makeFragment( const uint32_t index, PackedByteArray &fragment ) {
	PackedByteArray albedo, pbr;
	albedo.resize( 256 * 256 * 4 ); pbr.resize( 128 * 128 * 4 );
	albedo.fill( 170 ); pbr.fill( 128 );
	t3dVTCookSparseSourceV3_t source{};
	source.m_nStableKey = t3d_vt_terrain_region_stable_key( index );
	source.m_nGenerationSeed = 7;
	source.m_nConsumer = T3D_VT_CONSUMER_CLIPMAP;
	source.m_nWrapMode = T3D_VT_COOK_WRAP_CLAMP;
	source.m_nAllocationFlags = T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP | T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE;
	source.m_nAuthoredChannelMask = T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO )
	        | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_PBR );
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = { albedo.ptr(), 256, 256, 4, 1024 };
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_PBR] = { pbr.ptr(), 128, 128, 4, 512 };
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = t3dHashPublicationBytes( albedo.ptr(), albedo.size() );
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_PBR] = t3dHashPublicationBytes( pbr.ptr(), pbr.size() );
	return t3d_vt_godot_cook_source_fragment_v3( source, fragment, 32ull * 1024 * 1024 );
}

int64_t restored( Terrain3DIncrementalMegaVTCooker &cooker ) {
	return ( int64_t )cooker.getDiagnosticsCold()["cache_restored_regions"];
}
}

bool t3dRunMegaVTCacheTests() {
	uint32_t passed = 0, failed = 0;
	auto expect = [&]( const bool condition, const char *name ) {
		if ( condition ) { ++passed; } else { ++failed; }
		UtilityFunctions::print( condition ? "PASSED: " : "FAILED: ", name );
	};
	const String directory = ProjectSettings::get_singleton()->globalize_path( "res://.godot/terrain3d/tests" ).path_join(
	        "megavt_cache_test_" + String::num_int64( OS::get_singleton()->get_process_id() ) );
	const String cacheDirectory = directory.path_join( ".vt_cache/terrain_regions" );
	if ( DirAccess::make_dir_recursive_absolute( cacheDirectory ) != OK ) { return false; }
	uint32_t indices[2]{};
	Terrain3DTerrainMegaVT::locationToRegionIndex( 0, 0, indices[0] );
	Terrain3DTerrainMegaVT::locationToRegionIndex( 1, 0, indices[1] );
	String paths[2]; PackedByteArray fragments[2];
	bool fixtureReady = true;
	for ( uint32_t i = 0; i < 2; ++i ) {
		paths[i] = cacheDirectory.path_join( String::num_int64( indices[i] ) + ".t3dvt" );
		fixtureReady &= makeFragment( indices[i], fragments[i] ) && writeBytes( paths[i], fragments[i] );
	}
	expect( fixtureReady, "MegaVT cache fixtures use valid production-cooked fragments" );
	if ( fixtureReady ) {
		CacheTestSource source;
		t3dMegaVTAuthoringInputs_t inputs{ source, directory };
		{
			// A large subsystem ceiling must not reserve idle space and starve
			// instance material packages that share the same terrain account.
			Terrain3DMemoryAccountOwner owner(8ull * 1024 * 1024);
			Terrain3DMemoryAccountScope scope(owner.get());
			Terrain3DIncrementalMegaVTCooker admitted;
			admitted.restoreCachedRegionsCold({source, directory, 128ull * 1024 * 1024}, 2);
			expect(restored(admitted) == 2, "owner budget admits cache bytes without reserving the entire cooker ceiling");
			const uint64_t retainedPayload = uint64_t(fragments[0].size()) + fragments[1].size() + 128u;
			expect(admitted.getReservedCpuBytesCold() == retainedPayload && owner.get()->stats().committed == retainedPayload,
			        "idle cooker charges only retained Godot fragment storage");
			Terrain3DTerrainMegaVT emptyMegaVT;
			admitted.shutdownCold(emptyMegaVT);
			expect(owner.get()->stats().committed == 0, "cooker shutdown returns its payload allowance");
		}
		for (const uint64_t totalMb : {256ull, 320ull, 359ull, 360ull}) {
			Terrain3DMemoryAccountOwner owner(totalMb * 1024 * 1024);
			Terrain3DMemoryAccountScope scope(owner.get());
			Terrain3DIncrementalMegaVTCooker admitted;
			admitted.restoreCachedRegionsCold({source, directory, t3dMemoryCookingLimit(owner.get())}, 2);
			expect(restored(admitted) == 2, "cached regions restore with a total budget at or below 360 MiB");
			// Fill the retained-cache allowance. Composition must be able to borrow
			// unused shared space rather than requiring another copy in this grant.
			admitted.restoreCachedRegionsCold({source, directory, admitted.getRetainedCpuBytesCold()}, 2);
			Terrain3DMemoryReservation otherConsumers;
			const auto stats = owner.get()->stats();
			expect(otherConsumers.resize(stats.limit - stats.committed - 8ull * 1024 * 1024),
			        "other terrain consumers leave a bounded 8 MiB composition window");
			PackedByteArray package; uint32_t generation = 0; uint64_t fingerprint = 0;
			Terrain3DSharedMemoryReservation packageMemory;
			t3dVTPackViewV3_t view{};
			expect(admitted.composePackageCold(package, generation, fingerprint, &packageMemory)
			        && t3d_vt_pack_open_view_v3(package.ptr(), package.size(), view)
			        && view.m_pHeader->m_nAllocationCount == 2,
			        "composition succeeds from shared headroom with a fully occupied cache allowance");
			expect(owner.get()->stats().peak <= stats.limit, "composition never exceeds the total owner limit");
			package.clear(); packageMemory.reset();
			Terrain3DMemoryReservation pressure;
			expect(pressure.resize(stats.limit - owner.get()->stats().committed), "composition headroom can be exhausted");
			expect(!admitted.composePackageCold(package, generation, fingerprint, &packageMemory)
			        && package.is_empty() && owner.get()->stats().denied > 0,
			        "real total-budget exhaustion rejects composition and reports an admission denial");
			pressure.reset();
			expect(admitted.composePackageCold(package, generation, fingerprint, &packageMemory),
			        "unchanged cached regions compose again after shared headroom returns");
		}
		source.residentCalls = 0;
		Terrain3DIncrementalMegaVTCooker cooker;
		cooker.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( cooker ) == 2, "startup restores all clean region fragments before VT initialization" );
		expect( ( int64_t )cooker.getDiagnosticsCold()["submitted"] == 0, "cache restoration queues no cooking work" );
		PackedByteArray package; uint32_t generation = 0; uint64_t fingerprint = 0;
		t3dVTPackViewV3_t view{};
		expect( cooker.composePackageCold( package, generation, fingerprint ) && !package.is_empty()
		        && t3d_vt_pack_open_view_v3( package.ptr(), package.size(), view ) && view.m_pHeader->m_nAllocationCount == 2,
		        "one initial package includes both restored regions" );
		cooker.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( cooker ) == 2 && source.residentCalls == 2, "restoration does not poll files or reload source regions" );
		Terrain3DIncrementalMegaVTCooker reopened;
		reopened.restoreCachedRegionsCold( inputs, 2 );
		PackedByteArray reopenedPackage; uint32_t reopenedGeneration = 0; uint64_t reopenedFingerprint = 0;
		expect( reopened.composePackageCold( reopenedPackage, reopenedGeneration, reopenedFingerprint )
		        && reopenedFingerprint == fingerprint && reopenedPackage == package,
		        "editor restart restores the identical package without cooking" );
		Terrain3DTerrainMegaVT megaVT;
		// Establish CPU region identities without allocating GPU resources.
		for ( uint32_t i = 0; i < 2; ++i ) {
			t3dTerrainMegaVTRegion_t *const region = const_cast<t3dTerrainMegaVTRegion_t *>( megaVT.getRegion( indices[i] ) );
			region->m_nAllocationId = i; region->m_nGeneration = 7;
			region->m_nFlags = T3D_TERRAIN_MEGAVT_REGION_PRESENT;
			uint32_t pages = 0;
			Terrain3DTerrainMegaVT::computeFootprint( 2, region->m_nMip0WidthPages, region->m_nMip0HeightPages, region->m_nMipCount, pages );
		}
		uint32_t sourceGeneration = 0, dirtyMask = 0;
		expect( megaVT.importPersistentEditorStateHot( indices[0], 7, 15 )
		        && megaVT.getPersistentEditorState( indices[0], sourceGeneration, dirtyMask )
		        && sourceGeneration == 7 && dirtyMask == 15,
		        "queuing missing derived data preserves its saved source generation" );
		expect( megaVT.getPersistentEditorState( indices[1], sourceGeneration, dirtyMask )
		        && sourceGeneration == 7 && dirtyMask == 0,
		        "a missing region cache does not invalidate cached neighbors" );
		cooker.shutdownCold( megaVT ); cooker.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( cooker ) == 4, "cold shutdown permits cache restoration on the next lifecycle" );
		source.regions[0]->setMegaVTSourceGeneration( 8 );
		Terrain3DIncrementalMegaVTCooker stale;
		stale.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( stale ) == 1, "stale source generation falls back while other regions remain cached" );
		source.regions[0]->setMegaVTSourceGeneration( 7 );
		source.regions[0]->setMegaVTDirtyMip0Mask( 1 );
		Terrain3DIncrementalMegaVTCooker dirty;
		dirty.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( dirty ) == 1, "dirty authored region is not replaced by its cached fragment" );
		source.regions[0]->setMegaVTDirtyMip0Mask( 0 );
		Terrain3DIncrementalMegaVTCooker resolution;
		resolution.restoreCachedRegionsCold( inputs, 4 );
		expect( restored( resolution ) == 0, "different texel resolution rejects the old cached layout" );
		Terrain3DIncrementalMegaVTCooker bounded;
		bounded.restoreCachedRegionsCold( { source, directory, 256 }, 2 );
		expect( restored( bounded ) == 0, "cache file reads respect the CPU memory limit" );
		Terrain3DIncrementalMegaVTCooker tight;
		const uint64_t nTightLimit = tight.getRetainedCpuBytesCold() + ( uint64_t )fragments[0].size() + 64u;
		tight.restoreCachedRegionsCold( { source, directory, nTightLimit }, 2 );
		expect( restored( tight ) == 1 && tight.getRetainedCpuBytesCold() <= nTightLimit,
		        "batched restoration updates its budget after each accepted fragment" );
		expect( writeBytes( paths[0], fragments[1] ), "wrong-region cache fixture saved" );
		Terrain3DIncrementalMegaVTCooker wrongRegion;
		wrongRegion.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( wrongRegion ) == 1, "wrong stable region identity rejects the fragment" );
		PackedByteArray damaged = fragments[0]; damaged.set( damaged.size() - 1, damaged[damaged.size() - 1] ^ 1 );
		expect( writeBytes( paths[0], damaged ), "damaged payload fixture saved" );
		Terrain3DIncrementalMegaVTCooker corrupt;
		corrupt.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( corrupt ) == 1, "corrupt payload checksum rejects the fragment" );
		damaged.resize( 16 ); expect( writeBytes( paths[0], damaged ), "truncated fixture saved" );
		Terrain3DIncrementalMegaVTCooker truncated;
		truncated.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( truncated ) == 1, "truncated cache file falls back safely" );
		DirAccess::remove_absolute( paths[0] );
		Terrain3DIncrementalMegaVTCooker missing;
		missing.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( missing ) == 1, "missing cache file leaves other cached regions usable" );
		writeBytes( paths[0], fragments[0] ); missing.restoreCachedRegionsCold( inputs, 2 );
		expect( restored( missing ) == 1, "a cache miss is not retried each frame" );
	}
	for ( const String &path : paths ) { DirAccess::remove_absolute( path ); }
	DirAccess::remove_absolute( cacheDirectory );
	DirAccess::remove_absolute( directory.path_join( ".vt_cache" ) );
	DirAccess::remove_absolute( directory );
	UtilityFunctions::print( "=== MegaVT cache tests: ", passed, " passed, ", failed, " failed ===" );
	return failed == 0;
}
