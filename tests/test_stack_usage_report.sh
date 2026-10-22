#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(sys.argv.pop()) / 'scripts/report_stack_usage.py'


class StackReportTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='3dslibris-stack-report-')
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        for path in ('source/app.cpp', 'include/shared/helper.h', 'source/expat/xmlparse.c'):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.touch()
        self.release = self.root / 'build'
        self.debug = self.root / 'build-debug'
        self.release.mkdir()
        self.debug.mkdir()

    def run_report(self, *options):
        return subprocess.run([sys.executable, str(SCRIPT), str(self.root),
                               str(self.release), str(self.debug), *options],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_own_frames_keep_variants_and_exclude_vendor_and_sdk(self):
        (self.release / 'app.su').write_text(
            f'{self.root}/source/app.cpp:10:4:void open_book()\t20112\tstatic\n'
            f'{self.root}/include/shared/helper.h:8:1:helper()\t64\tdynamic,bounded\n'
            f'{self.root}/source/expat/xmlparse.c:3:1:vendor()\t50000\tstatic\n'
            f'{self.root}/third_party/library/source/app.cpp:10:4:vendor()\t90000\tstatic\n'
            '/opt/sdk/include/vector:20:1:system()\t80000\tstatic\n')
        (self.debug / 'app.su').write_text(
            f'{self.root}/source/app.cpp:10:4:void open_book()\t20040\tstatic\n')
        result = self.run_report('--format', 'json')
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data['excluded_frames'], 3)
        self.assertEqual([(f['build'], f['bytes']) for f in data['frames']],
                         [('build', 20112), ('build-debug', 20040), ('build', 64)])
        self.assertEqual(data['frames'][-1]['qualifier'], 'dynamic,bounded')
        self.assertEqual(data['frames'][0]['source'], 'source/app.cpp')

    def test_compiler_root_alias_and_unbounded_dynamic_warning(self):
        (self.release / 'app.su').write_text(
            '/project/source/app.cpp:10:4:void open_book()\t128\tdynamic\n')
        result = self.run_report('--source-root', '/project')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('128', result.stdout)
        self.assertIn('dynamic', result.stdout)
        self.assertIn('not peak stack', result.stdout)
        self.assertIn('source/app.cpp:10', result.stdout)

    def test_missing_or_malformed_input_fails(self):
        result = self.run_report()
        self.assertNotEqual(result.returncode, 0)
        (self.release / 'app.su').write_text('invalid stack usage record\n')
        result = self.run_report()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('app.su:1', result.stderr)


unittest.main()
PY
