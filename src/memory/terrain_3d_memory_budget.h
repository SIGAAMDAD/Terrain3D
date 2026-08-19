// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#pragma once

#ifndef TERRAIN3D_MEMORY_BUDGET_H
#define TERRAIN3D_MEMORY_BUDGET_H

#include "eastl_config.h"
#include <EASTL/array.h>
#include <cstddef>
#include <cstdint>
#include <limits>


typedef enum t3dmemorySubsystem_e : uint8_t {
	TERRAIN3D_MEMORY_GPU_TERRAIN_PAGES = 0,
	TERRAIN3D_MEMORY_CPU_TERRAIN_PAGES,
	TERRAIN3D_MEMORY_INSTANCE_PAGES,
	TERRAIN3D_MEMORY_BAKED_LIGHTING,
	TERRAIN3D_MEMORY_HLOD_PROXIES,
	TERRAIN3D_MEMORY_SHADER_CACHE,
	TERRAIN3D_MEMORY_SUBSYSTEM_COUNT,
} t3dmemorySubsystem_t;

typedef enum t3dmemoryLimitReason_e : uint8_t {
	TERRAIN3D_MEMORY_LIMIT_NONE = 0,
	TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET,
	TERRAIN3D_MEMORY_LIMIT_FIXED_CAP,
	TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE,
	TERRAIN3D_MEMORY_LIMIT_DISABLED,
} t3dmemoryLimitReason_t;

typedef struct t3dmemorySubsystemInput_s {
	bool bEnabled = true;
	bool bAutomatic = true;
	uint64_t nManualBytes = 0u;
	uint64_t nMinimumBytes = 0u;
	uint64_t nHardCapBytes = std::numeric_limits<uint64_t>::max();
	uint32_t weight = 1u;
} t3dmemorySubsystemInput_t;

typedef struct t3dmemoryBudgetInput_s {
	uint64_t nTotalBudgetBytes = 0u;
	uint64_t nFixedBytes = 0u;
	uint64_t nTransientReserveBytes = 0u;
	eastl::array<t3dmemorySubsystemInput_t, TERRAIN3D_MEMORY_SUBSYSTEM_COUNT> subsystems{};
} t3dmemoryBudgetInput_t;

typedef struct t3dmemorySubsystemAllocation_s {
	uint64_t nRequestedBytes = 0u;
	uint64_t nEffectiveBytes = 0u;
	t3dmemoryLimitReason_t limitingReason = TERRAIN3D_MEMORY_LIMIT_NONE;
	bool bBelowMinimum = false;
} t3dmemorySubsystemAllocation_t;

typedef struct t3dmemoryBudgetResult_s {
	uint64_t nTotalBudgetBytes = 0u;
	uint64_t nFixedBytes = 0u;
	uint64_t nTransientReserveBytes = 0u;
	uint64_t nManagedBudgetBytes = 0u;
	uint64_t nManualReservedBytes = 0u;
	uint64_t nAutoMinimumBytes = 0u;
	uint64_t nManagedEffectiveBytes = 0u;
	uint64_t nHeadroomBytes = 0u;
	bool bHardOvercommit = false;
	eastl::array<t3dmemorySubsystemAllocation_t, TERRAIN3D_MEMORY_SUBSYSTEM_COUNT> subsystems{};
} t3dmemoryBudgetResult_t;

class T3DMemoryBudgetCoordinator {
	static uint64_t clampToCap( const uint64_t nBytes, const uint64_t nCap ) {
		return nBytes > nCap ? nCap : nBytes;
	}

public:
	static t3dmemoryBudgetResult_t calculate( const t3dmemoryBudgetInput_t &rInput ) {
		t3dmemoryBudgetResult_t result;
		result.nTotalBudgetBytes = rInput.nTotalBudgetBytes;
		result.nFixedBytes = rInput.nFixedBytes;
		result.nTransientReserveBytes = rInput.nTransientReserveBytes;
		const uint64_t fixed_and_transient = rInput.nFixedBytes > std::numeric_limits<uint64_t>::max() - rInput.nTransientReserveBytes ?
			std::numeric_limits<uint64_t>::max() : rInput.nFixedBytes + rInput.nTransientReserveBytes;
		result.nManagedBudgetBytes = rInput.nTotalBudgetBytes > fixed_and_transient ?
			rInput.nTotalBudgetBytes - fixed_and_transient : 0u;
		if ( fixed_and_transient > rInput.nTotalBudgetBytes ) {
			result.bHardOvercommit = true;
		}

		uint64_t remaining = result.nManagedBudgetBytes;
		for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
			const t3dmemorySubsystemInput_t &input = rInput.subsystems[( size_t ) ( index )];
			t3dmemorySubsystemAllocation_t &allocation = result.subsystems[( size_t ) ( index )];
			if ( !input.bEnabled ) {
				allocation.limitingReason = TERRAIN3D_MEMORY_LIMIT_DISABLED;
				continue;
			}
			if ( !input.bAutomatic ) {
				allocation.nRequestedBytes = input.nManualBytes;
				allocation.nEffectiveBytes = clampToCap( input.nManualBytes, input.nHardCapBytes );
				allocation.limitingReason = allocation.nEffectiveBytes < input.nManualBytes ?
					TERRAIN3D_MEMORY_LIMIT_FIXED_CAP : TERRAIN3D_MEMORY_LIMIT_MANUAL_OVERRIDE;
				result.nManualReservedBytes += allocation.nEffectiveBytes;
				if ( remaining >= allocation.nEffectiveBytes ) {
					remaining -= allocation.nEffectiveBytes;
				} else {
					remaining = 0u;
					result.bHardOvercommit = true;
				}
			}
		}

