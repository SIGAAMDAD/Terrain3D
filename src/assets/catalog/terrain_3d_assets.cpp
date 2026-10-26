#include "memory/policy/terrain_3d_memory_account.h"
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "integration/godot/terrain_3d_binding.h"
#include "core/utility/terrain_3d_generation.h"
#include "Terrain3DConstants.h"
#include "assets/catalog/terrain_3d_assets.h"

#include "core/utility/logger.h"
#include "core/utility/terrain_3d_owner.h"
#include "core/utility/terrain_3d_util.h"
#include "core/utility/terrain_3d_hash_bytes.h"
#include "eastl_config.h"
#include "integration/godot/terrain_3d_resource_persistence.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "rendering/material/terrain_3d_material.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>

// -----------------------------------------------------------------------------
// Private functions

static void t3dAssetsMaterialHashU64( uint64_t &rHash, const uint64_t nValue ) {
	rHash = t3dHashFnv1a64LittleU64( rHash, nValue );
}

void Terrain3DAssets::advanceTexturePublicationGeneration( const bool ) {
	m_nTexturePublicationGeneration = t3dNextGeneration( m_nTexturePublicationGeneration );
}

void Terrain3DAssets::advanceMeshSourceGeneration( void ) {
	m_nMeshSourceGeneration = t3dNextGeneration( m_nMeshSourceGeneration );
}

void Terrain3DAssets::advanceInstanceVtMaterialSourceGeneration( void ) {
	m_nInstanceVtMaterialSourceGeneration = t3dNextGeneration( m_nInstanceVtMaterialSourceGeneration );
}

template <typename T>
static void t3dAssetsAppendResourceSignature( uint64_t &rHash, const Ref<T> &rResource ) {
	if ( rResource.is_null() ) {
		t3dAssetsMaterialHashU64( rHash, 0u );
		return;
	}
	const String path = rResource->get_path();
	t3dAssetsMaterialHashU64( rHash, ( uint64_t )path.hash() );
	t3dAssetsMaterialHashU64( rHash, !path.is_empty() && FileAccess::file_exists( path ) ? FileAccess::get_modified_time( path ) : 0u );
}

static bool t3dAssetsIsNativePackedSurfaceFormat( const Image::Format format ) {
	// Terrain3D's optimized surface uses all four channels (normal XY, roughness, AO).
	// BC3 and BC7 preserve those channels and are directly usable by the sampler.
	return format == Image::FORMAT_RGBA8 || format == Image::FORMAT_DXT5 || format == Image::FORMAT_BPTC_RGBA;
}

static Image::Format t3dAssetsSelectPackedSurfaceArrayFormat( const Image::Format current, const Image::Format incoming ) {
	if ( current == incoming || incoming == Image::FORMAT_MAX ) {
		return current;
	}
	if ( current == Image::FORMAT_MAX ) {
		return incoming;
	}
	// Keep the array GPU-compressed when compatible packed-surface sources mix
	// RGBA8 with BC3/BC7. Prefer BC7 when any layer already supplies it, then
	// BC3, and use RGBA8 only when no compressed source exists.
	if ( current == Image::FORMAT_BPTC_RGBA || incoming == Image::FORMAT_BPTC_RGBA ) {
		return Image::FORMAT_BPTC_RGBA;
	}
	if ( current == Image::FORMAT_DXT5 || incoming == Image::FORMAT_DXT5 ) {
		return Image::FORMAT_DXT5;
	}
	return Image::FORMAT_RGBA8;
}

EA_FORCE_INLINE static bool t3dAssetsIsSupportedNativeArrayFormat( const Image::Format format ) {
	return format == Image::FORMAT_DXT1 || format == Image::FORMAT_DXT5 || format == Image::FORMAT_BPTC_RGBA
	        || format == Image::FORMAT_RGTC_R || format == Image::FORMAT_RGTC_RG;
}

static Error t3dAssetsCompressArrayImageToFormat( const Ref<Image> &rImage, const Image::Format format ) {
	if ( rImage.is_null() ) {
		return ERR_INVALID_PARAMETER;
	}
	switch ( format ) {
		case Image::FORMAT_DXT1:
			rImage->convert( Image::FORMAT_RGB8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RGB );
		case Image::FORMAT_DXT5:
			rImage->convert( Image::FORMAT_RGBA8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RGBA );
		case Image::FORMAT_RGTC_R:
			rImage->convert( Image::FORMAT_R8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_R );
		case Image::FORMAT_RGTC_RG:
			rImage->convert( Image::FORMAT_RG8 );
			return rImage->compress_from_channels( Image::COMPRESS_S3TC, Image::USED_CHANNELS_RG );
		case Image::FORMAT_BPTC_RGBA:
			rImage->convert( Image::FORMAT_RGBA8 );
			return rImage->compress( Image::COMPRESS_BPTC, Image::COMPRESS_SOURCE_GENERIC );
		default:
			return ERR_UNAVAILABLE;
	}
}

static Ref<Image> t3dAssetsMakeArrayDefault(
        const Vector2i &rSize, const Image::Format format, const bool bMipmaps, const Color &rDefaultColor ) {
	if ( !t3dAssetsIsSupportedNativeArrayFormat( format ) ) {
		return Terrain3DUtil::getFilledImage( rSize, rDefaultColor, bMipmaps, format );
	}
	Image::Format working_format = Image::FORMAT_RGBA8;
	if ( format == Image::FORMAT_DXT1 ) {
		working_format = Image::FORMAT_RGB8;
	} else if ( format == Image::FORMAT_RGTC_R ) {
		working_format = Image::FORMAT_R8;
	} else if ( format == Image::FORMAT_RGTC_RG ) {
		working_format = Image::FORMAT_RG8;
	}
	Ref<Image> pImage = Terrain3DUtil::getFilledImage( rSize, rDefaultColor, bMipmaps, working_format );
	if ( pImage.is_null() || t3dAssetsCompressArrayImageToFormat( pImage, format ) != OK || pImage->get_format() != format ) {
		return Ref<Image>();
	}
	return pImage;
}

static Ref<Image> t3dAssetsPrepareArrayImage( const Ref<Texture2D> &rTexture,
        const Vector2i &rSize,
        const Image::Format format,
        const bool bMipmaps,
        const Color &rDefaultColor ) {
	if ( rTexture.is_null() ) {
		return t3dAssetsMakeArrayDefault( rSize, format, bMipmaps, rDefaultColor );
	}
	Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		return Ref<Image>();
	}
	if ( pImage->get_size() == rSize && pImage->get_format() == format && pImage->has_mipmaps() == bMipmaps ) {
		// Preserve a native DDS/VRAM-compressed layer byte-for-byte. This is the
		// zero-expansion path used by production texture arrays.
		return pImage;
	}
	pImage = pImage->duplicate();
	if ( pImage->is_compressed() && pImage->decompress() != OK ) {
		return Ref<Image>();
	}
	if ( pImage->has_mipmaps() ) {
		pImage->clear_mipmaps();
	}
	if ( pImage->get_size() != rSize ) {
		pImage->resize( rSize.x, rSize.y, Image::INTERPOLATE_LANCZOS );
	}
	if ( t3dAssetsIsSupportedNativeArrayFormat( format ) ) {
		if ( bMipmaps && pImage->generate_mipmaps() != OK ) {
			return Ref<Image>();
		}
		if ( t3dAssetsCompressArrayImageToFormat( pImage, format ) != OK ) {
			return Ref<Image>();
		}
	} else {
		if ( pImage->get_format() != format ) {
			pImage->convert( format );
		}
		if ( bMipmaps && pImage->generate_mipmaps() != OK ) {
			return Ref<Image>();
		}
	}
	return pImage->get_size() == rSize && pImage->get_format() == format && pImage->has_mipmaps() == bMipmaps ? pImage : Ref<Image>();
}

EA_FORCE_INLINE static void t3dAssetsReleaseGeneratedReplacements( Terrain3DGeneratedTexture &rAlbedo,
        Terrain3DGeneratedTexture &rNormal,
        Terrain3DGeneratedTexture &rSurface,
        Terrain3DGeneratedTexture &rDisplacement ) {
	rAlbedo.clear();
	rNormal.clear();
	rSurface.clear();
	rDisplacement.clear();
}

uint64_t Terrain3DAssets::computeRuntimeMaterialSourceSignature( void ) const {
	uint64_t hash = 1469598103934665603ull;
	t3dAssetsMaterialHashU64( hash, ( uint64_t )( m_texturePackingMode ) );
	t3dAssetsMaterialHashU64( hash, ( uint64_t )( m_textureList.size() ) );
	for ( int index = 0; index < m_textureList.size(); ++index ) {
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[index];
		t3dAssetsMaterialHashU64( hash, ( uint64_t )( index ) );
		if ( pAsset.is_null() ) {
			continue;
		}
		t3dAssetsAppendResourceSignature( hash, pAsset->m_albedoTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_normalTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_surfaceTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_displacementTexture );
		t3dAssetsAppendResourceSignature( hash, pAsset->m_packedPbrResource );
	}
	return hash;
}

