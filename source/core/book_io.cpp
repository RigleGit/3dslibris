#include "book.h"

#include "epub.h"
#include "main.h"
#include "parse.h"
#include <stdio.h>
#include <sys/param.h>

void Book::Cache() {
  FILE *fp = fopen("/cache.dat", "w");
  if (!fp)
    return;

  int buflen = 0;
  int pagecount = GetPageCount();
  fprintf(fp, "%d\n", pagecount);
  for (int i = 0; i < pagecount; i++) {
    buflen += GetPage(i)->GetLength();
    fprintf(fp, "%d\n", buflen);
    GetPage(i)->Cache(fp);
  }
  fclose(fp);
}

u8 Book::Open() {
  std::string path;
  path.append(GetFolderName());
  path.append("/");
  path.append(GetFileName());

  char logmsg[256];
  sprintf(logmsg, "Opening: %s", path.c_str());
  app->PrintStatus(logmsg);

  // Page layout is a function of the current style.
  app->ts->SetStyle(TEXT_STYLE_REGULAR);
  u8 err = 1;
  if (format == FORMAT_EPUB) {
    err = epub(this, path, false);
  } else {
    err = Parse(true);
  }
  if (!err)
    if (position > (int)pages.size())
      position = pages.size() - 1;
  return err;
}

u8 Book::Index() {
  if (metadataIndexTried)
    return metadataIndexed ? 0 : 1;
  metadataIndexTried = true;

  int err = 1;
  if (format == FORMAT_EPUB) {
    std::string path;
    path.append(GetFolderName());
    path.append("/");
    path.append(GetFileName());
    err = epub(this, path, true);
  } else {
    // Non-EPUB files currently use filename labels in browser; defer full parse
    // until open to keep startup responsive.
    err = 0;
  }
  if (!err) {
    metadataIndexed = true;
  }
  return err;
}

u8 Book::Parse(bool fulltext) {
  //! Parse full text (true) or titles only (false).
  //! Expat callback handlers do the heavy work.
  u8 rc = 0;

  char *filebuf = new char[BUFSIZE];
  if (!filebuf) {
    rc = 1;
    return (rc);
  }

  char path[MAXPATHLEN];
  snprintf(path, sizeof(path), "%s/%s", GetFolderName(), GetFileName());
  FILE *fp = fopen(path, "r");
  if (!fp) {
    delete[] filebuf;
    rc = 255;
    return (rc);
  }

  parsedata_t parsedata;
  parse_init(&parsedata);
  parsedata.cachefile = fopen("/cache.dat", "w");
  parsedata.book = this;

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    delete[] filebuf;
    fclose(fp);
    rc = 253;
    return rc;
  }
  XML_ParserReset(p, NULL);
  XML_SetUserData(p, &parsedata);
  XML_SetDefaultHandler(p, xml::book::fallback);
  XML_SetProcessingInstructionHandler(p, xml::book::instruction);
  XML_SetElementHandler(p, xml::book::start, xml::book::end);
  XML_SetCharacterDataHandler(p, xml::book::chardata);
  if (!fulltext) {
    XML_SetElementHandler(p, xml::book::metadata::start,
                          xml::book::metadata::end);
    XML_SetCharacterDataHandler(p, xml::book::metadata::chardata);
  }

  enum XML_Status status;
  while (true) {
    int bytes_read = fread(filebuf, 1, BUFSIZE, fp);
    status = XML_Parse(p, filebuf, bytes_read, (bytes_read == 0));
    if (status == XML_STATUS_ERROR) {
      app->parse_error(p);
      rc = 254;
      break;
    }
    if (parsedata.status)
      break; // non-fulltext parsing signals it is done.
    if (bytes_read == 0)
      break; // assume our buffer ran out.
  }

  XML_ParserFree(p);
  fclose(fp);
  delete[] filebuf;

  return (rc);
}

void Book::Restore() {
  FILE *fp = fopen("/cache.dat", "r");
  if (!fp)
    return;

  int len, pagecount;
  u8 buf[BUFSIZE];

  fscanf(fp, "%d\n", &pagecount);
  for (int i = 0; i < pagecount - 1; i++) {
    fscanf(fp, "%d\n", &len);
    fread(buf, sizeof(char), len, fp);
    GetPage(i)->SetBuffer(buf, len);
  }
  fclose(fp);
}
