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
#include "shared/path_constants.h"
#include "settings/font_config_utils.h"
#include "sys/stat.h"
#include "sys/time.h"
#include "ui/text_limits.h"
#include <stdio.h>
#include <sys/param.h>
#include <string>
#include <vector>

#define PARSEBUFSIZE 1024 * 64

namespace {

static int ClampLineSpacingSetting(int value) {
  if (value < 0)
    return 0;
  if (value > 16)
    return 16;
  return value;
}

}

std::string Prefs::MakeBookKey(const char *folder, const char *filename) {
  return std::string(folder ? folder : "") + "\n" +
         std::string(filename ? filename : "");
}

namespace xml::prefs {

void start(void *data, const XML_Char *name, const XML_Char **attr) {
  // Central XML dispatcher for preference tags.
  // Each branch maps one persisted element into runtime App/Text state.
  parsedata_t *p = (parsedata_t *)data;
  if (!p->prefs || !p->ts)
    return;
  App *app = p->prefs->GetApp();
  if (!app)
    return;
  Text *ts = p->ts;
  int position = 0; //! Page position in book.
  char filename[MAXPATHLEN];
  char folder[MAXPATHLEN];
  bool current = false;
  int i;

  if (!strcmp(name, "library")) {
    for (i = 0; attr[i]; i += 2)
      if (!strcmp(attr[i], "modtime"))
        p->prefs->modtime = atoi(attr[i + 1]);
  } else if (!strcmp(name, "screen")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "brightness")) {
        // Ignored on 3DS (brightness is system-managed).
      } else if (!strcmp(attr[i], "colorMode")) {
        int mode = atoi(attr[i + 1]);
        app->colorMode = mode;
        ts->SetColorMode(mode);
        UiButtonSkin_SetColorMode(mode);
      } else if (!strcmp(attr[i], "flip")) {
        app->orientation = atoi(attr[i + 1]);
      }
    }
  } else if (!strcmp(name, "paragraph")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "spacing"))
        app->paraspacing = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "lineSpacing")) {
        app->reader_line_spacing = ClampLineSpacingSetting(atoi(attr[i + 1]));
        ts->linespacing = app->reader_line_spacing;
      }
      if (!strcmp(attr[i], "indent"))
        app->paraindent = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "publisherTextIndent"))
        app->publisher_text_indent = atoi(attr[i + 1]) != 0;
      if (!strcmp(attr[i], "publisherBlockMargins"))
        app->publisher_block_margins = atoi(attr[i + 1]) != 0;
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
      ts->ClearFallbackFonts();

    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "size")) {
        app->reader_font_size = ClampTextPixelSize(atoi(attr[i + 1]));
        ts->SetPixelSize((u8)app->reader_font_size);
      }
      else if (!strcmp(attr[i], "fallback1") && strlen(attr[i + 1]))
        ts->SetFallbackFontFile(0, attr[i + 1]);
      else if (!strcmp(attr[i], "fallback2") && strlen(attr[i + 1]))
        ts->SetFallbackFontFile(1, attr[i + 1]);
      else if (!strcmp(attr[i], "fallback3") && strlen(attr[i + 1]))
        ts->SetFallbackFontFile(2, attr[i + 1]);
      else if (!strcmp(attr[i], "fallback4") && strlen(attr[i + 1]))
        ts->SetFallbackFontFile(3, attr[i + 1]);
      else if (!strcmp(attr[i], "path")) {
        if (strlen(attr[i + 1])) {
          app->fontdir = std::string(attr[i + 1]);
          ts->SetFontDir(app->fontdir);
        }
      } else {
        u8 style = 0;
        if (font_config_utils::StyleFromFontPrefAttr(attr[i], &style))
          ts->SetFontFile((char *)attr[i + 1], style);
      }
    }
    if (has_fallback_attrs)
      ts->AutoLoadFallbackFonts();
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
    strcpy(folder, "");
    p->book = NULL;
    current = false;
    position = 0;
    bool mobi_line_wrap_fix = false;
    int style_font_size = -1;
    int style_line_spacing = -1;
    int style_paragraph_spacing = -1;
    int style_publisher_text_indent = -1;
    int style_publisher_block_margins = -1;
    uint32_t last_opened = 0;
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "file"))
        snprintf(filename, sizeof(filename), "%s", attr[i + 1]);
      if (!strcmp(attr[i], "folder"))
        snprintf(folder, sizeof(folder), "%s", attr[i + 1]);
      if (!strcmp(attr[i], "page"))
        position = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "mobiLineWrapFix"))
        mobi_line_wrap_fix = atoi(attr[i + 1]) != 0;
      if (!strcmp(attr[i], "fontSize"))
        style_font_size = ClampTextPixelSize(atoi(attr[i + 1]));
      if (!strcmp(attr[i], "lineSpacing"))
        style_line_spacing = ClampLineSpacingSetting(atoi(attr[i + 1]));
      if (!strcmp(attr[i], "paragraphSpacing"))
        style_paragraph_spacing = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "publisherTextIndent"))
        style_publisher_text_indent = atoi(attr[i + 1]) != 0 ? 1 : 0;
      if (!strcmp(attr[i], "publisherBlockMargins"))
        style_publisher_block_margins = atoi(attr[i + 1]) != 0 ? 1 : 0;
      if (!strcmp(attr[i], "lastOpened"))
        last_opened = (uint32_t)atol(attr[i + 1]);
      if (!strcmp(attr[i], "current")) {
        // Should warn if multiple books are current...
        // the last current book will win.
        if (atoi(attr[i + 1]))
          current = true;
      }
    }

    if (filename[0] && last_opened > 0)
      p->prefs->RememberSavedLastOpened(folder, filename, last_opened);

    // Find the book index for this library entry and set parsing context.
    // Subsequent <bookmark> tags attach to p->book until </book>.
    Book *matched = NULL;
    if (current) {
      p->prefs->SetPendingCurrentBookRestore(
          folder, filename, position, mobi_line_wrap_fix,
          style_font_size, style_line_spacing,
          style_paragraph_spacing, style_publisher_text_indent,
          style_publisher_block_margins);
    } else {
      std::vector<Book *>::iterator it;
      for (it = app->books.begin(); it < app->books.end(); it++) {
        const char *bookname = (*it)->GetFileName();
        if (!strcmp(bookname, filename) &&
            (!folder[0] || !strcmp((*it)->GetFolderName(), folder))) {
          matched = *it;
          break;
        }
      }
    }

    if (matched) {
      // bookmark tags will refer to this.
      p->book = matched;
      // Per-book render fixes live alongside progress/bookmarks in prefs.
      matched->SetMobiLineWrapFix(mobi_line_wrap_fix);
      matched->SetStyleFontSizeOverride(style_font_size);
      matched->SetStyleLineSpacingOverride(style_line_spacing);
      matched->SetStyleParagraphSpacingOverride(style_paragraph_spacing);
      matched->SetStylePublisherTextIndentOverride(style_publisher_text_indent);
      matched->SetStylePublisherBlockMarginsOverride(style_publisher_block_margins);
      if (last_opened > 0)
        matched->SetLastOpenedTime(last_opened);
      DBG_LOGF(app, "recently-opened: read lastOpened=%lu matched=%s file=\"%s\"",
               (unsigned long)last_opened,
               matched ? "yes" : "no",
               filename);

      if (current) {
        // Set this book as current.
        app->SetCurrentBook(matched);
        app->SetSelectedBook(matched);
      }
      if (position)
        // Set current page in this book.
        matched->SetPosition(position - 1);
    }
  } else if (!strcmp(name, "bookmark")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "page"))
        position = atoi(attr[i + 1]);
    }

    if (p->book) {
      p->book->GetBookmarks().push_back(position - 1);
    } else if (p->prefs) {
      p->prefs->AddPendingCurrentBookBookmark(position - 1);
    }
  } else if (!strcmp(name, "margin")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "left"))
        ts->margin.left = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "right"))
        ts->margin.right = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "top"))
        ts->margin.top = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "bottom")) {
        int parsedBottom = atoi(attr[i + 1]);
        // 3DS screens are 400/320px tall; legacy DS values like 65 leave too
        // much blank area, especially on the 320px screen.
        ts->margin.bottom = MIN(MAX(parsedBottom, 16), 36);
      }
    }
  } else if (!strcmp(name, "option")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "swapshoulder"))
        p->prefs->swapshoulder = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "time24h"))
        p->prefs->time24h = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "showTimeRemaining"))
        p->prefs->show_time_remaining = atoi(attr[i + 1]) != 0;
      if (!strcmp(attr[i], "browserView")) {
        p->prefs->browser_view_mode =
            browser_view_utils::ParsePrefValue(attr[i + 1]);
      }
      if (!strcmp(attr[i], "fixedLayoutRtl")) {
        p->prefs->fixed_layout_rtl = atoi(attr[i + 1]) != 0;
      }
      if (!strcmp(attr[i], "circlePadPageTurn")) {
        p->prefs->circle_pad_page_turn = atoi(attr[i + 1]) != 0;
      }
      if (!strcmp(attr[i], "librarySortMode")) {
        int m = atoi(attr[i + 1]);
        if (m >= 0 && m < LIBRARY_SORT_COUNT)
          p->prefs->library_sort_mode = static_cast<LibrarySortMode>(m);
      }
    }
  }
}

