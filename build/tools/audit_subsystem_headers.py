"""Report project header dependencies by owning subsystem (no external deps)."""
import argparse
import json
import re
from pathlib import Path
from collections import Counter, defaultdict

SRC_TYPES = ('.h', '.hpp', '.cpp', '.inl')
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SUBSYSTEMS = (
    'assets', 'authoring', 'core', 'data', 'diagnostics', 'hlod', 'instances',
    'integration', 'lighting', 'memory', 'physics', 'rendering', 'simd', 'streaming',
)

def owner(path):
    name = path.split('/', 1)[0]
    return name if name in SUBSYSTEMS else 'global'

def analyze(root):
    files = {p.relative_to(root).as_posix(): p for p in root.rglob('*') if p.is_file() and p.suffix in SRC_TYPES}
    edges = defaultdict(list)
    for rel, p in files.items():
        for inc in INCLUDE.findall(p.read_text(errors='replace')):
            candidates = ((p.parent / inc).resolve(), (root / inc).resolve())
            dep = next((v.relative_to(root).as_posix() for v in candidates if v.is_file() and v.is_relative_to(root)), None)
            if dep:
                edges[rel].append(dep)
    totals = {}
    for domain in SUBSYSTEMS + ('global',):
        native = [p for p in files if owner(p) == domain]
        local = Counter()
        for p in native:
            for h in edges[p]:
                local[owner(h)] += 1
        totals[domain] = {
            'sources': sum(p.endswith('.cpp') for p in native),
            'headers': sum(p.endswith(('.h', '.hpp', '.inl')) for p in native),
            'direct_owned_includes': local.pop(domain, 0),
            'cross_owner_includes': dict(sorted(local.items())),
        }
    return {'subsystems': totals, 'all_native_files': len(files), 'direct_local_include_edges': sum(map(len, edges.values()))}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default='src')
    ap.add_argument('--out')
    args = ap.parse_args()
    result = analyze(Path(args.src).resolve())
    output = json.dumps(result, indent=2) + '\n'
    if args.out:
        Path(args.out).write_text(output)
    else:
        print(output)

if __name__ == '__main__':
    main()
