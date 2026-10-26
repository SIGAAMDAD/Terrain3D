#include "memory/policy/terrain_3d_memory_budget.h"

t3dMemoryBudgetResult_t Terrain3DMemoryBudgetCoordinator::calculate( const t3dMemoryBudgetInput_t &rInput ) {
		t3dMemoryBudgetResult_t result;
		result.m_nTotalBudgetBytes = rInput.m_nTotalBudgetBytes;
		// Fixed logical capacity physically served by the reserved zone is already
		// paid for by m_nAllocatorReserveBytes. Charge only any excess once.
		const uint64_t allocator_contained = rInput.m_nAllocatorContainedFixedBytes < rInput.m_nFixedBytes
		        ? rInput.m_nAllocatorContainedFixedBytes
		        : rInput.m_nFixedBytes;
		result.m_nAllocatorContainedFixedBytes =
		        allocator_contained < rInput.m_nAllocatorReserveBytes ? allocator_contained : rInput.m_nAllocatorReserveBytes;
		result.m_nFixedBytes = rInput.m_nFixedBytes - result.m_nAllocatorContainedFixedBytes;
		result.m_nAllocatorReserveBytes = rInput.m_nAllocatorReserveBytes;
		result.m_nTransientReserveBytes = rInput.m_nTransientReserveBytes;
		bool arithmetic_overflow = false;
		const uint64_t fixed_and_allocator = addChecked( result.m_nFixedBytes, result.m_nAllocatorReserveBytes, arithmetic_overflow );
		const uint64_t fixed_and_transient = addChecked( fixed_and_allocator, rInput.m_nTransientReserveBytes, arithmetic_overflow );
		result.m_nManagedBudgetBytes =
		        rInput.m_nTotalBudgetBytes > fixed_and_transient ? rInput.m_nTotalBudgetBytes - fixed_and_transient : 0u;
		if ( arithmetic_overflow || fixed_and_transient > rInput.m_nTotalBudgetBytes ) {
			result.m_bHardOvercommit = true;
		}

		uint64_t remaining = result.m_nManagedBudgetBytes;
		for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
			const t3dMemorySubsystemInput_t &input = rInput.m_szSubsystems[( size_t )( index )];
			t3dMemorySubsystemAllocation_t &allocation = result.m_szSubsystems[( size_t )( index )];
			if ( !input.m_bEnabled ) {
				allocation.m_limitingReason = TERRAIN3D_MEMORY_LIMIT_DISABLED;
				continue;
			}
			if ( !input.m_bAutomatic ) {
				allocation.m_nRequestedBytes = input.m_nManualBytes;
				const uint64_t capped_manual = clampToCap( input.m_nManualBytes, input.m_nHardCapBytes );
				allocation.m_nEffectiveBytes = capped_manual < remaining ? capped_manual : remaining;
				allocation.m_limitingReason = allocation.m_nEffectiveBytes < capped_manual
				        ? TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET
				        : ( capped_manual < input.m_nManualBytes ? TERRAIN3D_MEMORY_LIMIT_FIXED_CAP
				                                                 : TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE );
				result.m_nManualReservedBytes =
				        addChecked( result.m_nManualReservedBytes, allocation.m_nEffectiveBytes, arithmetic_overflow );
				remaining -= allocation.m_nEffectiveBytes;
				if ( allocation.m_nEffectiveBytes < capped_manual ) {
					result.m_bHardOvercommit = true;
				}
			}
		}

		for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
			const t3dMemorySubsystemInput_t &input = rInput.m_szSubsystems[( size_t )( index )];
			t3dMemorySubsystemAllocation_t &allocation = result.m_szSubsystems[( size_t )( index )];
			if ( !input.m_bEnabled || !input.m_bAutomatic ) {
				continue;
			}
			const uint64_t minimum = clampToCap( input.m_nMinimumBytes, input.m_nHardCapBytes );
			allocation.m_nRequestedBytes = minimum;
			allocation.m_nEffectiveBytes = minimum < remaining ? minimum : remaining;
			result.m_nAutoMinimumBytes = addChecked( result.m_nAutoMinimumBytes, minimum, arithmetic_overflow );
			remaining -= allocation.m_nEffectiveBytes;
			if ( allocation.m_nEffectiveBytes < minimum ) {
				allocation.m_bBelowMinimum = true;
				allocation.m_limitingReason = TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET;
				result.m_bHardOvercommit = true;
			}
		}

		for ( int pass = 0; pass < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT && remaining > 0u; ++pass ) {
			uint64_t total_weight = 0u;
			for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
				const t3dMemorySubsystemInput_t &input = rInput.m_szSubsystems[( size_t )( index )];
				const t3dMemorySubsystemAllocation_t &allocation = result.m_szSubsystems[( size_t )( index )];
				if ( input.m_bEnabled && input.m_bAutomatic && allocation.m_nEffectiveBytes < input.m_nHardCapBytes ) {
					total_weight = addChecked( total_weight, input.m_nWeight > 0u ? input.m_nWeight : 1u, arithmetic_overflow );
				}
			}
			if ( total_weight == 0u ) {
				break;
			}
			const uint64_t pass_budget = remaining;
			uint64_t distributed = 0u;
			uint64_t remaining_weight = total_weight;
			for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
				const t3dMemorySubsystemInput_t &input = rInput.m_szSubsystems[( size_t )( index )];
				t3dMemorySubsystemAllocation_t &allocation = result.m_szSubsystems[( size_t )( index )];
				if ( !input.m_bEnabled || !input.m_bAutomatic || allocation.m_nEffectiveBytes >= input.m_nHardCapBytes ) {
					continue;
				}
				const uint64_t weight = input.m_nWeight > 0u ? input.m_nWeight : 1u;
				const uint64_t remaining_in_pass = pass_budget - distributed;
				const uint64_t share = remaining_weight > 0u ? ( remaining_in_pass / remaining_weight ) * weight : 0u;
				remaining_weight -= weight;
				const uint64_t room = input.m_nHardCapBytes - allocation.m_nEffectiveBytes;
				uint64_t add = share < room ? share : room;
				add = add < remaining_in_pass ? add : remaining_in_pass;
				if ( add == 0u ) {
					continue;
				}
				allocation.m_nRequestedBytes = addChecked( allocation.m_nRequestedBytes, add, arithmetic_overflow );
				allocation.m_nEffectiveBytes = addChecked( allocation.m_nEffectiveBytes, add, arithmetic_overflow );
				distributed = addChecked( distributed, add, arithmetic_overflow );
				if ( allocation.m_nEffectiveBytes >= input.m_nHardCapBytes && input.m_nHardCapBytes != UINT64_MAX ) {
					allocation.m_limitingReason = TERRAIN3D_MEMORY_LIMIT_FIXED_CAP;
				}
			}
			if ( distributed == 0u ) {
				for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
					const t3dMemorySubsystemInput_t &input = rInput.m_szSubsystems[( size_t )( index )];
					t3dMemorySubsystemAllocation_t &allocation = result.m_szSubsystems[( size_t )( index )];
					if ( !input.m_bEnabled || !input.m_bAutomatic || allocation.m_nEffectiveBytes >= input.m_nHardCapBytes ) {
						continue;
					}
					const uint64_t room = input.m_nHardCapBytes - allocation.m_nEffectiveBytes;
					const uint64_t add = remaining < room ? remaining : room;
					allocation.m_nRequestedBytes = addChecked( allocation.m_nRequestedBytes, add, arithmetic_overflow );
					allocation.m_nEffectiveBytes = addChecked( allocation.m_nEffectiveBytes, add, arithmetic_overflow );
					distributed = add;
					if ( allocation.m_nEffectiveBytes >= input.m_nHardCapBytes && input.m_nHardCapBytes != UINT64_MAX ) {
						allocation.m_limitingReason = TERRAIN3D_MEMORY_LIMIT_FIXED_CAP;
					}
					break;
				}
			}
			if ( distributed == 0u ) {
				break;
			}
			remaining -= distributed;
		}

		result.m_nManagedEffectiveBytes = 0u;
		for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
			t3dMemorySubsystemAllocation_t &allocation = result.m_szSubsystems[( size_t )( index )];
			result.m_nManagedEffectiveBytes =
			        addChecked( result.m_nManagedEffectiveBytes, allocation.m_nEffectiveBytes, arithmetic_overflow );
			if ( allocation.m_limitingReason == TERRAIN3D_MEMORY_LIMIT_NONE && allocation.m_nEffectiveBytes > 0u && remaining == 0u ) {
				allocation.m_limitingReason = TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET;
			}
		}
		result.m_nCommittedBytes = addChecked( fixed_and_transient, result.m_nManagedEffectiveBytes, arithmetic_overflow );
		result.m_nHeadroomBytes =
		        rInput.m_nTotalBudgetBytes > result.m_nCommittedBytes ? rInput.m_nTotalBudgetBytes - result.m_nCommittedBytes : 0u;
		result.m_bHardOvercommit = result.m_bHardOvercommit || arithmetic_overflow || result.m_nCommittedBytes > rInput.m_nTotalBudgetBytes;
		return result;
	}
