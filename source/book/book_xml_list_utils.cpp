#include "book/book_xml_list_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "shared/string_utils.h"

namespace book_xml_list_utils {

namespace {

bool AttrNameEqualsLocal(const char *name, const char *literal) {
  return name && literal && EqualsAsciiNoCase(name, literal);
}

std::string ToLowerAscii(const char *value) {
  if (!value)
    return std::string();
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    if (c >= 'A' && c <= 'Z')
      return (char)(c - 'A' + 'a');
    return (char)c;
  });
  return out;
}

bool ContainsClassToken(const std::string &class_names, const char *token) {
  if (class_names.empty() || !token || !token[0])
    return false;
  return ContainsToken(class_names, ToLowerAscii(token));
}

bool AttrValueEqualsNoCase(const char *value, const char *literal) {
  return value && literal && EqualsAsciiNoCase(value, literal);
}

bool ParseListMarkerHiddenAttr(const char **attr) {
  if (!attr)
    return false;
  for (int i = 0; attr[i]; i += 2) {
    const char *name = attr[i];
    const char *value = attr[i + 1];
    if (!value || !value[0])
      continue;

    if (AttrNameEqualsLocal(name, "style")) {
      const std::string style = ToLowerAscii(value);
      if (style.find("list-style-type:none") != std::string::npos ||
          style.find("list-style-type: none") != std::string::npos ||
          style.find("list-style:none") != std::string::npos ||
          style.find("list-style: none") != std::string::npos) {
        return true;
      }
    } else if (AttrNameEqualsLocal(name, "class")) {
      const std::string class_names = ToLowerAscii(value);
      if (ContainsClassToken(class_names, "simplelist") ||
          ContainsClassToken(class_names, "toc") ||
          ContainsClassToken(class_names, "index")) {
        return true;
      }
    } else if (AttrNameEqualsLocal(name, "data-type")) {
      if (AttrValueEqualsNoCase(value, "toc") ||
          AttrValueEqualsNoCase(value, "index")) {
        return true;
      }
    }
  }
  return false;
}

ordered_list_style_t ParseOrderedListStyleAttr(const char **attr,
                                               bool *has_explicit_style) {
  if (has_explicit_style)
    *has_explicit_style = false;
  if (!attr)
    return ORDERED_LIST_DECIMAL;
  for (int i = 0; attr[i]; i += 2) {
    const char *name = attr[i];
    const char *value = attr[i + 1];
    if (!value || !value[0])
      continue;
    if (AttrNameEqualsLocal(name, "type")) {
      if (AttrValueEqualsNoCase(value, "1")) {
        if (has_explicit_style)
          *has_explicit_style = true;
        return ORDERED_LIST_DECIMAL;
      }
      if (std::strcmp(value, "a") == 0) {
        if (has_explicit_style)
          *has_explicit_style = true;
        return ORDERED_LIST_LOWER_ALPHA;
      }
      if (std::strcmp(value, "A") == 0) {
        if (has_explicit_style)
          *has_explicit_style = true;
        return ORDERED_LIST_UPPER_ALPHA;
      }
      if (std::strcmp(value, "i") == 0) {
        if (has_explicit_style)
          *has_explicit_style = true;
        return ORDERED_LIST_LOWER_ROMAN;
      }
      if (std::strcmp(value, "I") == 0) {
        if (has_explicit_style)
          *has_explicit_style = true;
        return ORDERED_LIST_UPPER_ROMAN;
      }
    } else if (AttrNameEqualsLocal(name, "class")) {
      const std::string class_names = ToLowerAscii(value);
      if (ContainsClassToken(class_names, "orderedlistalpha")) {
        if (has_explicit_style)
          *has_explicit_style = true;
        return ORDERED_LIST_UPPER_ALPHA;
      }
    }
  }
  return ORDERED_LIST_DECIMAL;
}

ordered_list_style_t ResolveDefaultOrderedListStyle(unsigned int depth) {
  if (depth <= 1)
    return ORDERED_LIST_DECIMAL;
  if (depth == 2)
    return ORDERED_LIST_LOWER_ALPHA;
  return ORDERED_LIST_LOWER_ROMAN;
}

std::string BuildAlphabeticOrdinal(unsigned int ordinal, bool uppercase) {
  if (ordinal == 0)
    return std::string("0");
  std::string out;
  while (ordinal > 0) {
    ordinal--;
    const char base = uppercase ? 'A' : 'a';
    out.insert(out.begin(), (char)(base + (ordinal % 26)));
    ordinal /= 26;
  }
  return out;
}

