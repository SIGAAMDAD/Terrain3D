// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.
#ifndef TERRAIN3D_WORLD_HIZ_H
#define TERRAIN3D_WORLD_HIZ_H

#include "Terrain3DTypes.h"
#include "Terrain3DConstants.h"

#include "rendering/rd/infrastructure/visibility/terrain_3d_rd_hiz_reducer.h"
#include "rendering/rd/world/core/terrain_3d_world_contract.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

class Terrain3DWorldHiZ {
private:
Terrain3DRdContext &m_context;
	Terrain3DRdHiZReducer m_reducer;
	uint64_t m_nUnavailableFrames = 0u;

	static void writePush( void *pUser, const t3dRdHiZDispatchInfo_t &rInfo, uint8_t *pDestination, uint32_t nDestinationBytes );

public:
	explicit Terrain3DWorldHiZ( Terrain3DRdContext &rContext );
	~Terrain3DWorldHiZ( void );
	bool initializeCold( const t3dWorldResourceLimits_t &rLimits );
	void shutdownCold( void );
	bool bindPreviousDepthCold( const godot::RID &rDepth );
	bool buildHot( const godot::Vector2i &rRenderSize );
	bool isReady( void ) const;
	const godot::RID &getTexture( void ) const;
	const godot::RID &getSampler( void ) const;
	uint32_t getWidth( void ) const;
	uint32_t getHeight( void ) const;
	uint32_t getMipCount( void ) const;
	uint64_t getResourceGeneration( void ) const;
	uint64_t getBuildCount( void ) const;
	uint64_t getBuildDispatches( void ) const;
	uint64_t getSourceRebinds( void ) const;
	uint64_t getUnavailableFrames( void ) const;
	uint64_t getGpuBytes( void ) const;
};

#endif // TERRAIN3D_WORLD_HIZ_H
