// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_WORLD_IMPORT_PROFILE_H
#define TERRAIN3D_WORLD_IMPORT_PROFILE_H

#include "authoring/world/terrain_3d_world_authoring.h"
#include "authoring/vt/terrain_3d_vt_cooker.h"
#include "core/terrain_3d_compiler.h"

#include <godot_cpp/classes/resource.hpp>

class Terrain3DWorldImportProfile : public godot::Resource {
	GDCLASS( Terrain3DWorldImportProfile, godot::Resource );

private:
	int32_t m_iCategory = ( int32_t )( T3D_WORLD_IMPORT_AUTO );
	int32_t m_iMaterialPolicy = ( int32_t )( T3D_WORLD_MATERIAL_AUTO_CONVERT );
	int32_t m_iTransparencyPolicy = ( int32_t )( T3D_WORLD_TRANSPARENCY_NATIVE_SORTED );
	float m_fMaxDistance = 0.0f;
	int32_t m_iVtOuterCompression = ( int32_t )( T3D_VT_OUTER_COMPRESSION_NONE );
	int32_t m_iVtClusterTargetPages = 64;
	int32_t m_iVtClusterTargetBytes = 4 * 1024 * 1024;
	int32_t m_iVtMipQuality = 1;
	int32_t m_iVtCompressionQuality = 2;
	bool m_bEnabled = true;
	bool m_bStaticOnly = true;
	bool m_bCastShadows = true;
	bool m_bReceiveShadows = true;

protected:
	static void _bind_methods( void );

public:
	Terrain3DWorldImportProfile( void ) = default;
	~Terrain3DWorldImportProfile( void ) = default;

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


	void setVtClusterTargetPages( int32_t iPages );
	EA_FORCE_INLINE int32_t getVtClusterTargetPages( void ) const {
		return m_iVtClusterTargetPages;
	}
	void setVtClusterTargetBytes( int32_t iBytes );
	EA_FORCE_INLINE int32_t getVtClusterTargetBytes( void ) const {
		return m_iVtClusterTargetBytes;
	}
	void setVtOuterCompression( int32_t iCompression );
	EA_FORCE_INLINE int32_t getVtOuterCompression( void ) const {
		return m_iVtOuterCompression;
	}
	void setVtMipQuality( int32_t iQuality );
	EA_FORCE_INLINE int32_t getVtMipQuality( void ) const {
		return m_iVtMipQuality;
	}
	void setVtCompressionQuality( int32_t iQuality );
	EA_FORCE_INLINE int32_t getVtCompressionQuality( void ) const {
		return m_iVtCompressionQuality;
	}
	EA_FORCE_INLINE int32_t getVtPageInterior( void ) const {
		return ( int32_t )T3D_VT_PAGE_INTERIOR;
	}
	EA_FORCE_INLINE int32_t getVtPageGutter( void ) const {
		return ( int32_t )T3D_VT_PAGE_GUTTER;
	}
	void resolveVtCookerSettings( t3dVTCookerSettings_t &rSettings ) const;

	uint32_t resolveTier( uint32_t nDefaultTier ) const;
};

#endif // TERRAIN3D_WORLD_IMPORT_PROFILE_H
