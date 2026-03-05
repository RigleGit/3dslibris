#include "prefs.h"

#include "3ds.h"
#include "app.h"
#include "book.h"
#include "main.h"
#include "sys/stat.h"
#include "sys/time.h"
#include <stdio.h>
#include <sys/param.h>
#include <vector>

#define PARSEBUFSIZE 1024 * 64

namespace xml::prefs {

void start(void *data, const XML_Char *name, const XML_Char **attr) {
  parsedata_t *p = (parsedata_t *)data;
  App *app = p->app;
  int position = 0; //! Page position in book.
  char filename[MAXPATHLEN];
  bool current = false;
  int i;

  if (!strcmp(name, "library")) {
    for (i = 0; attr[i]; i += 2)
      if (!strcmp(attr[i], "modtime"))
        app->prefs->modtime = atoi(attr[i + 1]);
  }
  // TODO this will never run
  else if (!strcmp(name, "library")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "folder"))
        app->bookdir = std::string(attr[i + 1]);
    }
  } else if (!strcmp(name, "screen")) {
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "brightness")) {
        app->brightness = atoi(attr[i + 1]);
        app->brightness = app->brightness % 4;
      } else if (!strcmp(attr[i], "colorMode")) {
        app->colorMode = atoi(attr[i + 1]);
        app->ts->SetColorMode(atoi(attr[i + 1]));
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
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "size"))
        app->ts->SetPixelSize(atoi(attr[i + 1]));
      else if (!strcmp(attr[i], "normal"))
        app->ts->SetFontFile((char *)attr[i + 1], TEXT_STYLE_REGULAR);
      else if (!strcmp(attr[i], "bold"))
        app->ts->SetFontFile((char *)attr[i + 1], TEXT_STYLE_BOLD);
      else if (!strcmp(attr[i], "italic"))
        app->ts->SetFontFile((char *)attr[i + 1], TEXT_STYLE_ITALIC);
      else if (!strcmp(attr[i], "bolditalic"))
        app->ts->SetFontFile((char *)attr[i + 1], TEXT_STYLE_BOLDITALIC);
      else if (!strcmp(attr[i], "path")) {
        if (strlen(attr[i + 1]))
          app->fontdir = std::string(attr[i + 1]);
      }
    }
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
    for (i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "file"))
        strcpy(filename, attr[i + 1]);
      if (!strcmp(attr[i], "page"))
        position = atoi(attr[i + 1]);
      if (!strcmp(attr[i], "current")) {
        // Should warn if multiple books are current...
        // the last current book will win.
        if (atoi(attr[i + 1]))
          current = true;
      }
    }

    // Find the book index for this library entry
    // and set context for later bookmarks.
    std::vector<Book *>::iterator it;
    for (it = app->books.begin(); it < app->books.end(); it++) {
      const char *bookname = (*it)->GetFileName();
      if (!strcmp(bookname, filename)) {
        // bookmark tags will refer to this.
        p->book = *it;

        if (current) {
          // Set this book as current.
          app->bookcurrent = *it;
          app->bookselected = *it;
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
      p->book->GetBookmarks()->push_back(position - 1);
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

Prefs::Prefs(App *_app) {
  app = _app;
  Init();
}
Prefs::~Prefs() {}

//! \return 0: success, 255: file open failure, 254: no bytes read, 253: parse
//! failure.
int Prefs::Read() {
  int err = 0;

  FILE *fp = fopen(PREFSPATH, "r");
  if (!fp) {
    err = 255;
    return err;
  }

  parsedata_t pdata;
  parse_init(&pdata);
  pdata.prefs = this;
  pdata.app = app;
  pdata.ts = app->ts;

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    fclose(fp);
    err = 254;
    return err;
  }
  XML_SetUnknownEncodingHandler(p, xml::book::unknown, NULL);
  XML_SetStartElementHandler(p, xml::prefs::start);
  XML_SetEndElementHandler(p, xml::prefs::end);
  XML_SetUserData(p, (void *)&pdata);
  while (true) {
    void *buff = XML_GetBuffer(p, PARSEBUFSIZE);
    int bytes_read = fread(buff, sizeof(char), PARSEBUFSIZE, fp);
    if (bytes_read < 0) {
      err = 254;
      break;
    }
    enum XML_Status status = XML_ParseBuffer(p, bytes_read, bytes_read == 0);
    if (status == XML_STATUS_ERROR) {
      err = XML_GetErrorCode(p);
      break;
    }
    if (bytes_read == 0)
      break;
  }
  XML_ParserFree(p);
  fclose(fp);
  return err;
}

void Prefs::Apply() {
  //! After Read().
  if (swapshoulder) {
    int tmp = app->key.l;
    app->key.l = app->key.r;
    app->key.r = tmp;
  }
}

//! Write settings to PREFSPATH.
//! \return Error code.
int Prefs::Write() {
  if (app->melonds)
    return 0;

  int err = 0;
  int colorMode = 0;

  if (app)
    colorMode = app->ts->GetColorMode();

  FILE *fp = fopen(PREFSPATH, "w");
  if (!fp)
    return 255;

  fprintf(fp, "<dslibris format=\"2\">\n");
  fprintf(fp, "<option swapshoulder=\"%d\" time24h=\"%d\" />\n", swapshoulder,
          time24h);
  fprintf(fp, "\t<screen colorMode=\"%d\" flip=\"%d\" />\n", colorMode,
          app->orientation);
  fprintf(fp,
          "\t<margin top=\"%d\" left=\"%d\" bottom=\"%d\" right=\"%d\" />\n",
          app->ts->margin.top, app->ts->margin.left, app->ts->margin.bottom,
          app->ts->margin.right);
  fprintf(fp,
          "\t<font size=\"%d\" normal=\"%s\" bold=\"%s\" italic=\"%s\" "
          "bolditalic=\"%s\" />\n",
          app->ts->GetPixelSize(),
          app->ts->GetFontFile(TEXT_STYLE_REGULAR).c_str(),
          app->ts->GetFontFile(TEXT_STYLE_BOLD).c_str(),
          app->ts->GetFontFile(TEXT_STYLE_ITALIC).c_str(),
          app->ts->GetFontFile(TEXT_STYLE_BOLDITALIC).c_str());
  fprintf(fp, "\t<paragraph indent=\"%d\" spacing=\"%d\" />\n", app->paraindent,
          app->paraspacing);
  fprintf(fp, "\t<books reopen=\"%d\">\n", app->reopen);

  for (u8 i = 0; i < app->bookcount; i++) {
    Book *book = app->books[i];
    fprintf(fp, "\t\t<book file=\"%s\" page=\"%d\"", book->GetFileName(),
            book->GetPosition() + 1);
    if (app->bookcurrent == app->books[i])
      fprintf(fp, " current=\"1\"");
    fprintf(fp, ">\n");
    std::list<u16> *bookmarks = book->GetBookmarks();
    for (std::list<u16>::iterator j = bookmarks->begin(); j != bookmarks->end();
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
}
