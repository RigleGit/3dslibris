/*
    3dslibris - epub_cover.cpp
    EPUB cover image extraction and decoding.
    Extracted from epub.cpp by Rigle.
*/

#include "formats/epub/epub_cover.h"

#include "book/book.h"
#include "debug_log.h"
#include "formats/common/epub_image_utils.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_limits.h"
#include "minizip/unzip.h"
#include "path_utils.h"
#include "stb_image.h"
#include "shared/string_utils.h"
#include <3ds.h>
#include <algorithm>
#include <ctype.h>
#include <vector>

namespace {

static std::string BuildDocPath(const std::string &opf_folder,
                                const std::string &href) {
  if (opf_folder.empty())
    return NormalizePath(UrlDecode(href));
  return NormalizePath(opf_folder + "/" + UrlDecode(href));
}

static bool FindManifestItemPath(epub_data_t &data, const std::string &id,
                                 const std::string &opf_folder,
                                 std::string &path_out) {
  for (auto item : data.manifest) {
    if (item->id == id) {
      path_out = BuildDocPath(opf_folder, item->href);
      return true;
    }
  }
  return false;
}

static bool LocateZipEntrySafe(unzFile uf, const std::string &entry_path) {
  if (!uf || entry_path.empty())
    return false;

  if (unzLocateFile(uf, entry_path.c_str(), 0) == UNZ_OK)
    return true;

  std::string wanted = NormalizeZipEntryName(entry_path);
  int rc = unzGoToFirstFile(uf);
  if (rc != UNZ_OK)
    return false;

  do {
    unz_file_info fi;
    char fname[1024];
    int info_rc =
        unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL, 0);
    if (info_rc == UNZ_OK) {
      std::string current = NormalizeZipEntryName(std::string(fname));
      if (current == wanted || EqualsAsciiNoCase(current, wanted))
        return true;
    }
    rc = unzGoToNextFile(uf);
  } while (rc == UNZ_OK);

  return false;
}

static bool ResolveSvgCoverPayload(unzFile uf, const std::string &svg_path,
                                   const std::vector<u8> &svg_buf,
                                   std::vector<u8> *out,
                                   std::string *resolved_path) {
  if (!out || svg_buf.empty() ||
      svg_buf.size() > epub_limits::kSvgWrapperMaxBytes)
    return false;
  return epub_image_utils::ResolveSvgWrapperImage(
      uf, svg_path, svg_buf, out, epub_limits::kCoverMaxEntryBytes,
      resolved_path);
}

} // namespace

namespace epub_cover {

int Extract(Book *book, const std::string &epubpath) {
  if (!book || book->coverImagePath.empty())
    return 1;

  unzFile uf = unzOpen(epubpath.c_str());
  if (!uf)
    return 2;

  if (!LocateZipEntrySafe(uf, book->coverImagePath)) {
    unzClose(uf);
    return 3;
  }

  unz_file_info fi;
  unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0);

  if (fi.uncompressed_size == 0 ||
      fi.uncompressed_size > epub_limits::kCoverMaxEntryBytes ||
      fi.uncompressed_size > (uLong)INT_MAX) {
    unzClose(uf);
    return 8;
  }

  std::vector<u8> imgbuf((size_t)fi.uncompressed_size);
  int rc = unzOpenCurrentFile(uf);
  if (rc != UNZ_OK) {
    unzClose(uf);
    return 5;
  }

  int total = 0;
  while (total < (int)imgbuf.size()) {
    int n = unzReadCurrentFile(uf, imgbuf.data() + total,
                               (unsigned int)(imgbuf.size() - (size_t)total));
    if (n < 0) {
      unzCloseCurrentFile(uf);
      unzClose(uf);
      return 5;
    }
    if (n == 0)
      break;
    total += n;
  }
  unzCloseCurrentFile(uf);
  if (total <= 0) {
    unzClose(uf);
    return 5;
  }
  imgbuf.resize((size_t)total);

