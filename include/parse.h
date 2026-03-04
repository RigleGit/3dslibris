#pragma once

#include <expat.h>
#include <3ds.h>
#include "text.h"

#define PAGEBUFSIZE 2048

//! Symbols for known XHTML tags.

//! Not all tags here necessary affect rendering.
typedef enum {
	TAG_ANCHOR,
	TAG_BR,TAG_BODY,
	TAG_DIV,TAG_DT,
	TAG_H1,TAG_H2,TAG_H3,TAG_H4,TAG_H5,TAG_H6,TAG_HTML,TAG_HEAD,
	TAG_NONE,
	TAG_OL,
	TAG_P,TAG_PRE,
	TAG_SCRIPT,TAG_STYLE,
	TAG_TD,TAG_TITLE,
	TAG_STRONG,TAG_EM,
	TAG_UL,TAG_UNKNOWN
} context_t;

//! Expat parsing state.

//! This data structure is made available
//! to all expat callbacks via (void*)data.
typedef struct {
	context_t stack[32];
	u8 stacksize;
	class App *app;
	class Text *ts;  //! Text renderer.
	class Book *book;
	class Prefs *prefs;
	int screen;
	FT_Vector pen;
	u8 buf[PAGEBUFSIZE];
	FILE *cachefile;
	int buflen;
	//! Our total parse position in terms of cooked text.
	int pos;
	bool linebegan;
	bool bold;
	bool italic;
	int status;
	int totalbytes;
	int pagecount;
} parsedata_t;

bool iswhitespace(u8 c);

void parse_error(XML_ParserStruct *ps);
void parse_init(parsedata_t *data);
bool parse_in(parsedata_t *data, context_t context);
context_t parse_pop(parsedata_t *data);
void parse_printerror(XML_Parser p);
void parse_push(parsedata_t *data, context_t context);
