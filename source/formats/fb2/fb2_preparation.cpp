#include "formats/fb2/fb2_preparation.h"
#include "book/book.h"
#include "book/book_parse_deps.h"
#include "book/book_xml.h"
#include "formats/common/plain_parser.h"
#include "formats/common/page_text_extract_utils.h"
#include "parse.h"
#include "ui/text.h"
#include <algorithm>
#include <memory>
#include <new>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fb2_parser {
struct Preparation::State {
  Book *book = NULL;
  parsedata_t data;
  XML_Parser parser = NULL;
  int fd = -1;
  size_t size = 0, read_bytes = 0, fed = 0, capacity = 65536, cursor = 0,
         available = 0;
  std::unique_ptr<char[]> buffer;
  bool paused = false, suspended = false, final = false, structured = false;
  enum Phase { Xml, Fallback, Done } phase = Xml;
  ReflowPreparation::Result result = ReflowPreparation::Pending;
  unsigned page = 0, line_page = 0;
  size_t line = 0;
  std::vector<std::string> lines;
  std::string pending;
  bool have_pending = false, prev_blank = true, prev_candidate = false;
  ~State() {
    if (parser)
      XML_ParserFree(parser);
    if (fd >= 0)
      close(fd);
  }
  void MaybePause() {
    if (!paused)
      return;
    XML_ParsingStatus status;
    XML_GetParsingStatus(parser, &status);
    if (status.parsing == XML_PARSING)
      XML_StopParser(parser, XML_TRUE);
  }
  static void Start(void *ctx, const char *name, const char **attrs) {
    State &s = *static_cast<State *>(ctx);
    xml::book::start(&s.data, name, attrs);
    s.MaybePause();
  }
  static void End(void *ctx, const char *name) {
    State &s = *static_cast<State *>(ctx);
    xml::book::end(&s.data, name);
    s.MaybePause();
  }
  static void TextData(void *ctx, const char *data, int n) {
    State &s = *static_cast<State *>(ctx);
    xml::book::chardata(&s.data, data, n);
    s.MaybePause();
  }
  static void Default(void *ctx, const char *data, int n) {
    State &s = *static_cast<State *>(ctx);
    xml::book::fallback(&s.data, data, n);
    s.MaybePause();
  }
  static void Instruction(void *ctx, const char *target, const char *data) {
    State &s = *static_cast<State *>(ctx);
    xml::book::instruction(&s.data, target, data);
    s.MaybePause();
  }
  void AcceptPending(const std::string *next) {
    if (!have_pending)
      return;
    bool strong = false, next_strong = false;
    const bool candidate =
        plain_parser::LooksLikePlainChapterHeading(pending, &strong);
    const bool next_candidate =
        next && plain_parser::LooksLikePlainChapterHeading(*next, &next_strong);
    const bool next_blank = !next || plain_parser::IsBlankLine(*next);
    if (candidate && plain_parser::ShouldAcceptHeuristicHeading(
                         pending, prev_blank, next_blank, prev_candidate,
                         next_candidate, strong))
      plain_parser::AddChapterAtPageIfUnique(book, line_page, pending, 0);
    prev_blank = plain_parser::IsBlankLine(pending);
    prev_candidate = candidate;
  }
};
Preparation::Preparation() : state_(NULL) {}
Preparation::~Preparation() {
  delete state_;
}
bool Preparation::Begin(Book *book, const std::string &source) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_ || !book)
    return false;
  State &s = *state_;
  s.book = book;
  s.fd = open(source.c_str(), O_RDONLY);
  struct stat info;
  if (s.fd < 0 || fstat(s.fd, &info) != 0) {
    s.result = ReflowPreparation::IoError;
    return false;
  }
  if (info.st_size < 0 || (uint64_t)info.st_size > 0xffffffffU) {
    s.result = ReflowPreparation::Failed;
    return false;
  }
  s.size = info.st_size;
  s.buffer.reset(new (std::nothrow) char[s.capacity]);
  if (!s.buffer) {
    s.capacity = 16384;
    s.buffer.reset(new (std::nothrow) char[s.capacity]);
  }
  if (!s.buffer) {
    s.result = ReflowPreparation::NoMemory;
    return false;
  }
  parse_init(&s.data);
  const BookParseDeps deps = BuildBookParseDeps(book);
  s.data.reporter = deps.reporter;
  s.data.book = book;
  s.data.ts = deps.ts;
  s.data.prefs = deps.prefs;
  s.data.fb2_mode = true;
  if (deps.ts) {
    s.data.pen.x = deps.ts->margin.left;
    s.data.pen.y = deps.ts->margin.top + deps.ts->GetHeight();
  }
  s.parser = XML_ParserCreate(NULL);
  if (!s.parser) {
    s.result = ReflowPreparation::NoMemory;
    return false;
  }
  XML_SetUserData(s.parser, &s);
  XML_SetElementHandler(s.parser, State::Start, State::End);
  XML_SetCharacterDataHandler(s.parser, State::TextData);
  XML_SetDefaultHandler(s.parser, State::Default);
  XML_SetProcessingInstructionHandler(s.parser, State::Instruction);
  return true;
}
void Preparation::SignalPause() {
  if (state_)
    state_->paused = true;
}
void Preparation::SignalResume() {
  if (state_)
    state_->paused = false;
}
ReflowPreparation::Result Preparation::GetResult() const {
  return state_ ? state_->result : ReflowPreparation::NoMemory;
}
ReflowPreparation::Result Preparation::Step() {
  if (!state_)
    return ReflowPreparation::Failed;
  State &s = *state_;
  if (s.paused)
    return ReflowPreparation::Paused;
  if (s.result != ReflowPreparation::Pending)
    return s.result;
  if (s.phase == State::Xml) {
    XML_Status status;
    if (s.suspended) {
      status = XML_ResumeParser(s.parser);
      s.suspended = false;
    } else {
      if (s.cursor == s.available && s.read_bytes < s.size) {
        const size_t n = std::min(s.capacity, s.size - s.read_bytes);
        const ssize_t got = read(s.fd, s.buffer.get(), n);
        if (got != (ssize_t)n)
          return s.result = ReflowPreparation::IoError;
        s.read_bytes += n;
        s.cursor = 0;
        s.available = n;
        return ReflowPreparation::Pending;
      }
      const size_t n = std::min<size_t>(8192, s.available - s.cursor);
      s.final = s.fed + n == s.size;
      status = XML_Parse(s.parser, s.buffer.get() + s.cursor, n, s.final);
      s.cursor += n;
      s.fed += n;
    }
    if (status == XML_STATUS_SUSPENDED) {
      s.suspended = true;
      return ReflowPreparation::Paused;
    }
    if (status == XML_STATUS_ERROR)
      return s.result = XML_GetErrorCode(s.parser) == XML_ERROR_NO_MEMORY
                            ? ReflowPreparation::NoMemory
                            : ReflowPreparation::Failed;
    if (s.final) {
      XML_ParserFree(s.parser);
      s.parser = NULL;
      close(s.fd);
      s.fd = -1;
      s.buffer.reset();
      if (!s.book->GetPageCount())
        return s.result = ReflowPreparation::Failed;
      s.structured = !s.book->GetChapters().empty();
      s.phase = State::Fallback;
    }
  } else if (s.phase == State::Fallback) {
    if (!s.structured) {
      if (s.line == s.lines.size() && s.page < s.book->GetPageCount()) {
        s.lines = page_text_extract_utils::ExtractTextLinesFromPage(
            s.book->GetPage(s.page));
        s.line = 0;
        ++s.page;
      }
      if (s.line < s.lines.size()) {
        const std::string &next = s.lines[s.line++];
        s.AcceptPending(&next);
        s.pending = next;
        s.line_page = s.page - 1;
        s.have_pending = true;
        return ReflowPreparation::Pending;
      }
      if (s.page < s.book->GetPageCount())
        return ReflowPreparation::Pending;
      s.AcceptPending(NULL);
      s.have_pending = false;
    }
    if (!s.book->GetChapters().empty())
      plain_parser::SetNonEpubTocConfidence(s.book, s.structured);
    else
      s.book->ClearTocConfidence();
    s.phase = State::Done;
    s.result = ReflowPreparation::Complete;
  }
  return s.result;
}
const char *Preparation::Label() const {
  return state_ && state_->phase == State::Xml ? "processing XML... B: cancel"
                                               : "preparing index... B: cancel";
}
unsigned Preparation::Progress() const {
  if (!state_)
    return 0;
  if (state_->phase != State::Xml)
    return state_->page;
  if (state_->suspended)
    return std::max<XML_Index>(0, XML_GetCurrentByteIndex(state_->parser));
  return state_->fed;
}
unsigned Preparation::Total() const {
  return !state_                       ? 0
         : state_->phase == State::Xml ? state_->size
                                       : state_->book->GetPageCount();
}
} // namespace fb2_parser
