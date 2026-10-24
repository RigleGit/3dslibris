#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
build_test test_base64_utils "$TEST_ROOT/tests/test_base64_utils.cpp"
