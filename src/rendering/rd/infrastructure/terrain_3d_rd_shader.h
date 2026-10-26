#ifndef TERRAIN3D_RD_SHADER_H
#define TERRAIN3D_RD_SHADER_H

#include "rendering/rd/terrain_3d_rd_context.h"

#include <stdint.h>
#include <godot_cpp/classes/rd_shader_spirv.hpp>

using namespace godot;

class Terrain3DRdShader {
	RID m_hRid;
	uint32_t m_nGeneration = 0u;
	Terrain3DRdContext::ResourceOwner m_owner = Terrain3DRdContext::OWNER_UNSPECIFIED;
	bool m_bLocal = false;

	static uint32_t nextGeneration( uint32_t nGeneration ) { return nGeneration == UINT32_MAX ? 1u : nGeneration + 1u; }
	static RID createPhysical( RenderingDevice *pRd, const Ref<RDShaderSPIRV> &rSpirv, const String &rName );

public:
	Terrain3DRdShader( void ) = default;
	Terrain3DRdShader( const Terrain3DRdShader & ) = delete;
	Terrain3DRdShader &operator=( const Terrain3DRdShader & ) = delete;

	EA_FORCE_INLINE bool isValid( void ) const { return m_hRid.is_valid(); }
	EA_FORCE_INLINE const RID &rid( void ) const { return m_hRid; }
	EA_FORCE_INLINE uint32_t generation( void ) const { return m_nGeneration; }
	EA_FORCE_INLINE Terrain3DRdContext::ResourceOwner owner( void ) const { return m_owner; }
	EA_FORCE_INLINE bool isLocal( void ) const { return m_bLocal; }

	bool createTracked( Terrain3DRdContext &rContext, Terrain3DRdContext::ResourceOwner owner,
	        const String &rName, const Ref<RDShaderSPIRV> &rSpirv );
	bool createTracked( Terrain3DRdContext &rContext, Terrain3DRdContext::ResourceOwner owner,
	        const String &rName, const Ref<RDShaderSPIRV> &rSpirv, const String &rRegistryName );
	bool createLocal( RenderingDevice *pRd, const String &rName, const Ref<RDShaderSPIRV> &rSpirv );
	void releaseTracked( Terrain3DRdContext &rContext );
	void releaseLocal( RenderingDevice *pRd );
	void abandon( void );
	void swap( Terrain3DRdShader &rOther );
};

#endif // TERRAIN3D_RD_SHADER_H
