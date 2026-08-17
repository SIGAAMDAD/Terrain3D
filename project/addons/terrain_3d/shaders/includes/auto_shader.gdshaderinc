// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_AUTO_SHADER
	{
		// Auto blend calculation
		float auto_blend = clamp(fma(auto_slope * 2.0, (w_normal.y - 1.0), 1.0)
			- auto_height_reduction * 0.01 * v_vertex.y, 0.0, 1.0);
		// Enable Autoshader if outside regions or painted in regions, otherwise manual painted
		uvec4 is_auto = (control & uvec4(0x1u)) |
			uvec4(lessThan(ivec4(index[0].z, index[1].z, index[2].z, index[3].z), ivec4(0)));
		int auto_layer_max = max(_texture_array_layer_count - 1, 0);
		uint authored_base_id = uint(clamp(auto_base_texture, 0, auto_layer_max));
		uint authored_overlay_id = uint(clamp(auto_overlay_texture, 0, auto_layer_max));
		bool swap_pair = authored_base_id > authored_overlay_id;
		uint auto_base_id = swap_pair ? authored_overlay_id : authored_base_id;
		uint auto_overlay_id = swap_pair ? authored_base_id : authored_overlay_id;
		float canonical_blend = swap_pair ? 1.0 - auto_blend : auto_blend;
		uint u_auto =
			((auto_base_id & 0x1Fu) << 27u) |
			((auto_overlay_id & 0x1Fu) << 22u) |
			((uint(fma(canonical_blend, 255.0 , 0.5)) & 0xFFu) << 14u);
		control = control * (1u - is_auto) + u_auto * is_auto;
	}
#endif
