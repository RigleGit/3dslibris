#!/usr/bin/env bash
set -euo pipefail

# Reuse the existing debug objects; only diagnostics/font metrics differ.
# Keep normal/debug delivery artifacts and source files untouched.
ROOT="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/.private/azahar-font-probe"
IMAGE="${IMAGE_TAG:-3dslibris-cia:release}"
docker_cmd=(docker)
if command -v rtk >/dev/null 2>&1; then docker_cmd=(rtk docker); fi
mkdir -p "$OUT/baseline-source"
test -f "$ROOT/build-debug/font_manager.o"
for variant in current baseline; do
  build="$ROOT/build-azahar-font-$variant"
  mkdir -p "$build"
  if [ "$variant" = current ]; then
    cp -p "$ROOT"/build-debug/*.o "$ROOT"/build-debug/*.d "$build/"
    rm -f "$build/text_renderer.o" "$build/debug_log.o"
  else
    cp -p "$ROOT"/build-azahar-font-current/*.o "$ROOT"/build-azahar-font-current/*.d "$build/"
    rm -f "$build/font_manager.o"
    # Raw source is required here; filtered git output would corrupt the file.
    git -C "$ROOT" show "${FONT_BASELINE_REF:-3db90f51}:source/ui/font_manager.cpp" > "$OUT/baseline-source/font_manager.cpp"
  fi
  "${docker_cmd[@]}" run --rm --platform linux/amd64 -v "$ROOT:/project" -w /project \
    -e DEVKITPRO=/opt/devkitpro -e DEVKITARM=/opt/devkitpro/devkitARM \
    -e FONT_PROBE_VARIANT="$variant" "$IMAGE" sh -eu -c '
      if [ "$FONT_PROBE_VARIANT" = baseline ]; then
        search_path=$(python3 -c "from pathlib import Path; print(\":\".join([\"/project/.private/azahar-font-probe/baseline-source\"] + sorted({str(p.parent.resolve()) for p in Path(\"source\").rglob(\"*.cpp\")})))")
        set -- "VPATH=$search_path"
      else
        set --
      fi
      make -j2 TARGET="3dslibris-font-$FONT_PROBE_VARIANT" \
        BUILD="build-azahar-font-$FONT_PROBE_VARIANT" DEBUG_LOGGING=1 \
        DEBUG_OPTIMIZATION="-Og -DDSLIBRIS_DEBUG_LOG_LEVEL=5" "$@"
    '
  for ext in 3dsx elf smdh; do
    mv "$ROOT/3dslibris-font-$variant.$ext" "$OUT/3dslibris-font-$variant.$ext"
  done
done
echo "Probe binaries: $OUT"
