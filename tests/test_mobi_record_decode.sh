#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++17 -Iinclude tests/test_mobi_record_decode.cpp \
  source/formats/mobi/mobi_record_decode.cpp -o /tmp/test_mobi_record_decode
/tmp/test_mobi_record_decode
