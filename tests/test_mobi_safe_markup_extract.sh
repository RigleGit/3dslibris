#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. ./tests/test_build.sh

build_test test_mobi_safe_markup_extract \
  tests/test_mobi_safe_markup_extract.cpp \
  source/formats/mobi/mobi_safe_markup_extract.cpp \
  source/formats/common/html_entity_utils.cpp \
  source/formats/common/text_helpers.cpp \
  source/shared/utf8_utils.cpp \
  --stubs mobi_safe_markup_extract_deps.cpp
