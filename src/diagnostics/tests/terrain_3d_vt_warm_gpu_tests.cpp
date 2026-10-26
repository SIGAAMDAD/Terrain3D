// Real RenderingDevice publication checks without terrain shaders or scene setup.
#include "diagnostics/tests/unit_testing.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include <godot_cpp/variant/utility_functions.hpp>

namespace {
bool makePackage( uint8_t color, uint32_t generation, PackedByteArray &package ) {
	PackedByteArray albedo, pbr;
	albedo.resize( 256 * 256 * 4 ); pbr.resize( 128 * 128 * 4 ); albedo.fill( color ); pbr.fill( 128 );
	t3dVTCookSparseSourceV3_t source{};
	source.m_nStableKey = t3d_vt_terrain_region_stable_key( 7 ); source.m_nGenerationSeed = generation;
	source.m_nConsumer = T3D_VT_CONSUMER_CLIPMAP; source.m_nWrapMode = T3D_VT_COOK_WRAP_CLAMP;
	source.m_nAllocationFlags = T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP | T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE;
	source.m_nAuthoredChannelMask = T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO )
	        | T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_PBR );
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = { albedo.ptr(), 256, 256, 4, 1024 };
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_PBR] = { pbr.ptr(), 128, 128, 4, 512 };
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = t3dHashPublicationBytes( albedo.ptr(), albedo.size() );
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_PBR] = t3dHashPublicationBytes( pbr.ptr(), pbr.size() );
	return t3d_vt_godot_cook_source_fragment_v3( source, package, 32ull * 1024 * 1024 );
}
}

