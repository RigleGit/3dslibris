# Code quality checks

These are optional maintenance tools, separate from the 3DS build and host tests.
Run them from the repository root. Review findings before changing code.

## Duplicate code

Use jscpd 5.4.0 (the version checked with this configuration):

```bash
npx --yes jscpd@5.4.0 --config .jscpd.json --fail-on-empty
```

An installed `jscpd` command can run the same arguments without npx.
Reports go to `build/quality/duplicates/` as JSON and Markdown.

The configuration scans our C++ sources and headers, skips comments and vendor
code, and reports blocks of at least 100 tokens and 12 lines. It does not enforce
a duplication threshold. Similar parsing, rendering or lifecycle code may have
different requirements; a match is a review candidate, not a request to merge it.
Scan tests separately when reviewing fixtures or assertions:

```bash
npx --yes jscpd@5.4.0 --config .jscpd.json --output build/quality/test-duplicates tests
```

## Unused C++ functions

Knip analyzes JavaScript/TypeScript projects, so it is not useful for our C++
application. Cppcheck's whole-program `unusedFunction` check is a better fit:

```bash
mkdir -p build/quality/cppcheck
cppcheck --enable=unusedFunction --std=c++11 --platform=unix32 \
  --cppcheck-build-dir=build/quality/cppcheck \
  -D__3DS__ -Iinclude -ithird_party -isource/expat source \
  2> build/quality/cppcheck/unused-functions.txt
```

This is a starting scan, not a complete devkitARM compilation model. Check the
diagnostics for missing includes, macros or configurations. When available, use
the real release/debug compilation database and the SDK include paths for a
more accurate analysis. Check callbacks, conditional builds and non-test callers
before treating a finding as dead code. The build's `--gc-sections` discards
unreachable linked sections, but that alone does not prove a source API is unused.

## Host coverage

```bash
make coverage-host
```

`build/coverage-host/report/app-coverage.md` separates our instrumented sources
from temporary platform harness fragments and lists uninstrumented files.
LLVM diagnostics are retained alongside the report. Coverage does not prove
libctru/GPU/SD behavior or whole-application HOME transitions.

When pruning tests, compare the same source-file inventory before and after;
removing a suite must not hide its production file from the denominator. Retain
independent contracts even when a test contributes few lines of coverage.

Save the initial JSON outside the coverage build directory before rebuilding,
then check both lines and branches with a fixed denominator:

```bash
python3 scripts/compare_host_coverage.py "$PWD" /path/to/before.json \
  build/coverage-host/report/llvm-cov-summary.json --max-loss 2
```

Missing files count as zero; newly instrumented files cannot hide lost coverage.
A file with a changed line or branch count also receives zero credit for that
metric: summaries cannot locate its old coverage. A conservative failure needs
a location-level review before treating it as an actual regression. The
comparison checks both percentage-point and relative loss. Equal counts do not
prove identical source locations; compare consistent source/build inventories.
LLVM warnings still limit the precision of the input data; investigate large
changes or inconsistent mappings before relying on the result.

Tool references: [jscpd](https://github.com/kucherenko/jscpd),
[Knip](https://knip.dev/), [Cppcheck manual](https://cppcheck.sourceforge.io/manual.html).
