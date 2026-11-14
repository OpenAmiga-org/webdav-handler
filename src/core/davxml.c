/* nextcloud-handler - WebDAV multistatus (PROPFIND) parser. Public domain. */
#include "davxml.h"
#include "util.h"

#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_SEP '|'

enum {
	P_NONE,
	P_LENGTH,
	P_MODIFIED,
	P_ETAG,
	P_RESTYPE,
	P_COMMENT,
	P_PERMS,
	P_QUSED,
	P_QAVAIL,
	P_PRINCIPAL
};

struct ms_parser {
	XML_Parser xp;
	int        failed;
	char       errmsg[128];

	dav_resp  *items;
	size_t     count;
	size_t     cap;

	/* state */
	int        depth;
	int        in_response;  /* depth of <response>, 0 if outside */
	int        in_propstat;
	int        in_prop;
	int        prop;         /* P_* of the property being read */
	int        prop_depth;
	strbuf     text;

	/* properties of the current propstat until its status is known */
	dav_resp   ps;
	int        ps_status;
};

static void resp_free(dav_resp *r)
{
	free(r->href);
	free(r->etag);
	free(r->comment);
	free(r->permissions);
	free(r->principal);
}

static void resp_reset(dav_resp *r)
{
	memset(r, 0, sizeof(*r));
	r->size = -1;
	r->quota_used = -1;
	r->quota_avail = -1;
}

static int is_dav(const char *name, const char *local)
{
	return strncmp(name, "DAV:|", 5) == 0 && strcmp(name + 5, local) == 0;
}

static int parse_status(const char *s)
{
	/* "HTTP/1.1 200 OK" */
	const char *sp = strchr(s, ' ');

	return sp != NULL ? atoi(sp + 1) : 0;
}

static void set_str(char **dst, const char *src)
{
	free(*dst);
	*dst = xstrdup(src);
}

static void XMLCALL on_start(void *ud, const XML_Char *name, const XML_Char **atts)
{
	ms_parser *p = ud;
	dav_resp *r;

	(void)atts;
	p->depth++;
	sb_clear(&p->text);

	if (is_dav(name, "response") && p->in_response == 0) {
		if (p->count == p->cap) {
			size_t ncap = p->cap ? p->cap * 2 : 32;
			dav_resp *n = realloc(p->items, ncap * sizeof(*n));
			if (n == NULL) {
				p->failed = 1;
				XML_StopParser(p->xp, XML_FALSE);
				return;
			}
			p->items = n;
			p->cap = ncap;
		}
		r = &p->items[p->count++];
		resp_reset(r);
		p->in_response = p->depth;
		return;
	}
	if (p->in_response == 0)
		return;
	if (is_dav(name, "propstat")) {
		p->in_propstat = p->depth;
		resp_reset(&p->ps);
		p->ps_status = 0;
		return;
	}
	if (p->in_propstat && is_dav(name, "prop") && p->in_prop == 0) {
		p->in_prop = p->depth;
		return;
	}
	if (p->in_prop && p->depth == p->in_prop + 1) {
		p->prop_depth = p->depth;
		if (is_dav(name, "getcontentlength"))
			p->prop = P_LENGTH;
		else if (is_dav(name, "getlastmodified"))
			p->prop = P_MODIFIED;
		else if (is_dav(name, "getetag"))
			p->prop = P_ETAG;
		else if (is_dav(name, "resourcetype"))
			p->prop = P_RESTYPE;
		else if (is_dav(name, "quota-used-bytes"))
			p->prop = P_QUSED;
		else if (is_dav(name, "quota-available-bytes"))
			p->prop = P_QAVAIL;
		else if (is_dav(name, "current-user-principal"))
			p->prop = P_PRINCIPAL;
		else if (strcmp(name, "urn:amiga:|comment") == 0)
			p->prop = P_COMMENT;
		else if (strcmp(name, "http://owncloud.org/ns|permissions") == 0)
			p->prop = P_PERMS;
		else
			p->prop = P_NONE;
		return;
	}
	if (p->prop == P_RESTYPE && is_dav(name, "collection"))
		p->ps.is_dir = 1;
}

