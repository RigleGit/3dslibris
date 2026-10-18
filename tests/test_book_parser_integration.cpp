#include "book/book_search_source.h"
#include "book/book.h"
#include "book/book_context.h"
#include "book/book_parser.h"
#include "book/book_renderer.h"
#include "book/page.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/common/book_error.h"
#include "formats/mobi/mobi_text_decode.h"
#include "formats/mobi/mobi_page_cache.h"
#include "formats/epub/epub_page_cache.h"
#include "minizip/unzip.h"
#include "shared/app_flow_utils.h"
#include "shared/open_cancel_poll.h"
#include "shared/status_reporter.h"
#include "ui/text.h"
#include "test_assert.h"

#include <algorithm>
#include <cassert>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "tests/fixtures"
#endif

#ifdef IMAGE_TEST_WRAP_MALLOC
// Exercise real allocation refusal without adding a production injection hook.
static size_t fail_image_allocation = 0;
extern "C" void *__real_malloc(size_t);
extern "C" void *__wrap_malloc(size_t size) {
  if (fail_image_allocation && size == fail_image_allocation) {
    fail_image_allocation = 0;
    return nullptr;
  }
  return __real_malloc(size);
}
#endif

namespace {

std::string BookText(Book *book) {
  std::string text;
  for (int p = 0; p < book->GetPageCount(); ++p) {
    const std::vector<std::string> lines =
        page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(p));
    for (size_t i = 0; i < lines.size(); ++i) {
      text += lines[i];
      text += ' ';
    }
  }
  return text;
}

text_search::Match ExpectSearch(Book *book, const char *phrase, bool expected = true) {
  text_search::Engine engine;
  test::ExpectTrue("format search accepts query", engine.Configure(phrase, true));
  engine.Start(text_search::FromBook(book), {}, false, true);
  unsigned ticks = 0;
  while (engine.GetStatus() == text_search::Searching && ++ticks < 1000000)
    engine.Pump(31);
  test::ExpectTrue("format search terminates", ticks < 1000000);
  test::ExpectTrue(phrase, engine.GetStatus() == (expected ? text_search::Found : text_search::NotFound));
  return engine.GetMatch();
}

std::string EncodeZeroImageBytes(size_t count) {
  std::string encoded((count / 3) * 4, 'A');
  if (count % 3 == 1)
    encoded += "AA==";
  else if (count % 3 == 2)
    encoded += "AAA=";
  return encoded;
}

void TestFb2ResourceStorageLimitsAndRecovery() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  const u16 first = book.RegisterInlineImage("fb2:first");
  image_memory::Bytes bytes;
  test::ExpectTrue("store initial FB2 bytes", book.StoreFb2InlineImage("first", "T2xk"));
#ifdef IMAGE_TEST_WRAP_MALLOC
  const std::string replacement = "UmVwbGFjZW1lbnQ=";
  fail_image_allocation = 11;
  test::ExpectFalse("FB2 replacement survives refused payload allocation",
      book.StoreFb2InlineImage("#first", replacement));
  test::ExpectEq("FB2 replacement exercised allocation refusal", fail_image_allocation, (size_t)0);
  test::ExpectTrue("failed FB2 replacement keeps original resource",
      book.ReadInlineImageSource(first, &bytes));
  test::ExpectTrue("failed FB2 replacement preserves original bytes",
      bytes.size() == 3 && std::string((const char *)bytes.data(), bytes.size()) == "Old");
  const u16 missing = book.RegisterInlineImage("fb2:missing");
  fail_image_allocation = 11;
  test::ExpectFalse("new FB2 resource survives refused payload allocation",
      book.StoreFb2InlineImage("missing", replacement));
  test::ExpectEq("new FB2 resource exercised allocation refusal", fail_image_allocation, (size_t)0);
  test::ExpectFalse("refused FB2 resource leaves no readable partial bytes",
      book.ReadInlineImageSource(missing, &bytes));
  test::ExpectTrue("FB2 storage recovers after allocation refusal",
      book.StoreFb2InlineImage("first", replacement));
  test::ExpectTrue("recovered FB2 replacement is readable",
      book.ReadInlineImageSource(first, &bytes));
  test::ExpectTrue("recovered FB2 replacement has complete bytes",
      bytes.size() == 11 && std::string((const char *)bytes.data(), bytes.size()) == "Replacement");
#endif
  const size_t resource_limit = 4u * 1024u * 1024u;
  const std::string full = EncodeZeroImageBytes(resource_limit);
  test::ExpectFalse("FB2 rejects a resource one byte over its limit",
      book.StoreFb2InlineImage("oversized", EncodeZeroImageBytes(resource_limit + 1)));
  for (const char *id : {"first", "second", "third"})
    test::ExpectTrue("FB2 admits resources at exact aggregate limit",
        book.StoreFb2InlineImage(id, full));
  const u16 overflow = book.RegisterInlineImage("fb2:overflow");
  test::ExpectFalse("FB2 rejects aggregate limit plus one byte",
      book.StoreFb2InlineImage("overflow", "AA=="));
  test::ExpectFalse("aggregate rejection publishes no resource",
      book.ReadInlineImageSource(overflow, &bytes));
  test::ExpectTrue("same-sized FB2 replacement fits at aggregate limit",
      book.StoreFb2InlineImage("#first", full));
  test::ExpectTrue("exact-limit FB2 resource remains readable",
      book.ReadInlineImageSource(first, &bytes));
  test::ExpectTrue("exact-limit FB2 resource is complete",
      bytes.size() == resource_limit && bytes[0] == 0 && bytes[resource_limit - 1] == 0);
  image_memory::Bytes again;
  test::ExpectTrue("repeated exact-limit FB2 lookup works",
      book.ReadInlineImageSource(first, &again));
  test::ExpectTrue("repeated FB2 lookup borrows one retained payload",
      again.data() == bytes.data());
  test::ExpectTrue("smaller FB2 replacement releases aggregate budget",
      book.StoreFb2InlineImage("first", "T2xk"));
  test::ExpectTrue("released FB2 budget admits a new resource",
      book.StoreFb2InlineImage("overflow", "AA=="));
  book.ClearInlineImages();
  test::ExpectFalse("cleared FB2 resource cannot be read",
      book.ReadInlineImageSource(first, &bytes));
  test::ExpectTrue("clearing FB2 resources resets the aggregate budget",
      book.StoreFb2InlineImage("first", full));
}

