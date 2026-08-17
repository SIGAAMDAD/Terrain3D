// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_DUAL_SCALING_MIX
		// If dual scaling, apply to overlay texture
		if (id == dual_scale_texture && far_factor > 0.) {
			alb = mix(alb, far_alb, far_factor);
			nrm = mix(nrm, far_nrm, far_factor);
			ao = mix(ao, far_ao, far_factor);
			metallic = mix(metallic, far_metallic, far_factor);
		}
#endif
