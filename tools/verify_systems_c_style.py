#!/usr/bin/env python3
from pathlib import Path
import re
import sys
import argparse

ROOT = Path(__file__).resolve().parents[1] / "src"
SUFFIXES = {".h", ".cpp"}
errors = []
parser = argparse.ArgumentParser(description='Check repository conventions for its 2009-2012 C++ style.')
parser.add_argument('--strict-2009', action='store_true', help='Also reject C++11 auto, lambdas and range loops')
style_args = parser.parse_args()


def strip_comments_and_strings(line: str) -> str:
    out=[]; i=0; quote=None; esc=False
    while i < len(line):
        ch=line[i]
        if quote:
            if esc:
                esc=False
            elif ch == '\\':
                esc=True
            elif ch == quote:
                quote=None
            out.append(' ')
            i += 1
            continue
        if ch in ('"', "'"):
            quote=ch; out.append(' '); i += 1; continue
        if ch == '/' and i + 1 < len(line) and line[i+1] == '/':
            break
        out.append(ch); i += 1
    return ''.join(out)

def is_range_for(code: str) -> bool:
    m=re.search(r'\bfor\s*\(', code)
    if not m: return False
    start=code.find('(', m.start())
    depth=0
    for i in range(start, len(code)):
        ch=code[i]
        if ch=='(': depth += 1
        elif ch==')':
            depth -= 1
            if depth==0:
                body=code[start+1:i]
                if ';' in body: return False
                return re.search(r'(?<!:):(?!:)', body) is not None
    return False

def has_lambda(code: str) -> bool:
    # Exclude operator[]; require a closing capture bracket followed by optional
    # params/specifiers and either '(' or '{'.
    if 'operator[]' in code:
        return False
    return re.search(r'(?<![A-Za-z0-9_\]])\[[^\]\n]*\]\s*(?:\([^\n;{}]*\)\s*)?(?:mutable\s*)?(?:noexcept\s*)?(?:->[^\n{]+)?\{', code) is not None

for path in ROOT.rglob('*'):
    if path.suffix not in SUFFIXES:
        continue
    try:
        lines=path.read_text().splitlines()
    except UnicodeDecodeError:
        continue
    rel=path.relative_to(ROOT)
    for n,line in enumerate(lines,1):
        code=strip_comments_and_strings(line)
        checks=[
            ('generated range index', r'\biT3dRange\d+\b'),
            ('nullptr', r'\bnullptr\b'),
            ('std memory operation', r'\bstd::(?:memcpy|memmove|memset|memcmp)\b'),
            ('std algorithm', r'\bstd::(?:fill|fill_n|copy|copy_n|move|sort|stable_sort|find|find_if|accumulate|min|max|clamp|swap)\b'),
            ('static_cast; use project C-style cast convention', r'\bstatic_cast\s*<'),
            ('reinterpret_cast; use project C-style cast convention', r'\breinterpret_cast\s*<'),
        ]
        for label,rx in checks:
            if re.search(rx,code): errors.append((rel,n,label,line.strip()))
        if style_args.strict_2009 and is_range_for(code): errors.append((rel,n,'range-for',line.strip()))
        if re.search(r'\[[^\]\n]*\]\s*\([^)]*\bauto\b', code): errors.append((rel,n,'generic lambda requires C++14',line.strip()))
        if style_args.strict_2009 and has_lambda(code): errors.append((rel,n,'lambda',line.strip()))
        if style_args.strict_2009 and re.search(r'\bauto\b',code) and not line.lstrip().startswith('*'): errors.append((rel,n,'auto',line.strip()))
        if line.strip() == '#pragma once': errors.append((rel,n,'pragma once; use the repository include-guard convention',line.strip()))
        if 'std::' in code and rel.as_posix() not in {'core/utility/terrain_3d_sync.h', 'core/utility/terrain_3d_binary_io.h'} and 'std::' in code.replace('std::nothrow', ''):
            errors.append((rel,n,'std namespace leakage outside synchronization facade',line.strip()))
        wrapper_match = re.match(r'\s*#include\s*<(algorithm|numeric|limits|type_traits|cstdint|cstddef|cstring|cmath|cstdlib|cstdio|cfloat|climits|cassert|utility)>', line)
        if wrapper_match and not ( (rel.as_posix() == 'core/utility/terrain_3d_sync.h' and wrapper_match.group(1) == 'utility') or (rel.as_posix() == 'core/utility/terrain_3d_binary_io.h' and wrapper_match.group(1) == 'type_traits') ):
            errors.append((rel,n,'forbidden C++ standard wrapper/STL header',line.strip()))
        if re.match(r'\s*(if|for|while)\s*\([^\n]*\)\s*(return|continue|break)\b',code):
            errors.append((rel,n,'unbraced single-line control',line.strip()))
# Header guards use one project family and must wrap the complete authored header.
for path in ROOT.rglob('*.h'):
    lines = path.read_text().splitlines()
    index = 0
    while index < len(lines) and ( not lines[index].strip() or lines[index].strip().startswith('//') ):
        index += 1
    rel = path.relative_to(ROOT)
    if index >= len(lines):
        errors.append((rel, 1, 'missing header guard', ''))
        continue
    match = re.match(r'#ifndef\s+(TERRAIN3D_[A-Z0-9_]+)$', lines[index].strip())
    if not match:
        errors.append((rel, index + 1, 'header guard must use TERRAIN3D_*', lines[index].strip()))
        continue
    guard = match.group(1)
    define_index = index + 1
    while define_index < len(lines) and ( not lines[define_index].strip() or lines[define_index].strip().startswith('//') ):
        define_index += 1
    if define_index >= len(lines) or lines[define_index].strip() != '#define ' + guard:
        errors.append((rel, define_index + 1, 'header guard define mismatch', lines[define_index].strip() if define_index < len(lines) else 'EOF'))
        continue
    last_index = len(lines) - 1
    while last_index >= 0 and not lines[last_index].strip():
        last_index -= 1
    if last_index < 0 or lines[last_index].strip() != '#endif // ' + guard:
        errors.append((rel, last_index + 1, 'header guard footer mismatch', lines[last_index].strip() if last_index >= 0 else 'EOF'))

# Project-local includes should be rooted at the configured source include path.
for path in ROOT.rglob('*'):
    if path.suffix not in SUFFIXES:
        continue
    rel = path.relative_to(ROOT)
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if re.match(r'\s*#include\s*"\.\.?/', line):
            if not ( rel.as_posix() == 'core/runtime/eastl_runtime.cpp' and '../thirdparty/EASTL/source/hashtable.cpp' in line ):
                errors.append((rel, n, 'relative project include', line.strip()))

if errors:
    for rel,n,label,line in errors:
        print(f'{rel}:{n}: {label}: {line}')
    print(f'FAIL: {len(errors)} systems-C style violations')
    sys.exit(1)
print('PASS: Terrain3D systems-C conventions, ' + ('strict 2009' if style_args.strict_2009 else '2009-2012 language features'))
