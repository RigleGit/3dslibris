#include "formats/common/epub_image_utils.h"

#include "base64_utils.h"
#include "path_utils.h"
#include "string_utils.h"
#include "unzip.h"

#include <algorithm>
#include <limits.h>

namespace epub_image_utils {

namespace {

static bool DefaultZipBinaryReader(unzFile uf, const std::string &path,
                                   std::vector<unsigned char> *out,
                                   size_t max_bytes, void *) {
  return ReadZipEntryBinary(uf, path, out, max_bytes);
}

} // namespace

bool StartsWithNoCase(const std::string &s, const char *prefix) {
  if (!prefix)
    return false;
  size_t len = strlen(prefix);
  if (s.size() < len)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char a = (unsigned char)s[i];
    unsigned char b = (unsigned char)prefix[i];
    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = (unsigned char)(b - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

bool LooksLikeSvgWrapper(const std::string &path_hint,
                         const std::vector<unsigned char> &buf) {
  std::string lower_path = ToLowerAscii(path_hint);
  if (lower_path.size() >= 4 &&
      lower_path.rfind(".svg") == lower_path.size() - 4) {
    return true;
  }
  if (buf.empty())
    return false;
  size_t sample = std::min((size_t)512, buf.size());
  std::string head((const char *)buf.data(), sample);
  return ToLowerAscii(head).find("<svg") != std::string::npos;
}

bool DecodeDataUriImage(const std::string &href,
                        std::vector<unsigned char> *out, size_t max_bytes) {
  if (!out || !StartsWithNoCase(href, "data:"))
    return false;
  size_t comma = href.find(',');
  if (comma == std::string::npos || comma <= 5 || comma + 1 >= href.size())
    return false;

  std::string meta = ToLowerAscii(href.substr(5, comma - 5));
  if (meta.find("image/") == std::string::npos ||
      meta.find(";base64") == std::string::npos) {
    return false;
  }

  return DecodeBase64Bytes(href.substr(comma + 1), out, max_bytes);
}

bool ReadZipEntryBinary(unzFile uf, const std::string &path,
                        std::vector<unsigned char> *out, size_t max_bytes) {
  if (!out || !uf || path.empty() || max_bytes == 0)
    return false;
  out->clear();

  if (unzLocateFile(uf, path.c_str(), 2) != UNZ_OK)
    return false;

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK) {
    if (fi.uncompressed_size == 0 || fi.uncompressed_size > max_bytes ||
        fi.uncompressed_size > (uLong)INT_MAX) {
      return false;
    }
    out->reserve((size_t)fi.uncompressed_size);
  }

  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  unsigned char buf[8 * 1024];
  int n = 0;
  size_t total = 0;
  while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
    if (total + (size_t)n > max_bytes) {
      unzCloseCurrentFile(uf);
      out->clear();
      return false;
    }
    out->insert(out->end(), buf, buf + n);
    total += (size_t)n;
  }

  unzCloseCurrentFile(uf);
  if (n < 0 || out->empty()) {
    out->clear();
    return false;
  }
  return true;
}

bool ResolveSvgWrapperImage(unzFile uf, const std::string &svg_path,
                            const std::vector<unsigned char> &svg_buf,
                            std::vector<unsigned char> *out,
                            size_t max_bytes,
                            std::string *resolved_path,
                            ZipBinaryReader reader, void *reader_ctx) {
  if (!out || !uf || svg_buf.empty() || svg_buf.size() > max_bytes) {
    if (resolved_path)
      resolved_path->clear();
    return false;
  }
  out->clear();
  if (resolved_path)
    resolved_path->clear();

  std::string xml((const char *)svg_buf.data(), svg_buf.size());
  size_t pos = 0;
  while (pos < xml.size()) {
    size_t lt = xml.find('<', pos);
    if (lt == std::string::npos)
      break;
    size_t gt = xml.find('>', lt + 1);
    if (gt == std::string::npos)
      break;
    pos = gt + 1;

    std::string tag = Trim(xml.substr(lt + 1, gt - lt - 1));
    if (tag.empty() || tag[0] == '/' || tag[0] == '!' || tag[0] == '?')
      continue;

    size_t name_end = 0;
    while (name_end < tag.size() &&
           IsHtmlNameChar((unsigned char)tag[name_end]))
      name_end++;
    std::string name = ToLowerAscii(tag.substr(0, name_end));
    if (name != "image" && name != "img")
      continue;

    std::string href = ExtractHtmlAttrValue(tag, "href");
    if (href.empty())
      href = ExtractHtmlAttrValue(tag, "xlink:href");
    if (href.empty())
      href = ExtractHtmlAttrValue(tag, "src");
    href = Trim(href);
    if (href.empty())
      continue;

    if (StartsWithNoCase(href, "data:")) {
      if (DecodeDataUriImage(href, out, max_bytes)) {
        if (resolved_path)
          *resolved_path = "data:image";
        return true;
      }
      continue;
    }

    std::string resolved =
        StripFragmentAndQuery(ResolveRelativePath(svg_path, href));
    if (resolved.empty() || resolved == svg_path)
      continue;
    if (!reader)
      reader = DefaultZipBinaryReader;
    if (reader(uf, resolved, out, max_bytes, reader_ctx)) {
      if (resolved_path)
        *resolved_path = resolved;
      return true;
    }
  }

  out->clear();
  return false;
}

} // namespace epub_image_utils
