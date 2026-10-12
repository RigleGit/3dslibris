#!/bin/bash
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
ARM_BIN="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin"
if [ ! -x "$ARM_BIN/arm-none-eabi-g++" ]; then
  if command -v arm-none-eabi-g++ >/dev/null 2>&1; then
    ARM_BIN="$(dirname "$(command -v arm-none-eabi-g++)")"
  else
    echo "SKIP test_main_stack: missing arm-none-eabi-g++"
    exit 77
  fi
fi

TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
if [ "$#" -gt 0 ]; then
  ELF="$1"
else
  # Exercise libctru's startup ABI, including its weak default when the
  # application override is absent. No platform calls need to be executed.
  printf 'int main() { return 0; }\n' > "$TASK_TMP/main.cpp"
  sources=("$TASK_TMP/main.cpp")
  if [ -f "$ROOT/source/core/runtime_config.cpp" ]; then
    sources+=("$ROOT/source/core/runtime_config.cpp")
  fi
  ELF="$TASK_TMP/stack.elf"
  "$ARM_BIN/arm-none-eabi-g++" -std=gnu++11 -march=armv6k -mtune=mpcore \
    -mfloat-abi=hard -mtp=soft -fno-exceptions -fno-rtti -specs=3dsx.specs \
    -I"${DEVKITPRO:-/opt/devkitpro}/libctru/include" "${sources[@]}" \
    -L"${DEVKITPRO:-/opt/devkitpro}/libctru/lib" -lctru -o "$ELF"
fi

address="$("$ARM_BIN/arm-none-eabi-nm" "$ELF" | awk '$3 == "__stacksize__" {print $1}')"
test -n "$address"
value="$("$ARM_BIN/arm-none-eabi-objdump" -s -j .data \
  --start-address="$((16#$address))" --stop-address="$((16#$address + 4))" \
  "$ELF" | awk 'length($2) == 8 && $2 !~ /[^[:xdigit:]]/ {print $2}')"
python3 - "$value" <<'PY'
import sys

size = int.from_bytes(bytes.fromhex(sys.argv[1]), 'little')
# The observed cold EPUB path needs >36 KiB (parser + CFF glyph interpreter),
# before the callers and Expat callbacks. Keep headroom without a large heap
# reservation; the CIA descriptors already allow 512 KiB.
assert 64 * 1024 <= size <= 512 * 1024, 'unsafe main-thread stack: %d bytes' % size
print('PASS: linked libctru main-thread stack is %d bytes' % size)
PY
