#ifndef TERRAIN3D_AUTHORING_HOOKS_H
#define TERRAIN3D_AUTHORING_HOOKS_H

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/dictionary.hpp>

class Terrain3DRegion;

// Runtime-facing, non-owning authoring attachment. This contract has no
// dependency on Terrain3DEditor or any Godot editor-only class. Its concrete
// provider belongs to the editor binary; runtime builds never install one.
class Terrain3DAuthoringHooks
{
protected:
	virtual ~Terrain3DAuthoringHooks( void ) = default;

public:
	virtual bool			  isOperating( void ) const = 0;
	virtual void			  backupRegion( const godot::Ref< Terrain3DRegion > &rRegion, bool bMapWasImplicit ) = 0;
	virtual godot::Dictionary getBrushData( void ) const = 0;
	virtual void			  releaseRuntimeAllocatorStorage( void ) = 0;
	// Called while the owner is still alive. Invalidate the editor's borrowed
	// Terrain3D pointer before a scene unload can destroy the native owner.
	virtual void onTerrainOwnerDetached( void ) = 0;
};

#endif // TERRAIN3D_AUTHORING_HOOKS_H
