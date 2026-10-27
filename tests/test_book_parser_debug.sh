#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
PARSER_DEBUG_FLAGS="-DDSLIBRIS_DEBUG -I$TEST_ROOT/tests/stubs/parser_debug"
# The normal driver owns the complete functional matrix. This variant executes
# the additional diagnostic size/capacity/release contract with real MOBI data.
if [ "$#" -eq 1 ] && [ "$1" = all ]; then
  set --
elif [ "$#" -eq 0 ]; then
  set -- mobi-diagnostics
fi
# Extra dependencies used only by the debug paths of the existing real parser
# integration. Keep the profiler and buffered logger real.
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} $PARSER_DEBUG_FLAGS -I"$TEST_ROOT/include" \
  -c "$TEST_ROOT/source/shared/fixed_layout_perf.cpp" -o "$TEST_OUTDIR/parser_debug_perf.o"
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -I"$TEST_ROOT/include" \
  -c "$TEST_ROOT/source/formats/common/buffered_status_log.cpp" -o "$TEST_OUTDIR/parser_debug_log.o"
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} $PARSER_DEBUG_FLAGS -I"$TEST_ROOT/include" \
  -c "$TEST_ROOT/source/shared/boot_trace.cpp" -o "$TEST_OUTDIR/parser_debug_boot_trace.o"
CXXFLAGS="${CXXFLAGS:-} $PARSER_DEBUG_FLAGS" \
LDFLAGS="${LDFLAGS:-} $TEST_OUTDIR/parser_debug_perf.o $TEST_OUTDIR/parser_debug_log.o $TEST_OUTDIR/parser_debug_boot_trace.o -pthread" \
  bash "$TEST_ROOT/tests/test_book_parser_integration.sh" "$@"
