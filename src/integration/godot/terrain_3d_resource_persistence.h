// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_RESOURCE_PERSISTENCE_H
#define TERRAIN3D_RESOURCE_PERSISTENCE_H

#include "core/utility/logger.h"

#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

using namespace godot;

template< typename TResource, typename TPrepareCallback >
Error t3dSaveExternalResource(
	TResource *const pResource,
	const String	&rPath,
	bool			&rPersistenceDirty,
	TPrepareCallback pPrepare ) {
	static const char *const __class__ = "Terrain3DResourcePersistence";
	if ( pResource == NULL || ( rPath.is_empty() && pResource->get_path().is_empty() ) ) {
		return ERR_FILE_NOT_FOUND;
	}
	if ( !rPath.is_empty() ) {
		LOG( DEBUG, "Setting file path to ", rPath );
		pResource->take_over_path( rPath );
	}

	pPrepare();

	const String path = pResource->get_path();
	if ( path.get_extension() != "tres" && path.get_extension() != "res" ) {
		return OK;
	}
	const bool explicit_path = !rPath.is_empty();
	if ( !explicit_path && !rPersistenceDirty && FileAccess::file_exists( path ) ) {
		return OK;
	}

	LOG( DEBUG, String( "Attempting to save external file: " ) + path );
	const Error err = ResourceSaver::get_singleton()->save( pResource, path, ResourceSaver::FLAG_COMPRESS );
	if ( err != OK ) {
		LOG( ERROR,
			 "Cannot save file: ",
			 path,
			 ". Error code: ",
			 ERROR,
			 ". Look up @GlobalScope Error enum in the Godot docs" );
		return err;
	}

	rPersistenceDirty = false;
	LOG( INFO, "File saved successfully: ", path );
	if ( Engine::get_singleton()->is_editor_hint() && path.begins_with( "res://" ) ) {
		EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
		if ( pFilesystem && !pFilesystem->is_scanning() ) {
			pFilesystem->update_file( path );
		}
	}
	return OK;
}

#endif // TERRAIN3D_RESOURCE_PERSISTENCE_H
