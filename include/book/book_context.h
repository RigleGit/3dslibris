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
  void (*draw_background)(void *);    //! Non-owning callback (bottom screen).
  void *draw_background_user_data;    //! Non-owning callback user data.
  void (*draw_top_background)(void *); //! Non-owning callback (top screen).
  void *draw_top_background_user_data; //! Non-owning callback user data.
  void (*on_spine_progress)(unsigned done, unsigned total, void *user_data); //! Non-owning; called during EPUB spine parse.
  void *on_spine_progress_user_data;   //! Non-owning callback user data.

  BookContext()
      : text(nullptr),
        prefs(nullptr),
        paragraph_spacing(nullptr),
        paragraph_indent(nullptr),
        orientation(nullptr),
        status_reporter(nullptr),
        draw_background(nullptr),
        draw_background_user_data(nullptr),
        draw_top_background(nullptr),
        draw_top_background_user_data(nullptr),
        on_spine_progress(nullptr),
        on_spine_progress_user_data(nullptr) {}
};
