#ifndef TERRAIN3D_RD_FRAMEBUFFER_H
#define TERRAIN3D_RD_FRAMEBUFFER_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_resource_state.h"

#include <stdint.h>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

class Terrain3DRdFramebuffer
{
	Terrain3DRdResourceState< 1u > m_state;

public:
	Terrain3DRdFramebuffer( void ) = default;
	Terrain3DRdFramebuffer( const Terrain3DRdFramebuffer & ) = delete;
	Terrain3DRdFramebuffer &operator=( const Terrain3DRdFramebuffer & ) = delete;

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

	bool	createTracked( Terrain3DRdContext	   &rContext,
						   t3dRdResourceOwner_t		owner,
						   const String			   &rName,
						   const TypedArray< RID > &rAttachments,
						   int64_t					nValidateWithFormat = -1,
						   uint32_t					nViewCount = 1u );
	bool	replaceTracked( Terrain3DRdContext		&rContext,
							t3dRdResourceOwner_t	 owner,
							const String			&rName,
							const TypedArray< RID > &rAttachments,
							int64_t					 nValidateWithFormat = -1,
							uint32_t				 nViewCount = 1u );
	int64_t format( RenderingDevice *pRd ) const;
	void	releaseTracked( Terrain3DRdContext &rContext );
	void	abandon( void );
	void	swap( Terrain3DRdFramebuffer &rOther );
};

#endif // TERRAIN3D_RD_FRAMEBUFFER_H
