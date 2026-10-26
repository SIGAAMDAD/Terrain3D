// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RD_DESCRIPTOR_LEASE_TABLE_H
#define TERRAIN3D_RD_DESCRIPTOR_LEASE_TABLE_H

#include "rendering/rd/core/terrain_3d_rd_context.h"
#include "rendering/rd/infrastructure/descriptors/terrain_3d_rd_global_descriptor_cache.h"
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// Small per-consumer lookup table. It owns ONLY global-cache leases, never
// physical uniform-set RIDs. Physical creation and retirement are centralized.
template< typename TKey, uint32_t Capacity > class Terrain3DRdDescriptorLeaseTable
{
	static_assert( Capacity > 0u, "Descriptor lease table requires storage" );
	struct Entry
	{
		TKey				   m_key{};
		t3dRdDescriptorLease_t m_lease{};
		uint64_t			   m_nFingerprint = 0u;
		godot::RID			   m_hLastResolved;
	};
	Terrain3DRdContext	&m_context;
	t3dRdResourceOwner_t m_owner;
	Entry				 m_entries[Capacity]{};
	uint32_t			 m_nCount = 0u;
	uint32_t			 m_nEvictCursor = 0u;

	Terrain3DRdGlobalDescriptorCache *cache() const {
		return m_context.tryGetGlobalDescriptorCacheHot();
	}
	godot::RID resolveEntry(
		const Entry &rEntry ) const {
		Terrain3DRdGlobalDescriptorCache *const pCache = cache();
		godot::RID								result;
		// A render-owner epoch is advanced only at a new native render frame.
		// GPU completion is proven separately by the async readback callback.
		return pCache && pCache->resolveDescriptorHot(
			rEntry.m_lease, pCache->currentSubmissionEpochHot(), result ) == T3D_RD_DESCRIPTOR_OK
				   ? result
				   : godot::RID();
	}
	void releaseAt(
		uint32_t nIndex ) {
		if ( Terrain3DRdGlobalDescriptorCache *const pCache = cache() ) {
			pCache->releaseDescriptorCold( m_entries[nIndex].m_lease );
		}
		const uint32_t nLast = --m_nCount;
		if ( nIndex != nLast ) {
			m_entries[nIndex] = m_entries[nLast];
		}
		m_entries[nLast] = Entry{};
	}

public:
	Terrain3DRdDescriptorLeaseTable(
		Terrain3DRdContext	&rContext,
		t3dRdResourceOwner_t eOwner ) :
		m_context( rContext ),
		m_owner( eOwner ) {
	}
	~Terrain3DRdDescriptorLeaseTable() {
		clear();
	}

	godot::RID find(
		const TKey &rKey ) const {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_key == rKey ) {
				return resolveEntry( m_entries[i] );
			}
		}
		return godot::RID();
	}
	godot::RID resolve(
		const godot::RID &rPrevious ) const {
		if ( !rPrevious.is_valid() ) {
			return godot::RID();
		}
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_hLastResolved == rPrevious ) {
				return resolveEntry( m_entries[i] );
			}
		}
		return godot::RID();
	}
	godot::RID getOrCreate(
		const TKey												  &rKey,
		const godot::String										  &rName,
		const godot::TypedArray< godot::Ref< godot::RDUniform > > &rUniforms,
		const godot::RID										  &rShader,
		uint32_t												   nSetIndex ) {
		Terrain3DRdGlobalDescriptorCache *const pCache = cache();
		if ( !pCache ) {
			godot::UtilityFunctions::push_error( "[Terrain3D][DescriptorLease] Global cache unavailable name=",
												 rName,
												 " set=",
												 ( int64_t ) nSetIndex );
			return godot::RID();
		}
		t3dRdDescriptorRecipe_t			 recipe{};
		const t3dRdDescriptorDeviceKey_t device{ T3D_RD_DESCRIPTOR_GLOBAL_DEVICE_ID,
												 m_context.getDeviceGeneration(),
												 T3D_RD_DESCRIPTOR_DEVICE_GLOBAL };
		if ( t3dRdDescriptorRecipeBeginCold( recipe, device, rShader, nSetIndex ) != T3D_RD_DESCRIPTOR_OK ) {
			return godot::RID();
		}
		for ( int64_t i = 0; i < rUniforms.size(); ++i ) {
			const godot::Ref< godot::RDUniform > uniform = rUniforms[i];
			if ( uniform.is_null() ) {
				return godot::RID();
			}
			const godot::TypedArray< godot::RID > ids = uniform->get_ids();
			if ( ids.size() <= 0 || ids.size() > ( int64_t ) T3D_RD_DESCRIPTOR_MAX_RESOURCES ) {
				return godot::RID();
			}
			godot::RID resources[T3D_RD_DESCRIPTOR_MAX_RESOURCES];
			for ( int64_t j = 0; j < ids.size(); ++j ) {
				resources[j] = ids[j];
			}
			if ( t3dRdDescriptorRecipeAddBindingCold( recipe,
													  ( uint32_t ) uniform->get_binding(),
													  ( uint32_t ) uniform->get_uniform_type(),
													  resources,
													  ( uint32_t ) ids.size() ) != T3D_RD_DESCRIPTOR_OK ) {
				return godot::RID();
			}
		}
		const uint64_t nFingerprint = t3dRdDescriptorRecipeFingerprint( recipe );
		if ( !nFingerprint ) {
			return godot::RID();
		}
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( !( m_entries[i].m_key == rKey ) ) {
				continue;
			}
			if ( pCache->descriptorRecipeMatchesHot( m_entries[i].m_lease, recipe, nFingerprint ) ) {
				const godot::RID existing = resolveEntry( m_entries[i] );
				if ( existing.is_valid() ) {
					return existing;
				}
			}
			releaseAt( i );
			break;
		}
		// This consumer releases its oldest lease at a cold replacement boundary.
		// The global service retains the physical RID until GPU-safe retirement.
		if ( m_nCount == Capacity ) {
			releaseAt( m_nEvictCursor++ % m_nCount );
		}
		t3dRdDescriptorLease_t		  lease;
		const t3dRdDescriptorStatus_t status = pCache->acquireDescriptorCold( recipe, m_owner, lease );
		if ( status != T3D_RD_DESCRIPTOR_OK ) {
			godot::UtilityFunctions::push_error( "[Terrain3D][DescriptorLease] Acquire failed name=",
												 rName,
												 " status=",
												 ( int64_t ) status,
												 " set=",
												 ( int64_t ) nSetIndex,
												 " bindings=",
												 ( int64_t ) recipe.m_nBindingCount,
												 " resources=",
												 ( int64_t ) recipe.m_nResourceCount,
												 " rd_failure=",
												 m_context.getLastFailure() );
			return godot::RID();
		}
		Entry &rEntry = m_entries[m_nCount];
		rEntry.m_key = rKey;
		rEntry.m_nFingerprint = nFingerprint;
		rEntry.m_lease = lease;
		rEntry.m_hLastResolved = resolveEntry( rEntry );
		if ( !rEntry.m_hLastResolved.is_valid() ) {
			godot::UtilityFunctions::push_error( "[Terrain3D][DescriptorLease] Resolve failed name=",
												 rName,
												 " set=",
												 ( int64_t ) nSetIndex );
			pCache->releaseDescriptorCold( rEntry.m_lease );
			rEntry = Entry{};
			return godot::RID();
		}
		++m_nCount;
		return rEntry.m_hLastResolved;
	}
	void erase(
		const godot::RID &rRid ) {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			if ( m_entries[i].m_hLastResolved == rRid ) {
				releaseAt( i );
				return;
			}
		}
	}
	void clear() {
		while ( m_nCount ) {
			releaseAt( m_nCount - 1u );
		}
		m_nEvictCursor = 0u;
	}
	// Global cache discards the old device's physical RIDs; only our leases
	// need clearing here, without calling into a lost RenderingDevice.
	void abandon() {
		for ( uint32_t i = 0u; i < m_nCount; ++i ) {
			m_entries[i] = Entry{};
		}
		m_nCount = m_nEvictCursor = 0u;
	}
	uint32_t size() const {
		return m_nCount;
	}
};

#endif // TERRAIN3D_RD_DESCRIPTOR_LEASE_TABLE_H
