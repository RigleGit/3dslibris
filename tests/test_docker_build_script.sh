set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
SCRIPT="$ROOT/scripts/build_docker_release.sh"

test -f "$SCRIPT"
bash -n "$SCRIPT"
grep -q 'docker/Dockerfile.cia' "$SCRIPT"
grep -q 'make zip-sdmc' "$SCRIPT"
grep -q 'make debug-3dsx' "$SCRIPT"
grep -q 'make cia' "$SCRIPT"
grep -q 'make source-release' "$SCRIPT"