bool Terrain3DAssets::initializeFarMaterialArray( void ) {
	if ( !m_pTerrain || m_textureList.is_empty() || m_generatedAlbedoTextures.size() != m_textureList.size() ) {
		return false;
	}
	const int resolution = MAX( 1, m_pTerrain->getFarMaterialResolution() );
	TypedArray<Image> layers;
	layers.resize( m_textureList.size() );
	for ( int logical = 0; logical < m_textureList.size(); ++logical ) {
		Ref<Image> pImage = m_generatedAlbedoTextures.getLayerImage( logical );
		if ( pImage.is_null() || pImage->is_empty() ) {
			return false;
		}
		pImage = pImage->duplicate();
		if ( pImage->is_compressed() && pImage->decompress() != OK ) {
			return false;
		}
		if ( pImage->has_mipmaps() ) {
			pImage->clear_mipmaps();
		}
		if ( pImage->get_format() != Image::FORMAT_RGB8 ) {
			pImage->convert( Image::FORMAT_RGB8 );
		}
		if ( pImage->get_width() != resolution || pImage->get_height() != resolution ) {
			pImage->resize( resolution, resolution, Image::INTERPOLATE_LANCZOS );
		}
		if ( pImage->generate_mipmaps() != OK ) {
			return false;
		}
		layers[logical] = pImage;
	}
	Terrain3DGeneratedTexture replacement;
	if ( !replacement.create( layers, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid() ) {
		return false;
	}
	m_generatedFarAlbedoTextures.swap( replacement );
	replacement.clear();
	return true;
}

void Terrain3DAssets::connectAssetSignals( const t3dAssetType_t type, const Ref<Terrain3DAssetResource> &rAsset ) {
	if ( rAsset.is_null() ) {
		return;
	}
	const Callable id_callback = callable_mp( this, &Terrain3DAssets::swapIds );
	if ( !rAsset->is_connected( "id_changed", id_callback ) ) {
		rAsset->connect( "id_changed", id_callback );
	}
	if ( type == TYPE_TEXTURE ) {
		Ref<Terrain3DTextureAsset> pTexture = rAsset;
		if ( pTexture.is_null() ) {
			return;
		}
		const Callable file_callback = callable_mp( this, &Terrain3DAssets::updateTextureFiles );
		const Callable setting_callback = callable_mp( this, &Terrain3DAssets::updateTextureSettings );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateTexturePreview );
		if ( !pTexture->is_connected( "file_changed", file_callback ) ) {
			pTexture->connect( "file_changed", file_callback );
		}
		if ( !pTexture->is_connected( "setting_changed", setting_callback ) ) {
			pTexture->connect( "setting_changed", setting_callback );
		}
		if ( !pTexture->is_connected( "preview_changed", preview_callback ) ) {
			pTexture->connect( "preview_changed", preview_callback );
		}
	} else if ( type == TYPE_MESH ) {
		Ref<Terrain3DMeshAsset> pMesh = rAsset;
		if ( pMesh.is_null() ) {
			return;
		}
		const Callable settings_callback = callable_mp( this, &Terrain3DAssets::updateMesh );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateMeshPreview );
		const Callable vt_material_callback = callable_mp( this, &Terrain3DAssets::advanceInstanceVtMaterialSourceGeneration );
		if ( !pMesh->is_connected( "instancer_setting_changed", settings_callback ) ) {
			pMesh->connect( "instancer_setting_changed", settings_callback );
		}
		if ( !pMesh->is_connected( "preview_changed", preview_callback ) ) {
			pMesh->connect( "preview_changed", preview_callback );
		}
		if ( !pMesh->is_connected( "vt_material_changed", vt_material_callback ) ) {
			pMesh->connect( "vt_material_changed", vt_material_callback );
		}
	}
}

void Terrain3DAssets::disconnectAssetSignals( const t3dAssetType_t type, const Ref<Terrain3DAssetResource> &rAsset ) {
	if ( rAsset.is_null() ) {
		return;
	}
	const Callable id_callback = callable_mp( this, &Terrain3DAssets::swapIds );
	if ( rAsset->is_connected( "id_changed", id_callback ) ) {
		rAsset->disconnect( "id_changed", id_callback );
	}
	if ( type == TYPE_TEXTURE ) {
		Ref<Terrain3DTextureAsset> pTexture = rAsset;
		if ( pTexture.is_null() ) {
			return;
		}
		const Callable file_callback = callable_mp( this, &Terrain3DAssets::updateTextureFiles );
		const Callable setting_callback = callable_mp( this, &Terrain3DAssets::updateTextureSettings );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateTexturePreview );
		if ( pTexture->is_connected( "file_changed", file_callback ) ) {
			pTexture->disconnect( "file_changed", file_callback );
		}
		if ( pTexture->is_connected( "setting_changed", setting_callback ) ) {
			pTexture->disconnect( "setting_changed", setting_callback );
		}
		if ( pTexture->is_connected( "preview_changed", preview_callback ) ) {
			pTexture->disconnect( "preview_changed", preview_callback );
		}
	} else if ( type == TYPE_MESH ) {
		Ref<Terrain3DMeshAsset> pMesh = rAsset;
		if ( pMesh.is_null() ) {
			return;
		}
		const Callable settings_callback = callable_mp( this, &Terrain3DAssets::updateMesh );
		const Callable preview_callback = callable_mp( this, &Terrain3DAssets::updateMeshPreview );
		const Callable vt_material_callback = callable_mp( this, &Terrain3DAssets::advanceInstanceVtMaterialSourceGeneration );
		if ( pMesh->is_connected( "instancer_setting_changed", settings_callback ) ) {
			pMesh->disconnect( "instancer_setting_changed", settings_callback );
		}
		if ( pMesh->is_connected( "preview_changed", preview_callback ) ) {
			pMesh->disconnect( "preview_changed", preview_callback );
		}
		if ( pMesh->is_connected( "vt_material_changed", vt_material_callback ) ) {
			pMesh->disconnect( "vt_material_changed", vt_material_callback );
		}
	}
}

Ref<Terrain3DAssetResource> Terrain3DAssets::makePlaceholderAsset( const t3dAssetType_t type, const int nId ) const {
	Ref<Terrain3DAssetResource> pAsset;
	if ( type == TYPE_TEXTURE ) {
		Ref<Terrain3DTextureAsset> pTexture;
		pTexture.instantiate();
		pAsset = pTexture;
	} else if ( type == TYPE_MESH ) {
		Ref<Terrain3DMeshAsset> pMesh;
		pMesh.instantiate();
		pAsset = pMesh;
	}
	if ( pAsset.is_valid() ) {
		pAsset->m_nId = nId;
		pAsset->m_bPlaceholder = true;
	}
	return pAsset;
}

void Terrain3DAssets::swapIds( const t3dAssetType_t type, const int nSrcId, const int nDstId ) {
	m_bPersistenceDirty = true;
	LOG( INFO, "Swapping asset ID: ", nSrcId, " and ID: ", nDstId );
	Array list;
	switch ( type ) {
		case TYPE_TEXTURE:
			list = m_textureList;
			break;
		case TYPE_MESH:
			list = m_meshList;
			break;
		default:
			return;
	}
	if ( nSrcId < 0 || nSrcId >= list.size() ) {
		LOG( ERROR, "Source ID out of range: ", nSrcId );
		return;
	}
	Ref<Terrain3DAssetResource> pResSrc = list[nSrcId];
	if ( pResSrc.is_null() ) {
		LOG( ERROR, "Source Asset is null at ID: ", nSrcId );
		return;
	}
	// User can type in any value, -1 means swap with first, >size means swap with last
	int dst_id = CLAMP( nDstId, 0, list.size() - 1 );
	if ( dst_id == nSrcId ) {
		// They're likely the same due to the clamp, when new ID is <0 or >= array_size)
		pResSrc->m_nId = nSrcId;
		return;
	}
	Ref<Terrain3DAssetResource> pResDst = list[dst_id];
	if ( pResDst.is_valid() ) {
		pResDst->m_nId = nSrcId;
	}
	pResSrc->m_nId = dst_id;
	list[dst_id] = pResSrc;
	list[nSrcId] = pResDst;

	switch ( type ) {
		case TYPE_TEXTURE:
			updateTextureList();
			break;
		case TYPE_MESH:
			if ( m_pTerrain ) {
				m_pTerrain->instancerObject()->swapIds( nSrcId, dst_id );
			} else {
				LOG( ERROR, "Changing IDs before the terrain is initialized. Meshes on the ground may be wrong." );
			}
			updateMeshList();
			break;
		default:
			return;
	}
}

/**
 * _set_asset_list attempts to keep the asset ID as saved in the resource file.
 * But if an ID is invalid or already taken, the new ID is changed to the next available one
 */
