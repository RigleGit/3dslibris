/*
 * Stub for source/book/reflow_worker.cpp.
 * Provides Book async-reflow methods without 3DS thread/APT APIs.
 */
#include "book/book.h"
#include "shared/open_cancel_poll.h"
#include "ui/text.h"

void Book::PrepareForOpen() {
  Text *text = GetText();
  if (text)
    text->SetStyle(0); // TEXT_STYLE_REGULAR
  ClearOpenAbortRequest();
  open_cancel_poll::Reset();
  tocResolveTried = false;
  tocResolved = false;
  ClearTocConfidence();
  ClearChapterAnchors();
}

bool Book::SupportsAsyncReflowOpen() const { return false; }

bool Book::StartAsyncReflowOpen(unsigned int) { return false; }

bool Book::PumpAsyncReflowOpen() { return false; }

bool Book::IsAsyncReflowOpenPending() const { return false; }

void Book::CancelAsyncReflowOpen() {}

void Book::ResetReflowWorkerState() {}
