#pragma once

#include <stdint.h>

#include "reader/reader_controls.h"

// Data-driven reader control schemes.
//
// A scheme is a table of logical-button bitsets, one entry per ReaderAction.
// Tables are resolved through the active KeyMap (App::key), so the L/R and
// ZL/ZR handedness swap keeps working. Orientation picks the table group.
//
// Portrait note: the device is held rotated clockwise, so dleft is physically
// down on screen (forward) and dright is physically up (backward).

enum ControlScheme : uint8_t {
  CONTROL_SCHEME_DEFAULT = 0,
  CONTROL_SCHEME_ONE_HANDED = 1,
  CONTROL_SCHEME_COUNT
};

enum LogicalButton : uint8_t {
  LB_A, LB_B, LB_X, LB_Y, LB_L, LB_R, LB_ZL, LB_ZR,
  LB_DUP, LB_DDOWN, LB_DLEFT, LB_DRIGHT,
  LB_CUP, LB_CDOWN, LB_CLEFT, LB_CRIGHT,
  LB_START, LB_SELECT,
  LB_COUNT
};

#define LBM(b) (1u << (b))

enum ReaderAction : uint8_t {
  RA_PAGE_NEXT, RA_PAGE_PREV,
  RA_REPEAT_NEXT_DPAD, RA_REPEAT_PREV_DPAD,
  RA_REPEAT_NEXT_CIRCLE, RA_REPEAT_PREV_CIRCLE,
  RA_BOOKMARK_NEXT, RA_BOOKMARK_PREV,
  RA_LINK_NEXT, RA_LINK_PREV,
  RA_FIXED_PAGE_NEXT, RA_FIXED_PAGE_PREV,
  RA_FIXED_CHAPTER_NEXT, RA_FIXED_CHAPTER_PREV,
  RA_ZOOM_IN, RA_ZOOM_OUT,
  RA_BACK_TO_LIBRARY, RA_OPEN_SETTINGS,
  RA_FOCUS_HOLD, RA_LINK_ACTIVATE, RA_LINK_EXIT, RA_LINK_FOLLOW_IMAGE,
  RA_COLOR_CYCLE,
  RA_FIXED_SHOULDER_NEXT, RA_FIXED_SHOULDER_PREV,
  READER_ACTION_COUNT
};

struct SchemeTable {
  uint32_t buttons[READER_ACTION_COUNT]; // LogicalButton bitsets per action
};

// Default scheme, portrait (also used as the reference for the golden test).
static const SchemeTable kSchemeDefaultPortrait = {{
    /* RA_PAGE_NEXT */ LBM(LB_A) | LBM(LB_R) | LBM(LB_CDOWN) | LBM(LB_DDOWN) | LBM(LB_ZL),
    /* RA_PAGE_PREV */ LBM(LB_B) | LBM(LB_L) | LBM(LB_CUP) | LBM(LB_DUP) | LBM(LB_ZR),
    /* RA_REPEAT_NEXT_DPAD */ LBM(LB_DDOWN),
    /* RA_REPEAT_PREV_DPAD */ LBM(LB_DUP),
    /* RA_REPEAT_NEXT_CIRCLE */ LBM(LB_CDOWN),
    /* RA_REPEAT_PREV_CIRCLE */ LBM(LB_CUP),
    /* RA_BOOKMARK_NEXT */ LBM(LB_CLEFT) | LBM(LB_DLEFT),
    /* RA_BOOKMARK_PREV */ LBM(LB_CRIGHT) | LBM(LB_DRIGHT),
    /* RA_LINK_NEXT */ LBM(LB_DDOWN) | LBM(LB_DLEFT),
    /* RA_LINK_PREV */ LBM(LB_DUP) | LBM(LB_DRIGHT),
    /* RA_FIXED_PAGE_NEXT */ LBM(LB_DRIGHT),
    /* RA_FIXED_PAGE_PREV */ LBM(LB_DLEFT),
    /* RA_FIXED_CHAPTER_NEXT */ LBM(LB_DDOWN),
    /* RA_FIXED_CHAPTER_PREV */ LBM(LB_DUP),
    /* RA_ZOOM_IN */ LBM(LB_A),
    /* RA_ZOOM_OUT */ LBM(LB_B),
    /* RA_BACK_TO_LIBRARY */ LBM(LB_START),
    /* RA_OPEN_SETTINGS */ LBM(LB_SELECT),
    /* RA_FOCUS_HOLD */ LBM(LB_Y),
    /* RA_LINK_ACTIVATE */ LBM(LB_A),
    /* RA_LINK_EXIT */ LBM(LB_B) | LBM(LB_Y),
    /* RA_LINK_FOLLOW_IMAGE */ LBM(LB_X),
    /* RA_COLOR_CYCLE */ LBM(LB_X),
    /* RA_FIXED_SHOULDER_NEXT */ LBM(LB_R) | LBM(LB_ZL),
    /* RA_FIXED_SHOULDER_PREV */ LBM(LB_L) | LBM(LB_ZR),
}};

