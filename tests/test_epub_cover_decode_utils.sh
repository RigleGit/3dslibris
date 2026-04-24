#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. ./tests/test_build.sh

build_test test_epub_cover_decode_utils \
  tests/test_epub_cover_decode_utils.cpp
