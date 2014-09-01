#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>

#include "stmd.h"
#include "uthash.h"
#include "debug.h"
#include "scanners.h"
#include "utf8.h"

typedef struct Subject {
  const gh_buf   *buffer;
  int            pos;
  reference**    reference_map;
  int            label_nestlevel;
} subject;

reference* lookup_reference(reference** refmap, chunk *label);
reference* make_reference(chunk *label, chunk *url, chunk *title);

static unsigned char *clean_url(chunk *url);
static unsigned char *clean_title(chunk *title);

inline static unsigned char *chunk_to_cstr(chunk *c);
inline static void chunk_free(chunk *c);
inline static void chunk_trim(chunk *c);

inline static chunk chunk_literal(const char *data);
inline static chunk chunk_buf_detach(gh_buf *buf);
inline static chunk chunk_buf(const gh_buf *buf, int pos, int len);

static inl *parse_chunk_inlines(chunk *chunk, reference** refmap);
static inl *parse_inlines_while(subject* subj, int (*f)(subject*));
static int parse_inline(subject* subj, inl ** last);

extern void free_reference(reference *ref) {
	free(ref->label);
	free(ref->url);
	free(ref->title);
	free(ref);
}

extern void free_reference_map(reference **refmap) {
	/* free the hash table contents */
	reference *s;
	reference *tmp;
	if (refmap != NULL) {
		HASH_ITER(hh, *refmap, s, tmp) {
			HASH_DEL(*refmap, s);
			free_reference(s);
		}
		free(refmap);
	}
}

// normalize reference:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
static unsigned char *normalize_reference(chunk *ref)
{
	gh_buf normalized = GH_BUF_INIT;
	int r, w;

	utf8proc_case_fold(&normalized, ref->data, ref->len);
	gh_buf_trim(&normalized);

	for (r = 0, w = 0; r < normalized.size; ++r) {
		if (r && gh_buf_at(&normalized, r - 1) == ' ') {
			while (gh_buf_at(&normalized, r) == ' ')
				r++;
		}

		normalized.ptr[w++] = normalized.ptr[r];
	}

	return gh_buf_detach(&normalized);
}

// Returns reference if refmap contains a reference with matching
// label, otherwise NULL.
extern reference* lookup_reference(reference** refmap, chunk *label)
{
	reference *ref = NULL;
	unsigned char *norm = normalize_reference(label);
	if (refmap != NULL) {
		HASH_FIND_STR(*refmap, (char*)norm, ref);
	}
	free(label);
	return ref;
}

extern reference* make_reference(chunk *label, chunk *url, chunk *title)
{
	reference *ref;
	ref = malloc(sizeof(reference));
	ref->label = normalize_reference(label);
	ref->url = clean_url(url);
	ref->title = clean_title(title);
	return ref;
}

extern void add_reference(reference** refmap, reference* ref)
{
	reference * t = NULL;
	HASH_FIND(hh, *refmap, (char*)ref->label, (unsigned)strlen(ref->label), t);

	if (t == NULL) {
		HASH_ADD_KEYPTR(hh, *refmap, (char*)ref->label, (unsigned)strlen(ref->label), ref);
	} else {
		free_reference(ref);  // we free this now since it won't be in the refmap
	}
}

// Create an inline with a linkable string value.
inline static inl* make_linkable(int t, inl* label, chunk url, chunk title)
{
	inl* e = (inl*) malloc(sizeof(inl));
	e->tag = t;
	e->content.linkable.label = label;
	e->content.linkable.url   = chunk_to_cstr(&url);
	e->content.linkable.title = chunk_to_cstr(&title);
	e->next = NULL;
	return e;
}

inline static inl* make_inlines(int t, inl* contents)
{
	inl* e = (inl*) malloc(sizeof(inl));
	e->tag = t;
	e->content.inlines = contents;
	e->next = NULL;
	return e;
}

// Create an inline with a literal string value.
inline static inl* make_literal(int t, chunk s)
{
	inl* e = (inl*) malloc(sizeof(inl));
	e->tag = t;
	e->content.literal = s;
	e->next = NULL;
	return e;
}

// Create an inline with no value.
inline static inl* make_simple(int t)
{
	inl* e = (inl*) malloc(sizeof(inl));
	e->tag = t;
	e->next = NULL;
	return e;
}

