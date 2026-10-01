#include "settings/prefs.h"
#include "settings/prefs_file_utils.h"
#include "shared/orientation_utils.h"
#include "expat.h"
#include "formats/common/xml_parse_utils.h"
#include "library/browser_view_utils.h"
#include "settings/prefs_style_value_utils.h"
#include "shared/utf8_utils.h"
#include "ui/text_limits.h"
#include <sys/param.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
typedef uint8_t u8;
typedef uint16_t u16;
#include "shared/text_token_constants.h"
#define DBG_LOGF(...) ((void)0)
static uint64_t now=0;
uint64_t osGetTime() { return now; }
namespace paths {
static std::string prefs_path;
std::string GetPrefsFile() { return prefs_path; }
std::string GetPrefsTempFile() { return prefs_path + ".tmp"; }
std::string GetPrefsBackupFile() { return prefs_path + ".bak"; }
}
// Host collaborators only store values or record external requests. XML,
// persistence, migration and restoration policy execute the actual Prefs code.
struct Text {
  struct { int top=0, left=0, bottom=0, right=0; } margin;
  int color=0, size=12, linespacing=0;
  std::string fonts[7], fallbacks[4], fontdir;
  int GetColorMode() const { return color; }
  void SetColorMode(int value) { color=value; }
  int GetPixelSize() const { return size; }
  void SetPixelSize(u8 value) { size=value; }
  std::string GetFontFile(int style) const { return fonts[style]; }
  void SetFontFile(char *value, u8 style) { fonts[style]=value; }
  std::string GetFallbackFontFile(int slot) const { return fallbacks[slot]; }
  void SetFallbackFontFile(int slot, const char *value) { fallbacks[slot]=value; }
  void ClearFallbackFonts() { for (auto &font : fallbacks) font.clear(); }
  void AutoLoadFallbackFonts() {}
  void SetFontDir(const std::string &value) { fontdir=value; }
};
struct Book {
  std::string folder, filename;
  int position=0; bool browser_folder=false, wrap=false;
  uint32_t last_opened=0;
  int FontSize=-1, LineSpacing=-1, ParagraphSpacing=-1;
  int PublisherTextIndent=-1, PublisherBlockMargins=-1, PublisherHorizontalMargins=-1;
  std::list<u16> bookmarks;
  bool IsBrowserFolder() const { return browser_folder; }
  int GetPosition() const { return position; }
  void SetPosition(int value) { position=value; }
  bool GetMobiLineWrapFix() const { return wrap; }
  void SetMobiLineWrapFix(bool value) { wrap=value; }
#define STYLE_STORAGE(name) \
  int GetStyle##name##Override() const { return name; } \
  void SetStyle##name##Override(int value) { name=value; }
  STYLE_STORAGE(FontSize)
  STYLE_STORAGE(LineSpacing)
  STYLE_STORAGE(ParagraphSpacing)
  STYLE_STORAGE(PublisherTextIndent)
  STYLE_STORAGE(PublisherBlockMargins)
  STYLE_STORAGE(PublisherHorizontalMargins)
#undef STYLE_STORAGE
  uint32_t GetLastOpenedTime() const { return last_opened; }
  void SetLastOpenedTime(uint32_t value) { last_opened=value; }
  std::list<u16> &GetBookmarks() { return bookmarks; }
  const std::list<u16> &GetBookmarks() const { return bookmarks; }
  const char *GetFolderName() const { return folder.c_str(); }
  const char *GetFileName() const { return filename.c_str(); }
};
struct App {
  std::unique_ptr<Text> ts;
  unsigned char portrait_orientation=0, orientation=0;
  bool landscape=false, publisher_text_indent=false, publisher_block_margins=false, publisher_horizontal_margins=false;
  int colorMode=0, reader_font_size=12, paraindent=0, paraspacing=0, reader_line_spacing=0, reopen=0;
  std::string fontdir, bookdir, requested_folder, requested_file;
  std::vector<Book *> books;
  Book *current=nullptr, *selected=nullptr, *restore_result=nullptr;
  struct { int l=1, r=2, zl=3, zr=4; } key;
  int BookCount() const { return (int)books.size(); }
  Book *GetCurrentBook() const { return current; }
  void SetCurrentBook(Book *book) { current=book; }
  void SetSelectedBook(Book *book) { selected=book; }
  Book *RestoreSavedBookSelection(const char *folder, const char *file) {
    requested_folder=folder; requested_file=file; return restore_result;
  }
  static App *GetInstance() { return nullptr; }
};
static void UiButtonSkin_SetColorMode(int) {}
struct parsedata_t { Prefs *prefs=nullptr; Text *ts=nullptr; App *reporter=nullptr; Book *book=nullptr; };
static void parse_init(parsedata_t *data) { *data=parsedata_t(); }
namespace xml { namespace book {
int unknown(void *, const XML_Char *, XML_Encoding *) { return XML_STATUS_ERROR; }
} }
#define PARSEBUFSIZE (1024 * 64)
#define RECENT_BOOKS_TRACE 0
namespace font_config_utils { const char *FontPrefAttrForStyle(u8); bool StyleFromFontPrefAttr(const char *, u8 *); }
#include "prefs_read_callbacks.inc"
#include "prefs_deferred.inc"
static std::string ReadCurrent() {
  FILE *f=fopen(paths::GetPrefsFile().c_str(), "rb"); assert(f);
  std::string contents; char buffer[256]; size_t n;
  while ((n=fread(buffer, 1, sizeof(buffer), f)) != 0) contents.append(buffer, n);
  assert(!ferror(f) && fclose(f) == 0); return contents;
}
struct PersistedBook {
  std::string filename, folder, page, current;
  std::vector<std::string> bookmarks;
};
struct PersistedBooks {
  std::vector<PersistedBook> records;
  size_t active=std::string::npos;
};
static std::string Attribute(const XML_Char **attrs, const char *name) {
  for (size_t i=0; attrs[i]; i+=2)
    if (!strcmp(attrs[i], name)) return attrs[i+1];
  return std::string();
}
static void XMLCALL StartRecord(void *data, const XML_Char *name,
                                const XML_Char **attrs) {
  PersistedBooks &books=*static_cast<PersistedBooks *>(data);
  if (!strcmp(name, "book")) {
    assert(books.active == std::string::npos);
    PersistedBook record;
    record.filename=Attribute(attrs, "file");
    record.folder=Attribute(attrs, "folder");
    record.page=Attribute(attrs, "page");
    record.current=Attribute(attrs, "current");
    books.records.push_back(record);
    books.active=books.records.size()-1;
  } else if (!strcmp(name, "bookmark")) {
    assert(books.active != std::string::npos);
    books.records[books.active].bookmarks.push_back(Attribute(attrs, "page"));
  }
}
static void XMLCALL EndRecord(void *data, const XML_Char *name) {
  if (!strcmp(name, "book"))
    static_cast<PersistedBooks *>(data)->active=std::string::npos;
}
static const PersistedBook &FindRecord(const PersistedBooks &books,
                                     const char *filename, const char *folder) {
  const PersistedBook *found=nullptr;
  for (size_t i=0; i<books.records.size(); ++i) {
    const PersistedBook &record=books.records[i];
    if (record.filename == filename && record.folder == folder) {
      assert(!found); found=&record;
    }
  }
  assert(found); return *found;
}
static void WriteXml(const std::string &xml) {
  FILE *f=fopen(paths::GetPrefsFile().c_str(), "wb"); assert(f);
  assert(fwrite(xml.data(), 1, xml.size(), f) == xml.size() && fclose(f) == 0);
}
static void TestReadContracts() {
  const int inputs[]={0, 7, 8, 12, 20, 21, 255};
  const int expected[]={8, 8, 8, 12, 20, 20, 20};
  for (size_t i=0; i<sizeof(inputs)/sizeof(inputs[0]); ++i) {
    const std::string size=std::to_string(inputs[i]);
    WriteXml("<dslibris><font size=\"" + size + "\"/><book file=\"size.epub\" fontSize=\"" + size + "\"/></dslibris>");
    App app; app.ts.reset(new Text()); Book book; book.filename="size.epub"; app.books={&book}; Prefs prefs(&app);
    assert(prefs.Read() == 0);
    assert(app.reader_font_size == expected[i] && app.ts->GetPixelSize() == expected[i]);
    assert(book.FontSize == expected[i]);
  }
  struct Migration { const char *attrs; int book_sides; bool global_sides; };
  const Migration cases[]={
    {"publisherBlockMargins=\"0\"", 0, false},
    {"publisherBlockMargins=\"1\"", 1, true},
    {"", -1, false},
    {"publisherHorizontalMargins=\"-1\" publisherBlockMargins=\"0\"", -1, true},
    {"publisherHorizontalMargins=\"0\" publisherBlockMargins=\"1\"", 0, false},
    {"publisherHorizontalMargins=\"1\" publisherBlockMargins=\"0\"", 1, true},
  };
  for (const auto &row : cases) {
    WriteXml(std::string("<dslibris><paragraph ") + row.attrs + "/><book file=\"legacy.epub\" " + row.attrs + "/></dslibris>");
    App app; app.ts.reset(new Text()); Book book; book.filename="legacy.epub"; app.books={&book}; Prefs prefs(&app);
    assert(prefs.Read() == 0 && book.PublisherHorizontalMargins == row.book_sides);
    assert(app.publisher_horizontal_margins == row.global_sides);
  }
  // Deferred restoration owns bookmarks even when the library is not loaded.
  WriteXml("<dslibris><book file=\"A &amp; B.epub\" folder=\"books/nested\" page=\"6\" current=\"1\"><bookmark page=\"3\"/></book></dslibris>");
  App app; app.ts.reset(new Text()); Book book; app.restore_result=&book; Prefs prefs(&app);
  assert(prefs.Read() == 0 && prefs.ApplyPendingCurrentBookRestore());
  assert(app.requested_file == "A & B.epub" && app.requested_folder == "books/nested");
  assert(book.position == 5 && book.bookmarks == std::list<u16>{2});
  assert(!prefs.ApplyPendingCurrentBookRestore() && book.bookmarks.size() == 1);
  assert(prefs.Read() == 0); app.restore_result=nullptr;
  assert(!prefs.ApplyPendingCurrentBookRestore());
  app.restore_result=&book; assert(!prefs.ApplyPendingCurrentBookRestore());
  // Real stream parser rejects malformed input; absent main file uses backup.
  WriteXml("<dslibris><font></dslibris>"); assert(prefs.Read() != 0);
  WriteXml("<dslibris><option time24h=\"0\"/></dslibris>");
  assert(rename(paths::GetPrefsFile().c_str(), paths::GetPrefsBackupFile().c_str()) == 0);
  Prefs backup(&app); assert(backup.Read() == 0 && !backup.time24h);
  assert(remove(paths::GetPrefsBackupFile().c_str()) == 0);
  assert(backup.Read() == 255 && !backup.ApplyPendingCurrentBookRestore());
}
int main() {
  const char *dir=getenv("PREFS_TEST_DIR"); assert(dir);
  paths::prefs_path=std::string(dir) + "/prefs.xml";
  App owner; Prefs p(&owner);
  assert(!p.FlushPendingWrite(true));
  // Failed temp-open must stay dirty and wait five seconds before retrying.
  assert(mkdir(paths::GetPrefsTempFile().c_str(), 0700) == 0);
  p.RequestWrite(); now=1999; assert(!p.FlushPendingWrite());
  now=2000; assert(p.FlushPendingWrite());
  assert(access(paths::GetPrefsFile().c_str(), F_OK) != 0);
  now=2001; assert(!p.FlushPendingWrite());
  assert(rmdir(paths::GetPrefsTempFile().c_str()) == 0);
  now=6999; assert(!p.FlushPendingWrite());
  now=7000; assert(p.FlushPendingWrite());
  const std::string first=ReadCurrent();
  assert(first.find("swapshoulder=\"0\"") != std::string::npos);
  assert(first.find("time24h=\"1\"") != std::string::npos);
  assert(!p.FlushPendingWrite(true));

  // Later changes restart the coalescing delay and persist their latest value.
  p.swapshoulder=true; now=8000; p.RequestWrite();
  now=8300; p.RequestWrite(); now=10299; assert(!p.FlushPendingWrite());
  assert(ReadCurrent() == first);
  now=10300; assert(p.FlushPendingWrite());
  const std::string second=ReadCurrent();
  assert(second.find("swapshoulder=\"1\"") != std::string::npos);
  assert(second.find("time24h=\"1\"") != std::string::npos);
  assert(!p.FlushPendingWrite());

  // Commit failure must preserve the old file and keep the retry scheduled.
  assert(remove(paths::GetPrefsBackupFile().c_str()) == 0);
  assert(mkdir(paths::GetPrefsBackupFile().c_str(), 0700) == 0);
  const std::string blocker=paths::GetPrefsBackupFile() + "/block";
  FILE *f=fopen(blocker.c_str(), "wb"); assert(f && fclose(f) == 0);
  p.time24h=false; now=11000; p.RequestWrite(); now=13000;
  assert(p.FlushPendingWrite() && ReadCurrent() == second);
  assert(!p.FlushPendingWrite());
  assert(remove(blocker.c_str()) == 0 && rmdir(paths::GetPrefsBackupFile().c_str()) == 0);
  now=17999; assert(!p.FlushPendingWrite());
  now=18000; assert(p.FlushPendingWrite());
  const std::string retried=ReadCurrent();
  assert(retried.find("swapshoulder=\"1\"") != std::string::npos);
  assert(retried.find("time24h=\"0\"") != std::string::npos);
  assert(!p.FlushPendingWrite(true));
  // A forced flush bypasses the normal coalescing wait.
  p.swapshoulder=false; p.RequestWrite(); assert(p.FlushPendingWrite(true));
  const std::string forced=ReadCurrent();
  assert(forced.find("swapshoulder=\"0\"") != std::string::npos);
  assert(forced.find("time24h=\"0\"") != std::string::npos);
  assert(!p.FlushPendingWrite(true));
  // Browser folder changes replace the visible library. Saved progress from
  // the previous folder must remain in independently parsed writer output.
  Book nested; nested.folder="books/series"; nested.filename="novel.epub";
  nested.position=79; nested.bookmarks.push_back(12);
  nested.FontSize=18; nested.LineSpacing=5; nested.ParagraphSpacing=2;
  nested.PublisherTextIndent=1; nested.PublisherBlockMargins=0;
  nested.PublisherHorizontalMargins=1; nested.wrap=true; nested.last_opened=1234;
  owner.ts.reset(new Text()); owner.ts->margin.bottom=24;
  owner.reader_font_size=16; owner.ts->SetPixelSize(16);
  owner.reader_line_spacing=3; owner.ts->linespacing=3;
  p.browser_view_mode=BROWSER_VIEW_LIST; p.fixed_layout_rtl=true;
  p.circle_pad_page_turn=false; p.library_sort_mode=LIBRARY_SORT_RECENT;
  owner.books={&nested}; owner.current=&nested;
  assert(p.Write() == 0);
  Book root_book; root_book.folder="books"; root_book.filename="other.epub";
  root_book.position=3; owner.books={&root_book}; owner.current=&root_book;
  assert(p.Write() == 0);
  const std::string folders=ReadCurrent();
  PersistedBooks saved;
  XML_Parser parser=XML_ParserCreate(nullptr); assert(parser);
  XML_SetUserData(parser, &saved);
  XML_SetElementHandler(parser, StartRecord, EndRecord);
  assert(XML_Parse(parser, folders.data(), (int)folders.size(), XML_TRUE) == XML_STATUS_OK);
  XML_ParserFree(parser);
  assert(saved.records.size() == 2 && saved.active == std::string::npos);
  const PersistedBook &nested_record=FindRecord(saved, "novel.epub", "books/series");
  assert(nested_record.page == "80" && nested_record.current.empty());
  assert(nested_record.bookmarks.size() == 1 && nested_record.bookmarks[0] == "13");
  const PersistedBook &root_record=FindRecord(saved, "other.epub", "books");
  assert(root_record.page == "4" && root_record.current == "1");
  assert(root_record.bookmarks.empty());
  // Read the saved file into a fresh application/preferences session. The
  // current book is restored only after the external library lookup succeeds.
  App restarted; restarted.ts.reset(new Text());
  Book loaded_nested; loaded_nested.folder=nested.folder; loaded_nested.filename=nested.filename;
  Book loaded_root; loaded_root.folder=root_book.folder; loaded_root.filename=root_book.filename;
  restarted.books={&loaded_nested}; restarted.restore_result=&loaded_root;
  Prefs restored(&restarted);
  assert(restored.Read() == 0 && restarted.current == nullptr);
  assert(!restored.time24h && restored.browser_view_mode == BROWSER_VIEW_LIST);
  assert(restored.fixed_layout_rtl && !restored.circle_pad_page_turn);
  assert(restored.library_sort_mode == LIBRARY_SORT_RECENT);
  assert(restarted.reader_font_size == 16 && restarted.ts->GetPixelSize() == 16);
  assert(restarted.reader_line_spacing == 3 && restarted.ts->linespacing == 3);
  assert(restarted.ts->margin.bottom == 24);
  assert(loaded_nested.position == 79 && loaded_nested.bookmarks == std::list<u16>{12});
  assert(loaded_nested.FontSize == 18 && loaded_nested.LineSpacing == 5);
  assert(loaded_nested.ParagraphSpacing == 2 && loaded_nested.PublisherTextIndent == 1);
  assert(loaded_nested.PublisherBlockMargins == 0 && loaded_nested.PublisherHorizontalMargins == 1);
  assert(loaded_nested.wrap && loaded_nested.last_opened == 1234);
  assert(restored.ApplyPendingCurrentBookRestore());
  assert(restarted.current == &loaded_root && restarted.selected == &loaded_root);
  assert(restarted.requested_folder == "books" && restarted.requested_file == "other.epub");
  assert(loaded_root.position == 3 && loaded_root.FontSize == -1);
  assert(!restored.ApplyPendingCurrentBookRestore());
  Book later_nested; later_nested.folder=nested.folder; later_nested.filename=nested.filename;
  restored.ApplySavedBookState(&later_nested);
  assert(later_nested.position == 79 && later_nested.bookmarks == std::list<u16>{12});
  assert(later_nested.FontSize == 18 && later_nested.last_opened == 1234);
  Book wrong_folder; wrong_folder.folder="books/another"; wrong_folder.filename=nested.filename;
  restored.ApplySavedBookState(&wrong_folder);
  assert(wrong_folder.position == 0 && wrong_folder.bookmarks.empty() && wrong_folder.FontSize == -1);
  TestReadContracts();
  puts("PASS: real preference save/read/restart, retry, migration and folder restoration");
}
