#include "book/book_xml_table_utils.h"

#include <algorithm>

#include "shared/string_utils.h"
#include "shared/text_token_constants.h"

namespace book_xml_table_utils {
namespace {

bool IsEmphasisMarker(unsigned char c) {
  return c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
         c == TEXT_ITALIC_OFF;
}

std::string JoinParts(const std::vector<std::string> &parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (parts[i].empty())
      continue;
    if (!out.empty())
      out += " | ";
    out += parts[i];
  }
  return out;
}

static std::string BoldWrap(const std::string &s) {
  return std::string(1, (char)TEXT_BOLD_ON) + s +
         std::string(1, (char)TEXT_BOLD_OFF);
}

std::vector<std::string> BuildRowBlockWithHeaders(const TableRow &row,
                                                  const TableRow *header_row) {
  std::vector<std::string> lines;
  const size_t count = row.cells.size();
  for (size_t i = 0; i < count; i++) {
    const std::string value = NormalizeTableCellText(row.cells[i].text);
    if (value.empty())
      continue;
    if (lines.empty()) {
      lines.push_back(value);
      continue;
    }

    std::string line = "- " + value;
    if (header_row && i < header_row->cells.size()) {
      const std::string label =
          NormalizeTableCellText(header_row->cells[i].text);
      if (!label.empty()) {
        line = "- " + BoldWrap(label) + ": " + value;
      }
    }
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> BuildRowBlockWithoutHeaders(const TableRow &row) {
  std::vector<std::string> lines;
  std::vector<std::string> parts;
  parts.reserve(row.cells.size());
  std::string row_header;
  for (size_t i = 0; i < row.cells.size(); i++) {
    const TableCell &cell = row.cells[i];
    const std::string value = NormalizeTableCellText(cell.text);
    if (value.empty())
      continue;
    if (row_header.empty() && cell.is_row_header) {
      row_header = value;
      continue;
    }
    parts.push_back(value);
  }

  if (!row_header.empty()) {
    lines.push_back(BoldWrap(row_header));
    for (size_t i = 0; i < parts.size(); i++)
      lines.push_back("- " + parts[i]);
    return lines;
  }

  const std::string line = JoinParts(parts);
  if (!line.empty())
    lines.push_back(line);
  return lines;
}

} // namespace

std::string NormalizeTableCellText(const std::string &text) {
  // Collapses runs of blanks to one space and keeps hard line breaks ('\n',
  // from <br>) as single separators. Emphasis markers pass through; opening
  // markers flush a pending separator first, closing markers do not, so the
  // separator stays outside the emphasized run.
  std::string out;
  out.reserve(text.size());
  int pending = 0; // 0 none, 1 space, 2 line break
  bool have_content = false;
  for (size_t i = 0; i < text.size(); i++) {
    const unsigned char c = (unsigned char)text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      const int kind = (c == '\n') ? 2 : 1;
      if (kind > pending)
        pending = kind;
      continue;
    }
    const bool closing = (c == TEXT_BOLD_OFF || c == TEXT_ITALIC_OFF);
    if (!closing && have_content && pending) {
      out.push_back(pending == 2 ? '\n' : ' ');
      pending = 0;
    } else if (!closing && !have_content) {
      pending = 0;
    }
    out.push_back((char)c);
    if (!IsEmphasisMarker(c))
      have_content = true;
  }
  return have_content ? out : std::string();
}

std::vector<std::string> SplitCellLines(const std::string &text) {
  std::vector<std::string> lines;
  bool bold = false, italic = false;
  std::string cur;
  const std::string bold_on(1, (char)TEXT_BOLD_ON);
  const std::string italic_on(1, (char)TEXT_ITALIC_ON);
  for (size_t i = 0; i <= text.size(); i++) {
    const bool end = (i == text.size());
    const char c = end ? '\n' : text[i];
    if (c != '\n') {
      cur.push_back(c);
      if ((unsigned char)c == TEXT_BOLD_ON) bold = true;
      else if ((unsigned char)c == TEXT_BOLD_OFF) bold = false;
      else if ((unsigned char)c == TEXT_ITALIC_ON) italic = true;
      else if ((unsigned char)c == TEXT_ITALIC_OFF) italic = false;
      continue;
    }
    // Close emphasis at the end of the line and reopen it on the next one so
    // every emitted line is self-contained.
    if (bold && !end) cur.push_back((char)TEXT_BOLD_OFF);
    if (italic && !end) cur.push_back((char)TEXT_ITALIC_OFF);
    lines.push_back(cur);
    cur.clear();
    if (!end) {
      if (bold) cur += bold_on;
      if (italic) cur += italic_on;
    }
  }
  return lines;
}

std::vector<std::string> BuildTableLines(const std::string &caption,
                                         const TableRow *header_row,
                                         const std::vector<TableRow> &rows) {
  std::vector<std::string> lines;
  const std::string normalized_caption = NormalizeTableCellText(caption);
  if (!normalized_caption.empty())
    lines = SplitCellLines(normalized_caption);

  bool emitted_row = false;
  for (size_t i = 0; i < rows.size(); i++) {
    const TableRow &row = rows[i];
    std::vector<std::string> row_lines;
    if (header_row && !header_row->cells.empty()) {
      row_lines = BuildRowBlockWithHeaders(row, header_row);
    } else {
      row_lines = BuildRowBlockWithoutHeaders(row);
    }
    if (row_lines.empty())
      continue;
    if (emitted_row)
      lines.push_back("");
    for (size_t j = 0; j < row_lines.size(); j++) {
      const std::vector<std::string> parts = SplitCellLines(row_lines[j]);
      lines.insert(lines.end(), parts.begin(), parts.end());
    }
    emitted_row = true;
  }

  return lines;
}

} // namespace book_xml_table_utils
