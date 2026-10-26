#!/usr/bin/env python3
"""Install official meshoptimizer v1.3 sources with upstream LICENSE (offline from --archive or GitHub)."""
import argparse
import io
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

TAG = 'v1.3'
URL = 'https://github.com/zeux/meshoptimizer/archive/refs/tags/v1.3.tar.gz'
REQUIRED = ('allocator.cpp', 'indexgenerator.cpp', 'vcacheoptimizer.cpp',
            'vfetchoptimizer.cpp', 'simplifier.cpp', 'meshoptimizer.h')
ROOT = Path(__file__).resolve().parent.parent


def install(raw):
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as archive:
        files = {}
        for member in archive.getmembers():
            if not member.isfile():
                continue
            name = Path(member.name)
            if name.is_absolute() or '..' in name.parts:
                raise ValueError('unsafe upstream archive member')
            relative = Path(*name.parts[1:])
            if relative == Path('LICENSE.md') or (relative.parts[:1] == ('src',) and relative.suffix in ('.cpp', '.h')):
                files[str(relative)] = archive.extractfile(member).read()
    if not all('src/' + filename in files for filename in REQUIRED):
        raise ValueError('missing mandatory meshoptimizer translation units')
    if b'MESHOPTIMIZER_VERSION 1030' not in files['src/meshoptimizer.h']:
        raise ValueError('wrong meshoptimizer release')
    if b'MIT License' not in files.get('LICENSE.md', b''):
        raise ValueError('upstream MIT license missing')
    target = ROOT / 'thirdparty' / 'meshoptimizer'
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent) as temp:
        staging = Path(temp) / 'meshoptimizer'
        staging.mkdir()
        for name, contents in files.items():
            path = staging / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        (staging / 'TERRAIN3D_VENDOR_VERSION').write_text('zeux/meshoptimizer v1.3; source distribution, MESHOPTIMIZER_VERSION=1030\n')
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(staging), str(target))
    print('Installed meshoptimizer v1.3 (LICENSE + %d source files)' % len(files))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, help='official v1.3.tar.gz, for offline installations')
    args = parser.parse_args()
    raw = args.archive.read_bytes() if args.archive else urllib.request.urlopen(URL, timeout=30).read()
    install(raw)
