// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_VT_FIXED_PROFILE_H
#define TERRAIN3D_VT_FIXED_PROFILE_H

#include "rendering/rd/vt/terrain_3d_vt_contract.h"

#include <stdint.h>

#define T3D_VT_MAX_AUTHORED_EXTENT 4096u

typedef enum t3dVTFixedProfile_e : uint32_t {
	T3D_VT_FIXED_PROFILE_JUST_MAKE_IT_RUN = 0u,
	T3D_VT_FIXED_PROFILE_LOW = 1u,
	T3D_VT_FIXED_PROFILE_NORMAL = 2u,
	T3D_VT_FIXED_PROFILE_HIGH = 3u,
	T3D_VT_FIXED_PROFILE_VERY_HIGH = 4u,
	T3D_VT_FIXED_PROFILE_MAKE_MY_GPU_SCREAM = 5u,
	T3D_VT_FIXED_PROFILE_COUNT = T3D_VT_QUALITY_PROFILE_COUNT,
	T3D_VT_FIXED_PROFILE_INVALID = 0xffffffffu,
} t3dVTFixedProfile_t;

typedef struct t3dVTFixedProfileDesc_s {
	uint32_t m_nExtent[T3D_VT_LOGICAL_CHANNEL_COUNT];
	uint32_t m_nSupportedChannelMask;
} t3dVTFixedProfileDesc_t;

const t3dVTFixedProfileDesc_t *t3d_vt_fixed_profile_get( t3dVTFixedProfile_t Profile );
uint32_t t3d_vt_fixed_profile_channel_extent( t3dVTFixedProfile_t Profile, t3dVTLogicalChannel_t Channel );
uint32_t t3d_vt_fixed_profile_mip0_pages( t3dVTFixedProfile_t Profile, t3dVTLogicalChannel_t Channel );
bool t3d_vt_fixed_profile_validate_extent( t3dVTFixedProfile_t Profile, t3dVTLogicalChannel_t Channel, uint32_t nWidth, uint32_t nHeight );
uint32_t t3d_vt_fixed_profile_clipmap_mask( t3dVTFixedProfile_t Profile, bool bHeightEnabled );
uint32_t t3d_vt_fixed_profile_instance_mask( t3dVTFixedProfile_t Profile, bool bNormalEnabled, bool bOrmEnabled );
bool t3d_vt_fixed_profile_source_supports( t3dVTFixedProfile_t Profile,
        t3dVTConsumer_t Consumer,
        uint32_t nAuthoredChannelMask,
        const uint32_t nSourceExtent[T3D_VT_LOGICAL_CHANNEL_COUNT] );
t3dVTFixedProfile_t t3d_vt_fixed_profile_highest_supported( t3dVTConsumer_t Consumer,
        uint32_t nAuthoredChannelMask,
        const uint32_t nSourceExtent[T3D_VT_LOGICAL_CHANNEL_COUNT] );

#endif // TERRAIN3D_VT_FIXED_PROFILE_H
