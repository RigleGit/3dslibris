#pragma once

#include <string>

namespace settings {

int ClampLineSpacingSetting(int value);

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
