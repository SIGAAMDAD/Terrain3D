#ifndef TERRAIN3D_RD_SHADER_H
#define TERRAIN3D_RD_SHADER_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_state.h"
#include "rendering/rd/shaders/terrain_3d_rd_shader_catalog.h"

#include <stdint.h>
#include <godot_cpp/classes/rd_shader_spirv.hpp>

using namespace godot;

class Terrain3DRdShader
{
	Terrain3DRdResourceState< 1u > m_state;
	bool						   m_bLocal = false;
	bool						   m_bBorrowed = false;

	static RID createPhysical( RenderingDevice *pRd, const Ref< RDShaderSPIRV > &rSpirv, const String &rName );

public:
	Terrain3DRdShader( void ) = default;
	Terrain3DRdShader( const Terrain3DRdShader & ) = delete;
	Terrain3DRdShader &operator=( const Terrain3DRdShader & ) = delete;

	EA_FORCE_INLINE bool isValid(
		void ) const {
		return m_state.isValid();
	}
	EA_FORCE_INLINE const RID &rid(
		void ) const {
		return m_state.rid();
	}
	EA_FORCE_INLINE uint32_t generation(
		void ) const {
		return m_state.generation();
	}
	EA_FORCE_INLINE t3dRdResourceOwner_t owner(
		void ) const {
		return m_state.owner();
	}
	EA_FORCE_INLINE bool isLocal(
		void ) const {
		return m_bLocal;
	}

	bool
	createTracked( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner, const String &rName, t3dRdShaderId_t id );
	bool createTracked( Terrain3DRdContext	&rContext,
						t3dRdResourceOwner_t owner,
						const String		&rName,
						t3dRdShaderId_t		 id,
						const String		&rRegistryName );
	bool createTracked( Terrain3DRdContext		   &rContext,
						t3dRdResourceOwner_t		owner,
						const String			   &rName,
						const Ref< RDShaderSPIRV > &rSpirv );
	bool createTracked( Terrain3DRdContext		   &rContext,
						t3dRdResourceOwner_t		owner,
						const String			   &rName,
						const Ref< RDShaderSPIRV > &rSpirv,
						const String			   &rRegistryName );
	bool createLocal( RenderingDevice *pRd, const String &rName, const Ref< RDShaderSPIRV > &rSpirv );
	void releaseTracked( Terrain3DRdContext &rContext );
	void releaseLocal( RenderingDevice *pRd );
	void abandon( void );
	void swap( Terrain3DRdShader &rOther );
};

#endif // TERRAIN3D_RD_SHADER_H