		for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
			const t3dmemorySubsystemInput_t &input = rInput.subsystems[( size_t ) ( index )];
			t3dmemorySubsystemAllocation_t &allocation = result.subsystems[( size_t ) ( index )];
			if ( !input.bEnabled || !input.bAutomatic ) {
				continue;
			}
			const uint64_t minimum = clampToCap( input.nMinimumBytes, input.nHardCapBytes );
			allocation.nRequestedBytes = minimum;
			allocation.nEffectiveBytes = minimum;
			result.nAutoMinimumBytes += minimum;
			if ( remaining >= minimum ) {
				remaining -= minimum;
			} else {
				allocation.bBelowMinimum = true;
				allocation.limitingReason = TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET;
				remaining = 0u;
				result.bHardOvercommit = true;
			}
		}

		for ( int pass = 0; pass < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT && remaining > 0u; ++pass ) {
			uint64_t total_weight = 0u;
			for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
				const t3dmemorySubsystemInput_t &input = rInput.subsystems[( size_t ) ( index )];
				const t3dmemorySubsystemAllocation_t &allocation = result.subsystems[( size_t ) ( index )];
				if ( input.bEnabled && input.bAutomatic && allocation.nEffectiveBytes < input.nHardCapBytes ) {
					total_weight += input.weight > 0u ? input.weight : 1u;
				}
			}
			if ( total_weight == 0u ) {
				break;
			}
			const uint64_t pass_budget = remaining;
			uint64_t distributed = 0u;
			uint64_t remaining_weight = total_weight;
			for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
				const t3dmemorySubsystemInput_t &input = rInput.subsystems[( size_t ) ( index )];
				t3dmemorySubsystemAllocation_t &allocation = result.subsystems[( size_t ) ( index )];
				if ( !input.bEnabled || !input.bAutomatic || allocation.nEffectiveBytes >= input.nHardCapBytes ) {
					continue;
				}
				const uint64_t weight = input.weight > 0u ? input.weight : 1u;
				const uint64_t remaining_in_pass = pass_budget - distributed;
				const uint64_t share = remaining_weight > 0u ?
					( remaining_in_pass / remaining_weight ) * weight : 0u;
				remaining_weight -= weight;
				const uint64_t room = input.nHardCapBytes - allocation.nEffectiveBytes;
				uint64_t add = share < room ? share : room;
				add = add < remaining_in_pass ? add : remaining_in_pass;
				if ( add == 0u ) {
					continue;
				}
				allocation.nRequestedBytes += add;
				allocation.nEffectiveBytes += add;
				distributed += add;
				if ( allocation.nEffectiveBytes >= input.nHardCapBytes && input.nHardCapBytes != std::numeric_limits<uint64_t>::max() ) {
					allocation.limitingReason = TERRAIN3D_MEMORY_LIMIT_FIXED_CAP;
				}
			}
			if ( distributed == 0u ) {
				for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
					const t3dmemorySubsystemInput_t &input = rInput.subsystems[( size_t ) ( index )];
					t3dmemorySubsystemAllocation_t &allocation = result.subsystems[( size_t ) ( index )];
					if ( !input.bEnabled || !input.bAutomatic || allocation.nEffectiveBytes >= input.nHardCapBytes ) {
						continue;
					}
					const uint64_t room = input.nHardCapBytes - allocation.nEffectiveBytes;
					const uint64_t add = remaining < room ? remaining : room;
					allocation.nRequestedBytes += add;
					allocation.nEffectiveBytes += add;
					distributed = add;
					if ( allocation.nEffectiveBytes >= input.nHardCapBytes && input.nHardCapBytes != std::numeric_limits<uint64_t>::max() ) {
						allocation.limitingReason = TERRAIN3D_MEMORY_LIMIT_FIXED_CAP;
					}
					break;
				}
			}
			if ( distributed == 0u ) {
				break;
			}
			remaining -= distributed;
		}

		result.nManagedEffectiveBytes = 0u;
		for ( int index = 0; index < TERRAIN3D_MEMORY_SUBSYSTEM_COUNT; ++index ) {
			t3dmemorySubsystemAllocation_t &allocation = result.subsystems[( size_t ) ( index )];
			result.nManagedEffectiveBytes += allocation.nEffectiveBytes;
			if ( allocation.limitingReason == TERRAIN3D_MEMORY_LIMIT_NONE &&
				allocation.nEffectiveBytes > 0u && remaining == 0u ) {
				allocation.limitingReason = TERRAIN3D_MEMORY_LIMIT_TOTAL_BUDGET;
			}
		}
		const uint64_t committed = fixed_and_transient > std::numeric_limits<uint64_t>::max() - result.nManagedEffectiveBytes ?
			std::numeric_limits<uint64_t>::max() : fixed_and_transient + result.nManagedEffectiveBytes;
		result.nHeadroomBytes = rInput.nTotalBudgetBytes > committed ? rInput.nTotalBudgetBytes - committed : 0u;
		return result;
	}
};

#endif // TERRAIN3D_MEMORY_BUDGET_H
