#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++11 -Iinclude tests/test_mobi_position_map.cpp \
  source/formats/mobi/mobi_position_map.cpp -o /tmp/test_mobi_position_map
/tmp/test_mobi_position_map
