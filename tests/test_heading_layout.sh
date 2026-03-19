#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++17 -Iinclude tests/test_heading_layout.cpp \
  source/book/heading_layout.cpp -o /tmp/test_heading_layout
/tmp/test_heading_layout
