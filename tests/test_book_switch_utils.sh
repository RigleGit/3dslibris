#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/test
c++ -std=c++17 -Iinclude tests/test_book_switch_utils.cpp -o build/test/test_book_switch_utils
./build/test/test_book_switch_utils
