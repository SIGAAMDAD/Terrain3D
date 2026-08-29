// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_INSTANCE_3D_H
#define TERRAIN3D_RENDER_WORLD_INSTANCE_3D_H

#include "assets/terrain_3d_render_world_import_profile.h"
#include "assets/terrain_3d_render_world_material_resource.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>

class Terrain3DRenderWorldInstance3D : public godot::MeshInstance3D {
	GDCLASS( Terrain3DRenderWorldInstance3D, godot::MeshInstance3D );

private:
	godot::Ref<Terrain3DRenderWorldImportProfile> m_importProfile;
	godot::Ref<Terrain3DRenderWorldMaterial> m_renderWorldMaterial;
	int32_t m_iTierOverride = ( int32_t )( T3D_RW_IMPORT_AUTO );
	bool m_bForceNative = false;
	bool m_bDynamicInstance = false;

protected:
	static void _bind_methods( void );

public:
	Terrain3DRenderWorldInstance3D( void ) = default;
	~Terrain3DRenderWorldInstance3D( void ) = default;

	void setImportProfile( const godot::Ref<Terrain3DRenderWorldImportProfile> &rProfile );
	EA_FORCE_INLINE godot::Ref<Terrain3DRenderWorldImportProfile> getImportProfile( void ) const {
		return m_importProfile;
	}
	void setRenderWorldMaterial( const godot::Ref<Terrain3DRenderWorldMaterial> &rMaterial );
	EA_FORCE_INLINE godot::Ref<Terrain3DRenderWorldMaterial> getRenderWorldMaterial( void ) const {
		return m_renderWorldMaterial;
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

#endif // TERRAIN3D_RENDER_WORLD_INSTANCE_3D_H
