#ifndef TERRAIN3D_RD_SAMPLER_H
#define TERRAIN3D_RD_SAMPLER_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_state.h"

#include <stdint.h>
#include <godot_cpp/classes/rd_sampler_state.hpp>

using namespace godot;

class Terrain3DRdSampler
{
	Terrain3DRdResourceState< 1u > m_state;

public:
	Terrain3DRdSampler( void ) = default;
	Terrain3DRdSampler( const Terrain3DRdSampler & ) = delete;
	Terrain3DRdSampler &operator=( const Terrain3DRdSampler & ) = delete;

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

	bool createTracked( Terrain3DRdContext			&rContext,
						t3dRdResourceOwner_t		 owner,
						const String				&rName,
						const Ref< RDSamplerState > &rState );
	void releaseTracked( Terrain3DRdContext &rContext );
	void abandon( void );
	void swap( Terrain3DRdSampler &rOther );
};

#endif // TERRAIN3D_RD_SAMPLER_H
