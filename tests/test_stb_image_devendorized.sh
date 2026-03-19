#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ -e include/stb_image.h ]; then
  echo "include/stb_image.h still exists"
  exit 1
fi

if [ ! -f third_party/stb/stb_image.h ]; then
  echo "third_party/stb/stb_image.h is missing"
  exit 1
fi

if ! rg -n '\-I.*/?third_party/stb|\-Ithird_party/stb' Makefile tests >/dev/null 2>&1; then
  echo "build and tests do not expose third_party/stb include path"
  exit 1
fi

if ! rg -n '^#define STBI_NO_FAILURE_STRINGS$' source/core/stb_image_impl.cpp >/dev/null 2>&1; then
  echo "stb_image implementation still compiles failure strings"
  exit 1
fi

echo "stb_image vendor layout OK"
