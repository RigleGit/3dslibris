#pragma once

class Text;
class Prefs;
class IStatusReporter;

struct BookContext {
  Text *text;
  Prefs *prefs;
  const unsigned char *paragraph_spacing;
  const unsigned char *paragraph_indent;
  const unsigned char *orientation;
  IStatusReporter *status_reporter;
  void (*draw_background)(void *);
  void *draw_background_user_data;

  BookContext()
      : text(nullptr), prefs(nullptr), paragraph_spacing(nullptr),
        paragraph_indent(nullptr), orientation(nullptr),
        status_reporter(nullptr), draw_background(nullptr),
        draw_background_user_data(nullptr) {}
};