void end(void *data, const char *name) {
  //! Exit element callback for the prefs file.
  parsedata_t *p = (parsedata_t *)data;
  if (!strcmp(name, "book")) {
    p->book = NULL;
    if (p->prefs)
      p->prefs->EndPendingCurrentBookRestoreEntry();
  }
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
  ClearPendingCurrentBookRestore();
  last_opened_by_book_key.clear();

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

void Prefs::ClearPendingCurrentBookRestore() {
  pending_current_book_restore = false;
  collecting_pending_current_book = false;
  pending_current_folder.clear();
  pending_current_filename.clear();
  pending_current_position = 0;
  pending_current_mobi_line_wrap_fix = false;
  pending_current_style_font_size = -1;
  pending_current_style_line_spacing = -1;
  pending_current_style_paragraph_spacing = -1;
  pending_current_style_publisher_text_indent = -1;
  pending_current_style_publisher_block_margins = -1;
  pending_current_bookmarks.clear();
}

void Prefs::SetPendingCurrentBookRestore(
    const char *folder, const char *filename, int position,
    bool mobi_line_wrap_fix, int style_font_size, int style_line_spacing,
    int style_paragraph_spacing,
    int style_publisher_text_indent, int style_publisher_block_margins) {
  pending_current_book_restore = filename && filename[0];
  collecting_pending_current_book = pending_current_book_restore;
  pending_current_folder = folder ? folder : "";
  pending_current_filename = filename ? filename : "";
  pending_current_position = position;
  pending_current_mobi_line_wrap_fix = mobi_line_wrap_fix;
  pending_current_style_font_size = style_font_size;
  pending_current_style_line_spacing = style_line_spacing;
  pending_current_style_paragraph_spacing = style_paragraph_spacing;
  pending_current_style_publisher_text_indent = style_publisher_text_indent;
  pending_current_style_publisher_block_margins =
      style_publisher_block_margins;
  pending_current_bookmarks.clear();
}

void Prefs::AddPendingCurrentBookBookmark(uint16_t page) {
  if (collecting_pending_current_book)
    pending_current_bookmarks.push_back(page);
}

void Prefs::EndPendingCurrentBookRestoreEntry() {
  collecting_pending_current_book = false;
}

bool Prefs::ApplyPendingCurrentBookRestore() {
  if (!pending_current_book_restore || !app)
    return false;

  Book *matched = app->RestoreSavedBookSelection(
      pending_current_folder.c_str(), pending_current_filename.c_str());
  if (!matched) {
    ClearPendingCurrentBookRestore();
    return false;
  }

  matched->SetMobiLineWrapFix(pending_current_mobi_line_wrap_fix);
  matched->SetStyleFontSizeOverride(pending_current_style_font_size);
  matched->SetStyleLineSpacingOverride(pending_current_style_line_spacing);
  matched->SetStyleParagraphSpacingOverride(
      pending_current_style_paragraph_spacing);
  matched->SetStylePublisherTextIndentOverride(
      pending_current_style_publisher_text_indent);
  matched->SetStylePublisherBlockMarginsOverride(
      pending_current_style_publisher_block_margins);
  if (pending_current_position)
    matched->SetPosition(pending_current_position - 1);
  for (size_t i = 0; i < pending_current_bookmarks.size(); i++)
    matched->GetBookmarks().push_back(pending_current_bookmarks[i]);

  app->SetCurrentBook(matched);
  app->SetSelectedBook(matched);
  ClearPendingCurrentBookRestore();
  return true;
}

void Prefs::RememberSavedLastOpened(const char *folder, const char *filename,
                                    uint32_t last_opened) {
  if (!filename || !filename[0] || last_opened == 0)
    return;
  last_opened_by_book_key[MakeBookKey(folder, filename)] = last_opened;
}

void Prefs::ApplySavedBookState(Book *book) const {
  if (!book)
    return;
  const auto it =
      last_opened_by_book_key.find(MakeBookKey(book->GetFolderName(),
                                               book->GetFileName()));
  if (it != last_opened_by_book_key.end())
    book->SetLastOpenedTime(it->second);
}

//! Write settings to prefs file.
//! \return Error code.
int Prefs::Write() {
  int err = 0;
  Text *ts = app ? app->ts.get() : nullptr;

  FILE *fp = fopen(paths::GetPrefsFile().c_str(), "w");
  if (!fp)
    return 255;

  fprintf(fp, "<dslibris format=\"2\">\n");
  fprintf(fp,
      "<option swapshoulder=\"%d\" time24h=\"%d\" showTimeRemaining=\"%d\" browserView=\"%s\" fixedLayoutRtl=\"%d\" circlePadPageTurn=\"%d\" librarySortMode=\"%d\" />\n",
          swapshoulder, time24h,
      show_time_remaining ? 1 : 0,
          browser_view_utils::ToPrefValue(browser_view_mode),
          fixed_layout_rtl ? 1 : 0,
          circle_pad_page_turn ? 1 : 0,
          static_cast<int>(library_sort_mode));
  fprintf(fp, "\t<screen colorMode=\"%d\" flip=\"%d\" />\n",
          ts ? ts->GetColorMode() : 0,
          app ? app->orientation : 0);
  fprintf(fp,
          "\t<margin top=\"%d\" left=\"%d\" bottom=\"%d\" right=\"%d\" />\n",
          ts ? ts->margin.top : 0, ts ? ts->margin.left : 0,
          ts ? ts->margin.bottom : 0, ts ? ts->margin.right : 0);
  const std::string font_regular =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_REGULAR).c_str() : "");
  const std::string font_bold =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_BOLD).c_str() : "");
  const std::string font_italic =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_ITALIC).c_str() : "");
  const std::string font_bolditalic =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_BOLDITALIC).c_str() : "");
  const std::string font_browser =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_BROWSER).c_str() : "");
  const std::string font_mono =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_MONO).c_str() : "");
  const std::string font_mono_bold =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_MONO_BOLD).c_str() : "");
  const std::string font_mono_italic =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_MONO_ITALIC).c_str() : "");
  const std::string font_mono_bolditalic =
      XmlEscapeAttr(ts ? ts->GetFontFile(TEXT_STYLE_MONO_BOLDITALIC).c_str() : "");
  const std::string fallback1 =
      XmlEscapeAttr(ts ? ts->GetFallbackFontFile(0).c_str() : "");
  const std::string fallback2 =
      XmlEscapeAttr(ts ? ts->GetFallbackFontFile(1).c_str() : "");
  const std::string fallback3 =
      XmlEscapeAttr(ts ? ts->GetFallbackFontFile(2).c_str() : "");
  const std::string fallback4 =
      XmlEscapeAttr(ts ? ts->GetFallbackFontFile(3).c_str() : "");

  fprintf(fp,
          "\t<font size=\"%d\" %s=\"%s\" %s=\"%s\" %s=\"%s\" "
          "%s=\"%s\" %s=\"%s\" %s=\"%s\" %s=\"%s\" %s=\"%s\" "
          "%s=\"%s\" fallback1=\"%s\" "
          "fallback2=\"%s\" fallback3=\"%s\" fallback4=\"%s\" />\n",
          app ? app->reader_font_size : (ts ? ts->GetPixelSize() : 12),
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
  fprintf(fp,
          "\t<paragraph indent=\"%d\" spacing=\"%d\" lineSpacing=\"%d\" publisherTextIndent=\"%d\" publisherBlockMargins=\"%d\" />\n",
          app->paraindent, app->paraspacing, app->reader_line_spacing,
          app->publisher_text_indent ? 1 : 0,
          app->publisher_block_margins ? 1 : 0);
  fprintf(fp, "\t<books reopen=\"%d\">\n", app->reopen);

  // Persist all known books so last page and bookmarks survive restarts.
  for (int i = 0; i < app->BookCount(); i++) {
    Book *book = app->books[i];
    if (!book || book->IsBrowserFolder())
      continue;
    const std::string escaped_filename = XmlEscapeAttr(book->GetFileName());
    const std::string escaped_folder = XmlEscapeAttr(book->GetFolderName());
    fprintf(fp, "\t\t<book file=\"%s\" folder=\"%s\" page=\"%d\"",
            escaped_filename.c_str(), escaped_folder.c_str(),
            book->GetPosition() + 1);
    // Only persist the override when enabled so old prefs stay readable.
    if (book->GetMobiLineWrapFix())
      fprintf(fp, " mobiLineWrapFix=\"1\"");
    if (book->GetStyleFontSizeOverride() >= 0)
      fprintf(fp, " fontSize=\"%d\"",
              book->GetStyleFontSizeOverride());
    if (book->GetStyleLineSpacingOverride() >= 0)
      fprintf(fp, " lineSpacing=\"%d\"",
              book->GetStyleLineSpacingOverride());
    if (book->GetStyleParagraphSpacingOverride() >= 0)
      fprintf(fp, " paragraphSpacing=\"%d\"",
              book->GetStyleParagraphSpacingOverride());
    if (book->GetStylePublisherTextIndentOverride() >= 0)
      fprintf(fp, " publisherTextIndent=\"%d\"",
              book->GetStylePublisherTextIndentOverride());
    if (book->GetStylePublisherBlockMarginsOverride() >= 0)
      fprintf(fp, " publisherBlockMargins=\"%d\"",
              book->GetStylePublisherBlockMarginsOverride());
    if (book->GetLastOpenedTime() > 0) {
      fprintf(fp, " lastOpened=\"%lu\"", (unsigned long)book->GetLastOpenedTime());
      DBG_LOGF(app, "recently-opened: write lastOpened=%lu for \"%s\"",
               (unsigned long)book->GetLastOpenedTime(),
               book->GetFileName() ? book->GetFileName() : "?");
    }
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

  last_opened_by_book_key.clear();
  for (int i = 0; i < app->BookCount(); i++) {
    Book *book = app->books[i];
    if (!book || book->IsBrowserFolder() || book->GetLastOpenedTime() == 0)
      continue;
    last_opened_by_book_key[MakeBookKey(book->GetFolderName(),
                                        book->GetFileName())] =
        book->GetLastOpenedTime();
  }

  return err;
}

void Prefs::Init() {
  modtime = 0; // fill this in with gettimeofday()
  swapshoulder = false;
  time24h = true;
  show_time_remaining = false;
  browser_view_mode = BROWSER_VIEW_GALLERY;
  fixed_layout_rtl = false;
  circle_pad_page_turn = true;
  library_sort_mode = LIBRARY_SORT_TITLE;
  last_opened_by_book_key.clear();
  ClearPendingCurrentBookRestore();
}
