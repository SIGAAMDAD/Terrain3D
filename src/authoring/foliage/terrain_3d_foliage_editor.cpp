// All foliage publication policies/state are editor-owned. Templates exclude this translation unit.
#include "authoring/foliage/terrain_3d_foliage_editor.h"
#include "terrain_3d.h"
#include "instances/runtime/terrain_3d_instance_mutation_helpers.h"
#include "instances/storage/terrain_3d_instance_page_codec.h"
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#if defined(T3D_EDITOR_NATIVE)
// Editor-thread reservation. Transactions cannot overwrite one another's journal
// while staging the same terrain, and no reservation lives on a runtime object.
static eastl::vector_map<uint64_t, uint64_t> s_publications;
Terrain3DFoliageEditor::~Terrain3DFoliageEditor() { setTerrain(nullptr); }
Terrain3D *Terrain3DFoliageEditor::getTerrain() const {
    return Object::cast_to<Terrain3D>(ObjectDB::get_instance(m_terrainId));
}
void Terrain3DFoliageEditor::setTerrain(Terrain3D *pTerrain) {
    const uint64_t id = pTerrain ? (uint64_t)pTerrain->get_instance_id() : 0u;
    if (id == m_terrainId) { return; }
    Terrain3D *previous = getTerrain();
    const Callable callback = callable_mp(this, &Terrain3DFoliageEditor::terrainExiting);
    if (previous && previous->is_connected("tree_exiting", callback)) {
        previous->disconnect("tree_exiting", callback);
    }
    cancelRestore();
    m_terrainId = id;
    if (pTerrain) { pTerrain->connect("tree_exiting", callback); }
}
void Terrain3DFoliageEditor::terrainExiting() {
    // Drop borrowed region resources before the terrain's allocators shut down.
    cancelRestore();
    m_terrainId = 0;
}
int64_t Terrain3DFoliageEditor::sourceStamp() { return (int64_t)Terrain3DEditorRevisions::sourceStamp(); }
int64_t Terrain3DFoliageEditor::instanceRevision(const Ref<Terrain3DRegion> &rRegion) {
    return rRegion.is_valid() ? (int64_t)rRegion->getAuthoringInstanceRevision() : 0;
}
Dictionary Terrain3DFoliageEditor::replaceOwnedRegions(
	const Array &rBatches,
	const bool	 bBackup ) {
	return replaceOwnedRegionsInternal( rBatches, bBackup, true );
}

Dictionary Terrain3DFoliageEditor::prepareOwnedRegions(
	const Array &rBatches ) {
	return replaceOwnedRegionsInternal( rBatches, false, false );
}

struct Terrain3DFoliageEditor::RestoreState {
	String id;
	Dictionary after, before, afterGenerations, beforeGenerations, persistence, expected;
	Array locations;
	TypedArray<Terrain3DRegion> regions;
	eastl::vector<Terrain3DInstanceStore, Terrain3DEastlAllocator> afterStores, beforeStores;
	int decoded = 0;
	bool publishedAfter = false, publishedBefore = false;
};

