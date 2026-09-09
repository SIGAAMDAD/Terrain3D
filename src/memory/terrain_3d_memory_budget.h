// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_MEMORY_BUDGET_H
#define TERRAIN3D_MEMORY_BUDGET_H

#include "core/terrain_3d_compiler.h"

#include <stddef.h>
#include <stdint.h>

typedef enum t3dMemorySubsystem_e : uint8_t {
	TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES = 0,
	TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES,
	TERRAIN3D_MEMORY_INSTANCE_PAGES,
	TERRAIN3D_MEMORY_BAKED_LIGHTING,
	TERRAIN3D_MEMORY_HLOD_PROXIES,
	TERRAIN3D_MEMORY_SHADER_CACHE,
	TERRAIN3D_MEMORY_SUBSYSTEM_COUNT,
} t3dMemorySubsystem_t;

typedef enum t3dMemoryLimitReason_e : uint8_t {
	TERRAIN3D_MEMORY_LIMIT_NONE = 0,
	TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET,
	TERRAIN3D_MEMORY_LIMIT_FIXED_CAP,
	TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE,
	TERRAIN3D_MEMORY_LIMIT_DISABLED,
} t3dMemoryLimitReason_t;

typedef struct t3dMemorySubsystemInput_s {
	bool m_bEnabled = true;
	bool m_bAutomatic = true;
	uint64_t m_nManualBytes = 0u;
	uint64_t m_nMinimumBytes = 0u;
	uint64_t m_nHardCapBytes = UINT64_MAX;
	uint32_t m_nWeight = 1u;
} t3dMemorySubsystemInput_t;

typedef struct t3dMemoryBudgetInput_s {
	uint64_t m_nTotalBudgetBytes = 0u;
	uint64_t m_nFixedBytes = 0u;
	uint64_t m_nAllocatorContainedFixedBytes = 0u;
	uint64_t m_nAllocatorReserveBytes = 0u;
	uint64_t m_nTransientReserveBytes = 0u;
	t3dMemorySubsystemInput_t m_szSubsystems[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT]{};
} t3dMemoryBudgetInput_t;

typedef struct t3dMemorySubsystemAllocation_s {
	uint64_t m_nRequestedBytes = 0u;
	uint64_t m_nEffectiveBytes = 0u;
	t3dMemoryLimitReason_t m_limitingReason = TERRAIN3D_MEMORY_LIMIT_NONE;
	bool m_bBelowMinimum = false;
} t3dMemorySubsystemAllocation_t;

typedef struct t3dMemoryBudgetResult_s {
	uint64_t m_nTotalBudgetBytes = 0u;
	uint64_t m_nFixedBytes = 0u;
	uint64_t m_nAllocatorContainedFixedBytes = 0u;
	uint64_t m_nAllocatorReserveBytes = 0u;
	uint64_t m_nTransientReserveBytes = 0u;
	uint64_t m_nManagedBudgetBytes = 0u;
	uint64_t m_nManualReservedBytes = 0u;
	uint64_t m_nAutoMinimumBytes = 0u;
	uint64_t m_nManagedEffectiveBytes = 0u;
	uint64_t m_nCommittedBytes = 0u;
	uint64_t m_nHeadroomBytes = 0u;
	bool m_bHardOvercommit = false;
	t3dMemorySubsystemAllocation_t m_szSubsystems[TERRAIN3D_MEMORY_SUBSYSTEM_COUNT]{};
} t3dMemoryBudgetResult_t;

class Terrain3DMemoryBudgetCoordinator {
	EA_FORCE_INLINE static uint64_t addChecked( const uint64_t nA, const uint64_t nB, bool &rOverflow ) {
		if ( nA > UINT64_MAX - nB ) {
			rOverflow = true;
			return UINT64_MAX;
		}
		return nA + nB;
	}

	EA_FORCE_INLINE static uint64_t clampToCap( const uint64_t nBytes, const uint64_t nCap ) {
		return nBytes > nCap ? nCap : nBytes;
	}

public:
	static t3dMemoryBudgetResult_t calculate( const t3dMemoryBudgetInput_t &rInput ) {
		t3dMemoryBudgetResult_t result;
		result.m_nTotalBudgetBytes = rInput.m_nTotalBudgetBytes;
		// Fixed logical capacity physically served by the reserved zone is already
		// paid for by m_nAllocatorReserveBytes. Charge only any excess once.
		const uint64_t allocator_contained = rInput.m_nAllocatorContainedFixedBytes < rInput.m_nFixedBytes
		        ? rInput.m_nAllocatorContainedFixedBytes
		        : rInput.m_nFixedBytes;
		result.m_nAllocatorContainedFixedBytes = allocator_contained < rInput.m_nAllocatorReserveBytes
		        ? allocator_contained
		        : rInput.m_nAllocatorReserveBytes;
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
				        : ( capped_manual < input.m_nManualBytes ? TERRAIN3D_MEMORY_LIMIT_FIXED_CAP : TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE );
				result.m_nManualReservedBytes = addChecked( result.m_nManualReservedBytes, allocation.m_nEffectiveBytes, arithmetic_overflow );
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
				if ( allocation.m_nEffectiveBytes >= input.m_nHardCapBytes
				        && input.m_nHardCapBytes != UINT64_MAX ) {
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
					if ( allocation.m_nEffectiveBytes >= input.m_nHardCapBytes
					        && input.m_nHardCapBytes != UINT64_MAX ) {
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
			result.m_nManagedEffectiveBytes = addChecked( result.m_nManagedEffectiveBytes, allocation.m_nEffectiveBytes, arithmetic_overflow );
			if ( allocation.m_limitingReason == TERRAIN3D_MEMORY_LIMIT_NONE && allocation.m_nEffectiveBytes > 0u && remaining == 0u ) {
				allocation.m_limitingReason = TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET;
			}
		}
		result.m_nCommittedBytes = addChecked( fixed_and_transient, result.m_nManagedEffectiveBytes, arithmetic_overflow );
		result.m_nHeadroomBytes = rInput.m_nTotalBudgetBytes > result.m_nCommittedBytes ? rInput.m_nTotalBudgetBytes - result.m_nCommittedBytes : 0u;
		result.m_bHardOvercommit = result.m_bHardOvercommit || arithmetic_overflow || result.m_nCommittedBytes > rInput.m_nTotalBudgetBytes;
		return result;
	}
};

#endif // TERRAIN3D_MEMORY_BUDGET_H
