#include "formats/mobi/mobi_safe_markup_extract.h"
#include "formats/common/cache_memory_utils.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include "formats/common/html_entity_utils.h"
#include "formats/mobi/mobi_position_map.h"
#include "shared/text_token_constants.h"

bool mobi_extract_image_recindex(const std::string &tag,
                                 uint16_t *recindex_out);
std::string mobi_inline_image_path(uint16_t recindex);

namespace mobi_safe_markup_extract {
namespace {

static bool IsAsciiSpace(char c) {
  unsigned char uc = (unsigned char)c;
  return uc == ' ' || uc == '\t' || uc == '\n' || uc == '\r';
}

static void TrimTrailingInlineWhitespace(std::string *out) {
  if (!out)
    return;
  while (!out->empty() &&
         (out->back() == ' ' || out->back() == '\t' || out->back() == '\r'))
    out->pop_back();
}

static void AppendParagraphBreak(std::string *out) {
  if (!out)
    return;
  TrimTrailingInlineWhitespace(out);
  while (!out->empty() && out->back() == '\n')
    out->pop_back();
  if (!out->empty())
    out->append("\n\n");
}

static void AppendSingleSpace(std::string *out) {
  if (!out || out->empty())
    return;
  char tail = out->back();
  if (tail != ' ' && tail != '\n' && tail != '\t' && tail != '\r')
    out->push_back(' ');
}

static std::string LowerAscii(std::string s) {
  for (size_t i = 0; i < s.size(); i++)
    s[i] = (char)std::tolower((unsigned char)s[i]);
  return s;
}

static std::string ParseTagName(const std::string &tag_text) {
  size_t i = 0;
  while (i < tag_text.size() && IsAsciiSpace(tag_text[i]))
    i++;
  if (i < tag_text.size() && tag_text[i] == '/')
    i++;
  while (i < tag_text.size() && IsAsciiSpace(tag_text[i]))
    i++;

  size_t start = i;
  while (i < tag_text.size()) {
    unsigned char c = (unsigned char)tag_text[i];
    if (IsAsciiSpace((char)c) || c == '/' || c == '>')
      break;
    i++;
  }
  return LowerAscii(tag_text.substr(start, i - start));
}

static bool IsClosingTag(const std::string &tag_text) {
  size_t i = 0;
  while (i < tag_text.size() && IsAsciiSpace(tag_text[i]))
    i++;
  return i < tag_text.size() && tag_text[i] == '/';
}

static bool IsBlockTag(const std::string &name) {
  return name == "p" || name == "div" || name == "section" ||
         name == "article" || name == "blockquote" || name == "h1" ||
         name == "h2" || name == "h3" || name == "h4" || name == "h5" ||
         name == "h6" || name == "tr";
}

static void AppendInlineImageToken(std::string *out, uint16_t image_id,
                                   bool at_paragraph_start) {
  if (!out)
    return;
  out->push_back((char)(at_paragraph_start ? TEXT_IMAGE_LEADING_PARAGRAPH
                                           : TEXT_IMAGE_CONTEXT_DEFAULT));
  out->push_back((char)TEXT_IMAGE);
  out->push_back((char)((image_id >> 8) & 0xFF));
  out->push_back((char)(image_id & 0xFF));
}

// Extraction state shared by the whole-buffer Session and the incremental
// Stream. `out` holds the output not yet handed away; `out_base` counts the
// bytes already drained, so map offsets stay absolute.
struct Core {
  std::string *out = nullptr;
  size_t out_base = 0;
  InlineImageCallbacks callbacks;
  std::vector<std::pair<uint32_t, uint32_t>> *map = nullptr;
  size_t interval = 0, next_sample = 0;
  bool in_script = false, in_style = false, pending_space = false,
       at_paragraph_start = true;
};

void BeginCore(Core *c, std::string *output, const InlineImageCallbacks &cb,
               std::vector<std::pair<uint32_t, uint32_t>> *map,
               size_t total_markup_bytes) {
  c->out = output;
  c->callbacks = cb;
  c->map = map;
  output->clear();
  if (map) {
    c->interval =
        mobi_position_map::HtmlSampleIntervalForTextBytes(total_markup_bytes);
    c->next_sample = c->interval;
    map->clear();
    map->push_back({0u, 0u});
  }
}

// Consumes markup_utf8[i..) for at most `limit` bytes (a construct that starts
// inside the limit is finished). `abs_base` is the absolute offset of
// markup_utf8[0]. With !final, a tag or entity that is not complete inside the
// window stops the run with *need_more set; with final it behaves exactly like
// the whole-buffer scan (an unterminated tag stops without progress).
void Run(Core *c, const std::string &markup_utf8, size_t &i, size_t abs_base,
         size_t limit, bool final, bool *need_more) {
  std::string &out = *c->out;
  const InlineImageCallbacks &image_callbacks = c->callbacks;
  auto *html_to_text_map = c->map;
  const size_t kSampleInterval = c->interval;
  size_t &next_sample = c->next_sample;
  bool &in_script = c->in_script, &in_style = c->in_style,
       &pending_space = c->pending_space,
       &at_paragraph_start = c->at_paragraph_start;
  const size_t begin = i;
  *need_more = false;
  for (; i < markup_utf8.size() && i - begin < limit;) {
    if (html_to_text_map && kSampleInterval > 0 &&
        abs_base + i >= next_sample) {
      html_to_text_map->push_back(
          {(uint32_t)(abs_base + i), (uint32_t)(c->out_base + out.size())});
      next_sample = abs_base + i + kSampleInterval;
    }
    const unsigned char ch = (unsigned char)markup_utf8[i];

    if (ch == '<') {
      size_t close = markup_utf8.find('>', i + 1);
      if (close == std::string::npos) {
        *need_more = !final;
        break;
      }
      const std::string tag_text = markup_utf8.substr(i + 1, close - i - 1);
      const std::string tag_name = ParseTagName(tag_text);
      const bool closing = IsClosingTag(tag_text);

      if (tag_name == "script") {
        in_script = !closing;
      } else if (tag_name == "style") {
        in_style = !closing;
      } else if (!in_script && !in_style) {
        if (tag_name == "br") {
          TrimTrailingInlineWhitespace(&out);
          if (!out.empty() && out.back() != '\n')
            out.push_back('\n');
          pending_space = false;
        } else if (tag_name == "img" && !closing) {
          uint16_t recindex = 0;
          if (image_callbacks.register_inline_image &&
              mobi_extract_image_recindex(tag_text, &recindex)) {
            const uint16_t image_id = image_callbacks.register_inline_image(
                image_callbacks.user_data, mobi_inline_image_path(recindex));
            if (pending_space) {
              AppendSingleSpace(&out);
              pending_space = false;
            }
            AppendInlineImageToken(&out, image_id, at_paragraph_start);
            at_paragraph_start = false;
          }
        } else if (tag_name == "li" && !closing) {
          AppendParagraphBreak(&out);
          out.append("- ");
          pending_space = false;
          at_paragraph_start = false;
        } else if (IsBlockTag(tag_name)) {
          AppendParagraphBreak(&out);
          pending_space = false;
          at_paragraph_start = true;
        }
      }

      i = close + 1;
      continue;
    }

    if (in_script || in_style) {
      i++;
      continue;
    }

    if (ch == '&') {
      // An entity is a ';' within 12 bytes of the '&'.
      size_t semi = std::string::npos;
      const size_t scan_end = std::min(markup_utf8.size(), i + 13);
      for (size_t k = i + 1; k < scan_end; ++k)
        if (markup_utf8[k] == ';') {
          semi = k;
          break;
        }
      if (semi == std::string::npos && !final && markup_utf8.size() < i + 13) {
        *need_more = true;
        break;
      }
      if (semi != std::string::npos) {
        std::string decoded;
        if (html_entity_utils::DecodeHtmlEntityUtf8(
                markup_utf8.substr(i + 1, semi - i - 1), &decoded)) {
          if (pending_space) {
            AppendSingleSpace(&out);
            pending_space = false;
          }
          out += decoded;
          if (!decoded.empty() &&
              decoded.find_first_not_of(" \t\r\n") != std::string::npos) {
            at_paragraph_start = false;
          }
          i = semi + 1;
          continue;
        }
      }
    }

    if (IsAsciiSpace((char)ch)) {
      pending_space = true;
      i++;
      continue;
    }

    if (ch < 0x20) {
      i++;
      continue;
    }

    if (pending_space) {
      AppendSingleSpace(&out);
      pending_space = false;
    }

    out.push_back((char)ch);
    at_paragraph_start = false;
    i++;
  }
}

void FinishOutput(std::string *out) {
  TrimTrailingInlineWhitespace(out);
  while (!out->empty() && out->back() == '\n')
    out->pop_back();
}

} // namespace

struct Session::State {
  const std::string *source;
  Core core;
  size_t cursor = 0;
  bool done = false, no_memory = false;
};
Session::Session() : state_(nullptr) {}
Session::~Session() {
  delete state_;
}
bool Session::Begin(const std::string *source, std::string *output,
                    const InlineImageCallbacks &callbacks,
                    std::vector<std::pair<uint32_t, uint32_t>> *map) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_ || !source || !output)
    return false;
  state_->source = source;
  BeginCore(&state_->core, output, callbacks, map, source->size());
  return true;
}
size_t Session::Progress() const {
  return state_ ? state_->cursor : 0;
}
bool Session::NoMemory() const {
  return state_ && state_->no_memory;
}
bool Session::Step(size_t bytes) {
  if (!state_ || state_->done)
    return true;
  State &s = *state_;
  const std::string &markup_utf8 = *s.source;
  std::string &out = *s.core.out;
  if (s.no_memory)
    return false;
  if (!cache_memory::CanAppend(
          out, std::min(markup_utf8.size() - s.cursor, bytes + 16))) {
    s.no_memory = true;
    return false;
  }
  bool need_more = false;
  Run(&s.core, markup_utf8, s.cursor, 0, bytes, true, &need_more);
  if (s.cursor < markup_utf8.size())
    return false;
  FinishOutput(&out);
  s.done = true;
  return true;
}