void Terrain3DAssets::setAssetList( const t3dAssetType_t type, const TypedArray<Terrain3DAssetResource> &rList ) {
	m_bPersistenceDirty = true;
	const int max_size = type == TYPE_TEXTURE ? T3D_MAX_TEXTURE_ASSETS : type == TYPE_MESH ? T3D_MAX_MESH_ASSETS : 0;
	if ( max_size == 0 ) {
		return;
	}

	// Build a fresh fixed-slot registry. Reusing the old Array makes replacement
	// depend on its previous occupants and can silently mix old and new assets.
	const int array_size = CLAMP( rList.size(), 0, max_size );
	Array replacement;
	replacement.resize( array_size );
	eastl::bitset<T3D_MAX_MESH_ASSETS> occupied;
	int next_free = 0;
	for ( int i = 0; i < array_size; ++i ) {
		Ref<Terrain3DAssetResource> pAsset = rList[i];
		if ( pAsset.is_null() ) {
			continue;
		}
		int target_id = pAsset->getId();
		if ( target_id < 0 || target_id >= array_size || occupied.test( ( size_t )( target_id ) ) ) {
			while ( next_free < array_size && occupied.test( ( size_t )( next_free ) ) ) {
				++next_free;
			}
			if ( next_free >= array_size ) {
				LOG( ERROR, "No free slot for asset: ", pAsset->get_name() );
				continue;
			}
			LOG( WARN, pAsset->get_class(), " ID ", target_id, " is invalid or duplicated. Assigning slot ", next_free );
			target_id = next_free;
		}
		pAsset->m_nId = target_id;
		pAsset->m_bPlaceholder = false;
		replacement[target_id] = pAsset;
		occupied.set( ( size_t )( target_id ) );
	}

	// Holes are explicit placeholder resources. Texture/control IDs and mesh IDs
	// are stable slot identifiers and therefore cannot be compacted.
	for ( int slot = 0; slot < array_size; ++slot ) {
		if ( !occupied.test( ( size_t )( slot ) ) ) {
			replacement[slot] = makePlaceholderAsset( type, slot );
		}
	}

	if ( type == TYPE_TEXTURE ) {
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pOldAsset = ( m_textureList )[iTexture];
			disconnectAssetSignals( TYPE_TEXTURE, pOldAsset );
		}
		m_textureList.clear();
		m_textureList.resize( array_size );
		for ( int i = 0; i < array_size; ++i ) {
			Ref<Terrain3DTextureAsset> pAsset = replacement[i];
			m_textureList[i] = pAsset;
			connectAssetSignals( TYPE_TEXTURE, pAsset );
			if ( occupied.test( ( size_t )( i ) ) ) {
				pAsset->initialize();
			}
		}
	} else {
		for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
			const Ref<Terrain3DMeshAsset> pOldAsset = ( m_meshList )[iMesh];
			disconnectAssetSignals( TYPE_MESH, pOldAsset );
		}
		m_meshList.clear();
		m_meshList.resize( array_size );
		for ( int i = 0; i < array_size; ++i ) {
			Ref<Terrain3DMeshAsset> pAsset = replacement[i];
			m_meshList[i] = pAsset;
			connectAssetSignals( TYPE_MESH, pAsset );
			if ( occupied.test( ( size_t )( i ) ) ) {
				pAsset->initialize();
			}
		}
		advanceInstanceVtMaterialSourceGeneration();
	}
}

void Terrain3DAssets::setAsset( const t3dAssetType_t type, const int nId, const Ref<Terrain3DAssetResource> &rAsset ) {
	m_bPersistenceDirty = true;
	LOG( INFO, "Setting asset type: ", type, ", ID: ", nId, ", asset: ", rAsset );
	const int max_size = type == TYPE_TEXTURE ? T3D_MAX_TEXTURE_ASSETS : type == TYPE_MESH ? T3D_MAX_MESH_ASSETS : 0;
	if ( max_size == 0 || nId < 0 || nId >= max_size ) {
		LOG( ERROR, "Invalid asset id: ", nId, " range is 0-", max_size - 1 );
		return;
	}
	Array list = type == TYPE_TEXTURE ? Array( m_textureList ) : Array( m_meshList );
	if ( rAsset.is_null() && nId >= list.size() ) {
		return;
	}
	if ( nId >= list.size() ) {
		const int old_size = list.size();
		list.resize( nId + 1 );
		for ( int slot = old_size; slot < nId; ++slot ) {
			const Ref<Terrain3DAssetResource> placeholder = makePlaceholderAsset( type, slot );
			list[slot] = placeholder;
			connectAssetSignals( type, placeholder );
		}
	}

	const Ref<Terrain3DAssetResource> pOldAsset = list[nId];
	if ( rAsset.is_null() ) {
		// Texture IDs are persisted in control-map pixels. Removing the final
		// array entry cannot compact the registry without remapping every painted
		// pixel, so texture deletion always leaves a placeholder layer. Mesh IDs
		// may trim the tail only after set_mesh_asset() has cleared all instances.
		const bool bPreserveStableSlot = type == TYPE_TEXTURE || nId < list.size() - 1;
		disconnectAssetSignals( type, pOldAsset );
		if ( bPreserveStableSlot ) {
			const Ref<Terrain3DAssetResource> placeholder = makePlaceholderAsset( type, nId );
			list[nId] = placeholder;
			connectAssetSignals( type, placeholder );
		} else {
			list.pop_back();
		}
	} else {
		if ( pOldAsset != rAsset ) {
			disconnectAssetSignals( type, pOldAsset );
		}
		rAsset->m_nId = nId;
		rAsset->m_bPlaceholder = false;
		list[nId] = rAsset;
		connectAssetSignals( type, rAsset );
		rAsset->initialize();
	}

	if ( type == TYPE_TEXTURE ) {
		m_textureList = list;
	} else {
		m_meshList = list;
		advanceInstanceVtMaterialSourceGeneration();
	}
}

void Terrain3DAssets::validateArrayMember( const Ref<Texture2D> &rTexture,
        const Ref<Terrain3DTextureAsset> &rAsset,
        const String &rLabel,
        t3dTextureArrayLayout_t &rLayout,
        const Image::Format eRequiredFormat,
        const bool bAllowCompressedSurface ) {
	if ( rTexture.is_null() ) {
		return;
	}
	const Ref<Image> pImage = rTexture->get_image();
	if ( pImage.is_null() || pImage->is_empty() ) {
		m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
		LOG( ERROR, "Texture ID ", rAsset->getId(), " ", rLabel, " image is invalid or empty." );
		return;
	}
	const Vector2i vTextureSize = pImage->get_size();
	const Image::Format eFormat = pImage->get_format();
	const bool bMipmaps = pImage->has_mipmaps();
	const bool bAcceptedSurfaceCompression =
	        bAllowCompressedSurface && eRequiredFormat == Image::FORMAT_RGBA8 && t3dAssetsIsNativePackedSurfaceFormat( eFormat );
	if ( eRequiredFormat != Image::FORMAT_MAX && eFormat != eRequiredFormat && !bAcceptedSurfaceCompression ) {
		m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
		LOG( ERROR,
		        "Texture ID ",
		        rAsset->getId(),
		        " ",
		        rLabel,
		        " must use format ",
		        ( int )( eRequiredFormat ),
		        " or a supported native compressed equivalent, received ",
		        ( int )( eFormat ),
		        "." );
		return;
	}
	if ( rLayout.m_eFormat == Image::FORMAT_MAX ) {
		rLayout.m_vSize = vTextureSize;
		rLayout.m_eFormat = eFormat;
		rLayout.m_bMipmaps = bMipmaps;
		return;
	}
	if ( vTextureSize != rLayout.m_vSize ) {
		rLayout.m_bNeedsNormalization = true;
		rLayout.m_vSize.x = MAX( rLayout.m_vSize.x, vTextureSize.x );
		rLayout.m_vSize.y = MAX( rLayout.m_vSize.y, vTextureSize.y );
		LOG( WARN,
		        "Texture ID ",
		        rAsset->getId(),
		        " ",
		        rLabel,
		        " size ",
		        vTextureSize,
		        " differs from another layer in the same array and will be normalized to ",
		        rLayout.m_vSize,
		        "." );
	}
	if ( eFormat != rLayout.m_eFormat ) {
		rLayout.m_bNeedsNormalization = true;
		if ( bAllowCompressedSurface && t3dAssetsIsNativePackedSurfaceFormat( rLayout.m_eFormat )
		        && t3dAssetsIsNativePackedSurfaceFormat( eFormat ) ) {
			rLayout.m_eFormat = t3dAssetsSelectPackedSurfaceArrayFormat( rLayout.m_eFormat, eFormat );
		} else {
			LOG( WARN,
			        "Texture ID ",
			        rAsset->getId(),
			        " ",
			        rLabel,
			        " format ",
			        ( int )( eFormat ),
			        " differs from another layer in the same array; the array will be normalized instead of rejected." );
		}
	}
	if ( bMipmaps != rLayout.m_bMipmaps ) {
		rLayout.m_bNeedsNormalization = true;
		rLayout.m_bMipmaps = rLayout.m_bMipmaps || bMipmaps;
		LOG( WARN,
		        "Texture ID ",
		        rAsset->getId(),
		        " ",
		        rLabel,
		        " mipmap layout differs from another layer in the same array and will be normalized." );
	}
}