  std::vector<u8> decodebuf = imgbuf;
  std::string decode_path = book->coverImagePath;
  if (epub_image_utils::LooksLikeSvgWrapper(decode_path, decodebuf)) {
    std::vector<u8> resolved;
    std::string resolved_path;
    if (ResolveSvgCoverPayload(uf, decode_path, decodebuf, &resolved,
                               &resolved_path)) {
      decodebuf.swap(resolved);
      if (!resolved_path.empty())
        decode_path = resolved_path;
    }
  }
  unzClose(uf);

  auto IsJpegCover = [&]() -> bool {
    if (decode_path.size() >= 4) {
      std::string lower = decode_path;
      for (size_t i = 0; i < lower.size(); i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
      if (lower.rfind(".jpg") == lower.size() - 4 ||
          lower.rfind(".jpeg") == lower.size() - 5)
        return true;
    }
    return decodebuf.size() >= 3 && decodebuf[0] == 0xFF &&
           decodebuf[1] == 0xD8 && decodebuf[2] == 0xFF;
  };

  if (decodebuf.size() > epub_limits::kCoverMaxNonJpegBytes && !IsJpegCover())
    return 8;

  int infoW = 0, infoH = 0, infoChannels = 0;
  bool hasInfo = stbi_info_from_memory(decodebuf.data(), (int)decodebuf.size(),
                                       &infoW, &infoH, &infoChannels) != 0;
  if (!hasInfo && decodebuf.size() > epub_limits::kCoverMaxNonJpegBytes)
    return 9;

  if (hasInfo) {
    if (infoW <= 0 || infoH <= 0 || infoW > epub_limits::kCoverMaxDimension ||
        infoH > epub_limits::kCoverMaxDimension)
      return 7;
    size_t decoded_bytes = (size_t)infoW * (size_t)infoH * 3;
    if (decoded_bytes > epub_limits::kCoverMaxDecodedRgbBytes)
      return 9;
  }

  int imgW, imgH, channels;
  unsigned char *pixels =
      stbi_load_from_memory(decodebuf.data(), (int)decodebuf.size(), &imgW,
                            &imgH, &channels, 3);

  if (!pixels)
    return 4;

  if (imgW <= 0 || imgH <= 0 || imgW > epub_limits::kCoverMaxDimension ||
      imgH > epub_limits::kCoverMaxDimension) {
    stbi_image_free(pixels);
    return 7;
  }

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

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }
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
      u16 r = (px[0] >> 3) & 0x1F;
      u16 g = (px[1] >> 2) & 0x3F;
      u16 b = (px[2] >> 3) & 0x1F;
      book->coverPixels[y * finalW + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);
  return 0;
}

bool FindLikelyImagePath(epub_data_t &data, const std::string &opf_folder,
                         std::string &path_out) {
  if (!data.coverid.empty()) {
    for (auto item : data.manifest) {
      if (item && item->id == data.coverid) {
        if (item->media_type.find("image/") == 0) {
          path_out = NormalizePath(opf_folder + "/" + UrlDecode(item->href));
          return true;
        }
        break;
      }
    }
  }

  for (auto item : data.manifest) {
    if (!item || item->media_type.find("image/") != 0)
      continue;
    if (ContainsNoCase(item->id, "cover") ||
        ContainsNoCase(item->href, "cover") ||
        ContainsNoCase(item->href, "portada") ||
        ContainsNoCase(item->properties, "cover")) {
      path_out = NormalizePath(opf_folder + "/" + UrlDecode(item->href));
      return true;
    }
  }

  for (auto item : data.manifest) {
    if (!item || item->media_type.find("image/") != 0)
      continue;
    path_out = NormalizePath(opf_folder + "/" + UrlDecode(item->href));
    return true;
  }

  path_out.clear();
  return false;
}

} // namespace epub_cover
