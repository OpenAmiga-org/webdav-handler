/* nextcloud-handler - Amiga 8-bit character set <-> UTF-8. Public domain. */
#include "charset.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ISO-8859-15 differs from ISO-8859-1 in eight places. */
static const struct { unsigned char byte; unsigned cp; } iso15[] = {
	{ 0xA4, 0x20AC }, { 0xA6, 0x0160 }, { 0xA8, 0x0161 }, { 0xB4, 0x017D },
	{ 0xB8, 0x017E }, { 0xBC, 0x0152 }, { 0xBD, 0x0153 }, { 0xBE, 0x0178 },
};

int cs_from_name(const char *name)
{
	if (name != NULL && (strcasecmp(name, "ISO-8859-15") == 0 ||
	                     strcasecmp(name, "ISO8859-15") == 0 ||
	                     strcasecmp(name, "Latin-9") == 0))
		return CS_ISO_8859_15;
	return CS_ISO_8859_1;
}

static unsigned byte_to_cp(int cs, unsigned char b)
{
	size_t i;

	if (cs == CS_ISO_8859_15) {
		for (i = 0; i < sizeof(iso15) / sizeof(iso15[0]); i++) {
			if (iso15[i].byte == b)
				return iso15[i].cp;
		}
	}
	return b;
}

/* Returns the local byte, or -1 when there is none. */
static int cp_to_byte(int cs, unsigned cp)
{
	size_t i;

	if (cs == CS_ISO_8859_15) {
		for (i = 0; i < sizeof(iso15) / sizeof(iso15[0]); i++) {
			if (iso15[i].cp == cp)
				return iso15[i].byte;
			if (iso15[i].byte == cp)
				return -1; /* replaced in ISO-8859-15 */
		}
	}
	/* control characters stay escaped so that names stay printable */
	if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0))
		return -1;
	return cp <= 0xFF ? (int)cp : -1;
}

static void put_utf8(strbuf *sb, unsigned cp)
{
	char b[4];

	if (cp < 0x80) {
		b[0] = (char)cp;
		sb_append(sb, b, 1);
	} else if (cp < 0x800) {
		b[0] = (char)(0xC0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3F));
		sb_append(sb, b, 2);
	} else if (cp < 0x10000) {
		b[0] = (char)(0xE0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F));
		sb_append(sb, b, 3);
	} else {
		b[0] = (char)(0xF0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[3] = (char)(0x80 | (cp & 0x3F));
		sb_append(sb, b, 4);
	}
}

static int hexdigits(const char *s, int n, unsigned *out)
{
	unsigned v = 0;
	int i;

	for (i = 0; i < n; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

char *cs_local_to_utf8(int cs, const char *local)
{
	const unsigned char *p = (const unsigned char *)local;
	strbuf sb;
	unsigned cp;

	sb_init(&sb);
	while (*p != '\0') {
		if (p[0] == '%' && p[1] == 'u' && hexdigits((const char *)p + 2, 4, &cp) == 0) {
			put_utf8(&sb, cp);
			p += 6;
		} else if (p[0] == '%' && p[1] == 'U' &&
		           hexdigits((const char *)p + 2, 6, &cp) == 0 && cp <= 0x10FFFF) {
			put_utf8(&sb, cp);
			p += 8;
		} else {
			put_utf8(&sb, byte_to_cp(cs, *p));
			p++;
		}
	}
	return sb_steal(&sb);
}

enum { TO_TEXT, TO_NAME, TO_MESSAGE };

static char *to_local(int cs, const char *utf8, int mode)
{
	const unsigned char *p = (const unsigned char *)utf8;
	strbuf sb;

	sb_init(&sb);
	while (*p != '\0') {
		unsigned cp;
		int n, b;
		char esc[12];

		if (p[0] < 0x80) {
			cp = p[0];
			n = 1;
		} else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
			cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
			n = 2;
		} else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
			cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
			n = 3;
		} else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
		           (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
			cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
			     ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
			n = 4;
		} else {
			/* not UTF-8: take the byte as it is */
			cp = p[0];
			n = 1;
		}
		p += n;
		if (cp == '%' && (p[0] == 'u' || p[0] == 'U')) {
			/* a real "%u" must not be read back as an escape */
			sb_puts(&sb, "%u0025");
			continue;
		}
		b = cp < 0x80 && cp >= 0x20 && cp != 0x7F ? (int)cp : cp_to_byte(cs, cp);
		/* AmigaDOS reads ':' in a name as the end of a device name */
		if (mode == TO_NAME && cp == ':')
			b = -1;
		/* requesters show line breaks */
		if (mode == TO_MESSAGE && (cp == '\n' || cp == '\t'))
			b = (int)cp;
		if (b >= 0) {
			char c = (char)b;
			sb_append(&sb, &c, 1);
		} else if (cp <= 0xFFFF) {
			snprintf(esc, sizeof(esc), "%%u%04X", cp);
			sb_puts(&sb, esc);
		} else {
			snprintf(esc, sizeof(esc), "%%U%06X", cp);
			sb_puts(&sb, esc);
		}
	}
	return sb_steal(&sb);
}

char *cs_utf8_to_local(int cs, const char *utf8)
{
	return to_local(cs, utf8, TO_TEXT);
}

char *cs_utf8_to_name(int cs, const char *utf8)
{
	return to_local(cs, utf8, TO_NAME);
}

char *cs_utf8_to_message(int cs, const char *utf8)
{
	return to_local(cs, utf8, TO_MESSAGE);
}