void TestOriginalImageResourceAndDrawBounds() {
  {
    Text text;
    text.capture_rendered_text=true;
    text.display.width=240;text.display.height=400;
    BookContext ctx;ctx.text=&text;Book book(ctx);
    test::ExpectTrue("store alpha reference",book.StoreFb2InlineImage("alpha",
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAEUlEQVR4nGNgYGBoYGD4zwAABIcBgPkkc34AAAAASUVORK5CYII="));
    const u16 id=book.RegisterInlineImage("fb2:alpha");
    std::vector<u16> screen(400*400);
    text.screenleft=text.screenright=screen.data();text.SetScreen(screen.data());
    InlineImageLayoutPlan plan{};plan.draw_width=20;plan.draw_height=10;
    // The framebuffer may contain a gradient or drawing under the image.
    // Changing background colors must work on both cold and cached draws.
    for(int theme=0;theme<2;++theme) {
      const u16 background=theme ? 0x07e0 : 0x001f;
      std::fill(screen.begin(),screen.end(),background);
      text.bgcolor.r=text.bgcolor.g=text.bgcolor.b=theme ? 0 : 255;
      text.SetPen(20,40);
      InlineImageDrawBounds bounds{};
      test::ExpectTrue("draw transparent image",book.DrawInlineImage(&text,id,&plan,0,0,&bounds));
      const size_t row=(size_t)(bounds.y0+bounds.y1)/2*400;
      test::ExpectEq("transparent pixel preserves actual framebuffer",
          screen[row+bounds.x1-1],background);
      test::ExpectEq("partial alpha blends actual framebuffer",
          screen[row+bounds.x0],theme ? 0x03e0 : 0x000f);
    }
  }
  for (unsigned char orientation : {0, 1, 2}) {
    Text text;
    text.capture_rendered_text = true;
    text.landscape = orientation == 2;
    text.display.width = orientation == 2 ? 400 : 240;
    text.display.height = 400; // real square-buffer stride
    std::vector<u16> left(400*400, 0xffff), right(400*400, 0xffff);
    text.screenleft = left.data(); text.screenright = right.data();
    BookContext ctx; ctx.text = &text; ctx.orientation = &orientation;
    Book book(ctx);
    test::ExpectTrue("store original FB2 resource", book.StoreFb2InlineImage("red", "iVBORw0KGgoAAAANSUhEUgAAABAAAAAICAIAAAB/FOjAAAAAFElEQVR4nGP4z8BAEiJN9agGIhEAJMZ/gd5KfaIAAAAASUVORK5CYII="));
    const u16 image = book.RegisterInlineImage("fb2:red");
    image_memory::Bytes original;
    test::ExpectTrue("read original bytes", book.ReadInlineImageSource(image, &original));
    test::ExpectTrue("original PNG dimensions retained", original.size() > 24 &&
        original[19] == 16 && original[23] == 8);
    image_memory::Bytes borrowed;
    test::ExpectTrue("FB2 original remains owned by book",book.ReadInlineImageSource(image,&borrowed));
    test::ExpectTrue("FB2 source reading avoids a duplicate payload",borrowed.data()==original.data());
    for (int reading_screen = 0; reading_screen < 2; ++reading_screen) {
      const bool is_left = (orientation != 1) == (reading_screen == 0);
      auto &pixels = is_left ? left : right;
      text.SetScreen(pixels.data());
      for (int mode : {0,1,2}) for (int align : {0,1,2}) {
        std::fill(pixels.begin(), pixels.end(), 0xffff);
        text.SetPen(230,40);
        InlineImageLayoutPlan plan{};
        plan.mode = (InlineImageLayoutMode)mode; plan.draw_width = 20; plan.draw_height = 10;
        InlineImageDrawBounds bounds{};
        test::ExpectTrue("draw real source image", book.DrawInlineImage(&text,image,&plan,reading_screen,align,&bounds));
        int x0=400,y0=400,x1=0,y1=0;
        for (int y=0;y<400;++y) for(int x=0;x<400;++x) {
          if (pixels[y*400+x] != 0xffff) {
            test::ExpectTrue("source red survives decode and cache", pixels[y*400+x] == 0xf800);
            x0=std::min(x0,x); y0=std::min(y0,y); x1=std::max(x1,x+1); y1=std::max(y1,y+1);
          }
        }
        test::ExpectTrue("returned bounds equal actual painted pixels",
            bounds.x0==x0 && bounds.y0==y0 && bounds.x1==x1 && bounds.y1==y1);
      }
    }
    test::ExpectTrue("reading original never changes pages", book.GetPageCount() == 0 && book.GetPosition() == 0);
    book.ClearInlineImages();
    test::ExpectFalse("closed resource cannot be reused", book.ReadInlineImageSource(image,&original));
  }
}

void TestInlineImageCacheRetainsDistinctDimensions() {
  const char *folder = getenv("TEST_IMAGE_VIEWER_DIR");
  test::ExpectTrue("cache test image fixture directory", folder != nullptr);
  for (int changed_dimension = 0; changed_dimension < 2; ++changed_dimension) {
    const std::string source_path = std::string(folder) + "/images.epub";
    std::string copy_template = std::string(folder) + "/cache-variants-XXXXXX";
    std::vector<char> copy_name(copy_template.begin(), copy_template.end());
    copy_name.push_back('\0');
    const int fd = mkstemp(copy_name.data());
    test::ExpectTrue("create isolated image cache archive", fd >= 0);
    FILE *copy = fdopen(fd, "wb");
    FILE *source = fopen(source_path.c_str(), "rb");
    test::ExpectTrue("open archive copy streams", copy != nullptr && source != nullptr);
    char chunk[4096];
    size_t count;
    while ((count = fread(chunk, 1, sizeof(chunk), source)))
      test::ExpectEq("copy archive fixture bytes", fwrite(chunk, 1, count, copy), count);
    test::ExpectEq("read complete image cache archive fixture", ferror(source), 0);
    test::ExpectEq("close image cache archive source", fclose(source), 0);
    test::ExpectEq("finish isolated image cache archive", fclose(copy), 0);

    unsigned char orientation = 2;
    Text text;
    text.capture_rendered_text = true;
    text.landscape = true;
    text.display.width = 400;
    text.display.height = 400;
    BookContext ctx;
    ctx.text = &text;
    ctx.orientation = &orientation;
    Book book(ctx);
    book.SetFolderName(folder);
    const std::string copied_path = copy_name.data();
    book.SetFileName(copied_path.substr(copied_path.find_last_of('/') + 1).c_str());
    book.format = FORMAT_EPUB;
    const u16 image = book.RegisterInlineImage("OEBPS/tiny.png");
    std::vector<u16> screen(400 * 400, 0xffff);
    text.screenleft = text.screenright = screen.data();
    text.SetScreen(screen.data());
    auto draw = [&](int width, int height) {
      std::fill(screen.begin(), screen.end(), 0xffff);
      text.SetPen(10, 40);
      InlineImageLayoutPlan plan{};
      plan.mode = INLINE_IMAGE_LAYOUT_INLINE;
      plan.draw_width = width;
      plan.draw_height = height;
      InlineImageDrawBounds bounds{};
      test::ExpectTrue("draw retained inline image variant",
          book.DrawInlineImage(&text, image, &plan, 0, 0, &bounds));
      test::ExpectTrue("cached variant keeps requested dimensions",
          bounds.x1 - bounds.x0 == width && bounds.y1 - bounds.y0 == height);
      for (int y = bounds.y0; y < bounds.y1; ++y)
        for (int x = bounds.x0; x < bounds.x1; ++x)
          test::ExpectEq("cached variant retains original red pixels", screen[y * 400 + x], (u16)0xf800);
    };
    draw(40, 20);
    draw(changed_dimension == 0 ? 296 : 40, changed_dimension == 0 ? 20 : 30);
    test::ExpectEq("remove only isolated image cache archive", remove(copied_path.c_str()), 0);
    // Both variants fit the cache. The first must remain usable after the
    // source disappears, including widths separated by 256 and distinct heights.
    draw(40, 20);
  }
}

void TestImageResourcesAcrossBookFormats() {
  const char *folder = getenv("TEST_IMAGE_VIEWER_DIR");
  test::ExpectTrue("image fixture directory", folder != nullptr);
  const std::string cache = std::string(folder) + "/cache";
  mkdir(cache.c_str(), 0700);
  mobi_page_cache::SetCacheDirForTest(cache.c_str());
  epub_page_cache::SetCacheDirForTest(cache.c_str());
  {
    Text text;BookContext ctx;ctx.text=&text;Book book(ctx);
    const std::string path=std::string(folder)+"/oversized-image.base64";
    FILE *fp=fopen(path.c_str(),"rb");test::ExpectTrue("large image fixture exists",fp!=nullptr);
    std::string encoded;char chunk[4096];size_t n;
    while((n=fread(chunk,1,sizeof(chunk),fp))) encoded.append(chunk,n);
    fclose(fp);
    test::ExpectTrue("compressed large image fits resource limit",book.StoreFb2InlineImage("huge",encoded));
    const u16 image=book.RegisterInlineImage("fb2:huge");
    std::vector<u16> screen(400*400,0xffff);
    text.screenleft=text.screenright=screen.data();text.SetScreen(screen.data());text.SetPen(0,30);
    InlineImageLayoutPlan plan{};plan.draw_width=20;plan.draw_height=20;
    InlineImageDrawBounds bounds{1,1,2,2};
    test::ExpectFalse("inline decode rejects excessive memory without drawing",
        book.DrawInlineImage(&text,image,&plan,0,0,&bounds));
    test::ExpectTrue("failed inline image leaves the framebuffer intact",
        std::all_of(screen.begin(),screen.end(),[](u16 p){return p==0xffff;}));
    test::ExpectTrue("failed inline image has no selectable geometry",
        bounds.x0==0 && bounds.y0==0 && bounds.x1==0 && bounds.y1==0);
  }
#ifdef IMAGE_TEST_WRAP_MALLOC
  {
    Text text; text.capture_rendered_text=true;
    text.display.width=240;text.display.height=400;
    BookContext ctx;ctx.text=&text;Book book(ctx);
    book.SetFolderName(folder);book.SetFileName("images.epub");book.format=FORMAT_EPUB;
    const u16 id=book.RegisterInlineImage("OEBPS/tiny.png");
    image_memory::Bytes source;
    test::ExpectTrue("read EPUB source before failure",book.ReadInlineImageSource(id,&source));
    // Metadata probing and the full read allocate exactly this resource size.
    // Only the first allocation fails: PAGE mode must recover via full decode.
    fail_image_allocation=source.size();
    std::vector<u16> screen(400*400,0xffff);
    text.screenleft=text.screenright=screen.data();text.SetScreen(screen.data());
    InlineImageLayoutPlan plan{};plan.mode=INLINE_IMAGE_LAYOUT_PAGE;
    InlineImageDrawBounds bounds{};
    test::ExpectTrue("PAGE image recovers from failed metadata allocation",
        book.DrawInlineImage(&text,id,&plan,0,0,&bounds));
    test::ExpectEq("metadata allocation failure was exercised",fail_image_allocation,(size_t)0);
    test::ExpectTrue("recovered image has visible bounds",bounds.x1>bounds.x0 && bounds.y1>bounds.y0);
    test::ExpectTrue("recovered image draws original red pixels",
        std::find(screen.begin(),screen.end(),(u16)0xf800)!=screen.end());
  }
#endif
  for (const char *name : {"images.epub", "images.mobi", "images.fb2"}) {
    Text text; BookContext ctx; ctx.text = &text;
    text.display.width = 240;
    Book book(ctx); book.SetFolderName(folder); book.SetFileName(name);
    if (std::string(name) == "images.epub") book.format = FORMAT_EPUB;
    test::ExpectEq(name, book_parser::Open(&book), 0);
    const int pages = book.GetPageCount(), position = book.GetPosition();
    test::ExpectTrue("image book spans pages", pages > 1);
    test::ExpectTrue("format registers all original resources", book.GetInlineImageCount() >= 3);
    bool found_detail = false;
    for (u16 id = 0; id < book.GetInlineImageCount(); ++id) {
      image_memory::Bytes bytes;
      test::ExpectTrue("format reads original image", book.ReadInlineImageSource(id,&bytes));
      if (bytes.size() > 24 && bytes[0] == 0x89 && bytes[16] == 0 && bytes[17] == 0 &&
          bytes[18] == 4 && bytes[19] == 0) found_detail = true;
    }
    test::ExpectTrue("1024px detail never replaced by page thumbnail", found_detail);
    test::ExpectEq("resource reading preserves page count",book.GetPageCount(),pages);
    test::ExpectEq("resource reading preserves position",book.GetPosition(),position);
    book.Close();
    test::ExpectEq("book close releases image resources",book.GetInlineImageCount(),0);
    // Exercise warm EPUB/MOBI cache reconstruction as well as a cold parse.
    if (std::string(name) == "images.epub") book.format = FORMAT_EPUB;
    test::ExpectEq("image book reopens", book_parser::Open(&book),0);
    test::ExpectEq("image book reopens at identical layout",book.GetPageCount(),pages);
    test::ExpectTrue("cache retains image resource mapping",book.GetInlineImageCount() >= 3);
    for (u16 id = 0; id < book.GetInlineImageCount(); ++id) {
      image_memory::Bytes bytes;
      test::ExpectTrue("reopened image resolves original",book.ReadInlineImageSource(id,&bytes));
    }
    book.Close();
  }
}

void TestOdtSearch() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(getenv("TEST_CBZ_READING_DIR"));
  book.SetFileName("search.odt");
  test::ExpectEq("real ODT dispatch opens", book_parser::Open(&book), 0);
  ExpectSearch(&book, "tiny ODT fixture");
  ExpectSearch(&book, "café año");
  ExpectSearch(&book, "ano", false);
  book.Close();
}

void TestDispatchContentAndCloseBetweenFormats() {
  struct Case {
    const char *name;
    const char *content;
    const char *excluded_markup;
  };
  const Case cases[] = {
      {"basic.txt", "tiny TXT fixture", "tiny FB2 fixture"},
      {"basic.fb2", "tiny FB2 fixture", "<FictionBook"},
      {"basic.rtf", "tiny RTF fixture", "\\rtf1"},
      {"basic.md", "small Markdown fixture", "**small**"},
  };
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  std::string previous_text;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    book.SetFileName(cases[i].name);
    book.format = FORMAT_UNDEF; // Dispatch must recognize the file extension.
    test::ExpectEq("format dispatch opens actual fixture", book_parser::Open(&book), 0);
    test::ExpectGt("format dispatch creates pages", book.GetPageCount(), 0);
    ExpectSearch(&book, cases[i].content);
    const std::string rendered = BookText(&book);
    test::ExpectStrContains(cases[i].name, rendered.c_str(), cases[i].content);
    test::ExpectTrue("format markup is consumed", rendered.find(cases[i].excluded_markup) == std::string::npos);
    if (i > 0)
      test::ExpectTrue("previous format content is gone", rendered.find(cases[i-1].content) == std::string::npos);
    if (i == 1) {
      const std::vector<ChapterEntry> &chapters = book.GetChapters();
      test::ExpectTrue("FB2 section creates navigation", !chapters.empty());
      test::ExpectStrEq("FB2 section title", chapters[0].title.c_str(), "Chapter 1");
      test::ExpectEq("FB2 basic section is top level", (int)chapters[0].level, 0);
    }
    if (i == 3) {
      test::ExpectStrContains("Markdown link label survives", rendered.c_str(), "link text");
      test::ExpectTrue("Markdown URL is not visible prose", rendered.find("https://example.com") == std::string::npos);
      test::ExpectStrContains("Markdown first heading survives", rendered.c_str(), "Markdown Chapter");
      test::ExpectStrContains("Markdown second heading survives", rendered.c_str(), "Second Section");
      test::ExpectTrue("Markdown heading markers consumed", rendered.find("# Markdown") == std::string::npos);
    }
    previous_text = rendered;
    book.Close();
    test::ExpectEq("close releases pages", book.GetPageCount(), 0);
    test::ExpectTrue("close releases chapters", book.GetChapters().empty());
    test::ExpectEqU("close releases inline links", book.GetInlineLinkHrefCount(), 0);
  }
  book.SetFileName("basic.md");
  test::ExpectEq("reopen final format", book_parser::Open(&book), 0);
  test::ExpectStrEq("reopen retains exactly the same content", BookText(&book).c_str(), previous_text.c_str());
  book.Close();

  book.SetFileName("missing-audit-fixture.txt");
  test::ExpectTrue("missing TXT returns error", book_parser::Open(&book) != 0);
  test::ExpectEq("failed open exposes no old pages", book.GetPageCount(), 0);
  book.Close();
  book.SetFileName("basic.txt");
  test::ExpectEq("open recovers after missing file", book_parser::Open(&book), 0);
  test::ExpectStrContains("recovery uses requested TXT", BookText(&book).c_str(), "tiny TXT fixture");
  book.Close();
}