// Macros for creating various kinds of inlines.
#define make_str(s) make_literal(str, s)
#define make_code(s) make_literal(code, s)
#define make_raw_html(s) make_literal(raw_html, s)
#define make_entity(s) make_literal(entity, s)
#define make_linebreak() make_simple(linebreak)
#define make_softbreak() make_simple(softbreak)
#define make_link(label, url, title) make_linkable(link, label, url, title)
#define make_emph(contents) make_inlines(emph, contents)
#define make_strong(contents) make_inlines(strong, contents)

// Free an inline list.
extern void free_inlines(inl* e)
{
	inl * next;
	while (e != NULL) {
		switch (e->tag){
			case str:
			case raw_html:
			case code:
			case entity:
				chunk_free(&e->content.literal);
				break;
			case linebreak:
			case softbreak:
				break;
			case link:
			case image:
				free(e->content.linkable.url);
				free(e->content.linkable.title);
				free_inlines(e->content.linkable.label);
				break;
			case emph:
			case strong:
				free_inlines(e->content.inlines);
				break;
			default:
				break;
		}
		next = e->next;
		free(e);
		e = next;
	}
}

// Append inline list b to the end of inline list a.
// Return pointer to head of new list.
inline static inl* append_inlines(inl* a, inl* b)
{
	if (a == NULL) {  // NULL acts like an empty list
		return b;
	}
	inl* cur = a;
	while (cur->next) {
		cur = cur->next;
	}
	cur->next = b;
	return a;
}

// Make a 'subject' from an input string.
static void init_subject(subject *e, gh_buf *buffer, int input_pos, reference** refmap)
{
	e->buffer = buffer;
	e->pos = input_pos;
	e->label_nestlevel = 0;
	e->reference_map = refmap;
}

inline static int isbacktick(int c)
{
	return (c == '`');
}

inline static void chunk_free(chunk *c)
{
	if (c->alloc)
		free((char *)c->data);

	c->data = NULL;
	c->alloc = 0;
	c->len = 0;
}

inline static void chunk_trim(chunk *c)
{
	while (c->len && isspace(c->data[0])) {
		c->data++;
		c->len--;
	}

	while (c->len > 0) {
		if (!isspace(c->data[c->len - 1]))
			break;

		c->len--;
	}
}

inline static unsigned char *chunk_to_cstr(chunk *c)
{
	unsigned char *str;

	str = malloc(c->len + 1);
	memcpy(str, c->data, c->len);
	str[c->len] = 0;

	return str;
}

inline static chunk chunk_literal(const char *data)
{
	chunk c = {data, strlen(data), 0};
	return c;
}

inline static chunk chunk_buf(const gh_buf *buf, int pos, int len)
{
	chunk c = {buf->ptr + pos, len, 0};
	return c;
}

inline static chunk chunk_buf_detach(gh_buf *buf)
{
	chunk c;

	c.len = buf->size;
	c.data = gh_buf_detach(buf);
	c.alloc = 1;

	return c;
}

// Return the next character in the subject, without advancing.
// Return 0 if at the end of the subject.
#define peek_char(subj) gh_buf_at((subj)->buffer, (subj)->pos)

// Return true if there are more characters in the subject.
inline static int is_eof(subject* subj)
{
	return (subj->pos >= gh_buf_len(subj->buffer));
}

// Advance the subject.  Doesn't check for eof.
#define advance(subj) (subj)->pos += 1

