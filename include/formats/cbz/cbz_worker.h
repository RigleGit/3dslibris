#pragma once

#include "book/book.h"

enum class CbzPreloadPumpResult {
  Idle = 0,
  Submitted,
  Integrated,
};

void InitCbzWorker(Book::CbzState *cbz_state);
void ShutdownCbzWorker(Book::CbzState *cbz_state);
CbzPreloadPumpResult PumpCbzPreloadWorker(Book::CbzState *cbz_state,
                                          int current_page);
