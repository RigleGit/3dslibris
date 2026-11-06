/*
    3dslibris - book_xml_table_handler.cpp

    Table capture handler extracted from book_xml_parser.cpp.

    Intercepts table/thead/tbody/tr/th/td/caption elements during XML parsing,
    accumulates cell text, and emits formatted table lines into the page buffer
    when the closing </table> tag is reached.
*/

#include "book/book_xml_table_handler.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_table_utils.h"
#include "parse.h"
#include "ui/text.h"

#include <string.h>
#include <string>
#include <vector>

namespace {

// Trivial helpers duplicated from book_xml_parser.cpp (no shared state).

static bool EqAsciiICase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool AttrEq(const char *name, const char *needle) {
  if (!name || !needle) return false;
  if (!strcmp(name, needle) || EqAsciiICase(name, needle)) return true;
  const char *colon = strrchr(name, ':');
  return colon && (strcmp(colon + 1, needle) == 0 || EqAsciiICase(colon + 1, needle));
}

static bool XmlEq(const char *name, const char *needle) {
  if (!name || !needle) return false;
  if (!strcmp(name, needle) || EqAsciiICase(name, needle)) return true;
  const char *colon = strrchr(name, ':');
  return colon && (strcmp(colon + 1, needle) == 0 || EqAsciiICase(colon + 1, needle));
}

static void HideCurrentStack(parsedata_t *p, bool hidden) {
  if (!p || p->stacksize == 0) return;
  p->style_hidden_stack[p->stacksize - 1] = hidden;
}

static void HideFlags(const char **attr, bool *out) {
  book_xml_css_resolver::ParseElementHiddenFlags(attr, out);
}

// Table capture internals.

static void ResetCapturedTable(parsedata_t *p) {
  if (!p) return;
  p->table_in_header_section = false;
  p->table_in_caption = false;
  p->table_in_row = false;
  p->table_in_cell = false;
  p->table_current_cell_is_header = false;
  p->table_current_cell_is_row_header = false;
  p->table_caption_text.clear();
  p->table_current_cell_text.clear();
  p->table_header_cells.clear();
  p->table_current_row_cells.clear();
  p->table_current_row_header_flags.clear();
  p->table_body_rows.clear();
  p->table_body_row_header_flags.clear();
}

static void AppendCapturedTableSeparator(parsedata_t *p, char separator) {
  std::string *buffer = GetActiveCapturedTableText(p);
  if (!buffer || buffer->empty()) return;
  const char last = (*buffer)[buffer->size() - 1];
  if (separator == '\n') {
    if (last != '\n') buffer->push_back('\n');
    return;
  }
  if (last != ' ' && last != '\n') buffer->push_back(' ');
}

static bool AttrScopeEqualsRow(const char **attr) {
  if (!attr) return false;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0]) continue;
    if (AttrEq(attr[i], "scope") && EqAsciiICase(attr[i + 1], "row"))
      return true;
  }
  return false;
}

static void FinishCapturedTableCell(parsedata_t *p) {
  if (!p || !p->table_in_cell) return;
  p->table_current_row_cells.push_back(
      book_xml_table_utils::NormalizeTableCellText(p->table_current_cell_text));
  p->table_current_row_header_flags.push_back(
      p->table_current_cell_is_row_header ? 1
                                          : (p->table_current_cell_is_header ? 2 : 0));
  p->table_current_cell_text.clear();
  p->table_current_cell_is_header = false;
  p->table_current_cell_is_row_header = false;
}

static void FinishCapturedTableRow(parsedata_t *p) {
  if (!p || p->table_current_row_cells.empty()) return;

  bool all_header = p->table_in_header_section;
  if (!all_header) {
    all_header = true;
    for (size_t i = 0; i < p->table_current_row_header_flags.size(); i++) {
      if (p->table_current_row_header_flags[i] != 2) { all_header = false; break; }
    }
  }

  if (p->table_header_cells.empty() && all_header) {
    p->table_header_cells = p->table_current_row_cells;
  } else {
    p->table_body_rows.push_back(p->table_current_row_cells);
    p->table_body_row_header_flags.push_back(p->table_current_row_header_flags);
  }

  p->table_current_row_cells.clear();
  p->table_current_row_header_flags.clear();
}

