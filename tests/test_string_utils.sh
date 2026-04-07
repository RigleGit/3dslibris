#!/bin/bash
source "$(dirname "$0")/test_build.sh"
build_test test_string_utils \
  tests/test_string_utils.cpp