Dictionary Terrain3DFoliageEditor::beginRestore( const String &rId, const Dictionary &rAfter,
	const Dictionary &rBefore, const Dictionary &rAfterGenerations, const Dictionary &rBeforeGenerations,
	const Dictionary &rBeforePersistence, const Dictionary &rExpectedRevisions ) {
    Terrain3D *m_pTerrain = getTerrain();
    Terrain3DInstancer *instancer = m_pTerrain ? m_pTerrain->getInstancer() : nullptr;
    if (!instancer) { Dictionary failure; failure["ok"] = false; failure["error"] = "Foliage terrain is no longer available"; return failure; }

	Dictionary result;
	result["ok"] = false;
	result["error"] = "Another foliage publication is pending or its input is invalid";
	if (m_restore || s_publications.find(m_terrainId) != s_publications.end() || !m_pTerrain || !m_pTerrain->dataObject() || rId.is_empty() || rAfter.is_empty()) { return result; }
	RestoreState *state = memnew(RestoreState);
	state->id = rId; state->after = rAfter; state->before = rBefore;
	state->afterGenerations = rAfterGenerations; state->beforeGenerations = rBeforeGenerations;
	state->persistence = rBeforePersistence; state->expected = rExpectedRevisions;
	state->locations = rAfter.keys();
	state->afterStores.set_allocator(m_placements.get_allocator());
	state->beforeStores.set_allocator(m_placements.get_allocator());
	if (!t3dContainerReserveCold(state->afterStores, (size_t)rAfter.size()) ||
		!t3dContainerReserveCold(state->beforeStores, (size_t)rAfter.size())) { memdelete(state); return result; }
	for (int i = 0; i < state->locations.size(); ++i) {
		const Vector2i location = state->locations[i];
		const Ref<Terrain3DRegion> region = m_pTerrain->dataObject()->getRegion(location);
		if (region.is_null() || region->isDeleted() || !rBefore.has(location) || !rExpectedRevisions.has(location) ||
			!rAfterGenerations.has(location) || !rBeforeGenerations.has(location)) { memdelete(state); return result; }
		state->regions.push_back(region);
	}
	m_restore = state;
	s_publications[m_terrainId] = (uint64_t)get_instance_id();
	result["ok"] = true; result["error"] = "";
	return result;
}

Dictionary Terrain3DFoliageEditor::stepRestore( const String &rId, int nPages, int64_t nBytes ) {
    Terrain3D *m_pTerrain = getTerrain();
    Terrain3DInstancer *instancer = m_pTerrain ? m_pTerrain->getInstancer() : nullptr;
    if (!instancer) { Dictionary failure; failure["ok"] = false; failure["error"] = "Foliage terrain is no longer available"; return failure; }

	Dictionary result;
	result["ok"] = false; result["error"] = "No matching foliage publication";
	RestoreState *state = m_restore;
	if (!state || state->id != rId) { return result; }
	nPages = CLAMP(nPages, 1, 16); nBytes = MAX((int64_t)65536, nBytes);
	int pages = 0; int64_t bytes = 0;
	const int count = state->locations.size();
	while (state->decoded < 2 * count && pages < nPages) {
		const bool after = state->decoded < count;
		const int index = state->decoded % count;
		const Vector2i location = state->locations[index];
		const PackedByteArray page = (after ? state->after : state->before)[location];
		if (pages > 0 && bytes + page.size() > nBytes) { break; }
		Terrain3DInstanceStore store;
		uint32_t counts[T3D_MAX_MESH_ASSETS] = {}; uint32_t total = 0; uint16_t meshes = 0;
		if (!Terrain3DInstancePageCodec::decode(page, store, counts, total, meshes) || !store.validateDeep()) {
			result["error"] = "Cannot decode prepared foliage publication page"; return result;
		}
		auto &stores = after ? state->afterStores : state->beforeStores;
		if (!t3dVectorPushBackWithinCapacity(stores, eastl::move(store))) { result["error"] = "Foliage publication memory exhausted"; return result; }
		bytes += page.size(); ++pages; ++state->decoded;
	}
	result["ok"] = true; result["error"] = "";
	result["ready"] = state->decoded == 2 * count; result["decoded_pages"] = state->decoded;
	result["slice_bytes"] = bytes;
	return result;
}

