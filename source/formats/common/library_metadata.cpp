#include "formats/common/library_metadata.h"
#include "shared/string_utils.h"
#include "shared/utf8_utils.h"
#include "shared/path_utils.h"
#include "minizip/unzip.h"
#include "expat.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <memory>
#include <new>

namespace library_metadata {
namespace {
const size_t kFieldLimit=4096, kReadLimit=4u*1024u*1024u;
std::string Local(const char *name) {
  const char *p=strrchr(name,'|');return p ? p+1 : name;
}
const char *Attribute(const XML_Char **attrs,const char *name) {
  for(size_t i=0;attrs && attrs[i];i+=2)
    if(Local(attrs[i])==name) return attrs[i+1];
  return "";
}
void Trim(std::string *text) {
  const auto first=text->find_first_not_of(" \r\n\t");
  const auto last=text->find_last_not_of(" \r\n\t");
  *text=first==std::string::npos ? "" : text->substr(first,last-first+1);
}
bool Append(std::string *out,const char *data,size_t size) {
  if(size>kFieldLimit-out->size()) return false;
  out->append(data,size);return true;
}
bool Join(std::string *out,const std::string &value) {
  if(value.empty()) return true;
  return (out->empty() || Append(out,"; ",2)) && Append(out,value.data(),value.size());
}
// Bibliographic display text is not a searchable field. Keep it bounded and
// explicitly mark omitted text, without rejecting genuine title/author data.
void AppendDisplay(std::string *out,const char *data,size_t size,bool *overflow) {
  if(*overflow) return;
  const size_t count=std::min(size,kFieldLimit-out->size());
  out->append(data,count);
  if(count!=size) {
    *overflow=true;out->resize(std::min(out->size(),kFieldLimit-3));
    while(!out->empty() && !utf8_utils::IsValidUtf8(*out)) out->pop_back();
    *out+="...";
  }
}
enum class XmlKind {Container,Opf,Fb2,Comic,Relations,Core};
struct Xml {
  XML_Parser parser=nullptr;
  XmlKind kind;
  book_meta_cache::MetaEntry &entry;
  std::string rootfile,cover_id,first_image,likely_image,author,nickname,part;
  std::string *field=nullptr;
  unsigned depth=0,field_depth=0,metadata_depth=0,title_info_depth=0,author_depth=0;
  bool failed=false,done=false,seen=false;
  bool searchable=true,field_overflow=false,subjects_overflow=false;
  Xml(XmlKind k,book_meta_cache::MetaEntry &e):kind(k),entry(e) {}
  ~Xml() {if(parser) XML_ParserFree(parser);}
  void Stop(bool error) {failed=error;done=!error;XML_StopParser(parser,XML_FALSE);}
  void Capture(std::string *out,bool strict=true) {field=out;field_depth=depth;searchable=strict;field_overflow=false;}
  static void Start(void *ctx,const XML_Char *tag,const XML_Char **attrs) {
    Xml &x=*(Xml*)ctx;
    if(++x.depth>64) {x.Stop(true);return;}
    const auto name=Local(tag);
    if(x.depth==1) {
      const char *expected=x.kind==XmlKind::Container ? "container" : x.kind==XmlKind::Opf ? "package" : x.kind==XmlKind::Fb2 ? "FictionBook" : x.kind==XmlKind::Comic ? "ComicInfo" : x.kind==XmlKind::Relations ? "Relationships" : "coreProperties";
      if(name!=expected) {x.Stop(true);return;} x.seen=true;
    }
    if(x.kind==XmlKind::Relations && name=="Relationship" &&
       std::string(Attribute(attrs,"Type")).find("/metadata/core-properties")!=std::string::npos) {
      const char *target=Attribute(attrs,"Target");
      if(strlen(target)>kFieldLimit) {x.Stop(true);return;}
      x.rootfile=target;
      if(!x.rootfile.empty() && x.rootfile[0]=='/') x.rootfile.erase(0,1);
    } else if(x.kind==XmlKind::Core) {
      if(name=="title") x.Capture(&x.entry.title);
      else if(name=="creator") x.Capture(&x.entry.author);
      else if(name=="language") x.Capture(&x.entry.language,false);
    } else if(x.kind==XmlKind::Container && name=="rootfile" && x.rootfile.empty()) {
      const char *value=Attribute(attrs,"full-path");
      if(strlen(value)>kFieldLimit) x.Stop(true);else x.rootfile=value;
    } else if(x.kind==XmlKind::Opf) {
      if(name=="metadata") x.metadata_depth=x.depth;
      if(x.metadata_depth && !x.field) {
        if(name=="title" && x.entry.title.empty()) x.Capture(&x.entry.title);
        else if(name=="creator") {x.part.clear();x.Capture(&x.part);}
        else if(name=="language") x.Capture(&x.entry.language,false);
        else if(name=="publisher") x.Capture(&x.entry.publisher,false);
        else if(name=="date") x.Capture(&x.entry.published,false);
        else if(name=="subject") {x.part.clear();x.Capture(&x.part,false);}
        else if(name=="description") x.Capture(&x.entry.description,false);
        else if(name=="meta" && std::string(Attribute(attrs,"name"))=="cover") {
          const char *value=Attribute(attrs,"content");
          if(strlen(value)<=kFieldLimit) x.cover_id=value;
        }
      }
      if(name=="meta" && (std::string(Attribute(attrs,"name"))=="calibre:series" ||
                            std::string(Attribute(attrs,"property"))=="calibre:series")) {
        const char *series=Attribute(attrs,"content");
        bool overflow=false;AppendDisplay(&x.entry.series,series,strlen(series),&overflow);
      }
      if(name=="item") {
        const std::string href=Attribute(attrs,"href"),type=Attribute(attrs,"media-type");
        if(href.size()>kFieldLimit) return;
        if(type.compare(0,6,"image/")==0) {
          const std::string id=Attribute(attrs,"id"),properties=Attribute(attrs,"properties");
          if(x.first_image.empty()) x.first_image=href;
          if(x.likely_image.empty() && (ContainsNoCase(id,"cover") ||
             ContainsNoCase(href,"cover") || ContainsNoCase(href,"portada") ||
             ContainsNoCase(properties,"cover"))) x.likely_image=href;
          if(ContainsToken(properties,"cover-image") ||
             (!x.cover_id.empty() && x.cover_id==id)) x.entry.cover_image_path=href;
        }
      }
    } else if(x.kind==XmlKind::Fb2) {
      // Stop at the start tag; no body text or binary payload is parsed.
      if(name=="body" || name=="binary") {x.Stop(!x.seen);return;}
      if(name=="title-info" && !x.title_info_depth) x.title_info_depth=x.depth;
      if(x.title_info_depth && !x.field) {
        if(name=="book-title") x.Capture(&x.entry.title);
        else if(name=="lang") x.Capture(&x.entry.language,false);
        else if(name=="author") {x.author_depth=x.depth;x.author.clear();x.nickname.clear();}
        else if(x.author_depth && (name=="first-name" || name=="middle-name" || name=="last-name")) {x.part.clear();x.Capture(&x.part);}
        else if(x.author_depth && name=="nickname") x.Capture(&x.nickname);
      }
    } else if(x.kind==XmlKind::Comic && x.depth==2) {
      if(name=="Title") x.Capture(&x.entry.title);
      else if(name=="Writer") x.Capture(&x.entry.author);
      else if(name=="Series") x.Capture(&x.entry.series,false);
      else if(name=="Publisher") x.Capture(&x.entry.publisher,false);
    }
  }
  static void End(void *ctx,const XML_Char *tag) {
    Xml &x=*(Xml*)ctx;const auto name=Local(tag);
    if(x.field && x.depth==x.field_depth) {
      Trim(x.field);
      if(x.field==&x.part) {
        bool ok=true;
        if(x.kind==XmlKind::Opf) {
          if(name=="creator") ok=Join(&x.entry.author,x.part);
          else {
            if(!x.entry.subjects.empty()) AppendDisplay(&x.entry.subjects,"; ",2,&x.subjects_overflow);
            AppendDisplay(&x.entry.subjects,x.part.data(),x.part.size(),&x.subjects_overflow);
          }
        }
        else {
          if(!x.author.empty() && !x.part.empty()) ok=Append(&x.author," ",1);
          if(ok) ok=Append(&x.author,x.part.data(),x.part.size());
        }
        if(!ok) {x.Stop(true);return;}
      }
      x.field=nullptr;
    }
    if(x.kind==XmlKind::Fb2) {
      if(x.depth==x.author_depth) {if(!Join(&x.entry.author,x.author.empty() ? x.nickname : x.author)) {x.Stop(true);return;}x.author_depth=0;}
      if(x.depth==x.title_info_depth) x.title_info_depth=0;
      // description contains all metadata; stop before even reading the body.
      if(name=="description") {x.Stop(false);return;}
    }
    if(x.depth==x.metadata_depth) x.metadata_depth=0;
    --x.depth;
  }
  static void Text(void *ctx,const XML_Char *text,int length) {
    Xml &x=*(Xml*)ctx;
    if(x.field) {
      if(!x.searchable) AppendDisplay(x.field,text,(size_t)length,&x.field_overflow);
      else if(!Append(x.field,text,(size_t)length)) x.Stop(true);
    }
  }
  static void Doctype(void *ctx,const XML_Char *,const XML_Char *,const XML_Char *,int) {((Xml*)ctx)->Stop(true);}
  bool Init() {
    parser=XML_ParserCreateNS(nullptr,'|');if(!parser) return false;
    XML_SetUserData(parser,this);XML_SetElementHandler(parser,Start,End);
    XML_SetCharacterDataHandler(parser,Text);XML_SetStartDoctypeDeclHandler(parser,Doctype);
    XML_SetParamEntityParsing(parser,XML_PARAM_ENTITY_PARSING_NEVER);
    return true;
  }
  bool Feed(const char *bytes,int count,bool final) {
    if(XML_Parse(parser,bytes,count,final)==XML_STATUS_ERROR && !done) failed=true;
    return !failed;
  }
};
bool ZipXml(unzFile zip,const std::string &path,Xml &xml,size_t *budget) {
  if(unzLocateFile(zip,path.c_str(),2)!=UNZ_OK) return false;
  unz_file_info info{};
  if(unzGetCurrentFileInfo(zip,&info,nullptr,0,nullptr,0,nullptr,0)!=UNZ_OK ||
     info.uncompressed_size>*budget || !xml.Init() || unzOpenCurrentFile(zip)!=UNZ_OK) return false;
  bool ok=true;char buffer[4096];
  while(ok && !xml.done) {
    const int n=unzReadCurrentFile(zip,buffer,sizeof(buffer));
    if(n<0 || (size_t)n>*budget) {ok=false;break;}
    *budget-=(size_t)n;ok=xml.Feed(buffer,n,n==0);
    if(n==0) break;
  }
  const int closed=unzCloseCurrentFile(zip);
  return ok && closed==UNZ_OK && xml.seen && !xml.failed;
}
Status ZipMetadata(const std::string &path,bool epub,book_meta_cache::MetaEntry *entry) {
  unzFile zip=unzOpen(path.c_str());if(!zip) return Status::Error;
  size_t budget=kReadLimit;bool ok=false;
  if(epub) {
    Xml container(XmlKind::Container,*entry);
    if(ZipXml(zip,"META-INF/container.xml",container,&budget) && !container.rootfile.empty()) {
      Xml opf(XmlKind::Opf,*entry);
      ok=ZipXml(zip,container.rootfile,opf,&budget);
      if(ok) {
        if(entry->cover_image_path.empty()) entry->cover_image_path=opf.likely_image.empty() ? opf.first_image : opf.likely_image;
        if(!entry->cover_image_path.empty()) {
          entry->cover_image_path=ResolveRelativePath(container.rootfile,entry->cover_image_path);
          if(entry->cover_image_path.size()>kFieldLimit) ok=false;
        }
      }
    }
  } else if(unzLocateFile(zip,"ComicInfo.xml",2)==UNZ_END_OF_LIST_OF_FILE) {
    // A valid archive without ComicInfo correctly has no searchable metadata.
    unz_file_info info{};ok=unzGoToFirstFile(zip)==UNZ_OK && unzGetCurrentFileInfo(zip,&info,nullptr,0,nullptr,0,nullptr,0)==UNZ_OK;
  } else {Xml comic(XmlKind::Comic,*entry);ok=ZipXml(zip,"ComicInfo.xml",comic,&budget);}
  unzClose(zip);return ok ? Status::Ready : Status::Error;
}
Status XpsMetadata(const std::string &path,book_meta_cache::MetaEntry *entry) {
  // MuPDF's XPS handler exposes format only, so read the OPC core properties.
  unzFile zip=unzOpen(path.c_str());if(!zip) return Status::Error;
  size_t budget=kReadLimit;Xml rel(XmlKind::Relations,*entry);
  bool ok=ZipXml(zip,"_rels/.rels",rel,&budget);
  if(ok && !rel.rootfile.empty()) {
    Xml core(XmlKind::Core,*entry);ok=ZipXml(zip,NormalizePath(rel.rootfile),core,&budget);
  }
  unzClose(zip);return ok ? Status::Ready : Status::Error;
}
Status Fb2Metadata(const std::string &path,book_meta_cache::MetaEntry *entry) {
  FILE *fp=fopen(path.c_str(),"rb");if(!fp) return Status::Error;
  Xml xml(XmlKind::Fb2,*entry);bool ok=xml.Init();size_t read=0;char buffer[4096];
  while(ok && !xml.done) {
    const size_t count=fread(buffer,1,sizeof(buffer),fp);
    if(count>kReadLimit-read || ferror(fp)) {ok=false;break;}
    read+=count;ok=xml.Feed(buffer,(int)count,count==0);
    if(count==0) break;
  }
  fclose(fp);return ok && xml.seen && !xml.failed ? Status::Ready : Status::Error;
}
uint32_t Be32(const unsigned char *p) {return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];}
bool MobiField(const unsigned char *p,size_t length,uint32_t encoding,std::string *out) {
  if(length>kFieldLimit) return false;
  std::string text((const char*)p,length);
  if(encoding==1252) text=utf8_utils::DecodeCp1252ToUtf8(text);
  else if(encoding!=65001 || !utf8_utils::IsValidUtf8(text)) return false;
  if(text.size()>kFieldLimit || text.find('\0')!=std::string::npos) return false;
  Trim(&text);*out=text;return true;
}
Status MobiMetadata(const std::string &path,book_meta_cache::MetaEntry *entry) {
  FILE *fp=fopen(path.c_str(),"rb");if(!fp) return Status::Error;
  unsigned char header[94];
  bool ok=fread(header,1,sizeof(header),fp)==sizeof(header) && !memcmp(header+60,"BOOKMOBI",8);
  const unsigned count=ok ? ((unsigned)header[76]<<8 | header[77]) : 0;
  const uint32_t start=ok ? Be32(header+78) : 0,end=ok && count>1 ? Be32(header+86) : 0;
  struct stat st;
  ok=ok && count>1 && start>=78+count*8 && end>start &&
      end-start<=kReadLimit && fstat(fileno(fp),&st)==0 && (uint64_t)end<=(uint64_t)st.st_size;
  const size_t record_size=ok ? end-start : 0;
  std::unique_ptr<unsigned char[]> record;
  if(ok) {
    record.reset(new(std::nothrow) unsigned char[record_size]);
    ok=record && fseek(fp,start,SEEK_SET)==0 && fread(record.get(),1,record_size,fp)==record_size;
  }
  fclose(fp);if(!ok || record_size<16+0x4c) return Status::Error;
  const unsigned char *mobi=record.get()+16;
  const uint32_t length=Be32(mobi+4),encoding=Be32(mobi+12);
  if(memcmp(mobi,"MOBI",4) || length<0x4c || length>record_size-16) return Status::Error;
  const size_t title_offset=Be32(mobi+0x44),title_length=Be32(mobi+0x48);
  if(title_length && (title_offset>record_size || title_length>record_size-title_offset ||
     !MobiField(record.get()+title_offset,title_length,encoding,&entry->title))) return Status::Error;
  const size_t offset=16+length,remain=record_size-offset;
  const bool flagged=length>=0x84 && (Be32(mobi+0x80)&0x40);
  if(flagged || (remain>=12 && !memcmp(record.get()+offset,"EXTH",4))) {
    const auto exth=record.get()+offset;
    if(remain<12 || memcmp(exth,"EXTH",4)) return Status::Error;
    const size_t size=Be32(exth+4),records=Be32(exth+8);
    if(size<12 || size>remain || records>(size-12)/8) return Status::Error;
    size_t at=12;
    for(size_t i=0;i<records;++i) {
      if(at>size-8) return Status::Error;
      const uint32_t type=Be32(exth+at),n=Be32(exth+at+4);
      if(n<8 || n>size-at) return Status::Error;
      if(type==100 || type==503) {
        std::string value;if(!MobiField(exth+at+8,n-8,encoding,&value)) return Status::Error;
        if(type==100) {if(!Join(&entry->author,value)) return Status::Error;}
        else entry->title=value;
      }
      at+=n;
    }
  }
  return Status::Ready;
}
}
Status Prepare(const std::string &path,Value *out,Cancelled cancelled,void *context,bool cache_only) {
  if(!out) return Status::Error;
  *out=Value();
  if(cancelled && cancelled(context)) return Status::Cancelled;
  const bool epub=HasExtCI(path.c_str(),".epub"),fb2=HasExtCI(path.c_str(),".fb2"),
      mobi=HasExtCI(path.c_str(),".mobi") || HasExtCI(path.c_str(),".azw") || HasExtCI(path.c_str(),".azw3"),
      cbz=HasExtCI(path.c_str(),".cbz"),xps=HasExtCI(path.c_str(),".xps") || HasExtCI(path.c_str(),".oxps"),
      pdf=HasExtCI(path.c_str(),".pdf") || xps;
  if(!epub && !fb2 && !mobi && !cbz && !pdf) return Status::Unsupported;
  struct stat st;if(!paths::StatWithMtime(path.c_str(),&st) || !S_ISREG(st.st_mode)) return Status::Error;
  const auto cache=book_meta_cache::BuildPath(path,st.st_size,st.st_mtime);
  bool prepared=false;
  if(book_meta_cache::Load(cache,&out->entry,&prepared) && prepared) return Status::Ready;
  out->entry=book_meta_cache::MetaEntry();
  if(cache_only) return Status::NeedsPreparation;
  out->extracted=true;
  const Status status=epub || cbz ? ZipMetadata(path,epub,&out->entry) :
      fb2 ? Fb2Metadata(path,&out->entry) : mobi ? MobiMetadata(path,&out->entry) : xps ? XpsMetadata(path,&out->entry) : ReadMuPdf(path,&out->entry);
  if(cancelled && cancelled(context)) return Status::Cancelled;
  if(status!=Status::Ready) {out->entry=book_meta_cache::MetaEntry();return status;}
  if(out->entry.title.size()>kFieldLimit || out->entry.author.size()>kFieldLimit ||
     !utf8_utils::IsValidUtf8(out->entry.title) || !utf8_utils::IsValidUtf8(out->entry.author)) return Status::Error;
  book_meta_cache::EnsureDir();out->cache_saved=book_meta_cache::Save(cache,out->entry,true);
  return Status::Ready;
}
}
