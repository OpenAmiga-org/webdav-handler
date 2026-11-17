/* nextcloud-handler - the server list (XML). Public domain. */
#include "servers.h"
#include "util.h"

#include <errno.h>
#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- server list ---------------------------------------------------------- */

void servers_init(server_list *l)
{
	memset(l, 0, sizeof(*l));
}

void servers_free(server_list *l)
{
	if (l->server != NULL)
		memset(l->server, 0, (size_t)l->cap * sizeof(*l->server));   /* passwords */
	free(l->server);
	memset(l, 0, sizeof(*l));
}

int servers_add(server_list *l, const server_cfg *s)
{
	if (l->count == l->cap) {
		int ncap = l->cap ? l->cap * 2 : 8;
		server_cfg *n = calloc((size_t)ncap, sizeof(*n));

		if (n == NULL)
			return -1;
		if (l->server != NULL) {
			memcpy(n, l->server, (size_t)l->count * sizeof(*n));
			memset(l->server, 0, (size_t)l->cap * sizeof(*n));
			free(l->server);
		}
		l->server = n;
		l->cap = ncap;
	}
	l->server[l->count] = *s;
	return l->count++;
}

void servers_remove(server_list *l, int index)
{
	if (index < 0 || index >= l->count)
		return;
	memmove(&l->server[index], &l->server[index + 1],
	        (size_t)(l->count - index - 1) * sizeof(*l->server));
	l->count--;
	memset(&l->server[l->count], 0, sizeof(*l->server));
}

int servers_find(const server_list *l, const char *name)
{
	int i;

	for (i = 0; i < l->count; i++)
		if (name_casecmp(l->server[i].name, name) == 0)
			return i;
	return -1;
}

int server_cfg_same_account(const server_cfg *a, const server_cfg *b)
{
	return name_casecmp(a->name, b->name) == 0 && strcmp(a->url, b->url) == 0 &&
	       strcmp(a->user, b->user) == 0 && strcmp(a->cafile, b->cafile) == 0 &&
	       a->insecure == b->insecure;
}

int server_cfg_equal(const server_cfg *a, const server_cfg *b)
{
	return server_cfg_same_account(a, b) && strcmp(a->name, b->name) == 0 &&
	       strcmp(a->password, b->password) == 0;
}

int servers_equal(const server_list *a, const server_list *b)
{
	int i;

	if (a->count != b->count)
		return 0;
	for (i = 0; i < a->count; i++)
		if (!server_cfg_equal(&a->server[i], &b->server[i]))
			return 0;
	return 1;
}

/* --- names ---------------------------------------------------------------- */

const char *server_name_problem(const char *name)
{
	const unsigned char *p;
	size_t n = strlen(name);

	if (n == 0)
		return "The name is empty.";
	if (n > SERVER_NAME_MAX)
		return "The name is too long (107 characters at most).";
	for (p = (const unsigned char *)name; *p != '\0'; p++) {
		if (*p == ':' || *p == '/')
			return "A name cannot contain ':' or '/'.";
		if (*p < 32 || (*p >= 127 && *p < 160))
			return "The name contains a control character.";
	}
	if (name[0] == ' ' || name[n - 1] == ' ')
		return "A name cannot start or end with a space.";
	if (n >= 5 && strcasecmp(name + n - 5, ".info") == 0)
		return "A name cannot end in \".info\" (that is used for the icons).";
	return NULL;
}

/* checks the UTF-8 form of a name read from the file */
static int utf8_name_ok(const char *name)
{
	const unsigned char *p;
	size_t n = strlen(name);

	if (n == 0 || n > sizeof(((server_cfg *)0)->name) - 1)
		return 0;
	for (p = (const unsigned char *)name; *p != '\0'; p++)
		if (*p == ':' || *p == '/' || *p < 32 || *p == 127)
			return 0;
	if (name[0] == ' ' || name[n - 1] == ' ')
		return 0;
	if (n >= 5 && strcasecmp(name + n - 5, ".info") == 0)
		return 0;
	return 1;
}

/* --- reading ---------------------------------------------------------------- */

struct parse {
	XML_Parser   xp;
	int          depth;
	int          root_ok;
	server_list *servers;
	int          skipped;
	int          failed;
};

static void copy_attr(char *dst, size_t size, const char *val, int *bad)
{
	if (strlen(val) >= size)
		*bad = 1;
	snprintf(dst, size, "%s", val);
}

static void server_element(struct parse *ps, const XML_Char **attr)
{
	server_cfg s;
	int i, bad = 0;

	memset(&s, 0, sizeof(s));
	for (i = 0; attr[i] != NULL; i += 2) {
		const char *k = attr[i], *v = attr[i + 1];

		if (strcmp(k, "name") == 0)
			copy_attr(s.name, sizeof(s.name), v, &bad);
		else if (strcmp(k, "url") == 0)
			copy_attr(s.url, sizeof(s.url), v, &bad);
		else if (strcmp(k, "user") == 0)
			copy_attr(s.user, sizeof(s.user), v, &bad);
		else if (strcmp(k, "password") == 0)
			copy_attr(s.password, sizeof(s.password), v, &bad);
		else if (strcmp(k, "cafile") == 0)
			copy_attr(s.cafile, sizeof(s.cafile), v, &bad);
		else if (strcmp(k, "insecure") == 0)
			s.insecure = atoi(v) != 0;
	}
	if (bad || !utf8_name_ok(s.name) || servers_find(ps->servers, s.name) >= 0) {
		ps->skipped++;
		memset(&s, 0, sizeof(s));
		return;
	}
	if (servers_add(ps->servers, &s) < 0)
		ps->failed = 1;
	memset(&s, 0, sizeof(s));
}

