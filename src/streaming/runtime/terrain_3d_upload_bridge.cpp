// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "streaming/runtime/terrain_3d_upload_bridge.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

static Object *t3dUploadBridgeGetUploadBridge( void ) {
	Engine *pEngine = Engine::get_singleton();
	if ( !pEngine ) {
		return NULL;
	}
	static const StringName s_singletonName( "TerrainUploadBridge" );
	if ( !pEngine->has_singleton( s_singletonName ) ) {
		return NULL;
	}
	return pEngine->get_singleton( s_singletonName );
}

bool t3dUploadBridgeIsAvailable( void ) {
	Object *pBridge = t3dUploadBridgeGetUploadBridge();
	if ( !pBridge ) {
		return false;
	}
	const Variant result = pBridge->call( "is_available" );
	return result.get_type() == Variant::BOOL && ( bool )( result );
}

bool t3dUploadBridgeUploadTextureLayer( const RID hTexture, const Ref<Image> &rImage, const int iLayer ) {
	if ( !hTexture.is_valid() || rImage.is_null() || rImage->is_empty() || iLayer < 0 ) {
		return false;
	}
	Object *pBridge = t3dUploadBridgeGetUploadBridge();
	if ( !pBridge ) {
		return false;
	}
	const PackedByteArray bytes = rImage->get_data();
	if ( bytes.is_empty() ) {
		return false;
	}
	const Variant result = pBridge->call( "upload_texture_layer", hTexture, iLayer, bytes );
	return result.get_type() == Variant::BOOL && ( bool )( result );
}

RID t3dUploadBridgeCreatePersistentStorageBuffer( const uint32_t nSizeBytes ) {
	if ( nSizeBytes == 0u ) {
		return RID();
	}
	Object *pBridge = t3dUploadBridgeGetUploadBridge();
	if ( !pBridge ) {
		return RID();
	}
	const Variant result = pBridge->call( "create_persistent_storage_buffer", ( int64_t )( nSizeBytes ) );
	if ( result.get_type() != Variant::RID ) {
		return RID();
	}
	return RID( result );
}

Dictionary t3dUploadBridgeGetStats( void ) {
	Object *pBridge = t3dUploadBridgeGetUploadBridge();
	if ( !pBridge ) {
		return Dictionary();
	}
	const Variant result = pBridge->call( "get_stats" );
	if ( result.get_type() != Variant::DICTIONARY ) {
		return Dictionary();
	}
	return Dictionary( result );
}
