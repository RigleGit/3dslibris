#!/bin/bash
# Run all host tests and report results
set -u

TEST_ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
TEST_TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/3dslibris-host-tests.XXXXXX")" || exit 1
if [ "${DSLIBRIS_TEST_KEEP_OUTPUT:-0}" != 1 ]; then
  trap 'rm -rf "$TEST_TMP_ROOT"' EXIT
fi
trap 'exit 130' INT
trap 'exit 143' TERM
export TMPDIR="$TEST_TMP_ROOT"
test_errors="$TEST_TMP_ROOT/test-errors.txt"
passed=0
failed=0
skipped=0
total=0
failures=""

for test_script in "$TEST_ROOT"/test_*.sh; do
  # The build helper is sourced by tests; it is not an independent suite.
  basename="$(basename "$test_script")"
  if [ "$basename" = "test_build.sh" ]; then
    continue
  fi
  
  total=$((total + 1))
  name="$(basename "$test_script" .sh)"
  
  if (cd "$TEST_ROOT/.." && bash "$test_script") 2>"$test_errors"; then
    printf "  PASS  %s\n" "$name"
    passed=$((passed + 1))
  else
    result=$?
    if [ "$result" -eq 77 ]; then
      skipped=$((skipped + 1))
      printf "  SKIP  %s\n" "$name"
      rm -f "$test_errors"
      continue
    fi
    printf "  FAIL  %s\n" "$name"
    failed=$((failed + 1))
    failures="$failures\n  - $name"
    head -5 "$test_errors"
  fi
  rm -f "$test_errors"
done

echo ""
echo "Results: $passed/$total passed, $failed failed, $skipped skipped"
if [ $failed -gt 0 ]; then
  echo "Failed tests:$failures"
  exit 1
fi
