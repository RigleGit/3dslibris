#pragma once

#include <string>
#include <vector>

namespace book_xml_table_utils {

struct TableCell {
  std::string text;
  bool is_header;
  bool is_row_header;

  TableCell() : text(), is_header(false), is_row_header(false) {}
};

struct TableRow {
  std::vector<TableCell> cells;
};

std::string NormalizeTableCellText(const std::string &text);
std::vector<std::string> BuildTableLines(const std::string &caption,
                                         const TableRow *header_row,
                                         const std::vector<TableRow> &rows);

} // namespace book_xml_table_utils
