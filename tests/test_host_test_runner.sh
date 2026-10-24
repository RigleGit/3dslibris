#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

source = Path(sys.argv.pop()) / 'tests'


class HostTestRunnerTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='3dslibris-runner-test-')
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.scratch = self.root / 'scratch'
        self.scratch.mkdir()
        self.checkout = self.root / 'checkout'
        tests = self.checkout / 'tests'
        tests.mkdir(parents=True)
        for name in ('run_all_tests.sh', 'test_build.sh'):
            shutil.copyfile(source / name, tests / name)
        dependency = self.checkout / 'third_party/utf8proc'
        dependency.mkdir(parents=True)
        (dependency / 'utf8proc.c').write_text(
            'int fixture_value(void) { return TEST_VALUE; }\n')
        (tests / 'contract.cpp').write_text('''#include <assert.h>
extern "C" int fixture_value(void);
int main() {
    int assertions = 0;
    assert(++assertions);
    return assertions != 1 || fixture_value() != TEST_VALUE;
}
''')
        (tests / 'test_contract.sh').write_text('''set -eu
printf '%s\\n' "$TMPDIR" > "$RUN_RECEIPT"
if [ "${RUN_FAIL:-0}" = 1 ]; then
  exit 9
fi
source "$(dirname "$0")/test_build.sh"
build_test contract "$TEST_ROOT/tests/contract.cpp"
# Integration runners also invoke the compiler directly after sourcing the helper.
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} "$TEST_ROOT/tests/contract.cpp" \
  "$TEST_OUTDIR/utf8proc.o" ${LDFLAGS:-} -o "$TEST_OUTDIR/contract-direct"
"$TEST_OUTDIR/contract-direct"
''')

    def start(self, name, value=1, checkout=None, from_tests=False,
              ndebug=False, fail=False, keep=False):
        checkout = checkout or self.checkout
        receipt = self.root / (name + '.receipt')
        env = dict(os.environ)
        env.update(TMPDIR=str(self.scratch), RUN_RECEIPT=str(receipt),
                   RUN_FAIL=str(int(fail)),
                   DSLIBRIS_TEST_KEEP_OUTPUT=str(int(keep)))
        env['CFLAGS'] = env.get('CFLAGS', '') + ' -DTEST_VALUE=%d' % value
        env['CXXFLAGS'] = (env.get('CXXFLAGS', '') +
                           ' -DTEST_VALUE=%d' % value +
                           (' -DNDEBUG' if ndebug else ''))
        process = subprocess.Popen(
            ['bash', str(checkout / 'tests/run_all_tests.sh')],
            cwd=checkout / 'tests' if from_tests else checkout,
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(process.communicate, timeout=60)
        return process, receipt

    def finish(self, pending, expected=0):
        process, receipt = pending
        stdout, stderr = process.communicate(timeout=60)
        self.assertEqual(process.returncode, expected, stdout + stderr)
        self.assertTrue(receipt.is_file(), stdout + stderr)
        return Path(receipt.read_text().strip())

    def test_changed_flags_rebuild_from_root_and_tests_directory(self):
        first = self.finish(self.start('first', value=11))
        second = self.finish(self.start('second', value=23, from_tests=True))
        self.assertNotEqual(first, second)
        self.assertFalse(first.exists())
        self.assertFalse(second.exists())
        self.assertTrue(self.scratch.is_dir())

    def test_asserts_remain_active_with_inherited_ndebug(self):
        self.finish(self.start('asserts', ndebug=True))

    def test_concurrent_checkouts_have_independent_outputs(self):
        other = self.root / 'other-checkout'
        shutil.copytree(self.checkout, other)
        first = self.start('concurrent-first', value=31)
        second = self.start('concurrent-second', value=47, checkout=other)
        first_dir = self.finish(first)
        second_dir = self.finish(second)
        self.assertNotEqual(first_dir, second_dir)
        self.assertFalse(first_dir.exists())
        self.assertFalse(second_dir.exists())

    def test_failure_cleans_outputs_and_coverage_can_retain_them(self):
        failed_dir = self.finish(self.start('failure', fail=True), expected=1)
        self.assertFalse(failed_dir.exists())
        kept_dir = self.finish(self.start('coverage', keep=True))
        self.assertTrue((kept_dir / '3dslibris-tests/contract').is_file())
        self.assertTrue(self.scratch.is_dir())


unittest.main()
PY
