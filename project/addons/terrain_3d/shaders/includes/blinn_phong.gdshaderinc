// x = N.L
// y = (N.H)^8
// z = (N.H)^16
// w = (N.H)^32
vec4 terrain3d_blinn_terms(
	vec3 normal,
	vec3 light_dir,
	vec3 view_dir
) {
	float ndotl = max(dot(normal, light_dir), 0.0);

	// Never produce a specular lobe from behind the surface.
	if (ndotl <= 0.0) {
		return vec4(0.0);
	}

	vec3 half_sum = light_dir + view_dir;
	float half_len_sq = dot(half_sum, half_sum);

	// LIGHT == -VIEW makes the Blinn half-vector undefined.
	if (half_len_sq <= 1e-6) {
		return vec4(ndotl, 0.0, 0.0, 0.0);
	}

	vec3 half_vector = half_sum * inversesqrt(half_len_sq);
	float nh = max(dot(normal, half_vector), 0.0);

	float nh2 = nh * nh;
	float nh4 = nh2 * nh2;
	float nh8 = nh4 * nh4;
	float nh16 = nh8 * nh8;
	float nh32 = nh16 * nh16;

	return vec4(ndotl, nh8, nh16, nh32);
}
