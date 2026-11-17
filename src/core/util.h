/* nextcloud-handler - helpers shared by all parts. Public domain. */
#ifndef NCH_UTIL_H
#define NCH_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Growable byte buffer, always NUL-terminated. */
typedef struct {
	char  *data;
	size_t len;
	size_t cap;
	int    failed;   /* set when an allocation failed */
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
void sb_clear(strbuf *sb);
int  sb_append(strbuf *sb, const void *data, size_t len);
int  sb_puts(strbuf *sb, const char *s);
int  sb_printf(strbuf *sb, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;
/* Takes the string out of the buffer; the caller frees it. */
char *sb_steal(strbuf *sb);

/* Percent-encodes a path. '/' is kept, all bytes outside the RFC 3986
 * unreserved set are encoded. */
int  url_encode_path(strbuf *out, const char *path);
/* Decodes %XX sequences. Returns a malloc'ed string or NULL. */
char *url_decode(const char *s, size_t len);

/* Parses an RFC 1123 date ("Sun, 06 Nov 1994 08:49:37 GMT").
 * Returns 0 and sets *t (UTC seconds since 1970) on success. */
int  parse_http_date(const char *s, time_t *t);
/* UTC broken-down time to seconds since 1970 (portable timegm). */
int64_t days_from_civil(int y, unsigned m, unsigned d);

/* Compares two UTF-8 names without regard to case. ASCII and Latin-1
 * letters are folded, which covers the names an Amiga can create. */
int  name_casecmp(const char *a, const char *b);

/* Path helpers for "/a/b/c" style paths (UTF-8, '/' separated, root "/"). */
char *path_parent(const char *path);           /* "/a/b" -> "/a", "/a" -> "/" */
const char *path_basename(const char *path);   /* pointer into path */
char *path_join(const char *dir, const char *name);
/* Normalises "", "//a//b/" and similar into "/a/b". malloc'ed. */
char *path_normalize(const char *path);
/* True when path equals prefix or lies below it. */
int  path_is_under(const char *path, const char *prefix);

char *xstrdup(const char *s);

/* A copy of a message with lines longer than width broken at spaces (or
 * cut, when a line has none), so that a requester fits the screen.
 * malloc'ed; NULL when out of memory. */
char *text_wrap(const char *s, size_t width);

#endif
