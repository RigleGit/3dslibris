/*

dslibris - an ebook reader for the Nintendo DS.

 Copyright (C) 2007-2008 Ray Haleblian

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

#include "epub.h"

#include "book.h"
#include "expat.h"
#include "main.h"
#include "parse.h"
#include "stb_image.h"
#include "unzip.h"
#include "zlib.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <vector>

void epub_data_delete(epub_data_t *d);

void epub_data_init(epub_data_t *d) {
  // Reset any leftover heap objects from previous parses.
  epub_data_delete(d);

  d->type = PARSE_CONTAINER;
  d->ctx.push_back(new std::string("TOP"));
  d->rootfile = "";
  d->title = "";
  d->creator = "";
  d->coverid = "";
  d->metadataonly = false;
  d->book = nullptr;
}

void epub_data_delete(epub_data_t *d) {
  for (auto *item : d->manifest)
    delete item;
  d->manifest.clear();

  for (auto *itemref : d->spine)
    delete itemref;
  d->spine.clear();

  for (auto *ctx : d->ctx)
    delete ctx;
  d->ctx.clear();
}

void epub_container_start(void *data, const char *el, const char **attr) {
  epub_data_t *d = (epub_data_t *)data;
  if (!strcmp(el, "rootfile"))
    for (int i = 0; attr[i]; i += 2)
      if (!strcmp(attr[i], "full-path"))
        d->rootfile = attr[i + 1];
}

void epub_rootfile_start(void *data, const char *el, const char **attr) {
  epub_data_t *d = (epub_data_t *)data;
  std::string elem = el;
  if (d->ctx.empty())
    return;
  std::string *ctx = d->ctx.back();
  if (!ctx)
    return;

  if ((*ctx == "manifest" || *ctx == "opf:manifest") &&
      (elem == "item" || elem == "opf:item")) {
    epub_item *item = new epub_item;
    d->manifest.push_back(item);
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "id"))
        item->id = attr[i + 1];
      if (!strcmp(attr[i], "href"))
        item->href = attr[i + 1];
      if (!strcmp(attr[i], "media-type"))
        item->media_type = attr[i + 1];
    }
  }

  else if ((*ctx == "spine" || *ctx == "opf:spine") &&
           (elem == "itemref" || elem == "opf:itemref")) {
    epub_itemref *itemref = new epub_itemref;
    d->spine.push_back(itemref);
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "idref"))
        itemref->idref = attr[i + 1];
    }
  }

  else if (elem == "dc:title") {
    d->title.clear();
  }

  else if (elem == "dc:creator") {
    d->creator.clear();
  }

  // Capture <meta name="cover" content="cover-image-id"/>
  else if (elem == "meta" || elem == "opf:meta") {
    std::string name_val, content_val;
    for (int i = 0; attr[i]; i += 2) {
      if (!strcmp(attr[i], "name"))
        name_val = attr[i + 1];
      if (!strcmp(attr[i], "content"))
        content_val = attr[i + 1];
    }
    if (name_val == "cover" && content_val.length()) {
      d->coverid = content_val;
    }
  }

  // Also check for <item properties="cover-image"> (EPUB3)
  else if ((*ctx == "manifest" || *ctx == "opf:manifest") &&
           (elem == "item" || elem == "opf:item")) {
    // Already handled above, but check properties
    // We need to re-check the last added item
    if (!d->manifest.empty()) {
      epub_item *last = d->manifest.back();
      for (int i = 0; attr[i]; i += 2) {
        if (!strcmp(attr[i], "properties") &&
            !strcmp(attr[i + 1], "cover-image")) {
          d->coverid = last->id;
        }
      }
    }
  }

  d->ctx.push_back(new std::string(elem));
}

void epub_rootfile_end(void *data, const char *el) {
  epub_data_t *d = (epub_data_t *)data;
  if (d->ctx.empty())
    return;
  delete d->ctx.back();
  d->ctx.pop_back();
}

void epub_rootfile_char(void *data, const XML_Char *txt, int len) {
  epub_data_t *d = (epub_data_t *)data;
  if (d->ctx.empty())
    return;
  std::string *ctx = d->ctx.back();
  if (!ctx)
    return;

  if (*ctx == "dc:title") {
    d->title.append((char *)txt, len);
  } else if (*ctx == "dc:creator") {
    d->creator.append((char *)txt, len);
  }
}

int epub_parse_currentfile(unzFile uf, epub_data_t *epd) {
  int rc = 0;
  parsedata_t pd;
  char *filebuf = new char[BUFSIZE];
  XML_Parser p = XML_ParserCreate(NULL);
  if (epd->type == PARSE_CONTAINER) {
    XML_SetUserData(p, epd);
    XML_SetElementHandler(p, epub_container_start, NULL);
  } else if (epd->type == PARSE_ROOTFILE) {
    XML_SetUserData(p, epd);
    XML_SetElementHandler(p, epub_rootfile_start, epub_rootfile_end);
    XML_SetCharacterDataHandler(p, epub_rootfile_char);
  } else if (epd->type == PARSE_CONTENT) {
    parse_init(&pd);
    pd.book = epd->book;
    pd.app = pd.book->GetApp();
    pd.ts = pd.app->ts;
    pd.prefs = pd.app->prefs;
    XML_SetUserData(p, &pd);
    XML_SetElementHandler(p, xml::book::start, xml::book::end);
    XML_SetCharacterDataHandler(p, xml::book::chardata);
    XML_SetDefaultHandler(p, xml::book::fallback);
    XML_SetProcessingInstructionHandler(p, xml::book::instruction);
  } else
    return 0;

  int len = 0;
  size_t len_total = 0;
  enum XML_Status status;
  do {
    len = unzReadCurrentFile(uf, filebuf, BUFSIZE);
    if (len < 0) {
      rc = len;
      break;
    }
    status = XML_Parse(p, filebuf, len, len == 0);
    if (status == XML_STATUS_ERROR) {
      rc = status;
      break;
    }
    len_total += (size_t)len;
  } while (len);

  XML_ParserFree(p);
  delete[] filebuf;
  return (rc);
}

int epub(Book *book, std::string name, bool metadataonly) {
  //! Parse EPUB file.
  //! Set metadataonly to true if you only want the title and author.
  int rc = 0;
  static epub_data_t parsedata;

  // Parse top-level container XML for the rootfile.

  unzFile uf = unzOpen(name.c_str());
  if (!uf)
    return 1;
  rc = unzLocateFile(uf, "META-INF/container.xml", 2); // 2 = case insensitive
  if (rc != UNZ_OK) {
    unzClose(uf);
    return 2;
  }
  rc = unzOpenCurrentFile(uf);
  epub_data_init(&parsedata);
  parsedata.book = book;
  parsedata.type = PARSE_CONTAINER;
  rc = epub_parse_currentfile(uf, &parsedata);
  rc = unzCloseCurrentFile(uf);

  // Extract any leading path for the rootfile.
  // The manifest in the rootfile will list filenames
  // relative to the rootfile location.
  std::string folder = "";
  size_t pos =
      parsedata.rootfile.find_last_of("/", parsedata.rootfile.length());
  if (pos < parsedata.rootfile.length()) {
    folder = parsedata.rootfile.substr(0, pos);
  }

  rc = unzLocateFile(uf, parsedata.rootfile.c_str(), 0);
  if (rc == UNZ_OK) {
    rc = unzOpenCurrentFile(uf);
    epub_data_init(&parsedata);
    parsedata.book = book;
    parsedata.type = PARSE_ROOTFILE;
    epub_parse_currentfile(uf, &parsedata);
    rc = unzCloseCurrentFile(uf);
  }

  // Stop here if only metadata is required.
  if (metadataonly) {
    if (parsedata.title.length()) {
      book->SetTitle(parsedata.title.c_str());
      if (parsedata.creator.length())
        book->SetAuthor(parsedata.creator);
    }
    // Find cover image path from manifest
    if (parsedata.coverid.length()) {
      for (auto item : parsedata.manifest) {
        if (item->id == parsedata.coverid) {
          std::string coverpath = folder;
          if (coverpath.length())
            coverpath += "/";
          coverpath += item->href;
          book->coverImagePath = coverpath;
          break;
        }
      }
    }
    unzClose(uf);
    epub_data_delete(&parsedata);
    return rc;
  }

  // Read the XHTML in the manifest, ordering by spine if needed.
  parsedata.ctx.clear();
  parsedata.book = book;
  parsedata.type = PARSE_CONTENT;
  std::vector<std::string *> href;
  if (parsedata.spine.size()) {
    // Use spine for reading order.
    std::vector<epub_itemref *>::iterator itemref;
    for (itemref = parsedata.spine.begin(); itemref != parsedata.spine.end();
         itemref++) {
      std::vector<epub_item *>::iterator item;
      for (item = parsedata.manifest.begin(); item != parsedata.manifest.end();
           item++) {
        if ((*item)->id == (*itemref)->idref) {
          std::string *h = new std::string((*item)->href);
          href.push_back(h);
        }
      }
    }
  } else {
    std::vector<epub_item *>::iterator item;
    for (item = parsedata.manifest.begin(); item != parsedata.manifest.end();
         item++)
      href.push_back(new std::string((*item)->href));
  }

  std::vector<std::string *>::iterator it;
  for (it = href.begin(); it != href.end(); it++) {
    size_t pos = (*it)->find_last_of('.');
    if (pos < (*it)->length()) {
      std::string ext = (*it)->substr(pos);
      std::string path = folder;
      if (path.length())
        path += "/";
      path += (*it)->c_str();

      // Simple URL decode (e.g. %20 -> space)
      std::string decoded_path;
      for (size_t k = 0; k < path.length(); k++) {
        if (path[k] == '%' && k + 2 < path.length()) {
          int value;
          sscanf(path.substr(k + 1, 2).c_str(), "%x", &value);
          decoded_path += static_cast<char>(value);
          k += 2;
        } else {
          decoded_path += path[k];
        }
      }
      path = decoded_path;

      rc = unzLocateFile(uf, path.c_str(), 2); // 2 = case insensitive
      if (rc == UNZ_OK) {
        rc = unzOpenCurrentFile(uf);
        epub_parse_currentfile(uf, &parsedata);
        rc = unzCloseCurrentFile(uf);
      } else {
        char msg[256];
        sprintf(msg, "NOT FOUND IN ZIP: %s", path.c_str());
        book->GetApp()->PrintStatus(msg);
      }
    }
  }
  unzClose(uf);
  epub_data_delete(&parsedata);
  return rc;
}

int epub_extract_cover(Book *book, const std::string &epubpath) {
  if (book->coverImagePath.empty())
    return 1;

  unzFile uf = unzOpen(epubpath.c_str());
  if (!uf)
    return 2;

  int rc = unzLocateFile(uf, book->coverImagePath.c_str(), 2);
  if (rc != UNZ_OK) {
    unzClose(uf);
    return 3;
  }

  unz_file_info fi;
  unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0);

  // Safety: limit uncompressed cover to 2 MB to avoid OOM on 3DS
  if (fi.uncompressed_size > 2 * 1024 * 1024) {
    unzClose(uf);
    return 8;
  }

  u8 *imgbuf = new u8[fi.uncompressed_size];
  unzOpenCurrentFile(uf);
  unzReadCurrentFile(uf, imgbuf, fi.uncompressed_size);
  unzCloseCurrentFile(uf);
  unzClose(uf);

  // Decode image using stb_image (supports JPEG + PNG)
  int imgW, imgH, channels;
  unsigned char *pixels = stbi_load_from_memory(
      imgbuf, fi.uncompressed_size, &imgW, &imgH, &channels, 3); // Force RGB
  delete[] imgbuf;

  if (!pixels)
    return 4; // Failed to decode image

  // Safety: skip images that are too large for 3DS RAM
  if (imgW > 2048 || imgH > 2048) {
    stbi_image_free(pixels);
    return 7;
  }

  // Scale to portrait thumbnail (85x115 to fit inside 89x119 button)
  int thumbW = 85;
  int thumbH = 115;
  float scaleX = (float)imgW / thumbW;
  float scaleY = (float)imgH / thumbH;
  float scale = (scaleX > scaleY) ? scaleX : scaleY;
  int finalW = (int)(imgW / scale);
  int finalH = (int)(imgH / scale);
  if (finalW > thumbW)
    finalW = thumbW;
  if (finalH > thumbH)
    finalH = thumbH;

  book->coverPixels = new u16[finalW * finalH];
  book->coverWidth = finalW;
  book->coverHeight = finalH;

  for (int y = 0; y < finalH; y++) {
    int srcY = (int)(y * scale);
    if (srcY >= imgH)
      srcY = imgH - 1;
    for (int x = 0; x < finalW; x++) {
      int srcX = (int)(x * scale);
      if (srcX >= imgW)
        srcX = imgW - 1;
      unsigned char *px = &pixels[(srcY * imgW + srcX) * 3];
      // Convert RGB to RGB565
      u16 r = (px[0] >> 3) & 0x1F;
      u16 g = (px[1] >> 2) & 0x3F;
      u16 b = (px[2] >> 3) & 0x1F;
      book->coverPixels[y * finalW + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);
  return 0;
}
