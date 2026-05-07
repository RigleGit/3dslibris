# 3dslibris test build helper
# Usage: source test_build.sh
# Then call: build_test <test_name> <source_files...> [-l<lib>...] [-I<include>...]

set -eu

if [ -n "${BASH_SOURCE:-}" ]; then
  TEST_ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
else
  TEST_ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
fi
TEST_OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$TEST_OUTDIR"

# Compile third-party C dependencies once
_build_third_party_objs() {
  local -a objs
  objs=()

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

  for obj in "${objs[@]}"; do
    printf '%s\n' "$obj"
  done
}

THIRD_PARTY_OBJS=()
while IFS= read -r obj; do
  [ -n "$obj" ] || continue
  THIRD_PARTY_OBJS+=("$obj")
done <<EOF
$(_build_third_party_objs)
EOF

# Compile expat once — only included when tests pass --expat
_build_expat_objs() {
  local -a objs
  objs=()
  if [ -f "$TEST_ROOT/third_party/expat/xmlparse.c" ]; then
    local expat_flags="-DXML_CONTEXT_BYTES=1024 -DHAVE_ARC4RANDOM_BUF"
    local expat_inc="-I$TEST_ROOT/third_party/expat"
    for f in xmlparse xmlrole xmltok; do
      local src="$TEST_ROOT/third_party/expat/${f}.c"
      if [ -f "$src" ] && [ ! -f "$TEST_OUTDIR/expat_${f}.o" ]; then
        cc -std=c99 $expat_flags $expat_inc -c "$src" -o "$TEST_OUTDIR/expat_${f}.o"
      fi
      if [ -f "$TEST_OUTDIR/expat_${f}.o" ]; then
        objs+=("$TEST_OUTDIR/expat_${f}.o")
      fi
    done
  fi
  for obj in "${objs[@]}"; do
    printf '%s\n' "$obj"
  done
}

build_test() {
  local name="$1"
  shift

  local -a sources
  local -a libs
  local -a includes
  local -a stubs
  local use_expat=0
  sources=()
  libs=()
  stubs=()
  includes=(
    "-I$TEST_ROOT/include"
    "-I$TEST_ROOT/third_party/utf8proc"
    "-I$TEST_ROOT/third_party/libunibreak/src"
    "-I$TEST_ROOT/third_party/mupdf/thirdparty/zlib/contrib"
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
      --expat)
        use_expat=1
        ;;
      *)
        sources+=("$1")
        ;;
    esac
    shift
  done

  local -a expat_objs
  expat_objs=()
  if [ "$use_expat" = "1" ]; then
    while IFS= read -r obj; do
      [ -n "$obj" ] || continue
      expat_objs+=("$obj")
    done <<EOFEXPAT
$(_build_expat_objs)
EOFEXPAT
  fi

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
  if [ ${#expat_objs[@]} -gt 0 ]; then
    cmd+=("${expat_objs[@]}")
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