void Terrain3DAssets::updateTextureFiles( void ) {
	Terrain3DMemoryAccountScope memory_scope(m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr);
	m_nTerrainVtContentGeneration = t3dNextGeneration( m_nTerrainVtContentGeneration );
	advanceTexturePublicationGeneration( false );
	m_bPersistenceDirty = true;
	IS_INIT( VOID );
	LOG( DEBUG, "Received texture_changed signal" );
	if ( m_textureList.is_empty() ) {
		m_generatedAlbedoTextures.clear();
		m_generatedNormalTextures.clear();
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
		m_generatedFarAlbedoTextures.clear();
		if ( !m_bSuppressTextureChanged ) {
			emit_signal( "textures_changed" );
		}
		return;
	}

	const bool bPackedPbr = usesPackedPbr();
	const bool bUseDisplacement = bPackedPbr && m_pTerrain->getTessellationLevel() > 0;
	t3dTextureArrayLayout_t albedoLayout;
	t3dTextureArrayLayout_t secondaryLayout;
	t3dTextureArrayLayout_t displacementLayout;
	m_pTerrain->setWarning( WARN_ALL, false );

	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_null() ) {
			continue;
		}
		validateArrayMember( pAsset->m_albedoTexture, pAsset, "albedo", albedoLayout );
		if ( bPackedPbr ) {
			const Ref<Texture2D> pSurface = pAsset->getEffectiveSurfaceTexture();
			if ( pAsset->m_packedPbrResource.is_valid() && pSurface.is_null() ) {
				m_pTerrain->setWarning( WARN_MISMATCHED_FORMAT, true );
				LOG( ERROR,
				        "Texture ID ",
				        pAsset->getId(),
				        " packed-PBR/displacement compatibility resource failed to build: ",
				        pAsset->m_packedPbrResource->getLastError() );
			}
			validateArrayMember( pSurface, pAsset, "surface", secondaryLayout, Image::FORMAT_RGBA8, true );
			if ( bUseDisplacement ) {
				const Ref<Texture2D> pDisplacement = pAsset->getEffectiveDisplacementTexture();
				validateArrayMember( pDisplacement, pAsset, "R8 displacement", displacementLayout, Image::FORMAT_R8 );
			}
		} else {
			validateArrayMember( pAsset->m_normalTexture, pAsset, "normal", secondaryLayout );
		}
	}
	if ( m_pTerrain->getWarnings() ) {
		return;
	}

	if ( bPackedPbr ) {
		if ( albedoLayout.m_vSize == s_v2iZero && secondaryLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = V2I( 1024 );
			secondaryLayout.m_vSize = V2I( 512 );
		} else if ( albedoLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = secondaryLayout.m_vSize * 2;
		} else if ( secondaryLayout.m_vSize == s_v2iZero ) {
			secondaryLayout.m_vSize = Vector2i( MAX( albedoLayout.m_vSize.x / 2, 1 ), MAX( albedoLayout.m_vSize.y / 2, 1 ) );
		}
		if ( albedoLayout.m_eFormat == Image::FORMAT_MAX ) {
			albedoLayout.m_eFormat = Image::FORMAT_RGB8;
		}
		if ( secondaryLayout.m_eFormat == Image::FORMAT_MAX ) {
			secondaryLayout.m_eFormat = Image::FORMAT_RGBA8;
		}
		if ( displacementLayout.m_vSize == s_v2iZero ) {
			displacementLayout.m_vSize = secondaryLayout.m_vSize;
		}
		displacementLayout.m_eFormat = Image::FORMAT_R8;
	} else {
		if ( secondaryLayout.m_vSize == s_v2iZero ) {
			secondaryLayout.m_vSize = albedoLayout.m_vSize;
		} else if ( albedoLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = secondaryLayout.m_vSize;
		}
		if ( albedoLayout.m_vSize == s_v2iZero ) {
			albedoLayout.m_vSize = V2I( 1024 );
			secondaryLayout.m_vSize = albedoLayout.m_vSize;
		}
	}

	// GPU texture arrays require a uniform layout only among their own layers.
	// Albedo, packed surface/PBR, normal, and displacement are separate arrays and
	// deliberately keep independent dimensions and formats. Preserve native packed
	// surface compression across size/mipmap normalization and transcode mixed
	// RGBA8/BC3/BC7 surface sources to one compatible compressed array format.
	if ( albedoLayout.m_bNeedsNormalization ) {
		albedoLayout.m_eFormat = Image::FORMAT_RGBA8;
	}
	if ( secondaryLayout.m_bNeedsNormalization && ( !bPackedPbr || !t3dAssetsIsNativePackedSurfaceFormat( secondaryLayout.m_eFormat ) ) ) {
		secondaryLayout.m_eFormat = Image::FORMAT_RGBA8;
	}
	if ( displacementLayout.m_bNeedsNormalization ) {
		displacementLayout.m_eFormat = Image::FORMAT_R8;
	}

	Terrain3DGeneratedTexture replacement_albedo;
	Terrain3DGeneratedTexture replacement_normal;
	Terrain3DGeneratedTexture replacement_surface;
	Terrain3DGeneratedTexture replacement_displacement;
	bool bReplacementOk = true;

	if ( albedoLayout.m_vSize != s_v2iZero ) {
		TypedArray<Image> layers;
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->m_albedoTexture : Ref<Texture2D>();
			const Ref<Image> pImage = t3dAssetsPrepareArrayImage(
			        pTexture, albedoLayout.m_vSize, albedoLayout.m_eFormat, albedoLayout.m_bMipmaps, COLOR_CHECKED );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( ERROR, "Failed to normalize albedo texture layer ", layers.size(), "." );
				break;
			}
			layers.push_back( pImage );
		}
		if ( bReplacementOk && !layers.is_empty() ) {
			bReplacementOk = replacement_albedo.create( layers, T3D_RD_TEXTURE_SOURCE_COLOR ).is_valid();
		}
	}

	if ( bReplacementOk && bPackedPbr && secondaryLayout.m_vSize != s_v2iZero ) {
		TypedArray<Image> surface_layers;
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getEffectiveSurfaceTexture() : Ref<Texture2D>();
			const Ref<Image> pImage = t3dAssetsPrepareArrayImage( pTexture,
			        secondaryLayout.m_vSize,
			        secondaryLayout.m_eFormat,
			        secondaryLayout.m_bMipmaps,
			        Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( ERROR, "Failed to normalize packed surface texture layer ", surface_layers.size(), "." );
				break;
			}
			surface_layers.push_back( pImage );
		}
		if ( bReplacementOk && !surface_layers.is_empty() ) {
			bReplacementOk = replacement_surface.create( surface_layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
		}

		if ( bReplacementOk && bUseDisplacement ) {
			TypedArray<Image> displacement_layers;
			for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
				const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
				const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->getEffectiveDisplacementTexture() : Ref<Texture2D>();
				const Ref<Image> pImage = t3dAssetsPrepareArrayImage( pTexture,
				        displacementLayout.m_vSize,
				        displacementLayout.m_eFormat,
				        displacementLayout.m_bMipmaps,
				        Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
				if ( pImage.is_null() ) {
					bReplacementOk = false;
					LOG( ERROR, "Failed to normalize displacement texture layer ", displacement_layers.size(), "." );
					break;
				}
				displacement_layers.push_back( pImage );
			}
			if ( bReplacementOk && !displacement_layers.is_empty() ) {
				bReplacementOk = replacement_displacement.create( displacement_layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
			}
		}
	} else if ( bReplacementOk && !bPackedPbr && secondaryLayout.m_vSize != s_v2iZero ) {
		TypedArray<Image> layers;
		for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
			const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
			const Ref<Texture2D> pTexture = pAsset.is_valid() ? pAsset->m_normalTexture : Ref<Texture2D>();
			const Ref<Image> pImage = t3dAssetsPrepareArrayImage(
			        pTexture, secondaryLayout.m_vSize, secondaryLayout.m_eFormat, secondaryLayout.m_bMipmaps, COLOR_NORMAL );
			if ( pImage.is_null() ) {
				bReplacementOk = false;
				LOG( ERROR, "Failed to normalize normal texture layer ", layers.size(), "." );
				break;
			}
			layers.push_back( pImage );
		}
		if ( bReplacementOk && !layers.is_empty() ) {
			bReplacementOk = replacement_normal.create( layers, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
		}
	}

	if ( !bReplacementOk ) {
		t3dAssetsReleaseGeneratedReplacements( replacement_albedo, replacement_normal, replacement_surface, replacement_displacement );
		LOG( ERROR, "Terrain texture-array rebuild failed. Keeping the last complete generated texture set." );
		return;
	}

	m_generatedAlbedoTextures.swap( replacement_albedo );
	if ( bPackedPbr ) {
		m_generatedSurfaceTextures.swap( replacement_surface );
		m_generatedNormalTextures.clear();
		if ( bUseDisplacement ) {
			m_generatedDisplacementTextures.swap( replacement_displacement );
		} else {
			m_generatedDisplacementTextures.clear();
		}
	} else {
		m_generatedNormalTextures.swap( replacement_normal );
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
	}

	// Preserve the asset-level placeholder behavior of the original pipeline, but only
	// after a complete replacement set has been committed. This keeps failed rebuilds
	// transactional while editor previews and serialized asset state still see defaults.
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_null() ) {
			continue;
		}
		if ( pAsset->m_albedoTexture.is_null() ) {
			const Ref<Image> pImage =
			        t3dAssetsMakeArrayDefault( albedoLayout.m_vSize, albedoLayout.m_eFormat, albedoLayout.m_bMipmaps, COLOR_CHECKED );
			if ( pImage.is_valid() ) {
				pAsset->m_albedoTexture = ImageTexture::create_from_image( pImage );
			}
		}
		if ( bPackedPbr ) {
			if ( pAsset->m_packedPbrResource.is_null() && pAsset->m_surfaceTexture.is_null() ) {
				const Ref<Image> pImage = t3dAssetsMakeArrayDefault(
				        secondaryLayout.m_vSize, secondaryLayout.m_eFormat, secondaryLayout.m_bMipmaps, Color( 0.5f, 0.5f, 1.0f, 1.0f ) );
				if ( pImage.is_valid() ) {
					pAsset->m_surfaceTexture = ImageTexture::create_from_image( pImage );
				}
			}
			if ( bUseDisplacement && pAsset->m_packedPbrResource.is_null() && pAsset->m_displacementTexture.is_null() ) {
				const Ref<Image> pImage = t3dAssetsMakeArrayDefault( displacementLayout.m_vSize,
				        displacementLayout.m_eFormat,
				        displacementLayout.m_bMipmaps,
				        Color( 0.5f, 0.0f, 0.0f, 1.0f ) );
				if ( pImage.is_valid() ) {
					pAsset->m_displacementTexture = ImageTexture::create_from_image( pImage );
				}
			}
		} else if ( pAsset->m_normalTexture.is_null() ) {
			const Ref<Image> pImage = t3dAssetsMakeArrayDefault(
			        secondaryLayout.m_vSize, secondaryLayout.m_eFormat, secondaryLayout.m_bMipmaps, COLOR_NORMAL );
			if ( pImage.is_valid() ) {
				pAsset->m_normalTexture = ImageTexture::create_from_image( pImage );
			}
		}
	}

	t3dAssetsReleaseGeneratedReplacements( replacement_albedo, replacement_normal, replacement_surface, replacement_displacement );
	if ( !initializeFarMaterialArray() ) {
		m_generatedFarAlbedoTextures.clear();
	}

	if ( !m_bSuppressTextureChanged ) {
		emit_signal( "textures_changed" );
	}
}

