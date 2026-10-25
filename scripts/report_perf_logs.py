#!/usr/bin/env python3
"""Summarize existing PERF logs; compare only matching measurement groups."""
import argparse
import csv
import json
from pathlib import Path
import shlex
import statistics
import sys
import zipfile


KEYS = ('format', 'file', 'page', 'zoom', 'target', 'size', 'metric', 'unit')


def read_log(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist()
                     if not n.startswith('__MACOSX/') and
                     Path(n).name == '3dslibris.log']
            if len(names) != 1:
                raise ValueError('ZIP must contain exactly one 3dslibris.log')
            return archive.read(names[0]).decode('utf-8')
    return path.read_text(encoding='utf-8')


def nonnegative(value):
    number = int(value)
    if number < 0:
        raise ValueError('negative measurement')
    return number


def parse_log(path):
    groups, documents, active_files = {}, {}, {}
    info = {'path': str(path), 'metadata': None, 'dropped_records': 0,
            'failed_records': 0, 'unattributed_records': 0}
    for line_number, line in enumerate(read_log(path).splitlines(), 1):
        if 'PERF ' not in line:
            continue
        try:
            fields = dict(token.split('=', 1) for token in
                          shlex.split(line.split('PERF ', 1)[1]))
            if 'schema' in fields:
                metadata = {k: fields[k] for k in
                            ('schema', 'new3ds', 'homebrew', 'units', 'memory')}
                if metadata['schema'] != '1' or metadata['units'] != 'us' or metadata['memory'] != 'bytes':
                    raise ValueError('unsupported PERF schema or units')
                if metadata['new3ds'] not in ('0', '1') or metadata['homebrew'] not in ('0', '1'):
                    raise ValueError('invalid device/runtime')
                if info['metadata'] is not None and info['metadata'] != metadata:
                    raise ValueError('mixed device/runtime metadata')
                # A header starts another process/session in concatenated logs.
                # Pointer and active-file identities cannot cross that boundary.
                documents.clear()
                active_files.clear()
                info['metadata'] = metadata
                continue
            if 'dropped' in fields:
                info['dropped_records'] += nonnegative(fields['dropped'])
                continue
            doc = fields.get('doc')
            if 'file' in fields:
                raw_format, file = fields['format'], fields['file']
                family = raw_format.split('_')[0]
                if raw_format == 'CBZ_ENTRY':
                    previous = documents.get(doc)
                    if previous is None:
                        continue
                    file = previous[1] + '::' + file
                elif '/' in file:
                    active_files[family] = file
                else:
                    active = active_files.get(family)
                    if active and Path(active).name == file:
                        file = active
                documents[doc] = (family, file, fields.get('target', '0x0'))
                continue
            if 'us' not in fields and fields.get('stage') != 'memory':
                continue
            if fields.get('ok', '1') != '1':
                info['failed_records'] += 1
                continue
            if doc not in documents:
                info['unattributed_records'] += 1
                continue
            family, file, target = documents[doc]
            stage = fields['stage']
            if stage == 'memory':
                # Original schema-1 producer sampled only after presentation.
                stage = 'memory.' + fields.get('sample', 'present')
                value, unit = nonnegative(fields['heap_used']), 'bytes'
            else:
                value, unit = nonnegative(fields['us']) / 1000, 'ms'
            key = (family, file, int(fields['page']), int(fields['zoom']),
                   target, fields.get('size', '0x0'), stage, unit)
            groups.setdefault(key, []).append(value)
        except (ValueError, KeyError) as error:
            raise ValueError(f'{path}:{line_number}: invalid PERF record: {error}') from error
    if info['metadata'] is None:
        raise ValueError(f'{path}: missing PERF schema/device metadata')
    if not groups:
        raise ValueError(f'{path}: no attributed measurements')
    info['measurement_groups'] = len(groups)
    return info, groups


def summary(values):
    if values is None:
        return None
    return {'n': len(values), 'median': statistics.median(values),
            'min': min(values), 'max': max(values)}