void TestFb2NestedNavigation() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  book.SetFileName("chapters.fb2");
  book.format = FORMAT_UNDEF;
  test::ExpectEq("nested FB2 opens", book_parser::Open(&book), 0);
  test::ExpectGt("nested FB2 creates pages", book.GetPageCount(), 0);
  const std::vector<ChapterEntry> &chapters = book.GetChapters();
  test::ExpectEq("nested FB2 has exactly three sections", (int)chapters.size(), 3);
  const char *titles[] = {"Chapter One", "Section 1.1", "Chapter Two"};
  const int levels[] = {0, 1, 0};
  for (size_t i = 0; i < 3; ++i) {
    test::ExpectStrEq("FB2 section label", chapters[i].title.c_str(), titles[i]);
    test::ExpectEq("FB2 section nesting resets", (int)chapters[i].level, levels[i]);
  }
  book.Close();
}

void TestOpenErrorMessages() {
  test::ExpectStrEq("corrupt books have short tag", BookOpenErrorTag(BOOK_ERR_CORRUPT), "corrupt_or_empty_book");
  test::ExpectStrEq("corrupt books have friendly text", DescribeBookOpenError(BOOK_ERR_CORRUPT), "error: corrupt or empty book");
  test::ExpectTrue("unknown errors have no short tag", BookOpenErrorTag(253) == nullptr);
  test::ExpectTrue("unknown errors fall back to numeric formatting", DescribeBookOpenError(253) == nullptr);
}

