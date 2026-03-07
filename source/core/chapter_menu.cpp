#include "chapter_menu.h"

#include <ctype.h>

#include "app.h"
#include "book.h"

namespace {

static std::string TrimLabel(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

static std::string BuildTwoLineLabelIfNeeded(const std::string &raw) {
  std::string clean = TrimLabel(raw);
  if (clean.empty())
    return clean;

  size_t split = clean.find(" : ");
  if (split == std::string::npos)
    split = clean.find(": ");
  if (split == std::string::npos)
    split = clean.find(':');

  if (split == std::string::npos)
    return clean;

  std::string title = TrimLabel(clean.substr(0, split));
  size_t rhs_start = split + 1;
  if (rhs_start < clean.size() && clean[rhs_start] == ' ')
    rhs_start++;
  std::string subtitle = TrimLabel(clean.substr(rhs_start));

  if (title.size() < 4 || subtitle.size() < 4)
    return clean;

  return title + "\n" + subtitle;
}

static std::string ApplyLevelPrefix(const std::string &label, u8 level) {
  if (label.empty() || level == 0)
    return label;

  const u8 capped = (level > 6) ? 6 : level;
  std::string prefix((size_t)capped * 2, ' ');
  prefix += "- ";

  std::string out;
  out.reserve(label.size() + prefix.size() * 2);
  bool line_start = true;
  for (size_t i = 0; i < label.size(); i++) {
    if (line_start) {
      out += prefix;
      line_start = false;
    }
    out.push_back(label[i]);
    if (label[i] == '\n')
      line_start = true;
  }
  return out;
}

static const char *TocQualityLabel(TocQuality q) {
  switch (q) {
  case TOC_QUALITY_STRONG:
    return "index";
  case TOC_QUALITY_MIXED:
    return "index (~)";
  case TOC_QUALITY_HEURISTIC:
    return "index (approx)";
  case TOC_QUALITY_UNKNOWN:
  default:
    return "index";
  }
}

} // namespace

ChapterMenu::ChapterMenu(App *_app) : PagedListMenu(_app, "index") {}

ChapterMenu::~ChapterMenu() {}

void ChapterMenu::BuildEntries(std::vector<std::string> &labels,
                               std::vector<u16> &pages) {
  if (!app || !app->bookcurrent)
    return;

  SetHeaderTitle(TocQualityLabel(app->bookcurrent->GetTocQuality()));

  const std::vector<ChapterEntry> &chapters = app->bookcurrent->GetChapters();
  labels.reserve(chapters.size());
  pages.reserve(chapters.size());

  for (const auto &ch : chapters) {
    std::string label = BuildTwoLineLabelIfNeeded(ch.title);
    labels.push_back(ApplyLevelPrefix(label, ch.level));
    pages.push_back(ch.page);
  }
}
