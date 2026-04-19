/*
    3dslibris - mobi_markup_extract.cpp
*/

#include "formats/mobi/mobi_markup_extract.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>

#include "book/book.h"
#include "book/book_parse_deps.h"
#include "formats/common/html_entity_utils.h"
#include "formats/common/text_helpers.h"
#include "formats/mobi/mobi.h"
#include "formats/mobi/mobi_heading_markers.h"
#include "formats/mobi/mobi_markup_tag.h"
#include "formats/mobi/mobi_position_map.h"
#include "parse.h"
#include "ui/text.h"

namespace mobi_markup_extract {
namespace {

thread_local const ExtractCallbacks *g_callbacks = nullptr;

static std::string Trim(const std::string &in) {
  return (g_callbacks && g_callbacks->trim_ascii_whitespace)
             ? g_callbacks->trim_ascii_whitespace(in)
             : in;
}

static std::string Collapse(const std::string &in) {
  return (g_callbacks && g_callbacks->collapse_ascii_whitespace)
             ? g_callbacks->collapse_ascii_whitespace(in)
             : in;
}

static bool LooksStructured(const std::string &title) {
  return g_callbacks && g_callbacks->looks_like_structured_chapter_title
             ? g_callbacks->looks_like_structured_chapter_title(title)
             : false;
}

static void AppendParagraphBreak(std::string *out) {
  if (!out)
    return;
  while (!out->empty() &&
         (out->back() == ' ' || out->back() == '\t' || out->back() == '\r'))
    out->pop_back();
  if (out->empty()) {
    out->append("\n\n");
    return;
  }
  if (out->size() >= 2 && (*out)[out->size() - 1] == '\n' &&
      (*out)[out->size() - 2] == '\n')
    return;
  if (out->back() == '\n')
    out->push_back('\n');
  else
    out->append("\n\n");
}

static void AppendSingleSpace(std::string *out) {
  if (!out)
    return;
  if (out->empty())
    return;
  char tail = out->back();
  if (tail != ' ' && tail != '\n' && tail != '\t' && tail != '\r')
    out->push_back(' ');
}

static bool DecodeHtmlEntity(const std::string &entity, std::string *out) {
  return html_entity_utils::DecodeHtmlEntityUtf8(entity, out);
}

enum MobiMarkupBlockItemType {
  MOBI_MARKUP_BLOCK_TEXT = 0,
  MOBI_MARKUP_BLOCK_HARD_BREAK,
  MOBI_MARKUP_BLOCK_IMAGE
};

struct MobiMarkupBlockItem {
  MobiMarkupBlockItemType type;
  std::string text;
  u16 image_id;
  size_t marker_offset;
};

struct MobiMarkupBlock {
  std::vector<MobiMarkupBlockItem> items;
};

static void AppendTextToMobiMarkupBlock(MobiMarkupBlock *block,
                                        const std::string &text) {
  if (!block || text.empty())
    return;
  if (!block->items.empty() &&
      block->items.back().type == MOBI_MARKUP_BLOCK_TEXT) {
    block->items.back().text += text;
    return;
  }
  MobiMarkupBlockItem item;
  item.type = MOBI_MARKUP_BLOCK_TEXT;
  item.text = text;
  item.image_id = 0;
  item.marker_offset = SIZE_MAX;
  block->items.push_back(item);
}

static void AppendHardBreakToMobiMarkupBlock(MobiMarkupBlock *block) {
  if (!block)
    return;
  if (!block->items.empty() &&
      block->items.back().type == MOBI_MARKUP_BLOCK_HARD_BREAK)
    return;
  MobiMarkupBlockItem item;
  item.type = MOBI_MARKUP_BLOCK_HARD_BREAK;
  item.image_id = 0;
  item.marker_offset = SIZE_MAX;
  block->items.push_back(item);
}

static void AppendImageToMobiMarkupBlock(MobiMarkupBlock *block, u16 image_id,
                                         size_t marker_offset) {
  if (!block)
    return;
  MobiMarkupBlockItem item;
  item.type = MOBI_MARKUP_BLOCK_IMAGE;
  item.image_id = image_id;
  item.marker_offset = marker_offset;
  block->items.push_back(item);
}

static bool MobiMarkupBlockItemHasMeaningfulText(const MobiMarkupBlockItem &item) {
  return item.type == MOBI_MARKUP_BLOCK_TEXT && !Trim(item.text).empty();
}

static bool MobiMarkupBlockItemIsMeaningful(const MobiMarkupBlockItem &item) {
  return item.type == MOBI_MARKUP_BLOCK_IMAGE ||
         MobiMarkupBlockItemHasMeaningfulText(item);
}

static bool MobiMarkupBlockHasMeaningfulContent(const MobiMarkupBlock &block) {
  for (size_t i = 0; i < block.items.size(); i++) {
    if (MobiMarkupBlockItemIsMeaningful(block.items[i]))
      return true;
  }
  return false;
}

static void FinalizeMobiMarkupBlock(std::vector<MobiMarkupBlock> *blocks,
                                    MobiMarkupBlock *current) {
  if (!blocks || !current)
    return;
  if (!MobiMarkupBlockHasMeaningfulContent(*current)) {
    current->items.clear();
    return;
  }
  blocks->push_back(*current);
  current->items.clear();
}

static int FindFirstMeaningfulMobiMarkupBlockItem(const MobiMarkupBlock &block) {
  for (size_t i = 0; i < block.items.size(); i++) {
    if (MobiMarkupBlockItemIsMeaningful(block.items[i]))
      return (int)i;
  }
  return -1;
}

static bool MobiMarkupBlockHasMeaningfulTextAfter(
    const MobiMarkupBlock &block, size_t start_index) {
  if (start_index >= block.items.size())
    return false;
  for (size_t i = start_index + 1; i < block.items.size(); i++) {
    if (MobiMarkupBlockItemHasMeaningfulText(block.items[i]))
      return true;
  }
  return false;
}

static bool MobiMarkupBlockHasMeaningfulTextBefore(
    const MobiMarkupBlock &block, size_t end_index) {
  if (end_index >= block.items.size())
    return false;
  for (size_t i = 0; i < end_index; i++) {
    if (MobiMarkupBlockItemHasMeaningfulText(block.items[i]))
      return true;
  }
  return false;
}

static int FindLastMeaningfulMobiMarkupBlockItem(const MobiMarkupBlock &block) {
  for (int i = (int)block.items.size() - 1; i >= 0; i--) {
    if (MobiMarkupBlockItemIsMeaningful(block.items[(size_t)i]))
      return i;
  }
  return -1;
}

static size_t CountMobiMarkupBlockImages(const MobiMarkupBlock &block) {
  size_t count = 0;
  for (size_t i = 0; i < block.items.size(); i++) {
    if (block.items[i].type == MOBI_MARKUP_BLOCK_IMAGE)
      count++;
  }
  return count;
}

static bool MobiMarkupBlockHasOnlyImage(const MobiMarkupBlock &block,
                                        size_t image_index) {
  if (image_index >= block.items.size())
    return false;
  int first = FindFirstMeaningfulMobiMarkupBlockItem(block);
  int last = FindLastMeaningfulMobiMarkupBlockItem(block);
  if (first < 0 || last < 0 || (size_t)first != image_index ||
      (size_t)last != image_index)
    return false;
  if (block.items[image_index].type != MOBI_MARKUP_BLOCK_IMAGE)
    return false;
  return true;
}

static std::string CollectMobiMarkupBlockText(const MobiMarkupBlock &block) {
  std::string text;
  for (size_t i = 0; i < block.items.size(); i++) {
    const MobiMarkupBlockItem &item = block.items[i];
    if (item.type == MOBI_MARKUP_BLOCK_TEXT) {
      text += item.text;
    } else if (item.type == MOBI_MARKUP_BLOCK_HARD_BREAK) {
      if (!text.empty() && text.back() != '\n')
        text.push_back('\n');
    }
  }
  return text;
}

static int EstimateWrappedLineCount(const TextLayoutSnapshot *layout,
                                    const std::string &text, int width) {
  if (text.empty())
    return 0;
  if (!layout || width <= 0)
    return 1;

  const int avg_char_width = std::max(1, layout->pixel_size / 2); // snapshot heuristic, no live Text*
  const int space_width = avg_char_width;

  int lines = 1;
  int line_width = 0;
  int word_width = 0;
  bool have_word = false;
  bool pending_space = false;
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = (unsigned char)text[i];
    if (c == '\r') {
      i++;
      continue;
    }
    if (c == '\n') {
      if (have_word) {
        if (line_width == 0 || !pending_space) {
          line_width = (line_width == 0) ? word_width : (line_width + word_width);
        } else if (line_width + space_width + word_width <= width) {
          line_width += space_width + word_width;
        } else {
          lines++;
          line_width = word_width;
        }
        word_width = 0;
        have_word = false;
      }
      lines++;
      line_width = 0;
      pending_space = false;
      i++;
      continue;
    }
    if (isspace(c)) {
      if (have_word) {
        if (line_width == 0) {
          line_width = word_width;
        } else if (line_width + space_width + word_width <= width) {
          line_width += space_width + word_width;
        } else {
          lines++;
          line_width = word_width;
        }
        word_width = 0;
        have_word = false;
      }
      pending_space = true;
      i++;
      continue;
    }

    u32 cp = 0;
    size_t step = 1;
    if (c >= 0x80) {
      // Simple UTF-8 decoder without needing Text*
      if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
        cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
        step = 2;
      } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
        cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        step = 3;
      } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
        cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) |
             ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
        step = 4;
      } else {
        cp = c;
      }
    } else {
      cp = c;
    }
    if (step == 0 || i + step > text.size()) {
      step = 1;
      cp = c;
    }
    word_width += std::max(1, avg_char_width);
    have_word = true;
    i += step;
  }

  if (have_word) {
    if (line_width == 0) {
      line_width = word_width;
    } else if (pending_space && line_width + space_width + word_width <= width) {
      line_width += space_width + word_width;
    } else if (!pending_space && line_width + word_width <= width) {
      line_width += word_width;
    } else {
      lines++;
    }
  }

  return std::max(1, lines);
}

