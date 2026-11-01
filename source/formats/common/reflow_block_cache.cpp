#include "formats/common/reflow_block_cache.h"
#include "formats/common/page_cache_codec.h"
#include "formats/common/cache_memory_utils.h"
#include "formats/common/page_cache_utils.h"
#include "book/book.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "shared/debug_log.h"
#include <3ds.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace reflow_block_cache {
namespace {
uint32_t Magic(Format f) { return f == Mobi ? 0x4d504347U : f == Fb2 ? 0x46504347U : 0x45504347U; }
unsigned Version(Format f) { return f == Mobi ? 27 : f == Fb2 ? 1 : 23; }
struct Resource {
  std::string id;
  uint64_t offset;
  uint32_t size, crc;
};
const unsigned kIndexLimit = 4 * 1024 * 1024, kBytes = 65536,
               kNoBlock = 0xffffffffU;
struct Header {
  uint32_t magic;
  uint16_t version, flags;
  uint32_t pages, blocks;
  uint64_t index_offset;
  uint32_t index_size, index_crc;
};
static_assert(sizeof(Header) == 32, "stable v23 header");
struct Ref {
  uint32_t block, offset;
  uint16_t count;
  Ref() : block(kNoBlock), offset(0), count(0) {}
};
struct PageRef {
  Ref text, search;
  uint16_t links;
  PageRef() : links(0) {}
};
struct Block {
  uint64_t offset;
  uint32_t size, encoded, words, crc;
  uint16_t document;
  uint8_t compressed, kind;
};
uint32_t Crc(const void *p, size_t n) {
  return (uint32_t)crc32(0, (const Bytef *)p, n);
}
struct IndexWriter {
  std::vector<uint8_t> data;
  bool ok;
  IndexWriter() : ok(true) {}
  template <typename T> void Put(T value) {
    if (data.size() + sizeof(T) > kIndexLimit) {
      ok = false;
      return;
    }
    for (size_t i = 0; i < sizeof(T); ++i)
      data.push_back((uint8_t)((uint64_t)value >> (i * 8)));
  }
  void String(const std::string &s, size_t max = 65535, bool display = false) {
    if (!display && s.size() > max) {
      ok = false;
      return;
    }
    size_t n = std::min(s.size(), max);
    // Display labels may shorten; resource identities must remain exact.
    while (display && n < s.size() && n && ((unsigned char)s[n] & 0xc0) == 0x80)
      --n;
    Put<uint16_t>(n);
    if (data.size() + n > kIndexLimit) {
      ok = false;
      return;
    }
    data.insert(data.end(), s.begin(), s.begin() + n);
  }
};
struct IndexReader {
  const uint8_t *p, *end;
  bool ok;
  explicit IndexReader(const std::vector<uint8_t> &d)
      : p(d.data()), end(d.data() + d.size()), ok(true) {}
  template <typename T> T Get() {
    if ((size_t)(end - p) < sizeof(T)) {
      ok = false;
      return 0;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
      value |= (uint64_t)*p++ << (8 * i);
    return (T)value;
  }
  std::string String(size_t max = 65535) {
    const unsigned n = Get<uint16_t>();
    if (n > max || (size_t)(end - p) < n) {
      ok = false;
      return std::string();
    }
    std::string out((const char *)p, n);
    p += n;
    return out;
  }
};
void PutRef(IndexWriter &w, const Ref &r) {
  w.Put<uint32_t>(r.block);
  w.Put<uint32_t>(r.offset);
  w.Put<uint16_t>(r.count);
}
Ref GetRef(IndexReader &r) {
  Ref v;
  v.block = r.Get<uint32_t>();
  v.offset = r.Get<uint32_t>();
  v.count = r.Get<uint16_t>();
  return v;
}
void PutReferences(IndexWriter &w,
                   const std::unordered_map<std::string, u16> &map,
                   unsigned pages) {
  unsigned count = 0;
  for (const auto &v : map)
    if (v.second < pages)
      ++count;
  w.Put<uint32_t>(count);
  for (const auto &v : map)
    if (v.second < pages) {
      w.Put<uint16_t>(v.second);
      w.String(v.first);
    }
}
bool Publish(const std::string &path, const std::string &temporary) {
  const std::string backup = path + ".bak";
  struct stat info;
  const bool previous = stat(path.c_str(), &info) == 0;
  if ((previous && !S_ISREG(info.st_mode)) || (!previous && errno != ENOENT))
    return false;
  if (previous && ((remove(backup.c_str()) != 0 && errno != ENOENT) ||
                   rename(path.c_str(), backup.c_str()) != 0))
    return false;
  if (rename(temporary.c_str(), path.c_str()) != 0) {
    if (previous)
      rename(backup.c_str(), path.c_str());
    return false;
  }
  if (previous)
    remove(backup.c_str());
  return true;
}
} // namespace

struct Writer::State {
  Book *book;
  std::string path, temporary;
  int fd;
  Result result;
  Format format = Epub;
  std::vector<Resource> resources;
  size_t resource_next = 0, resource_cursor = 0;
  std::vector<PageRef> refs;
  std::vector<Block> blocks;
  std::vector<uint8_t> encoded, packed, staging, index;
  page_cache_codec::Compressor compressor;
  uint64_t logical;
  unsigned first, end, next, document, words, kind;
  unsigned completed_records = 0;
  enum Phase {
    Idle,
    Records,
    Drain,
    Resources,
    Index,
    Tail,
    HeaderCommit,
    PublishFile,
    Done
  } phase;
  Phase after_drain;
  size_t pending_offset, index_offset;
  bool compressed, finishing;
  u64 started, io_ms = 0, last_slow = 0;
  size_t calls;
  State()
      : book(NULL), fd(-1), result(Ready), logical(sizeof(Header)), first(0),
        end(0), next(0), document(0), words(0), kind(0), phase(Idle),
        after_drain(Records), pending_offset(0), index_offset(0),
        compressed(false), finishing(false), started(osGetTime()), calls(0) {
    encoded.reserve(kBytes);
    staging.reserve(kBytes);
  }
  ~State() {
    if (fd >= 0)
      close(fd);
    if (!temporary.empty())
      remove(temporary.c_str());
  }
  bool Write(const void *p, size_t n) {
    const u64 begin = osGetTime();
    ++calls;
    const bool ok = write(fd, p, n) == (ssize_t)n;
    const u64 now = osGetTime(), elapsed = now - begin;
    io_ms += elapsed;
    if (elapsed >= 100 && (!last_slow || now - last_slow >= 5000)) {
      last_slow = now;
      DBG_LOGF(book->GetStatusReporter(),
               "CACHE IO slow write bytes=%u ms=%llu", (unsigned)n,
               (unsigned long long)elapsed);
    }
    return ok;
  }
  bool Queue(const uint8_t *p, size_t n, size_t &offset) {
    while (offset < n) {
      const size_t part = std::min(n - offset, kBytes - staging.size());
      staging.insert(staging.end(), p + offset, p + offset + part);
      offset += part;
      logical += part;
      if (staging.size() == kBytes) {
        if (!Write(staging.data(), staging.size())) {
          result = IoError;
          return false;
        }
        staging.clear();
        return false; // Yield after one data write.
      }
    }
    return true;
  }
  bool FlushBlock(Phase after) {
    if (!words) {
      phase = after;
      return true;
    }
    if (blocks.size() >= 50000 ||
        !compressor.Pack(encoded, &packed, &compressed)) {
      result = NoMemory;
      return false;
    }
    Block block = {logical,
                   (uint32_t)packed.size(),
                   (uint32_t)encoded.size(),
                   words,
                   Crc(encoded.data(), encoded.size()),
                   (uint16_t)document,
                   (uint8_t)compressed,
                   (uint8_t)kind};
    blocks.push_back(block);
    phase = Drain;
    after_drain = after;
    pending_offset = 0;
    return true;
  }
  void BuildIndex() {
    IndexWriter w;
    for (const PageRef &p : refs) {
      PutRef(w, p.text);
      PutRef(w, p.search);
      w.Put<uint16_t>(p.links);
    }
    for (const Block &b : blocks) {
      w.Put<uint64_t>(b.offset);
      w.Put<uint32_t>(b.size);
      w.Put<uint32_t>(b.encoded);
      w.Put<uint32_t>(b.words);
      w.Put<uint32_t>(b.crc);
      w.Put<uint16_t>(b.document);
      w.Put<uint8_t>(b.compressed);
      w.Put<uint8_t>(b.kind);
    }
    w.String(book->GetTitle() ? book->GetTitle() : "", 1000, true);
    w.Put<uint8_t>(book->GetTocQuality());
    w.Put<uint16_t>(book->GetTocDirectCount());
    w.Put<uint16_t>(book->GetTocHeuristicCount());
    w.Put<uint16_t>(book->GetTocUnresolvedCount());
    w.Put<uint32_t>(book->GetChapters().size());
    for (const auto &c : book->GetChapters()) {
      w.Put<uint16_t>(c.page);
      w.Put<uint8_t>(c.level);
      w.String(c.title, 2048, true);
    }
    PutReferences(w, book->GetChapterDocStartPages(), refs.size());
    PutReferences(w, book->GetChapterAnchorPages(), refs.size());
    w.Put<uint32_t>(book->GetInlineImageCount());
    for (unsigned i = 0; i < book->GetInlineImageCount(); ++i) {
      w.String(*book->GetInlineImagePath(i));
      if (format != Epub) {
        const auto &image = book->inline_images[i];
        w.Put<uint8_t>(image.follow_text_lines);
        w.Put<uint8_t>((image.metadata_probed ? 1 : 0) | (image.metadata_ok ? 2 : 0));
        w.Put<uint32_t>(image.source_width);
        w.Put<uint32_t>(image.source_height);
      }
    }
    w.Put<uint32_t>(book->GetInlineLinkHrefCount());
    for (unsigned i = 1; i <= book->GetInlineLinkHrefCount(); ++i)
      w.String(*book->GetInlineLinkHref(i));
    if (format == Fb2) {
      w.Put<uint32_t>(resources.size());
      for (const auto &r : resources) {
        w.String(r.id);
        w.Put<uint64_t>(r.offset);
        w.Put<uint32_t>(r.size);
        w.Put<uint32_t>(r.crc);
      }
    }
    if (!w.ok || refs.size() != book->GetPageCount()) {
      result = IoError;
      return;
    }
    index.swap(w.data);
    index_offset = 0;
    phase = Index;
  }
};
Writer::Writer() : state_(NULL) {}
Writer::~Writer() {
  delete state_;
}
void Writer::Abort() {
  delete state_;
  state_ = NULL;
}
Result Writer::GetResult() const {
  return state_ ? state_->result : Cancelled;
}
unsigned Writer::Progress() const {
  return state_ ? state_->completed_records / 2 : 0;
}
bool Writer::Begin(Book *book, const std::string &path, Format format) {
  Abort();
  state_ = new (std::nothrow) State;
  if (!state_)
    return false;
  State &s = *state_;
  s.book = book;
  s.format = format;
  s.path = path;
  s.temporary = path + ".tmp";
  s.fd = open(s.temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  Header h = {Magic(format), (uint16_t)Version(format), 0, 0, 0, 0, 0, 0};
  if (!book || s.fd < 0 || !s.Write(&h, sizeof(h))) {
    s.result = IoError;
    return false;
  }
  return true;
}
bool Writer::AppendDocument(unsigned first, unsigned end) {
  if (!state_ || state_->phase != State::Idle || state_->result != Ready ||
      first != state_->refs.size() || end < first ||
      end > state_->book->GetPageCount())
    return false;
  State &s = *state_;
  s.refs.resize(end);
  s.first = first;
  s.end = end;
  s.next = first;
  s.kind = 0;
  s.phase = State::Records;
  s.result = Pending;
  return true;
}
bool Writer::Finish() {
  if (!state_ || state_->result != Ready || !state_->book->GetPageCount())
    return false;
  state_->finishing = true;
  if (state_->format == Fb2) {
    uint64_t offset = state_->logical;
    for (const auto &item : state_->book->fb2_inline_images) {
      if (item.second.empty()) continue;
      Resource r = {item.first, offset, (uint32_t)item.second.size(), 0};
      if (r.size > 4u*1024u*1024u || offset - state_->logical + r.size > 12u*1024u*1024u) return false;
      state_->resources.push_back(r);
      offset += r.size;
    }
    state_->phase = State::Resources;
  } else state_->BuildIndex();
  if (state_->result != IoError)
    state_->result = Pending;
  return state_->result == Pending;
}
Result Writer::Step() {
  if (!state_ || state_->result != Pending)
    return GetResult();
  State &s = *state_;
  const u64 start = osGetTime();
  do {
    if (s.phase == State::Drain) {
      if (!s.Queue(s.packed.data(), s.packed.size(), s.pending_offset))
        return s.result;
      s.encoded.clear();
      s.packed.clear();
      s.words = 0;
      s.phase = s.after_drain;
    } else if (s.phase == State::Records) {
      if (s.next == s.end) {
        if (s.words) {
          s.FlushBlock(State::Records);
          continue;
        }
        if (!s.kind) {
          s.kind = 1;
          s.next = s.first;
          continue;
        }
        ++s.document;
        s.phase = State::Idle;
        s.result = Ready;
        return Ready;
      }
      Page *page = s.book->GetPage(s.next);
      if (!page || page->GetLength() < 0 || page->GetLength() > 4096) {
        s.result = Corrupt;
        break;
      }
      const u32 *data =
          s.kind ? page->GetSearchEvents().data() : page->GetBuffer();
      const size_t count =
          s.kind ? page->GetSearchEvents().size() : page->GetLength();
      if (s.kind &&
          !page_search::Valid(page->GetSearchEvents(), page->GetLength())) {
        s.result = Corrupt;
        break;
      }
      if (s.words + count > kBytes / 4 ||
          !page_cache_codec::Append(data, count, &s.encoded)) {
        s.FlushBlock(State::Records);
        continue;
      }
      Ref &ref = s.kind ? s.refs[s.next].search : s.refs[s.next].text;
      ref.block = count ? s.blocks.size() : kNoBlock;
      ref.offset = s.words;
      ref.count = count;
      s.refs[s.next].links = page->GetSelectableCount();
      s.words += count;
      ++s.next;
      ++s.completed_records;
    } else if (s.phase == State::Resources) {
      if (s.resource_next == s.resources.size()) {
        s.BuildIndex();
        continue;
      }
      Resource &r = s.resources[s.resource_next];
      const auto item = s.book->fb2_inline_images.find(r.id);
      if (item == s.book->fb2_inline_images.end() || item->second.size() != r.size) return s.result = Corrupt;
      const size_t before = s.resource_cursor;
      const bool done = s.Queue(item->second.data(), r.size, s.resource_cursor);
      r.crc = crc32(r.crc, item->second.data()+before, s.resource_cursor-before);
      if (!done) return s.result;
      ++s.resource_next;
      s.resource_cursor = 0;
    } else if (s.phase == State::Index) {
      if (!s.Queue(s.index.data(), s.index.size(), s.index_offset))
        return s.result;
      s.phase = State::Tail;
    } else if (s.phase == State::Tail) {
      if (!s.staging.empty()) {
        if (!s.Write(s.staging.data(), s.staging.size())) {
          s.result = IoError;
          break;
        }
        s.staging.clear();
        s.phase = State::HeaderCommit;
        return Pending;
      }
      s.phase = State::HeaderCommit;
    } else if (s.phase == State::HeaderCommit) {
      Header h = {Magic(s.format),
                  (uint16_t)Version(s.format),
                  0,
                  (uint32_t)s.refs.size(),
                  (uint32_t)s.blocks.size(),
                  s.logical - s.index.size(),
                  (uint32_t)s.index.size(),
                  Crc(s.index.data(), s.index.size())};
      if (lseek(s.fd, 0, SEEK_SET) < 0 || !s.Write(&h, sizeof(h))) {
        s.result = IoError;
        break;
      }
      if (close(s.fd) != 0) {
        s.fd = -1;
        s.result = IoError;
        break;
      }
      s.fd = -1;
      s.phase = State::PublishFile;
      return Pending;
    } else if (s.phase == State::PublishFile) {
      if (!Publish(s.path, s.temporary)) {
        s.result = IoError;
        break;
      }
      s.temporary.clear();
      s.result = Complete;
      s.phase = State::Done;
      DBG_LOGF(s.book->GetStatusReporter(),
               "CACHE IO save bytes=%llu calls=%u ms=%llu io_ms=%llu pages=%u",
               (unsigned long long)s.logical, (unsigned)s.calls,
               (unsigned long long)(osGetTime() - s.started),
               (unsigned long long)s.io_ms, (unsigned)s.refs.size());
    }
  } while (s.result == Pending && osGetTime() - start < 50);
  return s.result;
}

struct Reader::State {
  Book *book;
  std::string path;
  int fd;
  Result result;
  Header header;
  Format format = Epub;
  std::vector<Resource> resources;
  size_t file_size, index_read, budget, read_bytes, payload_bytes,
      selection_bytes;
  std::vector<size_t> selection_sizes;
  unsigned target, full_next, pinned;
  unsigned phase;
  u64 tick, started = osGetTime(), io_ms = 0, last_slow = 0;
  size_t read_calls = 0;
  bool logged = false, validate_only = false, memory_retry_used = false;
  std::vector<uint8_t> index;
  std::vector<PageRef> refs;
  std::vector<Block> blocks;
  std::vector<bool> ready;
  std::vector<u64> ages;
  struct CachedBlock {
    unsigned id;
    std::vector<u32> words;
    u64 age;
  };
  std::vector<CachedBlock> cache;
  std::unique_ptr<uint8_t[]> read_window;
  size_t window_capacity = 0, window_size = 0;
  uint64_t window_offset = 0, fd_offset = 0;
  State()
      : book(NULL), fd(-1), result(Missing), file_size(0), index_read(0),
        budget(0), read_bytes(0), payload_bytes(0), selection_bytes(0),
        target(0), full_next(0), pinned(0), phase(0), tick(0) {
    memset(&header, 0, sizeof(header));
  }
  ~State() {
    if (fd >= 0)
      close(fd);
  }
  bool ReadAt(uint64_t offset, void *p, size_t n) {
    if (offset > file_size || n > file_size - offset)
      return false;
    const u64 begin = osGetTime();
    if (offset != fd_offset && lseek(fd, offset, SEEK_SET) < 0)
      return false;
    ++read_calls;
    const ssize_t got = read(fd, p, n);
    if (got > 0) {
      read_bytes += got;
      fd_offset = offset + got;
    }
    const u64 now = osGetTime(), elapsed = now - begin;
    io_ms += elapsed;
    if (elapsed >= 100 && (!last_slow || now - last_slow >= 5000)) {
      last_slow = now;
      DBG_LOGF(book->GetStatusReporter(), "CACHE IO slow read bytes=%u ms=%llu",
               (unsigned)n, (unsigned long long)elapsed);
    }
    return got == (ssize_t)n;
  }
  Result ReadBlock(const Block &b, uint8_t *out) {
    if (!read_window) {
      window_capacity = kBytes;
      read_window.reset(new (std::nothrow) uint8_t[window_capacity]);
      if (!read_window) {
        window_capacity = 16384;
        read_window.reset(new (std::nothrow) uint8_t[window_capacity]);
      }
      if (!read_window)
        return NoMemory;
    }
    if (b.size > window_capacity)
      return ReadAt(b.offset, out, b.size) ? Ready : IoError;
    if (b.offset < window_offset || b.offset - window_offset > window_size ||
        b.size > window_size - (size_t)(b.offset - window_offset)) {
      window_offset = b.offset;
      window_size =
          std::min<uint64_t>(window_capacity, header.index_offset - b.offset);
      if (!ReadAt(window_offset, read_window.get(), window_size)) {
        window_size = 0;
        return IoError;
      }
    }
    memcpy(out, read_window.get() + (size_t)(b.offset - window_offset), b.size);
    return Ready;
  }
  bool ValidRef(const Ref &r, unsigned kind) {
    if (!r.count)
      return r.block == kNoBlock;
    return r.block < blocks.size() && blocks[r.block].kind == kind &&
           r.offset <= blocks[r.block].words &&
           r.count <= blocks[r.block].words - r.offset;
  }
  bool ParseIndex() {
    IndexReader r(index);
    refs.resize(header.pages);
    blocks.resize(header.blocks);
    for (PageRef &p : refs) {
      p.text = GetRef(r);
      p.search = GetRef(r);
      p.links = r.Get<uint16_t>();
    }
    uint64_t offset = sizeof(Header);
    for (Block &b : blocks) {
      b.offset = r.Get<uint64_t>();
      b.size = r.Get<uint32_t>();
      b.encoded = r.Get<uint32_t>();
      b.words = r.Get<uint32_t>();
      b.crc = r.Get<uint32_t>();
      b.document = r.Get<uint16_t>();
      b.compressed = r.Get<uint8_t>();
      b.kind = r.Get<uint8_t>();
      if (!r.ok || b.offset != offset || !b.size || b.size > kBytes ||
          b.encoded > kBytes || !b.words || b.words > kBytes / 4 ||
          b.compressed > 1 || b.kind > 1 || b.offset > header.index_offset ||
          b.size > header.index_offset - b.offset)
        return false;
      offset += b.size;
    }
    const uint64_t resource_start = offset;
    if (format != Fb2 && offset != header.index_offset) return false;
    for (const PageRef &p : refs)
      if (p.text.count > 4096 || p.search.count > p.text.count + 1 ||
          !ValidRef(p.text, 0) || !ValidRef(p.search, 1))
        return false;
    const std::string title = r.String(1000);
    const unsigned quality = r.Get<uint8_t>();
    const u16 direct = r.Get<uint16_t>(), heuristic = r.Get<uint16_t>(),
              unresolved = r.Get<uint16_t>();
    if (quality > TOC_QUALITY_HEURISTIC || !r.ok)
      return false;
    if (!validate_only) {
      book->ReservePageCapacity(header.pages);
      for (unsigned i = 0; i < header.pages; ++i)
        book->AppendPage();
      book->SetTitle(title.c_str());
      book->SetTocConfidence((TocQuality)quality, direct, heuristic,
                             unresolved);
    }
    unsigned count = r.Get<uint32_t>();
    if (count > 4000)
      return false;
    for (unsigned i = 0; i < count; ++i) {
      const u16 page = r.Get<uint16_t>();
      const u8 level = r.Get<uint8_t>();
      const std::string text = r.String(2048);
      if (!r.ok || page >= header.pages)
        return false;
      if (!validate_only)
        book->AddChapter(page, text, level);
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
      count = r.Get<uint32_t>();
      if (count > (kind ? 8192U : 4000U))
        return false;
      for (unsigned i = 0; i < count; ++i) {
        const u16 page = r.Get<uint16_t>();
        const std::string text = r.String();
        if (!r.ok || page >= header.pages)
          return false;
        if (!validate_only) {
          if (kind)
            book->SetChapterAnchorPage(text, page);
          else
            book->SetChapterDocStartPage(text, page);
        }
      }
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
      count = r.Get<uint32_t>();
      if (count > 65535)
        return false;
      for (unsigned i = 0; i < count; ++i) {
        const std::string text = r.String();
        if (!r.ok || text.empty())
          return false;
        unsigned follow = 0, flags = 0, width = 0, height = 0;
        if (!kind && format != Epub) {
          follow = r.Get<uint8_t>(); flags = r.Get<uint8_t>();
          width = r.Get<uint32_t>(); height = r.Get<uint32_t>();
          if ((flags & ~3U) || ((flags & 2) && (!(flags & 1) || !width || !height)) ||
              width > 0x7fffffffU || height > 0x7fffffffU) return false;
        }
        if (!r.ok) return false;
        if (validate_only) continue;
        if (kind) {
          if (!book->RegisterInlineLinkHref(text))
            return false;
        } else {
          const u16 id = book->RegisterInlineImage(text);
          book->SetInlineImageFollowTextLines(id, follow);
          if (format != Epub && id < book->inline_images.size()) {
            auto &image = book->inline_images[id];
            image.metadata_probed = flags & 1; image.metadata_ok = flags & 2;
            image.source_width = width; image.source_height = height;
          }
        }
      }
    }
    if (format == Fb2) {
      const unsigned count = r.Get<uint32_t>();
      if (count > 65535) return false;
      uint64_t next = resource_start;
      std::unordered_map<std::string, bool> seen;
      for (unsigned i = 0; i < count; ++i) {
        Resource v;
        v.id = r.String(); v.offset = r.Get<uint64_t>();
        v.size = r.Get<uint32_t>(); v.crc = r.Get<uint32_t>();
        if (!r.ok || v.id.empty() || !seen.emplace(v.id, true).second ||
            v.offset != next || !v.size || v.size > 4u*1024u*1024u ||
            next > header.index_offset || v.size > header.index_offset-next ||
            next-resource_start+v.size > 12u*1024u*1024u) return false;
        next += v.size; resources.push_back(v);
      }
      if (next != header.index_offset) return false;
    }
    if (!r.ok || r.p != r.end)
      return false;
    ready.assign(header.pages, false);
    ages.assign(header.pages, 0);
    selection_sizes.assign(header.pages, 0);
    std::vector<uint8_t>().swap(index);
    return true;
  }
  CachedBlock *Find(unsigned id) {
    for (auto &b : cache)
      if (b.id == id) {
        b.age = ++tick;
        return &b;
      }
    return NULL;
  }
  void UpdateSelections(unsigned page) {
    if (page >= ready.size() || !ready[page])
      return;
    const size_t bytes =
        book->GetPage(page)->GetRenderedSelectables().capacity() *
        sizeof(Page::SelectableRenderEntry);
    selection_bytes -= selection_sizes[page];
    selection_bytes += bytes;
    selection_sizes[page] = bytes;
  }
  size_t Memory() const {
    size_t bytes = payload_bytes + selection_bytes;
    for (const auto &b : cache)
      bytes += 4 * b.words.capacity();
    return bytes;
  }
  void Trim(size_t incoming, unsigned wanted_block = kNoBlock) {
    const size_t limit = budget ? budget : 2 * kBytes;
    // Full loading retains Page buffers; only its bounded decode blocks evict.
    auto usage = [&]() {
      if (budget)
        return Memory();
      size_t n = 0;
      for (const auto &b : cache)
        n += 4 * b.words.capacity();
      return n;
    };
    while (usage() + incoming > limit) {
      size_t oldest = cache.size();
      for (size_t i = 0; i < cache.size(); ++i)
        if (cache[i].id != wanted_block &&
            (!budget || ready[target] ||
             (cache[i].id != refs[target].text.block &&
              cache[i].id != refs[target].search.block)) &&
            (oldest == cache.size() || cache[i].age < cache[oldest].age))
          oldest = i;
      if (oldest != cache.size()) {
        cache.erase(cache.begin() + oldest);
        continue;
      }
      if (!budget)
        break;
      unsigned page = header.pages;
      for (unsigned i = 0; i < ready.size(); ++i)
        if (ready[i] && i != pinned && i != target &&
            (page == header.pages || ages[i] < ages[page]))
          page = i;
      if (page == header.pages)
        break;
      payload_bytes -=
          4 * ((size_t)refs[page].text.count + refs[page].search.count);
      selection_bytes -= selection_sizes[page];
      selection_sizes[page] = 0;
      book->GetPage(page)->FreeBuffer();
      ready[page] = false;
    }
  }
  bool RecoverMemory() {
    if (result != NoMemory || !budget || phase != 2 || memory_retry_used)
      return false;
    memory_retry_used = true;
    const size_t before = Memory();
    // Drop everything dispensable, retaining the visible Page and blocks needed
    // by the pending target. Retry only when this actually recovered memory.
    Trim(budget);
    const size_t after = Memory();
    if (after >= before)
      return false;
    DBG_LOGF(book->GetStatusReporter(),
             "CACHE memory retry page=%u before=%u after=%u", target,
             (unsigned)before, (unsigned)after);
    result = Pending;
    return true;
  }
  Result LoadBlock(unsigned id) {
    const Block &b = blocks[id];
    Trim(b.words * 4);
    if (budget && Memory() + b.words * 4 > budget)
      return NoMemory;
    std::vector<uint8_t> bytes(b.size), encoded;
    const Result read_result = ReadBlock(b, bytes.data());
    if (read_result != Ready)
      return read_result;
    bool out_of_memory = false;
    if (!page_cache_codec::Unpack(bytes.data(), bytes.size(), b.encoded,
                                  b.compressed, &encoded, &out_of_memory))
      return out_of_memory ? NoMemory : Corrupt;
    if (Crc(encoded.data(), encoded.size()) != b.crc)
      return Corrupt;
    CachedBlock block;
    block.id = id;
    block.age = ++tick;
    if (!page_cache_codec::Decode(encoded.data(), encoded.size(), b.words,
                                  &block.words))
      return Corrupt;
    cache.push_back(std::move(block));
    return Pending;
  }
};
Reader::Reader() : state_(NULL) {}
Reader::~Reader() {
  delete state_;
}
void Reader::Close() {
  delete state_;
  state_ = NULL;
}
Result Reader::ReadResource(const std::string &id, image_memory::Bytes *out) {
  if (!out) return IoError;
  out->clear();
  if (!HasIndex()) return Missing;
  for (const auto &r : state_->resources) if (r.id == id) {
    if (!out->resize(r.size, 4u*1024u*1024u)) return NoMemory;
    uint32_t crc = 0;
    for (size_t off = 0; off < r.size;) {
      const size_t n = std::min<size_t>(kBytes, r.size-off);
      if (!state_->ReadAt(r.offset+off, out->mutable_data()+off, n)) { out->clear(); return IoError; }
      crc = crc32(crc, out->data()+off, n); off += n;
    }
    if (crc != r.crc) { out->clear(); return Corrupt; }
    return Ready;
  }
  return Missing;
}
Result Reader::GetResult() const {
  return state_ ? state_->result : Cancelled;
}
Result Reader::Begin(Book *book, const std::string &path, size_t budget,
                     bool validate_only, Format format) {
  Close();
  state_ = new (std::nothrow) State;
  if (!state_)
    return NoMemory;
  State &s = *state_;
  s.book = book;
  s.path = path;
  s.budget = budget;
  s.format = format;
  s.validate_only = validate_only;
  if (!book || (!validate_only && book->GetPageCount()))
    return s.result = IoError;
  s.fd = open(path.c_str(), O_RDONLY);
  if (s.fd < 0)
    return s.result = errno == ENOENT ? Missing : IoError;
  struct stat info;
  if (fstat(s.fd, &info) != 0 || info.st_size < (long)sizeof(Header))
    return s.result = Corrupt;
  s.file_size = info.st_size;
  return s.result = Pending;
}
bool Reader::HasIndex() const {
  return state_ && state_->phase >= 2;
}
size_t Reader::ResidentBytes() const {
  return state_ ? state_->Memory() : 0;
}
size_t Reader::BytesRead() const {
  return state_ ? state_->read_bytes : 0;
}
unsigned Reader::Links(unsigned page) const {
  return HasIndex() && page < state_->refs.size() ? state_->refs[page].links
                                                  : 0;
}
bool Reader::IsReady(unsigned page) const {
  return HasIndex() && page < state_->ready.size() && state_->ready[page];
}
void Reader::Pin(unsigned page) {
  if (state_) {
    state_->UpdateSelections(state_->pinned);
    state_->pinned = page;
  }
}
void Reader::FrameEnd() {
  if (HasIndex() && state_->budget) {
    state_->UpdateSelections(state_->pinned);
    state_->Trim(0);
  }
}
void Reader::InvalidateFile() {
  if (state_)
    remove(state_->path.c_str());
}
bool Reader::ActivatePreparedPages() {
  if (!state_ || !state_->validate_only || GetResult() != Complete ||
      !state_->budget)
    return false;
  State &s = *state_;
  Page *page = s.book->GetPage(s.target);
  const PageRef &ref = s.refs[s.target];
  if (!page || page->GetLength() != ref.text.count ||
      page->GetSearchEvents().size() != ref.search.count)
    return false;
  const size_t need = 4 * ((size_t)ref.text.count + ref.search.count);
  const size_t selections = page->GetRenderedSelectables().capacity() *
                            sizeof(Page::SelectableRenderEntry);
  if (need + selections > s.budget) {
    s.result = NoMemory;
    return false;
  }
  s.selection_sizes[s.target] = selections;
  s.selection_bytes = selections;
  if (s.format == Fb2) {
    s.book->fb2_inline_images.clear();
    s.book->fb2_inline_images_bytes = 0;
  }
  s.validate_only = false;
  s.phase = 3;
  s.full_next = 0;
  s.payload_bytes = need;
  s.ready[s.target] = true;
  s.result = Pending;
  return true;
}
bool Reader::Request(unsigned page) {
  if (!HasIndex() || state_->phase == 3 || page >= state_->header.pages)
    return false;
  if (state_->target != page || state_->result == Complete)
    state_->memory_retry_used = false;
  state_->target = page;
  state_->result = IsReady(page) ? Complete : Pending;
  if (IsReady(page))
    state_->ages[page] = ++state_->tick;
  else {
    // Decode blocks are already owned: materialize this page without any I/O.
    for (const Ref &ref : {state_->refs[page].text, state_->refs[page].search})
      if (ref.count && !state_->Find(ref.block))
        return true;
    return Step() == Complete || GetResult() == Pending;
  }
  return true;
}
Result Reader::Step() {
  const u64 begin = osGetTime();
  const size_t reads = state_ ? state_->read_calls : 0;
  Result result;
  do {
    result = Advance();
  } while (result == Pending && state_ && state_->phase == 2 &&
           !state_->budget && state_->read_calls == reads &&
           osGetTime() - begin < 50);
  if (state_ && result == NoMemory && state_->RecoverMemory())
    result = Pending;
  if (state_ && result == Complete && !state_->validate_only &&
      !state_->logged) {
    State &s = *state_;
    s.logged = true;
    DBG_LOGF(s.book->GetStatusReporter(),
             "CACHE IO load v=%u bytes=%u calls=%u io_ms=%llu total_ms=%llu "
             "resident=%u pages=%u",
             Version(s.format), (unsigned)s.read_bytes, (unsigned)s.read_calls,
             (unsigned long long)s.io_ms,
             (unsigned long long)(osGetTime() - s.started),
             (unsigned)s.Memory(), s.header.pages);
  }
  return result;
}
Result Reader::Advance() {
  if (!state_ || state_->result != Pending)
    return GetResult();
  State &s = *state_;
  if (s.phase == 0) {
    if (!s.ReadAt(0, &s.header, sizeof(Header)))
      return s.result = IoError;
    const Header &h = s.header;
    if (h.magic != Magic(s.format) || h.version != Version(s.format) || h.flags || !h.pages ||
        h.pages > 50000 || h.blocks > 50000 || !h.index_size ||
        h.index_size > kIndexLimit || h.index_offset < sizeof(Header) ||
        h.index_offset > s.file_size ||
        h.index_size != s.file_size - h.index_offset ||
        (uint64_t)h.pages * 22 + (uint64_t)h.blocks * 28 > h.index_size)
      return s.result = Corrupt;
    if (!cache_memory::HasHeadroom(
            (size_t)h.index_size * 2 +
                (s.validate_only ? 0 : (size_t)h.pages * sizeof(Page)),
            512 * 1024))
      return s.result = NoMemory;
    s.index.resize(h.index_size);
    s.phase = 1;
    return Pending;
  }
  if (s.phase == 1) {
    if (s.index_read < s.index.size()) {
      size_t n = std::min((size_t)kBytes, s.index.size() - s.index_read);
      if (!s.ReadAt(s.header.index_offset + s.index_read,
                    s.index.data() + s.index_read, n))
        return s.result = IoError;
      s.index_read += n;
      return Pending;
    }
    if (Crc(s.index.data(), s.index.size()) != s.header.index_crc ||
        !s.ParseIndex())
      return s.result = Corrupt;
    if (s.validate_only && s.book->GetPageCount() != s.header.pages)
      return s.result = Corrupt;
    s.phase = 2;
    s.target = std::min<unsigned>((unsigned)std::max(0, s.book->GetPosition()),
                                  s.header.pages - 1);
    s.pinned = s.target;
    return s.validate_only ? (s.result = Complete) : Pending;
  }
  if (s.phase == 3) {
    const unsigned end = std::min<unsigned>(s.header.pages, s.full_next + 64);
    for (; s.full_next < end; ++s.full_next)
      if (s.full_next != s.pinned)
        s.book->GetPage(s.full_next)->FreeBuffer();
    if (s.full_next == s.header.pages) {
      s.phase = 2;
      s.result = Complete;
    }
    return s.result;
  }
  const unsigned target = s.budget ? s.target : s.full_next;
  if (target >= s.header.pages)
    return s.result = Complete;
  const PageRef p = s.refs[target];
  for (const Ref &ref : {p.text, p.search})
    if (ref.count && !s.Find(ref.block)) {
      Result r = s.LoadBlock(ref.block);
      if (r != Pending)
        s.result = r;
      return s.result;
    }
  const size_t need = 4 * ((size_t)p.text.count + p.search.count);
  // Copy the requested page only. Borrowed decode-block pointers cannot survive
  // Trim.
  std::vector<u32> text, events;
  if (p.text.count) {
    const auto &v = s.Find(p.text.block)->words;
    text.assign(v.begin() + p.text.offset,
                v.begin() + p.text.offset + p.text.count);
  }
  if (p.search.count) {
    const auto &v = s.Find(p.search.block)->words;
    events.assign(v.begin() + p.search.offset,
                  v.begin() + p.search.offset + p.search.count);
  }
  if (!page_search::Valid(events, p.text.count))
    return s.result = Corrupt;
  s.Trim(need);
  if (s.budget && s.Memory() + need > s.budget)
    return s.result = NoMemory;
  Page *page = s.book->GetPage(target);
  page_buffer_utils::OwnedPageBuffer owned =
      page_buffer_utils::AdoptPageBuffer(&text);
  page->AdoptBuffer(&owned);
  page->SetSearchEvents(events);
  s.payload_bytes += need;
  s.ready[target] = true;
  s.ages[target] = ++s.tick;
  if (s.budget)
    return s.result = Complete;
  ++s.full_next;
  if (s.full_next == s.header.pages)
    s.result = Complete;
  return s.result;
}
} // namespace reflow_block_cache
