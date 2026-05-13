#include "book/book_xml_table_utils.h"
#include "test_assert.h"
#include "shared/text_token_constants.h"

#include <string>
#include <vector>

static std::string B(const char *s) {
  return std::string(1, (char)TEXT_BOLD_ON) + s +
         std::string(1, (char)TEXT_BOLD_OFF);
}

namespace {

using book_xml_table_utils::TableCell;
using book_xml_table_utils::TableRow;

TableCell MakeCell(const char *text, bool is_header = false,
                   bool is_row_header = false) {
  TableCell cell;
  cell.text = text ? text : "";
  cell.is_header = is_header;
  cell.is_row_header = is_row_header;
  return cell;
}

void TestBuildTableLinesUsesCaptionAndColumnHeaders() {
  TableRow header;
  header.cells.push_back(MakeCell("Field", true));
  header.cells.push_back(MakeCell("Description", true));
  header.cells.push_back(MakeCell("Type", true));

  TableRow row;
  row.cells.push_back(MakeCell("abstract"));
  row.cells.push_back(MakeCell("paper abstract"));
  row.cells.push_back(MakeCell("string"));

  TableRow row2;
  row2.cells.push_back(MakeCell("listen_count"));
  row2.cells.push_back(MakeCell("user song listen count"));
  row2.cells.push_back(MakeCell("integer"));

  const std::vector<TableRow> rows = {row, row2};
  const std::vector<std::string> lines = book_xml_table_utils::BuildTableLines(
      "Dataset Fields", &header, rows);

  test::ExpectEq("caption plus two row blocks", (int)lines.size(), 8);
  test::ExpectStrEq("caption line", lines[0].c_str(), "Dataset Fields");
  test::ExpectStrEq("row title", lines[1].c_str(), "abstract");
  test::ExpectStrEq("row second field", lines[2].c_str(),
                    ("- " + B("Description") + ": paper abstract").c_str());
  test::ExpectStrEq("row third field", lines[3].c_str(),
                    ("- " + B("Type") + ": string").c_str());
  test::ExpectStrEq("row separator", lines[4].c_str(), "");
  test::ExpectStrEq("second row first field", lines[5].c_str(),
                    "listen_count");
  test::ExpectStrEq("second row second field", lines[6].c_str(),
                    ("- " + B("Description") + ": user song listen count").c_str());
  test::ExpectStrEq("second row third field", lines[7].c_str(),
                    ("- " + B("Type") + ": integer").c_str());
}

void TestBuildTableLinesUsesRowHeaderWhenPresent() {
  TableRow row;
  row.cells.push_back(MakeCell("BoW", true, true));
  row.cells.push_back(MakeCell("l2 regularization: 0.1"));
  row.cells.push_back(MakeCell("accuracy: 95%"));

  TableRow row2;
  row2.cells.push_back(MakeCell("TF-IDF", true, true));
  row2.cells.push_back(MakeCell("min_df: 3"));
  row2.cells.push_back(MakeCell("accuracy: 96%"));

  const std::vector<TableRow> rows = {row, row2};
  const std::vector<std::string> lines =
      book_xml_table_utils::BuildTableLines("", NULL, rows);

  test::ExpectEq("two row blocks", (int)lines.size(), 7);
  test::ExpectStrEq("row header title", lines[0].c_str(), B("BoW").c_str());
  test::ExpectStrEq("row item one", lines[1].c_str(),
                    "- l2 regularization: 0.1");
  test::ExpectStrEq("row item two", lines[2].c_str(), "- accuracy: 95%");
  test::ExpectStrEq("row separator", lines[3].c_str(), "");
  test::ExpectStrEq("second row header title", lines[4].c_str(), B("TF-IDF").c_str());
  test::ExpectStrEq("second row item one", lines[5].c_str(), "- min_df: 3");
  test::ExpectStrEq("second row item two", lines[6].c_str(), "- accuracy: 96%");
}

void TestBuildTableLinesFallsBackToSimpleJoinWithoutHeaders() {
  TableRow row;
  row.cells.push_back(MakeCell("30"));
  row.cells.push_back(MakeCell("64"));
  row.cells.push_back(MakeCell("49"));

  const std::vector<TableRow> rows(1, row);
  const std::vector<std::string> lines =
      book_xml_table_utils::BuildTableLines("", NULL, rows);

  test::ExpectEq("one simple line", (int)lines.size(), 1);
  test::ExpectStrEq("plain join", lines[0].c_str(), "30 | 64 | 49");
}

void TestNormalizeTableCellTextCollapsesWhitespace() {
  test::ExpectStrEq("collapse whitespace",
                    book_xml_table_utils::NormalizeTableCellText(
                        "  Field \n\t Name   ")
                        .c_str(),
                    "Field Name");
}

} // namespace

int main() {
  TestBuildTableLinesUsesCaptionAndColumnHeaders();
  TestBuildTableLinesUsesRowHeaderWhenPresent();
  TestBuildTableLinesFallsBackToSimpleJoinWithoutHeaders();
  TestNormalizeTableCellTextCollapsesWhitespace();
  return 0;
}
