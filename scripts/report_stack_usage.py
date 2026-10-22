#!/usr/bin/env python3
"""Inventory GCC .su frames for application sources; this is not a peak-stack check."""
import argparse
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path, help='repository root')
    parser.add_argument('builds', type=Path, nargs='+', help='directories containing .su files')
    parser.add_argument('--source-root', type=Path, action='append', default=[],
                        help='compiler-side repository path, e.g. /project in Docker')
    parser.add_argument('--format', choices=('markdown', 'json'), default='markdown')
    args = parser.parse_args()
    root = args.root.resolve()
    source_roots = [root] + [path.resolve() for path in args.source_root]

    def own_source(raw):
        path = Path(raw)
        if path.is_absolute():
            path = path.resolve()
            relative = None
            for prefix in source_roots:
                try:
                    relative = path.relative_to(prefix)
                    break
                except ValueError:
                    continue
            if relative is None:
                return None
        else:
            # GCC may use repo-relative paths or ../source from a build dir.
            parts = list(path.parts)
            while parts and parts[0] == '..':
                parts.pop(0)
            relative = Path(*parts)
        if not relative.parts or relative.parts[0] not in ('source', 'include'):
            return None
        if relative.parts[:2] == ('source', 'expat') or 'third_party' in relative.parts:
            return None
        if relative.parent == Path('include') and relative.name.startswith('expat'):
            return None
        resolved = (root / relative).resolve()
        if root not in resolved.parents or not resolved.is_file():
            return None
        return relative.as_posix()

    inputs = []
    frames = []
    excluded = 0
    visited = set()
    for directory in args.builds:
        directory = directory.resolve()
        if not directory.is_dir():
            parser.error('not a build directory: ' + str(directory))
        files = sorted(directory.rglob('*.su'))
        inputs.append({'directory': str(directory), 'files': len(files)})
        for usage in files:
            if usage in visited:
                continue
            visited.add(usage)
            for number, record in enumerate(usage.read_text().splitlines(), 1):
                if not record.strip():
                    continue
                fields = record.split('\t')
                location = re.fullmatch(r'(.*?):(\d+):(\d+):(.+)', fields[0])
                if len(fields) != 3 or not location or not fields[1].isdigit():
                    parser.error('%s:%d: malformed stack usage record' % (usage, number))
                source, line, column, function = location.groups()
                source = own_source(source)
                if source is None:
                    excluded += 1
                    continue
                frames.append({'build': directory.name, 'source': source,
                               'line': int(line), 'column': int(column),
                               'function': function, 'bytes': int(fields[1]),
                               'qualifier': fields[2], 'usage_file': str(usage)})
    if not visited:
        parser.error('no .su files; build with -fstack-usage first')
    if not frames:
        parser.error('no application frames; check --source-root and build inputs')
    frames.sort(key=lambda row: (-row['bytes'], row['source'], row['line'], row['build']))
    report = {'inputs': inputs, 'excluded_frames': excluded, 'frames': frames}
    if args.format == 'json':
        print(json.dumps(report, indent=2))
        return

    print('# Application stack frames\n')
    for item in inputs:
        print('- `%s`: %d .su files' % (item['directory'], item['files']))
    print('\nOwn frames: %d; excluded vendor/SDK/other records: %d.\n' % (len(frames), excluded))
    print('| Build | Source | Function | Bytes | Qualifier |')
    print('| --- | --- | --- | ---: | --- |')
    for row in frames:
        function = row['function'].replace('|', '\\|').replace('`', "'")
        print('| %s | %s:%d | `%s` | %d | %s |' % (
            row['build'], row['source'], row['line'], function,
            row['bytes'], row['qualifier']))
    print('\nPer-function frames are not peak stack: callers, callbacks, library frames,')
    print('interrupts and runtime stack reservations require separate analysis.')
    print('A dynamic frame without a bounded qualifier has no compiler-proven upper bound.')
    print('No global pass/fail threshold is inferred from this inventory.')


if __name__ == '__main__':
    main()