static void XMLCALL on_start(void *ud, const XML_Char *el, const XML_Char **attr)
{
	struct parse *ps = ud;

	ps->depth++;
	if (ps->depth == 1) {
		ps->root_ok = strcmp(el, "nextcloudhandler") == 0;
		return;
	}
	if (ps->depth == 2 && ps->root_ok && strcmp(el, "server") == 0)
		server_element(ps, attr);
}

static void XMLCALL on_end(void *ud, const XML_Char *el)
{
	struct parse *ps = ud;

	(void)el;
	ps->depth--;
}

static char *read_file(const char *file, size_t *len, int *err)
{
	FILE *f = fopen(file, "rb");
	char *buf = NULL;
	size_t cap = 0, n = 0, got;

	if (f == NULL) {
		*err = -ENOENT;
		return NULL;
	}
	do {
		if (n + 4096 > cap) {
			char *nb = realloc(buf, cap + 16384);

			if (nb == NULL) {
				free(buf);
				fclose(f);
				*err = -ENOMEM;
				return NULL;
			}
			buf = nb;
			cap += 16384;
		}
		got = fread(buf + n, 1, cap - n, f);
		n += got;
	} while (got > 0);
	fclose(f);
	*len = n;
	*err = 0;
	return buf;
}

static int parse_file(const char *file, struct parse *ps, char *err, size_t errlen)
{
	size_t len = 0;
	int rc;
	char *buf = read_file(file, &len, &rc);

	if (buf == NULL)
		return rc;
	ps->xp = XML_ParserCreate(NULL);
	if (ps->xp == NULL) {
		free(buf);
		return -ENOMEM;
	}
	XML_SetUserData(ps->xp, ps);
	XML_SetElementHandler(ps->xp, on_start, on_end);
	rc = 0;
	if (XML_Parse(ps->xp, buf, (int)len, 1) == XML_STATUS_ERROR) {
		if (err != NULL)
			snprintf(err, errlen, "%s: line %lu: %s", file,
			         (unsigned long)XML_GetCurrentLineNumber(ps->xp),
			         XML_ErrorString(XML_GetErrorCode(ps->xp)));
		rc = -EINVAL;
	} else if (!ps->root_ok) {
		if (err != NULL)
			snprintf(err, errlen, "%s is not a nextcloud-handler file.", file);
		rc = -EINVAL;
	} else if (ps->failed) {
		rc = -ENOMEM;
	}
	XML_ParserFree(ps->xp);
	memset(buf, 0, len);   /* passwords */
	free(buf);
	return rc;
}

int servers_load(const char *file, server_list *l, char *err, size_t errlen, int *skipped)
{
	struct parse ps;
	int rc;

	memset(&ps, 0, sizeof(ps));
	ps.servers = l;
	l->count = 0;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	rc = parse_file(file, &ps, err, errlen);
	if (skipped != NULL)
		*skipped = ps.skipped;
	return rc;
}

/* --- writing ---------------------------------------------------------------- */

static void put_escaped(FILE *f, const char *s)
{
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '&':  fputs("&amp;", f); break;
		case '<':  fputs("&lt;", f); break;
		case '>':  fputs("&gt;", f); break;
		case '"':  fputs("&quot;", f); break;
		case '\t': fputs("&#9;", f); break;
		case '\n': fputs("&#10;", f); break;
		case '\r': fputs("&#13;", f); break;
		default:
			if ((unsigned char)*s >= 32)
				fputc(*s, f);
			break;
		}
	}
}

static void put_attr(FILE *f, const char *name, const char *val)
{
	fprintf(f, " %s=\"", name);
	put_escaped(f, val);
	fputc('"', f);
}

/* copies src over dst (for file systems without rename, such as ENV:) */
static int copy_over(const char *src, const char *dst)
{
	char buf[4096];
	FILE *in = fopen(src, "rb"), *out;
	size_t n;
	int bad = 0;

	if (in == NULL)
		return -EIO;
	out = fopen(dst, "wb");
	if (out == NULL) {
		fclose(in);
		return -(errno != 0 ? errno : EIO);
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, n, out) != n)
			bad = 1;
	memset(buf, 0, sizeof(buf));
	fclose(in);
	if (fclose(out) != 0 || bad)
		return -EIO;
	return 0;
}

int servers_save(const char *file, const server_list *l)
{
	char tmp[600];
	FILE *f;
	int i, bad, rc;

	/* written to file.new, then put in place (AmigaDOS cannot rename over
	 * an existing file, and ENV: cannot rename at all) */
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	f = fopen(tmp, "wb");
	if (f == NULL)
		return -(errno != 0 ? errno : EIO);
	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<nextcloudhandler version=\"1\">\n", f);
	for (i = 0; i < l->count; i++) {
		const server_cfg *s = &l->server[i];

		fputs("  <server", f);
		put_attr(f, "name", s->name);
		put_attr(f, "url", s->url);
		put_attr(f, "user", s->user);
		put_attr(f, "password", s->password);
		put_attr(f, "cafile", s->cafile);
		put_attr(f, "insecure", s->insecure ? "1" : "0");
		fputs("/>\n", f);
	}
	fputs("</nextcloudhandler>\n", f);
	bad = ferror(f);
	if (fclose(f) != 0 || bad) {
		remove(tmp);
		return -EIO;
	}
	remove(file);
	if (rename(tmp, file) == 0)
		return 0;
	rc = copy_over(tmp, file);
	remove(tmp);
	return rc;
}
