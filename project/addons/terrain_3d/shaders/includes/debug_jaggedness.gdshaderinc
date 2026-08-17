// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DEBUG_JAGGEDNESS
	// Show turbulent areas of the terrain surface
	{
		const vec3 __offsets = vec3(0, 1, 2);
		vec2 __index_id = floor((INV_VIEW_MATRIX * vec4(VERTEX,1.0)).xz);
		float __h[6];
		__h[0] = decode_terrain_height(texelFetch(_height_maps, get_index_coord(__index_id + __offsets.xy), 0).r);
		__h[1] = decode_terrain_height(texelFetch(_height_maps, get_index_coord(__index_id + __offsets.yy), 0).r);
		__h[2] = decode_terrain_height(texelFetch(_height_maps, get_index_coord(__index_id + __offsets.yx), 0).r);
		__h[3] = decode_terrain_height(texelFetch(_height_maps, get_index_coord(__index_id + __offsets.xx), 0).r);
		__h[4] = decode_terrain_height(texelFetch(_height_maps, get_index_coord(__index_id + __offsets.zx), 0).r);
		__h[5] = decode_terrain_height(texelFetch(_height_maps, get_index_coord(__index_id + __offsets.xz), 0).r);

		vec3 __normal[3];
		__normal[0] = normalize(vec3(__h[0] - __h[1], _vertex_spacing, __h[0] - __h[5]));
		__normal[1] = normalize(vec3(__h[2] - __h[4], _vertex_spacing, __h[2] - __h[1]));
		__normal[2] = normalize(vec3(__h[3] - __h[2], _vertex_spacing, __h[3] - __h[0]));

		float __jaggedness = max(length(__normal[2] - __normal[1]),length(__normal[2] - __normal[0]));
		ALBEDO = vec3(0.01 + pow(__jaggedness, 8.));
		ROUGHNESS = 0.7;
		SPECULAR = 0.;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
		AO = 1.0;
	}
#endif
