#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
build_test test_image_rotation "$TEST_ROOT/tests/test_image_rotation.cpp" \
 "$TEST_ROOT/source/formats/common/fixed_layout_surface.cpp" \
 "$TEST_ROOT/source/formats/common/pdf_view_utils.cpp"
