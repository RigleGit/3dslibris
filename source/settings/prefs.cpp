/*
    3dslibris - prefs.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Changes by Rigle (summary):
    - Persist and restore 3DS-specific options (color mode, orientation, fonts).
    - Clamp legacy margin values to sensible 3DS screen bounds.
    - Keep per-book reading progress and bookmark state in PREFSPATH.
*/

#include "settings/prefs.h"

#include "3ds.h"
#include "app/app.h"
#include "shared/debug_log.h"
#include "book/book.h"
#include "book/book_xml.h"
#include "formats/common/xml_parse_utils.h"
#include "library/browser_view_utils.h"
#include "shared/path_utils.h"
#include "settings/font_config_utils.h"
#include "sys/stat.h"
#include "sys/time.h"
#include "ui/text_limits.h"
#include <stdio.h>
#include <sys/param.h>
#include <string>
#include <vector>

#define PARSEBUFSIZE 1024 * 64

namespace xml::prefs {

void start(void *data, const XML_Char *name, const XML_Char **attr) {
  // Central XML dispatcher for preference tags.
  // Each branch maps one persisted element into runtime App/Text state.
  parsedata_t *p = (parsedata_t *)data;
  App *app = App::GetInstance();
  if (!app || !p->prefs || !p->ts)
    return;
  int position = 0; //! Page position in book.
  char filename[MAXPATHLEN];
  bool current = false;
  int i;

  if (!strcmp(name, "library")) {
    for (i = 0; attr[i]; i += 2)
      if (!strcmp(attr[i], "modtime"))
        app->prefs->modtime = atoi(attr[i + 1]);
  } else if (!strcmp(name, "screen")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "brightness")) {
        // Ignored on 3DS (brightness is system-managed).
      } else if (!strcmp(attr[i], "colorMode")) {
        int mode = atoi(attr[i + 1]);
        app->colorMode = mode;
        app->ts->SetColorMode(mode);
        UiButtonSkin_SetColorMode(mode);
      } else if (!strcmp(attr[i], "flip")) {
        app->orientation = atoi(attr[i + 1]);
      }
    }
  } else if (!strcmp(name, "paragraph")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "spacing"))
        app->paraspacing = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "indent"))
        app->paraindent = atoi(attr[i + 1]);
    }
  } else if (!strcmp(name, "font")) {
    bool has_fallback_attrs = false;
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "fallback1") || !strcmp(attr[i], "fallback2") ||
          !strcmp(attr[i], "fallback3") || !strcmp(attr[i], "fallback4")) {
        has_fallback_attrs = true;
        break;
      }
    }
    if (has_fallback_attrs)
      app->ts->ClearFallbackFonts();

	    for (i = 0; attr[i]; i += 2) {
	      if (!strcmp(attr[i], "size"))
	        app->ts->SetPixelSize((u8)ClampTextPixelSize(atoi(attr[i + 1])));
	      else if (!strcmp(attr[i], "fallback1") && strlen(attr[i + 1]))
	        app->ts->SetFallbackFontFile(0, attr[i + 1]);
      else if (!strcmp(attr[i], "fallback2") && strlen(attr[i + 1]))
        app->ts->SetFallbackFontFile(1, attr[i + 1]);
      else if (!strcmp(attr[i], "fallback3") && strlen(attr[i + 1]))
        app->ts->SetFallbackFontFile(2, attr[i + 1]);
      else if (!strcmp(attr[i], "fallback4") && strlen(attr[i + 1]))
        app->ts->SetFallbackFontFile(3, attr[i + 1]);
	      else if (!strcmp(attr[i], "path")) {
	        if (strlen(attr[i + 1])) {
	          app->fontdir = std::string(attr[i + 1]);
	          if (app->ts)
	            app->ts->SetFontDir(app->fontdir);
	        }
	      } else {
	        u8 style = 0;
	        if (font_config_utils::StyleFromFontPrefAttr(attr[i], &style))
	          app->ts->SetFontFile((char *)attr[i + 1], style);
	      }
	    }
    if (has_fallback_attrs)
      app->ts->AutoLoadFallbackFonts();
  } else if (!strcmp(name, "books")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "reopen"))
        // For prefs where reopen was a string,
        // reopen will get turned off.
        app->reopen = atoi(attr[i + 1]);
      else if (!strcmp(attr[i], "path")) {
        if (strlen(attr[i + 1]))
          app->bookdir = std::string(attr[i + 1]);
      }
    }
  } else if (!strcmp(name, "book")) {
    strcpy(filename, "");
    current = false;
    position = 0;
    bool mobi_line_wrap_fix = false;
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "file"))
        snprintf(filename, sizeof(filename), "%s", attr[i + 1]);
      if (!strcmp(attr[i], "page"))
        position = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "mobiLineWrapFix"))
        mobi_line_wrap_fix = atoi(attr[i + 1]) != 0;
      if (!strcmp(attr[i], "current")) {
        // Should warn if multiple books are current...
        // the last current book will win.
        if (atoi(attr[i + 1]))
          current = true;
      }
    }

    // Find the book index for this library entry and set parsing context.
    // Subsequent <bookmark> tags attach to p->book until </book>.
    std::vector<Book *>::iterator it;
    for (it = app->books.begin(); it < app->books.end(); it++) {
      const char *bookname = (*it)->GetFileName();
      if (!strcmp(bookname, filename)) {
        // bookmark tags will refer to this.
        p->book = *it;
        // Per-book render fixes live alongside progress/bookmarks in prefs.
        (*it)->SetMobiLineWrapFix(mobi_line_wrap_fix);

        if (current) {
          // Set this book as current.
          app->SetCurrentBook(*it);
          app->SetSelectedBook(*it);
        }
        if (position)
          // Set current page in this book.
          (*it)->SetPosition(position - 1);

        break;
      }
    }
  } else if (!strcmp(name, "bookmark")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "page"))
        position = atoi(attr[i + 1]);
    }

    if (p->book) {
      p->book->GetBookmarks().push_back(position - 1);
    }
  } else if (!strcmp(name, "margin")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "left"))
        app->ts->margin.left = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "right"))
        app->ts->margin.right = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "top"))
        app->ts->margin.top = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "bottom")) {
        int parsedBottom = atoi(attr[i + 1]);
        // 3DS screens are 400/320px tall; legacy DS values like 65 leave too
        // much blank area, especially on the 320px screen.
        app->ts->margin.bottom = MIN(MAX(parsedBottom, 16), 36);
      }
    }
  } else if (!strcmp(name, "option")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "swapshoulder"))
        p->prefs->swapshoulder = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "time24h"))
        p->prefs->time24h = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "browserView")) {
        p->prefs->browser_view_mode =
            browser_view_utils::ParsePrefValue(attr[i + 1]);
      }
      if (!strcmp(attr[i], "fixedLayoutRtl")) {
        p->prefs->fixed_layout_rtl = atoi(attr[i + 1]) != 0;
      }
      if (!strcmp(attr[i], "respect_publisher_font_size")) {
        p->prefs->respect_publisher_font_size = atoi(attr[i + 1]) != 0;
      }
    }
  }
}

