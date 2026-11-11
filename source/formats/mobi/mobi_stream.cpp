#include "formats/mobi/mobi_stream.h"

#include "formats/common/cache_memory_utils.h"
#include "formats/common/plain_text_stream.h"

#include <algorithm>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace mobi_stream {
namespace {

const size_t kMaxTotalBytes = 64u * 1024u * 1024u;
const size_t kPrefixLimit = 1024u * 1024u;
const size_t kPumpChunk = 64u * 1024u;
// SD writes are slow per call on Old 3DS: hand the card large blocks.
const size_t kFlushBytes = 128u * 1024u;

bool WriteAll(int fd, const char *data, size_t n) {
  while (n) {
    const ssize_t w = write(fd, data, n);
    if (w <= 0)
      return false;
    data += w;
    n -= (size_t)w;
  }
  return true;
}

int OpenForWrite(const std::string &path) {
  return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

} // namespace

std::string RawPath(const std::string &base) { return base + ".stream.txt"; }
std::string CleanPath(const std::string &base) { return base + ".stream2.txt"; }
void RemoveScratch(const std::string &base) {
  unlink(RawPath(base).c_str());
  unlink(CleanPath(base).c_str());
}

// ---------------------------------------------------------------- Decoder

Decoder::Decoder()
    : next_record_(1), raw_total_(0), used_(0), mode_(mobi_text_decode::Legacy),
      markup_size_(0), text_size_(0), fed_(0), prefix_cap_(0),
      scan_done_(false), utf16_(false), utf16_size_(0), counter_(NULL),
      converter_(NULL), fd_(-1), has_cr_(false), has_c383_(false),
      last_byte_(0), write_calls_(0) {}
Decoder::~Decoder() {
  delete counter_;
  delete converter_;
  if (fd_ >= 0)
    close(fd_);
}

bool Decoder::Begin(const Setup &setup) {
  setup_ = setup;
  if (!setup.reader || !setup.header || !setup.records || !setup.map)
    return false;
  utf16_ = mobi_text_decode::IsUtf16Encoding(setup.header->encoding);
  if (utf16_) {
    counter_ = new (std::nothrow) mobi_text_decode::Converter(
        mobi_text_decode::Utf16);
    if (!counter_)
      return false;
  }
  next_record_ = 1;
  return true;
}

// Next text record decoded into text_ and cut at the header's text length like
// the merged buffer. Pending means text_ holds data; Done means no records left.
Status Decoder::NextText() {
  const mobi_parser_core::MobiHeaderInfo &h = *setup_.header;
  for (;;) {
    if (next_record_ > h.text_rec_count)
      return Done;
    const uint32_t rec = next_record_++;
    const mobi_record_reader::RecordReader::Status st =
        setup_.reader->Read(rec, &record_);
    if (st == mobi_record_reader::RecordReader::Empty)
      continue;
    if (st == mobi_record_reader::RecordReader::BadIndex)
      return Failed;
    if (st == mobi_record_reader::RecordReader::IoError)
      return IoError;
    if (st == mobi_record_reader::RecordReader::NoMemory)
      return NoMemory;
    if (!setup_.records->Decode((const uint8_t *)record_.data(),
                                record_.size(), &text_))
      return Failed;
    if (text_.size() > kMaxTotalBytes - raw_total_)
      return Failed;
    raw_total_ += text_.size();
    if (h.text_len) {
      if (used_ >= h.text_len)
        text_.clear();
      else if (used_ + text_.size() > h.text_len)
        text_.resize(h.text_len - used_);
    }
    used_ += text_.size();
    return Pending;
  }
}

Status Decoder::FinishScan() {
  size_t markup = 0;
  if (utf16_) {
    mode_ = mobi_text_decode::Utf16;
    markup = utf16_size_;
  } else {
    const mobi_text_decode::ScanScore &score = scorer_.Score();
    mode_ = mobi_text_decode::ChooseMode(setup_.header->encoding, score).mode;
    markup = mode_ == mobi_text_decode::Copy
                 ? score.bytes
                 : mode_ == mobi_text_decode::Mixed ? score.mixed_size
                                                    : score.legacy_size;
  }
  markup_size_ = markup;
  converter_ = new (std::nothrow) mobi_text_decode::Converter(mode_);
  if (!converter_)
    return NoMemory;
  fd_ = OpenForWrite(setup_.text_path);
  if (fd_ < 0)
    return IoError;
  if (setup_.on_convert)
    setup_.on_convert(setup_.context);
  if (!extractor_.Begin(setup_.images, setup_.map, markup_size_))
    return NoMemory;
  prefix_cap_ = std::min(kPrefixLimit, markup_size_);
  next_record_ = 1;
  raw_total_ = used_ = 0;
  scan_done_ = true;
  return Done;
}

Status Decoder::ScanStep(size_t budget) {
  if (scan_done_)
    return Done;
  size_t done = 0;
  while (done < budget) {
    const Status st = NextText();
    if (st == Done) {
      if (utf16_) {
        utf8_.clear();
        counter_->Feed("", 0, true, &utf8_);
        utf16_size_ += utf8_.size();
      } else
        scorer_.Feed("", 0, true);
      return FinishScan();
    }
    if (st != Pending)
      return st;
    if (utf16_) {
      utf8_.clear();
      counter_->Feed(text_.data(), text_.size(), false, &utf8_);
      utf16_size_ += utf8_.size();
    } else
      scorer_.Feed(text_.data(), text_.size(), false);
    done += text_.size() + 1;
  }
  return Pending;
}

Status Decoder::Write(const std::string &chunk) {
  for (size_t i = 0; i < chunk.size(); ++i) {
    const unsigned char b = (unsigned char)chunk[i];
    if (b == '\r')
      has_cr_ = true;
    else if (b == 0x83 && last_byte_ == 0xC3)
      has_c383_ = true;
    last_byte_ = b;
  }
  tokens_.Feed(chunk.data(), chunk.size(), false);
  wbuf_.append(chunk);
  return wbuf_.size() >= kFlushBytes ? Flush() : Pending;
}

Status Decoder::Flush() {
  if (!wbuf_.empty()) {
    ++write_calls_;
    if (!WriteAll(fd_, wbuf_.data(), wbuf_.size()))
      return IoError;
    wbuf_.clear();
  }
  return Pending;
}

Status Decoder::FeedExtractor(bool final) {
  fed_ += utf8_.size();
  if (prefix_.size() < prefix_cap_)
    prefix_.append(utf8_, 0,
                   std::min(utf8_.size(), prefix_cap_ - prefix_.size()));
  if (!extractor_.Feed(utf8_.data(), utf8_.size(), final))
    return NoMemory;
  chunk_.clear();
  extractor_.Drain(&chunk_);
  return Write(chunk_);
}

Status Decoder::ConvertStep(size_t budget) {
  if (!scan_done_)
    return Failed;
  size_t done = 0;
  while (done < budget) {
    const Status st = NextText();
    if (st == Done) {
      utf8_.clear();
      converter_->Feed("", 0, true, &utf8_);
      const Status fed = FeedExtractor(true);
      if (fed != Pending)
        return fed;
      tokens_.Feed("", 0, true);
      text_size_ = extractor_.OutputBytes();
      // Everything must be on the card before the file is read back.
      const Status flushed = Flush();
      if (flushed != Pending)
        return flushed;
      close(fd_);
      fd_ = -1;
      // The map sampling interval came from the pass 1 size.
      return fed_ == markup_size_ ? Done : Failed;
    }
    if (st != Pending)
      return st;
    utf8_.clear();
    converter_->Feed(text_.data(), text_.size(), false, &utf8_);
    const Status fed = FeedExtractor(false);
    if (fed != Pending)
      return fed;
    done += text_.size() + 1;
  }
  return Pending;
}

// ---------------------------------------------------------------- Cleaner

// Random-access view of a file for the remap (indexes never go backwards).
struct Cleaner::Source {
  int fd;
  size_t size, base, len;
  std::vector<char> buf;
  Source(int f, size_t n) : fd(f), size(n), base(0), len(0), buf(64 * 1024) {}
  static unsigned char At(void *context, size_t index) {
    Source *s = static_cast<Source *>(context);
    if (index < s->base || index >= s->base + s->len) {
      s->base = index;
      s->len = 0;
      if (index < s->size && lseek(s->fd, (off_t)index, SEEK_SET) == (off_t)index) {
        const ssize_t n = read(s->fd, &s->buf[0], s->buf.size());
        if (n > 0)
          s->len = (size_t)n;
      }
    }
    return index < s->base + s->len ? (unsigned char)s->buf[index - s->base] : 0;
  }
};

Cleaner::Cleaner()
    : stage_(Finished), final_size_(0), raw_is_final_(true), track_(false),
      rolled_back_(false), moj_(false), wrap_(false), in_fd_(-1), out_fd_(-1),
      write_calls_(0),
      in_read_(0), cleanup_(NULL), buffer_(kPumpChunk), before_(NULL),
      after_(NULL) {}
Cleaner::~Cleaner() {
  delete cleanup_;
  delete before_;
  delete after_;
  if (in_fd_ >= 0)
    close(in_fd_);
  if (out_fd_ >= 0)
    close(out_fd_);
}

bool Cleaner::OpenRun() {
  delete cleanup_;
  cleanup_ = NULL;
  mobi_text_cleanup::StreamOptions o;
  o.mojibake = !rolled_back_ && moj_;
  o.wrap_fix = !rolled_back_ && wrap_;
  o.token_aware = !rolled_back_ && track_;
  cleanup_ = new (std::nothrow) mobi_text_cleanup::CleanupStream(o);
  if (in_fd_ >= 0)
    close(in_fd_);
  if (out_fd_ >= 0)
    close(out_fd_);
  in_fd_ = open(setup_.raw_path.c_str(), O_RDONLY);
  out_fd_ = OpenForWrite(setup_.clean_path);
  in_read_ = 0;
  final_size_ = 0;
  wbuf_.clear();
  stage_ = Run;
  return cleanup_ && in_fd_ >= 0 && out_fd_ >= 0;
}

bool Cleaner::Begin(const Setup &setup) {
  setup_ = setup;
  stage_ = Finished;
  final_size_ = setup.raw_size;
  final_path_ = setup.raw_path;
  raw_is_final_ = true;
  moj_ = setup.has_mojibake_marker;
  wrap_ = setup.wrap_fix;
  if (!setup.has_cr && !moj_ && !wrap_)
    return true; // nothing to clean: the raw text is final
  raw_is_final_ = false;
  final_path_ = setup.clean_path;
  if ((moj_ || wrap_) && setup.has_cr) {
    // Carriage returns change what the token check sees: scan the normalized
    // text once (output discarded) to know whether tokens are present.
    mobi_text_cleanup::StreamOptions o;
    o.token_aware = true;
    cleanup_ = new (std::nothrow) mobi_text_cleanup::CleanupStream(o);
    in_fd_ = open(setup.raw_path.c_str(), O_RDONLY);
    in_read_ = 0;
    stage_ = Scan;
    return cleanup_ && in_fd_ >= 0;
  }
  track_ = (moj_ || wrap_) && setup.has_tokens_pre;
  return OpenRun();
}

// Feeds up to `budget` bytes of the raw file; true when all were fed.
bool Cleaner::FlushOut(bool force) {
  if (wbuf_.empty() || (!force && wbuf_.size() < kFlushBytes))
    return true;
  ++write_calls_;
  const bool ok = WriteAll(out_fd_, wbuf_.data(), wbuf_.size());
  wbuf_.clear();
  return ok;
}

Status Cleaner::Pump(bool scan_only, size_t budget) {
  size_t done = 0;
  do {
    const size_t want =
        std::min(std::min(buffer_.size(), setup_.raw_size - in_read_),
                 std::max<size_t>(1, budget - std::min(budget, done)));
    ssize_t n = 0;
    if (want) {
      n = read(in_fd_, &buffer_[0], want);
      if (n <= 0)
        return IoError;
    }
    in_read_ += (size_t)n;
    const bool final = in_read_ >= setup_.raw_size;
    out_.clear();
    cleanup_->Feed(buffer_.data(), (size_t)n, final, &out_);
    if (!scan_only) {
      wbuf_.append(out_);
      final_size_ += out_.size();
      if (!FlushOut(final))
        return IoError;
    }
    done += (size_t)n;
    if (final)
      return Done;
  } while (done < budget);
  return Pending;
}

Status Cleaner::Step(size_t budget) {
  if (stage_ == Scan) {
    const Status st = Pump(true, budget);
    if (st != Done)
      return st;
    track_ = !cleanup_->IdsBefore().empty();
    delete cleanup_;
    cleanup_ = NULL;
    if (!OpenRun())
      return IoError;
    return Pending;
  }
  if (stage_ == Run) {
    const Status st = Pump(false, budget);
    if (st != Done)
      return st;
    const bool rollback = track_ && !rolled_back_ &&
                          cleanup_->IdsBefore() != cleanup_->IdsAfter();
    delete cleanup_;
    cleanup_ = NULL;
    if (rollback) {
      // The repairs changed the image tokens: keep only the newline
      // normalization, as the whole-text cleanup does.
      rolled_back_ = true;
      if (!OpenRun())
        return IoError;
      return Pending;
    }
    if (close(out_fd_) != 0) {
      out_fd_ = -1;
      return IoError;
    }
    out_fd_ = -1;
    close(in_fd_);
    in_fd_ = -1;
    if (final_size_ == setup_.raw_size || setup_.map->empty()) {
      stage_ = Finished;
      unlink(setup_.raw_path.c_str());
      return Done;
    }
    before_ = new (std::nothrow) Source(open(setup_.raw_path.c_str(), O_RDONLY),
                                        setup_.raw_size);
    after_ = new (std::nothrow) Source(open(setup_.clean_path.c_str(), O_RDONLY),
                                       final_size_);
    if (!before_ || !after_ || before_->fd < 0 || after_->fd < 0 ||
        !remap_.Begin(setup_.raw_size, final_size_, Source::At, before_,
                      Source::At, after_, setup_.map))
      return NoMemory;
    stage_ = Remap;
    return Pending;
  }
  if (stage_ == Remap) {
    if (!remap_.Step(std::max<size_t>(budget, 4096)))
      return Pending;
    close(before_->fd);
    close(after_->fd);
    delete before_;
    delete after_;
    before_ = after_ = NULL;
    unlink(setup_.raw_path.c_str());
    stage_ = Finished;
    return Done;
  }
  return Done;
}

size_t Cleaner::Progress() const {
  return stage_ == Remap ? remap_.Progress() : in_read_;
}

// ----------------------------------------------------------------- Window

Window::Window() : fd_(-1), size_(0), read_(0), buffer_() {}
Window::~Window() { Close(); }
void Window::Close() {
  if (fd_ >= 0)
    close(fd_);
  fd_ = -1;
}
bool Window::Begin(const std::string &path, size_t size) {
  fd_ = open(path.c_str(), O_RDONLY);
  size_ = size;
  read_ = 0;
  return fd_ >= 0;
}
Status Window::Fill(plain_text_stream::State *state, std::string *window,
                    size_t chunk) {
  const size_t n = std::min(chunk, size_ - read_);
  if (buffer_.size() < n) {
    if (!cache_memory::HasHeadroom(n, 64 * 1024))
      return NoMemory;
    buffer_.resize(n);
  }
  if (n) {
    const ssize_t got = read(fd_, &buffer_[0], n);
    if (got < 0 || (size_t)got != n)
      return IoError;
  }
  read_ += n;
  return plain_text_stream::AppendWindow(state, window, buffer_.data(), n,
                                         read_ >= size_)
             ? Done
             : NoMemory;
}

} // namespace mobi_stream
