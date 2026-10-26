// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_FILE_PUBLICATION_H
#define TERRAIN3D_FILE_PUBLICATION_H

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <stdint.h>

// One writer owns each path. Keep the previous file until the caller accepts
// the published artifact. Recovery never discards an ambiguous previous file.
class Terrain3DFilePublication {
public:
	Terrain3DFilePublication( const godot::String &rPath, const godot::String &rStaged, const godot::String &rPrevious ) :
	        m_path( rPath ), m_staged( rStaged ), m_previous( rPrevious ) {}
	Terrain3DFilePublication( const Terrain3DFilePublication & ) = delete;
	Terrain3DFilePublication &operator=( const Terrain3DFilePublication & ) = delete;

	godot::Error prepare( void ) {
		if ( m_path.is_empty() || m_staged.is_empty() || m_previous.is_empty()
		        || canonicalPath( m_path ) == canonicalPath( m_staged )
		        || canonicalPath( m_path ) == canonicalPath( m_previous )
		        || canonicalPath( m_staged ) == canonicalPath( m_previous ) ) {
			return fail( godot::ERR_INVALID_PARAMETER, "Invalid publication paths." );
		}
		if ( godot::FileAccess::file_exists( m_previous ) ) {
			if ( godot::FileAccess::file_exists( m_path ) ) {
				return fail( godot::ERR_ALREADY_EXISTS, "Unresolved publication and previous file; both preserved." );
			}
			const godot::Error error = godot::DirAccess::rename_absolute( m_previous, m_path );
			if ( error != godot::OK ) {
				return fail( error, "Could not restore previous publication; previous file preserved." );
			}
		}
		m_bPrepared = true;
		return godot::OK;
	}

	godot::Error publish( void ) {
		if ( !m_bPrepared || m_bPublished || m_bBackedUp ) {
			return fail( godot::ERR_UNCONFIGURED, "Publication is not prepared." );
		}
		if ( godot::FileAccess::file_exists( m_previous ) ) {
			return fail( godot::ERR_ALREADY_EXISTS, "Previous file appeared during staging; preserved." );
		}
		if ( godot::FileAccess::file_exists( m_path ) ) {
			const godot::Error error = godot::DirAccess::rename_absolute( m_path, m_previous );
			if ( error != godot::OK ) {
				return fail( error, "Could not preserve previous publication." );
			}
			m_bBackedUp = true;
		}
		const godot::Error error = godot::DirAccess::rename_absolute( m_staged, m_path );
		if ( error != godot::OK ) {
			const godot::Error recovery = rollback();
			return recovery != godot::OK ? recovery : fail( error, "Could not publish staged file." );
		}
		m_bPublished = true;
		return godot::OK;
	}

	godot::Error rollback( void ) {
		if ( m_bPublished ) {
			const godot::Error error = godot::DirAccess::remove_absolute( m_path );
			if ( error != godot::OK ) {
				return fail( error, "Could not remove rejected publication; previous file preserved." );
			}
			m_bPublished = false;
		}
		if ( m_bBackedUp ) {
			const godot::Error error = godot::DirAccess::rename_absolute( m_previous, m_path );
			if ( error != godot::OK ) {
				return fail( error, "Publication rollback failed; previous file preserved." );
			}
			m_bBackedUp = false;
		}
		m_bPrepared = false;
		return godot::OK;
	}

	godot::Error commit( void ) {
		if ( !m_bPublished ) {
			return fail( godot::ERR_UNCONFIGURED, "No publication to commit." );
		}
		if ( m_bBackedUp ) {
			const godot::Error error = godot::DirAccess::remove_absolute( m_previous );
			if ( error != godot::OK ) {
				return fail( error, "Publication succeeded but previous-file cleanup failed; both preserved." );
			}
			m_bBackedUp = false;
		}
		m_bPrepared = false;
		m_bPublished = false;
		return godot::OK;
	}

	const godot::String &getError( void ) const { return m_error; }

	// Serialization stays with the format owner. These operations only own the
	// staged stream; callers may validate the published file before committing.
	godot::Error openStaged( godot::Ref<godot::FileAccess>& rFile )
	{
		const godot::Error error = prepare();
		if ( error != godot::OK ) {
			return error;
		}
		if ( godot::FileAccess::file_exists( m_staged ) ) {
			godot::DirAccess::remove_absolute( m_staged );
		}
		rFile = godot::FileAccess::open( m_staged, godot::FileAccess::WRITE );
		return rFile.is_valid() ? godot::OK : fail( godot::ERR_CANT_OPEN, "Could not open staged artifact." );
	}

	godot::Error finishStaged( godot::Ref<godot::FileAccess>& rFile, uint64_t nExpectedBytes = UINT64_MAX )
	{
		if ( rFile.is_null() ) {
			return fail( godot::ERR_UNCONFIGURED, "No staged artifact stream." );
		}
		if ( nExpectedBytes == UINT64_MAX ) {
			nExpectedBytes = rFile->get_position();
		}
		rFile->flush();
		godot::Error error = rFile->get_error();
		if ( error == godot::OK && rFile->get_length() != nExpectedBytes ) {
			error = godot::ERR_FILE_CANT_WRITE;
		}
		rFile.unref();
		if ( error != godot::OK ) {
			godot::DirAccess::remove_absolute( m_staged );
			return fail( error, "Could not write staged artifact." );
		}
		return godot::OK;
	}

	godot::Error stagePacked( const godot::PackedByteArray& rBytes )
	{
		godot::Ref<godot::FileAccess> file;
		const godot::Error error = openStaged( file );
		if ( error != godot::OK ) {
			return error;
		}
		file->store_buffer( rBytes );
		return finishStaged( file, (uint64_t)rBytes.size() );
	}

private:
	static godot::String canonicalPath( const godot::String &rPath ) {
		godot::ProjectSettings *const pSettings = godot::ProjectSettings::get_singleton();
		godot::String path = ( pSettings ? pSettings->globalize_path( rPath ) : rPath ).simplify_path();
#ifdef _WIN32
		path = path.to_lower();
#endif
		return path;
	}

	godot::Error fail( const godot::Error eError, const godot::String &rMessage ) {
		m_error = rMessage + godot::String( " Path: " ) + m_path + godot::String( "; previous: " ) + m_previous;
		godot::UtilityFunctions::push_error( m_error );
		return eError;
	}

	godot::String m_path;
	godot::String m_staged;
	godot::String m_previous;
	godot::String m_error;
	bool m_bPrepared = false;
	bool m_bBackedUp = false;
	bool m_bPublished = false;
};

// Common packed artifact writer. Explicit errors preserve the previous file.
inline godot::Error t3dPublishPackedFile( const godot::String &rPath, const godot::PackedByteArray &rBytes, godot::String *pError ) {
	Terrain3DFilePublication publication( rPath, rPath + godot::String( ".staged" ), rPath + godot::String( ".previous" ) );
	godot::Error error = publication.stagePacked( rBytes );
	if ( error == godot::OK ) {
		error = publication.publish();
	}
	if ( error == godot::OK ) { error = publication.commit(); }
	if ( error != godot::OK && pError ) { *pError = publication.getError(); }
	return error;
}

#endif // TERRAIN3D_FILE_PUBLICATION_H