void Terrain3DAssets::updateTextureSettingsInternal( const bool bPersistentChange ) {
	Terrain3DMemoryAccountScope memory_scope(m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr);
	advanceTexturePublicationGeneration( false );
	if ( bPersistentChange ) {
		m_bPersistenceDirty = true;
	}
	LOG( DEBUG, "Received setting_changed signal" );
	m_textureSettings.m_nCount = MIN( m_textureList.size(), T3D_MAX_TEXTURE_ASSETS );
	for ( int index = 0; index < m_textureSettings.m_nCount; ++index ) {
		const Ref<Terrain3DTextureAsset> pTa = m_textureList[index];
		if ( pTa.is_null() ) {
			m_textureSettings.m_szColors[( size_t )( index )] = s_colorWhite;
			m_textureSettings.m_szNormalDepths[( size_t )( index )] = 1.0f;
			m_textureSettings.m_szAoStrengths[( size_t )( index )] = 0.5f;
			m_textureSettings.m_szAoLightAffects[( size_t )( index )] = 0.0f;
			m_textureSettings.m_szRoughnessMods[( size_t )( index )] = 0.0f;
			m_textureSettings.m_szMetallics[( size_t )( index )] = 0.0f;
			m_textureSettings.m_szUvScales[( size_t )( index )] = 0.1f;
			m_textureSettings.m_szDetiles[( size_t )( index )] = Vector2();
			m_textureSettings.m_szDisplacements[( size_t )( index )] = Vector2();
			continue;
		}

		m_textureSettings.m_szColors[( size_t )( index )] = pTa->isHighlighted() ? pTa->getHighlightColor() : pTa->getAlbedoColor();
		m_textureSettings.m_szNormalDepths[( size_t )( index )] = pTa->getNormalDepth();
		m_textureSettings.m_szAoStrengths[( size_t )( index )] = pTa->getAoStrength();
		m_textureSettings.m_szAoLightAffects[( size_t )( index )] = pTa->getAoLightAffect();
		m_textureSettings.m_szRoughnessMods[( size_t )( index )] = pTa->getRoughness();
		m_textureSettings.m_szMetallics[( size_t )( index )] = pTa->getMetallic();
		m_textureSettings.m_szUvScales[( size_t )( index )] = pTa->getUvScale();
		m_textureSettings.m_szDetiles[( size_t )( index )] = Vector2( pTa->getDetilingRotation(), pTa->getDetilingShift() );
		m_textureSettings.m_szDisplacements[( size_t )( index )] = Vector2( pTa->getDisplacementOffset(), pTa->getDisplacementScale() );
	}

	const int count = m_textureSettings.m_nCount;
	m_textureColors.resize( count );
	m_textureNormalDepths.resize( count );
	m_textureAoStrengths.resize( count );
	m_textureAoLightAffects.resize( count );
	m_textureRoughnessMods.resize( count );
	m_textureMetallics.resize( count );
	m_textureUvScales.resize( count );
	m_textureDetiles.resize( count );
	m_textureDisplacements.resize( count );
	if ( count > 0 ) {
		memcpy( m_textureColors.ptrw(), m_textureSettings.m_szColors, sizeof( m_textureSettings.m_szColors[0] ) * ( size_t )( count ) );
		memcpy( m_textureNormalDepths.ptrw(),
		        m_textureSettings.m_szNormalDepths,
		        sizeof( m_textureSettings.m_szNormalDepths[0] ) * ( size_t )( count ) );
		memcpy( m_textureAoStrengths.ptrw(),
		        m_textureSettings.m_szAoStrengths,
		        sizeof( m_textureSettings.m_szAoStrengths[0] ) * ( size_t )( count ) );
		memcpy( m_textureAoLightAffects.ptrw(),
		        m_textureSettings.m_szAoLightAffects,
		        sizeof( m_textureSettings.m_szAoLightAffects[0] ) * ( size_t )( count ) );
		memcpy( m_textureRoughnessMods.ptrw(),
		        m_textureSettings.m_szRoughnessMods,
		        sizeof( m_textureSettings.m_szRoughnessMods[0] ) * ( size_t )( count ) );
		memcpy( m_textureMetallics.ptrw(),
		        m_textureSettings.m_szMetallics,
		        sizeof( m_textureSettings.m_szMetallics[0] ) * ( size_t )( count ) );
		memcpy( m_textureUvScales.ptrw(),
		        m_textureSettings.m_szUvScales,
		        sizeof( m_textureSettings.m_szUvScales[0] ) * ( size_t )( count ) );
		memcpy( m_textureDetiles.ptrw(), m_textureSettings.m_szDetiles, sizeof( m_textureSettings.m_szDetiles[0] ) * ( size_t )( count ) );
		memcpy( m_textureDisplacements.ptrw(),
		        m_textureSettings.m_szDisplacements,
		        sizeof( m_textureSettings.m_szDisplacements[0] ) * ( size_t )( count ) );
	}

	LOG( DEBUG, "Emitting textures_changed" );
	if ( !m_bSuppressTextureChanged ) {
		emit_signal( "textures_changed" );
	}
}

void Terrain3DAssets::updateTextureSettings( void ) {
	updateTextureSettingsInternal( true );
}

void Terrain3DAssets::updateTexturePreview( void ) {
	updateTextureSettingsInternal( false );
}

void Terrain3DAssets::updateMeshInternal( const int nId, const bool bPersistentChange ) {
	Terrain3DMemoryAccountScope memory_scope(m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr);
	advanceMeshSourceGeneration();
	if ( bPersistentChange ) {
		m_bPersistenceDirty = true;
	}
	if ( nId < 0 || nId >= m_meshList.size() ) {
		LOG( ERROR, "Invalid mesh ID: ", nId );
		return;
	}
	if ( m_pTerrain ) {
		m_pTerrain->onInstanceAssetPolicyChanged( nId );
	}
	if ( !m_pTerrain || !m_pTerrain->instancerObject() ) { return; }
	Terrain3DInstancer *const pInstancer = m_pTerrain->instancerObject();
	pInstancer->updateMmis( nId, s_v2iMax, false );
}

void Terrain3DAssets::updateMesh( const int nId ) {
	updateMeshInternal( nId, true );
}

void Terrain3DAssets::updateMeshPreview( const int nId ) {
	updateMeshInternal( nId, false );
}

// -----------------------------------------------------------------------------
// Public functions

void Terrain3DAssets::setTexturePackingMode( const t3dTexturePackingMode_t eMode ) {
	const t3dTexturePackingMode_t mode = ( t3dTexturePackingMode_t )( CLAMP(
	        ( int )( eMode ), ( int )( TEXTURE_PACKING_LEGACY ), ( int )( TEXTURE_PACKING_PACKED_PBR_R8 ) ) );
	if ( m_texturePackingMode == mode ) {
		return;
	}
	m_texturePackingMode = mode;
	m_bPersistenceDirty = true;
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_valid() ) {
			pAsset->setPackedPbrMode( usesPackedPbr() );
		}
	}
	m_generatedAlbedoTextures.clear();
	m_generatedNormalTextures.clear();
	m_generatedSurfaceTextures.clear();
	m_generatedDisplacementTextures.clear();
	m_generatedFarAlbedoTextures.clear();
	LOG( INFO, "Setting texture packing mode: ", ( int )( m_texturePackingMode ) );
	if ( m_pTerrain ) {
		updateTextureList();
		Ref<Terrain3DMaterial> pMaterial = m_pTerrain->materialResource();
		if ( pMaterial.is_valid() && pMaterial->isInitialized() ) {
			pMaterial->update( FULL_REBUILD );
		}
	}
	emit_changed();
}

