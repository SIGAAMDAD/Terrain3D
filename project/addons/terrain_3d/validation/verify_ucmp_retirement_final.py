#!/usr/bin/env python3
"""RET-07 unified terrain material and deletion acceptance gate."""
import sys
from pathlib import Path
root=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(root/'src/diagnostics/tests/shader_retirement'))
from verify_retirement import check
check(root)
assert 'm_szTier[1]' in (root/'src/Terrain3DTypes.h').read_text()
assert 'sizeof( t3dRdMaterialConfig_t ) == 288u' in (root/'src/rendering/rd/core/terrain_3d_rd_types.h').read_text()
assert 'TierMaterialConfig tier[1];' in (root/'project/addons/terrain_3d/rd/terrain_clipmap_contract.glslinc').read_text()
print('PASS: RET-07 UCMP material ABI and shader retirement')
