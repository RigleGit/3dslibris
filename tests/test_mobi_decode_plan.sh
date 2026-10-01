#!/bin/bash
source "$(dirname "$0")/test_build.sh"
build_test test_mobi_decode_plan "$TEST_ROOT/tests/test_mobi_decode_plan.cpp"
