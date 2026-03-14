#!/bin/sh
# install-cia-tools.sh — build makerom and bannertool from source
# This avoids architecture-specific binary downloads and works in arm64/x86_64
# Linux environments such as devkitpro/devkitarm on Apple Silicon.
set -eu

TOOLS_DIR="${1:-/usr/local/bin}"
BANNERTOOL_REF="${BANNERTOOL_REF:-master}"
MAKEROM_REF="${MAKEROM_REF:-makerom-v0.17}"

need_install() {
    if command -v "$1" >/dev/null 2>&1; then
        echo "$1 already installed: $(command -v "$1")"
        return 1
    fi
    return 0
}

if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y --no-install-recommends ca-certificates curl git g++ make libpng-dev zlib1g-dev
    rm -rf /var/lib/apt/lists/*
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

if need_install bannertool; then
    echo "Building bannertool from ${BANNERTOOL_REF}..."
    git clone --depth 1 --branch "${BANNERTOOL_REF}" https://github.com/diasurgical/bannertool.git "${TMP}/bannertool"
    cd "${TMP}/bannertool"
    g++ -std=c++11 -O2 -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=0 \
        source/main.cpp source/cmd.cpp \
        source/3ds/cbmd.cpp source/3ds/cwav.cpp source/3ds/lz11.cpp \
        source/pc/wav.cpp source/pc/stb_image.c source/pc/stb_vorbis.c \
        -Isource -Isource/3ds -Isource/pc -lpng -lz -lm \
        -o "${TOOLS_DIR}/bannertool"
    echo "bannertool installed to ${TOOLS_DIR}/bannertool"
fi

if need_install makerom; then
    echo "Building makerom from ${MAKEROM_REF}..."
    git clone --depth 1 --branch "${MAKEROM_REF}" https://github.com/3DSGuy/Project_CTR.git "${TMP}/Project_CTR"
    cd "${TMP}/Project_CTR/makerom"
    if grep -q '^deps:' Makefile; then
        make deps -j"${JOBS}"
    fi
    make -j"${JOBS}"
    if [ -x bin/makerom ]; then
        install -m 0755 bin/makerom "${TOOLS_DIR}/makerom"
    elif [ -x makerom ]; then
        install -m 0755 makerom "${TOOLS_DIR}/makerom"
    else
        echo "makerom binary not found after build" >&2
        exit 1
    fi
    echo "makerom installed to ${TOOLS_DIR}/makerom"
fi

echo "CIA tools ready."
