// Terrain3DData editor-only VT cooking and erosion operations.
#if defined( T3D_EDITOR_NATIVE )
#include "memory/policy/terrain_3d_memory_account.h"
#include "data/model/terrain_3d_data.h"
#include "terrain_3d.h"
#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_rendering_material.h"
#include "authoring/erosion/terrain_3d_erosion_job.h"
#include "authoring/erosion/terrain_3d_erosion_settings.h"
#include "authoring/vt/build/terrain_3d_incremental_megavt_cooker.h"
#include "authoring/vt/build/terrain_3d_vt_demand_cooker.h"
#include "authoring/vt/io/terrain_3d_vt_godot_cooker.h"
#include "data/persistence/terrain_3d_file_publication.h"
#include "core/utility/logger.h"
#include "rendering/material/terrain_3d_material.h"
#include "data/cooking/terrain_3d_control_map_cooker.h"
#include "data/sampling/terrain_3d_sampler.h"
#include "data/runtime/terrain_3d_runtime_bake_codec.h"
#include "physics/build/terrain_3d_collision_surface_builder.h"
#include "streaming/policy/terrain_3d_runtime_profile.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>

// Baked page encoding is an authoring operation; runtime reads packed pages.
bool Terrain3DData::encodeGpuRuntimePage(
	Terrain3DRegion *const pRegion,
	PackedByteArray		  &rBytes,
	String *const		   pError ) {
	if ( !pRegion || pRegion->isDeleted() || !m_pTerrain ) {
		if ( pError ) {
			*pError = "region/terrain owner is unavailable";
		}
		return false;
	}

	const int					 region_size = T3D_REGION_SIZE;
	const Vector2i				 location = pRegion->getLocation();
	const t3dRuntimePageConfig_s runtime_page_config = getRuntimePageConfig();
	// Collision and edge-derived cook data can touch the positive X/Z neighbor
	// border. Keep only that 2x2 source footprint resident while the page is built.
	if ( m_residency.cpuRegionCache().isEnabled() ) {
		for ( int dz = 0; dz <= 1; ++dz ) {
			for ( int dx = 0; dx <= 1; ++dx ) {
				const Vector2i		   neighbor = location + Vector2i( dx, dz );
				Terrain3DRegion *const pNeighbor = getRegionPtr( neighbor );
				if ( pNeighbor && !pNeighbor->isDeleted() && !ensureCpuRegionResidentInternal( neighbor, true ) ) {
					if ( pError ) {
						*pError =
							String( "collision bake neighbor could not be made CPU-resident: " ) + String( neighbor );
					}
					return false;
				}
			}
		}
	}

	t3dRuntimePageCookPayload_t payload;
	payload.m_height = pRegion->getHeightMap();
	const Ref< Image > authored_control = pRegion->getControlMap();
	if ( pRegion->hasChannel( TYPE_CONTROL ) ) {
		payload.m_control = makeGpuRenderControlImage( location );
	}
	if ( pRegion->hasChannel( TYPE_COLOR ) ) {
		payload.m_color = makeGpuColorImage( pRegion->getColorMap() );
	}
	payload.m_middleHeight =
		m_residency.middleHeightRegionCache().makeHeightImageForDivisor( location,
																		 runtime_page_config.m_nMiddleHeightDivisor );
	payload.m_derived = pRegion->getDerivedNormalSlopeMap();
	if ( payload.m_derived.is_null() ) {
		regenerateDerivedMaps( { location } );
		payload.m_derived = pRegion->getDerivedNormalSlopeMap();
	}
	payload.m_derived = makeGpuDerivedImage( payload.m_derived );
	payload.m_middleComposite = makeMiddleCompositeImage(
		location,
		payload.m_control.is_valid() ? payload.m_control : m_defaultControlMap,
		payload.m_color.is_valid() ? payload.m_color : makeGpuColorImage( m_defaultColorMap ) );
	const Ref< Terrain3DRegionLighting > lighting = pRegion->getLighting();
	if ( lighting.is_valid() && lighting->hasTerrainShadowMask() ) {
		// Regional lighting is an optional render enhancement. A stale or missing
		// shadow-mask sidecar must not make an otherwise valid terrain runtime page
		// uncookable. makeTerrainShadowMaskImage() normalizes valid masks to the
		// canonical region size and supplies a fully-lit L8 fallback when the sidecar
		// is absent or unreadable.
		const Ref< Image > shadow_mask = makeTerrainShadowMaskImage( pRegion );
		if ( shadow_mask.is_null() || shadow_mask->is_empty() || shadow_mask->get_format() != Image::FORMAT_L8 ||
			 !shadow_mask->has_mipmaps() || shadow_mask->get_width() != shadow_mask->get_height() ) {
			if ( pError ) {
				*pError = "regional terrain shadow-mask fallback could not be prepared";
			}
			return false;
		}
		payload.m_terrainShadowMask = shadow_mask;
	}
	payload.m_nMaterialMask =
		payload.m_control.is_valid() ? t3dControlCookMaterialMaskFromControlImage( payload.m_control ) : 1u;

	// Bake compact logical masks from authored control data, not the render halo.
	payload.m_holeMask = Terrain3DRuntimeBakeCodec::encodeHoleMask( authored_control );
	payload.m_minMaxHeight = Terrain3DRuntimeBakeCodec::encodeMinMaxHierarchy( payload.m_height );

	// Navigation classification uses the same positive border as collision so each
	// region page can generate all of its quads without consulting a neighbor page.
	const int		 collision_width = region_size + 1;
	PackedInt32Array navigation_controls;
	navigation_controls.resize( collision_width * collision_width );
	Terrain3DSampler sampler( getSamplingSourceInternal() );
	const int		 grid_x = location.x * region_size;
	const int		 grid_z = location.y * region_size;
	for ( int z = 0; z < collision_width; ++z ) {
		for ( int x = 0; x < collision_width; ++x ) {
			navigation_controls.set( z * collision_width + x,
									 ( int32_t ) sampler.sampleControlGrid( grid_x + x, grid_z + z ) );
		}
	}
	payload.m_navigation = Terrain3DRuntimeBakeCodec::encodeNavigationMask( navigation_controls );

	// Build the exact final physics surface through the same canonical path used
	// by live collision. This keeps holes and FLAT/NOISE world-background blend
	// semantics identical between cooked and sampler-backed tiles.
	//
	// build() itself never allocates. prepareRuntimeScratch() is the explicit cold
	// boundary that establishes retained oversized-axis backing before sampling.
	const int		   collision_sample_count = collision_width * collision_width;
	PackedFloat32Array collision_heights;
	collision_heights.resize( collision_sample_count );
	Terrain3DCollisionSurfaceBuilder collision_builder;
	if ( collision_heights.size() != collision_sample_count ||
		 !collision_builder.prepareRuntimeScratch( collision_width ) ) {
		if ( pError ) {
			*pError = String( "collision surface scratch could not be prepared for axis size " ) +
					  String::num_int64( collision_width );
		}
		return false;
	}
	float collision_min = 0.0f;
	float collision_max = 0.0f;
	if ( !collision_builder.build( this,
								   m_pTerrain->materialResource(),
								   Vector2i( grid_x, grid_z ),
								   region_size,
								   collision_heights,
								   collision_min,
								   collision_max ) ) {
		if ( pError ) {
			*pError = "canonical collision surface generation failed";
		}
		return false;
	}
	payload.m_collisionHeight = Terrain3DRuntimeBakeCodec::encodeCollisionHeight( collision_heights );
	payload.m_collisionBounds = Terrain3DRuntimeBakeCodec::encodeCollisionBounds( collision_min, collision_max );

	if ( pRegion->getStreamedNodeCount() > 0 ) {
		payload.m_nodeDescriptors =
			Terrain3DRuntimeBakeCodec::encodeNodeDescriptors( *pRegion, m_runtimeArtifacts.worldManifest() );
		if ( payload.m_nodeDescriptors.is_empty() ) {
			if ( pError ) {
				*pError = "streamed-node descriptor bake failed";
			}
			return false;
		}
	}
	if ( pRegion->hasInstances() ) {
		payload.m_instanceMetadata = Terrain3DRuntimeBakeCodec::encodeInstanceMetadata( *pRegion );
		payload.m_multimeshFastStart = Terrain3DRuntimeBakeCodec::encodeMultimeshFastStart( *pRegion );
	}
	payload.m_lightingReceivers = Terrain3DRuntimeBakeCodec::encodeLightingReceivers( *pRegion );

	if ( payload.m_holeMask.is_empty() || payload.m_navigation.is_empty() || payload.m_minMaxHeight.is_empty() ||
		 payload.m_collisionHeight.is_empty() || payload.m_collisionBounds.is_empty() ) {
		if ( pError ) {
			*pError = "mandatory baked runtime metadata generation failed";
		}
		return false;
	}
	return m_runtimeArtifacts.encodeBakedPage( pRegion, runtime_page_config, payload, rBytes, pError );
}

