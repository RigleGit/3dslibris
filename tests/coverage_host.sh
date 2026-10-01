#!/bin/bash
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
COVERAGE_ROOT="${ROOT}/build/coverage-host"
TMP_ROOT="${COVERAGE_ROOT}/tmp"
TEST_OUTDIR="${TMP_ROOT}/3dslibris-tests"
PROFILE_DIR="${COVERAGE_ROOT}/profiles"
REPORT_DIR="${COVERAGE_ROOT}/report"
SUMMARY_TXT="${REPORT_DIR}/llvm-cov-report.txt"
SUMMARY_JSON="${REPORT_DIR}/llvm-cov-summary.json"
PROFDATA="${REPORT_DIR}/host-tests.profdata"
DIAGNOSTICS="${REPORT_DIR}/llvm-cov-diagnostics.txt"

# Rebuild objects and fixtures so removed tests and old flags cannot skew coverage.
rm -rf "$TMP_ROOT" "$PROFILE_DIR"
mkdir -p "$TMP_ROOT" "$TEST_OUTDIR" "$PROFILE_DIR" "$REPORT_DIR"
rm -f "$PROFILE_DIR"/*.profraw "$PROFDATA" "$SUMMARY_TXT" "$SUMMARY_JSON"

if command -v xcrun >/dev/null 2>&1; then
  CC_BIN="$(xcrun --find clang)"
  CXX_BIN="$(xcrun --find clang++)"
  LLVM_COV_BIN="$(xcrun --find llvm-cov)"
  LLVM_PROFDATA_BIN="$(xcrun --find llvm-profdata)"
  SDKROOT_BIN="$(xcrun --show-sdk-path)"
  export SDKROOT="$SDKROOT_BIN"
else
  CC_BIN="${CC:-cc}"
  CXX_BIN="${CXX:-c++}"
  LLVM_COV_BIN="${LLVM_COV:-llvm-cov}"
  LLVM_PROFDATA_BIN="${LLVM_PROFDATA:-llvm-profdata}"
  SDKROOT_BIN=""
fi

export TMPDIR="$TMP_ROOT"
export CC="$CC_BIN"
export CXX="$CXX_BIN"
SYSROOT_FLAGS=""
if [ -n "$SDKROOT_BIN" ]; then
  SYSROOT_FLAGS="-isysroot $SDKROOT_BIN"
fi
export CFLAGS="${CFLAGS:-} $SYSROOT_FLAGS -O0 -g -fprofile-instr-generate -fcoverage-mapping"
export CXXFLAGS="${CXXFLAGS:-} $SYSROOT_FLAGS -O0 -g -fprofile-instr-generate -fcoverage-mapping"
export LDFLAGS="${LDFLAGS:-} -fprofile-instr-generate -fcoverage-mapping"
export LLVM_PROFILE_FILE="${PROFILE_DIR}/%p-%m.profraw"

cd "$ROOT/tests"
./run_all_tests.sh

find "$PROFILE_DIR" -name '*.profraw' -print0 | xargs -0 "$LLVM_PROFDATA_BIN" merge -sparse -o "$PROFDATA"

objects=()
while IFS= read -r -d '' obj; do
  objects+=("$obj")
done < <(find "$TEST_OUTDIR" -type f -perm -111 -print0 | sort -z)

if [ ${#objects[@]} -eq 0 ]; then
  echo "No coverage objects found."
  exit 1
fi

cov_args=()
for obj in "${objects[@]}"; do
  cov_args+=(-object "$obj")
done

ignore_regex='/(tests|third_party|build/coverage-host|tests/stubs|source/expat|source/core/stb_image_impl\.cpp)'

"$LLVM_COV_BIN" report \
  "${cov_args[@]}" \
  -instr-profile "$PROFDATA" \
  -ignore-filename-regex "$ignore_regex" \
  > "$SUMMARY_TXT" 2> "$DIAGNOSTICS"

"$LLVM_COV_BIN" export \
  -summary-only \
  "${cov_args[@]}" \
  -instr-profile "$PROFDATA" \
  -ignore-filename-regex "$ignore_regex" \
  > "$SUMMARY_JSON" 2>> "$DIAGNOSTICS"

cat "$DIAGNOSTICS" >&2

python3 "$ROOT/scripts/summarize_host_coverage.py" "$ROOT" "$SUMMARY_JSON" \
  "$REPORT_DIR/app-coverage.md" "" "$DIAGNOSTICS"

echo "Coverage report written to:"
echo "  $SUMMARY_TXT"
echo "  $SUMMARY_JSON"
