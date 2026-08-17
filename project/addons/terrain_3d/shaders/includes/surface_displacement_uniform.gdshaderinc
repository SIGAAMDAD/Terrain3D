// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_SURFACE_DISPLACEMENT_UNIFORM
// Dedicated R8_UNORM displacement array. This section is enabled only when
// Terrain3D tessellation is active, so non-tessellated shaders do not bind or
// sample displacement data.
uniform highp sampler2DArray _texture_array_displacement : FILTER_METHOD, repeat_enable;
#define SURFACE_DISPLACEMENT_ENABLED
#endif
