#include "diagnostics/tests/systems/test_harness.h"
#include "diagnostics/tests/systems/instance_fixture.h"
#include "memory/policy/terrain_3d_memory_account.h"
#include "memory/core/terrain_3d_memory.h"
#include "memory/arena/terrain_3d_linear_arena.h"
#include "core/runtime/terrain_3d_eastl_helpers.h"
#include "instances/contracts/terrain_3d_instance_cache_policy.h"
#include "terrain_3d.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_memory_domain.h"
#include <atomic>
#include <limits>
#include <thread>

namespace terrain3d_tests {
namespace {
struct TerrainFixture {
	Terrain3D *terrain = memnew(Terrain3D);
	~TerrainFixture() { memdelete(terrain); }
	void initialize() {
		Ref<Terrain3DMaterial> material;
		material.instantiate();
		terrain->setMaterial(material);
	}
};
}

void register_memory_budget_tests(TestRunner &runner) {
	// Resource caps are live constraints on the shared account, not independent
	// pools. Every reservation retains its original domain across moves/threads.
	runner.run("memory_budget", "resource_caps_and_total_ceiling_both_admit_allocations", [](TestContext &test) {
		Terrain3DMemoryAccount account(100);
		T3D_REQUIRE(test, account.setDomainLimit(Terrain3DMemoryAccount::VIRTUAL_TEXTURE, 40));
		T3D_REQUIRE(test, account.reserve(40, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::VIRTUAL_TEXTURE));
		T3D_CHECK(test, !account.reserve(1, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::VIRTUAL_TEXTURE));
		T3D_REQUIRE(test, account.reserve(60));
		T3D_CHECK(test, !account.reserve(1, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::INSTANCE_RENDER));
		const auto stats = account.stats();
		T3D_CHECK(test, stats.committed == 100 && stats.domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].denied == 1);
		T3D_CHECK(test, stats.domains[Terrain3DMemoryAccount::INSTANCE_RENDER].denied == 1);
	});
	runner.run("memory_budget", "live_resource_cap_reductions_are_transactional", [](TestContext &test) {
		Terrain3DMemoryAccount account(100);
		T3D_REQUIRE(test, account.setDomainLimit(Terrain3DMemoryAccount::INSTANCE_RENDER, 80));
		T3D_REQUIRE(test, account.reserve(60, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::INSTANCE_RENDER));
		T3D_CHECK(test, !account.setDomainLimit(Terrain3DMemoryAccount::INSTANCE_RENDER, 59));
		T3D_CHECK(test, account.stats().domains[Terrain3DMemoryAccount::INSTANCE_RENDER].limit == 80);
		T3D_REQUIRE(test, account.setDomainLimit(Terrain3DMemoryAccount::INSTANCE_RENDER, 60));
		T3D_CHECK(test, !account.reserve(1, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::INSTANCE_RENDER));
		T3D_REQUIRE(test, account.setDomainLimit(Terrain3DMemoryAccount::INSTANCE_RENDER, 0));
		T3D_CHECK(test, account.reserve(40, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::INSTANCE_RENDER));
		T3D_CHECK(test, !account.reserve(1, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::INSTANCE_RENDER));
	});
	runner.run("memory_budget", "reservation_growth_and_moves_preserve_the_original_domain", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(100);
		T3D_REQUIRE(test, owner.get()->setDomainLimit(Terrain3DMemoryAccount::VIRTUAL_TEXTURE, 50));
		Terrain3DMemoryReservation original;
		T3D_REQUIRE(test, original.resize(40, Terrain3DMemoryAccount::GPU, owner.get(), Terrain3DMemoryAccount::VIRTUAL_TEXTURE));
		Terrain3DMemoryReservation moved(std::move(original));
		T3D_CHECK(test, !moved.resize(51));
		T3D_REQUIRE(test, moved.resize(50));
		std::thread worker([&] { moved.reset(); }); worker.join();
		const auto stats = owner.get()->stats();
		T3D_CHECK(test, stats.committed == 0 && stats.domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].committed == 0);
		T3D_CHECK(test, stats.domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].peak == 50 && stats.kind_peaks[Terrain3DMemoryAccount::GPU] == 50);
	});
	runner.run("memory_budget", "concurrent_resource_allocations_cannot_cross_their_domain_cap", [](TestContext &test) {
		Terrain3DMemoryAccount account(1000);
		T3D_REQUIRE(test, account.setDomainLimit(Terrain3DMemoryAccount::VIRTUAL_TEXTURE, 100));
		std::atomic<int> successes{0};
		std::thread workers[8];
		for (auto &worker : workers) {
			worker = std::thread([&] { for (int i = 0; i < 100; ++i) { if (account.reserve(1, Terrain3DMemoryAccount::GPU, Terrain3DMemoryAccount::VIRTUAL_TEXTURE)) { ++successes; } } });
		}
		for (auto &worker : workers) { worker.join(); }
		T3D_CHECK(test, successes == 100 && account.stats().committed == 100);
		T3D_CHECK(test, account.stats().domains[Terrain3DMemoryAccount::VIRTUAL_TEXTURE].denied == 700);
	});
	runner.run("memory_budget", "invalid_resource_domains_cannot_corrupt_the_tracker", [](TestContext &test) {
		Terrain3DMemoryAccount account(100);
		const auto invalid = static_cast<Terrain3DMemoryAccount::Domain>(-1);
		T3D_CHECK(test, !account.reserve(1, Terrain3DMemoryAccount::CPU, invalid));
		T3D_CHECK(test, !account.setDomainLimit(invalid, 20));
		account.release(1, Terrain3DMemoryAccount::CPU, invalid);
		T3D_CHECK(test, account.stats().committed == 0);
	});
	runner.run("memory_budget", "world_instance_and_hlod_buffers_are_charged_to_their_consumer", [](TestContext &test) {
		T3D_CHECK(test, t3dRdMemoryDomain(T3D_RD_CONTEXT_OWNER_WORLD, T3D_RD_CONTEXT_MEMORY_INSTANCE) == Terrain3DMemoryAccount::INSTANCE_RENDER);
		T3D_CHECK(test, t3dRdMemoryDomain(T3D_RD_CONTEXT_OWNER_WORLD, T3D_RD_CONTEXT_MEMORY_HLOD) == Terrain3DMemoryAccount::HLOD_RENDER);
		T3D_CHECK(test, t3dRdMemoryDomain(T3D_RD_CONTEXT_OWNER_WORLD, T3D_RD_CONTEXT_MEMORY_MESH_VERTEX_ARENA) == Terrain3DMemoryAccount::WORLD_RENDER);
		T3D_CHECK(test, t3dRdMemoryDomain(T3D_RD_CONTEXT_OWNER_VT, T3D_RD_CONTEXT_MEMORY_TERRAIN_TEXTURE) == Terrain3DMemoryAccount::VIRTUAL_TEXTURE);
	});
	runner.run("memory_budget", "manual_cooking_limit_does_not_reintroduce_automatic_quarter_composition_cap", [](TestContext &test) {
		Terrain3DMemoryAccount account(256ull * 1024 * 1024);
		T3D_CHECK(test, t3dMemoryCookingLimit(&account) == 64ull * 1024 * 1024);
		T3D_CHECK(test, t3dMemoryCompositionLimit(&account) == 512ull * 1024 * 1024);
		account.setCookingLimit(100ull * 1024 * 1024);
		T3D_CHECK(test, t3dMemoryCookingLimit(&account) == 100ull * 1024 * 1024);
		T3D_CHECK(test, t3dMemoryCompositionLimit(&account) == 100ull * 1024 * 1024);
		account.setCookingLimit(512ull * 1024 * 1024);
		T3D_CHECK(test, t3dMemoryCookingLimit(&account) == account.stats().limit);
	});
	runner.run("memory_budget", "node_resource_caps_change_after_initialization_without_rebuilding_the_arena", [](TestContext &test) {
		TerrainFixture fixture; fixture.initialize();
		auto *terrain = fixture.terrain;
		const auto capacity = terrain->getRuntimeMemoryZoneInternal()->capacity();
		terrain->setVirtualTextureMemoryBudgetMb(16);
		terrain->setInstanceRenderMemoryBudgetMb(8);
		T3D_CHECK(test, terrain->getVirtualTextureMemoryBudgetMb() == 16 && terrain->getInstanceRenderMemoryBudgetMb() == 8);
		T3D_CHECK(test, terrain->getRuntimeMemoryZoneInternal()->capacity() == capacity);
		Terrain3DMemoryReservation instances;
		T3D_REQUIRE(test, instances.resize(2ull * 1024 * 1024, Terrain3DMemoryAccount::GPU, terrain->getMemoryAccountInternal(), Terrain3DMemoryAccount::INSTANCE_RENDER));
		terrain->setInstanceRenderMemoryBudgetMb(1);
		T3D_CHECK(test, terrain->getInstanceRenderMemoryBudgetMb() == 8);
		const Dictionary tracker = terrain->getMemoryTrackerStats();
		T3D_CHECK(test, !String(tracker["last_budget_change_error"]).is_empty());
		T3D_CHECK(test, int64_t(tracker["gpu_bytes"]) == 2ll * 1024 * 1024);
		T3D_CHECK(test, int64_t(tracker["current_bytes"]) == int64_t(terrain->getMemoryAccountInternal()->stats().committed));
		instances.reset(); terrain->setInstanceRenderMemoryBudgetMb(1);
		T3D_CHECK(test, terrain->getInstanceRenderMemoryBudgetMb() == 1);
		T3D_CHECK(test, String(terrain->getMemoryTrackerStats()["last_budget_change_error"]).is_empty());
	});
	// Admission is aggregate: CPU, GPU and external payloads share one ceiling.
	runner.run("memory_budget", "cpu_and_gpu_compete_for_the_same_limit", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(100);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DMemoryReservation cpu, gpu;
		T3D_REQUIRE(test, cpu.resize(60));
		T3D_REQUIRE(test, gpu.resize(40, Terrain3DMemoryAccount::GPU));
		T3D_CHECK(test, !gpu.resize(41));
		const auto stats = owner.get()->stats();
		T3D_CHECK(test, stats.committed == 100 && stats.peak == 100 && stats.denied == 1);
		T3D_CHECK(test, stats.bytes[Terrain3DMemoryAccount::CPU] == 60);
		T3D_CHECK(test, stats.bytes[Terrain3DMemoryAccount::GPU] == 40);
	});
	runner.run("memory_budget", "failed_growth_keeps_the_original_reservation", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(100);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DMemoryReservation memory;
		T3D_REQUIRE(test, memory.resize(70));
		T3D_CHECK(test, !memory.resize(101));
		T3D_CHECK(test, memory.bytes() == 70 && owner.get()->stats().committed == 70);
		T3D_REQUIRE(test, memory.resize(20));
		T3D_CHECK(test, owner.get()->stats().committed == 20);
		memory.reset();
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	runner.run("memory_budget", "limit_cannot_drop_below_live_commitments", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(100);
		Terrain3DMemoryReservation memory;
		T3D_REQUIRE(test, memory.resize(60, Terrain3DMemoryAccount::CPU, owner.get()));
		T3D_CHECK(test, !owner.get()->setLimit(59));
		T3D_CHECK(test, owner.get()->stats().limit == 100);
		T3D_REQUIRE(test, owner.get()->setLimit(60));
		T3D_CHECK(test, !memory.resize(61));
		T3D_REQUIRE(test, owner.get()->setLimit(200));
		T3D_CHECK(test, memory.resize(120));
	});
	runner.run("memory_budget", "zero_limit_and_integer_overflow_are_rejected", [](TestContext &test) {
		Terrain3DMemoryAccount account(0);
		T3D_CHECK(test, account.reserve(0));
		T3D_CHECK(test, !account.reserve(1));
		T3D_REQUIRE(test, account.setLimit(std::numeric_limits<uint64_t>::max()));
		T3D_REQUIRE(test, account.reserve(std::numeric_limits<uint64_t>::max()));
		T3D_CHECK(test, !account.reserve(1));
		T3D_CHECK(test, account.stats().committed == std::numeric_limits<uint64_t>::max());
		account.release(std::numeric_limits<uint64_t>::max());
		T3D_CHECK(test, account.stats().committed == 0);
	});
	runner.run("memory_budget", "invalid_kinds_cannot_corrupt_accounting", [](TestContext &test) {
		Terrain3DMemoryAccount account(100);
		T3D_CHECK(test, !account.reserve(1, static_cast<Terrain3DMemoryAccount::Kind>(-1)));
		T3D_CHECK(test, !account.reserve(1, Terrain3DMemoryAccount::KIND_COUNT));
		account.release(1, static_cast<Terrain3DMemoryAccount::Kind>(-1));
		T3D_CHECK(test, account.stats().committed == 0);
	});
	runner.run("memory_budget", "replacement_requires_room_for_both_live_resources", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(100);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DMemoryReservation current, replacement;
		T3D_REQUIRE(test, current.resize(60, Terrain3DMemoryAccount::GPU));
		T3D_CHECK(test, !replacement.resize(50, Terrain3DMemoryAccount::GPU));
		T3D_REQUIRE(test, replacement.resize(40, Terrain3DMemoryAccount::GPU));
		current.swap(replacement);
		T3D_CHECK(test, current.bytes() == 40 && replacement.bytes() == 60);
		replacement.reset();
		T3D_CHECK(test, owner.get()->stats().committed == 40 && owner.get()->stats().peak == 100);
	});
	runner.run("memory_budget", "aliases_share_one_payload_charge_until_last_release", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(100);
		Terrain3DMemoryAccountScope scope(owner.get());
		Terrain3DSharedMemoryReservation first;
		T3D_REQUIRE(test, first.reserve(60));
		auto second = first;
		T3D_CHECK(test, owner.get()->stats().committed == 60);
		first.reset();
		T3D_CHECK(test, second.bytes() == 60 && owner.get()->stats().committed == 60);
		T3D_CHECK(test, !second.reserve(101));
		T3D_CHECK(test, second.bytes() == 60);
		second.reset();
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	runner.run("memory_budget", "nested_owner_scopes_restore_the_original_account", [](TestContext &test) {
		Terrain3DMemoryAccountOwner first(100), second(100);
		Terrain3DMemoryAccountScope scope(first.get());
		Terrain3DMemoryReservation retained;
		{
			Terrain3DMemoryAccountScope nested(second.get());
			T3D_REQUIRE(test, retained.resize(40));
		}
		Terrain3DMemoryReservation local;
		T3D_REQUIRE(test, local.resize(50));
		T3D_CHECK(test, first.get()->stats().committed == 50 && second.get()->stats().committed == 40);
		T3D_REQUIRE(test, retained.resize(70));
		T3D_CHECK(test, second.get()->stats().committed == 70);
	});
	runner.run("memory_budget", "native_allocation_is_admitted_before_allocation_and_freed_cross_thread", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(256);
		void *memory = nullptr;
		{
			Terrain3DMemoryAccountScope scope(owner.get());
			memory = t3dMemoryAllocate(128);
			T3D_REQUIRE(test, memory != nullptr);
			T3D_CHECK(test, reinterpret_cast<uintptr_t>(memory) % alignof(max_align_t) == 0);
			T3D_CHECK(test, t3dMemoryAllocate(256) == nullptr);
		}
		T3D_CHECK(test, owner.get()->stats().committed == 128 + sizeof(t3dNativeMemoryHeader));
		std::thread releaser([memory]() { t3dMemoryRelease(memory); });
		releaser.join();
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	runner.run("memory_budget", "allocations_outlive_their_node_account_owner", [](TestContext &test) {
		void *memory = nullptr;
		Terrain3DMemoryAccount *account = nullptr;
		{
			Terrain3DMemoryAccountOwner owner(256);
			account = owner.get();
			Terrain3DMemoryAccountScope scope(account);
			memory = t3dMemoryAllocate(128);
			T3D_REQUIRE(test, memory != nullptr);
		}
		T3D_CHECK(test, account->stats().committed == 128 + sizeof(t3dNativeMemoryHeader));
		t3dMemoryRelease(memory);
	});
	runner.run("memory_budget", "concurrent_admissions_never_cross_the_shared_ceiling", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(1024);
		std::atomic<bool> start{false}, valid{true};
		std::thread workers[8];
		for (auto &worker : workers) {
			worker = std::thread([&]() {
				while (!start.load()) { std::this_thread::yield(); }
				Terrain3DMemoryAccountScope scope(owner.get());
				for (int iteration = 0; iteration < 1000; ++iteration) {
					Terrain3DMemoryReservation memory;
					(void)memory.resize(256, Terrain3DMemoryAccount::GPU);
					if (owner.get()->stats().committed > 1024) { valid.store(false); }
					std::this_thread::yield();
				}
			});
		}
		start.store(true);
		for (auto &worker : workers) { worker.join(); }
		T3D_CHECK(test, valid.load());
		T3D_CHECK(test, owner.get()->stats().committed == 0 && owner.get()->stats().peak <= 1024);
	});
	runner.run("memory_budget", "arena_backing_is_charged_once_and_released", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(8192);
		Terrain3DMemoryAccountScope scope(owner.get());
		{
			Terrain3DLinearArena arena;
			T3D_REQUIRE(test, arena.configure(4096));
			const auto committed = owner.get()->stats().committed;
			T3D_REQUIRE(test, arena.allocate(1024, 64) != nullptr);
			T3D_CHECK(test, owner.get()->stats().committed == committed && committed >= 4096);
		}
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	runner.run("memory_budget", "first_instance_page_and_demand_loads_must_fit", [](TestContext &test) {
		T3D_CHECK(test, !t3dInstanceCacheCanStream(0, 101, 100, 0));
		T3D_CHECK(test, !t3dInstanceCacheCanStream(0, 1, 0, 0));
		T3D_CHECK(test, t3dInstanceCacheCanStream(60, 40, 100, 1));
		T3D_CHECK(test, !t3dInstanceCacheCanStream(60, 41, 100, 1));
	});
	runner.run("memory_budget", "eastl_preflight_returns_failure_without_entering_an_unfunded_allocation", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(512);
		Terrain3DMemoryAccountScope scope(owner.get());
		eastl::vector<uint8_t, Terrain3DEastlAllocator> bytes;
		T3D_REQUIRE(test, t3dContainerReserveCold(bytes, 128));
		const auto committed = owner.get()->stats().committed;
		T3D_CHECK(test, !t3dContainerReserveCold(bytes, 512));
		T3D_CHECK(test, bytes.capacity() == 128 && owner.get()->stats().committed == committed);
		t3dVectorRelease(bytes);
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	runner.run("memory_budget", "prepaid_backing_consumes_credit_without_double_charging", [](TestContext &test) {
		Terrain3DMemoryAccountOwner owner(256);
		Terrain3DMemoryAccountScope scope(owner.get());
		void *memory = nullptr;
		{
			Terrain3DMemoryAllocationPermit permit(256);
			T3D_REQUIRE(test, permit.valid());
			memory = t3dMemoryAllocate(128);
			T3D_REQUIRE(test, memory != nullptr);
			T3D_CHECK(test, owner.get()->stats().committed == 256);
		}
		T3D_CHECK(test, owner.get()->stats().committed == 128 + sizeof(t3dNativeMemoryHeader));
		t3dMemoryRelease(memory);
		T3D_CHECK(test, owner.get()->stats().committed == 0);
	});
	// Exercise the real node setter after its arena exists, without a GPU or
	// SceneTree. Headless coverage verifies lifecycle ownership and authored data.
	runner.run("memory_budget", "editor_budget_changes_rebuild_an_existing_arena", [](TestContext &test) {
		TerrainFixture fixture;
		fixture.initialize();
		auto *terrain = fixture.terrain;
		T3D_REQUIRE(test, terrain->getRuntimeMemoryZoneInternal() != nullptr);
		const auto initial_capacity = terrain->getRuntimeMemoryZoneInternal()->capacity();
		terrain->setTotalMemoryBudgetMb(256);
		T3D_REQUIRE(test, terrain->getTotalMemoryBudgetMb() == 256);
		T3D_REQUIRE(test, terrain->getRuntimeMemoryZoneInternal() != nullptr);
		T3D_CHECK(test, terrain->getRuntimeMemoryZoneInternal()->capacity() < initial_capacity);
		terrain->setTotalMemoryBudgetMb(1024);
		T3D_REQUIRE(test, terrain->getRuntimeMemoryZoneInternal() != nullptr);
		T3D_CHECK(test, terrain->getTotalMemoryBudgetMb() == 1024);
		T3D_CHECK(test, terrain->getMemoryAccountInternal()->stats().limit == 1024ull * 1024ull * 1024ull);
		T3D_CHECK(test, terrain->getMemoryAccountInternal()->stats().committed <= terrain->getMemoryAccountInternal()->stats().limit);
	});
	runner.run("memory_budget", "budget_rebuild_preserves_the_authored_repository", [](TestContext &test) {
		TerrainFixture fixture;
		fixture.initialize();
		auto *terrain = fixture.terrain;
		auto *data = terrain->dataObject();
		const auto region = data->addRegionBlank(Vector2i(), false);
		T3D_REQUIRE(test, region.is_valid());
		const auto painted = region->materializeMap(TYPE_COLOR);
		T3D_REQUIRE(test, painted.is_valid());
		painted->set_pixel(7, 11, Color(0.2, 0.4, 0.7, 0.3));
		const Color original_pixel = painted->get_pixel(7, 11);
		Terrain3DInstanceStore *instances = nullptr;
		T3D_REQUIRE(test, region->tryGetInstanceStoreMut(instances) && instances);
		T3D_REQUIRE(test, populate_instance_store(*instances));
		region->markInstanceStoreModifiedInternal();
		Terrain3DInstanceStore expected(*instances);
		T3D_REQUIRE(test, same_instance_store(*instances, expected));
		region->setModified(true);
		terrain->setTotalMemoryBudgetMb(256);
		T3D_CHECK(test, terrain->dataObject() == data);
		T3D_CHECK(test, data->getRegion(Vector2i()) == region);
		T3D_CHECK(test, region->isModified());
		T3D_CHECK(test, region->getColorMap()->get_pixel(7, 11) == original_pixel);
		T3D_REQUIRE(test, region->tryGetInstanceStoreMut(instances) && instances);
		T3D_CHECK(test, same_instance_store(*instances, expected));
	});
}
} // namespace terrain3d_tests
