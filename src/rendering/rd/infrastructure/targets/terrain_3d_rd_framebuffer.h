#ifndef TERRAIN3D_RD_FRAMEBUFFER_H
#define TERRAIN3D_RD_FRAMEBUFFER_H

#include "rendering/rd/core/terrain_3d_rd_context.h"

#include <stdint.h>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

class Terrain3DRdFramebuffer {
	RID m_hRid;
	uint32_t m_nGeneration = 0u;
	t3dRdResourceOwner_t m_owner = T3D_RD_CONTEXT_OWNER_UNSPECIFIED;

	static uint32_t nextGeneration( uint32_t nGeneration ) { return nGeneration == UINT32_MAX ? 1u : nGeneration + 1u; }

public:
	Terrain3DRdFramebuffer( void ) = default;
	Terrain3DRdFramebuffer( const Terrain3DRdFramebuffer & ) = delete;
	Terrain3DRdFramebuffer &operator=( const Terrain3DRdFramebuffer & ) = delete;

	EA_FORCE_INLINE bool isValid( void ) const { return m_hRid.is_valid(); }
	EA_FORCE_INLINE const RID &rid( void ) const { return m_hRid; }
	EA_FORCE_INLINE uint32_t generation( void ) const { return m_nGeneration; }
	EA_FORCE_INLINE t3dRdResourceOwner_t owner( void ) const { return m_owner; }

	bool createTracked( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner,
	        const String &rName, const TypedArray<RID> &rAttachments,
	        int64_t nValidateWithFormat = -1, uint32_t nViewCount = 1u );
	bool replaceTracked( Terrain3DRdContext &rContext, t3dRdResourceOwner_t owner,
	        const String &rName, const TypedArray<RID> &rAttachments,
	        int64_t nValidateWithFormat = -1, uint32_t nViewCount = 1u );
	int64_t format( RenderingDevice *pRd ) const;
	void releaseTracked( Terrain3DRdContext &rContext );
	void abandon( void );
	void swap( Terrain3DRdFramebuffer &rOther );
};

#endif // TERRAIN3D_RD_FRAMEBUFFER_H