// Default scheme, landscape.
static const SchemeTable kSchemeDefaultLandscape = {{
    /* RA_PAGE_NEXT */ LBM(LB_A) | LBM(LB_R) | LBM(LB_CRIGHT) | LBM(LB_DRIGHT) | LBM(LB_ZL),
    /* RA_PAGE_PREV */ LBM(LB_B) | LBM(LB_L) | LBM(LB_CLEFT) | LBM(LB_DLEFT) | LBM(LB_ZR),
    /* RA_REPEAT_NEXT_DPAD */ LBM(LB_DRIGHT),
    /* RA_REPEAT_PREV_DPAD */ LBM(LB_DLEFT),
    /* RA_REPEAT_NEXT_CIRCLE */ LBM(LB_CRIGHT),
    /* RA_REPEAT_PREV_CIRCLE */ LBM(LB_CLEFT),
    /* RA_BOOKMARK_NEXT */ LBM(LB_CDOWN) | LBM(LB_DDOWN),
    /* RA_BOOKMARK_PREV */ LBM(LB_CUP) | LBM(LB_DUP),
    /* RA_LINK_NEXT */ LBM(LB_CDOWN) | LBM(LB_DDOWN),
    /* RA_LINK_PREV */ LBM(LB_CUP) | LBM(LB_DUP),
    /* RA_FIXED_PAGE_NEXT */ LBM(LB_DRIGHT),
    /* RA_FIXED_PAGE_PREV */ LBM(LB_DLEFT),
    /* RA_FIXED_CHAPTER_NEXT */ LBM(LB_DDOWN),
    /* RA_FIXED_CHAPTER_PREV */ LBM(LB_DUP),
    /* RA_ZOOM_IN */ LBM(LB_A),
    /* RA_ZOOM_OUT */ LBM(LB_B),
    /* RA_BACK_TO_LIBRARY */ LBM(LB_START),
    /* RA_OPEN_SETTINGS */ LBM(LB_SELECT),
    /* RA_FOCUS_HOLD */ LBM(LB_Y),
    /* RA_LINK_ACTIVATE */ LBM(LB_A),
    /* RA_LINK_EXIT */ LBM(LB_B) | LBM(LB_Y),
    /* RA_LINK_FOLLOW_IMAGE */ LBM(LB_X),
    /* RA_COLOR_CYCLE */ LBM(LB_X),
    /* RA_FIXED_SHOULDER_NEXT */ LBM(LB_R) | LBM(LB_ZL),
    /* RA_FIXED_SHOULDER_PREV */ LBM(LB_L) | LBM(LB_ZR),
}};