Dictionary Terrain3DFoliageEditor::publishRestore( const String &rId, const bool bAfter ) {
    Terrain3D *m_pTerrain = getTerrain();
    Terrain3DInstancer *instancer = m_pTerrain ? m_pTerrain->getInstancer() : nullptr;
    if (!instancer) { Dictionary failure; failure["ok"] = false; failure["error"] = "Foliage terrain is no longer available"; return failure; }

	Dictionary result;
	result["ok"] = false; result["error"] = "Prepared foliage publication is unavailable or stale";
	RestoreState *state = m_restore;
	if (!state || state->id != rId || !m_pTerrain || state->decoded != state->locations.size() * 2 ||
		(bAfter ? state->publishedAfter : state->publishedBefore)) { return result; }
	for (int i = 0; i < state->locations.size(); ++i) {
		const Vector2i location = state->locations[i];
		const Ref<Terrain3DRegion> region = state->regions[i];
		if (m_pTerrain->dataObject()->getRegionPtr(location) != region.ptr() || region->isDeleted() ||
			(bAfter && region->getAuthoringInstanceRevision() != (uint64_t)(int64_t)state->expected[location])) { return result; }
	}
	auto &stores = bAfter ? state->afterStores : state->beforeStores;
	const Dictionary pages = bAfter ? state->after : state->before;
	const Dictionary generations = bAfter ? state->afterGenerations : state->beforeGenerations;
	for (int i = 0; i < state->locations.size(); ++i) {
		const Vector2i location = state->locations[i];
		const Ref<Terrain3DRegion> region = state->regions[i];
		region->setInstancePage(pages[location]);
		region->adoptPreparedInstanceStoreInternal(eastl::move(stores[(size_t)i]));
		region->setInstanceGeneration(generations[location]);
		region->acknowledgeInstanceSave();
		if (!bAfter && state->persistence.has(location)) {
			const Dictionary flags = state->persistence[location];
			region->restoreInstancePersistenceState(flags.get("dirty_domains", 0), flags.get("modified", false));
		}
		t3dInstancerRefreshCommittedResidentBytes(m_pTerrain->dataObject(), location);
	}
	if (bAfter) { state->publishedAfter = true; } else { state->publishedBefore = true; }
	instancer->rebuildLogicalCountsFromStore();
	for (int i = 0; i < state->locations.size(); ++i) { instancer->updateMmis(-1, state->locations[i]); }
	result["ok"] = true; result["error"] = "";
	return result;
}

void Terrain3DFoliageEditor::cancelRestore( const String &rId ) {
	if (m_restore && (rId.is_empty() || m_restore->id == rId)) {
		memdelete(m_restore); m_restore = nullptr;
		const auto reservation = s_publications.find(m_terrainId);
		if (reservation != s_publications.end() && reservation->second == (uint64_t)get_instance_id()) {
			s_publications.erase(reservation);
		}
	}
	if (!m_restore) { m_placements.clear(); m_placements.shrink_to_fit(); }
}

