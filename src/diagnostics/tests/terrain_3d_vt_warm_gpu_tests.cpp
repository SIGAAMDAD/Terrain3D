// Real RenderingDevice publication checks without terrain shaders or scene setup.
#include "diagnostics/tests/unit_testing.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "rendering/rd/vt/runtime/terrain_3d_shared_vt_runtime.h"
#include "rendering/rd/world/resources/terrain_3d_world_resources.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_shader.h"
#include "rendering/rd/infrastructure/pipeline/terrain_3d_rd_pipeline.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace
{
bool makePackage(
	uint8_t			 color,
	uint32_t		 generation,
	PackedByteArray &package ) {
	PackedByteArray albedo, pbr;
	albedo.resize( 256 * 256 * 4 );
	pbr.resize( 128 * 128 * 4 );
	albedo.fill( color );
	pbr.fill( 128 );
	t3dVTCookSparseSourceV3_t source{};
	source.m_nStableKey = t3d_vt_terrain_region_stable_key( 7 );
	source.m_nGenerationSeed = generation;
	source.m_nConsumer = T3D_VT_CONSUMER_CLIPMAP;
	source.m_nWrapMode = T3D_VT_COOK_WRAP_CLAMP;
	source.m_nAllocationFlags =
		T3D_VT_ALLOCATION_MEGATEXTURE | T3D_VT_ALLOCATION_CLAMP | T3D_VT_ALLOCATION_SHIPPING_IMMUTABLE;
	source.m_nAuthoredChannelMask = T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_ALBEDO ) |
									T3D_VT_LOGICAL_CHANNEL_BIT( T3D_VT_LOGICAL_CHANNEL_PBR );
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = { albedo.ptr(), 256, 256, 4, 1024 };
	source.m_Channels[T3D_VT_LOGICAL_CHANNEL_PBR] = { pbr.ptr(), 128, 128, 4, 512 };
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_ALBEDO] = t3dHashPublicationBytes( albedo.ptr(), albedo.size() );
	source.m_nSourceFingerprint[T3D_VT_LOGICAL_CHANNEL_PBR] = t3dHashPublicationBytes( pbr.ptr(), pbr.size() );
	return t3d_vt_godot_cook_source_fragment_v3( source, package, 32ull * 1024 * 1024 );
}
}

