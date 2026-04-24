#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. ./tests/test_build.sh

build_test test_browser_grid_geometry_utils \
  tests/test_browser_grid_geometry_utils.cpp \
  source/library/browser_grid_geometry_utils.cpp
