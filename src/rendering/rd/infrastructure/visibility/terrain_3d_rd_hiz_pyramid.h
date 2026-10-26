#ifndef TERRAIN3D_RD_HIZ_PYRAMID_H
#define TERRAIN3D_RD_HIZ_PYRAMID_H

#include "rendering/rd/infrastructure/resources/terrain_3d_rd_sampler.h"
#include "rendering/rd/infrastructure/resources/terrain_3d_rd_texture.h"
#include <godot_cpp/classes/rd_sampler_state.hpp>

// Common R32F pyramid lifecycle. Reduction shaders remain domain specific.
// Callers release their descriptor sets before releasing these mip views.
class Terrain3DRdHiZPyramid
{
public:
	static void release( Terrain3DRdContext &rContext,
	        Terrain3DRdSampler &rSampler,
	        Terrain3DRdTexture &rTexture,
	        Terrain3DRdTexture *pMipViews,
	        uint32_t nViewCapacity );

	static bool create( Terrain3DRdContext &rContext,
	        t3dRdResourceOwner_t owner,
	        const String &rName,
	        uint32_t nWidth,
	        uint32_t nHeight,
	        uint32_t nMipCount,
	        Terrain3DRdSampler &rSampler,
	        Terrain3DRdTexture &rTexture,
	        Terrain3DRdTexture *pMipViews,
	        uint32_t nViewCapacity );
};

#endif // TERRAIN3D_RD_HIZ_PYRAMID_H
