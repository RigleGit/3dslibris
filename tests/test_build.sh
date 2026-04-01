# 3dslibris test build helper
# Usage: source test_build.sh
# Then call: build_test <test_name> <source_files...> [-l<lib>...] [-I<include>...]

set -eu

TEST_ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
TEST_OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$TEST_OUTDIR"

# Compile third-party C dependencies once
_build_third_party_objs() {
  local objs=""

  # utf8proc
  if [ -f "$TEST_ROOT/third_party/utf8proc/utf8proc.c" ]; then
    if [ ! -f "$TEST_OUTDIR/utf8proc.o" ]; then
      cc -std=c99 -c "$TEST_ROOT/third_party/utf8proc/utf8proc.c" \
        -I"$TEST_ROOT/third_party/utf8proc" \
        -o "$TEST_OUTDIR/utf8proc.o"
    fi
    objs="$objs $TEST_OUTDIR/utf8proc.o"
  fi

  # libunibreak
  if [ -d "$TEST_ROOT/third_party/libunibreak/src" ]; then
    for f in linebreak linebreakdata linebreakdef eastasianwidthdata eastasianwidthdef unibreakbase unibreakdef; do
      local src="$TEST_ROOT/third_party/libunibreak/src/${f}.c"
      if [ -f "$src" ] && [ ! -f "$TEST_OUTDIR/${f}.o" ]; then
        cc -std=c99 -c "$src" -I"$TEST_ROOT/third_party/libunibreak/src" -o "$TEST_OUTDIR/${f}.o"
      fi
      if [ -f "$TEST_OUTDIR/${f}.o" ]; then
        objs="$objs $TEST_OUTDIR/${f}.o"
      fi
    done
  fi

  echo "$objs"
}

THIRD_PARTY_OBJS="$(_build_third_party_objs)"

build_test() {
  local name="$1"
  shift

  local sources=""
  local libs=""
  local includes="-I$TEST_ROOT/include -I$TEST_ROOT/third_party/utf8proc -I$TEST_ROOT/third_party/libunibreak/src"
  local stubs=""

  while [ $# -gt 0 ]; do
    case "$1" in
      -l*)
        libs="$libs $1"
        ;;
      -I*)
        includes="$includes $1"
        ;;
      --stubs)
        shift
        stubs="$TEST_ROOT/tests/stubs/$1"
        ;;
      *)
        sources="$sources $1"
        ;;
    esac
    shift
  done

  c++ -std=c++11 \
    $sources $stubs $THIRD_PARTY_OBJS \
    $includes $libs \
    -o "$TEST_OUTDIR/$name"

  "$TEST_OUTDIR/$name"
}
