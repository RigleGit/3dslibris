#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

mkdir -p build/tests

c++ -std=c++17 -Wall -Wextra -Iinclude \
  source/core/inline_image_layout.cpp \
  tests/test_inline_image_layout.cpp \
  -o build/tests/test_inline_image_layout

build/tests/test_inline_image_layout