std::string BuildRomanOrdinal(unsigned int ordinal, bool uppercase) {
  struct RomanEntry {
    unsigned int value;
    const char *digits;
  };
  static const RomanEntry kTable[] = {
      {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"},
      {90, "XC"},  {50, "L"},   {40, "XL"}, {10, "X"},  {9, "IX"},
      {5, "V"},    {4, "IV"},   {1, "I"},
  };

  if (ordinal == 0)
    return std::string("0");

  std::string out;
  for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
    while (ordinal >= kTable[i].value) {
      out += kTable[i].digits;
      ordinal -= kTable[i].value;
    }
  }

  if (!uppercase) {
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
      if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
      return (char)c;
    });
  }
  return out;
}

} // namespace

void ConfigureElementListSemantics(parsedata_t *p, const char **attr) {
  if (!p || p->stacksize == 0)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  p->list_marker_hidden_stack[current] = ParseListMarkerHiddenAttr(attr);

  if (p->stack[current] != TAG_OL)
    return;

  bool has_explicit_style = false;
  ordered_list_style_t style =
      ParseOrderedListStyleAttr(attr, &has_explicit_style);
  if (!has_explicit_style) {
    const unsigned int depth = GetActiveListDepth(p);
    style = ResolveDefaultOrderedListStyle(depth);
  }
  p->ordered_list_style_stack[current] = (u8)style;
}

context_t GetActiveListContext(const parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return TAG_NONE;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->stack[i] == TAG_UL || p->stack[i] == TAG_OL)
      return p->stack[i];
  }
  return TAG_NONE;
}

unsigned int GetActiveListDepth(const parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return 0;
  unsigned int depth = 0;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->stack[i] == TAG_UL || p->stack[i] == TAG_OL)
      depth++;
  }
  return depth;
}

ordered_list_style_t GetActiveOrderedListStyle(const parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return ORDERED_LIST_DECIMAL;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->stack[i] != TAG_OL)
      continue;
    return (ordered_list_style_t)p->ordered_list_style_stack[i];
  }
  return ORDERED_LIST_DECIMAL;
}

bool HasSuppressedListMarkerContext(const parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->list_marker_hidden_stack[i])
      return true;
  }
  return false;
}

bool IsInsideListItem(const parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return false;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->stack[i] == TAG_LI)
      return true;
  }
  return false;
}

bool HasPendingListItemContent(const parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return false;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->stack[i] == TAG_LI)
      return p->list_item_pending_stack[i];
  }
  return false;
}

void MarkCurrentListItemPending(parsedata_t *p, bool pending) {
  if (!p || p->stacksize == 0)
    return;
  const int current = (int)p->stacksize - 1;
  if (p->stack[current] != TAG_LI)
    return;
  p->list_item_pending_stack[current] = pending;
}

void ConsumePendingListItemContent(parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->stack[i] != TAG_LI)
      continue;
    p->list_item_pending_stack[i] = false;
    return;
  }
}

unsigned int AdvanceOrderedListOrdinal(parsedata_t *p) {
  if (!p || p->stacksize == 0)
    return 0;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->stack[i] != TAG_OL)
      continue;
    p->ordered_list_ordinal_stack[i]++;
    return p->ordered_list_ordinal_stack[i];
  }
  return 0;
}

std::string BuildOrderedListMarker(unsigned int ordinal) {
  return BuildOrderedListMarker(ordinal, ORDERED_LIST_DECIMAL);
}

std::string BuildOrderedListMarker(unsigned int ordinal,
                                   ordered_list_style_t style) {
  std::string prefix;
  switch (style) {
  case ORDERED_LIST_LOWER_ALPHA:
    prefix = BuildAlphabeticOrdinal(ordinal, false);
    break;
  case ORDERED_LIST_UPPER_ALPHA:
    prefix = BuildAlphabeticOrdinal(ordinal, true);
    break;
  case ORDERED_LIST_LOWER_ROMAN:
    prefix = BuildRomanOrdinal(ordinal, false);
    break;
  case ORDERED_LIST_UPPER_ROMAN:
    prefix = BuildRomanOrdinal(ordinal, true);
    break;
  case ORDERED_LIST_DECIMAL:
  default: {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u", ordinal);
    prefix = buffer;
    break;
  }
  }
  prefix.push_back('.');
  return prefix;
}

} // namespace book_xml_list_utils