void end(void *data, const char *name) {
  //! Exit element callback for the prefs file.
  parsedata_t *p = (parsedata_t *)data;
  if (!strcmp(name, "book"))
    p->book = NULL;
}

} // namespace xml::prefs

namespace {

static std::string XmlEscapeAttr(const char *value) {
  if (!value)
    return std::string();
  std::string out;
  const size_t len = strlen(value);
  out.reserve(len + (len / 4));
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)value[i];
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      // XML 1.0 forbids most C0 control chars in attributes.
      out.push_back(' ');
      continue;
    }
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

} // namespace

Prefs::Prefs(App *_app) {
  app = _app ? _app : App::GetInstance();
  Init();
}
Prefs::~Prefs() {}

//! \return 0: success, 255: file open failure, 254: no bytes read, 253: parse
//! failure.
int Prefs::Read() {
  int err = 0;

  FILE *fp = fopen(paths::GetPrefsFile().c_str(), "r");
  if (!fp) {
    err = 255;
    return err;
  }

  parsedata_t pdata;
  parse_init(&pdata);
  pdata.prefs = this;
  pdata.reporter = app;
  pdata.ts = app->ts.get();

  xml_parse_utils::XmlParserOptions options;
  options.start_element = xml::prefs::start;
  options.end_element = xml::prefs::end;
  options.unknown_encoding = xml::book::unknown;
  options.user_data = &pdata;
  xml_parse_utils::XmlParseResult result =
      xml_parse_utils::ParseXmlFileStream(fp, options, PARSEBUFSIZE);
  fclose(fp);
  if (!result.ok)
    err = (int)result.error_code;
  return err;
}

void Prefs::Apply() {
  //! After Read().
  if (swapshoulder) {
    std::swap(app->key.l, app->key.r);
    std::swap(app->key.zl, app->key.zr);
  }
}

