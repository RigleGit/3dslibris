#pragma once
// Stream tier of the MOBI preparation: the book is read record by record from
// the file, decoded twice (scan, then convert and extract) and the extracted
// text goes to a temporary file that pagination reads through a window. The
// pieces here do not know Book; mobi_parser.cpp wires them into its phases.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "formats/mobi/mobi_parser_core.h"
#include "formats/mobi/mobi_position_map.h"
#include "formats/mobi/mobi_record_decode.h"
#include "formats/mobi/mobi_record_reader.h"
#include "formats/mobi/mobi_safe_markup_extract.h"
#include "formats/mobi/mobi_text_cleanup.h"
#include "formats/mobi/mobi_text_decode.h"

namespace plain_text_stream {
struct State;
}

namespace mobi_stream {

enum Status { Pending, Done, Failed, NoMemory, IoError };

typedef std::vector<std::pair<uint32_t, uint32_t> > HtmlMap;

// Passes 1 and 2: decode the text records twice. Pass 1 scores the bytes to
// pick the conversion; pass 2 converts and feeds the incremental markup
// extractor, whose text is written to text_path.
class Decoder {
public:
  Decoder();
  ~Decoder();
  struct Setup {
    const mobi_record_reader::RecordReader *reader;
    const mobi_parser_core::MobiHeaderInfo *header;
    mobi_record_decode::TextRecordDecoder *records;
    std::string text_path;
    HtmlMap *map;
    mobi_safe_markup_extract::InlineImageCallbacks images;
    // Called once when pass 1 is over, before pass 2 registers any image.
    void (*on_convert)(void *context);
    void *context;
    Setup()
        : reader(NULL), header(NULL), records(NULL), map(NULL),
          on_convert(NULL), context(NULL) {}
  };
  bool Begin(const Setup &setup);
  // Each processes about `budget` decoded bytes per call.
  Status ScanStep(size_t budget);
  Status ConvertStep(size_t budget);
  unsigned WriteCalls() const { return write_calls_; }
  uint32_t Record() const { return next_record_ - 1; }
  size_t MarkupSize() const { return markup_size_; }
  size_t TextSize() const { return text_size_; }
  // First min(1 MiB, markup size) bytes of the converted markup, for the
  // inline filepos TOC.
  std::string *Prefix() { return &prefix_; }
  // What the extracted text contains, for the cleanup decisions.
  bool HasCarriageReturn() const { return has_cr_; }
  bool HasMojibakeMarker() const { return has_c383_; }
  const std::vector<unsigned short> &TokenIds() const { return tokens_.Ids(); }

private:
  Status NextText();
  Status FinishScan();
  Status FeedExtractor(bool final);
  Status Write(const std::string &chunk);
  Status Flush();
  std::string wbuf_;
  unsigned write_calls_;
  Setup setup_;
  uint32_t next_record_;
  size_t raw_total_, used_;
  mobi_text_decode::Scorer scorer_;
  mobi_text_decode::DecodeMode mode_;
  size_t markup_size_, text_size_, fed_, prefix_cap_;
  bool scan_done_, utf16_;
  size_t utf16_size_;
  mobi_text_decode::Converter *counter_, *converter_;
  mobi_safe_markup_extract::Stream extractor_;
  std::string prefix_, record_, text_, utf8_, chunk_;
  int fd_;
  bool has_cr_, has_c383_;
  unsigned char last_byte_;
  mobi_text_cleanup::TokenScan tokens_;
  Decoder(const Decoder &);
  Decoder &operator=(const Decoder &);
};

// Cleanup of the extracted text file: the steps CleanupDecodedMobiText runs on
// the whole text, as a streaming pass A -> B, then the html-to-text map remap.
class Cleaner {
public:
  Cleaner();
  ~Cleaner();
  struct Setup {
    std::string raw_path, clean_path;
    size_t raw_size;
    bool has_cr, has_mojibake_marker, wrap_fix;
    // Token ids seen while the text was written (valid when !has_cr).
    bool has_tokens_pre;
    HtmlMap *map;
    Setup()
        : raw_size(0), has_cr(false), has_mojibake_marker(false),
          wrap_fix(false), has_tokens_pre(false), map(NULL) {}
  };
  bool Begin(const Setup &setup);
  Status Step(size_t budget);
  unsigned WriteCalls() const { return write_calls_; }
  // The text to paginate (the raw file when no cleanup was needed).
  const std::string &FinalPath() const { return final_path_; }
  size_t FinalSize() const { return final_size_; }
  bool OwnsRaw() const { return !raw_is_final_; }
  size_t Progress() const;

private:
  enum Stage { Scan, Run, Remap, Finished };
  bool OpenRun();
  Status Pump(bool scan_only, size_t budget);
  bool FlushOut(bool force);
  std::string wbuf_;
  unsigned write_calls_;
  Setup setup_;
  Stage stage_;
  std::string final_path_;
  size_t final_size_;
  bool raw_is_final_, track_, rolled_back_, moj_, wrap_;
  int in_fd_, out_fd_;
  size_t in_read_;
  mobi_text_cleanup::CleanupStream *cleanup_;
  std::vector<char> buffer_;
  std::string out_;
  mobi_position_map::StreamRemap remap_;
  struct Source;
  Source *before_, *after_;
  Cleaner(const Cleaner &);
  Cleaner &operator=(const Cleaner &);
};

// Reads a text file through a window for plain_text_stream's windowed mode.
class Window {
public:
  Window();
  ~Window();
  bool Begin(const std::string &path, size_t size);
  size_t Size() const { return size_; }
  size_t Read() const { return read_; }
  bool AtEnd() const { return read_ >= size_; }
  void Close();
  // Appends the next chunk to *window (see plain_text_stream::AppendWindow).
  Status Fill(plain_text_stream::State *state, std::string *window,
              size_t chunk);
private:
  int fd_;
  size_t size_, read_;
  std::vector<char> buffer_;
  Window(const Window &);
  Window &operator=(const Window &);
};

// Removes the temporary files of an interrupted preparation.
void RemoveScratch(const std::string &base);
std::string RawPath(const std::string &base);
std::string CleanPath(const std::string &base);

} // namespace mobi_stream