static void t3dDataAppendUniquePath(
	PackedStringArray &rPaths,
	const String	  &rPath ) {
	if ( rPath.is_empty() ) {
		return;
	}
	for ( int i = 0; i < rPaths.size(); ++i ) {
		if ( rPaths[i] == rPath ) {
			return;
		}
	}
	rPaths.push_back( rPath );
}

Dictionary Terrain3DData::cookGpuRegionPages(
	void ) {
	Terrain3DMemoryAccountScope memory_scope( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : NULL );
	Dictionary					result;
	PackedStringArray			source_files;
	PackedStringArray			errors;
	eastl::vector< Vector2i, Terrain3DEastlAllocator > locations;
	String											   data_dir;
	const bool preflight_ok = collectGpuRegionPageSources( data_dir, source_files, locations, errors );
	const int  discovered = source_files.size();
	const int  valid = ( int ) ( locations.size() );
	if ( !preflight_ok ) {
		m_residency.cpuRegionCache().releaseUntrackedMaps();
		result["data_directory"] = data_dir;
		result["far_atlas_cooked"] = false;
		result["preflight_failed"] = true;
		result["discovered"] = discovered;
		result["valid"] = valid;
		result["cooked"] = 0;
		result["unchanged"] = 0;
		result["usable"] = 0;
		result["skipped"] = 0;
		result["failed"] = errors.size();
		result["orphan_removed"] = 0;
		result["orphan_failed"] = 0;
		result["region_count"] = m_regionRepository.getActiveRegionCount();
		result["complete"] = false;
		result["errors"] = errors;
		return result;
	}

	if ( !m_runtimeArtifacts.buildAuthoringIndex() ) {
		errors.push_back( "World manifest dependency index could not be built before page cooking" );
		m_residency.cpuRegionCache().releaseUntrackedMaps();
		result["complete"] = false;
		result["errors"] = errors;
		return result;
	}

	const t3dRuntimePageConfig_s		  runtime_page_config = getRuntimePageConfig();
	const String						  streamPackPath = data_dir.path_join( "terrain_stream.t3sp" );
	eastl::vector< t3dBakedRegionPage_t > bakedPages;
	bakedPages.reserve( locations.size() );
	int cooked = 0;
	int failed = 0;
	for ( const Vector2i &location : locations ) {
		Terrain3DRegion *const region = getRegionPtr( location );
		if ( !region || region->isDeleted() ) {
			++failed;
			continue;
		}
		if ( m_residency.cpuRegionCache().isEnabled() && !ensureCpuRegionResidentInternal( location, true ) ) {
			++failed;
			errors.push_back( String( "Unable to make region source resident: " ) + String( location ) );
			continue;
		}
		t3dBakedRegionPage_t entry;
		entry.m_virtualPath = getGpuRuntimePagePath( region );
		String bakeError;
		if ( !encodeGpuRuntimePage( region, entry.m_encodedPage, &bakeError ) ) {
			++failed;
			errors.push_back( String( "Unable to encode baked terrain region " ) + String( location ) + String( ": " ) +
							  bakeError );
			continue;
		}
		bakedPages.push_back( entry );
		++cooked;
	}
	// Publication is all-or-nothing at the region coverage boundary.
	bool packReady = failed == 0 && cooked == valid && valid == discovered &&
					 Terrain3DSharedStreamPack::buildBakedPages( streamPackPath, bakedPages );
	if ( packReady ) {
		packReady = m_runtimeArtifacts.pageStore().setSharedSourcePackage( streamPackPath );
	}
	if ( !packReady ) {
		++failed;
		errors.push_back( "Authoritative terrain_stream.t3sp + .t3pm rebake/verification failed" );
	}
	const bool bFarAtlasCooked = writeCookedGpuFarAtlases();
	if ( !bFarAtlasCooked ) {
		errors.push_back( "Far atlas cook failed" );
	}
	bool		 bInstanceVtPackageCooked = true;
	uint32_t	 nInstanceVtPackageGeneration = 0u;
	uint32_t	 nInstanceVtMaterialCount = 0u;
	const String instance_vt_package_path = data_dir.path_join( "terrain3d_instances.t3dvt" );
	if ( m_pTerrain ) {
		const Ref< Terrain3DAssets > pAssets = m_pTerrain->assetsResource();
		if ( pAssets.is_valid() ) {
			// Only VT authoring is isolated; non-VT region cooking keeps its own account.
			Terrain3DMemoryAccountOwner vtExportAccount(
				t3dMemoryCookingLimit( m_pTerrain->getMemoryAccountInternal() ) );
			Terrain3DMemoryAccountScope vtAuthoringScope( vtExportAccount.get() );
			PackedByteArray				package_bytes;
			String						vt_error;
			if ( !t3d_vt_godot_cook_runtime_assets_package( pAssets,
															package_bytes,
															nInstanceVtPackageGeneration,
															nInstanceVtMaterialCount,
															&vt_error ) ||
				 !Terrain3DIncrementalMegaVTCooker::appendRuntimeRegionsCold(
					 { *this,
					   m_pTerrain->getDataDirectory(),
					   t3dMemoryCookingLimit( m_pTerrain->getMemoryAccountInternal() ),
					   m_pTerrain->getMegaVTTexelsPerMeterInternal() },
					 package_bytes,
					 nInstanceVtPackageGeneration,
					 vt_error ) ) {
				bInstanceVtPackageCooked = false;
				errors.push_back( String( "Runtime instance VT package cook failed: " ) +
								  ( vt_error.is_empty() ? String( "unknown error" ) : vt_error ) );
			} else if ( nInstanceVtMaterialCount == 0u && package_bytes.is_empty() ) {
				if ( FileAccess::file_exists( instance_vt_package_path ) ) {
					if ( DirAccess::remove_absolute( instance_vt_package_path ) != OK ) {
						bInstanceVtPackageCooked = false;
						errors.push_back( "Unable to remove obsolete runtime VT package" );
					}
				}
			} else if ( package_bytes.is_empty() ) {
				bInstanceVtPackageCooked = false;
				errors.push_back( "Runtime instance VT package cooker returned empty bytes for authored materials" );
			} else {
				String			publicationError;
				PackedByteArray demandBytes;
				String			demandPath;
                PackedByteArray v3Bytes;
                String v3Path;
				if ( !pAssets->get_path().is_empty() && !t3d_vt_demand_cook_asset_sidecar_cold( *this,
																								pAssets,
																								package_bytes,
																								demandBytes,
																								demandPath,
										                                                publicationError, &v3Bytes, &v3Path ) ) {
					bInstanceVtPackageCooked = false;
				} else {
					bInstanceVtPackageCooked = t3d_vt_demand_publish_triplet_cold( instance_vt_package_path,
                        package_bytes, demandPath, demandBytes, v3Path, v3Bytes, publicationError );
				}
				if ( !bInstanceVtPackageCooked ) {
					errors.push_back( publicationError );
				}
			}
		}
	}
	const bool bWorldManifestCooked = failed == 0 && packReady && bInstanceVtPackageCooked &&
									  m_runtimeArtifacts.writeWorldManifest( data_dir, runtime_page_config, &errors );
	if ( !bWorldManifestCooked ) {
		errors.push_back(
			failed == 0 ? String( "Cooked world manifest generation failed" )
						: String( "Cooked world manifest publication withheld because region-page cooking failed" ) );
	}
	PackedStringArray strip_safe_sources;
	PackedStringArray region_strip_sources;
	for ( size_t iFile = 0u; iFile < ( size_t ) ( ( source_files ).size() ); iFile++ ) {
		const String &source_file = ( source_files )[iFile];
		const String  path = data_dir.path_join( source_file );
		region_strip_sources.push_back( path );
		t3dDataAppendUniquePath( strip_safe_sources, path );
	}
	PackedStringArray material_strip_sources;
	if ( bInstanceVtPackageCooked && m_pTerrain ) {
		const Ref< Terrain3DAssets > pAssets = m_pTerrain->assetsResource();
		if ( pAssets.is_valid() && pAssets->usesPackedPbr() ) {
			const TypedArray< Terrain3DTextureAsset > textures = pAssets->getTextureList();
			for ( int index = 0; index < textures.size(); ++index ) {
				const Ref< Terrain3DTextureAsset > asset = textures[index];
				if ( asset.is_null() ) {
					continue;
				}
				const Ref< Texture2D > source_textures[] = { asset->getAlbedoTexture(),
															 asset->getNormalTexture(),
															 asset->getSurfaceTexture(),
															 asset->getDisplacementTexture(),
															 asset->getPbrTexture() };
				for ( size_t iTexture = 0u; iTexture < T3D_ARRAY_COUNT( source_textures ); iTexture++ ) {
					const Ref< Texture2D > &texture = ( source_textures )[iTexture];
					if ( texture.is_null() || texture->get_path().is_empty() ) {
						continue;
					}
					bool duplicate = false;
					for ( size_t iSource = 0u; iSource < ( size_t ) ( ( material_strip_sources ).size() ); iSource++ ) {
						const String &existing = ( material_strip_sources )[iSource];
						if ( existing == texture->get_path() ) {
							duplicate = true;
							break;
						}
					}
					if ( !duplicate ) {
						material_strip_sources.push_back( texture->get_path() );
					}
					t3dDataAppendUniquePath( strip_safe_sources, texture->get_path() );
				}
			}
		}
	}
	const String legacy_clipmap_pack_path = data_dir.path_join( "terrain3d_clipmap.t3dmeshpack" );
	if ( FileAccess::file_exists( legacy_clipmap_pack_path ) ) {
		DirAccess::remove_absolute( legacy_clipmap_pack_path );
	}
	DirAccess::remove_absolute( legacy_clipmap_pack_path + String( ".tmp" ) );
	DirAccess::remove_absolute( legacy_clipmap_pack_path + String( ".bak" ) );
	const String strip_manifest_path = data_dir.path_join( "terrain3d_runtime_sources.txt" );
	if ( bWorldManifestCooked ) {
		Ref< FileAccess > pStripFile = FileAccess::open( strip_manifest_path, FileAccess::WRITE );
		if ( pStripFile.is_valid() ) {
			pStripFile->store_line( "# Terrain3D cooked-runtime source-strip manifest" );
			pStripFile->store_line(
				"# Runtime authority: terrain3d_world.t3dmanifest + terrain_stream.t3sp + .t3pm baked terrain pages" );
			pStripFile->store_line( "[region_resources]" );
			for ( size_t iSource = 0u; iSource < ( size_t ) ( ( region_strip_sources ).size() ); iSource++ ) {
				const String &path = ( region_strip_sources )[iSource];
				pStripFile->store_line( path );
			}
			if ( !material_strip_sources.is_empty() ) {
				pStripFile->store_line( "[material_source_textures]" );
				for ( size_t iSource = 0u; iSource < ( size_t ) ( ( material_strip_sources ).size() ); iSource++ ) {
					const String &path = ( material_strip_sources )[iSource];
					pStripFile->store_line( path );
				}
			}
			pStripFile->store_line( "[runtime_artifact_authority]" );
			pStripFile->store_line( "instance_virtual_texture=terrain3d_instances.t3dvt" );
			pStripFile->store_line( "far_surface=terrain3d_far.t3dfar" );
		}
	}
	m_residency.cpuRegionCache().releaseUntrackedMaps();
	result["data_directory"] = data_dir;
	result["package_path"] = streamPackPath;
	result["package_metadata_path"] = streamPackPath + String( ".t3pm" );
	result["package_ready"] = packReady;
	result["far_atlas_cooked"] = bFarAtlasCooked;
	result["instance_vt_package_cooked"] = bInstanceVtPackageCooked;
	result["instance_vt_package_path"] = instance_vt_package_path;
	result["instance_vt_package_generation"] = ( int64_t ) nInstanceVtPackageGeneration;
	result["instance_vt_material_count"] = ( int64_t ) nInstanceVtMaterialCount;
	result["world_manifest_cooked"] = bWorldManifestCooked;
	result["world_manifest_path"] = m_runtimeArtifacts.worldManifest().getPath( data_dir );
	result["strip_safe_sources"] = strip_safe_sources;
	result["strip_manifest_path"] = strip_manifest_path;
	result["preflight_failed"] = false;
	result["discovered"] = discovered;
	result["valid"] = valid;
	result["cooked"] = cooked;
	result["unchanged"] = 0;
	result["usable"] = packReady ? cooked : 0;
	result["failed"] = failed;
	result["region_count"] = m_regionRepository.getActiveRegionCount();
	result["complete"] =
		packReady && failed == 0 && bFarAtlasCooked && bInstanceVtPackageCooked && bWorldManifestCooked;
	result["errors"] = errors;
	return result;
}