//! Write settings to prefs file.
//! \return Error code.
int Prefs::Write() {
  int err = 0;
  int colorMode = 0;

  if (app)
    colorMode = app->ts->GetColorMode();

  FILE *fp = fopen(paths::GetPrefsFile().c_str(), "w");
  if (!fp)
    return 255;

  fprintf(fp, "<dslibris format=\"2\">\n");
  fprintf(fp,
          "<option swapshoulder=\"%d\" time24h=\"%d\" browserView=\"%s\" fixedLayoutRtl=\"%d\" respect_publisher_font_size=\"%d\" />\n",
          swapshoulder, time24h,
          browser_view_utils::ToPrefValue(browser_view_mode),
          fixed_layout_rtl ? 1 : 0,
          respect_publisher_font_size ? 1 : 0);
  fprintf(fp, "\t<screen colorMode=\"%d\" flip=\"%d\" />\n", colorMode,
          app->orientation);
  fprintf(fp,
          "\t<margin top=\"%d\" left=\"%d\" bottom=\"%d\" right=\"%d\" />\n",
          app->ts->margin.top, app->ts->margin.left, app->ts->margin.bottom,
          app->ts->margin.right);
  const std::string font_regular =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_REGULAR).c_str());
  const std::string font_bold =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_BOLD).c_str());
  const std::string font_italic =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_ITALIC).c_str());
  const std::string font_bolditalic =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_BOLDITALIC).c_str());
  const std::string font_browser =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_BROWSER).c_str());
  const std::string font_mono =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_MONO).c_str());
  const std::string font_mono_bold =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_MONO_BOLD).c_str());
  const std::string font_mono_italic =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_MONO_ITALIC).c_str());
  const std::string font_mono_bolditalic =
      XmlEscapeAttr(app->ts->GetFontFile(TEXT_STYLE_MONO_BOLDITALIC).c_str());
  const std::string fallback1 =
      XmlEscapeAttr(app->ts->GetFallbackFontFile(0).c_str());
  const std::string fallback2 =
      XmlEscapeAttr(app->ts->GetFallbackFontFile(1).c_str());
  const std::string fallback3 =
      XmlEscapeAttr(app->ts->GetFallbackFontFile(2).c_str());
  const std::string fallback4 =
      XmlEscapeAttr(app->ts->GetFallbackFontFile(3).c_str());

  fprintf(fp,
          "\t<font size=\"%d\" %s=\"%s\" %s=\"%s\" %s=\"%s\" "
          "%s=\"%s\" %s=\"%s\" %s=\"%s\" %s=\"%s\" %s=\"%s\" "
          "%s=\"%s\" fallback1=\"%s\" "
          "fallback2=\"%s\" fallback3=\"%s\" fallback4=\"%s\" />\n",
          app->ts->GetPixelSize(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_REGULAR),
          font_regular.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_BOLD),
          font_bold.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_ITALIC),
          font_italic.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_BOLDITALIC),
          font_bolditalic.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_BROWSER),
          font_browser.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_MONO),
          font_mono.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_MONO_BOLD),
          font_mono_bold.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_MONO_ITALIC),
          font_mono_italic.c_str(),
          font_config_utils::FontPrefAttrForStyle(TEXT_STYLE_MONO_BOLDITALIC),
          font_mono_bolditalic.c_str(), fallback1.c_str(), fallback2.c_str(),
          fallback3.c_str(), fallback4.c_str());
  fprintf(fp, "\t<paragraph indent=\"%d\" spacing=\"%d\" />\n", app->paraindent,
          app->paraspacing);
  fprintf(fp, "\t<books reopen=\"%d\">\n", app->reopen);

  // Persist all known books so last page and bookmarks survive restarts.
  for (int i = 0; i < app->BookCount(); i++) {
    Book *book = app->books[i];
    const std::string escaped_filename = XmlEscapeAttr(book->GetFileName());
    fprintf(fp, "\t\t<book file=\"%s\" page=\"%d\"", escaped_filename.c_str(),
            book->GetPosition() + 1);
    // Only persist the override when enabled so old prefs stay readable.
    if (book->GetMobiLineWrapFix())
      fprintf(fp, " mobiLineWrapFix=\"1\"");
    if (app->GetCurrentBook() == app->books[i])
      fprintf(fp, " current=\"1\"");
    fprintf(fp, ">\n");
    std::list<u16> &bookmarks = book->GetBookmarks();
    for (std::list<u16>::iterator j = bookmarks.begin(); j != bookmarks.end();
         j++) {
      fprintf(fp, "\t\t\t<bookmark page=\"%d\" word=\"%d\" />\n", *j + 1, 0);
    }

    fprintf(fp, "\t\t</book>\n");
  }

  fprintf(fp, "\t</books>\n");

  fprintf(fp, "</dslibris>\n");
  fprintf(fp, "\n");
  fclose(fp);

  return err;
}

void Prefs::Init() {
  modtime = 0; // fill this in with gettimeofday()
  swapshoulder = false;
  time24h = true;
  browser_view_mode = BROWSER_VIEW_GALLERY;
  fixed_layout_rtl = false;
  respect_publisher_font_size = false;
}
