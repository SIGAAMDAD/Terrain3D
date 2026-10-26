// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_INSTANCE_3D_H
#define TERRAIN3D_WORLD_INSTANCE_3D_H

#include "assets/import/terrain_3d_world_import_profile.h"
#include "assets/resources/terrain_3d_world_material_resource.h"
#include "core/config/terrain_3d_compiler.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>

class Terrain3DWorldInstance3D : public godot::MeshInstance3D {
	GDCLASS( Terrain3DWorldInstance3D, godot::MeshInstance3D );

private:
	godot::Ref<Terrain3DWorldImportProfile> m_importProfile;
	godot::Ref<Terrain3DWorldMaterial> m_worldMaterial;
	int32_t m_iTierOverride = ( int32_t )( T3D_WORLD_IMPORT_AUTO );
	bool m_bForceNative = false;
	bool m_bDynamicInstance = false;

protected:
	static void _bind_methods( void );

public:
	Terrain3DWorldInstance3D( void ) = default;
	~Terrain3DWorldInstance3D( void ) = default;

	void setImportProfile( const godot::Ref<Terrain3DWorldImportProfile> &rProfile );
	EA_FORCE_INLINE godot::Ref<Terrain3DWorldImportProfile> getImportProfile( void ) const {
		return m_importProfile;
	}
	void setWorldMaterial( const godot::Ref<Terrain3DWorldMaterial> &rMaterial );
	EA_FORCE_INLINE godot::Ref<Terrain3DWorldMaterial> getWorldMaterial( void ) const {
		return m_worldMaterial;
	}
	void setForceNative( bool bEnabled );
	EA_FORCE_INLINE bool isForceNative( void ) const {
		return m_bForceNative;
	}
	void setDynamicInstance( bool bEnabled );
	EA_FORCE_INLINE bool isDynamicInstance( void ) const {
		return m_bDynamicInstance;
	}
	void setTierOverride( int32_t iTier );
	EA_FORCE_INLINE int32_t getTierOverride( void ) const {
		return m_iTierOverride;
	}
};

#endif // TERRAIN3D_WORLD_INSTANCE_3D_H
