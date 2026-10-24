#pragma once

#include <string>

namespace settings {

int ClampLineSpacingSetting(int value);

// Value snapshot for one <paragraph> tag. Presence matters: an omitted
// lineSpacing must not synchronize an independently configured renderer.
struct ParagraphPreferences {
  int spacing;
  int indent;
  int line_spacing;
  bool publisher_text_indent;
  bool publisher_block_margins;
  bool publisher_horizontal_margins;
  bool line_spacing_specified;
};

ParagraphPreferences ParseParagraphPreferences(
    const char *const *attributes, ParagraphPreferences current);

struct StyleValueContext {
  bool from_book;
  bool uses_text_layout;
  int global_value;
  int override_value;

  StyleValueContext()
      : from_book(false), uses_text_layout(true), global_value(0),
        override_value(-1) {}
};

// Missing side-margin settings in old configs inherit the former combined value.
int ReadPublisherHorizontalMargins(const char *const *attributes,
                                   int legacy_margins);

int EffectiveStyleValue(const StyleValueContext &context);
std::string FontSizeValueLabel(const StyleValueContext &context);
std::string LineSpacingValueLabel(const StyleValueContext &context);
std::string ParagraphSpacingValueLabel(const StyleValueContext &context);
std::string PublisherSettingValueLabel(bool from_book, bool uses_text_layout,
                                       int override_value,
                                       bool global_enabled,
                                       bool effective_enabled);

} // namespace settings
