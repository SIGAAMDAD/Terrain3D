// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

float ndotl = max(dot(NORMAL, LIGHT), 0.0);
float wrapped_diffuse = clamp((ndotl + 0.12) / 1.12, 0.0, 1.0);
DIFFUSE_LIGHT += LIGHT_COLOR * wrapped_diffuse * ATTENUATION / PI;
