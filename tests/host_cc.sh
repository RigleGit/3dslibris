#!/bin/sh
exec python3 "$(dirname "$0")/host_compile.py" CC "$@"