void Terrain3DAssets::initialize( Terrain3DOwner *pTerrain ) {
	if ( pTerrain ) {
		Terrain3DOwner *previous_owner = t3dTransferOwner( m_pTerrain, pTerrain );
		if ( previous_owner && previous_owner != pTerrain ) {
			LOG( INFO, "Transferring asset runtime ownership from ", t3dOwnerDiagnosticIdentity( previous_owner ), " to ", t3dOwnerDiagnosticIdentity( pTerrain ) );
		}
	} else {
		LOG( ERROR, "Initialization failed, p_terrain is null" );
		return;
	}
	LOG( INFO, "Initializing assets" );

	const bool v3_runtime_only = usesPackedPbr() && m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode();
	if ( v3_runtime_only ) {
		// V3 owns terrain material residency in cooked runtime. Keep only scalar
		// material state here; do not duplicate the logical material set in arrays.
		for ( size_t iTexture = 0u; iTexture < ( size_t )( m_textureList.size() ); ++iTexture ) {
			const Ref<Terrain3DTextureAsset> pAsset = m_textureList[iTexture];
			if ( pAsset.is_valid() ) {
				pAsset->setPackedPbrMode( true );
				connectAssetSignals( TYPE_TEXTURE, pAsset );
			}
		}
		m_generatedAlbedoTextures.clear();
		m_generatedNormalTextures.clear();
		m_generatedSurfaceTextures.clear();
		m_generatedDisplacementTextures.clear();
		m_generatedFarAlbedoTextures.clear();
		updateTextureSettings();
	} else {
		// Editor and legacy/non-VT modes retain static arrays as a compatibility
		// path. They are no longer a streamed runtime residency system.
		updateTextureList();
	}
	updateMeshList();
	m_bPersistenceDirty = false;
}

void Terrain3DAssets::uninitialize( Terrain3DOwner *pTerrain ) {
	if ( !t3dReleaseOwner( m_pTerrain, pTerrain ) ) {
		LOG( DEBUG, "Ignoring stale asset uninitialize request from ", t3dOwnerDiagnosticIdentity( pTerrain ), "; current owner: ", t3dOwnerDiagnosticIdentity( m_pTerrain ) );
		return;
	}
	LOG( INFO, "Uninitializing assets" );
}

void Terrain3DAssets::destroy( void ) {
	LOG( INFO, "Destroying assets" );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		disconnectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
		const Ref<Terrain3DMeshAsset> pAsset = ( m_meshList )[iMesh];
		disconnectAssetSignals( TYPE_MESH, pAsset );
	}
	m_pTerrain = NULL;
	m_generatedAlbedoTextures.clear();
	m_generatedNormalTextures.clear();
	m_generatedSurfaceTextures.clear();
	m_generatedDisplacementTextures.clear();
	m_generatedFarAlbedoTextures.clear();
	m_textureList.clear();
	m_meshList.clear();
	m_textureColors.clear();
	m_textureNormalDepths.clear();
	m_textureAoStrengths.clear();
	m_textureAoLightAffects.clear();
	m_textureRoughnessMods.clear();
	m_textureMetallics.clear();
	m_textureUvScales.clear();
	m_textureDetiles.clear();
	m_textureDisplacements.clear();
}

// Called when creating a new asset
void Terrain3DAssets::setTextureAsset( const int nId, const Ref<Terrain3DTextureAsset> &rTexture ) {
	if ( nId < 0 || nId >= T3D_MAX_TEXTURE_ASSETS ) {
		LOG( ERROR, "Invalid texture id: ", nId, " range is 0-", T3D_MAX_TEXTURE_ASSETS - 1 );
		return;
	}
	if ( nId < m_textureList.size() && m_textureList[nId] == rTexture ) {
		return;
	}
	LOG( INFO, "Setting texture id: ", nId );
	setAsset( TYPE_TEXTURE, nId, rTexture );
	updateTextureList();
}

// Called when loading a list of assets from an assets resource file on disk
TypedArray<Terrain3DTextureAsset> Terrain3DAssets::getTextureList( void ) const {
	TypedArray<Terrain3DTextureAsset> result;
	result.resize( m_textureList.size() );
	for ( int i = 0; i < m_textureList.size(); ++i ) {
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[i];
		if ( pAsset.is_valid() && !pAsset->m_bPlaceholder ) {
			result[i] = pAsset;
		}
	}
	return result;
}

void Terrain3DAssets::setTextureList( const TypedArray<Terrain3DTextureAsset> &rTextureList ) {
	LOG( INFO, "Setting texture list with ", rTextureList.size(), " entries" );
	if ( !t3dDiffers( getTextureList(), rTextureList ) ) {
		return;
	}
	setAssetList( TYPE_TEXTURE, rTextureList );
	updateTextureList();
}

void Terrain3DAssets::clearTextures( const bool bUpdate ) {
	m_bPersistenceDirty = true;
	LOG( INFO, "Clearing texture list" );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		disconnectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	m_textureList.clear();
	if ( bUpdate ) {
		updateTextureList();
	}
}

void Terrain3DAssets::updateTextureList( void ) {
	LOG( INFO, "Refreshing texture registry" );
	for ( size_t iTexture = 0u; iTexture < ( size_t )( ( m_textureList ).size() ); iTexture++ ) {
		const Ref<Terrain3DTextureAsset> pAsset = ( m_textureList )[iTexture];
		if ( pAsset.is_valid() ) {
			pAsset->setPackedPbrMode( usesPackedPbr() );
		}
		connectAssetSignals( TYPE_TEXTURE, pAsset );
	}
	m_bSuppressTextureChanged = true;
	updateTextureFiles();
	updateTextureSettings();
	m_bSuppressTextureChanged = false;
	emit_signal( "textures_changed" );
}

// Called when creating a new asset
void Terrain3DAssets::setMeshAsset( const int nId, const Ref<Terrain3DMeshAsset> &rMeshAsset ) {
	if ( nId < 0 || nId >= T3D_MAX_MESH_ASSETS ) {
		LOG( ERROR, "Invalid mesh id: ", nId, " range is 0-", T3D_MAX_MESH_ASSETS - 1 );
		return;
	}
	if ( nId < m_meshList.size() && m_meshList[nId] == rMeshAsset ) {
		return;
	}
	LOG( INFO, "Setting mesh id: ", nId, ", ", rMeshAsset );
	if ( rMeshAsset.is_null() ) {
		if ( !m_pTerrain || !m_pTerrain->instancerObject() ) { return; }
		m_pTerrain->instancerObject()->clearByMesh( nId );
	}
	setAsset( TYPE_MESH, nId, rMeshAsset );
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->instancerObject() ) {
		m_pTerrain->instancerObject()->updateMmis( nId, s_v2iMax, false );
	}
}

// Called when loading a list of assets from an assets resource file on disk
TypedArray<Terrain3DMeshAsset> Terrain3DAssets::getMeshList( void ) const {
	TypedArray<Terrain3DMeshAsset> result;
	result.resize( m_meshList.size() );
	for ( int i = 0; i < m_meshList.size(); ++i ) {
		const Ref<Terrain3DMeshAsset> pAsset = m_meshList[i];
		if ( pAsset.is_valid() && !pAsset->m_bPlaceholder ) {
			result[i] = pAsset;
		}
	}
	return result;
}

void Terrain3DAssets::setMeshList( const TypedArray<Terrain3DMeshAsset> &rMeshList ) {
	LOG( INFO, "Setting mesh list with ", rMeshList.size(), " entries" );
	if ( !t3dDiffers( getMeshList(), rMeshList ) ) {
		return;
	}
	setAssetList( TYPE_MESH, rMeshList );
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->instancerObject() ) {
		m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, true );
	}
}

