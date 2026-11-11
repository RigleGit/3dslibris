#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. ./tests/test_build.sh

build_test test_plain_text_stream_window \
  --expat -I"$PWD/tests/stubs" \
  tests/test_plain_text_stream_window.cpp \
  source/formats/common/plain_text_stream.cpp \
  source/formats/common/plain_text_perf_utils.cpp \
  source/formats/mobi/mobi_heading_markers.cpp \
  source/core/parse.cpp \
  source/shared/debug_log.cpp