// Take characters while a predicate holds, and return a string.
inline static chunk take_while(subject* subj, int (*f)(int))
{
	unsigned char c;
	int startpos = subj->pos;
	int len = 0;

	while ((c = peek_char(subj)) && (*f)(c)) {
		advance(subj);
		len++;
	}

	return chunk_buf(subj->buffer, startpos, len);
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
static int scan_to_closing_backticks(subject* subj, int openticklength)
{
	// read non backticks
	char c;
	while ((c = peek_char(subj)) && c != '`') {
		advance(subj);
	}
	if (is_eof(subj)) {
		return 0;  // did not find closing ticks, return 0
	}
	int numticks = 0;
	while (peek_char(subj) == '`') {
		advance(subj);
		numticks++;
	}
	if (numticks != openticklength){
		return(scan_to_closing_backticks(subj, openticklength));
	}
	return (subj->pos);
}

// Destructively modify string, collapsing consecutive
// space and newline characters into a single space.
static void normalize_whitespace(gh_buf *s)
{
	/* TODO */
#if 0
	bool last_char_was_space = false;
	int pos = 0;
	char c;
	while ((c = gh_buf_at(s, pos))) {
		switch (c) {
			case ' ':
				if (last_char_was_space) {
					bdelete(s, pos, 1);
				} else {
					pos++;
				}
				last_char_was_space = true;
				break;
			case '\n':
				if (last_char_was_space) {
					bdelete(s, pos, 1);
				} else {
					bdelete(s, pos, 1);
					binsertch(s, pos, 1, ' ');
					pos++;
				}
				last_char_was_space = true;
				break;
			default:
				pos++;
				last_char_was_space = false;
		}
	}
#endif
}

// Parse backtick code section or raw backticks, return an inline.
// Assumes that the subject has a backtick at the current position.
static inl* handle_backticks(subject *subj)
{
	chunk openticks = take_while(subj, isbacktick);
	int startpos = subj->pos;
	int endpos = scan_to_closing_backticks(subj, openticks.len);

	if (endpos == 0) { // not found
		subj->pos = startpos; // rewind
		return make_str(openticks);
	} else {
		gh_buf buf = GH_BUF_INIT;

		gh_buf_set(&buf, subj->buffer->ptr + startpos, endpos - startpos - openticks.len);
		gh_buf_trim(&buf);
		normalize_whitespace(&buf);

		return make_code(chunk_buf_detach(&buf));
	}
}

// Scan ***, **, or * and return number scanned, or 0.
// Don't advance position.
static int scan_delims(subject* subj, char c, bool * can_open, bool * can_close)
{
	int numdelims = 0;
	char char_before, char_after;
	int startpos = subj->pos;

	char_before = subj->pos == 0 ? '\n' : gh_buf_at(subj->buffer, subj->pos - 1);
	while (peek_char(subj) == c) {
		numdelims++;
		advance(subj);
	}
	char_after = peek_char(subj);
	*can_open = numdelims > 0 && numdelims <= 3 && !isspace(char_after);
	*can_close = numdelims > 0 && numdelims <= 3 && !isspace(char_before);
	if (c == '_') {
		*can_open = *can_open && !isalnum(char_before);
		*can_close = *can_close && !isalnum(char_after);
	}
	subj->pos = startpos;
	return numdelims;
}

// Parse strong/emph or a fallback.
// Assumes the subject has '_' or '*' at the current position.
static inl* handle_strong_emph(subject* subj, char c)
{
	bool can_open, can_close;
	inl * result = NULL;
	inl ** last = malloc(sizeof(inl *));
	inl * new;
	inl * il;
	inl * first_head = NULL;
	inl * first_close = NULL;
	int first_close_delims = 0;
	int numdelims;

	*last = NULL;

	numdelims = scan_delims(subj, c, &can_open, &can_close);
	subj->pos += numdelims;

	new = make_str(chunk_buf(subj->buffer, subj->pos - numdelims, numdelims));
	*last = new;
	first_head = new;
	result = new;

	if (!can_open || numdelims == 0) {
		goto done;
	}

	switch (numdelims) {
		case 1:
			while (true) {
				numdelims = scan_delims(subj, c, &can_open, &can_close);
				if (numdelims >= 1 && can_close) {
					subj->pos += 1;
					first_head->tag = emph;
					chunk_free(&first_head->content.literal);
					first_head->content.inlines = first_head->next;
					first_head->next = NULL;
					goto done;
				} else {
					if (!parse_inline(subj, last)) {
						goto done;
					}
				}
			}
			break;
		case 2:
			while (true) {
				numdelims = scan_delims(subj, c, &can_open, &can_close);
				if (numdelims >= 2 && can_close) {
					subj->pos += 2;
					first_head->tag = strong;
					chunk_free(&first_head->content.literal);
					first_head->content.inlines = first_head->next;
					first_head->next = NULL;
					goto done;
				} else {
					if (!parse_inline(subj, last)) {
						goto done;
					}
				}
			}
			break;
		case 3:
			while (true) {
				numdelims = scan_delims(subj, c, &can_open, &can_close);
				if (can_close && numdelims >= 1 && numdelims <= 3 &&
						numdelims != first_close_delims) {
					new = make_str(chunk_buf(subj->buffer, subj->pos, numdelims));
					append_inlines(*last, new);
					*last = new;
					if (first_close_delims == 1 && numdelims > 2) {
						numdelims = 2;
					} else if (first_close_delims == 2) {
						numdelims = 1;
					} else if (numdelims == 3) {
						// If we opened with ***, we interpret it as ** followed by *
						// giving us <strong><em>
						numdelims = 1;
					}
					subj->pos += numdelims;
					if (first_close) {
						first_head->tag = first_close_delims == 1 ? strong : emph;
						chunk_free(&first_head->content.literal);
						first_head->content.inlines =
							make_inlines(first_close_delims == 1 ? emph : strong,
									first_head->next);

						il = first_head->next;
						while (il->next && il->next != first_close) {
							il = il->next;
						}
						il->next = NULL;

						first_head->content.inlines->next = first_close->next;

						il = first_head->content.inlines;
						while (il->next && il->next != *last) {
							il = il->next;
						}
						il->next = NULL;
						free_inlines(*last);

						first_close->next = NULL;
						free_inlines(first_close);
						first_head->next = NULL;
						goto done;
					} else {
						first_close = *last;
						first_close_delims = numdelims;
					}
				} else {
					if (!parse_inline(subj, last)) {
						goto done;
					}
				}
			}
			break;
		default:
			goto done;
	}

done:
	free(last);
	return result;
}

// Parse backslash-escape or just a backslash, returning an inline.
static inl* handle_backslash(subject *subj)
{
	advance(subj);
	unsigned char nextchar = peek_char(subj);
	if (ispunct(nextchar)) {  // only ascii symbols and newline can be escaped
		advance(subj);
		return make_str(chunk_buf(subj->buffer, subj->pos - 1, 1));
	} else if (nextchar == '\n') {
		advance(subj);
		return make_linebreak();
	} else {
		return make_str(chunk_literal("\\"));
	}
}

// Parse an entity or a regular "&" string.
// Assumes the subject has an '&' character at the current position.
static inl* handle_entity(subject* subj)
{
	int match;
	inl *result;
	match = scan_entity(subj->buffer, subj->pos);
	if (match) {
		result = make_entity(chunk_buf(subj->buffer, subj->pos, match));
		subj->pos += match;
	} else {
		advance(subj);
		result = make_str(chunk_literal("&"));
	}
	return result;
}

// Like make_str, but parses entities.
// Returns an inline sequence consisting of str and entity elements.
static inl *make_str_with_entities(chunk *content)
{
	inl * result = NULL;
	inl * new;
	int searchpos;
	char c;
	subject subj;
	gh_buf content_buf = GH_BUF_INIT;

	gh_buf_set(&content_buf, content->data, content->len);
	init_subject(&subj, &content_buf, 0, NULL);

	while ((c = peek_char(&subj))) {
		switch (c) {
			case '&':
				new = handle_entity(&subj);
				break;
			default:
				searchpos = gh_buf_strchr(subj.buffer, '&', subj.pos);
				if (searchpos < 0) {
					searchpos = gh_buf_len(subj.buffer);
				}

				new = make_str(chunk_buf(subj.buffer, subj.pos, searchpos - subj.pos));
				subj.pos = searchpos;
		}
		result = append_inlines(result, new);
	}

	gh_buf_free(&content_buf);
	return result;
}

// Destructively unescape a string: remove backslashes before punctuation chars.
extern void unescape_buffer(gh_buf *buf)
{
	int r, w;

	for (r = 0, w = 0; r < buf->size; ++r) {
		if (buf->ptr[r] == '\\' && ispunct(buf->ptr[r + 1]))
			continue;

		buf->ptr[w++] = buf->ptr[r];
	}

	gh_buf_truncate(buf, w);
}

// Clean a URL: remove surrounding whitespace and surrounding <>,
// and remove \ that escape punctuation.
static unsigned char *clean_url(chunk *url)
{
	gh_buf buf = GH_BUF_INIT;

	chunk_trim(url);

	if (url->data[0] == '<' && url->data[url->len - 1] == '>') {
		gh_buf_set(&buf, url->data + 1, url->len - 2);
	} else {
		gh_buf_set(&buf, url->data, url->len);
	}

	unescape_buffer(&buf);
	return gh_buf_detach(&buf);
}

// Clean a title: remove surrounding quotes and remove \ that escape punctuation.
static unsigned char *clean_title(chunk *title)
{
	gh_buf buf = GH_BUF_INIT;
	unsigned char first = title->data[0];
	unsigned char last = title->data[title->len - 1];

	// remove surrounding quotes if any:
	if ((first == '\'' && last == '\'') ||
		(first == '(' && last == ')') ||
		(first == '"' && last == '"')) {
		gh_buf_set(&buf, title->data + 1, title->len - 2);
	} else {
		gh_buf_set(&buf, title->data, title->len);
	}

	unescape_buffer(&buf);
	return gh_buf_detach(&buf);
}

// Parse an autolink or HTML tag.
// Assumes the subject has a '<' character at the current position.
static inl* handle_pointy_brace(subject* subj)
{
	int matchlen = 0;
	chunk contents;

	advance(subj);  // advance past first <

	// first try to match a URL autolink
	matchlen = scan_autolink_uri(subj->buffer, subj->pos);
	if (matchlen > 0) {
		contents = chunk_buf(subj->buffer, subj->pos, matchlen - 1);
		subj->pos += matchlen;

		return make_link(
			make_str_with_entities(&contents),
			contents,
			chunk_literal("")
		);
	}

	// next try to match an email autolink
	matchlen = scan_autolink_email(subj->buffer, subj->pos);
	if (matchlen > 0) {
		gh_buf mail_url = GH_BUF_INIT;

		contents = chunk_buf(subj->buffer, subj->pos, matchlen - 1);
		subj->pos += matchlen;

		gh_buf_puts(&mail_url, "mailto:");
		gh_buf_put(&mail_url, contents.data, contents.len);

		return make_link(
				make_str_with_entities(&contents),
				chunk_buf_detach(&mail_url),
				chunk_literal("")
		);
	}

	// finally, try to match an html tag
	matchlen = scan_html_tag(subj->buffer, subj->pos);
	if (matchlen > 0) {
		contents = chunk_buf(subj->buffer, subj->pos - 1, matchlen + 1);
		subj->pos += matchlen;
		return make_raw_html(contents);
	}

	// if nothing matches, just return the opening <:
	return make_str(chunk_literal("<"));
}

// Parse a link label.  Returns 1 if successful.
// Unless raw_label is null, it is set to point to the raw contents of the [].
// Assumes the subject has a '[' character at the current position.
// Returns 0 and does not advance if no matching ] is found.
// Note the precedence:  code backticks have precedence over label bracket
// markers, which have precedence over *, _, and other inline formatting
// markers. So, 2 below contains a link while 1 does not:
// 1. [a link `with a ](/url)` character
// 2. [a link *with emphasized ](/url) text*
static int link_label(subject* subj, chunk *raw_label)
{
	int nestlevel = 0;
	inl* tmp = NULL;
	int startpos = subj->pos;

	if (subj->label_nestlevel) {
		// if we've already checked to the end of the subject
		// for a label, even with a different starting [, we
		// know we won't find one here and we can just return.
		// Note:  nestlevel 1 would be: [foo [bar]
		// nestlevel 2 would be: [foo [bar [baz]
		subj->label_nestlevel--;
		return 0;
	}

	advance(subj);  // advance past [
	char c;
	while ((c = peek_char(subj)) && (c != ']' || nestlevel > 0)) {
		switch (c) {
			case '`':
				tmp = handle_backticks(subj);
				free_inlines(tmp);
				break;
			case '<':
				tmp = handle_pointy_brace(subj);
				free_inlines(tmp);
				break;
			case '[':  // nested []
				nestlevel++;
				advance(subj);
				break;
			case ']':  // nested []
				nestlevel--;
				advance(subj);
				break;
			case '\\':
				advance(subj);
				if (ispunct(peek_char(subj))) {
					advance(subj);
				}
				break;
			default:
				advance(subj);
		}
	}
	if (c == ']') {
		*raw_label = chunk_buf(
			subj->buffer,
			startpos + 1,
			subj->pos - (startpos + 1)
		);

		subj->label_nestlevel = 0;
		advance(subj);  // advance past ]
		return 1;
	} else {
		if (c == 0) {
			subj->label_nestlevel = nestlevel;
		}
		subj->pos = startpos; // rewind
		return 0;
	}
}

// Parse a link or the link portion of an image, or return a fallback.
static inl* handle_left_bracket(subject* subj)
{
	inl *lab = NULL;
	inl *result = NULL;
	reference *ref;
	int n;
	int sps;
	int found_label;
	int endlabel, starturl, endurl, starttitle, endtitle, endall;

	chunk rawlabel;
	chunk url, title;

	found_label = link_label(subj, &rawlabel);
	endlabel = subj->pos;

	if (found_label) {
		if (peek_char(subj) == '(' &&
				((sps = scan_spacechars(subj->buffer, subj->pos + 1)) > -1) &&
				((n = scan_link_url(subj->buffer, subj->pos + 1 + sps)) > -1)) {

			// try to parse an explicit link:
			starturl = subj->pos + 1 + sps; // after (
			endurl = starturl + n;
			starttitle = endurl + scan_spacechars(subj->buffer, endurl);

			// ensure there are spaces btw url and title
			endtitle = (starttitle == endurl) ? starttitle :
				starttitle + scan_link_title(subj->buffer, starttitle);

			endall = endtitle + scan_spacechars(subj->buffer, endtitle);

			if (gh_buf_at(subj->buffer, endall) == ')') {
				subj->pos = endall + 1;

				url = chunk_buf(subj->buffer, starturl, endurl - starturl);
				title = chunk_buf(subj->buffer, starttitle, endtitle - starttitle);
				lab = parse_chunk_inlines(&rawlabel, NULL);

				return make_link(lab, url, title);
			} else {
				// if we get here, we matched a label but didn't get further:
				subj->pos = endlabel;
				lab = parse_chunk_inlines(&rawlabel, subj->reference_map);
				result = append_inlines(make_str(chunk_literal("[")),
						append_inlines(lab,
							make_str(chunk_literal("]"))));
				return result;
			}
		} else {
			chunk rawlabel_tmp;
			chunk reflabel;

			// Check for reference link.
			// First, see if there's another label:
			subj->pos = subj->pos + scan_spacechars(subj->buffer, endlabel);
			reflabel = rawlabel;

			// if followed by a nonempty link label, we change reflabel to it:
			if (peek_char(subj) == '[' && link_label(subj, &rawlabel_tmp)) {
				if (rawlabel_tmp.len > 0)
					reflabel = rawlabel_tmp;
			} else {
				subj->pos = endlabel;
			}

			// lookup rawlabel in subject->reference_map:
			ref = lookup_reference(subj->reference_map, &reflabel);
			if (ref != NULL) { // found
				lab = parse_chunk_inlines(&rawlabel, NULL);
				result = make_link(lab, chunk_literal(ref->url), chunk_literal(ref->title));
			} else {
				subj->pos = endlabel;
				lab = parse_chunk_inlines(&rawlabel, subj->reference_map);
				result = append_inlines(make_str(chunk_literal("[")),
						append_inlines(lab, make_str(chunk_literal("]"))));
			}
			return result;
		}
	}
	// If we fall through to here, it means we didn't match a link:
	advance(subj);  // advance past [
	return make_str(chunk_literal("["));
}

// Parse a hard or soft linebreak, returning an inline.
// Assumes the subject has a newline at the current position.
static inl* handle_newline(subject *subj)
{
	int nlpos = subj->pos;
	// skip over newline
	advance(subj);
	// skip spaces at beginning of line
	while (peek_char(subj) == ' ') {
		advance(subj);
	}
	if (nlpos > 1 &&
			gh_buf_at(subj->buffer, nlpos - 1) == ' ' &&
			gh_buf_at(subj->buffer, nlpos - 2) == ' ') {
		return make_linebreak();
	} else {
		return make_softbreak();
	}
}

inline static int not_eof(subject* subj)
{
	return !is_eof(subj);
}

// Parse inlines while a predicate is satisfied.  Return inlines.
extern inl* parse_inlines_while(subject* subj, int (*f)(subject*))
{
	inl* result = NULL;
	inl** last = &result;
	while ((*f)(subj) && parse_inline(subj, last)) {
	}
	return result;
}

inl *parse_chunk_inlines(chunk *chunk, reference** refmap)
{
	inl *result;
	subject subj;
	gh_buf full_chunk = GH_BUF_INIT;

	gh_buf_set(&full_chunk, chunk->data, chunk->len);
	init_subject(&subj, &full_chunk, 0, refmap);
	result = parse_inlines_while(&subj, not_eof);

	gh_buf_free(&full_chunk);
	return result;
}

static int find_special_char(subject *subj)
{
	int n = subj->pos + 1;
	int size = (int)gh_buf_len(subj->buffer);

	while (n < size) {
		if (strchr("\n\\`&_*[]<!", gh_buf_at(subj->buffer, n)))
			return n;
	}

	return -1;
}

// Parse an inline, advancing subject, and add it to last element.
// Adjust tail to point to new last element of list.
// Return 0 if no inline can be parsed, 1 otherwise.
static int parse_inline(subject* subj, inl ** last)
{
	inl* new = NULL;
	chunk contents;
	unsigned char c;
	int endpos;
	c = peek_char(subj);
	if (c == 0) {
		return 0;
	}
	switch(c){
		case '\n':
			new = handle_newline(subj);
			break;
		case '`':
			new = handle_backticks(subj);
			break;
		case '\\':
			new = handle_backslash(subj);
			break;
		case '&':
			new = handle_entity(subj);
			break;
		case '<':
			new = handle_pointy_brace(subj);
			break;
		case '_':
			if (subj->pos > 0 && (isalnum(gh_buf_at(subj->buffer, subj->pos - 1)) ||
						gh_buf_at(subj->buffer, subj->pos - 1) == '_')) {
				goto text_literal;
			}

			new = handle_strong_emph(subj, '_');
			break;
		case '*':
			new = handle_strong_emph(subj, '*');
			break;
		case '[':
			new = handle_left_bracket(subj);
			break;
		case '!':
			advance(subj);
			if (peek_char(subj) == '[') {
				new = handle_left_bracket(subj);
				if (new != NULL && new->tag == link) {
					new->tag = image;
				} else {
					new = append_inlines(make_str(chunk_literal("!")), new);
				}
			} else {
				new = make_str(chunk_literal("!"));
			}
			break;
		default:
		text_literal:
			endpos = find_special_char(subj);
			if (endpos < 0) {
				endpos = gh_buf_len(subj->buffer);
			}

			contents = chunk_buf(subj->buffer, subj->pos, endpos - subj->pos);
			subj->pos = endpos;

			// if we're at a newline, strip trailing spaces.
			if (peek_char(subj) == '\n') {
				chunk_trim(&contents);
			}

			new = make_str(contents);
	}
	if (*last == NULL) {
		*last = new;
	} else {
		append_inlines(*last, new);
	}
	return 1;
}

extern inl* parse_inlines(gh_buf *input, int input_pos, reference** refmap)
{
	subject subj;
	init_subject(&subj, input, input_pos, refmap);
	return parse_inlines_while(&subj, not_eof);
}

// Parse zero or more space characters, including at most one newline.
void spnl(subject* subj)
{
	bool seen_newline = false;
	while (peek_char(subj) == ' ' ||
			(!seen_newline &&
			 (seen_newline = peek_char(subj) == '\n'))) {
		advance(subj);
	}
}

// Parse reference.  Assumes string begins with '[' character.
// Modify refmap if a reference is encountered.
// Return 0 if no reference found, otherwise position of subject
// after reference is parsed.
extern int parse_reference(gh_buf *input, int input_pos, reference** refmap)
{
	subject subj;

	chunk lab;
	chunk url;
	chunk title;

	int matchlen = 0;
	int beforetitle;
	reference * new = NULL;

	init_subject(&subj, input, input_pos, NULL);

	// parse label:
	if (!link_label(&subj, &lab))
		return 0;

	// colon:
	if (peek_char(&subj) == ':') {
		advance(&subj);
	} else {
		return 0;
	}

	// parse link url:
	spnl(&subj);
	matchlen = scan_link_url(subj.buffer, subj.pos);
	if (matchlen) {
		url = chunk_buf(subj.buffer, subj.pos, matchlen);
		subj.pos += matchlen;
	} else {
		return 0;
	}

	// parse optional link_title
	beforetitle = subj.pos;
	spnl(&subj);
	matchlen = scan_link_title(subj.buffer, subj.pos);
	if (matchlen) {
		title = chunk_buf(subj.buffer, subj.pos, matchlen);
		subj.pos += matchlen;
	} else {
		subj.pos = beforetitle;
		title = chunk_literal("");
	}
	// parse final spaces and newline:
	while (peek_char(&subj) == ' ') {
		advance(&subj);
	}
	if (peek_char(&subj) == '\n') {
		advance(&subj);
	} else if (peek_char(&subj) != 0) {
		return 0;
	}
	// insert reference into refmap
	new = make_reference(&lab, &url, &title);
	add_reference(refmap, new);

	return subj.pos;
}