void TestEpubMetadataOpenCloseRecovery() {
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  book.SetFileName("basic.epub");
  book.format = FORMAT_EPUB;

  test::ExpectEq("EPUB metadata dispatch", book_parser::Index(&book), 0);
  test::ExpectStrEq("indexed title", book.GetTitle(), "Basic EPUB Fixture");
  test::ExpectStrEq("indexed author", book.GetAuthor().c_str(), "3dslibris Test");
  test::ExpectEq("metadata indexing does not paginate", book.GetPageCount(), 0);
  test::ExpectTrue("metadata indexing does not create chapters", book.GetChapters().empty());
  test::ExpectEq("repeat metadata index", book_parser::Index(&book), 0);
  test::ExpectEq("repeat index still has no pages", book.GetPageCount(), 0);

  test::ExpectEq("fulltext dispatch after metadata", book_parser::Open(&book), 0);
  test::ExpectGt("fulltext dispatch paginates", book.GetPageCount(), 0);
  test::ExpectStrEq("metadata title survives fulltext dispatch", book.GetTitle(), "Basic EPUB Fixture");
  test::ExpectStrEq("metadata author survives fulltext dispatch", book.GetAuthor().c_str(), "3dslibris Test");
  const u16 pages = book.GetPageCount();
  const std::vector<ChapterEntry> chapters = book.GetChapters();
  test::ExpectEq("dispatch preserves both EPUB navigation labels", (int)chapters.size(), 2);
  test::ExpectStrEq("first EPUB label", chapters[0].title.c_str(), "Chapter One");
  test::ExpectStrEq("second EPUB label", chapters[1].title.c_str(), "Chapter Two");
  book.Close();
  test::ExpectEq("close clears paginated pages", book.GetPageCount(), 0);
  test::ExpectTrue("close clears navigation", book.GetChapters().empty());

  test::ExpectEq("reopen dispatch", book_parser::Open(&book), 0);
  test::ExpectEq("reopen does not append pages", book.GetPageCount(), pages);
  test::ExpectEq("reopen does not append chapters", (int)book.GetChapters().size(), 2);
  for (size_t i = 0; i < chapters.size(); ++i) {
    test::ExpectEq("reopen chapter page stable", book.GetChapters()[i].page, chapters[i].page);
    test::ExpectStrEq("reopen chapter label stable", book.GetChapters()[i].title.c_str(), chapters[i].title.c_str());
  }
  book.Close();
  book.SetFileName("missing-audit-dispatch.epub");
  test::ExpectTrue("missing EPUB dispatch returns error", book_parser::Open(&book) != 0);
  test::ExpectEq("failed dispatch exposes no stale pages", book.GetPageCount(), 0);
  test::ExpectTrue("failed dispatch exposes no stale chapters", book.GetChapters().empty());
  book.Close();
  book.SetFileName("basic.epub");
  test::ExpectEq("dispatch recovers after open failure", book_parser::Open(&book), 0);
  test::ExpectEq("recovery restores original page count", book.GetPageCount(), pages);
  book.Close();
}