static bool StartsLikeListOrTitle(const std::string &text) {
  std::string compact = Collapse(Trim(text));
  if (compact.empty())
    return true;
  unsigned char first = (unsigned char)compact[0];
  if (first == '-' || first == '*' || first == 0xE2)
    return true;
  return LooksStructured(compact);
}

static bool IsMobiCaptionLikeBlock(const MobiMarkupBlock &block,
                                   const TextLayoutSnapshot *layout,
                                   int text_width, int max_bytes,
                                   int max_lines) {
  if (!MobiMarkupBlockHasMeaningfulContent(block))
    return false;
  for (size_t i = 0; i < block.items.size(); i++) {
    if (block.items[i].type == MOBI_MARKUP_BLOCK_IMAGE)
      return false;
  }
  std::string raw = CollectMobiMarkupBlockText(block);
  std::string collapsed = Collapse(Trim(raw));
  if (collapsed.empty())
    return false;
  if ((int)collapsed.size() > max_bytes)
    return false;
  if (StartsLikeListOrTitle(collapsed))
    return false;
  return EstimateWrappedLineCount(layout, raw, text_width) <= max_lines;
}

static bool IsMobiFigureFollowerBlock(const MobiMarkupBlock &block) {
  if (!MobiMarkupBlockHasMeaningfulContent(block))
    return false;
  for (size_t i = 0; i < block.items.size(); i++) {
    if (block.items[i].type == MOBI_MARKUP_BLOCK_IMAGE)
      return false;
  }
  std::string raw = CollectMobiMarkupBlockText(block);
  std::string collapsed = Collapse(Trim(raw));
  if (collapsed.empty())
    return false;
  return !StartsLikeListOrTitle(collapsed);
}

