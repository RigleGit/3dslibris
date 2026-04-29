/*
    3dslibris - parse.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Shared XML/HTML parse state used by EPUB/book content parsers.
    - Declares tag enums and parsing context used to produce flowed pages.
*/

#pragma once

#include <expat.h>
#include <3ds.h>
#include <map>
#include <string>
#include <vector>
#include "book/epub_css_class_map.h"
#include "shared/status_reporter.h"

#define PAGEBUFSIZE 4096

//! Symbols for known XHTML tags.

//! Not all tags here necessary affect rendering.
typedef enum {
	TAG_ANCHOR,
	TAG_ASIDE,
	TAG_BLOCKQUOTE,TAG_BODY,
	TAG_BR,
	TAG_CAPTION,
	TAG_DD,
	TAG_DIV,TAG_DT,
	TAG_FIGURE,
	TAG_H1,TAG_H2,TAG_H3,TAG_H4,TAG_H5,TAG_H6,TAG_HTML,TAG_HEAD,
	TAG_LI,
	TAG_NONE,
	TAG_OL,
	TAG_P,TAG_PRE,
	TAG_SCRIPT,TAG_STYLE,
	TAG_TABLE,TAG_TBODY,TAG_THEAD,TAG_TH,
	TAG_TD,TAG_TITLE,
	TAG_TR,
	TAG_STRONG,TAG_EM,
	TAG_UNDERLINE,TAG_STRIKETHROUGH,
	TAG_SUPERSCRIPT,TAG_SUBSCRIPT,
	TAG_CODE,
	TAG_UL,TAG_UNKNOWN
} context_t;

//! Expat parsing state.

//! This data structure is made available
//! to all expat callbacks via (void*)data.
typedef struct {
	int x;
	int y;
} parse_pen_t;

typedef struct parsedata_t parsedata_t;
typedef bool (*parse_page_flush_fn)(parsedata_t *data, void *ctx);

struct parsedata_t {
	context_t stack[32];
	u8 stacksize;
	class IStatusReporter *reporter;
	class Text *ts;  //! Text renderer.
	class Book *book;
	class Prefs *prefs;
	int screen;
	parse_pen_t pen;
	u32 buf[PAGEBUFSIZE];
	int buflen;
	bool pagebuf_overflowed;
	size_t pagebuf_overflow_bytes;
	//! Our total parse position in terms of cooked text.
	int pos;
	bool linebegan;
	bool preformatted_wrap_enabled;
	bool strip_leading_list_marker;
	bool in_paragraph;
	bool paragraph_has_content;
	bool bold;
	bool italic;
	bool underline;
	u8 underline_style;
	bool overline;
	bool strikethrough;
	bool superscript;
	bool subscript;
	bool mono;
	bool style_bold_stack[32];
	bool style_italic_stack[32];
	bool style_underline_stack[32];
	u8 style_underline_style_stack[32];
	bool style_overline_stack[32];
	bool style_strikethrough_stack[32];
	bool style_superscript_stack[32];
	bool style_subscript_stack[32];
	bool style_mono_stack[32];
	bool style_hidden_stack[32];
	bool link_active_stack[32];
	u16 link_href_id_stack[32];
	bool block_text_align_stack[32];
	u8 block_text_align_value_stack[32];
	bool list_marker_hidden_stack[32];
	bool list_item_pending_stack[32];
	unsigned int ordered_list_ordinal_stack[32];
	u8 ordered_list_style_stack[32];
	bool heading_font_size_emitted_stack[32];
	u8 heading_saved_font_size_stack[32];
	bool page_break_after_stack[32];
	bool deferred_style_sync;
	bool deferred_target_bold;
	bool deferred_target_italic;
	bool deferred_target_underline;
	u8 deferred_target_underline_style;
	bool deferred_target_overline;
	bool deferred_target_strikethrough;
	bool deferred_target_superscript;
	bool deferred_target_subscript;
	bool deferred_target_mono;
	bool table_in_header_section;
	bool table_in_caption;
	bool table_in_row;
	bool table_in_cell;
	bool table_current_cell_is_header;
	bool table_current_cell_is_row_header;
	std::string table_caption_text;
	std::string table_current_cell_text;
	std::vector<std::string> table_header_cells;
	std::vector<std::string> table_current_row_cells;
	std::vector<u8> table_current_row_header_flags;
	std::vector<std::vector<std::string> > table_body_rows;
	std::vector<std::vector<u8> > table_body_row_header_flags;
	std::string docpath; //! Current XHTML document path inside EPUB.
	std::string doc_title;   //! Current XHTML <title> text (best chapter label).
	std::string doc_heading; //! Fallback heading text from h1/h2/h3.
	bool doc_heading_complete;
	std::string last_p_style;    //! style= attr of the most-recently-opened <p>.
	std::string last_h1_style;   //! style= attr of the most-recently-opened <h1>.
	std::string last_h2_style;   //! style= attr of the most-recently-opened <h2>.
	std::string last_h_style;    //! style= attr of the most-recently-opened <h3..h6>.
	std::string last_hr_style;   //! style= attr of the most-recently-opened <hr>.
	std::string last_p_class;
	std::string last_h1_class;
	std::string last_h2_class;
	std::string last_h_class;
	std::string last_hr_class;
	epub_css_class_map::CssClassMap css_class_map;
	bool collecting_fb2_binary;
	bool fb2_binary_too_large;
	std::string fb2_binary_id;
	std::string fb2_binary_data;
	bool fb2_mode;
	int fb2_section_depth;
	int fb2_title_depth;
	int fb2_title_capture_depth;
	bool fb2_section_has_chapter[32];
	std::string fb2_title_text;
	u64 perf_chardata_ms;
	u32 perf_chardata_calls;
	u32 perf_inline_images;
	u32 perf_page_overflows;
	int status;
	int totalbytes;
	int pagecount;
};

bool iswhitespace(u32 c);

void parse_error(XML_ParserStruct *ps);
void parse_init(parsedata_t *data);
bool parse_append_page_byte(parsedata_t *data, u32 c);
bool parse_append_page_byte_soft(parsedata_t *data, u32 c,
                                 parse_page_flush_fn flush_page, void *ctx);
size_t parse_append_page_bytes(parsedata_t *data, const u32 *src, size_t len);
size_t parse_append_page_bytes_soft(parsedata_t *data, const u32 *src,
                                    size_t len,
                                    parse_page_flush_fn flush_page, void *ctx);
bool parse_in(parsedata_t *data, context_t context);
context_t parse_pop(parsedata_t *data);
bool parse_page_buffer_overflowed(const parsedata_t *data);
void parse_reset_page_buffer(parsedata_t *data);
void parse_printerror(XML_Parser p);
void parse_push(parsedata_t *data, context_t context);