static void XMLCALL on_end(void *ud, const XML_Char *name)
{
	ms_parser *p = ud;
	dav_resp *r = p->count > 0 ? &p->items[p->count - 1] : NULL;
	const char *t = p->text.data != NULL ? p->text.data : "";

	if (p->in_response == 0 || r == NULL) {
		p->depth--;
		return;
	}

	if (p->prop != P_NONE && p->depth == p->prop_depth) {
		switch (p->prop) {
		case P_LENGTH:
			p->ps.size = strtoll(t, NULL, 10);
			break;
		case P_MODIFIED:
			parse_http_date(t, &p->ps.mtime);
			break;
		case P_ETAG:
			set_str(&p->ps.etag, t);
			break;
		case P_COMMENT:
			set_str(&p->ps.comment, t);
			break;
		case P_PERMS:
			set_str(&p->ps.permissions, t);
			break;
		case P_QUSED:
			p->ps.quota_used = strtoll(t, NULL, 10);
			break;
		case P_QAVAIL:
			p->ps.quota_avail = strtoll(t, NULL, 10);
			break;
		default:
			break;
		}
		p->prop = P_NONE;
	} else if (p->prop == P_PRINCIPAL && is_dav(name, "href")) {
		set_str(&p->ps.principal, t);
	} else if (p->in_propstat && p->depth == p->in_propstat + 1 &&
	           is_dav(name, "status")) {
		p->ps_status = parse_status(t);
	} else if (p->depth == p->in_prop) {
		p->in_prop = 0;
	} else if (p->depth == p->in_propstat) {
		if (p->ps_status >= 200 && p->ps_status < 300) { /* 204 after a remove */
			/* merge the found properties into the response */
			r->found = 1;
			if (p->ps.is_dir)
				r->is_dir = 1;
			if (p->ps.size >= 0)
				r->size = p->ps.size;
			if (p->ps.mtime != 0)
				r->mtime = p->ps.mtime;
			if (p->ps.quota_used >= 0)
				r->quota_used = p->ps.quota_used;
			if (p->ps.quota_avail != -1)
				r->quota_avail = p->ps.quota_avail;
#define TAKE(f) if (p->ps.f != NULL) { free(r->f); r->f = p->ps.f; p->ps.f = NULL; }
			TAKE(etag)
			TAKE(comment)
			TAKE(permissions)
			TAKE(principal)
#undef TAKE
		}
		resp_free(&p->ps);
		resp_reset(&p->ps);
		p->in_propstat = 0;
	} else if (p->depth == p->in_response + 1 && is_dav(name, "href")) {
		set_str(&r->href, t);
	} else if (p->depth == p->in_response + 1 && is_dav(name, "status")) {
		r->status = parse_status(t);
	} else if (p->depth == p->in_response) {
		p->in_response = 0;
	}
	p->depth--;
}

static void XMLCALL on_text(void *ud, const XML_Char *s, int len)
{
	ms_parser *p = ud;

	if (sb_append(&p->text, s, (size_t)len) != 0) {
		p->failed = 1;
		XML_StopParser(p->xp, XML_FALSE);
	}
}

ms_parser *ms_new(void)
{
	ms_parser *p = calloc(1, sizeof(*p));

	if (p == NULL)
		return NULL;
	p->xp = XML_ParserCreateNS(NULL, NS_SEP);
	if (p->xp == NULL) {
		free(p);
		return NULL;
	}
	sb_init(&p->text);
	resp_reset(&p->ps);
	XML_SetUserData(p->xp, p);
	XML_SetElementHandler(p->xp, on_start, on_end);
	XML_SetCharacterDataHandler(p->xp, on_text);
	return p;
}

int ms_feed(ms_parser *p, const char *data, size_t len, int final)
{
	if (p->failed)
		return -1;
	if (XML_Parse(p->xp, data, (int)len, final) == XML_STATUS_ERROR) {
		p->failed = 1;
		snprintf(p->errmsg, sizeof(p->errmsg), "XML error at line %lu: %s",
		         (unsigned long)XML_GetCurrentLineNumber(p->xp),
		         XML_ErrorString(XML_GetErrorCode(p->xp)));
		return -1;
	}
	return p->failed ? -1 : 0;
}

dav_resp *ms_results(ms_parser *p, size_t *count)
{
	*count = p->count;
	return p->items;
}

const char *ms_error(ms_parser *p)
{
	if (p->errmsg[0] != '\0')
		return p->errmsg;
	return p->failed ? "out of memory while parsing XML" : "";
}

void ms_free(ms_parser *p)
{
	size_t i;

	if (p == NULL)
		return;
	for (i = 0; i < p->count; i++)
		resp_free(&p->items[i]);
	free(p->items);
	resp_free(&p->ps);
	sb_free(&p->text);
	XML_ParserFree(p->xp);
	free(p);
}
