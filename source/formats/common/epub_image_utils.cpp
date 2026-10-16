#include "formats/common/epub_image_utils.h"

#include "formats/common/zip_read_utils.h"
#include "shared/base64_utils.h"
#include "shared/path_utils.h"
#include "shared/string_utils.h"
#include "minizip/unzip.h"

#include <algorithm>

namespace epub_image_utils {

namespace {

template <typename Bytes>
static bool DefaultZipBinaryReader(unzFile uf, const std::string &path,
                                   Bytes *out,
                                   size_t max_bytes, void *) {
  return ReadZipEntryBinary(uf, path, out, max_bytes);
}

} // namespace

template <typename Bytes>
bool LooksLikeSvgWrapperImpl(const std::string &path_hint,const Bytes &buf) {
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
  return zip_read_utils::ReadEntryBinary(uf, path, out, max_bytes, NULL);
}

template <typename Bytes,typename Reader>
bool ResolveSvgWrapperImageImpl(unzFile uf, const std::string &svg_path,
                            const Bytes &svg_buf,Bytes *out,
                            size_t max_bytes,
                            std::string *resolved_path,
                            Reader reader, void *reader_ctx) {
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
      reader = DefaultZipBinaryReader<Bytes>;
    if (reader(uf, resolved, out, max_bytes, reader_ctx)) {
      if (resolved_path)
        *resolved_path = resolved;
      return true;
    }
  }

  out->clear();
  return false;
}

bool LooksLikeSvgWrapper(const std::string &path,const std::vector<unsigned char> &buf) {
  return LooksLikeSvgWrapperImpl(path,buf);
}
bool LooksLikeSvgWrapper(const std::string &path,const image_memory::Bytes &buf) {
  return LooksLikeSvgWrapperImpl(path,buf);
}
bool ResolveSvgWrapperImage(unzFile uf,const std::string &path,
    const std::vector<unsigned char> &input,std::vector<unsigned char> *out,
    size_t limit,std::string *resolved,ZipBinaryReader reader,void *ctx) {
  return ResolveSvgWrapperImageImpl(uf,path,input,out,limit,resolved,reader,ctx);
}
bool ResolveSvgWrapperImage(unzFile uf,const std::string &path,
    const image_memory::Bytes &input,image_memory::Bytes *out,size_t limit,
    std::string *resolved,CheckedZipBinaryReader reader,void *ctx) {
  return ResolveSvgWrapperImageImpl(uf,path,input,out,limit,resolved,reader,ctx);
}
bool ReadZipEntryBinary(unzFile uf,const std::string &path,
                        image_memory::Bytes *out,size_t limit) {
  if(!uf || !out) return false;
  out->clear();
  if(unzLocateFile(uf,path.c_str(),2)!=UNZ_OK || unzOpenCurrentFile(uf)!=UNZ_OK) return false;
  unz_file_info info;
  bool ok=unzGetCurrentFileInfo(uf,&info,NULL,0,NULL,0,NULL,0)==UNZ_OK &&
      info.uncompressed_size>0 && info.uncompressed_size<=limit &&
      out->resize(info.uncompressed_size,limit);
  size_t total=0;
  while(ok && total<out->size()) {
    int n=unzReadCurrentFile(uf,out->mutable_data()+total,(unsigned)(out->size()-total));
    if(n<=0) {ok=false;break;} total+=(size_t)n;
  }
  if(unzCloseCurrentFile(uf)!=UNZ_OK) ok=false;
  if(!ok) out->clear();
  return ok;
}
bool DecodeDataUriImage(const std::string &href,image_memory::Bytes *out,size_t limit) {
  if(!out) return false;
  out->clear();
  const size_t comma=href.find(',');
  if(!StartsWithNoCase(href,"data:") || comma==std::string::npos) return false;
  const std::string meta=ToLowerAscii(href.substr(5,comma-5));
  if(meta.find("image/")==std::string::npos || meta.find(";base64")==std::string::npos) return false;
  size_t digits=0;
  for(size_t i=comma+1;i<href.size() && href[i]!='=';++i) {
    const unsigned char c=href[i];
    if(c==' ' || c=='\t' || c=='\r' || c=='\n') continue;
    if(Base64Value(c)<0) return false;
    ++digits;
  }
  const size_t size=(digits/4)*3+(digits%4)*3/4;
  if(!size || !out->resize(size,limit)) return false;
  uint32_t accum=0;int bits=-8;size_t written=0;
  for(size_t i=comma+1;i<href.size() && href[i]!='=';++i) {
    int v=Base64Value((unsigned char)href[i]);if(v<0) continue;
    accum=(accum<<6)|(unsigned)v;bits+=6;
    if(bits>=0) {out->mutable_data()[written++]=(unsigned char)(accum>>bits);bits-=8;}
  }
  return written==size;
}
} // namespace epub_image_utils
