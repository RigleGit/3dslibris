#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

bool LooksLikeValidUtf8Bytes(const std::string &s);

void AppendUtf8Codepoint(std::string *out, uint32_t cp);

void AppendCp1252Byte(std::string *out, unsigned char b);

std::string DecodeLegacySingleByteToUtf8(const std::string &in);

size_t CountUtf8InvalidLeadBytes(const std::string &bytes);

std::string DecodeMostlyUtf8WithCp1252Fallback(const std::string &in,
                                               size_t *invalid_out);

std::string NormalizeTextUtf8(std::string raw);

void NormalizeNewlines(std::string *s);

std::string DecodeRtfToUtf8(const std::string &rtf);
