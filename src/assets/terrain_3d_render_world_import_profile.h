// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_IMPORT_PROFILE_H
#define TERRAIN3D_RENDER_WORLD_IMPORT_PROFILE_H

#include "authoring/render_world/terrain_3d_render_world_authoring.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/resource.hpp>

class Terrain3DRenderWorldImportProfile : public godot::Resource {
	GDCLASS( Terrain3DRenderWorldImportProfile, godot::Resource );

private:
	int32_t m_iCategory = ( int32_t )( T3D_RW_IMPORT_AUTO );
	int32_t m_iMaterialPolicy = ( int32_t )( T3D_RW_MATERIAL_AUTO_CONVERT );
	int32_t m_iTransparencyPolicy = ( int32_t )( T3D_RW_TRANSPARENCY_NATIVE_SORTED );
	float m_fMaxDistance = 0.0f;
	bool m_bEnabled = true;
	bool m_bStaticOnly = true;
	bool m_bCastShadows = true;
	bool m_bReceiveShadows = true;

protected:
	static void _bind_methods( void );

public:
	Terrain3DRenderWorldImportProfile( void ) = default;
	~Terrain3DRenderWorldImportProfile( void ) = default;

	void setEnabled( bool bEnabled );
	EA_FORCE_INLINE bool isEnabled( void ) const {
		return m_bEnabled;
	}
	void setCategory( int32_t iCategory );
	EA_FORCE_INLINE int32_t getCategory( void ) const {
		return m_iCategory;
	}
	void setMaterialPolicy( int32_t iPolicy );
	EA_FORCE_INLINE int32_t getMaterialPolicy( void ) const {
		return m_iMaterialPolicy;
	}
	void setTransparencyPolicy( int32_t iPolicy );
	EA_FORCE_INLINE int32_t getTransparencyPolicy( void ) const {
		return m_iTransparencyPolicy;
	}
	void setStaticOnly( bool bEnabled );
	EA_FORCE_INLINE bool isStaticOnly( void ) const {
		return m_bStaticOnly;
	}
	void setCastShadows( bool bEnabled );
	EA_FORCE_INLINE bool isCastShadows( void ) const {
		return m_bCastShadows;
	}
	void setReceiveShadows( bool bEnabled );
	EA_FORCE_INLINE bool isReceiveShadows( void ) const {
		return m_bReceiveShadows;
	}
	void setMaxDistance( float fDistance );
	EA_FORCE_INLINE float getMaxDistance( void ) const {
		return m_fMaxDistance;
	}

	uint32_t resolveTier( uint32_t nDefaultTier ) const;
};

#endif // TERRAIN3D_RENDER_WORLD_IMPORT_PROFILE_H
