#include "settings/font_config_utils.h"
#include "shared/text_token_constants.h"
#include "test_assert.h"

#include <cstring>

namespace {

bool HasPrefAttr(const char *attr_name, u8 style) {
  const font_config_utils::FontPrefBinding *bindings =
      font_config_utils::GetFontPrefBindings();
  const size_t count = font_config_utils::GetFontPrefBindingCount();
  for (size_t i = 0; i < count; i++) {
    if (bindings[i].style == style &&
        std::strcmp(bindings[i].attr_name, attr_name) == 0) {
      return true;
    }
  }
  return false;
}

void TestFontTargetsKeepBrowserAndMonoSeparate() {
  test::ExpectEq("mono target style",
                 font_config_utils::StyleFromTarget(
                     font_config_utils::FONT_TARGET_MONO),
                 TEXT_STYLE_MONO);
  test::ExpectEq("mono bold target style",
                 font_config_utils::StyleFromTarget(
                     font_config_utils::FONT_TARGET_MONO_BOLD),
                 TEXT_STYLE_MONO_BOLD);
  test::ExpectEq("mono italic target style",
                 font_config_utils::StyleFromTarget(
                     font_config_utils::FONT_TARGET_MONO_ITALIC),
                 TEXT_STYLE_MONO_ITALIC);
  test::ExpectEq("mono bold italic target style",
                 font_config_utils::StyleFromTarget(
                     font_config_utils::FONT_TARGET_MONO_BOLDITALIC),
                 TEXT_STYLE_MONO_BOLDITALIC);
  test::ExpectEq("browser target style",
                 font_config_utils::StyleFromTarget(
                     font_config_utils::FONT_TARGET_BROWSER),
                 TEXT_STYLE_BROWSER);
  test::ExpectStrEq("mono target label",
                    font_config_utils::GetFontTargetLabel(
                        font_config_utils::FONT_TARGET_MONO),
                    "mono font");
  test::ExpectStrEq("mono bold target label",
                    font_config_utils::GetFontTargetLabel(
                        font_config_utils::FONT_TARGET_MONO_BOLD),
                    "mono bold font");
  test::ExpectStrEq("mono italic target label",
                    font_config_utils::GetFontTargetLabel(
                        font_config_utils::FONT_TARGET_MONO_ITALIC),
                    "mono italic font");
  test::ExpectStrEq("mono bold italic target label",
                    font_config_utils::GetFontTargetLabel(
                        font_config_utils::FONT_TARGET_MONO_BOLDITALIC),
                    "mono bold italic font");
  test::ExpectStrEq("browser target label",
                    font_config_utils::GetFontTargetLabel(
                        font_config_utils::FONT_TARGET_BROWSER),
                    "browser/ui font");
}

void TestDefaultFontForMonoStyle() {
  test::ExpectStrEq("mono default font",
                    font_config_utils::DefaultFontForStyle(TEXT_STYLE_MONO),
                    "LiberationMono-Regular.ttf");
  test::ExpectStrEq("mono bold default font",
                    font_config_utils::DefaultFontForStyle(
                        TEXT_STYLE_MONO_BOLD),
                    "LiberationMono-Bold.ttf");
  test::ExpectStrEq("mono italic default font",
                    font_config_utils::DefaultFontForStyle(
                        TEXT_STYLE_MONO_ITALIC),
                    "LiberationMono-Italic.ttf");
  test::ExpectStrEq("mono bold italic default font",
                    font_config_utils::DefaultFontForStyle(
                        TEXT_STYLE_MONO_BOLDITALIC),
                    "LiberationMono-BoldItalic.ttf");
}

void TestFontPrefBindingsIncludeMonoAndBrowser() {
  test::ExpectTrue("mono pref binding", HasPrefAttr("mono", TEXT_STYLE_MONO));
  test::ExpectTrue("mono bold pref binding",
                   HasPrefAttr("monobold", TEXT_STYLE_MONO_BOLD));
  test::ExpectTrue("mono italic pref binding",
                   HasPrefAttr("monoitalic", TEXT_STYLE_MONO_ITALIC));
  test::ExpectTrue("mono bold italic pref binding",
                   HasPrefAttr("monobolditalic", TEXT_STYLE_MONO_BOLDITALIC));
  test::ExpectTrue("browser pref binding",
                   HasPrefAttr("browser", TEXT_STYLE_BROWSER));

  u8 style = 0xff;
  test::ExpectTrue("style from mono attr",
                   font_config_utils::StyleFromFontPrefAttr("mono", &style));
  test::ExpectEq("mono attr style", style, TEXT_STYLE_MONO);
  test::ExpectStrEq("attr for mono style",
                    font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_MONO),
                    "mono");
  test::ExpectTrue("style from mono bold attr",
                   font_config_utils::StyleFromFontPrefAttr("monobold", &style));
  test::ExpectEq("mono bold attr style", style, TEXT_STYLE_MONO_BOLD);
  test::ExpectStrEq(
      "attr for mono bold style",
      font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_MONO_BOLD),
      "monobold");
}

} // namespace

int main() {
  TestFontTargetsKeepBrowserAndMonoSeparate();
  TestDefaultFontForMonoStyle();
  TestFontPrefBindingsIncludeMonoAndBrowser();
  return 0;
}
