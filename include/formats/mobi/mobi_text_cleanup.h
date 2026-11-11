/*
    3dslibris - mobi_text_cleanup.h

    Summary:
    - Conservative text cleanup helpers for malformed MOBI prose.
    - Used only for per-book repair of hard-wrapped line breaks.
*/

#pragma once

#include <string>
#include <vector>

namespace mobi_text_cleanup {

void RepairCommonMojibakeInPlace(std::string *text);
std::string RepairCommonMojibake(const std::string &text);
std::string RepairCommonMojibakePreservingMobiImageTokens(
    const std::string &text);
std::string FixBrokenParagraphWraps(const std::string &text);
std::string FixBrokenParagraphWrapsPreservingMobiImageTokens(
    const std::string &text);

// Streaming form of the cleanup CleanupDecodedMobiText applies to a whole
// text: newline normalization, then optionally the mojibake repair and the
// paragraph wrap fix, either plain or preserving inline image tokens
// (token_aware). Input may be split anywhere; the output equals the whole-text
// result. Image token ids are collected at the repair input (after the newline
// normalization) and at the output, for the caller's rollback check.
struct StreamOptions {
  bool mojibake, wrap_fix, token_aware;
  StreamOptions() : mojibake(false), wrap_fix(false), token_aware(false) {}
};

class CleanupStream {
public:
  explicit CleanupStream(const StreamOptions &options);
  ~CleanupStream();
  // The last feed must set `final`. Appends the produced bytes to *out.
  void Feed(const char *data, size_t n, bool final, std::string *out);
  const std::vector<unsigned short> &IdsBefore() const;
  const std::vector<unsigned short> &IdsAfter() const;

private:
  struct State;
  State *state_;
  CleanupStream(const CleanupStream &);
  CleanupStream &operator=(const CleanupStream &);
};

// Finds MOBI inline image tokens in a stream split anywhere; ids come out in
// order, as CollectMobiInlineImageTokenIds yields them for the whole text.
class TokenScan {
public:
  TokenScan();
  void Feed(const char *data, size_t n, bool final);
  const std::vector<unsigned short> &Ids() const { return ids_; }

private:
  std::string carry_;
  std::vector<unsigned short> ids_;
};

} // namespace mobi_text_cleanup
