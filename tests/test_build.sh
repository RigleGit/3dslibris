# 3dslibris test build helper
# Usage: source test_build.sh
# Then call: build_test <test_name> <source_files...> [-l<lib>...] [-I<include>...]

set -eu

TEST_ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
TEST_OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$TEST_OUTDIR"

# Compile third-party C dependencies once
_build_third_party_objs() {
  local -a objs

  # utf8proc
  if [ -f "$TEST_ROOT/third_party/utf8proc/utf8proc.c" ]; then
    if [ ! -f "$TEST_OUTDIR/utf8proc.o" ]; then
      cc -std=c99 -c "$TEST_ROOT/third_party/utf8proc/utf8proc.c" \
        -I"$TEST_ROOT/third_party/utf8proc" \
        -o "$TEST_OUTDIR/utf8proc.o"
    fi
    objs+=("$TEST_OUTDIR/utf8proc.o")
  fi

  # libunibreak
  if [ -d "$TEST_ROOT/third_party/libunibreak/src" ]; then
    for f in linebreak linebreakdata linebreakdef eastasianwidthdata eastasianwidthdef unibreakbase unibreakdef; do
      local src="$TEST_ROOT/third_party/libunibreak/src/${f}.c"
      if [ -f "$src" ] && [ ! -f "$TEST_OUTDIR/${f}.o" ]; then
        cc -std=c99 -c "$src" -I"$TEST_ROOT/third_party/libunibreak/src" -o "$TEST_OUTDIR/${f}.o"
      fi
      if [ -f "$TEST_OUTDIR/${f}.o" ]; then
        objs+=("$TEST_OUTDIR/${f}.o")
      fi
    done
  fi

  printf '%s\n' "${objs[@]}"
}

THIRD_PARTY_OBJS=()
while IFS= read -r obj; do
  [ -n "$obj" ] || continue
  THIRD_PARTY_OBJS+=("$obj")
done <<EOF
$(_build_third_party_objs)
EOF

build_test() {
  local name="$1"
  shift

  local -a sources
  local -a libs
  local -a includes
  local -a stubs
  sources=()
  libs=()
  stubs=()
  includes=(
    "-I$TEST_ROOT/include"
    "-I$TEST_ROOT/third_party/utf8proc"
    "-I$TEST_ROOT/third_party/libunibreak/src"
  )

  while [ $# -gt 0 ]; do
    case "$1" in
      -l*)
        libs+=("$1")
        ;;
      -I*)
        includes+=("$1")
        ;;
      --stubs)
        shift
        stubs+=("$TEST_ROOT/tests/stubs/$1")
        ;;
      *)
        sources+=("$1")
        ;;
    esac
    shift
  done

  local -a cmd
  cmd=(c++ -std=c++11)
  if [ ${#sources[@]} -gt 0 ]; then
    cmd+=("${sources[@]}")
  fi
  if [ ${#stubs[@]} -gt 0 ]; then
    cmd+=("${stubs[@]}")
  fi
  if [ ${#THIRD_PARTY_OBJS[@]} -gt 0 ]; then
    cmd+=("${THIRD_PARTY_OBJS[@]}")
  fi
  if [ ${#includes[@]} -gt 0 ]; then
    cmd+=("${includes[@]}")
  fi
  if [ ${#libs[@]} -gt 0 ]; then
    cmd+=("${libs[@]}")
  fi
  cmd+=(-o "$TEST_OUTDIR/$name")

  "${cmd[@]}"

  "$TEST_OUTDIR/$name"
}