struct CancelReporter : IStatusReporter {
  bool requested=false;
  void PrintStatus(const char *) override {}
  void PrintStatus(std::string) override {}
  bool ShouldAbortWork() const override { return requested; }
};
void TestCancelledOpenAndRecovery() {
  Text text; CancelReporter reporter; BookContext ctx;
  ctx.text=&text; ctx.status_reporter=&reporter; Book book(ctx);
  for (int flags=0; flags<4; ++flags) {
    reporter.requested=(flags & 1) != 0;
    book.ClearOpenAbortRequest();
    if (flags & 2) book.RequestAbortOpen();
    assert(open_cancel_poll::Poll(&book, &reporter, "test") == (flags != 0));
  }
  assert(open_cancel_poll::Poll(nullptr, &reporter, "test"));
  reporter.requested=false;
  assert(!open_cancel_poll::Poll(nullptr, &reporter, "test"));
  assert(!open_cancel_poll::Poll(nullptr, nullptr, "test"));
  const char *names[]={"basic.txt", "basic.fb2", "basic.epub"};
  book.SetFolderName(TEST_FIXTURES_DIR "/books");
  for (const char *name : names) {
    book.SetFileName(name); book.format=std::string(name) == "basic.epub" ? FORMAT_EPUB : FORMAT_UNDEF;
    book.PrepareForOpen(); book.RequestAbortOpen();
    test::ExpectEq("prepared parser respects book cancellation", book_parser::OpenPrepared(&book), BOOK_ERR_CANCELLED);
    book.Close(); assert(book.GetPageCount() == 0 && !book.IsOpenAbortRequested());
    reporter.requested=true; book.PrepareForOpen();
    test::ExpectEq("prepared parser respects application cancellation", book_parser::OpenPrepared(&book), BOOK_ERR_CANCELLED);
    book.Close(); reporter.requested=false;
    test::ExpectEq("normal open recovers after cancellation", book_parser::Open(&book), 0);
    assert(book.GetPageCount() > 0 && !BookText(&book).empty());
    book.Close();
  }
}

void TestCbzReadPageZoomCloseAndReopen() {
  const char *folder=getenv("TEST_CBZ_READING_DIR"); assert(folder);
  Text text; text.display.width=240; text.display.height=240;
  std::vector<u16> top(240 * 400 + 2, 0xbeef), bottom(240 * 320 + 2, 0xbeef);
  text.screenleft=top.data()+1; text.screenright=bottom.data()+1;
  BookContext ctx; ctx.text=&text; Book book(ctx);
  book.SetFolderName(folder); book.SetFileName("first.cbz");
  book.format=FORMAT_CBZ; // LibraryController supplies the format before opening.
  test::ExpectEq("CBZ opens through Book parser dispatch", book_parser::Open(&book), 0);
  assert(book.IsCbz() && book.GetPageCount() == 3);
  assert(book.GetChapters().size() == 1 && book.GetChapters()[0].page == 1);
  assert(book.GetChapters()[0].title == "Blue chapter");
  book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800 && "first naturally sorted page is red");
  assert(book_renderer::ChangeFixedLayoutZoom(&book, 1));
  assert(book_renderer::TranslateFixedLayoutViewport(&book, .1f, .1f));
  book_renderer::SetFixedLayoutViewportInteraction(&book, true);
  book_renderer::DrawCurrentView(&book, &text);
  book_renderer::SetFixedLayoutViewportInteraction(&book, false);
  book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800 && "zoom/pan retains requested page pixels");
  book.SetPosition(1); book_renderer::ResetFixedLayoutViewportForNavigation(&book);
  book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0x001f && "page turn replaces red with blue");
  text.printed_strings.clear();
  book.SetPosition(2); book_renderer::DrawCurrentView(&book, &text);
  assert(std::find(text.printed_strings.begin(), text.printed_strings.end(), "image decode failed") != text.printed_strings.end());
  book.SetPosition(0); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800);
  // Suspend-style reset must close the retained ZIP handle. A subsequent
  // read observes the missing path rather than the still-open old descriptor.
  book.ResetCbzTransientViewState(true);
  const std::string original=std::string(folder) + "/first.cbz", moved=original + ".moved";
  assert(rename(original.c_str(), moved.c_str()) == 0);
  text.printed_strings.clear();
  book.SetPosition(1); book_renderer::DrawCurrentView(&book, &text);
  assert(std::find(text.printed_strings.begin(), text.printed_strings.end(), "CBZ page unavailable") != text.printed_strings.end());
  assert(rename(moved.c_str(), original.c_str()) == 0);
  book.ResetCbzTransientViewState(true); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0x001f);
  assert(top.front() == 0xbeef && top.back() == 0xbeef);
  assert(bottom.front() == 0xbeef && bottom.back() == 0xbeef);
  book.Close(); assert(book.GetPageCount() == 0 && book.GetChapters().empty());
  book.SetFileName("second.cbz");
  assert(book_parser::Open(&book) == 0 && book.GetPageCount() == 1 && book.GetChapters().empty());
  book.SetPosition(0); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0x07e0 && "another archive cannot reuse earlier page pixels");
  book.Close(); book.SetFileName("first.cbz");
  assert(book_parser::Open(&book) == 0 && book.GetPageCount() == 3 && book.GetChapters().size() == 1);
  book.SetPosition(0); book_renderer::DrawCurrentView(&book, &text);
  assert(top[1 + 200 * 240 + 120] == 0xf800);
  book.Close(); book.SetFileName("missing.cbz");
  assert(book_parser::Open(&book) != 0 && book.GetPageCount() == 0);
  book.Close();
}

