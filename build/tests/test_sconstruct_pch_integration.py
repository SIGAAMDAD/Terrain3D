"""Verify the actual SConstruct connects the BLD-01 PCH producer/consumers.

These are static wiring checks; the final MSVC /Yc and /Yu smoke build is run on
Windows against the vendored godot-cpp 4.7 API and cannot be simulated here.
"""
import ast
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCONS = ROOT / "SConstruct"
PCH_TU = ROOT / "src/core/build/terrain_3d_pch.cpp"


class SConstructPchWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = SCONS.read_text(encoding="utf-8")
        cls.tree = ast.parse(cls.script)

    def test_mode_is_hidden_from_godot_cpp_option_parser(self):
        self.assertIn('"pch",', self.script)
        self.assertIn('ARGUMENTS.pop(name)', self.script)
        self.assertIn('ARGUMENTS.update(terrain3d_options)', self.script)
        self.assertLess(self.script.index('ARGUMENTS.pop(name)'),
                        self.script.index('SConscript("godot-cpp/SConstruct"'))
        self.assertGreater(self.script.index('ARGUMENTS.update(terrain3d_options)'),
                           self.script.index('SConscript("godot-cpp/SConstruct"'))

    def test_producer_is_not_compiled_as_an_ordinary_source(self):
        self.assertTrue(PCH_TU.is_file())
        self.assertIn('PCH_SOURCE = SOURCE_ROOT / "core/build/terrain_3d_pch.cpp"', self.script)
        self.assertIn('path not in (GENERATED_DOC, PCH_SOURCE)', self.script)

    def test_pch_config_follows_native_config_and_precedes_objects(self):
        self.assertLess(self.script.index('env.AppendUnique(CPPDEFINES=[(name, value) for name, value in shader_defaults.items()])'),
                        self.script.index('pch_env, pch_nodes = build_flavor_pch('))
        self.assertLess(self.script.index('env.Append(CPPPATH=['),
                        self.script.index('pch_env, pch_nodes = build_flavor_pch('))
        self.assertLess(self.script.index('pch_env, pch_nodes = build_flavor_pch('),
                        self.script.index('objects.extend(object_env.SharedObject('))
        self.assertIn('build_dir=obj_dir', self.script)
        self.assertIn('"editor" if native_target == "editor" else "runtime"', self.script)

    def test_pch_options_and_exemptions(self):
        self.assertIn('pch_default = "stable" if env.get("is_msvc", False) else "off"', self.script)
        self.assertIn('pch_eligible(source_path)', self.script)
        self.assertIn('use_pch = False', self.script)
        self.assertIn('object_env = pch_env if use_pch else env', self.script)
        self.assertIn('object_env.AppendUnique(CPPDEFINES=["T3D_TESTS_ENABLED"])', self.script)
        self.assertIn('env.Alias("terrain3d-pch", pch_nodes[0])', self.script)

    def test_pch_producer_object_is_linked_once_when_pch_enabled(self):
        # With the base env as link_env, SCons does not automatically insert
        # the /Yc companion object. Omitting it causes MSVC LNK2011.
        self.assertIn('if pch_nodes:', self.script)
        self.assertIn('if len(pch_nodes) != 2:', self.script)
        self.assertEqual(self.script.count('objects.append(pch_nodes[1])'), 1)
        self.assertNotIn('objects.extend(pch_nodes)', self.script)
        self.assertLess(self.script.index('objects.append(pch_nodes[1])'),
                        self.script.index('library = env.SharedLibrary('))
        self.assertIn('source=objects', self.script)

    def test_pch_link_has_a_matching_producer_and_consumer_contract(self):
        adapter = (ROOT / 'build/pch.py').read_text(encoding='utf-8')
        self.assertIn('cloned["PCH"] = nodes[0]', adapter)
        self.assertIn('return cloned, tuple(nodes)', adapter)
        self.assertIn('object_env = pch_env if use_pch else env', self.script)
        self.assertIn('objects.append(pch_nodes[1])', self.script)


if __name__ == "__main__":
    unittest.main()
