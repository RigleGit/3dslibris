#include "formats/common/library_metadata.h"
#include <cassert>
#include <cstdio>
#include <string>
static void Pdf(const char *path,size_t length) {
  const std::string objects[]={"<< /Type /Catalog /Pages 2 0 R >>","<< /Type /Pages /Kids [3 0 R] /Count 1 >>","<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1 1] >>","<< /Title ("+std::string(length,'t')+") /Author (Author) >>"};
  FILE *f=fopen(path,"wb");assert(f);fputs("%PDF-1.4\n",f);long offsets[4];
  for(int i=0;i<4;++i) {offsets[i]=ftell(f);fprintf(f,"%d 0 obj\n%s\nendobj\n",i+1,objects[i].c_str());}
  long xref=ftell(f);fputs("xref\n0 5\n0000000000 65535 f \n",f);
  for(long offset:offsets) fprintf(f,"%010ld 00000 n \n",offset);
  fprintf(f,"trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\nstartxref\n%ld\n%%%%EOF\n",xref);fclose(f);
}
int main() {
  using namespace library_metadata;book_meta_cache::MetaEntry entry;
  Pdf("valid.pdf",4096);assert(ReadMuPdf("valid.pdf",&entry)==Status::Ready);
  assert(entry.title==std::string(4096,'t') && entry.author=="Author");
  Pdf("oversized.pdf",4097);assert(ReadMuPdf("oversized.pdf",&entry)==Status::Error);
  assert(ReadMuPdf("missing.pdf",&entry)==Status::Error);
  puts("PASS: real MuPDF metadata preserves full titles through 4096 bytes and rejects excess");
}
