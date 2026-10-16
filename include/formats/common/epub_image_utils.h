#pragma once

#include <stddef.h>
#include <string>
#include <vector>
#include "formats/common/image_buffer.h"

#include "minizip/unzip.h"

namespace epub_image_utils {

typedef bool (*ZipBinaryReader)(unzFile uf, const std::string &path,
                                std::vector<unsigned char> *out,
                                size_t max_bytes, void *ctx);
typedef bool (*CheckedZipBinaryReader)(unzFile,const std::string&,
                                      image_memory::Bytes*,size_t,void*);
bool LooksLikeSvgWrapper(const std::string&,const image_memory::Bytes&);
bool ResolveSvgWrapperImage(unzFile,const std::string&,const image_memory::Bytes&,
                            image_memory::Bytes*,size_t,std::string* = NULL,
                            CheckedZipBinaryReader = NULL,void* = NULL);
bool ReadZipEntryBinary(unzFile,const std::string&,image_memory::Bytes*,size_t);
bool DecodeDataUriImage(const std::string&,image_memory::Bytes*,size_t);

bool LooksLikeSvgWrapper(const std::string &path_hint,
                         const std::vector<unsigned char> &buf);
bool DecodeDataUriImage(const std::string &href,
                        std::vector<unsigned char> *out, size_t max_bytes);
bool ReadZipEntryBinary(unzFile uf, const std::string &path,
                        std::vector<unsigned char> *out, size_t max_bytes);
bool ResolveSvgWrapperImage(unzFile uf, const std::string &svg_path,
                            const std::vector<unsigned char> &svg_buf,
                            std::vector<unsigned char> *out,
                            size_t max_bytes,
                            std::string *resolved_path = NULL,
                            ZipBinaryReader reader = NULL,
                            void *reader_ctx = NULL);

} // namespace epub_image_utils
