#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++17 -Iinclude tests/test_mobi_heading_markers.cpp \
  source/core/mobi_heading_markers.cpp -o /tmp/test_mobi_heading_markers
/tmp/test_mobi_heading_markers