static u8 EstimateMobiImageFollowTextLines(const MobiMarkupBlock &block,
                                            const TextLayoutSnapshot *layout,
                                            int text_width, size_t image_index) {
  std::string follow_text;
  for (size_t i = image_index + 1; i < block.items.size(); i++) {
    const MobiMarkupBlockItem &item = block.items[i];
    if (item.type == MOBI_MARKUP_BLOCK_TEXT) {
      follow_text += item.text;
    } else if (item.type == MOBI_MARKUP_BLOCK_HARD_BREAK) {
      if (!follow_text.empty() && follow_text.back() != '\n')
        follow_text.push_back('\n');
    }
  }
  std::string collapsed = Collapse(Trim(follow_text));
  if (collapsed.empty())
    return 0;
  int lines = EstimateWrappedLineCount(layout, follow_text, text_width);
  lines = std::max(1, std::min(lines, 8));
  return (u8)lines;
}

static void ApplyMobiImageContexts(Book *book,
                                   std::vector<MobiMarkupBlock> *blocks,
                                   std::string *out,
                                   const TextLayoutSnapshot *layout,
                                   int text_width) {
  if (!book || !blocks || !out)
    return;

  for (size_t bi = 0; bi < blocks->size(); bi++) {
    MobiMarkupBlock &block = (*blocks)[bi];
    const int first = FindFirstMeaningfulMobiMarkupBlockItem(block);
    const int last = FindLastMeaningfulMobiMarkupBlockItem(block);
    const size_t image_count = CountMobiMarkupBlockImages(block);
    if (first < 0 || last < 0 || image_count == 0)
      continue;

    for (size_t ii = 0; ii < block.items.size(); ii++) {
      MobiMarkupBlockItem &image = block.items[ii];
      if (image.type != MOBI_MARKUP_BLOCK_IMAGE)
        continue;

      const bool has_text_before = MobiMarkupBlockHasMeaningfulTextBefore(block, ii);
      const bool has_text_after = MobiMarkupBlockHasMeaningfulTextAfter(block, ii);
      const bool boundary_image = ((int)ii == first || (int)ii == last);
      bool figure_with_caption =
          image_count == 1 && boundary_image && (has_text_before || has_text_after);
      u8 follow_text_lines = 0;

      if (figure_with_caption)
        follow_text_lines =
            EstimateMobiImageFollowTextLines(block, layout, text_width, ii);

      if (!figure_with_caption && MobiMarkupBlockHasOnlyImage(block, ii)) {
        for (size_t next = bi + 1; next < blocks->size(); next++) {
          if (!MobiMarkupBlockHasMeaningfulContent((*blocks)[next]))
            continue;
          const bool caption_like =
              IsMobiCaptionLikeBlock((*blocks)[next], layout, text_width, 220, 4);
          const bool figure_follower = IsMobiFigureFollowerBlock((*blocks)[next]);
          figure_with_caption = caption_like || figure_follower;
          if (figure_with_caption) {
            std::string raw = CollectMobiMarkupBlockText((*blocks)[next]);
            follow_text_lines = (u8)std::max(
                1, std::min(EstimateWrappedLineCount(layout, raw, text_width), 8));
          }
          break;
        }
      }

      if (!figure_with_caption)
        continue;
      if (image.marker_offset != SIZE_MAX && image.marker_offset < out->size())
        (*out)[image.marker_offset] = (char)TEXT_IMAGE_FIGURE_WITH_CAPTION;
      if (follow_text_lines > 0)
        book->SetInlineImageFollowTextLines(image.image_id, follow_text_lines);
    }
  }
}

} // namespace