void TestDecodeCp1252() {
  const std::string raw = std::string("caf") + "\xE9" + " y " + "\x97" + " fin";
  bool used_utf8_guess = true;
  bool used_legacy_guess = true;
  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8(raw, 1252, &used_utf8_guess,
                                          &used_legacy_guess);

  test::ExpectStrEq("cp1252 decode", decoded.c_str(), "caf\xC3\xA9 y \xE2\x80\x94 fin");
  test::ExpectFalse("cp1252 used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("cp1252 used_legacy_guess", used_legacy_guess);
}

std::vector<std::string> CacheFiles(const std::string &folder) {
  DIR *dir = opendir(folder.c_str());
  test::ExpectTrue("cache directory is readable", dir != nullptr);
  std::vector<std::string> files;
  while (dirent *entry = readdir(dir)) {
    if (entry->d_name[0] != '.')
      files.push_back(folder + "/" + entry->d_name);
  }
  closedir(dir);
  return files;
}

std::string ReadFileBytes(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  test::ExpectTrue("source fixture exists", fp != nullptr);
  std::string data;
  char buffer[4096];
  size_t length;
  while ((length = fread(buffer, 1, sizeof(buffer), fp)) != 0)
    data.append(buffer, length);
  test::ExpectFalse("fixture read has no I/O error", ferror(fp));
  test::ExpectEq("fixture read closes", fclose(fp), 0);
  return data;
}

void WriteFileBytes(const std::string &path, const std::string &data) {
  FILE *fp = fopen(path.c_str(), "wb");
  test::ExpectTrue("fixture opens for replacement", fp != nullptr);
  test::ExpectTrue("fixture replacement writes all bytes",
                   fwrite(data.data(), 1, data.size(), fp) == data.size());
  test::ExpectEq("fixture replacement closes", fclose(fp), 0);
}

typedef std::vector<std::vector<u32>> PageData;

PageData CapturePageData(Book *book) {
  PageData data;
  for (int i = 0; i < book->GetPageCount(); ++i) {
    Page *page = book->GetPage(i);
    data.push_back(std::vector<u32>());
    if (page->GetLength() > 0)
      data.back().assign(page->GetBuffer(), page->GetBuffer() + page->GetLength());
  }
  return data;
}

void ExpectMobiContent(Book *book) {
  ExpectSearch(book, "café & tea");
  ExpectSearch(book, "TOKEN1999 TOKEN2000");
  ExpectSearch(book, "HIDDEN-MOBI", false);
  const std::string content = BookText(book);
  size_t cursor = 0;
  for (unsigned word = 1; word <= 4000; ++word) {
    char expected[16];
    snprintf(expected, sizeof(expected), "TOKEN%04u", word);
    const size_t found = content.find(expected, cursor);
    test::ExpectTrue(expected, found != std::string::npos);
    test::ExpectTrue("every MOBI word appears exactly once",
                     content.find(expected, found + 1) == std::string::npos);
    cursor = found + 9;
  }
  test::ExpectStrContains("MOBI pagination reaches the end", content.c_str(), "MOBIEND");
  test::ExpectStrContains("MOBI decodes UTF-8 and entities", content.c_str(), "café & tea");
  test::ExpectTrue("MOBI excludes script text", content.find("HIDDEN-MOBI") == std::string::npos);
  test::ExpectTrue("MOBI consumes HTML", content.find("<h1>") == std::string::npos);
  test::ExpectGt("MOBI fixture actually spans pages", book->GetPageCount(), 1);
  const char *labels[] = {"Chapter One", "Chapter Two", "Chapter Three", "Chapter Four"};
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  test::ExpectEq("real MOBI hooks retain all TOC entries", (int)chapters.size(), 4);
  for (size_t i = 0; i < 4; ++i) {
    test::ExpectStrEq("MOBI chapter label", chapters[i].title.c_str(), labels[i]);
    test::ExpectTrue("MOBI chapter targets a readable page", chapters[i].page < book->GetPageCount());
    if (i)
      test::ExpectTrue("MOBI chapters follow the source order", chapters[i].page > chapters[i - 1].page);
  }
}

void TestMobiParseCloseCachedReopenAndRecovery() {
  const char *folder = getenv("TEST_MOBI_READING_DIR");
  test::ExpectTrue("MOBI fixture folder supplied", folder != nullptr);
  const std::string cache_dir = std::string(folder) + "/cache";
  mobi_page_cache::SetCacheDirForTest(cache_dir.c_str());
  const char *names[] = {"raw.mobi", "palmdoc.mobi"};
  for (const char *name : names) {
    Text text;
    CancelReporter reporter;
    BookContext ctx;
    ctx.text = &text;
    ctx.status_reporter = &reporter;
    PageData pages;
    std::vector<ChapterEntry> chapters;
    {
      Book book(ctx);
      book.SetFolderName(folder);
      book.SetFileName(name);
      test::ExpectEq(name, book_parser::Open(&book), 0);
      ExpectMobiContent(&book);
      pages = CapturePageData(&book);
      chapters = book.GetChapters();
      test::ExpectTrue("cold MOBI open schedules a real cache save", book.HasPendingMobiPageCacheSave());
      test::ExpectTrue("cold open leaves cache write off the critical path", CacheFiles(cache_dir).empty());
      book.Close();
      test::ExpectEq("MOBI close releases pages", book.GetPageCount(), 0);
      test::ExpectTrue("MOBI close releases chapters", book.GetChapters().empty());
    }
    const std::vector<std::string> caches = CacheFiles(cache_dir);
    test::ExpectEq("Close saves one MOBI cache artifact", (int)caches.size(), 1);
    const std::string path = std::string(folder) + "/" + name;
    const std::string source = ReadFileBytes(path);
    struct stat original;
    test::ExpectEq("source attributes readable", stat(path.c_str(), &original), 0);
    // Preserve the source identity used by the cache key but make decoding
    // impossible. A successful warm open must therefore come from the cache.
    WriteFileBytes(path, std::string(source.size(), '\0'));
    const utimbuf times = {original.st_atime, original.st_mtime};
    test::ExpectEq("preserve fixture timestamp", utime(path.c_str(), &times), 0);
    Book reopened(ctx);
    reopened.SetFolderName(folder);
    reopened.SetFileName(name);
    test::ExpectEq("cached open succeeds with source decoding unavailable", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    test::ExpectTrue("cache preserves every page codepoint", CapturePageData(&reopened) == pages);
    for (size_t i = 0; i < chapters.size(); ++i)
      test::ExpectEq("cache preserves chapter targets", reopened.GetChapters()[i].page, chapters[i].page);
    test::ExpectFalse("cache hit does not schedule reparsed content", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    test::ExpectEq("remove cache for negative control", remove(caches[0].c_str()), 0);
    test::ExpectTrue("same invalid source fails without the cache", book_parser::Open(&reopened) != 0);
    test::ExpectEq("rejected source creates no pages", reopened.GetPageCount(), 0);
    test::ExpectFalse("failed source cannot schedule a cache", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    WriteFileBytes(path, source);
    test::ExpectEq("restore fixture timestamp", utime(path.c_str(), &times), 0);
    test::ExpectEq("MOBI recovers after invalid source", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    reopened.Close();
    const std::vector<std::string> restored_caches = CacheFiles(cache_dir);
    test::ExpectEq("recovery recreates one cache", (int)restored_caches.size(), 1);
    const std::string saved_cache = ReadFileBytes(restored_caches[0]);
    WriteFileBytes(restored_caches[0], saved_cache.substr(0, saved_cache.size() / 2));
    test::ExpectEq("partial cache falls back to the real MOBI parser", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    test::ExpectTrue("fallback discards partially loaded pages", CapturePageData(&reopened) == pages);
    test::ExpectTrue("cache fallback schedules replacement", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    test::ExpectTrue("Close replaces the partial cache", ReadFileBytes(restored_caches[0]) == saved_cache);
    text.pixelsize += 4;
    test::ExpectEq("MOBI opens with a different layout", book_parser::Open(&reopened), 0);
    ExpectMobiContent(&reopened);
    test::ExpectTrue("different layout requires new cache save", reopened.HasPendingMobiPageCacheSave());
    test::ExpectGt("larger font actually repaginates", reopened.GetPageCount(), (int)pages.size());
    reopened.Close();
    for (const std::string &cache : CacheFiles(cache_dir))
      test::ExpectEq("remove isolated MOBI cache", remove(cache.c_str()), 0);
    reopened.SetFileName((std::string("truncated-") + name).c_str());
    test::ExpectTrue("truncated MOBI is rejected through dispatch", book_parser::Open(&reopened) != 0);
    test::ExpectFalse("truncated MOBI cannot schedule cache save", reopened.HasPendingMobiPageCacheSave());
    reopened.Close();
    test::ExpectTrue("failed MOBI close leaves no cache", CacheFiles(cache_dir).empty());
    printf("PASS: real MOBI %s, %lu pages, 4000 ordered words, four chapters, cache and recovery\n",
           name, (unsigned long)pages.size());
  }
  mobi_page_cache::SetCacheDirForTest(nullptr);
}

void ExpectClosedReadingState(Book *book) {
  test::ExpectEq("Close releases pages after interruption", book->GetPageCount(), 0);
  test::ExpectTrue("Close releases chapters after interruption", book->GetChapters().empty());
  test::ExpectEqU("Close releases links after interruption", book->GetInlineLinkHrefCount(), 0);
  test::ExpectEqU("Close releases anchors after interruption", book->GetChapterAnchorCount(), 0);
  test::ExpectTrue("Close releases document navigation", book->GetChapterDocStartPages().empty());
  test::ExpectFalse("Close clears pending EPUB cache", book->HasPendingEpubPageCacheSave());
}

void ExpectCompleteRecoveryEpub(Book *book, bool expect_middle = true) {
  const std::string content = BookText(book);
  test::ExpectStrContains("EPUB includes start of first document", content.c_str(), "FIRST-START");
  test::ExpectStrContains("EPUB includes end of first document", content.c_str(), "FIRST-END");
  test::ExpectStrContains("EPUB reaches final spine document", content.c_str(), "FINAL-SPINE");
  test::ExpectStrEq("EPUB metadata survives recovery", book->GetTitle(), "EPUB Recovery Fixture");
  if (expect_middle) {
    test::ExpectStrContains("EPUB includes the middle document", content.c_str(), "MIDDLE-SPINE");
  }
  test::ExpectEq("EPUB retains complete navigation", (int)book->GetChapters().size(), 3);
  bool first_found = false, last_found = false;
  for (const ChapterEntry &chapter : book->GetChapters()) {
    test::ExpectTrue("recovered EPUB chapter targets a readable page", chapter.page < book->GetPageCount());
    first_found |= chapter.title == "first chapter";
    last_found |= chapter.title == "last chapter";
  }
  test::ExpectTrue("EPUB keeps navigation for the valid documents", first_found && last_found);
  test::ExpectGt("EPUB registers real inline links", book->GetInlineLinkHrefCount(), 0);
  test::ExpectGt("EPUB registers real anchors", (int)book->GetChapterAnchorCount(), 0);
}

struct SpineCancellation : IStatusReporter {
  Book *book = nullptr;
  int mode = 0;
  bool enabled = true;
  bool requested = false;
  bool first_completed = false;
  void PrintStatus(const char *) override {}
  void PrintStatus(std::string) override {}
  bool ShouldAbortWork() const override {
    return enabled && (requested || (mode == 2 && book && book->GetPageCount() > 0));
  }
  static void OnProgress(unsigned done, unsigned total, void *user_data) {
    SpineCancellation *self = static_cast<SpineCancellation *>(user_data);
    if (!self->enabled || done != 1)
      return;
    test::ExpectEq("cancellation fixture contains three spine documents", total, 3);
    self->first_completed = true;
    if (self->mode == 0)
      self->book->RequestAbortOpen();
    else if (self->mode == 1)
      self->requested = true;
  }
};

void TestEpubInterruptedSpineAndRecovery() {
  const char *folder = getenv("TEST_EPUB_RECOVERY_DIR");
  test::ExpectTrue("EPUB recovery folder supplied", folder != nullptr);
  const std::string cache_dir = std::string(folder) + "/cache-cancel";
  epub_page_cache::SetCacheDirForTest(cache_dir.c_str());
  for (int mode = 0; mode < 3; ++mode) {
    Text text;
    SpineCancellation cancellation;
    cancellation.mode = mode;
    BookContext ctx;
    ctx.text = &text;
    ctx.status_reporter = &cancellation;
    ctx.on_spine_progress = SpineCancellation::OnProgress;
    ctx.on_spine_progress_user_data = &cancellation;
    Book book(ctx);
    cancellation.book = &book;
    book.SetFolderName(folder);
    book.SetFileName("valid.epub");
    book.format = FORMAT_EPUB;
    test::ExpectEq("interrupted EPUB returns cancellation", book_parser::Open(&book), BOOK_ERR_CANCELLED);
    test::ExpectGt("cancellation occurs after actual content was parsed", book.GetPageCount(), 0);
    const std::string partial = BookText(&book);
    test::ExpectStrContains("cancelled book reached the first document", partial.c_str(), "FIRST-START");
    test::ExpectTrue("cancelled book never reaches the next document", partial.find("MIDDLE-SPINE") == std::string::npos);
    test::ExpectTrue("cancelled book never reaches the last document", partial.find("FINAL-SPINE") == std::string::npos);
    test::ExpectTrue("cancellation happens at the intended boundary", cancellation.first_completed == (mode != 2));
    if (mode == 2)
      test::ExpectTrue("XML streaming aborts before the first document finishes", partial.find("FIRST-END") == std::string::npos);
    else
      test::ExpectStrContains("progress cancellation follows completed document", partial.c_str(), "FIRST-END");
    test::ExpectFalse("partial EPUB cannot schedule a cache save", book.HasPendingEpubPageCacheSave());
    book.Close();
    ExpectClosedReadingState(&book);
    test::ExpectTrue("cancelled EPUB Close writes no partial cache", CacheFiles(cache_dir).empty());

    cancellation.enabled = false;
    // Reuse the Book through a different parser before returning to this EPUB.
    book.SetFolderName(TEST_FIXTURES_DIR "/books");
    book.SetFileName("basic.txt");
    book.format = FORMAT_UNDEF;
    test::ExpectEq("TXT opens after interrupted EPUB", book_parser::Open(&book), 0);
    const std::string other = BookText(&book);
    test::ExpectStrContains("recovery uses the requested TXT", other.c_str(), "tiny TXT fixture");
    test::ExpectTrue("TXT contains no interrupted EPUB content", other.find("FIRST-START") == std::string::npos);
    book.Close();
    book.SetFolderName(folder);
    book.SetFileName("valid.epub");
    book.format = FORMAT_EPUB;
    test::ExpectEq("same EPUB opens completely after cancellation", book_parser::Open(&book), 0);
    ExpectCompleteRecoveryEpub(&book);
    test::ExpectTrue("complete recovery can schedule a cache", book.HasPendingEpubPageCacheSave());
    book.Close();
    ExpectClosedReadingState(&book);
    const std::vector<std::string> caches = CacheFiles(cache_dir);
    test::ExpectEq("complete recovery creates one EPUB cache", (int)caches.size(), 1);
    test::ExpectEq("remove isolated recovery cache", remove(caches[0].c_str()), 0);
    printf("PASS: EPUB cancellation mode %d, partial-cache rejection and cross-format recovery\n", mode);
  }
  epub_page_cache::SetCacheDirForTest(nullptr);
}

void TestEpubXmlRecoveryAndZipFailure() {
  const char *folder = getenv("TEST_EPUB_RECOVERY_DIR");
  test::ExpectTrue("EPUB recovery folder supplied", folder != nullptr);
  const char *variants[] = {"valid", "badxml", "badcrc"};
  for (const char *variant : variants) {
    const std::string cache_dir = std::string(folder) + "/cache-" + variant;
    epub_page_cache::SetCacheDirForTest(cache_dir.c_str());
    Text text;
    CancelReporter reporter;
    BookContext ctx;
    ctx.text = &text;
    ctx.status_reporter = &reporter;
    Book book(ctx);
    book.SetFolderName(folder);
    book.SetFileName((std::string(variant) + ".epub").c_str());
    book.format = FORMAT_EPUB;
    const u8 result = book_parser::Open(&book);
    if (std::string(variant) == "badcrc") {
      test::ExpectEq("ZIP checksum failure rejects the opening", result, (u8)UNZ_CRCERROR);
      const std::string partial = BookText(&book);
      test::ExpectStrContains("CRC failure occurs after the valid first document", partial.c_str(), "FIRST-END");
      test::ExpectTrue("CRC failure stops before the last document", partial.find("FINAL-SPINE") == std::string::npos);
      test::ExpectFalse("CRC failure cannot schedule a cache", book.HasPendingEpubPageCacheSave());
      book.Close();
      ExpectClosedReadingState(&book);
      test::ExpectTrue("CRC failure writes no partial cache", CacheFiles(cache_dir).empty());
      book.SetFileName("valid.epub");
      test::ExpectEq("valid EPUB opens after fatal ZIP failure", book_parser::Open(&book), 0);
    } else {
      test::ExpectEq("valid ZIP with recoverable XHTML opens", result, 0);
    }
    ExpectCompleteRecoveryEpub(&book, std::string(variant) != "badxml");
    const text_search::Match before = ExpectSearch(&book, "FIRST-START link label");
    ExpectSearch(&book, "FINAL-SPINE");
    ExpectSearch(&book, "FIRST-END MIDDLE-SPINE", false); // Separate XHTML documents are barriers.
    book.Close();
    test::ExpectEq("reopen EPUB through actual page cache", book_parser::Open(&book), 0);
    test::ExpectFalse("cached EPUB does not schedule another save", book.HasPendingEpubPageCacheSave());
    const text_search::Match after = ExpectSearch(&book, "FIRST-START link label");
    test::ExpectTrue("cached EPUB retains search coordinates", before.start == after.start && before.end == after.end);
    ExpectSearch(&book, "FINAL-SPINE");
    book.Close();
    ExpectClosedReadingState(&book);
    for (const std::string &cache : CacheFiles(cache_dir))
      test::ExpectEq("remove isolated archive test cache", remove(cache.c_str()), 0);
    printf("PASS: EPUB %s, XML/ZIP error distinction and recovery\n", variant);
  }
  epub_page_cache::SetCacheDirForTest(nullptr);
}

void TestUtf8DetectionAndPassThrough() {
  const std::string utf8 = "\xC2\xA1Hola, se\xC3\xB1or!";

  bool used_utf8_guess = false;
  bool used_legacy_guess = false;
  const std::string detected =
      mobi_text_decode::DecodeBytesToUtf8(utf8, 0, &used_utf8_guess,
                                          &used_legacy_guess);
  test::ExpectStrEq("unknown encoding utf8 passthrough", detected.c_str(),
                    utf8.c_str());
  test::ExpectTrue("unknown encoding used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("unknown encoding used_legacy_guess", used_legacy_guess);

  used_utf8_guess = true;
  used_legacy_guess = true;
  const std::string explicit_utf8 =
      mobi_text_decode::DecodeBytesToUtf8(utf8, 65001, &used_utf8_guess,
                                          &used_legacy_guess);
  test::ExpectStrEq("explicit utf8 passthrough", explicit_utf8.c_str(),
                    utf8.c_str());
  test::ExpectFalse("explicit utf8 used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("explicit utf8 used_legacy_guess", used_legacy_guess);
}

void TestDecodeIso88591() {
  const std::string raw = std::string("Ol") + "\xE1" + " mundo";
  bool used_utf8_guess = true;
  bool used_legacy_guess = true;
  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8(raw, 28591, &used_utf8_guess,
                                          &used_legacy_guess);

  test::ExpectStrEq("iso-8859-1 decode", decoded.c_str(), "Ol\xC3\xA1 mundo");
  test::ExpectFalse("iso-8859-1 used_utf8_guess", used_utf8_guess);
  test::ExpectFalse("iso-8859-1 used_legacy_guess", used_legacy_guess);
}

void TestApplyEmbeddedTitleFromMetadata() {
  mobi_parser_core::MobiHeaderInfo header;
  header.encoding = 1252;
  header.mobi_full_name_off = 12;
  const std::string raw_title = std::string("  T") + "\xED" + "tulo   de   prueba  ";
  header.mobi_full_name_len = static_cast<u32>(raw_title.size());

  std::string rec0(80, '\0');
  rec0.replace(header.mobi_full_name_off, raw_title.size(), raw_title);

  std::string raw = rec0;
  raw.append("NEXT_RECORD");

  header.offsets.push_back(0);
  header.offsets.push_back(static_cast<u32>(rec0.size()));
  header.offsets.push_back(static_cast<u32>(raw.size()));

  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  mobi_text_decode::ApplyEmbeddedTitle(&book, raw, header);
  test::ExpectStrEq("embedded title extracted and normalized", book.GetTitle(),
                    "T\xC3\xADtulo de prueba");
}

void TestUnknownEncodingWithMalformedBytes() {
  const std::string malformed = std::string("A") + "\xFF\x80";
  bool used_utf8_guess = false;
  bool used_legacy_guess = false;

  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8(malformed, 0xFFFFFFFFu,
                                          &used_utf8_guess,
                                          &used_legacy_guess);

  test::ExpectTrue("malformed unknown picks legacy guess", used_legacy_guess);
  test::ExpectFalse("malformed unknown utf8 guess", used_utf8_guess);
  test::ExpectStrContains("malformed unknown keeps ascii", decoded.c_str(), "A");
  test::ExpectStrContains("malformed unknown maps cp1252 euro", decoded.c_str(),
                          "\xE2\x82\xAC");
}

void TestNullAndEmptyInputHandling() {
  bool used_utf8_guess = true;
  bool used_legacy_guess = true;
  const std::string decoded =
      mobi_text_decode::DecodeBytesToUtf8("", 1252, &used_utf8_guess,
                                          &used_legacy_guess);
  test::ExpectStrEq("empty decode", decoded.c_str(), "");
  test::ExpectFalse("empty decode utf8 guess", used_utf8_guess);
  test::ExpectFalse("empty decode legacy guess", used_legacy_guess);

  mobi_parser_core::MobiHeaderInfo header;
  header.mobi_full_name_len = 10;
  Text text;
  BookContext ctx;
  ctx.text = &text;
  Book book(ctx);
  book.SetTitle("unchanged");
  mobi_text_decode::ApplyEmbeddedTitle(nullptr, "", header);
  mobi_text_decode::ApplyEmbeddedTitle(&book, "", header);
  test::ExpectStrEq("null/empty title apply leaves title unchanged",
                    book.GetTitle(), "unchanged");
}

} // namespace

int main(int argc, char **argv) {
  if(argc==2) {
    if(std::string(argv[1])=="fb2-storage") TestFb2ResourceStorageLimitsAndRecovery();
    else if(std::string(argv[1])=="image-cache") TestInlineImageCacheRetainsDistinctDimensions();
    else return 2;
    return 0;
  }
  TestFb2ResourceStorageLimitsAndRecovery();
  TestInlineImageCacheRetainsDistinctDimensions();
  TestOriginalImageResourceAndDrawBounds();
  TestImageResourcesAcrossBookFormats();
  TestDispatchContentAndCloseBetweenFormats();
  TestOdtSearch();
  TestFb2NestedNavigation();
  TestOpenErrorMessages();
  TestEpubMetadataOpenCloseRecovery();
  TestCbzReadPageZoomCloseAndReopen();
  TestCancelledOpenAndRecovery();
  TestMobiParseCloseCachedReopenAndRecovery();
  TestEpubInterruptedSpineAndRecovery();
  TestEpubXmlRecoveryAndZipFailure();
  TestDecodeCp1252();
  TestUtf8DetectionAndPassThrough();
  TestDecodeIso88591();
  TestApplyEmbeddedTitleFromMetadata();
  TestUnknownEncodingWithMalformedBytes();
  TestNullAndEmptyInputHandling();
  return 0;
}