// One-handed, portrait. Same page turns as Default; the Y-only actions get
// a second D-pad key so the thumb never leaves the D-pad/Circle Pad.
//   focus_hold: Y | DLEFT (tap = bookmark, hold = link focus)
//   bookmark jump: Circle Pad left/right only (D-pad keys are taken)
//   link focus: DDOWN/DUP move, A | DRIGHT activate, L/R also leave focus
static const SchemeTable kSchemeOneHandedPortrait = {{
    /* RA_PAGE_NEXT */ LBM(LB_A) | LBM(LB_R) | LBM(LB_CDOWN) | LBM(LB_DDOWN) | LBM(LB_ZL),
    /* RA_PAGE_PREV */ LBM(LB_B) | LBM(LB_L) | LBM(LB_CUP) | LBM(LB_DUP) | LBM(LB_ZR),
    /* RA_REPEAT_NEXT_DPAD */ LBM(LB_DDOWN),
    /* RA_REPEAT_PREV_DPAD */ LBM(LB_DUP),
    /* RA_REPEAT_NEXT_CIRCLE */ LBM(LB_CDOWN),
    /* RA_REPEAT_PREV_CIRCLE */ LBM(LB_CUP),
    /* RA_BOOKMARK_NEXT */ LBM(LB_CLEFT),
    /* RA_BOOKMARK_PREV */ LBM(LB_CRIGHT),
    /* RA_LINK_NEXT */ LBM(LB_DDOWN),
    /* RA_LINK_PREV */ LBM(LB_DUP),
    /* RA_FIXED_PAGE_NEXT */ LBM(LB_DRIGHT),
    /* RA_FIXED_PAGE_PREV */ LBM(LB_DLEFT),
    /* RA_FIXED_CHAPTER_NEXT */ LBM(LB_DDOWN),
    /* RA_FIXED_CHAPTER_PREV */ LBM(LB_DUP),
    /* RA_ZOOM_IN */ LBM(LB_A),
    /* RA_ZOOM_OUT */ LBM(LB_B),
    /* RA_BACK_TO_LIBRARY */ LBM(LB_START),
    /* RA_OPEN_SETTINGS */ LBM(LB_SELECT),
    /* RA_FOCUS_HOLD */ LBM(LB_Y) | LBM(LB_DLEFT),
    /* RA_LINK_ACTIVATE */ LBM(LB_A) | LBM(LB_DRIGHT),
    /* RA_LINK_EXIT */ LBM(LB_B) | LBM(LB_Y) | LBM(LB_L) | LBM(LB_R),
    /* RA_LINK_FOLLOW_IMAGE */ LBM(LB_X),
    /* RA_COLOR_CYCLE */ LBM(LB_X),
    /* RA_FIXED_SHOULDER_NEXT */ LBM(LB_R) | LBM(LB_ZL),
    /* RA_FIXED_SHOULDER_PREV */ LBM(LB_L) | LBM(LB_ZR),
}};

// One-handed, landscape. Page turns already live on DRIGHT/DLEFT.
//   focus_hold: Y | DUP
//   bookmark jump: Circle Pad down/up only
//   link focus: DRIGHT/DLEFT move, A | DDOWN activate, L/R also leave focus
static const SchemeTable kSchemeOneHandedLandscape = {{
    /* RA_PAGE_NEXT */ LBM(LB_A) | LBM(LB_R) | LBM(LB_CRIGHT) | LBM(LB_DRIGHT) | LBM(LB_ZL),
    /* RA_PAGE_PREV */ LBM(LB_B) | LBM(LB_L) | LBM(LB_CLEFT) | LBM(LB_DLEFT) | LBM(LB_ZR),
    /* RA_REPEAT_NEXT_DPAD */ LBM(LB_DRIGHT),
    /* RA_REPEAT_PREV_DPAD */ LBM(LB_DLEFT),
    /* RA_REPEAT_NEXT_CIRCLE */ LBM(LB_CRIGHT),
    /* RA_REPEAT_PREV_CIRCLE */ LBM(LB_CLEFT),
    /* RA_BOOKMARK_NEXT */ LBM(LB_CDOWN),
    /* RA_BOOKMARK_PREV */ LBM(LB_CUP),
    /* RA_LINK_NEXT */ LBM(LB_DRIGHT),
    /* RA_LINK_PREV */ LBM(LB_DLEFT),
    /* RA_FIXED_PAGE_NEXT */ LBM(LB_DRIGHT),
    /* RA_FIXED_PAGE_PREV */ LBM(LB_DLEFT),
    /* RA_FIXED_CHAPTER_NEXT */ LBM(LB_DDOWN),
    /* RA_FIXED_CHAPTER_PREV */ LBM(LB_DUP),
    /* RA_ZOOM_IN */ LBM(LB_A),
    /* RA_ZOOM_OUT */ LBM(LB_B),
    /* RA_BACK_TO_LIBRARY */ LBM(LB_START),
    /* RA_OPEN_SETTINGS */ LBM(LB_SELECT),
    /* RA_FOCUS_HOLD */ LBM(LB_Y) | LBM(LB_DUP),
    /* RA_LINK_ACTIVATE */ LBM(LB_A) | LBM(LB_DDOWN),
    /* RA_LINK_EXIT */ LBM(LB_B) | LBM(LB_Y) | LBM(LB_L) | LBM(LB_R),
    /* RA_LINK_FOLLOW_IMAGE */ LBM(LB_X),
    /* RA_COLOR_CYCLE */ LBM(LB_X),
    /* RA_FIXED_SHOULDER_NEXT */ LBM(LB_R) | LBM(LB_ZL),
    /* RA_FIXED_SHOULDER_PREV */ LBM(LB_L) | LBM(LB_ZR),
}};