std::string ExtractToText(
    Book *book, const BookParseDeps &deps, const std::string &in,
    std::vector<mobi_toc_finalize::MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map,
    const ExtractCallbacks &callbacks) {
  g_callbacks = &callbacks;

  std::string out;
  out.reserve(in.size());
  std::vector<MobiMarkupBlock> blocks;
  MobiMarkupBlock current_block;
  std::string block_pending_text;
  bool in_script = false;
  bool in_style = false;
  bool pending_space = false;
  bool at_paragraph_start = true;
  int heading_level = -1;
  std::string heading_text;

  const size_t kSampleInterval =
      mobi_position_map::HtmlSampleIntervalForTextBytes(in.size());
  size_t next_sample = 0;
  if (html_to_text_map) {
    html_to_text_map->clear();
    html_to_text_map->reserve(in.size() / kSampleInterval + 2);
    html_to_text_map->push_back({0, 0});
  }

  auto flush_block_text = [&]() {
    if (block_pending_text.empty())
      return;
    AppendTextToMobiMarkupBlock(&current_block, block_pending_text);
    block_pending_text.clear();
  };

  auto finalize_block = [&]() {
    flush_block_text();
    FinalizeMobiMarkupBlock(&blocks, &current_block);
  };

  auto append_pending_space = [&]() {
    if (!pending_space)
      return;
    AppendSingleSpace(&out);
    if (!out.empty() && out.back() == ' ')
      block_pending_text.push_back(' ');
    if (heading_level >= 0 && !heading_text.empty() && heading_text.back() != ' ')
      heading_text.push_back(' ');
    pending_space = false;
    at_paragraph_start = false;
  };

  for (size_t i = 0; i < in.size();) {
    if (html_to_text_map && i >= next_sample) {
      html_to_text_map->push_back({(u32)i, (u32)out.size()});
      next_sample = i + kSampleInterval;
    }
    unsigned char c = (unsigned char)in[i];
    if (c == '<') {
      size_t close = in.find('>', i + 1);
      if (close == std::string::npos) {
        i++;
        continue;
      }

      const size_t tag_offset = i + 1;
      const size_t tag_length = close - tag_offset;
      i = close + 1;
      if (tag_length >= 3 && in[tag_offset] == '!' && in[tag_offset + 1] == '-' &&
          in[tag_offset + 2] == '-')
        continue;

      MobiMarkupTagInfo tag_info;
      if (!mobi_parse_markup_tag(in.data() + tag_offset, tag_length, &tag_info) ||
          !tag_info.valid) {
        continue;
      }

      if (tag_info.kind == MOBI_MARKUP_TAG_SCRIPT) {
        in_script = !tag_info.closing;
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_STYLE) {
        in_style = !tag_info.closing;
        continue;
      }
      if (in_script || in_style)
        continue;

      int tag_heading_level = tag_info.heading_level;
      if (!tag_info.closing && tag_heading_level >= 0) {
        heading_level = tag_heading_level;
        heading_text.clear();
      } else if (tag_info.closing && tag_heading_level >= 0 && heading_level >= 0) {
        std::string normalized = Collapse(Trim(heading_text));
        if (heading_hints && normalized.size() >= 3 && normalized.size() <= 180) {
          if (heading_hints->empty() ||
              heading_hints->back().title != normalized ||
              heading_hints->back().level != (u8)heading_level) {
            mobi_toc_finalize::MobiHeadingHint hint;
            hint.title = normalized;
            hint.level = (u8)heading_level;
            heading_hints->push_back(hint);
          }
        }
        heading_level = -1;
        heading_text.clear();
      }

      if (tag_info.kind == MOBI_MARKUP_TAG_BR) {
        flush_block_text();
        AppendHardBreakToMobiMarkupBlock(&current_block);
        out.push_back('\n');
        if (heading_level >= 0 && !heading_text.empty() && heading_text.back() != ' ')
          heading_text.push_back(' ');
        pending_space = false;
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_IMG && !tag_info.closing) {
        u16 recindex = 0;
        if (book) {
          const std::string tag(in, tag_offset, tag_length);
          if (!mobi_extract_image_recindex(tag, &recindex))
            continue;
          if (pending_space) {
            AppendSingleSpace(&out);
            if (!out.empty() && out.back() == ' ')
              block_pending_text.push_back(' ');
            pending_space = false;
          }
          flush_block_text();
          u16 image_id =
              book->RegisterInlineImage(mobi_inline_image_path(recindex));
          size_t marker_offset = out.size();
          out.push_back((char)(at_paragraph_start ? TEXT_IMAGE_LEADING_PARAGRAPH
                                                  : TEXT_IMAGE_CONTEXT_DEFAULT));
          out.push_back((char)TEXT_IMAGE);
          out.push_back((char)((image_id >> 8) & 0xFF));
          out.push_back((char)(image_id & 0xFF));
          AppendImageToMobiMarkupBlock(&current_block, image_id, marker_offset);
          at_paragraph_start = false;
        }
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_LI && !tag_info.closing) {
        finalize_block();
        AppendParagraphBreak(&out);
        out.append("- ");
        block_pending_text += "- ";
        pending_space = false;
        at_paragraph_start = false;
        continue;
      }
      if (tag_info.kind == MOBI_MARKUP_TAG_BLOCK) {
        finalize_block();
        AppendParagraphBreak(&out);
        pending_space = false;
        at_paragraph_start = true;
        if (!tag_info.closing && tag_heading_level >= 0) {
          const unsigned char marker =
              mobi_heading_markers::MarkerForHeadingLevel(tag_heading_level);
          if (marker != 0)
            out.push_back((char)marker);
        }
      }
      continue;
    }

    if (in_script || in_style) {
      i++;
      continue;
    }

    if (c == '&') {
      size_t semi = in.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 12) {
        std::string entity = in.substr(i + 1, semi - i - 1);
        std::string decoded;
        if (DecodeHtmlEntity(entity, &decoded)) {
          out += decoded;
          block_pending_text += decoded;
          if (heading_level >= 0)
            heading_text += decoded;
          pending_space = false;
          if (!decoded.empty() &&
              decoded.find_first_not_of(" \t\r\n") != std::string::npos) {
            at_paragraph_start = false;
          }
          i = semi + 1;
          continue;
        }
      }
    }

    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      pending_space = true;
      while (i < in.size()) {
        i++;
        if (i >= in.size())
          break;
        c = (unsigned char)in[i];
        if (c != '\r' && c != '\n' && c != '\t' && c != ' ')
          break;
      }
      continue;
    }
    if (c < 0x20) {
      do {
        i++;
      } while (i < in.size() && (unsigned char)in[i] < 0x20 &&
               in[i] != '\r' && in[i] != '\n' && in[i] != '\t');
      continue;
    }

    if (c < 0x80) {
      append_pending_space();
      const size_t run_start = i;
      while (i < in.size()) {
        i++;
        if (i >= in.size())
          break;
        c = (unsigned char)in[i];
        if (c >= 0x80 || c == '<' || c == '&' || c == '\r' || c == '\n' ||
            c == '\t' || c == ' ' || c < 0x20)
          break;
      }
      const size_t run_len = i - run_start;
      out.append(in, run_start, run_len);
      block_pending_text.append(in, run_start, run_len);
      if (heading_level >= 0)
        heading_text.append(in, run_start, run_len);
      at_paragraph_start = false;
      continue;
    }

    append_pending_space();
    int step = 1;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;
    if (i + (size_t)step > in.size())
      step = 1;
    out.append(in, i, (size_t)step);
    block_pending_text.append(in, i, (size_t)step);
    if (heading_level >= 0)
      heading_text.append(in, i, (size_t)step);
    at_paragraph_start = false;
    i += (size_t)step;
  }

  finalize_block();

  if (book && !deps.layout.regular_font_path.empty()) {
    const int text_width = 240 - deps.layout.margin_left - deps.layout.margin_right;
    ApplyMobiImageContexts(book, &blocks, &out, &deps.layout, text_width);
  }

  if (html_to_text_map)
    html_to_text_map->push_back({(u32)in.size(), (u32)out.size()});

  g_callbacks = nullptr;
  return out;
}

} // namespace mobi_markup_extract
