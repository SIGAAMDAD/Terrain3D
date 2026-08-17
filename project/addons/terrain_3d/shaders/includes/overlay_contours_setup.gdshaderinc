// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifdef T3D_ENABLE_OVERLAY_CONTOURS_SETUP
group_uniforms shader_uniforms.contour_lines;
uniform float contour_interval: hint_range(0.25, 100.0, 0.001) = 1.0;
uniform float contour_thickness : hint_range(0.0, 10.0, 0.001) = 1.0;
uniform vec4 contour_color : source_color = vec4(.85, .85, .19, 1.);
group_uniforms;

float fractal_contour_lines(float thickness, float interval, vec3 spatial_coords, vec3 normal, vec3 base_ddx, vec3 base_ddy, vec3 __camera_pos) {
    float depth = max(log(length(spatial_coords - __camera_pos) / interval) * (1.0 / log2(2.0)) - 1.0, 1.0);

    float interval_a = interval * exp2(max(floor(depth) - 1.0, 1.0)) * 0.5;
    float interval_b = interval * exp2(max(floor(depth), 1.0)) * 0.5;
    float interval_c = interval * exp2(max(floor(depth + 0.5) - 1.0, 1.0)) * 0.5;

    float y = spatial_coords.y;
    float y_fwidth = abs(base_ddx.y) + abs(base_ddy.y);

    thickness *= smoothstep(0., 0.0125, clamp(1.0 - normal.y, 0., 1.));
    float mi = max(0.0, thickness - 1.0);
    float ma = max(1.0, thickness);
    float mx = max(0.0, 1.0 - thickness);

    // Line A
    float inv_interval_a = 1.0 / interval_a;
    float f_a = abs(fract((y + interval_a * 0.5) * inv_interval_a) - 0.5);
    float df_a = y_fwidth * inv_interval_a;
    float line_a = clamp((f_a - df_a * mi) / (df_a * (ma - mi)), mx, 1.0);

    // Line B
    float inv_interval_b = 1.0 / interval_b;
    float f_b = abs(fract((y + interval_b * 0.5) * inv_interval_b) - 0.5);
    float df_b = y_fwidth * inv_interval_b;
    float line_b = clamp((f_b - df_b * mi) / (df_b * (ma - mi)), mx, 1.0);

    // Line C
    float inv_interval_c = 1.0 / interval_c;
    float f_c = abs(fract((y + interval_c * 0.5) * inv_interval_c) - 0.5);
    float df_c = y_fwidth * inv_interval_c;
    float line_c = clamp((f_c - df_c * mi) / (df_c * (ma - mi)), mx, 1.0);

    // Blend out
    float p = fract(depth - 0.5);
    float line = mix(mix(line_a, line_b, fract(depth)), line_c, (4.0 * p * (1.0 - p)));
    return line;
}
#endif