Ref< Terrain3DErosionJob > Terrain3DData::createErosionJob(
	const Ref< Terrain3DErosionSettings > &rSettings ) {
	Ref< Terrain3DErosionJob > pJob;
	pJob.instantiate();
	if ( pJob->setup( this, rSettings ) != OK ) {
		return pJob;
	}
	return pJob;
}

bool Terrain3DData::publishRuntimeVirtualTexturesCold(
	String						 &rOutputPath,
	uint32_t					 &rGeneration,
	uint32_t					 &rMaterialCount,
	String						 &rError,
	PackedByteArray *const		  pStagedPackage,
	Terrain3DMemoryAccount *const pAuthoringAccount ) {
	Terrain3DMemoryAccountOwner exportAccount(
		t3dMemoryCookingLimit( m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : NULL ) );
	Terrain3DMemoryAccountScope authoring_scope( pAuthoringAccount != NULL ? pAuthoringAccount : exportAccount.get() );
	if ( m_pTerrain == NULL || m_pTerrain->getDataDirectory().is_empty() ) {
		rError = "Set a terrain data directory before saving runtime virtual textures";
		return false;
	}
	rOutputPath = m_pTerrain->getDataDirectory().path_join( "terrain3d_instances.t3dvt" );
	PackedByteArray bytes;
	if ( !t3d_vt_godot_cook_runtime_assets_package( m_pTerrain->assetsResource(),
													bytes,
													rGeneration,
													rMaterialCount,
													&rError ) ||
		 !Terrain3DIncrementalMegaVTCooker::appendRuntimeRegionsCold(
			 { *this,
			   m_pTerrain->getDataDirectory(),
			   t3dMemoryCookingLimit( m_pTerrain->getMemoryAccountInternal() ),
			   m_pTerrain->getMegaVTTexelsPerMeterInternal() },
			 bytes,
			 rGeneration,
			 rError ) ) {
		return false;
	}
	if ( bytes.is_empty() ) {
		rError = "No authored virtual-texture sources to save";
		return false;
	}
	if ( pStagedPackage != NULL ) {
		*pStagedPackage = bytes;
		return true;
	}
	PackedByteArray				 demand;
	String						 demandPath, demandError;
    PackedByteArray v3Bytes;
    String v3Path;
	const Ref< Terrain3DAssets > assets = m_pTerrain->assetsResource();
	if ( assets.is_valid() && !assets->get_path().is_empty() ) {
		if ( !t3d_vt_demand_cook_asset_sidecar_cold( *this, assets, bytes, demand, demandPath,
            demandError, &v3Bytes, &v3Path ) ) {
			rError = "VT demand cook failed: " + demandError;
			return false;
		}
	}
	return t3d_vt_demand_publish_triplet_cold( rOutputPath, bytes, demandPath,
        demand, v3Path, v3Bytes, rError );
}
#endif // T3D_EDITOR_NATIVE
