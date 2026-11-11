/*
    3dslibris - mobi_text_decode.h

    MOBI byte decoding and embedded-title extraction helpers.
*/

#pragma once

#include <3ds/types.h>

#include <string>

#include "formats/mobi/mobi_parser_core.h"

class Book;

namespace mobi_text_decode {

class Session {
public:
  Session();
  ~Session();
  bool Begin(const std::string *source, u32 encoding, std::string *output);
  bool Step(size_t bytes = 32768);
  size_t Progress() const;
  bool NoMemory() const;
  size_t Total() const;
  bool UsedUtf8Guess() const;
  bool UsedLegacyGuess() const;
private:
  struct State;
  State *state_;
  Session(const Session &);
  Session &operator=(const Session &);
};

std::string DecodeBytesToUtf8(const std::string &in, u32 encoding,
                              bool *used_utf8_guess,
                              bool *used_legacy_guess);

void ApplyEmbeddedTitle(Book *book, const std::string &raw,
                        const mobi_parser_core::MobiHeaderInfo &header);
// Same, from the record 0 payload alone.
void ApplyEmbeddedTitleFromRecord0(
    Book *book, const u8 *rec0, size_t rec0_len,
    const mobi_parser_core::MobiHeaderInfo &header);

} // namespace mobi_text_decode