Dictionary Terrain3DFoliageEditor::restoreInstancePages(
	const Dictionary &rPages, const Dictionary &rGenerations, const Dictionary &rPersistenceStates ) {
    Terrain3D *m_pTerrain = getTerrain();
    Terrain3DInstancer *instancer = m_pTerrain ? m_pTerrain->getInstancer() : nullptr;
    if (!instancer) { Dictionary failure; failure["ok"] = false; failure["error"] = "Foliage terrain is no longer available"; return failure; }

	Dictionary result;
	result["ok"] = false;
	result["error"] = "Cannot stage foliage page restoration";
	if ( !m_pTerrain || !m_pTerrain->dataObject() || rPages.is_empty() ) {
		return result;
	}
	Terrain3DData *const pData = m_pTerrain->dataObject();
	eastl::vector< t3dPreparedRegionMutation_t, Terrain3DEastlAllocator > stages;
	stages.set_allocator( m_placements.get_allocator() );
	if ( !t3dContainerReserveCold( stages, (size_t)rPages.size() ) ) {
		return result;
	}
	const Array locations = rPages.keys();
	for ( int index = 0; index < locations.size(); ++index ) {
		const Vector2i location = locations[index];
		Terrain3DRegion *const region = pData->getRegionPtr( location );
		if ( !region || region->isDeleted() || !rGenerations.has( location ) || (int64_t)rGenerations[location] < 0 ) {
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		t3dPreparedRegionMutation_t stage;
		stage.m_pRegion = region;
		stage.m_location = location;
		if ( !t3dBeginInstancePageAccess( instancer, pData, location, stage.m_bTemporaryPage ) ) {
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		uint32_t counts[T3D_MAX_MESH_ASSETS] = {};
		uint32_t count = 0u;
		uint16_t meshCount = 0u;
		const PackedByteArray page = rPages[location];
		if ( !Terrain3DInstancePageCodec::decode( page, stage.m_store, counts, count, meshCount ) ||
			 !stage.m_store.validateDeep() || !t3dVectorPushBackWithinCapacity( stages, eastl::move( stage ) ) ) {
			t3dEndInstancePageAccess( pData, location, stage.m_bTemporaryPage );
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
	}
	// Every page is decoded before publication. Reuse the normal authoritative
	// count/cache/render publication path; script-side setters cannot own these.
	for ( size_t index = 0; index < stages.size(); ++index ) {
		t3dPreparedRegionMutation_t &stage = stages[index];
		stage.m_pRegion->setInstancePage( rPages[stage.m_location] );
		stage.m_pRegion->adoptPreparedInstanceStoreInternal( eastl::move( stage.m_store ) );
		stage.m_pRegion->setInstanceGeneration( rGenerations[stage.m_location] );
		stage.m_pRegion->acknowledgeInstanceSave();
		if ( rPersistenceStates.has( stage.m_location ) ) {
			const Dictionary state = rPersistenceStates[stage.m_location];
			stage.m_pRegion->restoreInstancePersistenceState( state.get( "dirty_domains", 0 ), state.get( "modified", false ) );
		}
		t3dInstancerRefreshCommittedResidentBytes( pData, stage.m_location );
	}
	instancer->rebuildLogicalCountsFromStore();
	for ( size_t index = 0; index < stages.size(); ++index ) {
		t3dPreparedRegionMutation_t &stage = stages[index];
		if ( stage.m_bTemporaryPage && stage.m_pRegion->hasInstances() ) {
			(void)t3dRetainMutatedInstancePage( pData, stage.m_pRegion, stage.m_bTemporaryPage );
		}
		instancer->updateMmis( -1, stage.m_location );
	}
	t3dInstancerClosePreparedPages( pData, stages );
	result["ok"] = true;
	result["error"] = "";
	return result;
}

Dictionary Terrain3DFoliageEditor::replaceOwnedRegionsInternal(
	const Array &rBatches,
	const bool bBackup,
	const bool bPublish ) {
    Terrain3D *m_pTerrain = getTerrain();
    Terrain3DInstancer *instancer = m_pTerrain ? m_pTerrain->getInstancer() : nullptr;
    if (!instancer) { Dictionary failure; failure["ok"] = false; failure["error"] = "Foliage terrain is no longer available"; return failure; }

	Dictionary result;
	result["ok"] = false;
	result["error"] = "Invalid foliage replacement";
	if ( !m_pTerrain || !m_pTerrain->dataObject() || !m_pTerrain->assetsResource().is_valid() || rBatches.is_empty() ) {
		return result;
	}
	Terrain3DData *const												  pData = m_pTerrain->dataObject();
	eastl::vector< t3dPreparedRegionMutation_t, Terrain3DEastlAllocator > stages;
	stages.set_allocator( m_placements.get_allocator() );
	if ( !t3dContainerReserveCold( stages, ( size_t ) rBatches.size() ) ) {
		result["error"] = "No region staging memory";
		return result;
	}
	// Acquire and clone every affected region before touching any live state.
	for ( int i = 0; i < rBatches.size(); ++i ) {
		const Dictionary				batch = rBatches[i];
		const Vector2i					region = batch.get( "region", Vector2i( INT32_MAX, INT32_MAX ) );
		const int64_t					owner = batch.get( "owner", ( int64_t ) 0 );
		const int						mesh = batch.get( "mesh_id", -1 );
		const TypedArray< Transform3D > transforms = batch.get( "transforms", TypedArray< Transform3D >() );
		const bool invalid_mesh = !transforms.is_empty() &&
			( mesh < 0 || mesh >= m_pTerrain->assetsResource()->getMeshCount() );
		if ( owner <= 0 || owner > INT32_MAX || invalid_mesh ||
			 transforms.size() > 1048576 || !pData->hasRegion( region ) ) {
			result["error"] = "Invalid owner, mesh, region or batch capacity";
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		bool exists = false;
		for ( size_t j = 0; j < stages.size(); ++j ) {
			if ( stages[j].m_location == region ) {
				exists = true;
				break;
			}
		}
		if ( exists ) {
			continue;
		}
		Terrain3DRegion *const pRegion = pData->getRegionPtr( region );
		if ( !pRegion || pRegion->isDeleted() ) {
			result["error"] = "Region removed during staging";
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		t3dPreparedRegionMutation_t stage;
		stage.m_pRegion = pRegion;
		stage.m_location = region;
		if ( !t3dBeginInstancePageAccess( instancer, pData, region, stage.m_bTemporaryPage ) ) {
			result["error"] = "Cannot materialize instance page";
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		if ( !pRegion->tryGetInstanceStoreInternal( stage.m_pCommitStore ) || !stage.m_pCommitStore ) {
			t3dEndInstancePageAccess( pData, region, stage.m_bTemporaryPage );
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		stage.m_store = *stage.m_pCommitStore;
		if ( !t3dInstancerStoreCloneMatches( *stage.m_pCommitStore, stage.m_store ) ||
			 !t3dVectorPushBackWithinCapacity( stages, eastl::move( stage ) ) ) {
			t3dEndInstancePageAccess( pData, region, stage.m_bTemporaryPage );
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
	}
	// Drop only matching owner records, never owner 0 or unrelated generated owners.
	for ( int i = 0; i < rBatches.size(); ++i ) {
		const Dictionary batch = rBatches[i];
		const Vector2i	 region = batch["region"];
		const uint32_t	 owner = ( uint32_t ) ( ( int64_t ) batch["owner"] );
		for ( size_t j = 0; j < stages.size(); ++j ) {
			if ( stages[j].m_location != region ) {
				continue;
			}
			Terrain3DInstanceStore &store = stages[j].m_store;
			for ( int mi = ( int ) store.meshesMut().size() - 1; mi >= 0; --mi ) {
				const int				 mesh_id = store.meshesMut()[( size_t ) mi].m_nId;
				t3dInstanceMesh_t *const pMesh = store.findMesh( mesh_id );
				if ( !pMesh ) {
					continue;
				}
				for ( int ci = ( int ) pMesh->m_cells.size() - 1; ci >= 0; --ci ) {
					t3dInstanceCell_s &cell = pMesh->m_cells[( size_t ) ci];
					if ( !cell.usesOwners() ) {
						continue;
					}
					size_t write = 0u;
					for ( size_t read = 0u; read < cell.m_transforms.size(); ++read ) {
						if ( cell.getOwner( read ) == owner ) {
							continue;
						}
						if ( write != read ) {
							cell.m_transforms[write] = cell.m_transforms[read];
							if ( cell.usesColors() ) {
								cell.m_colors[write] = cell.m_colors[read];
							}
							cell.m_owners[write] = cell.m_owners[read];
						}
						++write;
					}
					if ( write == cell.m_transforms.size() ) {
						continue;
					}
					if ( !cell.m_transforms.resize( write ) ||
						 ( cell.usesColors() && !cell.m_colors.resize( write ) ) || !cell.m_owners.resize( write ) ) {
						result["error"] = "Owner compaction failed";
						t3dInstancerClosePreparedPages( pData, stages );
						return result;
					}
					cell.compactOwnerSidecar();
					cell.touch();
					if ( write == 0u ) {
						t3dVectorEraseOrdered( pMesh->m_cells, ( size_t ) ci );
					}
				}
				if ( pMesh->m_cells.empty() ) {
					store.eraseMesh( mesh_id );
				}
			}
			break;
		}
	}
	int added = 0;
	for ( int i = 0; i < rBatches.size(); ++i ) {
		const Dictionary				batch = rBatches[i];
		const Vector2i					region = batch["region"];
		const uint32_t					owner = ( uint32_t ) ( ( int64_t ) batch["owner"] );
		const int						mesh_id = batch["mesh_id"];
		const TypedArray< Transform3D > transforms = batch.get( "transforms", TypedArray< Transform3D >() );
		if ( transforms.is_empty() ) {
			continue;
		}
		m_placements.clear();
		if ( !t3dContainerReserveCold( m_placements, ( size_t ) transforms.size() ) ) {
			result["error"] = "Placement staging allocation failed";
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
		for ( int k = 0; k < transforms.size(); ++k ) {
			Transform3D	   transform = transforms[k];
			const Vector2i actual_region = pData->getRegionLocation( transform.origin );
			if ( actual_region != region || !transform.origin.is_finite() ) {
				result["error"] = "Placement outside owning region";
				t3dInstancerClosePreparedPages( pData, stages );
				return result;
			}
			transform.origin.x -= ( real_t ) region.x * T3D_REGION_WORLD_SIZE_METERS;
			transform.origin.z -= ( real_t ) region.y * T3D_REGION_WORLD_SIZE_METERS;
			if ( !t3dVectorPushBackWithinCapacity( m_placements,
												   t3dPendingPlacement_t{ pData->getRegionPtr( region ),
																		  instancer->getCell( transform.origin ),
																		  transform,
																		  s_colorWhite,
																		  owner,
																		  ( uint16_t ) ( MANUAL_ORIENTATION ) } ) ) {
				result["error"] = "Placement queue capacity";
				t3dInstancerClosePreparedPages( pData, stages );
				return result;
			}
		}
		eastl::sort( m_placements.begin(), m_placements.end(), Terrain3DInstancer::pendingPlacementCellLess );
		for ( size_t j = 0; j < stages.size(); ++j ) {
			if ( stages[j].m_location != region ) {
				continue;
			}
			if ( !t3dInstancerAppendPlacementsToPreparedStore( stages[j].m_store,
															   mesh_id,
															   m_placements.data(),
															   m_placements.size() ) ) {
				result["error"] = "Insufficient instance storage";
				t3dInstancerClosePreparedPages( pData, stages );
				return result;
			}
			added += transforms.size();
			break;
		}
	}
	for ( size_t j = 0; j < stages.size(); ++j ) {
		if ( !stages[j].m_store.validateDeep() ) {
			result["error"] = "Staged instance store validation failed";
			t3dInstancerClosePreparedPages( pData, stages );
			return result;
		}
	}
	if ( !bPublish ) {
		Dictionary pages;
		Dictionary beforePages;
		for ( size_t j = 0; j < stages.size(); ++j ) {
			PackedByteArray page;
			PackedByteArray beforePage;
			uint32_t counts[T3D_MAX_MESH_ASSETS] = {};
			uint32_t count = 0u;
			uint16_t meshCount = 0u;
			if ( !Terrain3DInstancePageCodec::encode( stages[j].m_store, page, counts, count, meshCount ) ||
				 !Terrain3DInstancePageCodec::encode( *stages[j].m_pCommitStore, beforePage, counts, count, meshCount ) ) {
				result["error"] = "Cannot encode staged foliage instance pages";
				t3dInstancerClosePreparedPages( pData, stages );
				return result;
			}
			pages[stages[j].m_location] = page;
			beforePages[stages[j].m_location] = beforePage;
		}
		t3dInstancerClosePreparedPages( pData, stages );
		result["ok"] = true;
		result["error"] = "";
		result["pages"] = pages;
		result["before_pages"] = beforePages;
		result["instances"] = added;
		return result;
	}
	// No fallible allocations after this point; every affected store is ready to publish.
	for ( size_t j = 0; j < stages.size(); ++j ) {
		t3dPreparedRegionMutation_t &stage = stages[j];
		if ( bBackup ) {
			instancer->backupRegion( Ref< Terrain3DRegion >( stage.m_pRegion ) );
		} else {
			stage.m_pRegion->setModified( true );
		}
		*stage.m_pCommitStore = eastl::move( stage.m_store );
		stage.m_pRegion->markInstanceStoreModifiedInternal();
		t3dInstancerRefreshCommittedResidentBytes( pData, stage.m_location );
	}
	instancer->rebuildLogicalCountsFromStore();
	for ( size_t j = 0; j < stages.size(); ++j ) {
		t3dPreparedRegionMutation_t &stage = stages[j];
		if ( stage.m_bTemporaryPage && stage.m_pRegion->hasInstances() ) {
			( void ) t3dRetainMutatedInstancePage( pData, stage.m_pRegion, stage.m_bTemporaryPage );
		}
		instancer->updateMmis( -1, stage.m_location );
	}
	t3dInstancerClosePreparedPages( pData, stages );
	result["ok"] = true;
	result["error"] = "";
	result["regions"] = ( int ) stages.size();
	result["instances"] = added;
	return result;
}

Dictionary Terrain3DFoliageEditor::captureSources( Terrain3D *pTerrain, const TypedArray< Vector2i > &rRegions, int nHalo ) {
	Dictionary result;
	if (!pTerrain || !pTerrain->dataObject()) { return result; }
	const auto &repository = *pTerrain->dataObject()->getSamplingSourceInternal().m_pRepository;
	Dictionary revisions;
	nHalo = CLAMP( nHalo, 0, 8 );
	for ( int i = 0; i < rRegions.size(); ++i ) {
		const Vector2i location = rRegions[i];
		for ( int z = -nHalo; z <= nHalo; ++z ) {
			for ( int x = -nHalo; x <= nHalo; ++x ) {
				const Vector2i neighbor = location + Vector2i( x, z );
				if ( revisions.has( neighbor ) ) { continue; }
				const Terrain3DRegion *region = repository.getRegionPtr( neighbor );
				Array revision;
				revision.push_back( region && !region->isDeleted() ? (int64_t)region->get_instance_id() : (int64_t)0 );
				revision.push_back( region && !region->isDeleted() ? (int64_t)region->getAuthoringSourceRevision() : (int64_t)0 );
				revisions[neighbor] = revision;
			}
		}
	}
	result["topology"] = (int64_t)repository.getTopologyGeneration();
	result["regions"] = revisions;
	return result;
}

bool Terrain3DFoliageEditor::validateInstancePage( const PackedByteArray &rPage ) {
	uint32_t counts[T3D_MAX_MESH_ASSETS] = {};
	uint32_t count = 0u;
	uint16_t meshCount = 0u;
	return Terrain3DInstancePageCodec::inspect( rPage, counts, count, meshCount );
}

Error Terrain3DFoliageEditor::stageRegionInstances(
	Terrain3D *pTerrain, const Vector2i &rRegionLoc, const String &rDir, const String &rStagePath,
	const PackedByteArray &rPage, const int64_t nGeneration ) {
	if ( !pTerrain || !pTerrain->dataObject() || rDir.is_empty() || rStagePath.is_empty() || nGeneration < 0 || !pTerrain->dataObject()->hasRegion( rRegionLoc ) ) {
		return ERR_INVALID_PARAMETER;
	}
	const String sourcePath = rDir.path_join( Terrain3DUtil::locationToFilename( rRegionLoc ) );
	if ( sourcePath == rStagePath ) {
		return ERR_INVALID_PARAMETER;
	}
	uint32_t counts[T3D_MAX_MESH_ASSETS] = {};
	uint32_t count = 0u;
	uint16_t meshCount = 0u;
	if ( !Terrain3DInstancePageCodec::inspect( rPage, counts, count, meshCount ) ) {
		return ERR_INVALID_DATA;
	}
	// Require an existing authored raster baseline. Never publish unsaved height,
	// control, color, lighting, or nodes as a side effect of a foliage operation.
	const Ref< Terrain3DRegion > saved = ResourceLoader::get_singleton()->load(
		sourcePath, "Terrain3DRegion", ResourceLoader::CACHE_MODE_IGNORE );
	if ( saved.is_null() || saved->getHeightMap().is_null() || !saved->hasValidGeometryContractInternal() ) {
		return ERR_FILE_NOT_FOUND;
	}
	saved->setInstancePage( rPage );
	if ( saved->getInstancePage() != rPage ) {
		return ERR_INVALID_DATA;
	}
	saved->setInstanceGeneration( nGeneration );
	return ResourceSaver::get_singleton()->save( saved, rStagePath, ResourceSaver::FLAG_COMPRESS );
}

Dictionary Terrain3DFoliageEditor::getOwnerIds( const Ref<Terrain3DRegion> &rRegion ) {
	Dictionary result;
	const Terrain3DInstanceStore *store = nullptr;
	if (rRegion.is_null() || !rRegion->tryGetInstanceStore(store) || !store) { result["ok"] = false; return result; }
	Dictionary owners;
	for (size_t mi = 0; mi < store->meshes().size(); ++mi) {
		const t3dInstanceMesh_t &mesh = store->meshes()[mi];
		for (size_t ci = 0; ci < mesh.m_cells.size(); ++ci) {
			const t3dInstanceCell_s &cell = mesh.m_cells[ci];
			for (size_t i = 0; i < cell.m_owners.size(); ++i) {
				if (cell.m_owners[i] != 0u) { owners[(int64_t)cell.m_owners[i]] = true; }
			}
		}
	}
	result["ok"] = true; result["owners"] = owners;
	return result;
}

void Terrain3DFoliageEditor::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_terrain", "terrain"), &Terrain3DFoliageEditor::setTerrain);
    ClassDB::bind_method(D_METHOD("replace_owned_regions", "batches", "backup"), &Terrain3DFoliageEditor::replaceOwnedRegions, DEFVAL(true));
    ClassDB::bind_method(D_METHOD("prepare_owned_regions", "batches"), &Terrain3DFoliageEditor::prepareOwnedRegions);
    ClassDB::bind_method(D_METHOD("begin_restore", "id", "after", "before", "after_generations", "before_generations", "before_persistence", "expected_revisions"), &Terrain3DFoliageEditor::beginRestore);
    ClassDB::bind_method(D_METHOD("step_restore", "id", "pages", "bytes"), &Terrain3DFoliageEditor::stepRestore, DEFVAL(1), DEFVAL((int64_t)1048576));
    ClassDB::bind_method(D_METHOD("publish_restore", "id", "after"), &Terrain3DFoliageEditor::publishRestore);
    ClassDB::bind_method(D_METHOD("cancel_restore", "id"), &Terrain3DFoliageEditor::cancelRestore, DEFVAL(String()));
    ClassDB::bind_method(D_METHOD("restore_instance_pages", "pages", "generations", "persistence_states"), &Terrain3DFoliageEditor::restoreInstancePages);
    ClassDB::bind_static_method("Terrain3DFoliageEditor", D_METHOD("capture_sources", "terrain", "regions", "halo"), &Terrain3DFoliageEditor::captureSources, DEFVAL(4));
    ClassDB::bind_static_method("Terrain3DFoliageEditor", D_METHOD("source_stamp"), &Terrain3DFoliageEditor::sourceStamp);
    ClassDB::bind_static_method("Terrain3DFoliageEditor", D_METHOD("instance_revision", "region"), &Terrain3DFoliageEditor::instanceRevision);
    ClassDB::bind_static_method("Terrain3DFoliageEditor", D_METHOD("get_owner_ids", "region"), &Terrain3DFoliageEditor::getOwnerIds);
    ClassDB::bind_static_method("Terrain3DFoliageEditor", D_METHOD("validate_instance_page", "page"), &Terrain3DFoliageEditor::validateInstancePage);
    ClassDB::bind_static_method("Terrain3DFoliageEditor", D_METHOD("stage_region_instances", "terrain", "region_location", "directory", "stage_path", "page", "generation"), &Terrain3DFoliageEditor::stageRegionInstances);
}
#endif
