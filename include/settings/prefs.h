/*
    3dslibris - prefs.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Preferences persistence API (read/apply/write).
    - Stores input/display options and per-book state in XML prefs file.
*/

#pragma once

#include "library/browser_view_mode.h"
#include "library/library_sort_mode.h"

#include <stdint.h>
#include <unordered_map>
#include <string>
#include <vector>

class App;
class Book;

class Prefs {
public:
  Prefs(App *app);
  ~Prefs();
  void Apply();
  int Read();
  int Write();
  void ClearPendingCurrentBookRestore();
  void SetPendingCurrentBookRestore(const char *folder, const char *filename,
                                    int position,
                                    bool mobi_line_wrap_fix,
                                    int style_font_size,
                                    int style_line_spacing,
                                    int style_paragraph_spacing,
                                    int style_publisher_text_indent,
                                    int style_publisher_block_margins);
  void AddPendingCurrentBookBookmark(uint16_t page);
  void EndPendingCurrentBookRestoreEntry();
  bool ApplyPendingCurrentBookRestore();
  void RememberSavedLastOpened(const char *folder, const char *filename,
                               uint32_t last_opened);
  void ApplySavedBookState(Book *book) const;
  App *GetApp() const { return app; }
  long modtime;
  bool swapshoulder;
  bool time24h;
  BrowserViewMode browser_view_mode;
  bool fixed_layout_rtl;
  bool circle_pad_page_turn;
  LibrarySortMode library_sort_mode;

private:
  App *app;
  bool pending_current_book_restore;
  bool collecting_pending_current_book;
  std::string pending_current_folder;
  std::string pending_current_filename;
  int pending_current_position;
  bool pending_current_mobi_line_wrap_fix;
  int pending_current_style_font_size;
  int pending_current_style_line_spacing;
  int pending_current_style_paragraph_spacing;
  int pending_current_style_publisher_text_indent;
  int pending_current_style_publisher_block_margins;
  std::vector<uint16_t> pending_current_bookmarks;
  std::unordered_map<std::string, uint32_t> last_opened_by_book_key;
  static std::string MakeBookKey(const char *folder, const char *filename);
  void Init();
};