inline const SchemeTable &GetSchemeTable(ControlScheme scheme, bool landscape)
{
  if (scheme == CONTROL_SCHEME_ONE_HANDED)
    return landscape ? kSchemeOneHandedLandscape : kSchemeOneHandedPortrait;
  return landscape ? kSchemeDefaultLandscape : kSchemeDefaultPortrait;
}

// Clamp an untrusted (prefs) value to a valid scheme id.
inline ControlScheme ClampControlScheme(int value)
{
  return (value >= 0 && value < (int)CONTROL_SCHEME_COUNT)
             ? (ControlScheme)value
             : CONTROL_SCHEME_DEFAULT;
}

// Scheme selected in prefs for the given orientation group; Default without prefs.
template <typename PrefsT>
inline ControlScheme SelectedControlScheme(const PrefsT *prefs, bool landscape)
{
  if (!prefs)
    return CONTROL_SCHEME_DEFAULT;
  return ClampControlScheme(landscape ? prefs->control_scheme_landscape
                                      : prefs->control_scheme_portrait);
}

// Resolve a LogicalButton bitset to the physical masks of the active KeyMap.
template <typename KeyMap>
inline uint32_t ResolveButtons(uint32_t lb, const KeyMap &key)
{
  uint32_t m = 0;
  if (lb & LBM(LB_A)) m |= key.a;
  if (lb & LBM(LB_B)) m |= key.b;
  if (lb & LBM(LB_X)) m |= key.x;
  if (lb & LBM(LB_Y)) m |= key.y;
  if (lb & LBM(LB_L)) m |= key.l;
  if (lb & LBM(LB_R)) m |= key.r;
  if (lb & LBM(LB_ZL)) m |= key.zl;
  if (lb & LBM(LB_ZR)) m |= key.zr;
  if (lb & LBM(LB_DUP)) m |= key.dup;
  if (lb & LBM(LB_DDOWN)) m |= key.ddown;
  if (lb & LBM(LB_DLEFT)) m |= key.dleft;
  if (lb & LBM(LB_DRIGHT)) m |= key.dright;
  if (lb & LBM(LB_CUP)) m |= key.up;
  if (lb & LBM(LB_CDOWN)) m |= key.down;
  if (lb & LBM(LB_CLEFT)) m |= key.left;
  if (lb & LBM(LB_CRIGHT)) m |= key.right;
  if (lb & LBM(LB_START)) m |= key.start;
  if (lb & LBM(LB_SELECT)) m |= key.select;
  return m;
}

template <typename KeyMap>
inline ReaderControls BuildReaderControls(ControlScheme scheme, bool landscape,
                                          const KeyMap &key)
{
  const SchemeTable &t = GetSchemeTable(scheme, landscape);
  const auto r = [&](ReaderAction a) { return ResolveButtons(t.buttons[a], key); };
  ReaderControls c;
  c.page_next = r(RA_PAGE_NEXT);
  c.page_prev = r(RA_PAGE_PREV);
  c.page_repeat_next_dpad = r(RA_REPEAT_NEXT_DPAD);
  c.page_repeat_prev_dpad = r(RA_REPEAT_PREV_DPAD);
  c.page_repeat_next_circle = r(RA_REPEAT_NEXT_CIRCLE);
  c.page_repeat_prev_circle = r(RA_REPEAT_PREV_CIRCLE);
  c.bookmark_next = r(RA_BOOKMARK_NEXT);
  c.bookmark_prev = r(RA_BOOKMARK_PREV);
  c.link_next = r(RA_LINK_NEXT);
  c.link_prev = r(RA_LINK_PREV);
  c.fixed_page_next = r(RA_FIXED_PAGE_NEXT);
  c.fixed_page_prev = r(RA_FIXED_PAGE_PREV);
  c.fixed_chapter_next = r(RA_FIXED_CHAPTER_NEXT);
  c.fixed_chapter_prev = r(RA_FIXED_CHAPTER_PREV);
  c.zoom_in = r(RA_ZOOM_IN);
  c.zoom_out = r(RA_ZOOM_OUT);
  c.back_to_library = r(RA_BACK_TO_LIBRARY);
  c.open_settings = r(RA_OPEN_SETTINGS);
  c.focus_hold = r(RA_FOCUS_HOLD);
  c.link_activate = r(RA_LINK_ACTIVATE);
  c.link_exit = r(RA_LINK_EXIT);
  c.link_follow_image = r(RA_LINK_FOLLOW_IMAGE);
  c.color_cycle = r(RA_COLOR_CYCLE);
  c.fixed_shoulder_next = r(RA_FIXED_SHOULDER_NEXT);
  c.fixed_shoulder_prev = r(RA_FIXED_SHOULDER_PREV);
  return c;
}

