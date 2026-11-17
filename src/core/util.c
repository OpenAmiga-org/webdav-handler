/* webdav-handler - helpers shared by all parts. Public domain. */
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(strbuf *sb)
{
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
	sb->failed = 0;
}

void sb_free(strbuf *sb)
{
	free(sb->data);
	sb_init(sb);
}

void sb_clear(strbuf *sb)
{
	sb->len = 0;
	if (sb->data != NULL)
		sb->data[0] = '\0';
}

static int sb_reserve(strbuf *sb, size_t extra)
{
	size_t need, cap;
	char *p;

	if (sb->failed)
		return -1;
	need = sb->len + extra + 1;
	if (need <= sb->cap)
		return 0;
	cap = sb->cap ? sb->cap : 64;
	while (cap < need)
		cap *= 2;
	p = realloc(sb->data, cap);
	if (p == NULL) {
		sb->failed = 1;
		return -1;
	}
	/* always a string, also before anything was appended */
	p[sb->len] = '\0';
	sb->data = p;
	sb->cap = cap;
	return 0;
}

int sb_append(strbuf *sb, const void *data, size_t len)
{
	if (sb_reserve(sb, len) != 0)
		return -1;
	if (len > 0)
		memcpy(sb->data + sb->len, data, len);
	sb->len += len;
	sb->data[sb->len] = '\0';
	return 0;
}

int sb_puts(strbuf *sb, const char *s)
{
	return sb_append(sb, s, strlen(s));
}

int sb_printf(strbuf *sb, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0 || sb_reserve(sb, (size_t)n) != 0)
		return -1;
	va_start(ap, fmt);
	vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	sb->len += (size_t)n;
	return 0;
}

char *sb_steal(strbuf *sb)
{
	char *s;

	if (sb->failed) {
		sb_free(sb);
		return NULL;
	}
	if (sb->data == NULL && sb_reserve(sb, 0) != 0)
		return NULL;
	s = sb->data;
	sb_init(sb);
	return s;
}

static int is_unreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
	       c == '~';
}

int url_encode_path(strbuf *out, const char *path)
{
	static const char hex[] = "0123456789ABCDEF";
	const unsigned char *p;
	char enc[3];

	for (p = (const unsigned char *)path; *p != '\0'; p++) {
		if (*p == '/' || is_unreserved(*p)) {
			if (sb_append(out, p, 1) != 0)
				return -1;
		} else {
			enc[0] = '%';
			enc[1] = hex[*p >> 4];
			enc[2] = hex[*p & 15];
			if (sb_append(out, enc, 3) != 0)
				return -1;
		}
	}
	return out->failed ? -1 : 0;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

char *url_decode(const char *s, size_t len)
{
	char *out, *o;
	size_t i;

	out = malloc(len + 1);
	if (out == NULL)
		return NULL;
	o = out;
	for (i = 0; i < len; i++) {
		if (s[i] == '%' && i + 2 < len && hexval(s[i + 1]) >= 0 &&
		    hexval(s[i + 2]) >= 0) {
			*o++ = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
			i += 2;
		} else {
			*o++ = s[i];
		}
	}
	*o = '\0';
	return out;
}

/* Howard Hinnant's days_from_civil: days since 1970-01-01. */
int64_t days_from_civil(int y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

int parse_http_date(const char *s, time_t *t)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4];
	int day, year, hh, mm, ss, m;
	const char *p;

	/* Skip the weekday ("Sun, "). */
	p = strchr(s, ',');
	p = (p != NULL) ? p + 1 : s;
	if (sscanf(p, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6)
		return -1;
	for (m = 0; m < 12; m++) {
		if (strncmp(months + m * 3, mon, 3) == 0)
			break;
	}
	if (m == 12 || day < 1 || day > 31 || year < 1970)
		return -1;
	*t = (time_t)(days_from_civil(year, (unsigned)m + 1, (unsigned)day) * 86400 +
	              hh * 3600 + mm * 60 + ss);
	return 0;
}

/* Decodes one UTF-8 sequence; returns the code point and advances *s.
 * Invalid bytes are returned as themselves. */
static unsigned long utf8_next(const unsigned char **s)
{
	const unsigned char *p = *s;
	unsigned long c = *p;

	if (c < 0x80) {
		*s = p + 1;
		return c;
	}
	if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
		*s = p + 2;
		return ((c & 0x1F) << 6) | (p[1] & 0x3F);
	}
	if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
		*s = p + 3;
		return ((c & 0x0F) << 12) | ((unsigned long)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
	}
	if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
	    (p[3] & 0xC0) == 0x80) {
		*s = p + 4;
		return ((c & 0x07) << 18) | ((unsigned long)(p[1] & 0x3F) << 12) |
		       ((unsigned long)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
	}
	*s = p + 1;
	return c;
}

