#pragma once

#include "book/inline_image_layout.h"

void ResetBookInlineImageStubState();
void ConfigureBookInlineImageStub(const InlineImageMetadata &meta,
                                  const InlineImageLayoutPlan &plan,
                                  bool plan_result);

void ConfigureBookInlineImageDrawBounds(const InlineImageDrawBounds &bounds);
