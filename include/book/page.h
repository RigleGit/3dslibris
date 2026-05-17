/*
 Copyright (C) 2007-2009 Ray Haleblian
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 
 To contact the copyright holder: rayh23@sourceforge.net
 */

/*
  3DS port modifications by Rigle (summary):
  - Preserved dslibris Page model for flowed text storage.
  - Integrated with 3DS renderer constraints (screen sizing and clipping).
*/
/*!
A Page stores the flowed output from the XML parse and is equipped
to render a full left and right screen of text.

A Book contains a vector of Pages.
*/
#pragma once

#include <3ds.h>
#include "book/page_buffer_utils.h"
#include "reader/inline_link_utils.h"
#include "ui/text.h"
#include <cstddef>
#include <vector>

class Book;

class Page {
 public:
	struct InlineLinkRenderEntry {
		u16 href_id;
		u8 screen_index;
		inline_link_utils::LinkRect bounds;
	};

 private:
	class Book *book;
	std::vector<u32> storage;
	std::vector<InlineLinkRenderEntry> rendered_inline_links_;
	mutable int cached_inline_link_count_;
	void DrawNumber(Text *ts, u16 *number_screen);
	void SyncBufferAlias();
	void InvalidateLinkCountCache();

 public:
	//! Pre-decoded Unicode codepoints, allocated per-page at parse time.
	u32 *buf;
	//! Length of buf.
	int length;
	//! Allocated capacity of buf.
	int capacity;
	//! In a book-long char buffer, where would i begin?
	int start;
	//! Ditto, for end char. 
	int end;
	Page(Book *b);
	Page(Book *b, Text *t);
	~Page();
	u32* GetBuffer() { return buf; }
	int  GetLength() { return length; }
	//! Copy src to buf for len codepoints.
	void SetBuffer(const u32 *src, int len); 
	void AdoptBuffer(page_buffer_utils::OwnedPageBuffer *owned);
	void FreeBuffer();
	const std::vector<InlineLinkRenderEntry> &GetRenderedInlineLinks() const {
		return rendered_inline_links_;
	}
	size_t GetInlineLinkCount() const;
	//	void Draw();
	void Draw(Text *ts);
};
