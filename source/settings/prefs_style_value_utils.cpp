#include "settings/prefs_style_value_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace settings {

int ClampLineSpacingSetting(int value) {
  if (value < 0)
    return 0;
  if (value > 16)
    return 16;
  return value;
}

int ReadPublisherHorizontalMargins(const char *const *attributes,
                                   int legacy_margins) {
  if (attributes) {
    for (int i = 0; attributes[i]; i += 2) {
      if (!strcmp(attributes[i], "publisherHorizontalMargins")) {
        const int value = atoi(attributes[i + 1]);
        return value < 0 ? -1 : (value != 0 ? 1 : 0);
      }
    }
  }
  return legacy_margins;
}

ParagraphPreferences ParseParagraphPreferences(
    const char *const *attributes, ParagraphPreferences current) {
  current.line_spacing_specified = false;
  for (int i = 0; attributes[i]; i += 2) {
    if (!strcmp(attributes[i], "spacing"))
      current.spacing = atoi(attributes[i + 1]);
    if (!strcmp(attributes[i], "lineSpacing")) {
      current.line_spacing = ClampLineSpacingSetting(atoi(attributes[i + 1]));
      current.line_spacing_specified = true;
    }
    if (!strcmp(attributes[i], "indent"))
      current.indent = atoi(attributes[i + 1]);
    if (!strcmp(attributes[i], "publisherTextIndent"))
      current.publisher_text_indent = atoi(attributes[i + 1]) != 0;
    if (!strcmp(attributes[i], "publisherBlockMargins"))
      current.publisher_block_margins = atoi(attributes[i + 1]) != 0;
  }
  // Old configs used block margins for both axes. Explicit -1 continues to
  // convert to true for the global boolean, as the existing reader did.
  current.publisher_horizontal_margins =
      ReadPublisherHorizontalMargins(
          attributes, current.publisher_block_margins ? 1 : 0) != 0;
  return current;
}

int EffectiveStyleValue(const StyleValueContext &context) {
  if (context.from_book && context.override_value >= 0)
    return context.override_value;
  return context.global_value;
}

std::string FontSizeValueLabel(const StyleValueContext &context) {
  if (context.from_book && !context.uses_text_layout)
    return std::string("(PDF fixed)");

  char label[64];
  if (context.from_book && context.override_value < 0) {
    snprintf(label, sizeof(label), "                  inherit < %d >",
             context.global_value);
  } else {
    snprintf(label, sizeof(label), "                        < %d >  ",
             EffectiveStyleValue(context));
  }
  return std::string(label);
}

std::string LineSpacingValueLabel(const StyleValueContext &context) {
  if (context.from_book && !context.uses_text_layout)
    return std::string("(PDF fixed)");

  char label[64];
  if (context.from_book && context.override_value < 0) {
    snprintf(label, sizeof(label), "        inherit < %d pixels >",
             context.global_value);
  } else {
    snprintf(label, sizeof(label), "               < %d pixels >  ",
             EffectiveStyleValue(context));
  }
  return std::string(label);
}

std::string ParagraphSpacingValueLabel(const StyleValueContext &context) {
  if (context.from_book && !context.uses_text_layout)
    return std::string("(PDF fixed)");

  char label[64];
  if (context.from_book && context.override_value < 0) {
    snprintf(label, sizeof(label), "          inherit < %d lines >",
             context.global_value);
  } else {
    snprintf(label, sizeof(label), "                 < %d lines >  ",
             EffectiveStyleValue(context));
  }
  return std::string(label);
}

std::string PublisherSettingValueLabel(bool from_book, bool uses_text_layout,
                                       int override_value,
                                       bool global_enabled,
                                       bool effective_enabled) {
  if (from_book && uses_text_layout && override_value < 0)
    return global_enabled ? std::string("inherit on")
                          : std::string("inherit off");
  const bool enabled = from_book ? effective_enabled : global_enabled;
  return enabled ? std::string("on") : std::string("off");
}

} // namespace settings
