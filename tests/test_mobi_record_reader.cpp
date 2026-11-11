#include "formats/mobi/mobi_parser_core.h"
#include "formats/mobi/mobi_record_reader.h"
#include "formats/mobi/mobi_record_scan.h"
#include "formats/mobi/mobi_text_decode.h"
#include "book/book.h"
#include "test_assert.h"

#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

static std::string g_title;
void Book::SetTitle(const char *title) { g_title = title ? title : ""; }

namespace {
std::string ReadAll(const std::string &path) {
  std::ifstream in(path.c_str(), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void CheckFile(const std::string &path) {
  const std::string raw = ReadAll(path);
  const std::string name = path.substr(path.find_last_of('/') + 1);
  std::vector<uint32_t> expected;
  const bool table_ok = mobi_record_scan::ParseRecordOffsets(raw, &expected);
  const int fd = open(path.c_str(), O_RDONLY);
  test::ExpectTrue("open fixture", fd >= 0);
  mobi_record_reader::RecordReader reader;
  test::ExpectTrue((name + " table verdict").c_str(),
                   reader.Open(fd, raw.size()) == table_ok);
  if (!table_ok) {
    close(fd);
    return;
  }
  test::ExpectTrue((name + " offsets").c_str(), reader.Offsets() == expected);

  for (uint32_t i = 0; i + 1 < expected.size(); ++i) {
    std::string got;
    const auto status = reader.Read(i, &got);
    const uint32_t start = expected[i], end = expected[i + 1];
    if (end > start && end <= raw.size()) {
      test::ExpectTrue("record read ok",
                       status == mobi_record_reader::RecordReader::Ok);
      test::ExpectTrue((name + " record bytes").c_str(),
                       got == raw.substr(start, end - start));
    } else {
      test::ExpectTrue("empty record",
                       status == mobi_record_reader::RecordReader::Empty);
    }
  }
  std::string none;
  test::ExpectTrue("index past the table",
                   reader.Read((uint32_t)expected.size(), &none) ==
                       mobi_record_reader::RecordReader::BadIndex);

  // Header parse from record 0 alone equals the whole-file parse.
  mobi_parser_core::MobiHeaderInfo whole, part;
  const unsigned char whole_rc = mobi_parser_core::ParseMobiHeader(raw, &whole);
  std::string rec0;
  part.offsets = reader.Offsets();
  unsigned char part_rc = 254;
  if (reader.Read(0, &rec0) == mobi_record_reader::RecordReader::Ok &&
      rec0.size() >= 16)
    part_rc = mobi_parser_core::ParseMobiHeaderRecord0(
        (const unsigned char *)rec0.data(), rec0.size(), &part);
  test::ExpectEq((name + " header rc").c_str(), part_rc, whole_rc);
  if (whole_rc == 0) {
    test::ExpectEqU("text_len", part.text_len, whole.text_len);
    test::ExpectEqU("text_rec_count", part.text_rec_count,
                    whole.text_rec_count);
    test::ExpectEqU("encoding", part.encoding, whole.encoding);
    test::ExpectEqU("trailing", part.trailing_flags, whole.trailing_flags);
    test::ExpectEqU("ncx", part.ncx_index, whole.ncx_index);
    // SetTitle is stubbed and never touches the object.
    static char storage[1];
    Book *book = reinterpret_cast<Book *>(storage);
    g_title.clear();
    mobi_text_decode::ApplyEmbeddedTitle(book, raw, whole);
    const std::string a = g_title;
    g_title.clear();
    mobi_text_decode::ApplyEmbeddedTitleFromRecord0(
        book, (const unsigned char *)rec0.data(), rec0.size(), part);
    test::ExpectTrue((name + " title").c_str(), a == g_title);
  }

  // Sparse loading: loaded records are byte-identical slices, others empty.
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  ranges.push_back(std::make_pair(0u, 0u));
  if (expected.size() > 4)
    ranges.push_back(std::make_pair(2u, (uint32_t)(expected.size() - 3)));
  std::string compact;
  std::vector<uint32_t> sparse;
  test::ExpectTrue("sparse load",
                   reader.LoadSparse(ranges, 1u << 28, &compact, &sparse) ==
                       mobi_record_reader::RecordReader::Ok);
  test::ExpectEqU("sparse table shape", (unsigned)sparse.size(),
                  (unsigned)expected.size());
  for (uint32_t i = 0; i + 1 < expected.size(); ++i) {
    bool wanted = false;
    for (size_t r = 0; r < ranges.size(); ++r)
      wanted = wanted || (i >= ranges[r].first && i <= ranges[r].second);
    const uint32_t start = expected[i], end = expected[i + 1];
    const bool real = end > start && end <= raw.size();
    const uint32_t s0 = sparse[i], s1 = sparse[i + 1];
    if (wanted && real)
      test::ExpectTrue("sparse bytes",
                       compact.substr(s0, s1 - s0) ==
                           raw.substr(start, end - start));
    else
      test::ExpectTrue("sparse empty", s1 <= s0);
  }
  test::ExpectTrue("sparse byte cap",
                   reader.LoadSparse(ranges, 4, &compact, &sparse) ==
                       mobi_record_reader::RecordReader::NoMemory);

  // Adopting a parsed table gives the same reads.
  mobi_record_reader::RecordReader adopted;
  test::ExpectTrue("adopt", adopted.Adopt(fd, raw.size(), expected));
  std::string a, b;
  reader.Read(1, &a);
  adopted.Read(1, &b);
  test::ExpectTrue("adopted read", a == b);
  close(fd);
}
} // namespace

int main() {
  const char *dir = getenv("TEST_MOBI_FIXTURE_DIR");
  test::ExpectTrue("fixture directory", dir != nullptr);
  DIR *d = opendir(dir);
  test::ExpectTrue("fixture directory opens", d != nullptr);
  unsigned checked = 0;
  while (dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 5 && name.substr(name.size() - 5) == ".mobi") {
      CheckFile(std::string(dir) + "/" + name);
      ++checked;
    }
  }
  closedir(d);
  test::ExpectTrue("fixtures were checked", checked >= 3);
  return 0;
}
