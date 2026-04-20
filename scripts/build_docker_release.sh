#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
IMAGE_TAG="${IMAGE_TAG:-3dslibris-cia}"
PLATFORM="${PLATFORM:-linux/amd64}"
JOBS="${JOBS:-2}"
DEVKITPRO_PATH="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM_PATH="${DEVKITARM:-/opt/devkitpro/devkitARM}"

usage() {
  cat <<'EOF'
Usage: scripts/build_docker_release.sh [--skip-image-build] [--jobs N] [--platform PLATFORM]

Builds the local Docker image used for 3dslibris and runs:
  make clean
  make -jN
  make zip-sdmc
  make debug-3dsx
  make cia
  make debug-cia
  make source-release

Environment overrides:
  IMAGE_TAG   Docker image tag to build/run
  PLATFORM    Docker platform (default: linux/amd64)
  JOBS        Parallel make jobs inside the container
  DEVKITPRO   Passed into the container (default: /opt/devkitpro)
  DEVKITARM   Passed into the container (default: /opt/devkitpro/devkitARM)
EOF
}

skip_image_build=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-image-build)
      skip_image_build=1
      ;;
    --jobs)
      shift
      [[ $# -gt 0 ]] || {
        echo "Missing value for --jobs" >&2
        exit 1
      }
      JOBS="$1"
      ;;
    --platform)
      shift
      [[ $# -gt 0 ]] || {
        echo "Missing value for --platform" >&2
        exit 1
      }
      PLATFORM="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required but was not found in PATH" >&2
  exit 1
fi

cd "$ROOT"

if [[ "$skip_image_build" -eq 0 ]]; then
  echo "[1/3] Building Docker image: $IMAGE_TAG ($PLATFORM)"
  docker build --platform "$PLATFORM" -f docker/Dockerfile.cia -t "$IMAGE_TAG" .
else
  echo "[1/3] Skipping image build, using existing image: $IMAGE_TAG ($PLATFORM)"
fi

uid="$(id -u)"
gid="$(id -g)"

echo "[2/3] Running release build inside Docker"
docker run --rm \
  --platform "$PLATFORM" \
  --user "${uid}:${gid}" \
  -v "$ROOT:/project" \
  -w /project \
  -e DEVKITPRO="$DEVKITPRO_PATH" \
  -e DEVKITARM="$DEVKITARM_PATH" \
  "$IMAGE_TAG" \
  sh -lc "make clean && make -j${JOBS} && make zip-sdmc && make debug-3dsx && make cia && make debug-cia && make source-release"

echo "[3/3] Verifying expected outputs"
test -f 3dslibris.cia
test -f 3dslibris-debug.cia
test -f 3dslibris.3dsx
test -f 3dslibris-debug.3dsx
test -f 3dslibris.smdh
test -f 3dslibris.elf
test -f dist/3dslibris-sdmc.zip
test -f dist/3dslibris-source.tar.gz
test -f dist/romfs/3ds/3dslibris/font/LiberationSerif-Regular.ttf
test -f dist/romfs/3ds/3dslibris/resources/splash.jpg

cat <<EOF
Build complete.

Outputs:
  $ROOT/3dslibris.cia
  $ROOT/3dslibris-debug.cia
  $ROOT/3dslibris.3dsx
  $ROOT/3dslibris-debug.3dsx
  $ROOT/3dslibris.smdh
  $ROOT/3dslibris.elf
  $ROOT/dist/3dslibris-sdmc.zip
  $ROOT/dist/3dslibris-source.tar.gz
EOF