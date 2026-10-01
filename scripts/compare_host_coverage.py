#!/usr/bin/env python3
"""Check coverage loss against a frozen inventory, including missing files as zero."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('root', type=Path)
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
parser.add_argument('--max-loss', type=float, default=2.0,
                    help='maximum percentage-point and relative loss (default: 2)')
args = parser.parse_args()
if not 0 <= args.max_loss <= 100:
    parser.error('--max-loss must be between 0 and 100')
root = args.root.resolve()


def own_files(path):
    data = json.loads(path.read_text())['data'][0]
    return {Path(f['filename']).resolve(): f['summary'] for f in data['files']
            if (root / 'source' in Path(f['filename']).resolve().parents
                or root / 'include' in Path(f['filename']).resolve().parents)
            and root / 'source/expat' not in Path(f['filename']).resolve().parents
            and Path(f['filename']).resolve() != root / 'source/core/stb_image_impl.cpp'}


before, after = own_files(args.before), own_files(args.after)
if not before:
    parser.error('baseline contains no own source/include coverage')
missing = sorted(before.keys() - after.keys())
print('Frozen inventory: %d files; absent after: %d' % (len(before), len(missing)))
for path in missing:
    print('  Missing: ' + str(path.relative_to(root)))
failed = False
for metric in ('lines', 'branches'):
    total = sum(value[metric]['count'] for value in before.values())
    if total == 0:
        continue
    old = sum(value[metric]['covered'] for value in before.values())
    # Summaries cannot identify which locations changed. Give changed mappings
    # zero credit rather than let newly measured functions mask old losses.
    changed = sorted(path for path, value in before.items() if path in after
                     and value[metric]['count'] != after[path][metric]['count'])
    for path in changed:
        print('  Changed %s mapping, credited as zero: %s (%d -> %d)' % (
            metric, path.relative_to(root), before[path][metric]['count'],
            after[path][metric]['count']))
    new = sum(after[path][metric]['covered']
              for path, value in before.items() if path in after
              and value[metric]['count'] == after[path][metric]['count'])
    loss = 100.0 * (old - new) / total
    relative_loss = 100.0 * (old - new) / old if old else 0.0
    print('%s: %d/%d -> %d/%d; loss %.3f pp (%.3f%% relative)' % (
        metric, old, total, new, total, loss, relative_loss))
    # Check both interpretations of "within 2%": points and relative decrease.
    failed |= loss > args.max_loss or relative_loss > args.max_loss
print('FAIL: loss exceeds limit or changed mappings need location-level review'
      if failed else 'PASS: conservative summary comparison')
raise SystemExit(1 if failed else 0)
