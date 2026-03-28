#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang++ -std=c++11 -Iinclude tests/test_mobi_markup_tag.cpp \
  source/formats/mobi/mobi_markup_tag.cpp -o /tmp/test_mobi_markup_tag
/tmp/test_mobi_markup_tag
