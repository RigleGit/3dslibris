#!/bin/bash
# GCC -O2 folds malloc/free of an unused pointer into "return true". Compile the
# real probe with the project's ARM flags and require actual malloc/free calls.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
ARM_BIN="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin"
if [ ! -x "$ARM_BIN/arm-none-eabi-g++" ]; then
  if command -v arm-none-eabi-g++ >/dev/null 2>&1; then
    ARM_BIN="$(dirname "$(command -v arm-none-eabi-g++)")"
  else
    echo "SKIP test_headroom_probe_arm: missing arm-none-eabi-g++"
    exit 77
  fi
fi

TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
cat > "$TASK_TMP/probe.cpp" <<'CPP'
#include "formats/common/cache_memory_utils.h"
bool Probe(unsigned bytes) { return cache_memory::HasHeadroom(bytes, 0); }
bool ProbeRaw(unsigned bytes) { return cache_memory::ProbeAlloc(bytes); }
CPP
"$ARM_BIN/arm-none-eabi-g++" -std=gnu++11 -O2 -march=armv6k -mtune=mpcore \
  -mfloat-abi=hard -fno-exceptions -fno-rtti -I"$ROOT/include" \
  -S "$TASK_TMP/probe.cpp" -o "$TASK_TMP/probe.s"
for symbol in malloc free; do
  if ! grep -Eq "bl[[:space:]]+$symbol\$|bl[[:space:]]+$symbol[[:space:]]" "$TASK_TMP/probe.s"; then
    echo "FAIL: the headroom probe does not call $symbol at -O2 (folded away)" >&2
    exit 1
  fi
done
echo "PASS: headroom probe keeps its malloc/free calls at -O2"
