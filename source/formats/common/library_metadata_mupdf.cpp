#include "formats/common/library_metadata.h"
#if defined(__3DS__) || defined(LIBRARY_METADATA_NATIVE_MUPDF)
#include "mupdf/fitz.h"
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <algorithm>
#include "shared/utf8_utils.h"
namespace library_metadata {
namespace {
const size_t kMetadataMemoryLimit=4u*1024u*1024u;
struct Memory {size_t bytes=0;};
union Header {size_t bytes;std::max_align_t alignment;};
void Free(void *context,void *ptr) {
  if(!ptr) return;
  auto &memory=*static_cast<Memory *>(context);
  Header *head=static_cast<Header *>(ptr)-1;
  memory.bytes-=head->bytes;free(head);
}
void *Resize(void *context,void *ptr,size_t bytes) {
  if(!bytes) {Free(context,ptr);return nullptr;}
  auto &memory=*static_cast<Memory *>(context);
  Header *head=ptr ? static_cast<Header *>(ptr)-1 : nullptr;
  const size_t old=head ? head->bytes : 0;
  if(bytes>kMetadataMemoryLimit-sizeof(Header) ||
     bytes+sizeof(Header)>kMetadataMemoryLimit-(memory.bytes-old)) return nullptr;
  const size_t total=bytes+sizeof(Header);
  Header *next=static_cast<Header *>(realloc(head,total));
  if(!next) return nullptr;
  next->bytes=total;memory.bytes=memory.bytes-old+total;return next+1;
}
void *Allocate(void *context,size_t bytes) {return Resize(context,nullptr,bytes);}
}
Status ReadMuPdf(const std::string &path,book_meta_cache::MetaEntry *out) {
  Memory memory;
  fz_alloc_context allocator={&memory,Allocate,Resize,Free};
  fz_context *ctx=fz_new_context(&allocator,nullptr,kMetadataMemoryLimit);
  if(!ctx) return Status::Error;
  fz_document *doc=nullptr;bool ok=true;
  // Keep C++ objects outside MuPDF's longjmp region.
  char buffer[4097];
  fz_var(doc);fz_var(ok);
  fz_try(ctx) {
    fz_register_document_handlers(ctx);doc=fz_open_document(ctx,path.c_str());
    if(fz_needs_password(ctx,doc)) ok=false;
    const char *keys[]={FZ_META_INFO_TITLE,FZ_META_INFO_AUTHOR,FZ_META_INFO_SUBJECT,FZ_META_INFO_CREATIONDATE};
    std::string *values[]={&out->title,&out->author,&out->subjects,&out->published};
    for(unsigned i=0;ok && i<4;++i) {
      const int needed=fz_lookup_metadata(ctx,doc,keys[i],nullptr,0);
      if(i<2 && needed>4097) {ok=false;break;}
      if(needed>0) {
        memset(buffer,0,sizeof(buffer));
        const int count=fz_lookup_metadata(ctx,doc,keys[i],buffer,sizeof(buffer));
        if(count<0 || (i<2 && count>4097)) {ok=false;break;}
        *values[i]=buffer;
        if(count>4097) {
          values[i]->resize(std::min(values[i]->size(),(size_t)4093));
          while(!values[i]->empty() && !utf8_utils::IsValidUtf8(*values[i])) values[i]->pop_back();
          *values[i]+="...";
        }
      }
    }
  }
  fz_catch(ctx) {ok=false;}
  fz_drop_document(ctx,doc);fz_drop_context(ctx);
  return ok ? Status::Ready : Status::Error;
}
}
#else
namespace library_metadata {
Status ReadMuPdf(const std::string &,book_meta_cache::MetaEntry *) {return Status::Unsupported;}
}
#endif