static std::vector<std::string> BuildCapturedTableLines(const parsedata_t *p) {
  std::vector<std::string> lines;
  if (!p) return lines;

  book_xml_table_utils::TableRow header_row;
  book_xml_table_utils::TableRow *header_ptr = NULL;
  if (!p->table_header_cells.empty()) {
    for (size_t i = 0; i < p->table_header_cells.size(); i++) {
      book_xml_table_utils::TableCell cell;
      cell.text = p->table_header_cells[i];
      cell.is_header = true;
      cell.is_row_header = false;
      header_row.cells.push_back(cell);
    }
    header_ptr = &header_row;
  }

  std::vector<book_xml_table_utils::TableRow> rows;
  rows.reserve(p->table_body_rows.size());
  for (size_t r = 0; r < p->table_body_rows.size(); r++) {
    book_xml_table_utils::TableRow row;
    const std::vector<std::string> &src_cells = p->table_body_rows[r];
    const std::vector<u8> &src_flags = p->table_body_row_header_flags[r];
    for (size_t c = 0; c < src_cells.size(); c++) {
      book_xml_table_utils::TableCell cell;
      cell.text = src_cells[c];
      cell.is_header = false;
      cell.is_row_header = c < src_flags.size() && src_flags[c] == 1;
      row.cells.push_back(cell);
    }
    rows.push_back(row);
  }

  return book_xml_table_utils::BuildTableLines(p->table_caption_text, header_ptr, rows);
}

static void EmitCapturedTable(parsedata_t *p, Text *ts,
                              const TableHandlerFns &fns) {
  if (!p || !ts) return;
  const std::vector<std::string> lines = BuildCapturedTableLines(p);
  if (lines.empty()) return;

  fns.flush_inline_tail(p, ts);
  if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
    fns.linefeed(p);

  for (size_t i = 0; i < lines.size(); i++) {
    const std::string &line = lines[i];
    if (line.empty()) {
      fns.linefeed(p);
      continue;
    }
    fns.emit_flowed_raw(p, line.c_str(), (int)line.size());
    fns.linefeed(p);
  }
}

} // namespace

std::string *GetActiveCapturedTableText(parsedata_t *p) {
  if (!p) return NULL;
  if (p->table_in_cell) return &p->table_current_cell_text;
  if (p->table_in_caption) return &p->table_caption_text;
  return NULL;
}

