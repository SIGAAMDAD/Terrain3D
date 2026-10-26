#ifndef TERRAIN3D_VT_CAPACITY_POLICY_H
#define TERRAIN3D_VT_CAPACITY_POLICY_H

#include <stdint.h>

// Residency requires neutral backing plus room for admission and retirement.
// Budget planning and residency admission must agree on this runtime minimum.
inline constexpr uint32_t T3D_VT_MIN_PHYSICAL_PAGE_SETS = 3u;

#endif
