#include "app/library_controller.h"
#include "library/library_search_session.h"
#include "formats/common/library_metadata.h"
#include "app/app.h"
#include "book/book.h"
#include "formats/common/fixed_layout_gpu_renderer.h"
#include "shared/path_utils.h"
#include "shared/path_constants.h"
#include "shared/debug_log.h"
#include "shared/text_unicode_utils.h"
#include "ui/text.h"
#include <3ds.h>
#include <algorithm>
#include <new>
#include <sys/stat.h>

namespace {
uint64_t SearchClock() {return osGetTime();}
std::string EntryPath(Book *book) {
  return book->IsBrowserFolder() ? book->GetBrowserFolderPath() :
      std::string(book->GetFolderName())+"/"+book->GetFileName();
}
std::string Parent(const std::string &path) {return path.substr(0,path.find_last_of('/'));}
bool Exists(const std::string &path,bool directory) {
  struct stat st;
  return stat(path.c_str(),&st)==0 && (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}
std::string Fit(Text *ts,const std::string &text) {
  const unsigned chars=ts->GetCharCountInsideWidth(text.c_str(),TEXT_STYLE_BROWSER,224);
  const size_t bytes=text_unicode_utils::Utf8BytesForDisplayChars(text.c_str(),chars);
  return text.substr(0,bytes);
}
void InitSearchButton(Button &button,Text *ts,int x,int y,int w,int h) {
  button.Init(ts);button.SetStyle(BUTTON_STYLE_SETTING);
  button.Move(x,y);button.Resize(w,h);
}
}

bool LibraryController::SearchRoot(void *ctx,size_t index,library_search::Entry *out) {
  LibraryController &self=*(LibraryController*)ctx;
  if(index>=self.app_.books.size()) return false;
  Book *book=self.app_.books[index];
  out->path=EntryPath(book);out->directory=book->IsBrowserFolder();
  out->name=out->directory ? book->GetBrowserFolderDisplayName() : book->GetFileName();
  return true;
}
library_search::MetadataStatus LibraryController::SearchMetadata(void *context,const std::string &path,library_search::MetadataValue *out) {
  LibraryController &self=*static_cast<LibraryController *>(context);
  if(self.app_.ShouldAbortWork()) return library_search::MetadataStatus::Cancelled;
  library_metadata::Value value;
  const auto status=library_metadata::Prepare(path,&value);
  out->extracted=value.extracted;out->cache_saved=value.cache_saved;
  out->title=std::move(value.entry.title);out->author=std::move(value.entry.author);
  if(status==library_metadata::Status::Ready) return library_search::MetadataStatus::Ready;
  if(status==library_metadata::Status::Unsupported) return library_search::MetadataStatus::Unsupported;
  if(status==library_metadata::Status::Cancelled) return library_search::MetadataStatus::Cancelled;
  return library_search::MetadataStatus::Error;
}

void LibraryController::OpenLibrarySearch() {
  if(app_.ShouldAbortWork()) return;
  PauseBrowserJobs();
  search_.reset(new(std::nothrow) LibrarySearchSession);
  if(!search_) {app_.PrintStatus("Search memory unavailable");return;}
  auto &s=*search_;
  s.origin_folder=inside_folder_ ? current_folder_path_ : "";
  s.origin_page=app_.GetBrowserPageStart();
  if(app_.GetSelectedBook()) s.origin_selection=EntryPath(app_.GetSelectedBook());
  Text *ts=app_.ts.get();
  fixed_layout_gpu::Software(ts,true);ts->SetTopScreenWideMode(false);
  app_.ApplyRenderOrientation(app_.portrait_orientation);
  for(int i=0;i<4;++i) InitSearchButton(s.editor_buttons[i],ts,5,i==3 ? 286 : 60+i*56,230,i==3 ? 28 : 44);
  for(int i=0;i<5;++i) InitSearchButton(s.rows[i],ts,5,44+i*46,230,44);
  for(int i=0;i<3;++i) InitSearchButton(s.footer[i],ts,6+i*80,286,68,28);
  s.footer[0].Label("prev");s.footer[1].Label("back");s.footer[2].Label("next");
  app_.SetMode(AppMode::LibrarySearch);
  DrawLibrarySearch();
}
void LibraryController::StartLibrarySearch() {
  auto &s=*search_;
  if(!s.engine.Start(s.field,s.query,SearchRoot,this,!inside_folder_,SearchMetadata)) {
    s.message="Enter a word or name";s.dirty=true;return;
  }
  s.editor=false;s.result_selection=0;s.message.clear();s.dirty=true;
}
void LibraryController::ResumeLibrarySearch() {
  if(!search_ || app_.GetMode()!=AppMode::LibrarySearch || app_.ShouldAbortWork()) return;
  RestoreSearchOrigin();
  app_.ApplyRenderOrientation(app_.portrait_orientation);
  if(!search_->editor) StartLibrarySearch();
  search_->dirty=search_->wait_release=true;
}
void LibraryController::RestoreSearchOrigin() {
  const auto &s=*search_;
  if(s.origin_folder.empty()) {if(inside_folder_) RebuildRoot();}
  else if(!Exists(s.origin_folder,true)) RebuildRoot();
  else if(!inside_folder_ || current_folder_path_!=s.origin_folder)
    LoadFolderPath(s.origin_folder,BasenamePath(s.origin_folder));
  for(Book *book:app_.books) if(EntryPath(book)==s.origin_selection) {
    app_.SetSelectedBook(book);app_.SetBrowserPageStart(s.origin_page);break;
  }
}
void LibraryController::CloseLibrarySearch() {
  if(!search_) return;
  search_->engine.Cancel();RestoreSearchOrigin();search_.reset();
  app_.ShowLibraryView();
}
void LibraryController::ActivateLibrarySearchResult(size_t index) {
  if(index>=search_->engine.Results().size()) return;
  const auto result=search_->engine.Results()[index];
  search_->engine.Cancel();
  bool opened=false;
  if(Exists(result.path,result.directory)) {
    const std::string folder=Parent(result.path);
    if(result.directory || folder==app_.bookdir || folder==paths::kRomfsBookDir) {
      if(inside_folder_) RebuildRoot();
    } else if(!inside_folder_ || current_folder_path_!=folder)
      LoadFolderPath(folder,BasenamePath(folder));
    for(Book *book:app_.books) if(EntryPath(book)==result.path) {
      app_.SetSelectedBook(book);
      if(result.directory) {EnterFolder(book);app_.ShowLibraryView();opened=true;}
      else opened=app_.OpenBook()==0 && app_.GetMode()==AppMode::Book;
      break;
    }
  }
  if(opened) {search_.reset();return;}
  if(!app_.ShouldAbortWork()) {
    RestoreSearchOrigin();app_.ApplyRenderOrientation(app_.portrait_orientation);
  }
  app_.SetMode(AppMode::LibrarySearch);
  search_->message="Unable to open result";search_->wait_release=search_->dirty=true;
}
void LibraryController::EditLibrarySearch() {
  auto &s=*search_;
  DrawLibrarySearch();app_.PresentIfDirty();
  if(app_.ShouldAbortWork()) return;
  SwkbdState keyboard;
  swkbdInit(&keyboard,SWKBD_TYPE_NORMAL,2,128);
  swkbdSetHintText(&keyboard,"Search books or folders");
  swkbdSetInitialText(&keyboard,s.query.c_str());
  swkbdSetValidation(&keyboard,SWKBD_NOTEMPTY_NOTBLANK,0,0);
  swkbdSetButton(&keyboard,SWKBD_BUTTON_LEFT,"Cancel",false);
  swkbdSetButton(&keyboard,SWKBD_BUTTON_RIGHT,"OK",true);
  swkbdSetFeatures(&keyboard,SWKBD_PREDICTIVE_INPUT|SWKBD_ALLOW_HOME|SWKBD_ALLOW_POWER);
  char query[513]={};
  const SwkbdButton result=swkbdInputText(&keyboard,query,sizeof(query));
  if(result==SWKBD_BUTTON_RIGHT) {s.query=query;s.selected=2;s.message.clear();}
  else if(result==SWKBD_BUTTON_NONE && swkbdGetResult(&keyboard)<0) s.message="Keyboard unavailable; try again";
  s.dirty=s.wait_release=true;app_.ts->MarkAllScreensDirty();
}
void LibraryController::DrawLibrarySearch() {
  if(!search_ || app_.ShouldAbortWork()) return;
  auto &s=*search_;Text *ts=app_.ts.get();
  const int style=ts->GetStyle(),size=ts->GetPixelSize(),bottom=ts->margin.bottom;
  const bool wrap=ts->IsAutoWrapEnabled();
  ts->SetStyle(TEXT_STYLE_BROWSER);ts->SetPixelSize(10);ts->SetAutoWrapEnabled(false);ts->margin.bottom=0;
  ts->SetScreen(ts->screenleft);ts->ClearScreen();
  ts->SetPen(6,24);ts->PrintString("library search");
  ts->SetPen(6,50);ts->PrintString(Fit(ts,s.origin_folder.empty() ? "Library + folders" : s.origin_folder).c_str());
  ts->SetPen(6,76);ts->PrintString(Fit(ts,s.query).c_str());
  char progress[96];
  snprintf(progress,sizeof(progress),"%u results / %u examined",(unsigned)s.engine.Results().size(),(unsigned)s.engine.Examined());
  ts->SetPen(6,108);ts->PrintString(progress);
  std::string status=s.message;
  if(status.empty() && !s.editor) {
    if(s.engine.GetStatus()==library_search::Status::Searching) status=s.field==library_search::Field::Name ? "Searching... B: cancel" : "Preparing metadata... B: cancel";
    else if(s.engine.GetStatus()==library_search::Status::Limited) status="Too many matches; refine search";
    else if(s.engine.Results().empty()) status="No matches";
  }
  ts->SetPen(6,140);ts->PrintString(Fit(ts,status).c_str());
  if(s.engine.Errors() || s.engine.Unsupported()) {
    snprintf(progress,sizeof(progress),"Incomplete: %u errors, %u unsupported",(unsigned)s.engine.Errors(),(unsigned)s.engine.Unsupported());
    ts->SetPen(6,164);ts->PrintString(Fit(ts,progress).c_str());
  }
  if(s.engine.Uncached()) {ts->SetPen(6,188);ts->PrintString("Some metadata was not saved");}
  if(s.field!=library_search::Field::Name) {
    ts->SetPen(6,216);ts->PrintString("Metadata: EPUB PDF XPS MOBI");
    ts->SetPen(6,236);ts->PrintString("FB2 CBZ; other formats excluded");
  }
  ts->SetScreen(ts->screenright);ts->ClearScreen();
  ts->SetPen(6,24);ts->PrintString(s.editor ? "search library" : "search results");
  if(s.editor) {
    const char *fields[]={"file / folder name","title","author"};
    s.editor_buttons[0].SetLabel1("search by");s.editor_buttons[0].SetLabel2(fields[(int)s.field]);
    s.editor_buttons[1].SetLabel1("word or name");s.editor_buttons[1].SetLabel2(s.query.empty() ? "open keyboard >" : s.query);
    s.editor_buttons[2].Label("search");s.editor_buttons[3].Label("back");
    for(int i=0;i<4;++i) s.editor_buttons[i].Draw(i==s.selected);
  } else {
    const auto &results=s.engine.Results();
    const size_t page=s.result_selection/5;
    for(int i=0;i<5 && page*5+i<results.size();++i) {
      const auto &result=results[page*5+i];
      s.rows[i].SetLabel1(result.directory ? "[folder] "+result.label : result.label);
      s.rows[i].SetLabel2(Parent(result.path));s.rows[i].Draw(page*5+i==s.result_selection);
    }
    for(int i=0;i<3;++i) s.footer[i].Draw(false);
  }
  ts->margin.bottom=bottom;ts->SetAutoWrapEnabled(wrap);ts->SetPixelSize(size);ts->SetStyle(style);
  ts->MarkAllScreensDirty();s.dirty=false;
}
void LibraryController::RunLibrarySearchFrame(const FrameInput &input) {
  if(!search_) {app_.ShowLibraryView();return;}
  if(app_.ShouldAbortWork()) return;
  auto &s=*search_;
  if(s.wait_release) {if(!input.keys_held) s.wait_release=false;return;}
  const u32 keys=input.keys_down;
  const auto touch=input.MapTouch(app_.portrait_orientation);
  if(keys&app_.key.b) {
    if(!s.editor && s.engine.GetStatus()==library_search::Status::Searching) {s.engine.Cancel();s.editor=s.dirty=true;}
    else {CloseLibrarySearch();return;}
  }
  int action=-1;
  if(s.editor) {
    if(keys&(KEY_DUP|KEY_DLEFT)) {s.selected=(s.selected+3)%4;s.dirty=true;}
    if(keys&(KEY_DDOWN|KEY_DRIGHT)) {s.selected=(s.selected+1)%4;s.dirty=true;}
    if(keys&app_.key.a) action=s.selected;
    if(keys&(app_.key.y|app_.key.x)) action=1;
    if(keys&KEY_TOUCH) for(int i=0;i<4;++i) if(s.editor_buttons[i].EnclosesPoint(touch.x,touch.y)) action=i;
    if(action==0) {s.NextField();s.dirty=true;}
    if(action==1) {EditLibrarySearch();return;}
    if(action==2) StartLibrarySearch();
    if(action==3) {CloseLibrarySearch();return;}
  } else {
    if(keys&(app_.key.y|app_.key.x)) {s.engine.Cancel();s.editor=s.dirty=true;s.selected=1;}
    const size_t count=s.engine.Results().size();
    if(count) {
      const size_t old=s.result_selection;
      if(keys&KEY_DUP) s.result_selection=s.result_selection ? s.result_selection-1 : 0;
      if(keys&KEY_DDOWN) s.result_selection=std::min(count-1,s.result_selection+1);
      if(keys&app_.key.l) s.result_selection=s.result_selection>=5 ? s.result_selection-5 : 0;
      if(keys&app_.key.r) s.result_selection=std::min(count-1,s.result_selection+5);
      s.dirty|=old!=s.result_selection;
      if(keys&app_.key.a) {ActivateLibrarySearchResult(s.result_selection);return;}
      if(keys&KEY_TOUCH) for(int i=0;i<5;++i) {
        const size_t index=(s.result_selection/5)*5+i;
        if(index<count && s.rows[i].EnclosesPoint(touch.x,touch.y)) {ActivateLibrarySearchResult(index);return;}
      }
    }
    if(keys&KEY_TOUCH) for(int i=0;i<3;++i) if(s.footer[i].EnclosesPoint(touch.x,touch.y)) {
      if(i==1) {CloseLibrarySearch();return;}
      if(count) s.result_selection=i==0 ? (s.result_selection>=5 ? s.result_selection-5 : 0) : std::min(count-1,s.result_selection+5);
      s.dirty=true;
    }
    if(s.engine.GetStatus()==library_search::Status::Searching) {
      s.engine.Pump(16,SearchClock,4);s.dirty=true;
      if(s.engine.GetStatus()!=library_search::Status::Searching)
        DBG_LOGF(&app_,"LIBSEARCH done field=%d matches=%u examined=%u errors=%u unsupported=%u bytes=%u",(int)s.field,(unsigned)s.engine.Results().size(),(unsigned)s.engine.Examined(),(unsigned)s.engine.Errors(),(unsigned)s.engine.Unsupported(),(unsigned)s.engine.AuxiliaryBytes());
    }
  }
  if(s.dirty) DrawLibrarySearch();
}
void App::OpenLibrarySearch() {library_controller_->OpenLibrarySearch();}
void App::RunLibrarySearchFrame(const FrameInput &input) {library_controller_->RunLibrarySearchFrame(input);}