bool t3dRunVTWarmGpuTests() {
	uint32_t   passed = 0, failed = 0;
	const auto expect = [&]( bool value, const char *name ) {
		value ? ++passed : ++failed;
		UtilityFunctions::print( value ? "PASSED: " : "FAILED: ", name );
	};
	Terrain3DRdContext &context = Terrain3DRdContext::getShared();
	const bool			deviceReady = Terrain3DRdContext::isOnRenderThread() && context.initialize();
	expect( deviceReady, "warm GPU checks have a real device and render-thread ownership" );
	if ( !deviceReady ) {
		return false;
	}
	// Real allocation rejection precedes the device call and preserves resident
	// texture ownership. Shared views never consume a second physical reservation.
	{
		Terrain3DMemoryAccountOwner owner( 16ull * 1024 * 1024 );
		Terrain3DMemoryAccountScope scope( owner.get() );
		expect( owner.get()->setDomainLimit( Terrain3DMemoryAccount::VIRTUAL_TEXTURE, 64 ),
				"VT GPU test installs its live resource cap" );
		Terrain3DRdTexture texture, rejected;
		t3dRdTextureSpec_t spec;
		spec.m_name = "BudgetTest.Texture";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_VT;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
		spec.m_layout.m_nWidth = 4;
		spec.m_layout.m_nHeight = 4;
		spec.m_layout.m_nLayers = 1;
		spec.m_layout.m_nMipmaps = 1;
		spec.m_layout.m_nBytes = 64;
		spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		spec.m_layout.m_nUsageBits =
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
		expect( texture.createTracked( context, spec ), "actual texture creation fits an exact VT GPU cap" );
		spec.m_name = "BudgetTest.Rejected";
		const RID resident = texture.rid();
		expect( !rejected.createTracked( context, spec ) && texture.rid() == resident && texture.isValid(),
				"VT cap rejects a second texture without changing the resident texture" );
		expect( owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 64 &&
					owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].denied == 1,
				"GPU domain tracker records physical capacity and rejected allocations" );
		texture.releaseTracked( context );
		expect( owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 0 &&
					owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].peak == 64,
				"texture retirement releases its original domain and keeps its allocation-time peak" );
	}
	// Exercise actual RD calls: creation payloads are traffic, shared views and
	// rejected updates are not. No terrain shader compilation is required.
	{
		Terrain3DMemoryAccountOwner owner( 16ull * 1024 * 1024 );
		Terrain3DMemoryAccountScope scope( owner.get() );
		owner.get()->setDomainLimit( Terrain3DMemoryAccount::INSTANCE_RENDER, 64 );
		Terrain3DRdGpuBuffer buffer;
		t3dRdGpuBufferSpec_t spec;
		spec.m_pName = "BudgetTest.WorldInstances";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_WORLD;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_INSTANCE;
		spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
		expect( buffer.configure( spec ) && buffer.createTracked( context, 64 ),
				"world instance storage fits its instance GPU cap" );
		const RID resident = buffer.rid();
		expect( !buffer.replaceTracked( context, 64 ) && buffer.rid() == resident,
				"buffer replacement must admit both old and new capacity under the instance cap" );
		expect( owner.get()->stats().domains[Terrain3DMemoryAccount::INSTANCE_RENDER].committed == 64 &&
					owner.get()->stats().domains[Terrain3DMemoryAccount::WORLD_RENDER].committed == 0,
				"unified-world instance buffers count under the instance resource domain" );
		buffer.releaseTracked( context );
	}
	context.beginFrame( 1001, 1 );
	{
		Terrain3DRdTexture texture;
		t3dRdTextureSpec_t spec;
		spec.m_name = "UploadTest.Texture";
		spec.m_owner = T3D_RD_CONTEXT_OWNER_VT;
		spec.m_memory = T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE;
		spec.m_layout.m_nWidth = 4;
		spec.m_layout.m_nHeight = 4;
		spec.m_layout.m_nLayers = 1;
		spec.m_layout.m_nMipmaps = 1;
		spec.m_layout.m_nBytes = 64;
		spec.m_layout.m_format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		spec.m_layout.m_nUsageBits =
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
		PackedByteArray pixels;
		pixels.resize( 64 );
		pixels.fill( 90 );
		TypedArray< PackedByteArray > initial;
		initial.push_back( pixels );
		const uint64_t before = context.getRenderUploadBytes( T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_PUBLICATION );
		expect( texture.createTracked( context, spec, initial ), "texture creation accepts the initial payload" );
		expect( context.getRenderUploadBytes( T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_PUBLICATION ) == before + 64,
				"texture creation counts supplied bytes once" );
		const uint64_t dirty = context.getRenderUploadBytes( T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_DIRTY_UPLOAD );
		expect( texture.updateLayerTracked( context, 0, pixels ) && !texture.updateLayerTracked( context, 1, pixels ),
				"texture updates reject invalid layers before submitting" );
		expect( context.getRenderUploadBytes( T3D_RD_CONTEXT_OWNER_VT, T3D_GPU_DIRTY_UPLOAD ) == dirty + 64,
				"only the successful texture update contributes traffic" );
		texture.releaseTracked( context );
	}
	{
		Terrain3DWorldPublication world;
		Terrain3DWorldResources	  resources;
		expect( t3dVectorResizeCold( world.m_vertices, 32 ) && t3dVectorResizeCold( world.m_materials, 1 ),
				"world upload fixture admits its publication storage" );
		world.m_nGeneration = 1;
		t3dWorldResourceLimits_t limits{};
		limits.m_nMaxSourceVertices = 32u;
		limits.m_nMaxMaterials = 1u;
		limits.m_nMaxClusters = 1u;
		limits.m_nMaxTerrainClusters = 1u;
		limits.m_nMaxTransforms = 1u;
		limits.m_nMaxVisibleItems = 1u;
		expect( resources.ensureCapacityCold( world, limits ) && resources.uploadPublicationWarm( world ),
				"world uploads its initial tables" );
		const uint64_t first = resources.getUploadBytes();
		const uint64_t refresh = T3D_WORLD_VIS_COUNTER_COUNT * sizeof( uint32_t ) +
								 T3D_WORLD_INDIRECT_COMMAND_COUNT * sizeof( t3dWorldDrawIndirectArgs_t );
		world.m_nGeneration = 2;
		expect( resources.uploadPublicationWarm( world ) && resources.getUploadBytes() - first == refresh,
				"new publication identity preserves unchanged geometry and material buffers" );
		const uint64_t second = resources.getUploadBytes();
		reinterpret_cast< uint8_t * >( world.m_vertices.data() )[0] ^= 1;
		world.m_nGeneration = 3;
		expect( resources.uploadPublicationWarm( world ) &&
					resources.getUploadBytes() - second ==
						refresh + world.m_vertices.size() * sizeof( t3dWorldVertex_t ),
				"changed vertex data uploads its table while unchanged material data stays resident" );
		const uint64_t skipped = resources.getSkippedPublicationUploadBytes();
		resources.shutdownCold();
		const uint64_t third = resources.getUploadBytes();
		expect( resources.ensureCapacityCold( world, limits ) && resources.uploadPublicationWarm( world ) &&
					resources.getUploadBytes() - third == first &&
					resources.getSkippedPublicationUploadBytes() == skipped,
				"recreated GPU storage receives all tables even when CPU bytes are identical" );
		UtilityFunctions::print( "GPU_UPLOAD_FIXTURE initial=",
								 first,
								 " unchanged_publication=",
								 refresh,
								 " changed_vertices=",
								 resources.getUploadBytes() - second - first,
								 " skipped_bytes=",
								 skipped );
		resources.shutdownCold();
	}
	PackedByteArray first, second, backing;
	expect( makePackage( 170, 7, first ) && makePackage( 90, 8, second ),
			"warm GPU fixtures use production-cooked packages" );
	backing.resize( 32 * 1024 * 1024 );
	Terrain3DMemoryZone *zone = memnew( Terrain3DMemoryZone );
	expect( zone->initialize( backing.ptrw(), backing.size() ), "warm GPU fixture binds one retained runtime zone" );
	{
		Terrain3DSharedVTRuntime shared;
		Terrain3DVirtualTexture &vt = shared.virtualTexture();
		auto					 publishedBindings = [&shared]() {
			t3dSharedVTBindings_t bindings{};
			( void ) shared.acquirePublishedBindingsHot( T3D_VT_SHARED_RESOURCES_PREVIEW, bindings );
			return bindings;
		};
		Terrain3DMemoryAccountOwner owner( 64ull * 1024 * 1024 );
		Terrain3DMemoryAccountScope scope( owner.get() );
		const bool					source_ready =
			shared.bindRuntimeAllocator( zone ) && vt.loadPackageCold( first.ptr(), first.size() );
		auto capped_limits = t3d_world_make_preview_limits( 8, 256 );
		capped_limits.nMaxVtPhysicalPageSets = 3;
		t3dVTGpuResourcePlan_t capped_plan{};
		const bool			   plan_ready =
			source_ready && t3d_vt_compute_resource_plan( capped_limits,
														  t3d_vt_make_preview_gpu_profile(),
														  capped_plan,
														  vt.getLoadedPackageTailAtlasHeightCold() );
		expect( plan_ready &&
					owner.get()->setDomainLimit( Terrain3DMemoryAccount::VIRTUAL_TEXTURE, capped_plan.nTotalBytes ),
				"real VT resource fixture installs a cap smaller than its default physical cache" );
		const bool initialized =
			source_ready && shared.ensure_shared_resources_cold( context, T3D_VT_SHARED_RESOURCES_PREVIEW, 8, 256 );
		expect( initialized, "first package completes one cold VT initialization" );
		if ( initialized ) {
			expect( shared.get_resources().get_plan().nTotalBytes <= capped_plan.nTotalBytes &&
						vt.get_residency().Buffers.nPhysicalPageSetCapacity == 3,
					"real VT initialization fits its manual GPU cap and preserves the complete database" );
			vt.seal_hot_runtime();
			const t3dSharedVTBindings_t bindings = publishedBindings();
			const uint64_t				creates = shared.get_resources().get_stats().nGpuCreateCount;
			const uint64_t				frees = shared.get_resources().get_stats().nGpuFreeCount;
			const uint64_t				bytes = vt.get_cpu_image_bytes();
			const void				   *allocations = vt.get_residency().Buffers.pAllocations;
			const uint64_t				signature = vt.getPackageContentSignature();
			expect( vt.getPackagePublicationGeneration() == 1, "initial publication has its own generation" );
			expect( shared.stagePackageWarm( second.ptr(), second.size() ),
					"compatible package stages independently of live ownership" );
			expect( vt.getPackageContentSignature() == signature, "staging keeps last-good database visible" );
			shared.abortPackageRepublishWarm();
			expect( !vt.isPackageRepublishPendingWarm() && vt.getPackageContentSignature() == signature,
					"aborted candidate leaves last-good package intact" );
			const size_t free_extent = zone->freeExtentBytes();
			void		*pressure = free_extent > 4096u ? zone->allocate( free_extent - 4096u, 64u ) : NULL;
			expect( pressure != NULL, "warm fixture can reproduce insufficient candidate coexistence capacity" );
			if ( pressure != NULL ) {
				expect( !shared.stagePackageWarm( second.ptr(), second.size() ) &&
							vt.isPackageResourceRebuildRequiredWarm(),
						"valid candidate storage denial requests controlled rebuild rather than permanent rejection" );
				expect( vt.getPackageContentSignature() == signature && vt.isPackageRollbackReadyWarm(),
						"storage-denied candidate keeps the live package and rollback available" );
				zone->deallocate( pressure );
			}
			expect( shared.stagePackageWarm( second.ptr(), second.size() ) &&
						shared.servicePackageRepublishWarm( 4096, 1 ) && !vt.isPackageRepublishPendingWarm(),
					"small upload slices commit one complete warm publication" );
			expect( vt.getPackageContentSignature() != signature && vt.getPackagePublicationGeneration() == 2,
					"warm commit changes content and advances publication generation" );
			uint32_t cookedGeneration = 0;
			expect( vt.getTerrainMegaVTCookedGenerationCold( 7, cookedGeneration ) && cookedGeneration == 8,
					"committed package retains coherent region identity and source generation" );
			t3dVTMaterialReadSnapshot_t materialSnapshot{};
			expect( vt.snapshotMaterialReadStateCold( materialSnapshot ) &&
						materialSnapshot.nDatabaseGeneration == vt.getDatabaseGeneration() &&
						materialSnapshot.nPackageContentSignature == vt.getPackageContentSignature() &&
						materialSnapshot.nPackagePublicationGeneration == vt.getPackagePublicationGeneration(),
					"material readers acquire one coherent committed VT package snapshot" );
			uint32_t regionAllocations[T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY]{};
			uint32_t regionGenerations[T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY]{};
			uint8_t	 regionCooked[T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY]{};
			expect( vt.snapshotTerrainMegaVTReadStateCold( regionAllocations,
														   regionGenerations,
														   regionCooked,
														   T3D_TERRAIN_MEGAVT_ALLOCATION_CAPACITY ) &&
						regionCooked[7] != 0u && regionGenerations[7] == 8u &&
						regionAllocations[7] == vt.getTerrainMegaVTAllocationId( 7 ),
					"MegaVT readers acquire coherent allocation and cooked-generation mappings" );
			expect( vt.get_residency().Buffers.pAllocations == allocations && vt.get_cpu_image_bytes() == bytes,
					"warm publication reuses CPU image pointers and capacity" );
			expect( vt.is_hot_sealed() && vt.validate_hot_plateau_reference_debug(),
					"warm publication preserves sealed runtime plateau" );
			expect( shared.get_resources().get_stats().nGpuCreateCount == creates &&
						shared.get_resources().get_stats().nGpuFreeCount == frees,
					"warm publication creates and frees zero GPU resources" );
			expect( publishedBindings().nBindingGeneration == bindings.nBindingGeneration &&
						publishedBindings().nResourceGeneration == bindings.nResourceGeneration,
					"compatible package retains binding and resource generations" );
			for ( uint32_t c = 0; c < T3D_VT_CHANNEL_COUNT; ++c ) {
				expect( publishedBindings().hTail[c] == bindings.hTail[c] &&
							publishedBindings().hPhysical[c][0] == bindings.hPhysical[c][0],
						"warm publication preserves tail and physical bank handles" );
			}
			PackedByteArray damaged = second.duplicate();
			damaged.set( 0, 0 );
			const uint64_t goodSignature = vt.getPackageContentSignature();
			expect( !shared.stagePackageWarm( damaged.ptr(), damaged.size() ) &&
						vt.getPackageContentSignature() == goodSignature && vt.getPackagePublicationGeneration() == 2,
					"invalid candidate retains previous GPU publication and generation" );
			expect( !vt.isPackageResourceRebuildRequiredWarm(),
					"invalid package cannot trigger a destructive capacity rebuild" );
			expect( shared.stagePackageWarm( NULL, 0 ) && shared.servicePackageRepublishWarm( 4096, 1 ) &&
						vt.getDatabaseKind() == T3D_VT_RUNTIME_DATABASE_LEGACY_NEUTRAL &&
						vt.getPackagePublicationGeneration() == 3,
					"neutral warm publication removes old package without resource restart" );
			expect( shared.stagePackageWarm( first.ptr(), first.size() ) &&
						shared.servicePackageRepublishWarm( 4096, 1 ) && vt.getPackageContentSignature() == signature &&
						vt.getPackagePublicationGeneration() == 4,
					"neutral-to-package transition reuses existing storage" );
			const Dictionary diagnostics = vt.get_startup_diagnostics();
			expect( ( int64_t ) diagnostics["cpu_image_prepare_count"] == 1 &&
						( int64_t ) diagnostics["gpu_storage_prepare_count"] == 1 &&
						( int64_t ) diagnostics["initial_publication_count"] == 1,
					"warm edits leave every cold preparation counter at one" );
			expect( ( int64_t ) diagnostics["warm_publication_success_count"] == 3 &&
						( int64_t ) diagnostics["warm_publication_failure_count"] == 2,
					"warm diagnostics distinguish commits from rejected candidates" );

			// Keep a real consumer shader/pipeline/buffer alive while replacing VT.
			// This exercises descriptor retirement/rebinding without scene startup.
			RenderingDevice *const rd = context.getDevice();
			Ref< RDShaderSource >  source;
			source.instantiate();
			source->set_stage_source( RenderingDevice::SHADER_STAGE_COMPUTE,
									  "#version 450\nlayout(local_size_x=1) in;\n"
									  "layout(set=0,binding=0,std430) readonly buffer VT { uint words[]; } vt;\n"
									  "layout(set=0,binding=1,std430) buffer Output { uint value; } result;\n"
									  "void main() { result.value = vt.words[0]; }\n" );
			Terrain3DRdShader	 shader;
			Terrain3DRdPipeline	 pipeline;
			Terrain3DRdGpuBuffer output;
			t3dRdGpuBufferSpec_t spec;
			spec.m_pName = "AuthoringReuse.ConsumerOutput";
			spec.m_owner = T3D_RD_CONTEXT_OWNER_WORLD;
			spec.m_type = T3D_RD_CONTEXT_RESOURCE_STORAGE_BUFFER;
			spec.m_memory = T3D_RD_CONTEXT_MEMORY_STORAGE;
			const bool consumerReady = shader.createTracked( context,
															 T3D_RD_CONTEXT_OWNER_WORLD,
															 "AuthoringReuse.ConsumerShader",
															 rd->shader_compile_spirv_from_source( source ) ) &&
									   pipeline.createTrackedCompute( context,
																	  T3D_RD_CONTEXT_OWNER_WORLD,
																	  "AuthoringReuse.ConsumerPipeline",
																	  shader.rid() ) &&
									   output.configure( spec ) && output.createTracked( context, 4 );
			expect( consumerReady, "VT capacity fixture retains a real consumer pipeline and output buffer" );
			if ( consumerReady ) {
				Terrain3DRdGlobalDescriptorCache *const cache = context.tryGetGlobalDescriptorCacheHot();
				if ( cache ) {
					const uint64_t safeRetires = cache->diagnostics().m_nGpuSafeRetirements;
					const uint64_t staleCallbacks = cache->diagnostics().m_nFenceStaleCallbacks;
					expect( cache->serviceRetirementsCold( cache->currentSubmissionEpochHot() ) == 0u &&
						cache->diagnostics().m_nGpuSafeRetirements == safeRetires,
						"current submission epoch cannot be mistaken for completed GPU work" );
					cache->consumeCompletionReadbackOnRenderThread( PackedByteArray(),
						cache->currentSubmissionEpochHot(), context.getDeviceGeneration(), UINT64_MAX );
					expect( cache->diagnostics().m_nFenceStaleCallbacks == staleCallbacks + 1u &&
						cache->diagnostics().m_nGpuSafeRetirements == safeRetires,
						"unmatched completion callback cannot retire descriptors" );
				}
				t3dRdDescriptorLease_t					descriptor{};
				RID										descriptorRid;
				const auto								bindConsumer = [&]() {
					 if ( !cache ) {
						 return false;
					 }
					 t3dRdDescriptorRecipe_t		  recipe{};
					 const t3dRdDescriptorDeviceKey_t device{ T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID,
															  context.getDeviceGeneration(),
															  T3D_RD_DESCRIPTOR_DEVICE_GLOBAL };
					 const RID						  allocation = publishedBindings().hAllocationTable;
					 const RID						  buffer = output.rid();
					 if ( t3dRdDescriptorRecipeBeginCold( recipe, device, shader.rid(), 0u ) != T3D_RD_DESCRIPTOR_OK ||
						  t3dRdDescriptorRecipeAddBindingCold( recipe,
															   0u,
															   RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
															   &allocation,
															   1u ) != T3D_RD_DESCRIPTOR_OK ||
						  t3dRdDescriptorRecipeAddBindingCold( recipe,
															   1u,
															   RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER,
															   &buffer,
															   1u ) != T3D_RD_DESCRIPTOR_OK ) {
						 return false;
					 }
					 if ( cache->acquireDescriptorCold( recipe, T3D_RD_CONTEXT_OWNER_WORLD, descriptor ) !=
						  T3D_RD_DESCRIPTOR_OK ) {
						 return false;
					 }
					 if ( cache->resolveDescriptorHot( descriptor, cache->currentSubmissionEpochHot(), descriptorRid ) != T3D_RD_DESCRIPTOR_OK ) {
						 cache->releaseDescriptorCold( descriptor );
						 return false;
					 }
					 return true;
				};
				const RID shaderRid = shader.rid(), pipelineRid = pipeline.rid(), outputRid = output.rid();
				expect( bindConsumer(), "consumer lease initially binds the published VT allocation table" );
				const auto oldBindings = publishedBindings();
				expect( shared.replacePackageResourcesCold( context,
															second.ptr(),
															second.size(),
															false,
															T3D_VT_SHARED_RESOURCES_PREVIEW ),
						"capacity fallback publishes replacement by recreating only VT ownership" );
				expect( publishedBindings().nBindingGeneration > oldBindings.nBindingGeneration &&
							publishedBindings().nResourceGeneration > oldBindings.nResourceGeneration,
						"capacity replacement advances both generations used by retained consumers" );
				expect( vt.getTerrainMegaVTCookedGenerationCold( 7, cookedGeneration ) && cookedGeneration == 8u &&
							vt.getPackageContentSignature() != signature,
						"capacity replacement exposes the new database to retained material readers" );
				RID staleResolution;
				expect( cache && cache->resolveDescriptorHot( descriptor, cache->currentSubmissionEpochHot(), staleResolution ) != T3D_RD_DESCRIPTOR_OK,
						"VT replacement rejects the stale descriptor lease before new GPU submission" );
				if ( cache ) {
					cache->releaseDescriptorCold( descriptor );
				}
				expect( bindConsumer() && rd->uniform_set_is_valid( descriptorRid ),
						"retained shader acquires a descriptor for replacement VT tables" );
				expect( shader.rid() == shaderRid && pipeline.rid() == pipelineRid && output.rid() == outputRid &&
							rd->compute_pipeline_is_valid( pipelineRid ),
						"capacity replacement preserves consumer shader, pipeline, and output storage" );
				if ( descriptorRid.is_valid() && rd->compute_pipeline_is_valid( pipelineRid ) ) {
					const int64_t list = rd->compute_list_begin();
					rd->compute_list_bind_compute_pipeline( list, pipelineRid );
					rd->compute_list_bind_uniform_set( list, descriptorRid, 0 );
					rd->compute_list_dispatch( list, 1, 1, 1 );
					rd->compute_list_end();
				}
				expect( vt.is_hot_sealed() && vt.validate_hot_plateau_reference_debug(),
						"VT-only replacement establishes a new sealed runtime plateau" );
				const uint64_t generation = publishedBindings().nBindingGeneration;
				expect( shared.replacePackageResourcesCold( context,
															first.ptr(),
															first.size(),
															false,
															T3D_VT_SHARED_RESOURCES_PREVIEW ) &&
							publishedBindings().nBindingGeneration > generation &&
							vt.getPackageContentSignature() == signature,
						"last-good rollback also refreshes retained consumer generations" );
				if ( cache ) {
					cache->releaseDescriptorCold( descriptor );
				}
			}

			output.releaseTracked( context );
			pipeline.releaseTracked( context );
			shader.releaseTracked( context );
		}
		shared.shutdown_cold();
		expect( owner.get()->stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 0,
				"capped VT runtime releases all physical resources at shutdown" );
	}
	expect( zone->liveAllocations() == 0 && zone->release(),
			"publication and candidate storage release without zone leaks" );
	memdelete( zone );
	UtilityFunctions::print( "=== VT warm GPU tests: ", passed, " passed, ", failed, " failed ===" );
	return failed == 0;
}