bool Terrain3DAssets::setMeshAssetsBatch( const Dictionary &rAssetsById ) {
	if ( rAssetsById.is_empty() ) {
		return true;
	}

	const Array keys = rAssetsById.keys();
	eastl::bitset<T3D_MAX_MESH_ASSETS> touched;
	int required_size = m_meshList.size();
	for ( int i = 0; i < keys.size(); ++i ) {
		const Variant key = keys[i];
		if ( key.get_type() != Variant::INT ) {
			LOG( ERROR, "Mesh batch contains a non-integer slot key" );
			return false;
		}
		const int id = ( int )( key );
		if ( id < 0 || id >= T3D_MAX_MESH_ASSETS || touched.test( ( size_t )( id ) ) ) {
			LOG( ERROR, "Mesh batch contains an invalid or duplicated slot: ", id );
			return false;
		}
		const Ref<Terrain3DMeshAsset> asset = rAssetsById[key];
		if ( asset.is_null() ) {
			LOG( ERROR, "Mesh batch slot ", id, " contains a null asset" );
			return false;
		}
		if ( id < m_meshList.size() ) {
			const Ref<Terrain3DMeshAsset> existing = m_meshList[id];
			if ( existing.is_valid() && !existing->m_bPlaceholder && existing != asset ) {
				LOG( ERROR, "Mesh batch refuses to replace occupied slot ", id, "; slot-preserving insertion requires a free slot" );
				return false;
			}
		}
		touched.set( ( size_t )( id ) );
		required_size = MAX( required_size, id + 1 );
	}

	// Build the complete target registry off to the side, then publish it once.
	// No callback can observe a partially-installed batch, and unrelated slots are
	// never reinterpreted through their embedded resource IDs.
	Array replacement = m_meshList;
	const int old_size = replacement.size();
	replacement.resize( required_size );
	for ( int slot = old_size; slot < required_size; ++slot ) {
		if ( !touched.test( ( size_t )( slot ) ) ) {
			replacement[slot] = makePlaceholderAsset( TYPE_MESH, slot );
		}
	}
	for ( int i = 0; i < keys.size(); ++i ) {
		const int id = ( int )( keys[i] );
		const Ref<Terrain3DMeshAsset> asset = rAssetsById[keys[i]];
		asset->m_nId = id;
		asset->m_bPlaceholder = false;
		replacement[id] = asset;
	}

	for ( int i = 0; i < keys.size(); ++i ) {
		const int id = ( int )( keys[i] );
		if ( id < m_meshList.size() ) {
			disconnectAssetSignals( TYPE_MESH, m_meshList[id] );
		}
	}
	m_meshList = replacement;
	for ( int slot = old_size; slot < required_size; ++slot ) {
		if ( !touched.test( ( size_t )( slot ) ) ) {
			connectAssetSignals( TYPE_MESH, m_meshList[slot] );
		}
	}
	for ( int i = 0; i < keys.size(); ++i ) {
		const int id = ( int )( keys[i] );
		const Ref<Terrain3DMeshAsset> asset = m_meshList[id];
		connectAssetSignals( TYPE_MESH, asset );
		asset->initialize();
	}

	m_bPersistenceDirty = true;
	updateMeshList();
	if ( m_pTerrain && m_pTerrain->instancerObject() ) {
		m_pTerrain->instancerObject()->updateMmis( -1, s_v2iMax, true );
	}
	return true;
}

// p_id = -1 for all meshes
// Adapted from godot\editor\plugins\editor_preview_plugins.cpp:EditorMeshPreviewPlugin
void Terrain3DAssets::createMeshThumbnails( const int nId, const Vector2i &rSize, const bool bForce ) {
	Terrain3DMemoryAccountScope memory_scope(m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr);
	if ( !IS_EDITOR || !m_pTerrain ) {
		return;
	}
	// Mesh previews render into preview-owned RD targets and use async readback.
	// Terrain-region residency is unrelated and may remain continuously active in
	// authoring mode, so it must not gate or starve thumbnail generation.
	const int max = getMeshCount();
	if ( nId < -1 || nId >= max || max <= 0 ) {
		return;
	}
	const int start_id = nId < 0 ? 0 : CLAMP( nId, 0, max - 1 );
	const int end_id = nId < 0 ? max : CLAMP( nId + 1, 0, max );
	m_nThumbnailBatchCount = 0;
	m_iThumbnailBatchCursor = 0;
	m_thumbnailBatchSize = CLAMP( rSize, V2I( 2 ), V2I( 4096 ) );
	m_nThumbnailBatchDeferredRetries = 0u;
	m_nThumbnailBatchGeneration = t3dNextGeneration( m_nThumbnailBatchGeneration );
	for ( int i = start_id; i < end_id && m_nThumbnailBatchCount < T3D_MAX_MESH_ASSETS; ++i ) {
		const Ref<Terrain3DMeshAsset> asset = getMeshAsset( i );
		if ( asset.is_null() || asset->getMesh( 0 ).is_null() ) {
			continue;
		}
		if ( !bForce && asset->getThumbnail().is_valid() ) {
			continue;
		}
		m_szThumbnailBatchIds[m_nThumbnailBatchCount++] = i;
	}
	m_bThumbnailBatchActive = m_nThumbnailBatchCount > 0;
	if ( m_bThumbnailBatchActive ) {
		LOG( INFO, "Queueing async RD mesh thumbnails for ", m_nThumbnailBatchCount, " assets" );
		serviceMeshThumbnailAsync();
	}
}

void Terrain3DAssets::serviceMeshThumbnailAsync( void ) {
	Terrain3DMemoryAccountScope memory_scope(m_pTerrain ? m_pTerrain->getMemoryAccountInternal() : nullptr);
	if ( !m_bThumbnailBatchActive || !m_pTerrain ) {
		return;
	}
	while ( m_iThumbnailBatchCursor < m_nThumbnailBatchCount ) {
		const int nAssetId = m_szThumbnailBatchIds[m_iThumbnailBatchCursor];
		const Ref<Terrain3DMeshAsset> asset = getMeshAsset( nAssetId );
		if ( asset.is_null() || asset->getMesh( 0 ).is_null() ) {
			++m_iThumbnailBatchCursor;
			continue;
		}
		t3dRdPreviewFailure_t failure = T3D_RD_PREVIEW_NONE;
		if ( m_pTerrain->requestRdMeshPreviewAsync(
		             ( uint16_t )nAssetId, m_thumbnailBatchSize, ( uint64_t )get_instance_id(), m_nThumbnailBatchGeneration, &failure ) ) {
			m_nThumbnailBatchDeferredRetries = 0u;
			return;
		}
		if ( failure == T3D_RD_PREVIEW_ASSET_SOURCE ) {
			LOG( WARN, nAssetId, ": Invalid mesh preview source, skipping thumbnail" );
			++m_iThumbnailBatchCursor;
			continue;
		}
		if ( failure == T3D_RD_PREVIEW_TRANSIENT_DEFERRED && m_nThumbnailBatchDeferredRetries < 120u ) {
			++m_nThumbnailBatchDeferredRetries;
			call_deferred( StringName( "_service_mesh_thumbnail_async" ) );
			return;
		}
		LOG( WARN, "Aborting async mesh thumbnail batch after shared RD preview failure at asset ", nAssetId );
		m_bThumbnailBatchActive = false;
		return;
	}
	m_bThumbnailBatchActive = false;
}

void Terrain3DAssets::completeMeshThumbnailAsync( const int64_t nAssetId,
        const Ref<Image> &rImage,
        const int64_t nFailure,
        const int64_t nBatchGeneration,
        const int64_t nMeshSourceGeneration,
        const int64_t nAssetObjectId ) {
	if ( !m_bThumbnailBatchActive || ( uint64_t )nBatchGeneration != m_nThumbnailBatchGeneration
	        || m_iThumbnailBatchCursor >= m_nThumbnailBatchCount || m_szThumbnailBatchIds[m_iThumbnailBatchCursor] != ( int )nAssetId ) {
		return;
	}
	const Ref<Terrain3DMeshAsset> currentAsset = getMeshAsset( ( int )nAssetId );
	if ( ( uint64_t )nMeshSourceGeneration != getMeshSourceGeneration() || currentAsset.is_null()
	        || ( uint64_t )currentAsset->get_instance_id() != ( uint64_t )nAssetObjectId ) {
		// The GPU card was rendered from an obsolete mesh/material identity. Never
		// publish it into a new asset occupying the same dense ID. Restart just this
		// asset against the current source generation.
		m_bThumbnailBatchActive = false;
		createMeshThumbnails( ( int )nAssetId, m_thumbnailBatchSize, true );
		return;
	}
	const t3dRdPreviewFailure_t failure = ( t3dRdPreviewFailure_t )nFailure;
	if ( failure == T3D_RD_PREVIEW_TRANSIENT_DEFERRED ) {
		if ( m_nThumbnailBatchDeferredRetries++ < 120u ) {
			call_deferred( StringName( "_service_mesh_thumbnail_async" ) );
		} else {
			m_bThumbnailBatchActive = false;
		}
		return;
	}
	m_nThumbnailBatchDeferredRetries = 0u;
	if ( failure == T3D_RD_PREVIEW_NONE && rImage.is_valid() ) {
		const Ref<Terrain3DMeshAsset> asset = getMeshAsset( ( int )nAssetId );
		if ( asset.is_valid() ) {
			asset->setThumbnail( ImageTexture::create_from_image( rImage ) );
		}
	} else if ( failure == T3D_RD_PREVIEW_ASSET_SOURCE ) {
		LOG( WARN, nAssetId, ": Invalid async mesh preview source, skipping thumbnail" );
	} else {
		LOG( WARN, "Aborting async mesh thumbnail batch after shared RD preview failure at asset ", nAssetId );
		m_bThumbnailBatchActive = false;
		return;
	}
	++m_iThumbnailBatchCursor;
	call_deferred( StringName( "_service_mesh_thumbnail_async" ) );
}

