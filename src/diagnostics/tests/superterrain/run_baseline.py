#!/usr/bin/env python3
"""ST00 frozen-contract gate. Does not invoke the production renderer."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def run(args, cwd=ROOT):
    print('+ ' + ' '.join(str(v) for v in args), flush=True)
    subprocess.run([str(v) for v in args], cwd=cwd, check=True)


def main():
    run([sys.executable, HERE / 'inventory.py'])
    run([sys.executable, HERE / 'shader_matrix.py'])
    run([sys.executable, HERE / 'corpus.py'])
    run([sys.executable, HERE / 'baseline.py'])
    run([sys.executable, '-m', 'unittest', 'discover', '-s',
         'src/diagnostics/tests/superterrain', '-p', 'test_baseline.py', '-v'])
    compiler = shutil.which('clang++') or shutil.which('g++') or shutil.which('cl')
    if compiler is None:
        raise SystemExit('ST00 native C++17 compiler missing: gate not complete')
    with tempfile.TemporaryDirectory(prefix='st00-contract-') as temp:
        binary = Path(temp) / ('st00_contract.exe' if os.name == 'nt' else 'st00_contract')
        if Path(compiler).name.lower() in ('cl', 'cl.exe'):
            run([compiler, '/nologo', '/std:c++17', '/W4', '/WX', '/permissive-',
                 HERE / 'test_baseline_contract.cpp', '/Fe:' + str(binary)], cwd=temp)
        else:
            run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                 '-O2', HERE / 'test_baseline_contract.cpp', '-o', binary])
        run([binary])
    print('ST00 PASS: inventory, shader matrix, deterministic corpus, static baseline, '
          'Python tests, C++17 contract. Vulkan / MSVC / image parity were not tested.', flush=True)


if __name__ == '__main__':
    main()
