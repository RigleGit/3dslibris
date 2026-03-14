#pragma once

#include <stddef.h>
#include <stdio.h>

#include <string>

namespace file_read_utils {

typedef size_t (*ReadChunkFn)(void *ctx, char *buf, size_t cap);
typedef bool (*StreamFlagFn)(void *ctx);

bool ReadAllToString(ReadChunkFn read_chunk, StreamFlagFn is_eof,
                     StreamFlagFn has_error, void *ctx, std::string *out,
                     size_t max_bytes, size_t chunk_size);

bool ReadFileToStringLimited(FILE *fp, std::string *out, size_t max_bytes);
bool ReadPathToStringLimited(const char *path, std::string *out,
                             size_t max_bytes);
bool ReadPathToStringLimited(const std::string &path, std::string *out,
                             size_t max_bytes);

} // namespace file_read_utils
