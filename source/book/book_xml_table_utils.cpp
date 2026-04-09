#include "book/book_xml_table_utils.h"

#include <algorithm>

#include "shared/string_utils.h"

namespace book_xml_table_utils {
namespace {

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
        line = "- " + label + ": " + value;
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
    lines.push_back(row_header);
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
  std::string out;
  out.reserve(text.size());
  bool prev_space = true;
  for (size_t i = 0; i < text.size(); i++) {
    const unsigned char c = (unsigned char)text[i];
    const bool is_space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (is_space) {
      if (!prev_space)
        out.push_back(' ');
      prev_space = true;
      continue;
    }
    out.push_back((char)c);
    prev_space = false;
  }
  return Trim(out);
}

std::vector<std::string> BuildTableLines(const std::string &caption,
                                         const TableRow *header_row,
                                         const std::vector<TableRow> &rows) {
  std::vector<std::string> lines;
  const std::string normalized_caption = NormalizeTableCellText(caption);
  if (!normalized_caption.empty())
    lines.push_back(normalized_caption);

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
    for (size_t j = 0; j < row_lines.size(); j++)
      lines.push_back(row_lines[j]);
    emitted_row = true;
  }

  return lines;
}

} // namespace book_xml_table_utils
