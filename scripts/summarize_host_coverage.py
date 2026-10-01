#!/usr/bin/env python3
"""Describe measured host coverage without treating unlinked code as covered."""
import json
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
summary = json.loads(Path(sys.argv[2]).read_text())['data'][0]
output = Path(sys.argv[3])
all_files = {Path(f['filename']).resolve(): f['summary'] for f in summary['files']}
files = {path: value for path, value in all_files.items()
         if (root / 'source' in path.parents or root / 'include' in path.parents)
         and root / 'source/expat' not in path.parents
         and path != root / 'source/core/stb_image_impl.cpp'}
external = {path: value for path, value in all_files.items()
            if root not in path.parents}
candidates = ([root / name for name in Path(sys.argv[4]).read_text().splitlines()]
              if len(sys.argv) > 4 and sys.argv[4] else (root / 'source').rglob('*.cpp'))
sources = sorted(p.resolve() for p in candidates if p.suffix == '.cpp'
                 and 'expat' not in p.relative_to(root / 'source').parts
                 and p.name != 'stb_image_impl.cpp')
modules = {}
for path in sources:
    parts = path.relative_to(root / 'source').parts
    module = '/'.join(parts[:2]) if parts[0] == 'formats' else parts[0]
    entry = modules.setdefault(module, {'total':0, 'measured':0, 'lines':0,
                                       'covered':0, 'branches':0, 'branch_covered':0})
    entry['total'] += 1
    if path in files:
        entry['measured'] += 1
        for metric, key in [('lines','covered'), ('branches','branch_covered')]:
            entry[metric] += files[path][metric]['count']
            entry[key] += files[path][metric]['covered']


def percent(covered, count):
    return '%.1f%%' % (100.0 * covered / count) if count else '—'


lines = ['# Cobertura host de 3dslibris', '',
         'Las métricas LLVM solo describen código instrumentado. Los archivos sin',
         'instrumentación no tienen denominador de líneas ejecutables disponible;',
         'no se incluyen en el porcentaje ni se presenta un porcentaje global ficticio.', '',
         '| Métrica LLVM (source + include instrumentados) | Cubierto | Total | Porcentaje |',
         '|---|---:|---:|---:|']
for metric in ['lines', 'branches', 'functions']:
    covered = sum(value[metric]['covered'] for value in files.values())
    count = sum(value[metric]['count'] for value in files.values())
    lines.append('| %s | %d | %d | %s |' % (metric, covered, count,
                                             percent(covered, count)))
measured = sum(p in files for p in sources)
lines += ['', '**Archivos .cpp propios instrumentados: %d/%d.**' % (measured, len(sources)), '',
          'Se excluyen fuentes vendorizadas de Expat y la implementación de stb.',
          'Instrumentado no significa cubierto completamente: algunos archivos',
          'solo aportan funciones extraídas para un harness de plataforma.', '',
          '| Módulo (solo .cpp) | Archivos medidos/propios | Líneas ejecutadas/medidas | Ramas ejecutadas/medidas |',
          '|---|---:|---:|---:|']
for name, e in sorted(modules.items()):
    lines.append('| %s | %d/%d | %d/%d (%s) | %d/%d (%s) |' % (
        name, e['measured'], e['total'], e['covered'], e['lines'],
        percent(e['covered'], e['lines']), e['branch_covered'], e['branches'],
        percent(e['branch_covered'], e['branches'])))
if external:
    lines += ['', '## Fragmentos de los harnesses de plataforma', '',
              'LLVM atribuye estos fragmentos a includes temporales; no se suman al',
              'porcentaje source/include ni prueban el fichero de producción completo.', '',
              '| Fragmento | Líneas ejecutadas/medidas |', '|---|---:|']
    for path, value in sorted(external.items()):
        lines.append('| %s | %d/%d |' % (path.name, value['lines']['covered'],
                                             value['lines']['count']))
if len(sys.argv) > 5:
    diagnostics = Path(sys.argv[5]).read_text().strip()
    if diagnostics:
        lines += ['', '## Limitación de la medición LLVM', '',
                  'LLVM ha emitido diagnósticos sobre funciones con datos no coincidentes',
                  'al combinar los binarios de los tests. Los porcentajes son',
                  'orientativos; no constituyen una medición exacta de toda la aplicación.',
                  '', '```text', diagnostics, '```']
lines += ['', '## Archivos sin instrumentación host', '']
lines += ['- `%s`' % p.relative_to(root).as_posix() for p in sources if p not in files]
lines += ['', 'Estos datos no certifican FreeType, libctru, GPU, SD ni workers reales en consola.', '']
output.write_text('\n'.join(lines))
print('Application coverage inventory written to: ' + str(output))