static unsigned long fold(unsigned long c)
{
	if (c >= 'A' && c <= 'Z')
		return c + 32;
	/* Latin-1 capitals, except the multiplication sign. */
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
		return c + 32;
	return c;
}

int name_casecmp(const char *a, const char *b)
{
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	unsigned long ca, cb;

	for (;;) {
		if (*pa == '\0' || *pb == '\0')
			return (int)*pa - (int)*pb;
		ca = fold(utf8_next(&pa));
		cb = fold(utf8_next(&pb));
		if (ca != cb)
			return ca < cb ? -1 : 1;
	}
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

char *path_normalize(const char *path)
{
	strbuf sb;
	const char *p = path, *start;

	sb_init(&sb);
	while (*p != '\0') {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		start = p;
		while (*p != '\0' && *p != '/')
			p++;
		sb_puts(&sb, "/");
		sb_append(&sb, start, (size_t)(p - start));
	}
	if (sb.len == 0)
		sb_puts(&sb, "/");
	return sb_steal(&sb);
}

char *path_parent(const char *path)
{
	const char *slash = strrchr(path, '/');
	char *r;
	size_t n;

	if (slash == NULL || slash == path)
		return xstrdup("/");
	n = (size_t)(slash - path);
	r = malloc(n + 1);
	if (r != NULL) {
		memcpy(r, path, n);
		r[n] = '\0';
	}
	return r;
}

const char *path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

char *path_join(const char *dir, const char *name)
{
	strbuf sb;

	sb_init(&sb);
	sb_puts(&sb, dir);
	if (sb.len == 0 || sb.data[sb.len - 1] != '/')
		sb_puts(&sb, "/");
	sb_puts(&sb, name);
	return sb_steal(&sb);
}

int path_is_under(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);

	if (strcmp(prefix, "/") == 0)
		return 1;
	if (strncmp(path, prefix, n) != 0)
		return 0;
	return path[n] == '\0' || path[n] == '/';
}

char *text_wrap(const char *s, size_t width)
{
	size_t n = strlen(s), col = 0, i, o = 0, last_space = (size_t)-1;
	/* every cut can add one line break */
	char *out = malloc(n + n / (width > 0 ? width : 1) + 2);

	if (out == NULL)
		return NULL;
	for (i = 0; i < n; i++) {
		char c = s[i];

		if (c == '\n') {
			out[o++] = c;
			col = 0;
			last_space = (size_t)-1;
			continue;
		}
		/* UTF-8 continuation bytes take no room of their own */
		if (((unsigned char)c & 0xC0) != 0x80)
			col++;
		if (c == ' ')
			last_space = o;
		out[o++] = c;
		if (col > width) {
			if (last_space != (size_t)-1) {
				out[last_space] = '\n';
				col = 0;
				for (size_t k = last_space + 1; k < o; k++)
					if (((unsigned char)out[k] & 0xC0) != 0x80)
						col++;
			} else if (i + 1 < n && s[i + 1] != '\n') {
				out[o++] = '\n';
				col = 0;
			}
			last_space = (size_t)-1;
		}
	}
	out[o] = '\0';
	return out;
}
