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

enum DecodeMode { Copy, Mixed, Legacy, Utf16 };

// Totals the decoder needs to pick a mode. Independent of how the input is
// split into feeds.
struct ScanScore {
  size_t bytes = 0, invalid = 0, legacy_score = 0, mixed_score = 0;
  // Output sizes the Mixed and Legacy conversions would produce.
  size_t mixed_size = 0, legacy_size = 0;
};

// Feedable scan scorer. Feed() accepts the input in any split, including in
// the middle of a UTF-8 sequence: an unfinished trailing sequence waits for
// the next feed. The last feed must set `final`.
class Scorer {
public:
  Scorer();
  void Feed(const char *data, size_t n, bool final);
  const ScanScore &Score() const { return score_; }
private:
  ScanScore score_;
  std::string carry_, legacy_tail_, mixed_tail_;
};

struct ModeChoice {
  DecodeMode mode;
  bool guessed, legacy_guess;
};
// Mode for a non-UTF-16 encoding from the finished scan.
ModeChoice ChooseMode(u32 encoding, const ScanScore &score);
bool IsUtf16Encoding(u32 encoding);

// Feedable converter to UTF-8 for a chosen mode; same output as converting the
// whole input at once, for any split of the feeds.
class Converter {
public:
  explicit Converter(DecodeMode mode);
  void Feed(const char *data, size_t n, bool final, std::string *out);
private:
  DecodeMode mode_;
  bool started_, little_;
  std::string carry_;
};

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
