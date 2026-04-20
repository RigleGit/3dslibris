/*
    Shared non-owning runtime context passed into Book.

    Provides access to rendering/services owned elsewhere (Text, Prefs,
    status reporting, layout settings, and background drawing hooks)
    without coupling Book directly to App.
*/

#pragma once

class Text;
class Prefs;
class IStatusReporter;

struct BookContext {
  Text *text;                         //! Non-owning.
  Prefs *prefs;                       //! Non-owning.
  const unsigned char *paragraph_spacing; //! Non-owning.
  const unsigned char *paragraph_indent;  //! Non-owning.
  const unsigned char *orientation;       //! Non-owning.
  IStatusReporter *status_reporter;   //! Non-owning.
  void (*draw_background)(void *);    //! Non-owning callback.
  void *draw_background_user_data;    //! Non-owning callback user data.

  BookContext()
      : text(nullptr),
        prefs(nullptr),
        paragraph_spacing(nullptr),
        paragraph_indent(nullptr),
        orientation(nullptr),
        status_reporter(nullptr),
        draw_background(nullptr),
        draw_background_user_data(nullptr) {}
};
