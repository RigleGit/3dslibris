#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/settings/prefs.cpp" 'static int ClampLineSpacingSetting(' > "$TASK_TMP/prefs_read_callbacks.inc"
printf '%s\n' 'namespace xml { namespace prefs {' >> "$TASK_TMP/prefs_read_callbacks.inc"
for signature in 'static bool MatchesBookFilename(' 'void start(' 'void end('; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/settings/prefs.cpp" "$signature" >> "$TASK_TMP/prefs_read_callbacks.inc"
done
printf '%s\n' '} }' >> "$TASK_TMP/prefs_read_callbacks.inc"
for signature in 'static std::string XmlEscapeAttr(' 'Prefs::Prefs(' 'Prefs::~Prefs()' 'void Prefs::Init()' 'int Prefs::Read()' 'void Prefs::Apply()' 'void Prefs::ClearPendingCurrentBookRestore()' 'void Prefs::SetPendingCurrentBookRestore(' 'void Prefs::AddPendingCurrentBookBookmark(' 'void Prefs::EndPendingCurrentBookRestoreEntry()' 'bool Prefs::ApplyPendingCurrentBookRestore()' 'void Prefs::RememberSavedBookState(' 'void Prefs::BeginSavedBookBookmarks(' 'void Prefs::RememberSavedBookBookmark(' 'void Prefs::EndSavedBookBookmarks()' 'void Prefs::RememberSavedLastOpened(' 'void Prefs::ApplySavedBookState(' 'void Prefs::RequestWrite()' 'bool Prefs::FlushPendingWrite(' 'int Prefs::Write()'; do
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/source/settings/prefs.cpp" "$signature" >> "$TASK_TMP/prefs_deferred.inc"
done
export PREFS_TEST_DIR="$TASK_TMP"
build_test test_prefs_deferred_write --expat -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" -I"$ROOT/include" "$ROOT/tests/test_prefs_deferred_write.cpp" "$ROOT/source/settings/prefs_file_utils.cpp" "$ROOT/source/settings/prefs_book_state.cpp" "$ROOT/source/settings/font_config_utils.cpp" "$ROOT/source/settings/prefs_style_value_utils.cpp" "$ROOT/source/library/browser_view_utils.cpp" "$ROOT/source/shared/utf8_utils.cpp" "$ROOT/source/formats/common/xml_parse_utils.cpp" "$ROOT/tests/stubs/minizip_unzip_stubs.cpp"
