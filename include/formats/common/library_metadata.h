#pragma once
#include "formats/common/book_meta_cache.h"

namespace library_metadata {
enum class Status {Ready, Unsupported, Error, Cancelled};
struct Value {
  book_meta_cache::MetaEntry entry;
  bool extracted=false, cache_saved=true;
};
typedef bool (*Cancelled)(void *);
// Metadata only. Never creates Book/pages, decodes images or polls APT/input.
// A successful result certifies both fields, including an absent (empty) field.
Status Prepare(const std::string &path, Value *out,
               Cancelled cancelled=nullptr, void *context=nullptr);
Status ReadMuPdf(const std::string &path, book_meta_cache::MetaEntry *out);
}
