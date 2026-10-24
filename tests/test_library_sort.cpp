#include "app/library_controller.h"
#include "library/library_sort_mode.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

// State collaborators; ordering and filesystem access come from production.
struct Book {
  std::string folder, filename, title, author;
  bool is_folder = false, missing_name = false;
  uint32_t last_opened = 0;
  bool IsBrowserFolder() const { return is_folder; }
  const char *GetFolderName() const { return folder.c_str(); }
  const char *GetFileName() const { return missing_name ? nullptr : filename.c_str(); }
  const char *GetTitle() const { return title.c_str(); }
  const std::string &GetAuthor() const { return author; }
  uint32_t GetLastOpenedTime() const { return last_opened; }
};
struct Prefs { LibrarySortMode library_sort_mode = LIBRARY_SORT_DATE_MODIFIED; };
struct App { Prefs settings; Prefs *prefs = &settings; std::vector<Book *> books; };
LibraryController::LibraryController(App &app) : app_(app), inside_folder_(false) {}

static std::map<std::string, int> stats;
static bool reject_snapshot = false;
static int rejected_snapshots = 0;
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  if (reject_snapshot) { ++rejected_snapshots; return nullptr; }
  return ::operator new[](size);
}
static int CountedStat(const char *path, struct stat *result) {
  ++stats[path];
  return ::stat(path, result);
}
static void Expect(bool condition, const char *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
// Interpose only the syscall boundary, retaining its actual result.
#define stat(...) CountedStat(__VA_ARGS__)
#include "library_sort.inc"
#undef stat

static void SetMtime(const Book &book, time_t value) {
  const std::string path = book.folder + "/" + book.filename;
  struct utimbuf times = {value, value};
  Expect(utime(path.c_str(), &times) == 0, "set actual fixture mtime");
}
static void MakeFile(Book *book, const std::string &root, const std::string &name, time_t value) {
  book->folder = root; book->filename = name; book->title = name;
  FILE *file = fopen((root + "/" + name).c_str(), "wb");
  Expect(file != nullptr, "create actual fixture book"); fclose(file);
  SetMtime(*book, value);
}
int main() {
  const std::string root = std::getenv("TEST_LIBRARY_SORT_DIR");
  App app; LibraryController controller(app);
  Book books[64], folder, absent, unnamed;
  folder.is_folder = true; folder.folder = root; folder.filename = "folder";
  folder.title = "Folder";
  Expect(mkdir((root + "/folder").c_str(), 0700) == 0, "create actual folder");
  absent.folder = root; absent.filename = "missing.epub"; absent.title = "Missing";
  unnamed.folder = root; unnamed.missing_name = true; unnamed.title = "Unnamed";
  app.books = {&absent, &folder, &unnamed};
  for (int i = 0; i < 64; ++i) {
    char name[32]; snprintf(name, sizeof(name), "book-%02d.epub", i);
    MakeFile(&books[i], root, name, 1000 + i);
    app.books.push_back(&books[i]);
  }
  controller.SortBooks();
  Expect(stats.size() == 65, "only ordinary paths are inspected (including stat failure)");
  for (const auto &entry : stats)
    Expect(entry.second == 1, "date sort calls stat once per path, outside comparator");
  Expect(app.books.front() == &folder, "folders remain before files");
  for (int i = 0; i < 64; ++i)
    Expect(app.books[i + 1] == &books[63 - i], "dates are newest first");
  Expect((app.books[65] == &absent && app.books[66] == &unnamed) ||
         (app.books[65] == &unnamed && app.books[66] == &absent),
         "missing path/name retain date-zero behavior without a new tie key");
  stats.clear(); SetMtime(books[0], 2000); controller.SortBooks();
  Expect(app.books[1] == &books[0], "each sort observes updated filesystem dates");
  for (const auto &entry : stats) Expect(entry.second == 1, "refresh still has one stat per path");

  // Two equal dates with names/titles in reverse order protect the existing
  // equivalence: do not add an alphabetical or pointer tie-breaker.
  SetMtime(books[0], 1000); SetMtime(books[1], 1000);
  books[1].title = "Zulu"; books[0].title = "Alpha";
  app.books = {&books[1], &books[0]}; stats.clear(); controller.SortBooks();
  Expect(app.books[0] == &books[1] && app.books[1] == &books[0], "equal dates keep existing small-range tie behavior");
  Expect(stats.size() == 2 && stats.begin()->second == 1 && stats.rbegin()->second == 1,
         "equal dates are also read once");
  app.books = {&books[0], &books[63], &folder}; stats.clear();
  const std::vector<Book *> before_failure = app.books;
  reject_snapshot = true; controller.SortBooks(); reject_snapshot = false;
  Expect(rejected_snapshots == 1 && app.books == before_failure && stats.empty(),
         "snapshot allocation failure preserves the prior order without I/O");
  controller.SortBooks();
  Expect(app.books[0] == &folder && app.books[1] == &books[63], "retry after allocation failure sorts normally");

  Book first, second;
  first.filename = "z.txt"; first.title = "Alpha"; first.author = "Zeta"; first.last_opened = 10;
  second.filename = "a.epub"; second.title = "Beta"; second.author = "Alpha"; second.last_opened = 20;
  struct Case { LibrarySortMode mode; Book *first_file; } cases[] = {
      {LIBRARY_SORT_TITLE, &first}, {LIBRARY_SORT_FILENAME, &second},
      {LIBRARY_SORT_AUTHOR, &second}, {LIBRARY_SORT_FILETYPE, &second},
      {LIBRARY_SORT_RECENT, &second}};
  for (const auto &test : cases) {
    app.settings.library_sort_mode = test.mode;
    app.books = {&first, &second, &folder}; stats.clear(); controller.SortBooks();
    Expect(app.books[0] == &folder && app.books[1] == test.first_file, "other sort modes preserve their order");
    Expect(stats.empty(), "other sort modes perform no mtime I/O");
  }
  app.prefs = nullptr; app.books = {&second, &first}; controller.SortBooks();
  Expect(app.books.front() == &first && stats.empty(), "missing prefs still default to title");
  app.prefs = &app.settings; app.settings.library_sort_mode = LIBRARY_SORT_DATE_MODIFIED;
  app.books.clear(); controller.SortBooks(); Expect(stats.empty(), "empty library performs no I/O");
  puts("PASS: date snapshot I/O, order, ties, refresh and other sort modes");
}
