// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_upload_bridge.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace {

Object *getUploadBridge( void ) {
	Engine *pEngine = Engine::get_singleton();
	if ( !pEngine ) {
		return nullptr;
	}
	static const StringName s_singletonName( "TerrainUploadBridge" );
	if ( !pEngine->has_singleton( s_singletonName ) ) {
		return nullptr;
	}
	return pEngine->get_singleton( s_singletonName );
}

} // namespace

namespace Terrain3DUploadBridge {

bool isAvailable( void ) {
	Object *pBridge = getUploadBridge();
	if ( !pBridge ) {
		return false;
	}
	const Variant result = pBridge->call( "is_available" );
	return result.get_type() == Variant::BOOL && ( bool ) ( result );
}

bool uploadTextureLayer( const RID texture, const Ref<Image> &rImage, const int nLayer ) {
	if ( !texture.is_valid() || rImage.is_null() || rImage->is_empty() || nLayer < 0 ) {
		return false;
	}
	Object *pBridge = getUploadBridge();
	if ( !pBridge ) {
		return false;
	}
	const PackedByteArray bytes = rImage->get_data();
	if ( bytes.is_empty() ) {
		return false;
	}
	const Variant result = pBridge->call( "upload_texture_layer", texture, nLayer, bytes );
	return result.get_type() == Variant::BOOL && ( bool ) ( result );
}

RID createPersistentStorageBuffer( const uint32_t nSizeBytes ) {
	if ( nSizeBytes == 0u ) {
		return RID();
	}
	Object *pBridge = getUploadBridge();
	if ( !pBridge ) {
		return RID();
	}
	const Variant result = pBridge->call( "create_persistent_storage_buffer", ( int64_t ) ( nSizeBytes ) );
	if ( result.get_type() != Variant::RID ) {
		return RID();
	}
	return RID( result );
}

Dictionary getStats( void ) {
	Object *pBridge = getUploadBridge();
	if ( !pBridge ) {
		return Dictionary();
	}
	const Variant result = pBridge->call( "get_stats" );
	if ( result.get_type() != Variant::DICTIONARY ) {
		return Dictionary();
	}
	return Dictionary( result );
}

} // namespace Terrain3DUploadBridge