bool HandleTableStart(parsedata_t *p, Text *ts, const char *el,
                      const char **attr, const TableHandlerFns &fns) {
  if (!p || !el) return false;
  const bool entering_table = !strcmp(el, "table");
  const bool inside_table = parse_in(p, TAG_TABLE);
  if (!entering_table && !inside_table) return false;

  bool hidden = false;
  HideFlags(attr, &hidden);

  // Nested tables are flattened into the active outer cell. Keep their
  // structural tags from resetting the outer row/cell capture.
  unsigned table_depth = 0;
  for (unsigned i = 0; i < p->stacksize; ++i)
    if (p->stack[i] == TAG_TABLE)
      ++table_depth;
  if (inside_table && (entering_table || table_depth > 1)) {
    const context_t tag = entering_table ? TAG_TABLE :
        !strcmp(el, "script") ? TAG_SCRIPT :
        !strcmp(el, "style") ? TAG_STYLE : TAG_UNKNOWN;
    parse_push(p, tag);
    HideCurrentStack(p, hidden);
    if (!hidden) AppendCapturedTableSeparator(p, ' ');
    return true;
  }

  if (entering_table) {
    fns.flush_inline_tail(p, ts);
    if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
      fns.linefeed(p);
    ResetCapturedTable(p);
    parse_push(p, TAG_TABLE);
    HideCurrentStack(p, hidden);
    p->in_paragraph = false;
    p->paragraph_has_content = false;
    return true;
  }

  if (!strcmp(el, "caption")) {
    parse_push(p, TAG_CAPTION);
    HideCurrentStack(p, hidden);
    p->table_in_caption = !hidden;
    return true;
  }
  if (!strcmp(el, "thead")) {
    parse_push(p, TAG_THEAD);
    HideCurrentStack(p, hidden);
    p->table_in_header_section = !hidden;
    return true;
  }
  if (!strcmp(el, "tbody")) {
    parse_push(p, TAG_TBODY);
    HideCurrentStack(p, hidden);
    return true;
  }
  if (!strcmp(el, "tr")) {
    parse_push(p, TAG_TR);
    HideCurrentStack(p, hidden);
    p->table_in_row = !hidden;
    p->table_current_row_cells.clear();
    p->table_current_row_header_flags.clear();
    return true;
  }
  if (!strcmp(el, "th")) {
    parse_push(p, TAG_TH);
    HideCurrentStack(p, hidden);
    p->table_in_cell = !hidden;
    p->table_current_cell_is_header = !hidden;
    p->table_current_cell_is_row_header = !hidden && AttrScopeEqualsRow(attr);
    p->table_current_cell_text.clear();
    return true;
  }
  if (!strcmp(el, "td")) {
    parse_push(p, TAG_TD);
    HideCurrentStack(p, hidden);
    p->table_in_cell = !hidden;
    p->table_current_cell_is_header = false;
    p->table_current_cell_is_row_header = false;
    p->table_current_cell_text.clear();
    return true;
  }
  if (!strcmp(el, "br")) {
    parse_push(p, TAG_BR);
    HideCurrentStack(p, hidden);
    if (!hidden) AppendCapturedTableSeparator(p, '\n');
    return true;
  }
  if (!strcmp(el, "p") || !strcmp(el, "div") || !strcmp(el, "li") ||
      !strcmp(el, "ul") || !strcmp(el, "ol") || !strcmp(el, "blockquote")) {
    parse_push(p, TAG_UNKNOWN);
    HideCurrentStack(p, hidden);
    if (!hidden) AppendCapturedTableSeparator(p, ' ');
    return true;
  }
  if (XmlEq(el, "img") || XmlEq(el, "image")) {
    parse_push(p, TAG_UNKNOWN);
    HideCurrentStack(p, hidden);
    // Images in table cells can't be positioned in columns; suppress them
    // entirely. BuildTableLines skips empty cells, so image-only tables
    // produce no output. Images outside tables still render normally.
    return true;
  }
  if (!strcmp(el, "script")) {
    parse_push(p, TAG_SCRIPT);
    HideCurrentStack(p, hidden);
    return true;
  }
  if (!strcmp(el, "style")) {
    parse_push(p, TAG_STYLE);
    HideCurrentStack(p, hidden);
    return true;
  }

  parse_push(p, TAG_UNKNOWN);
  HideCurrentStack(p, hidden);
  return true;
}

bool HandleTableEnd(parsedata_t *p, Text *ts, const char *el,
                    const TableHandlerFns &fns) {
  if (!p || !el || !parse_in(p, TAG_TABLE)) return false;

  unsigned table_depth = 0;
  for (unsigned i = 0; i < p->stacksize; ++i)
    if (p->stack[i] == TAG_TABLE)
      ++table_depth;
  if (table_depth > 1) {
    AppendCapturedTableSeparator(p, ' ');
    parse_pop(p);
    return true;
  }

  if (!strcmp(el, "th") || !strcmp(el, "td")) {
    FinishCapturedTableCell(p);
    p->table_in_cell = false;
    p->table_current_cell_is_header = false;
    p->table_current_cell_is_row_header = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "tr")) {
    if (p->table_in_cell) FinishCapturedTableCell(p);
    p->table_in_cell = false;
    FinishCapturedTableRow(p);
    p->table_in_row = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "thead")) {
    p->table_in_header_section = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "tbody")) {
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "caption")) {
    p->table_in_caption = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "table")) {
    if (p->table_in_cell) FinishCapturedTableCell(p);
    if (p->table_in_row) FinishCapturedTableRow(p);
    EmitCapturedTable(p, ts, fns);
    ResetCapturedTable(p);
    parse_pop(p);
    return true;
  }

  parse_pop(p);
  return true;
}