void Terrain3DAssets::updateMeshList( void ) {
	m_bPersistenceDirty = true;
	advanceMeshSourceGeneration();
	advanceInstanceVtMaterialSourceGeneration();
	if ( !m_pTerrain || !m_pTerrain->instancerObject() ) { return; }
	LOG( INFO, "Updating mesh registry" );
	if ( m_meshList.is_empty() ) {
		LOG( DEBUG, "Mesh list empty, clearing instancer and adding a default mesh" );
		m_pTerrain->instancerObject()->destroy();
		Ref<Terrain3DMeshAsset> pNewAsset;
		pNewAsset.instantiate();
		setMeshAsset( 0, pNewAsset );
		return;
	}
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
		const Ref<Terrain3DMeshAsset> pAsset = ( m_meshList )[iMesh];
		connectAssetSignals( TYPE_MESH, pAsset );
	}
	emit_signal( "meshes_changed" );
}

void Terrain3DAssets::loadPendingMeshes( void ) {
	// Reload meshes for all mesh assets that have changed. Used by the instancer after clearing.
	// A deferred commit changes the actual RD mesh source just as surely as an
	// inspector/preview edit does. The old path swapped m_pendingMeshes into
	// m_meshes without advancing the mesh-source generation, so the coordinator
	// kept its startup mesh snapshot until some later forced asset update.
	Terrain3DInstancer *const pInstancer = m_pTerrain ? m_pTerrain->instancerObject() : NULL;
	bool bCommittedAny = false;
	for ( size_t iMesh = 0u; iMesh < ( size_t )( ( m_meshList ).size() ); iMesh++ ) {
		const Ref<Terrain3DMeshAsset> pMa = ( m_meshList )[iMesh];
		if ( pMa.is_valid() && pMa->isSceneFilePending() ) {
			pMa->commitMeshes();
			bCommittedAny = true;
			// The render-cell LOD/settings image may also have been built from the
			// pre-commit mesh set. Requeue only this asset after the swap so the
			// instance scene and mesh-source snapshot converge together.
			if ( pInstancer ) {
				pInstancer->updateMmis( ( int )( iMesh ), s_v2iMax, false );
			}
		}
	}
	if ( bCommittedAny ) {
		advanceMeshSourceGeneration();
	}
}

Error Terrain3DAssets::save( const String &rPath ) {
	return t3dSaveExternalResource( this, rPath, m_bPersistenceDirty, []() {} );
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DAssets::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( TYPE_TEXTURE );
	BIND_ENUM_CONSTANT( TYPE_MESH );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_LEGACY );
	BIND_ENUM_CONSTANT( TEXTURE_PACKING_PACKED_PBR_R8 );
	BIND_CONSTANT( MAX_TEXTURE_ASSETS );
	BIND_CONSTANT( MAX_MESH_ASSETS );

	ClassDB::bind_method( D_METHOD( "set_texture_asset", "id", "texture" ), &Terrain3DAssets::setTextureAsset );
	ClassDB::bind_method( D_METHOD( "get_texture_asset", "id" ), &Terrain3DAssets::getTextureAsset );
	ClassDB::bind_method( D_METHOD( "get_texture_count" ), &Terrain3DAssets::getTextureCount );
	ClassDB::bind_method( D_METHOD( "get_albedo_array_rid" ), &Terrain3DAssets::getAlbedoArrayRid );
	ClassDB::bind_method( D_METHOD( "get_normal_array_rid" ), &Terrain3DAssets::getNormalArrayRid );
	ClassDB::bind_method( D_METHOD( "get_surface_array_rid" ), &Terrain3DAssets::getSurfaceArrayRid );
	ClassDB::bind_method( D_METHOD( "get_displacement_array_rid" ), &Terrain3DAssets::getDisplacementArrayRid );
	ClassDB::bind_method( D_METHOD( "get_packed_pbr_mipmap_count" ), &Terrain3DAssets::getPackedPbrMipmapCount );
	ClassDB::bind_method( D_METHOD( "get_texture_colors" ), &Terrain3DAssets::getTextureColors );
	ClassDB::bind_method( D_METHOD( "get_texture_normal_depths" ), &Terrain3DAssets::getTextureNormalDepths );
	ClassDB::bind_method( D_METHOD( "get_texture_ao_strengths" ), &Terrain3DAssets::getTextureAoStrengths );
	ClassDB::bind_method( D_METHOD( "get_texture_ao_light_affects" ), &Terrain3DAssets::getTextureAoLightAffects );
	ClassDB::bind_method( D_METHOD( "get_texture_roughness_mods" ), &Terrain3DAssets::getTextureRoughnessMods );
	ClassDB::bind_method( D_METHOD( "get_texture_metallics" ), &Terrain3DAssets::getTextureMetallics );
	ClassDB::bind_method( D_METHOD( "get_texture_uv_scales" ), &Terrain3DAssets::getTextureUvScales );
	ClassDB::bind_method( D_METHOD( "get_texture_detiles" ), &Terrain3DAssets::getTextureDetiles );
	ClassDB::bind_method( D_METHOD( "get_texture_displacements" ), &Terrain3DAssets::getTextureDisplacements );
	ClassDB::bind_method( D_METHOD( "clear_textures", "update" ), &Terrain3DAssets::clearTextures, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "update_texture_list" ), &Terrain3DAssets::updateTextureList );

	ClassDB::bind_method( D_METHOD( "set_mesh_asset", "id", "mesh" ), &Terrain3DAssets::setMeshAsset );
	ClassDB::bind_method( D_METHOD( "get_mesh_asset", "id" ), &Terrain3DAssets::getMeshAsset );
	ClassDB::bind_method( D_METHOD( "set_mesh_list", "mesh_list" ), &Terrain3DAssets::setMeshList );
	ClassDB::bind_method( D_METHOD( "set_mesh_assets_batch", "assets_by_id" ), &Terrain3DAssets::setMeshAssetsBatch );
	ClassDB::bind_method( D_METHOD( "get_mesh_list" ), &Terrain3DAssets::getMeshList );
	ClassDB::bind_method( D_METHOD( "get_mesh_count" ), &Terrain3DAssets::getMeshCount );
	ClassDB::bind_method( D_METHOD( "create_mesh_thumbnails", "id", "size", "force" ),
	        &Terrain3DAssets::createMeshThumbnails,
	        DEFVAL( -1 ),
	        DEFVAL( V2I( 256 ) ),
	        DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "_service_mesh_thumbnail_async" ), &Terrain3DAssets::serviceMeshThumbnailAsync );
	ClassDB::bind_method( D_METHOD( "_complete_mesh_thumbnail_async",
	                              "asset_id",
	                              "image",
	                              "failure",
	                              "generation",
	                              "mesh_source_generation",
	                              "asset_object_id" ),
	        &Terrain3DAssets::completeMeshThumbnailAsync );
	ClassDB::bind_method( D_METHOD( "update_mesh_list" ), &Terrain3DAssets::updateMeshList );

	ClassDB::bind_method( D_METHOD( "save", "path" ), &Terrain3DAssets::save, DEFVAL( "" ) );

	const int ro_flags = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
	ADD_GROUP( "Packing / Runtime Format", "" );
	T3D_BIND_PROPERTY( Terrain3DAssets, setTexturePackingMode, getTexturePackingMode, "mode",
	                   PropertyInfo( Variant::INT, "texture_packing_mode", PROPERTY_HINT_ENUM, "Legacy AlbedoHeight + NormalRoughness,Packed PBR RGBA8 + Displacement R8" ), "set_texture_packing_mode", "get_texture_packing_mode" );

	ADD_GROUP( "Texture Asset Registry", "" );
	T3D_BIND_PROPERTY( Terrain3DAssets, setTextureList, getTextureList, "texture_list",
	                   PropertyInfo( Variant::ARRAY, "texture_list", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DTextureAsset", ro_flags ), "set_texture_list", "get_texture_list" );

	ADD_GROUP( "Mesh Asset Registry", "" );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "mesh_list", PROPERTY_HINT_ARRAY_TYPE, "Terrain3DMeshAsset", ro_flags ),
	        "set_mesh_list",
	        "get_mesh_list" );

	ADD_SIGNAL( MethodInfo( "meshes_changed" ) );
	ADD_SIGNAL( MethodInfo( "textures_changed" ) );
}

// M39 extracted non-template header implementations.
Ref<Terrain3DTextureAsset> Terrain3DAssets::getTextureAsset( const int nId ) const {
	if ( nId >= 0 && nId < m_textureList.size() ) {
		const Ref<Terrain3DTextureAsset> pAsset = m_textureList[nId];
		return pAsset.is_valid() && !pAsset->m_bPlaceholder ? pAsset : Ref<Terrain3DTextureAsset>();
	}
	return Ref<Terrain3DTextureAsset>();
}

Ref<Terrain3DMeshAsset> Terrain3DAssets::getMeshAsset( const int nId ) const {
	if ( nId >= 0 && nId < m_meshList.size() ) {
		const Ref<Terrain3DMeshAsset> pAsset = m_meshList[nId];
		return pAsset.is_valid() && !pAsset->m_bPlaceholder ? pAsset : Ref<Terrain3DMeshAsset>();
	}
	return Ref<Terrain3DMeshAsset>();
}
