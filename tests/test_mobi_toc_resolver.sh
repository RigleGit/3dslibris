#!/bin/bash
source "$(dirname "$0")/test_build.sh"
build_test test_mobi_toc_resolver \
  tests/test_mobi_toc_resolver.cpp \
  source/formats/mobi/mobi_toc_resolver.cpp \
  source/formats/mobi/mobi_structured_toc_parser.cpp \
  source/formats/mobi/mobi_record_scan.cpp \
  source/formats/mobi/mobi_record_decode.cpp \
  source/formats/mobi/mobi_position_map.cpp \
  source/formats/common/file_read_utils.cpp \
  source/formats/mobi/mobi_toc_finalize.cpp \
  source/formats/mobi/mobi_toc_finalize_policy.cpp \
  source/formats/mobi/mobi_toc_apply.cpp \
  source/formats/common/html_entity_utils.cpp \
  source/formats/common/text_helpers.cpp \
  source/shared/utf8_utils.cpp \
  -I"$TEST_ROOT/tests/stubs"
