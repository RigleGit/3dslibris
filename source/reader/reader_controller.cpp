#include "app/reader_controller.h"

#include "app/app.h"

ReaderController::ReaderController(App &app) : app_(app) {}

void App::CloseBook() { reader_controller_->CloseBook(); }

int App::GetBookIndex(Book *book) { return reader_controller_->GetBookIndex(book); }

void App::HandleEventInBook() { reader_controller_->HandleEventInBook(); }

void App::HandleEventInOpening() { reader_controller_->HandleEventInOpening(); }

u8 App::OpenBook() { return reader_controller_->OpenBook(); }

void App::ToggleBookmark() { reader_controller_->ToggleBookmark(); }

void App::OnReaderAppletSuspended() { reader_controller_->OnAppletSuspended(); }

void App::OnReaderAppletResumed() { reader_controller_->OnAppletResumed(); }
