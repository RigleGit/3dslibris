#!/bin/bash
# Run all host tests and report results
set -u

TEST_ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
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
  
  if (cd "$TEST_ROOT/.." && bash "$test_script") 2>/tmp/test_err_$$.txt; then
    printf "  PASS  %s\n" "$name"
    passed=$((passed + 1))
  else
    result=$?
    if [ "$result" -eq 77 ]; then
      skipped=$((skipped + 1))
      printf "  SKIP  %s\n" "$name"
      rm -f /tmp/test_err_$$.txt
      continue
    fi
    printf "  FAIL  %s\n" "$name"
    failed=$((failed + 1))
    failures="$failures\n  - $name"
    cat /tmp/test_err_$$.txt 2>/dev/null | head -5
  fi
  rm -f /tmp/test_err_$$.txt
done

echo ""
echo "Results: $passed/$total passed, $failed failed, $skipped skipped"
if [ $failed -gt 0 ]; then
  echo "Failed tests:$failures"
  exit 1
fi