def report(current_path, reference_path, environment):
    current, current_groups = parse_log(current_path)
    reference, reference_groups = parse_log(reference_path) if reference_path else (None, {})
    for info in (current, reference):
        if info is None:
            continue
        expected = {'new3ds': '1', 'old3ds': '0'}.get(environment)
        if expected is not None and info['metadata']['new3ds'] != expected:
            raise ValueError('environment disagrees with device metadata')
    if reference:
        if current['metadata'] != reference['metadata']:
            raise ValueError('comparison requires the same device/runtime and units')
        if current['dropped_records'] or reference['dropped_records']:
            raise ValueError('comparison refused: dropped PERF events')
        if current['failed_records'] or reference['failed_records']:
            raise ValueError('comparison refused: failed measurements; collect successful runs')
    rows = []
    for key in sorted(current_groups.keys() | reference_groups.keys()):
        row = dict(zip(KEYS, key))
        row['current'] = summary(current_groups.get(key))
        row['reference'] = summary(reference_groups.get(key))
        before, after = row['reference'], row['current']
        # The original PDF producer omits raster dimensions/zoom. Its outer
        # view target cannot distinguish preview from interactive raster work.
        dimensions = row['size'].split('x')
        raster_size_known = (len(dimensions) == 2 and
                             all(v.isdigit() and int(v) > 0 for v in dimensions))
        row['comparable'] = row['metric'] != 'pdf.raster' or raster_size_known
        row['comparison_note'] = ('' if row['comparable'] else
                                  'PDF raster resolution unspecified; descriptive only')
        row['change_percent'] = (100 * (after['median'] / before['median'] - 1)
                                 if row['comparable'] and before and after and before['median'] else None)
        rows.append(row)
    return {'environment': environment, 'current': current, 'reference': reference,
            'limits': 'Elapsed stage times, not CPU time. Heap values are phase samples, not peaks. File labels do not verify input hashes, fonts or cache state.',
            'rows': rows}


def write_report(data, output):
    output.mkdir(parents=True, exist_ok=True)
    (output/'summary.json').write_text(json.dumps(data, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    lines = ['# PERF log report', '', 'Environment: '+data['environment'], '',
             data['limits'], '',
             'Change = (current / reference - 1) × 100; negative means a lower value.',
             'Groups without a reference/current value have no percentage. Repeats are observations, not independent hardware trials.', '',
             '| File | Page / zoom / target / size | Metric | Unit | Reference median (n) | Current median (n) | Change % |',
             '| --- | --- | --- | --- | ---: | ---: | ---: |']
    def display(stats):
        return f"{stats['median']:.3f} ({stats['n']})" if stats else '—'
    with (output/'measurements.csv').open('w', newline='', encoding='utf-8') as fp:
        writer = csv.writer(fp)
        writer.writerow([*KEYS, 'reference_n', 'reference_median', 'current_n', 'current_median', 'change_percent', 'comparison_note'])
        for row in data['rows']:
            before, after = row['reference'], row['current']
            change = row['change_percent']
            writer.writerow([*(row[k] for k in KEYS), *(before[k] if before else '' for k in ('n','median')),
                             *(after[k] if after else '' for k in ('n','median')), change if change is not None else '', row['comparison_note']])
            file = row['file'].replace('|', '\\|')
            delta = f'{change:+.2f}' if change is not None else ('descriptive only' if not row['comparable'] else '—')
            lines.append(f"| {file} | {row['page']} / {row['zoom']} / {row['target']} / {row['size']} | {row['metric']} | {row['unit']} | {display(before)} | {display(after)} | {delta} |")
    for name in ('reference', 'current'):
        info = data[name]
        if info:
            lines += ['', f"{name}: dropped={info['dropped_records']}; failed={info['failed_records']}; unattributed={info['unattributed_records']}."]
    (output/'summary.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--current', required=True, type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--environment', required=True, choices=('emulator', 'new3ds', 'old3ds'))
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        data = report(args.current, args.reference, args.environment)
        write_report(data, args.output)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    print(f"{len(data['rows'])} measurement groups: {args.output/'summary.md'}")


if __name__ == '__main__':
    main()