struct Stream::State {
  Core core;
  std::string window, out;
  size_t abs_base = 0;
  bool no_memory = false, finished = false;
};
Stream::Stream() : state_(nullptr) {}
Stream::~Stream() {
  delete state_;
}
bool Stream::Begin(const InlineImageCallbacks &callbacks,
                   std::vector<std::pair<uint32_t, uint32_t>> *map,
                   size_t total_markup_bytes) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_)
    return false;
  BeginCore(&state_->core, &state_->out, callbacks, map, total_markup_bytes);
  return true;
}
bool Stream::NoMemory() const {
  return state_ && state_->no_memory;
}
size_t Stream::Consumed() const {
  return state_ ? state_->abs_base : 0;
}
size_t Stream::OutputBytes() const {
  return state_ ? state_->core.out_base + state_->out.size() : 0;
}
bool Stream::Feed(const char *data, size_t n, bool final) {
  if (!state_ || state_->no_memory || state_->finished)
    return false;
  State &s = *state_;
  // A tag with no end inside this much input is not markup worth waiting for.
  const size_t kMaxPending = 4u * 1024u * 1024u;
  if (!cache_memory::CanAppend(s.window, n) ||
      !cache_memory::CanAppend(s.out, n + 64)) {
    s.no_memory = true;
    return false;
  }
  s.window.append(data, n);
  size_t i = 0;
  bool need_more = false;
  Run(&s.core, s.window, i, s.abs_base, (size_t)-1, final, &need_more);
  if (need_more && s.window.size() - i > kMaxPending) {
    s.no_memory = true;
    return false;
  }
  s.abs_base += final ? s.window.size() : i;
  if (final)
    s.window.clear();
  else
    s.window.erase(0, i);
  if (final) {
    FinishOutput(&s.out);
    s.finished = true;
  }
  return true;
}
void Stream::Drain(std::string *chunk) {
  if (!state_ || !chunk)
    return;
  State &s = *state_;
  size_t keep = s.out.size();
  if (!s.finished) {
    // The newest whitespace run and the byte before it can still be rewritten
    // by later trims and by the space/paragraph rules.
    while (keep > 0 && IsAsciiSpace(s.out[keep - 1]))
      --keep;
    if (keep > 0)
      --keep;
  }
  if (!keep)
    return;
  chunk->append(s.out, 0, keep);
  s.out.erase(0, keep);
  s.core.out_base += keep;
}
std::string ExtractToText(const std::string &source,
                          const InlineImageCallbacks &callbacks,
                          std::vector<std::pair<uint32_t, uint32_t>> *map) {
  std::string output;
  Session session;
  if (!session.Begin(&source, &output, callbacks, map))
    return output;
  while (!session.Step()) {
    if (session.NoMemory())
      return std::string();
  }
  return output;
}

} // namespace mobi_safe_markup_extract
