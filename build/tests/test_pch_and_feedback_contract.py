"""BLD-01/02 static contract regression gate (stdlib only)."""
from pathlib import Path
import re
import unittest
import sys
from copy import deepcopy

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'src'
sys.path.insert(0, str(ROOT))
from build.pch import build_flavor_pch, pch_eligible

OWNER = 'rendering/rd/vt/core/terrain_3d_vt_feedback_grid.h'
OWNED = re.compile(r'\b(?:t3dVTFeedback(?:Grid(?:Header|Params|Layout|Slot|SlotStatus)?|Word)_t|t3d_vt_feedback_grid_\w+|T3D_VT_FEEDBACK_(?:GRID|SLOT)_\w+)\b')

class DummyEnvironment(dict):
    def Clone(self):
        return DummyEnvironment(deepcopy(dict(self)))
    def subst(self, expr):
        return self[expr[1:]]
    def AppendUnique(self, **kwargs):
        for name, items in kwargs.items():
            self.setdefault(name, [])
            for item in items:
                if item not in self[name]:
                    self[name].append(item)
    def PCH(self, *, target, source):
        self['pch_requested'] = (target, source)
        return [target, target[:-4] + '.obj']

class Tests(unittest.TestCase):
    def test_header_network(self):
        types = (SRC / 'Terrain3DTypes.h').read_text()
        self.assertNotIn('#include "' + OWNER + '"', types)
        owner = (SRC / OWNER).read_text()
        self.assertIn('sizeof( t3dVTFeedbackGridHeader_t ) == 40u', owner)
        self.assertIn('sizeof( t3dVTFeedbackGridParams_t ) == 32u', owner)
        consumers = 0
        for p in SRC.rglob('*'):
            if p.suffix not in ('.cpp', '.h', '.hpp') or p.name == 'terrain_3d_vt_feedback_grid.h':
                continue
            txt = p.read_text(errors='replace')
            if OWNED.search(txt):
                consumers += 1
                self.assertIn('#include "' + OWNER + '"', txt, str(p.relative_to(SRC)))
        self.assertGreater(consumers, 20)

    def test_pch_only_external(self):
        pch = (SRC / 'core/build/terrain_3d_pch.h').read_text()
        self.assertNotIn('#include "Terrain3D', pch)
        for line in pch.splitlines():
            if line.lstrip().startswith('#include'):
                self.assertRegex(line, r'#include\s+<')

    def test_modes_and_exclusions(self):
        env = DummyEnvironment(CXX='cl.exe', BUILDERS={'PCH': 'present'}, CPPPATH=[], CXXFLAGS=[])
        off, nodes = build_flavor_pch(env, 'editor', 'off', source_root=ROOT)
        self.assertFalse(nodes)
        self.assertNotIn('PCH', off)
        self.assertEqual(env['CXXFLAGS'], [])
        enabled, nodes = build_flavor_pch(env, 'runtime', 'stable', source_root=ROOT)
        self.assertEqual(len(nodes), 2)
        self.assertIn('/FIterrain_3d_pch.h', enabled['CXXFLAGS'])
        self.assertIn('runtime/pch', nodes[0].replace('\\','/'))
        self.assertTrue(pch_eligible('src/data/model/terrain_3d_data.cpp'))
        self.assertFalse(pch_eligible('src/simd/backends/foo.cpp'))
        self.assertFalse(pch_eligible('src/diagnostics/tests/foo.cpp'))
        self.assertFalse(pch_eligible('src/integration/godot/register_types.cpp'))
        with self.assertRaises(ValueError):
            build_flavor_pch(env, 'editor', 'invalid', source_root=ROOT)
        with self.assertRaises(RuntimeError):
            build_flavor_pch(DummyEnvironment(CXX='clang++', BUILDERS={'PCH':'yes'}), 'editor', 'stable', source_root=ROOT)

if __name__ == '__main__':
    unittest.main()