template <typename KeyMap>
inline ReaderControls BuildPortraitControls(const KeyMap &key)
{
  return BuildReaderControls(CONTROL_SCHEME_DEFAULT, false, key);
}

template <typename KeyMap>
inline ReaderControls BuildLandscapeControls(const KeyMap &key)
{
  return BuildReaderControls(CONTROL_SCHEME_DEFAULT, true, key);
}

// Conflict check over resolved masks. Actions that can fire in the same input
// context must not share a key:
//   Reading:   page turns, bookmark jumps, focus_hold, color cycle, back, settings
//   LinkFocus: link moves, activate, exit, follow image, back, settings
//   Fixed:     page/chapter/shoulder turns, zoom, back, settings
// The repeat keys must also be a subset of their page keys, and the actions a
// context needs to be usable must be bound. Returns false and optionally sets
// *why to a static description of the first violation.
namespace reader_scheme_detail {

inline bool Disjoint(const uint32_t *masks, unsigned n)
{
  uint32_t seen = 0;
  for (unsigned i = 0; i < n; ++i) {
    if (masks[i] & seen)
      return false;
    seen |= masks[i];
  }
  return true;
}

} // namespace reader_scheme_detail

inline bool ValidateScheme(const ReaderControls &c, const char **why = 0)
{
  const char *dummy;
  if (!why)
    why = &dummy;

  const uint32_t reading[] = {c.page_next, c.page_prev, c.bookmark_next,
                              c.bookmark_prev, c.focus_hold, c.color_cycle,
                              c.back_to_library, c.open_settings};
  const uint32_t link_focus[] = {c.link_next, c.link_prev, c.link_activate,
                                 c.link_exit, c.link_follow_image,
                                 c.back_to_library, c.open_settings};
  const uint32_t fixed[] = {c.fixed_page_next, c.fixed_page_prev,
                            c.fixed_chapter_next, c.fixed_chapter_prev,
                            c.zoom_in, c.zoom_out, c.fixed_shoulder_next,
                            c.fixed_shoulder_prev, c.back_to_library,
                            c.open_settings};
  const unsigned nr = sizeof(reading) / sizeof(reading[0]);
  const unsigned nl = sizeof(link_focus) / sizeof(link_focus[0]);
  const unsigned nf = sizeof(fixed) / sizeof(fixed[0]);

  for (unsigned i = 0; i < nr; ++i)
    if (!reading[i]) { *why = "unbound reading action"; return false; }
  for (unsigned i = 0; i < nl; ++i)
    if (!link_focus[i]) { *why = "unbound link focus action"; return false; }
  for (unsigned i = 0; i < nf; ++i)
    if (!fixed[i]) { *why = "unbound fixed layout action"; return false; }
  if (!c.page_repeat_next_dpad || !c.page_repeat_prev_dpad ||
      !c.page_repeat_next_circle || !c.page_repeat_prev_circle) {
    *why = "unbound page repeat key";
    return false;
  }

  if (!reader_scheme_detail::Disjoint(reading, nr)) {
    *why = "reading actions share a key";
    return false;
  }
  if (!reader_scheme_detail::Disjoint(link_focus, nl)) {
    *why = "link focus actions share a key";
    return false;
  }
  if (!reader_scheme_detail::Disjoint(fixed, nf)) {
    *why = "fixed layout actions share a key";
    return false;
  }
  if ((c.page_repeat_next_dpad | c.page_repeat_next_circle) & ~c.page_next) {
    *why = "next repeat keys are not page_next keys";
    return false;
  }
  if ((c.page_repeat_prev_dpad | c.page_repeat_prev_circle) & ~c.page_prev) {
    *why = "prev repeat keys are not page_prev keys";
    return false;
  }
  return true;
}