bool t3dRunVTWarmGpuTests() {
	uint32_t passed = 0, failed = 0;
	const auto expect = [&]( bool value, const char *name ) {
		value ? ++passed : ++failed; UtilityFunctions::print( value ? "PASSED: " : "FAILED: ", name );
	};
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	const bool deviceReady = Terrain3DRdContext::isOnRenderThread() && context.initialize();
	expect( deviceReady, "warm GPU checks have a real device and render-thread ownership" );
	if ( !deviceReady ) { return false; }
	// Real allocation rejection precedes the device call and preserves resident
	// texture ownership. Shared views never consume a second physical reservation.
	{
		Terrain3DMemoryAccountOwner owner(16ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		expect(owner.get()->setDomainLimit(Terrain3DMemoryAccount::VIRTUAL_TEXTURE, 64), "VT GPU test installs its live resource cap");
		Terrain3DRdTexture texture, rejected;
		t3dRdTextureSpec_t spec;
		spec.m_name = "BudgetTest.Texture"; spec.m_owner = T3D_RD_CONTEXT_OWNER_VT;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
		spec.m_layout.m_nWidth = 4; spec.m_layout.m_nHeight = 4; spec.m_layout.m_nLayers = 1;
		spec.m_layout.m_nMipmaps = 1; spec.m_layout.m_nBytes = 64;
		spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		spec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
		expect(texture.createTracked(context, spec), "actual texture creation fits an exact VT GPU cap");
		spec.m_name = "BudgetTest.Rejected";
		const RID resident = texture.rid();
		expect(!rejected.createTracked(context, spec) && texture.rid() == resident && texture.isValid(),
		        "VT cap rejects a second texture without changing the resident texture");
		expect(owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 64
		        && owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].denied == 1,
		        "GPU domain tracker records physical capacity and rejected allocations");
		texture.releaseTracked(context);
		expect(owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 0
		        && owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].peak == 64,
		        "texture retirement releases its original domain and keeps its allocation-time peak");
	}
	// Exercise actual RD calls: creation payloads are traffic, shared views and
	// rejected updates are not. No terrain shader compilation is required.
	{
		Terrain3DMemoryAccountOwner owner(16ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		owner.get()->setDomainLimit(Terrain3DMemoryAccount::INSTANCE_RENDER, 64);
		Terrain3DRdGpuBuffer buffer;
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "BudgetTest.WorldInstances"; spec.m_owner = T3D_RD_CONTEXT_OWNER_WORLD;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_INSTANCE; spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		expect(buffer.configure(spec) && buffer.createTracked(context, 64), "world instance storage fits its instance GPU cap");
		const RID resident = buffer.rid();
		expect(!buffer.replaceTracked(context, 64) && buffer.rid() == resident,
		        "buffer replacement must admit both old and new capacity under the instance cap");
		expect(owner.get()->stats().domains[Terrain3DMemoryAccount::INSTANCE_RENDER].committed == 64
		        && owner.get()->stats().domains[Terrain3DMemoryAccount::WORLD_RENDER].committed == 0,
		        "unified-world instance buffers count under the instance resource domain");
		buffer.releaseTracked(context);
	}
	context.beginFrame(1001, 1);
	{
		Terrain3DRdTexture texture;
		t3dRdTextureSpec_t spec;
		spec.m_name = "UploadTest.Texture"; spec.m_owner = T3D_RD_CONTEXT_OWNER_VT;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
		spec.m_layout.m_nWidth = 4; spec.m_layout.m_nHeight = 4; spec.m_layout.m_nLayers = 1;
		spec.m_layout.m_nMipmaps = 1; spec.m_layout.m_nBytes = 64;
		spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		spec.m_layout.m_nUsageBits = RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
		PackedByteArray pixels; pixels.resize(64); pixels.fill(90);
		TypedArray<PackedByteArray> initial; initial.push_back(pixels);
		const uint64_t before = context.getRenderUploadBytes(T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_PUBLICATION);
		expect(texture.createTracked(context, spec, initial), "texture creation accepts the initial payload");
		expect(context.getRenderUploadBytes(T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_PUBLICATION) == before + 64,
		        "texture creation counts supplied bytes once");
		const uint64_t dirty = context.getRenderUploadBytes(T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_DIRTY_UPLOAD);
		expect(texture.updateLayerTracked(context, 0, pixels) && !texture.updateLayerTracked(context, 1, pixels),
		        "texture updates reject invalid layers before submitting");
		expect(context.getRenderUploadBytes(T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_DIRTY_UPLOAD) == dirty + 64,
		        "only the successful texture update contributes traffic");
		texture.releaseTracked(context);
	}
	{
		Terrain3DWorldPublication world;
		Terrain3DWorldResources resources;
		expect(t3dVectorResizeCold(world.m_vertices, 32) && t3dVectorResizeCold(world.m_materials, 1),
		        "world upload fixture admits its publication storage");
		world.m_nGeneration = 1;
		expect(resources.ensureCapacityCold(world) && resources.uploadPublicationWarm(world), "world uploads its initial tables");
		const uint64_t first = resources.getUploadBytes();
		const uint64_t refresh = T3D_WORLD_VIS_COUNTER_COUNT * sizeof(uint32_t)
		        + T3D_WORLD_INDIRECT_COMMAND_COUNT * sizeof(t3dWorldDrawIndirectArgs_t);
		world.m_nGeneration = 2;
		expect(resources.uploadPublicationWarm(world) && resources.getUploadBytes() - first == refresh,
		        "new publication identity preserves unchanged geometry and material buffers");
		const uint64_t second = resources.getUploadBytes();
		reinterpret_cast<uint8_t *>(world.m_vertices.data())[0] ^= 1;
		world.m_nGeneration = 3;
		expect(resources.uploadPublicationWarm(world)
		        && resources.getUploadBytes() - second == refresh + world.m_vertices.size() * sizeof(t3dWorldVertex_t),
		        "changed vertex data uploads its table while unchanged material data stays resident");
		const uint64_t skipped = resources.getSkippedPublicationUploadBytes();
		resources.shutdownCold();
		const uint64_t third = resources.getUploadBytes();
		expect(resources.ensureCapacityCold(world) && resources.uploadPublicationWarm(world)
		        && resources.getUploadBytes() - third == first && resources.getSkippedPublicationUploadBytes() == skipped,
		        "recreated GPU storage receives all tables even when CPU bytes are identical");
		UtilityFunctions::print("GPU_UPLOAD_FIXTURE initial=", first, " unchanged_publication=", refresh,
		        " changed_vertices=", resources.getUploadBytes() - second - first, " skipped_bytes=", skipped);
		resources.shutdownCold();
	}
	PackedByteArray first, second, backing;
	expect( makePackage( 170, 7, first ) && makePackage( 90, 8, second ), "warm GPU fixtures use production-cooked packages" );
	backing.resize( 32 * 1024 * 1024 );
	Terrain3DMemoryZone *zone = memnew( Terrain3DMemoryZone );
	expect( zone->initialize( backing.ptrw(), backing.size() ), "warm GPU fixture binds one retained runtime zone" );
	{
		Terrain3DVirtualTexture vt;
		Terrain3DSharedVTRuntime shared;
		Terrain3DMemoryAccountOwner owner(64ull * 1024 * 1024);
		Terrain3DMemoryAccountScope scope(owner.get());
		const bool source_ready = vt.bindRuntimeAllocator(zone) && vt.loadPackageCold(first.ptr(), first.size());
		auto capped_limits = t3d_world_make_preview_limits(8, 256);
		capped_limits.nMaxVtPhysicalPageSets = 3;
		t3dVTGpuResourcePlan_t capped_plan{};
		const bool plan_ready = source_ready && t3d_vt_compute_resource_plan(capped_limits, t3d_vt_make_preview_gpu_profile(),
		        capped_plan, vt.getLoadedPackageTailAtlasHeightCold());
		expect(plan_ready && owner.get()->setDomainLimit(Terrain3DMemoryAccount::VIRTUAL_TEXTURE, capped_plan.nTotalBytes),
		        "real VT resource fixture installs a cap smaller than its default physical cache");
		const bool initialized = source_ready
		        && shared.bindRuntimeAllocator( zone ) && shared.bindVirtualTexture( &vt )
		        && shared.ensure_shared_resources_cold( context, T3D_VT_SHARED_RESOURCES_PREVIEW, 8, 256 );
		expect( initialized, "first package completes one cold VT initialization" );
		if ( initialized ) {
			expect(shared.get_resources().get_plan().nTotalBytes <= capped_plan.nTotalBytes
			        && vt.get_residency().Buffers.nPhysicalPageSetCapacity == 3,
			        "real VT initialization fits its manual GPU cap and preserves the complete database");
			vt.seal_hot_runtime();
			const t3dSharedVTBindings_t bindings = shared.get_shared_vt_bindings();
			const uint64_t creates = shared.get_resources().get_stats().nGpuCreateCount;
			const uint64_t frees = shared.get_resources().get_stats().nGpuFreeCount;
			const uint64_t bytes = vt.get_cpu_image_bytes();
			const void *allocations = vt.get_residency().Buffers.pAllocations;
			const uint64_t signature = vt.getPackageContentSignature();
			expect( vt.getPackagePublicationGeneration() == 1, "initial publication has its own generation" );
			expect( shared.stagePackageWarm( second.ptr(), second.size() ), "compatible package stages independently of live ownership" );
			expect( vt.getPackageContentSignature() == signature, "staging keeps last-good database visible" );
			shared.abortPackageRepublishWarm();
			expect( !vt.isPackageRepublishPendingWarm() && vt.getPackageContentSignature() == signature,
			        "aborted candidate leaves last-good package intact" );
			expect( shared.stagePackageWarm( second.ptr(), second.size() ) && shared.servicePackageRepublishWarm( 4096, 1 )
			        && !vt.isPackageRepublishPendingWarm(), "small upload slices commit one complete warm publication" );
			expect( vt.getPackageContentSignature() != signature && vt.getPackagePublicationGeneration() == 2,
			        "warm commit changes content and advances publication generation" );
			uint32_t cookedGeneration = 0;
			expect( vt.getTerrainMegaVTCookedGenerationCold( 7, cookedGeneration ) && cookedGeneration == 8,
			        "committed package retains coherent region identity and source generation" );
			t3dVTMaterialReadSnapshot_t materialSnapshot{};
			expect( vt.snapshotMaterialReadStateCold( materialSnapshot )
			        && materialSnapshot.nDatabaseGeneration == vt.getDatabaseGeneration()
			        && materialSnapshot.nPackageContentSignature == vt.getPackageContentSignature()
			        && materialSnapshot.nPackagePublicationGeneration == vt.getPackagePublicationGeneration(),
			        "material readers acquire one coherent committed VT package snapshot" );
			uint32_t regionAllocations[T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY]{};
			uint32_t regionGenerations[T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY]{};
			uint8_t regionCooked[T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY]{};
			expect( vt.snapshotTerrainMegaVTReadStateCold( regionAllocations, regionGenerations, regionCooked,
			                T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY )
			        && regionCooked[7] != 0u && regionGenerations[7] == 8u
			        && regionAllocations[7] == vt.getTerrainMegaVTAllocationId( 7 ),
			        "MegaVT readers acquire coherent allocation and cooked-generation mappings" );
			expect( vt.get_residency().Buffers.pAllocations == allocations && vt.get_cpu_image_bytes() == bytes,
			        "warm publication reuses CPU image pointers and capacity" );
			expect( vt.is_hot_sealed() && vt.validate_hot_plateau_reference_debug(), "warm publication preserves sealed runtime plateau" );
			expect( shared.get_resources().get_stats().nGpuCreateCount == creates && shared.get_resources().get_stats().nGpuFreeCount == frees,
			        "warm publication creates and frees zero GPU resources" );
			expect( shared.get_shared_vt_bindings().nBindingGeneration == bindings.nBindingGeneration
			        && shared.get_shared_vt_bindings().nResourceGeneration == bindings.nResourceGeneration,
			        "compatible package retains binding and resource generations" );
			for ( uint32_t c = 0; c < T3D_VT_CHANNEL_COUNT; ++c ) {
				expect( shared.get_shared_vt_bindings().hTail[c] == bindings.hTail[c]
				        && shared.get_shared_vt_bindings().hPhysical[c][0] == bindings.hPhysical[c][0],
				        "warm publication preserves tail and physical bank handles" );
			}
			PackedByteArray damaged = second.duplicate(); damaged.set( 0, 0 );
			const uint64_t goodSignature = vt.getPackageContentSignature();
			expect( !shared.stagePackageWarm( damaged.ptr(), damaged.size() ) && vt.getPackageContentSignature() == goodSignature
			        && vt.getPackagePublicationGeneration() == 2, "invalid candidate retains previous GPU publication and generation" );
			expect( shared.stagePackageWarm( NULL, 0 ) && shared.servicePackageRepublishWarm( 4096, 1 )
			        && vt.getDatabaseKind() == T3D_VT_RUNTIME_DATABASE_LEGACY_NEUTRAL && vt.getPackagePublicationGeneration() == 3,
			        "neutral warm publication removes old package without resource restart" );
			expect( shared.stagePackageWarm( first.ptr(), first.size() ) && shared.servicePackageRepublishWarm( 4096, 1 )
			        && vt.getPackageContentSignature() == signature && vt.getPackagePublicationGeneration() == 4,
			        "neutral-to-package transition reuses existing storage" );
			const Dictionary diagnostics = vt.get_startup_diagnostics();
			expect( (int64_t)diagnostics["cpu_image_prepare_count"] == 1 && (int64_t)diagnostics["gpu_storage_prepare_count"] == 1
			        && (int64_t)diagnostics["initial_publication_count"] == 1, "warm edits leave every cold preparation counter at one" );
			expect( (int64_t)diagnostics["warm_publication_success_count"] == 3
			        && (int64_t)diagnostics["warm_publication_failure_count"] == 1, "warm diagnostics distinguish commits from rejected candidates" );
		}
		shared.shutdown_cold();
		expect(owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 0,
		        "capped VT runtime releases all physical resources at shutdown");
	}
	expect( zone->liveAllocations() == 0 && zone->release(), "publication and candidate storage release without zone leaks" );
	memdelete( zone );
	UtilityFunctions::print( "=== VT warm GPU tests: ", passed, " passed, ", failed, " failed ===" );
	return failed == 0;
}
