#!/usr/bin/env bash
# Regenerates tests/fixtures/epub-render-test.epub from the source directory.
# EPUB spec requires: mimetype must be the first entry, uncompressed, no extra fields.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE_DIR="$REPO_ROOT/tests/fixtures/epub-render-test"
OUT="$REPO_ROOT/tests/fixtures/epub-render-test.epub"

cd "$FIXTURE_DIR"

rm -f "$OUT"

# mimetype: first entry, stored (no compression), no extra fields (-X)
zip -0 -X "$OUT" mimetype

# Everything else, compressed
zip -r "$OUT" META-INF OEBPS --exclude '*.DS_Store'

echo "Built: $OUT"
