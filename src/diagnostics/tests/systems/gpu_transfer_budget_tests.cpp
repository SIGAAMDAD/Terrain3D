// P08 standalone fixed-budget admission tests; compile with -DT3D_GPU_BUDGET_TEST_MAIN.
#include "rendering/rd/core/terrain_3d_gpu_transfer_budget.h"
#include <assert.h>
#include <thread>
#include <atomic>
#include <type_traits>
#include <utility>

#ifdef T3D_GPU_BUDGET_TEST_MAIN
int main() {
	static_assert( !std::is_copy_constructible< t3dGpuTransferTicket_t >::value, "No duplicate tickets" );
	static_assert( std::is_move_constructible< t3dGpuTransferTicket_t >::value, "Ticket ownership is movable" );
	Terrain3DGpuTransferBudget &budget = Terrain3DGpuTransferBudget::get();
	t3dGpuTransferTicket_t		ticket;
	assert( !budget.reserve( T3D_GPU_TRANSFER_VT, Terrain3DGpuTransferBudget::BYTE_LIMIT + 1u, 1u, ticket ) );
	assert( !ticket.granted() );
	t3dGpuTransferTicket_t first;
	assert( budget.reserve( T3D_GPU_TRANSFER_VT, 1024u, 2u, first ) );
	assert( !budget.reserve( T3D_GPU_TRANSFER_REGION, Terrain3DGpuTransferBudget::BYTE_LIMIT, 1u, ticket ) );
	budget.refund( first, 1024u, 2u );
	assert( !first.granted() );
	const auto afterRefund = budget.snapshot();
	budget.refund( first, 1024u, 2u );
	assert( budget.snapshot().nBytes == afterRefund.nBytes );
	assert( budget.reserve( T3D_GPU_TRANSFER_WORLD, Terrain3DGpuTransferBudget::BYTE_LIMIT, 1u, ticket ) );
	t3dGpuTransferTicket_t moved( std::move( ticket ) );
	assert( !ticket.granted() && moved.granted() );
	budget.refund( moved, moved.bytes(), moved.calls() );
	assert( !moved.granted() );
	std::atomic< uint32_t > admitted{ 0u };
	std::thread				threads[8];
	for ( uint32_t n = 0u; n < 8u; ++n ) {
		threads[n] = std::thread( [&]() {
			for ( uint32_t i = 0u; i < 256u; ++i ) {
				t3dGpuTransferTicket_t local;
				if ( budget.reserve( T3D_GPU_TRANSFER_FEEDBACK, 4096u, 1u, local ) ) {
					++admitted;
				}
			}
		} );
	}
	for ( auto &thread : threads ) {
		thread.join();
	}
	const t3dGpuTransferSnapshot_t stats = budget.snapshot();
	assert( stats.nBytes <= Terrain3DGpuTransferBudget::BYTE_LIMIT );
	assert( stats.nCalls <= Terrain3DGpuTransferBudget::CALL_LIMIT );
	assert( stats.nLaneCalls[T3D_GPU_TRANSFER_FEEDBACK] <= Terrain3DGpuTransferBudget::CALL_LIMIT );
	assert( admitted.load() > 0u );
	// Simulates a delayed producer holding an older time sample.
	const uint64_t n = stats.nWindow + 1u;
	assert( budget.snapshotForWindowForTest( n ).nWindow == n );
	assert( budget.snapshotForWindowForTest( n + 1u ).nWindow == n + 1u );
	assert( budget.snapshotForWindowForTest( n ).nWindow == n + 1u );
	return 0;
}
#endif
