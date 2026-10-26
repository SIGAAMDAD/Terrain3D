#ifndef TERRAIN3D_RD_SAMPLER_H
#define TERRAIN3D_RD_SAMPLER_H

#include "rendering/rd/terrain_3d_rd_context.h"

#include <stdint.h>
#include <godot_cpp/classes/rd_sampler_state.hpp>

using namespace godot;

class Terrain3DRdSampler {
	RID m_hRid;
	uint32_t m_nGeneration = 0u;
	Terrain3DRdContext::ResourceOwner m_owner = Terrain3DRdContext::OWNER_UNSPECIFIED;

	static uint32_t nextGeneration( uint32_t nGeneration ) { return nGeneration == UINT32_MAX ? 1u : nGeneration + 1u; }

public:
	Terrain3DRdSampler( void ) = default;
	Terrain3DRdSampler( const Terrain3DRdSampler & ) = delete;
	Terrain3DRdSampler &operator=( const Terrain3DRdSampler & ) = delete;

	EA_FORCE_INLINE bool isValid( void ) const { return m_hRid.is_valid(); }
	EA_FORCE_INLINE const RID &rid( void ) const { return m_hRid; }
	EA_FORCE_INLINE uint32_t generation( void ) const { return m_nGeneration; }
	EA_FORCE_INLINE Terrain3DRdContext::ResourceOwner owner( void ) const { return m_owner; }

	bool createTracked( Terrain3DRdContext &rContext, Terrain3DRdContext::ResourceOwner owner,
	        const String &rName, const Ref<RDSamplerState> &rState );
	void releaseTracked( Terrain3DRdContext &rContext );
	void abandon( void );
	void swap( Terrain3DRdSampler &rOther );
};

#endif // TERRAIN3D_RD_SAMPLER_H
