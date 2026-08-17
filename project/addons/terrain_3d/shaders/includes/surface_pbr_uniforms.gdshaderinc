// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_SURFACE_PBR_UNIFORMS
// Linear surface data: R=normal X, G=normal Y, B=roughness, A=ambient occlusion.
uniform highp sampler2DArray _texture_array_surface : FILTER_METHOD, repeat_enable;
uniform float _texture_metallic_array[32];
#endif
