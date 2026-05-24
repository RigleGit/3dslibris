#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"

build_test test_cia_banner_audio \
  "$TEST_ROOT/tests/test_cia_banner_audio.cpp"
