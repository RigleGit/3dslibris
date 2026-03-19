#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++17 -Iinclude tests/test_mobi_record_scan.cpp \
  source/core/mobi_record_scan.cpp -o /tmp/test_mobi_record_scan
/tmp/test_mobi_record_scan
