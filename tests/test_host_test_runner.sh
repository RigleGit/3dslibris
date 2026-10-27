#!/usr/bin/env bash
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import os
import json
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
        for name in ('run_all_tests.sh', 'test_build.sh', 'host_compile.py',
                     'host_cc.sh', 'host_cxx.sh'):
            if (source / name).exists():
                shutil.copyfile(source / name, tests / name)
                (tests / name).chmod((source / name).stat().st_mode)
        dependency = self.checkout / 'third_party/utf8proc'
        dependency.mkdir(parents=True)
        self.header = dependency / 'fixture.h'
        self.header.write_text('#define HEADER_OFFSET 0\n')
        self.shadow = self.root / 'shadow'
        self.shadow.mkdir()
        (dependency / 'utf8proc.c').write_text(
            '#include <fixture.h>\nint fixture_value(void) { return TEST_VALUE + HEADER_OFFSET; }\n')
        self.calls = self.root / 'compiler-calls'
        for kind, compiler in (('cc', 'cc'), ('cxx', 'c++')):
            tool = self.root / ('trace-' + kind)
            tool.write_text('#!/usr/bin/env python3\nimport os,sys,json\n'
                'with open(os.environ["COMPILER_CALLS"],"a") as f: '
                'f.write(json.dumps(sys.argv[1:])+"\\n")\n'
                'os.execv(' + repr(shutil.which(compiler)) + ', [' +
                repr(compiler) + ', *sys.argv[1:]])\n')
            tool.chmod(0o755)
        (tests / 'contract.cpp').write_text('''#include <assert.h>
#include <cstdlib>
extern "C" int fixture_value(void);
int main() {
    int assertions = 0;
    assert(++assertions);
    return assertions != 1 || fixture_value() != std::atoi(std::getenv("EXPECTED_VALUE"));
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
              ndebug=False, fail=False, keep=False, trace=False, cache=True,
              extra_flags=''):
        checkout = checkout or self.checkout
        receipt = self.root / (name + '.receipt')
        env = dict(os.environ)
        env.update(TMPDIR=str(self.scratch), RUN_RECEIPT=str(receipt),
                   RUN_FAIL=str(int(fail)),
                   EXPECTED_VALUE=str(value),
                   DSLIBRIS_TEST_KEEP_OUTPUT=str(int(keep)))
        if trace:
            env.pop('LLVM_PROFILE_FILE', None)
            env.update(CC=str(self.root/'trace-cc'), CXX=str(self.root/'trace-cxx'),
                       CFLAGS='', CXXFLAGS='', LDFLAGS='',
                       DSLIBRIS_TEST_CACHE='1', COMPILER_CALLS=str(self.calls))
        if not cache:
            env['DSLIBRIS_TEST_CACHE'] = '0'
        env['CFLAGS'] = (env.get('CFLAGS', '') + ' -DTEST_VALUE=%d -I%s' % (value, self.shadow))
        env['CXXFLAGS'] = (env.get('CXXFLAGS', '') +
                           ' -DTEST_VALUE=%d' % value +
                           (' -DNDEBUG' if ndebug else '') + ' ' + extra_flags)
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
        self.finish(self.start('asserts', ndebug=True,
                              extra_flags='-D SPACED_FLAG=1 -U SPACED_FLAG -Werror'))

    def test_cache_reuses_objects_and_observes_changed_header(self):
        self.finish(self.start('cold', trace=True))
        cold = [json.loads(s) for s in self.calls.read_text().splitlines()]
        self.assertTrue(any('-c' in call for call in cold))
        self.calls.write_text('')
        self.finish(self.start('warm', trace=True))
        warm = [json.loads(s) for s in self.calls.read_text().splitlines()]
        self.assertFalse(any('-c' in call for call in warm), warm)
        self.calls.write_text('')
        self.finish(self.start('uncached', trace=True, cache=False))
        self.assertTrue(any('-c' in json.loads(s) for s in self.calls.read_text().splitlines()))
        # A new higher-priority include must invalidate the old resolution.
        shadow = self.shadow / 'fixture.h'
        shadow.mkdir()
        self.finish(self.start('header-directory', trace=True))
        shadow.rmdir()
        shadow.write_text('#define HEADER_OFFSET 1\n')
        self.finish(self.start('shadowed-header', trace=True), expected=1)
        shadow.unlink()
        # Equal length and timestamp must not conceal changed dependencies.
        before = self.header.stat()
        self.header.write_text('#define HEADER_OFFSET 1\n')
        os.utime(self.header, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.finish(self.start('changed-header', trace=True), expected=1)
        self.header.write_text('#error current_header_must_be_read\n')
        self.finish(self.start('broken-header', trace=True), expected=1)

    def test_cache_observes_new_header_beside_forced_include(self):
        injected = self.root / 'injected'
        injected.mkdir()
        forced = injected / 'forced.h'
        forced.write_text('#include "fixture.h"\n')
        contract = self.checkout / 'tests/contract.cpp'
        contract.write_text(contract.read_text().replace(
            'fixture_value() !=', 'fixture_value() + HEADER_OFFSET !='))
        flags = '-include %s -I%s' % (forced, self.header.parent)
        self.finish(self.start('forced-include', trace=True, extra_flags=flags))
        # Quoted includes search beside their including header before -I roots.
        (injected / 'fixture.h').write_text('#define HEADER_OFFSET 1\n')
        self.finish(self.start('forced-shadow', trace=True, extra_flags=flags),
                    expected=1)

    def test_concurrent_checkouts_have_independent_outputs(self):
        other = self.root / "other's checkout"
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
