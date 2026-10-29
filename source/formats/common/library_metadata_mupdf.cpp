#include "formats/common/library_metadata.h"
#if defined(__3DS__) || defined(LIBRARY_METADATA_NATIVE_MUPDF)
#include "mupdf/fitz.h"
#include <cstring>
namespace library_metadata {
Status ReadMuPdf(const std::string &path,book_meta_cache::MetaEntry *out) {
  fz_context *ctx=fz_new_context(nullptr,nullptr,4u*1024u*1024u);
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
      if(needed>4097) {ok=false;break;}
      if(needed>0) {
        memset(buffer,0,sizeof(buffer));
        const int count=fz_lookup_metadata(ctx,doc,keys[i],buffer,sizeof(buffer));
        if(count>4097 || count<0) {ok=false;break;}
        *values[i]=buffer;
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
