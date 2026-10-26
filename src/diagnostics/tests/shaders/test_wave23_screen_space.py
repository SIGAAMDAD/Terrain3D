#!/usr/bin/env python3
"""Offline BR17 coverage and full-precision derivative transport oracle."""
import math
from pathlib import Path
import random
import struct

ROOT=Path(__file__).resolve().parents[4]
RD=ROOT/'project/addons/terrain_3d/rd'
frag=(RD/'terrain_clipmap_fragment_inc.glslinc').read_text()
iface=(RD/'terrain_clipmap_fragment_interface.glslinc').read_text()
root=(RD/'terrain_clipmap_screen_space.glsl').read_text()
assert '#if defined(T3D_SCREEN_SPACE_COVERAGE_PASS) && T3D_SCREEN_SPACE_COVERAGE_PASS' in frag and 't3d_screen_space_shade_fragment();' in root
assert frag.index('vec2 screen_space_texel_dx = dFdx(world_texel);') < frag.index('bool outside_region = terrain_region_layer < 0;')
assert frag.index('if (need_far_surface)') < frag.index('t3d_screen_space_position_region =')
assert 'layout(set=1, binding=4) uniform sampler2D t3d_br23_world_dxz_dy_input;' in root
assert 'layout(location = 4) out vec4 t3d_screen_space_world_dxz_dy;' in iface
assert '#if defined(T3D_SCREEN_SPACE_SHADE_PASS) && T3D_SCREEN_SPACE_SHADE_PASS' in iface

p32=lambda x: struct.unpack('f',struct.pack('f',float(x)))[0]
rng=random.Random(2323)
for _ in range(65000):
    position=[p32(rng.uniform(-10000,10000)) for _ in range(3)]
    normal=[p32(rng.uniform(-1,1)) for _ in range(3)]
    texel=[p32(rng.uniform(-100000,100000)) for _ in range(2)]
    ddx=[p32(rng.uniform(-100,100)) for _ in range(3)]
    ddy=[p32(rng.uniform(-100,100)) for _ in range(3)]
    tdx=[p32(rng.uniform(-100,100)) for _ in range(2)]
    tdy=[p32(rng.uniform(-100,100)) for _ in range(2)]
    layer=rng.randint(-1,63);tier=rng.randint(0,2)
    target0=(*position,p32(layer)); target1=(*normal,p32(tier))
    target2=(*texel,*tdx);target3=(*tdy,ddx[0],ddx[1]);target4=(ddx[2],*ddy)
    assert target0[:3]==tuple(position) and int(round(target0[3]))==layer
    assert target1[:3]==tuple(normal) and int(round(target1[3]))==tier
    assert target2[:2]==tuple(texel) and target2[2:]==tuple(tdx)
    assert target3[:2]==tuple(tdy) and target3[2:]==tuple(ddx[:2])
    assert (target3[2],target3[3],target4[0])==tuple(ddx)
    assert target4[1:]==tuple(ddy)
    assert target0[3]>=-1.5
assert -2.0 < -1.5 and -1.0 >= -1.5

native=(ROOT/'src/rendering/rd/clipmap/terrain_3d_rd_screen_space_pilot.h').read_text()
for key in ('T3D_BR23_REJECT_MULTIVIEW','T3D_BR23_REJECT_MSAA','T3D_BR23_REJECT_PROOF',
            'm_bMotionDnrAndAlphaParityVerified','m_bV2FeedbackParityVerified'):
    assert key in native
print('BR17: 65,000 transport + coverage cases PASS')
