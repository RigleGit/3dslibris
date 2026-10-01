#include "settings/prefs_style_value_utils.h"

#include "test_assert.h"

int main() {
  settings::StyleValueContext value;
  value.global_value = 14;

  test::ExpectEq("global font value",
                 settings::EffectiveStyleValue(value), 14);
  test::ExpectStrEq("global font label",
                    settings::FontSizeValueLabel(value).c_str(),
                    "                        < 14 >  ");

  value.from_book = true;
  value.override_value = -1;
  test::ExpectEq("inherited font value",
                 settings::EffectiveStyleValue(value), 14);
  test::ExpectStrEq("inherited font label",
                    settings::FontSizeValueLabel(value).c_str(),
                    "                  inherit < 14 >");

  value.override_value = 18;
  test::ExpectEq("overridden font value",
                 settings::EffectiveStyleValue(value), 18);
  test::ExpectStrEq("overridden font label",
                    settings::FontSizeValueLabel(value).c_str(),
                    "                        < 18 >  ");

  value.uses_text_layout = false;
  test::ExpectStrEq("fixed layout font label",
                    settings::FontSizeValueLabel(value).c_str(),
                    "(PDF fixed)");
  test::ExpectStrEq("fixed layout line spacing label",
                    settings::LineSpacingValueLabel(value).c_str(),
                    "(PDF fixed)");
  test::ExpectStrEq("fixed layout paragraph spacing label",
                    settings::ParagraphSpacingValueLabel(value).c_str(),
                    "(PDF fixed)");

  value.uses_text_layout = true;
  value.global_value = 3;
  value.override_value = -1;
  test::ExpectStrEq("inherited line spacing label",
                    settings::LineSpacingValueLabel(value).c_str(),
                    "        inherit < 3 pixels >");
  test::ExpectStrEq("inherited paragraph spacing label",
                    settings::ParagraphSpacingValueLabel(value).c_str(),
                    "          inherit < 3 lines >");

  value.override_value = 5;
  test::ExpectStrEq("overridden line spacing label",
                    settings::LineSpacingValueLabel(value).c_str(),
                    "               < 5 pixels >  ");
  test::ExpectStrEq("overridden paragraph spacing label",
                    settings::ParagraphSpacingValueLabel(value).c_str(),
                    "                 < 5 lines >  ");

  test::ExpectStrEq(
      "global publisher setting enabled",
      settings::PublisherSettingValueLabel(false, true, -1, true, true).c_str(),
      "on");
  test::ExpectStrEq(
      "book publisher setting inherits enabled global",
      settings::PublisherSettingValueLabel(true, true, -1, true, true).c_str(),
      "inherit on");
  test::ExpectStrEq(
      "book publisher setting inherits disabled global",
      settings::PublisherSettingValueLabel(true, true, -1, false, false).c_str(),
      "inherit off");
  test::ExpectStrEq(
      "book publisher setting explicit enabled",
      settings::PublisherSettingValueLabel(true, true, 1, false, true).c_str(),
      "on");
  test::ExpectStrEq(
      "book publisher setting explicit disabled",
      settings::PublisherSettingValueLabel(true, true, 0, true, false).c_str(),
      "off");
  test::ExpectStrEq(
      "fixed layout publisher setting uses effective value",
      settings::PublisherSettingValueLabel(true, false, -1, true, true).c_str(),
      "on");

  return 0;
}
