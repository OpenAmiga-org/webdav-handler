/* webdav-handler - WebDAV client on top of libcurl. Public domain. */
#include "davclient.h"
#include "davxml.h"
#include "util.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ERRBODY_MAX 4096

struct dav_session {
	CURL       *curl;
	char       *url;          /* as configured */
	char       *user;
	char       *password;
	char       *ca_file;
	char       *ca_path;
	char       *proxy;
	char       *user_agent;
	int         insecure;
	long        connect_timeout;
	long        io_timeout;

	char       *origin;       /* "https://host:port" */
	char       *root_path;    /* percent-encoded, no trailing '/' ("" = server root) */
	char       *root_dec;     /* decoded root_path */
	char       *root_url;     /* origin + root_path + "/" */
	int         nextcloud;
	int         kind;         /* DAV_KIND_* asked for */
	char       *uploads_url;  /* Nextcloud chunked upload folder, or NULL */
	struct curl_slist *resolve; /* host:port:address for curl */
	int64_t     resolved_at;
	int64_t     chunk_size;
	unsigned    upload_seq;

	long        last_status;  /* HTTP status of the last failed request */
	int         last_kind;    /* DAV_FAIL_* of the last failed request */

	char        curlerr[CURL_ERROR_SIZE];
	char        errmsg[512];
};

/* One HTTP request and where its response goes. */
typedef struct {
	const char        *method;
	const char        *path;        /* relative path, encoded by us */
	const char        *url;         /* full URL instead of path */
	int                dir;         /* append '/' to the URL */
	struct curl_slist *headers;
	const char        *body;
	size_t             body_len;
	tmpf              *upload;
	int64_t            upload_base; /* offset of the part to send */
	int64_t            upload_size;
	int64_t            upload_pos;
	long               timeout;     /* overrides io_timeout */

	ms_parser         *xml;         /* 207 bodies */
	char              *buf;         /* range reads */
	size_t             buf_len;
	size_t             got;
	int64_t            range_off;   /* offset asked for with Range */
	int64_t            skip;        /* bytes to drop when Range was ignored */
	int                skip_checked;
	tmpf              *download;
	int64_t            dl_pos;

	long               status;
	char               etag[160];   /* ETag header of the response */
	int                sink_failed;
	strbuf             errbody;
} dav_req;

static int curl_ready;

static char *dupnull(const char *s)
{
	return s != NULL && s[0] != '\0' ? xstrdup(s) : NULL;
}

dav_session *dav_new(const dav_config *cfg)
{
	dav_session *s;

	if (!curl_ready) {
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
			return NULL;
		curl_ready = 1;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->curl = curl_easy_init();
	s->url = dupnull(cfg->url);
	s->user = dupnull(cfg->user);
	s->password = dupnull(cfg->password);
	s->ca_file = dupnull(cfg->ca_file);
	s->ca_path = dupnull(cfg->ca_path);
	s->proxy = dupnull(cfg->proxy);
	s->user_agent = dupnull(cfg->user_agent);
	s->insecure = cfg->insecure;
	s->kind = cfg->kind;
	s->connect_timeout = cfg->connect_timeout > 0 ? cfg->connect_timeout : 20;
	s->io_timeout = cfg->io_timeout > 0 ? cfg->io_timeout : 60;
	s->chunk_size = cfg->chunk_size > 0 ? cfg->chunk_size : (int64_t)16 * 1024 * 1024;
	if (s->chunk_size < (int64_t)5 * 1024 * 1024)
		s->chunk_size = (int64_t)5 * 1024 * 1024; /* Nextcloud minimum */
	if (s->curl == NULL || s->url == NULL) {
		dav_free(s);
		return NULL;
	}
	return s;
}

void dav_free(dav_session *s)
{
	if (s == NULL)
		return;
	if (s->curl != NULL)
		curl_easy_cleanup(s->curl);
	free(s->url);
	free(s->user);
	free(s->password);
	free(s->ca_file);
	free(s->ca_path);
	free(s->proxy);
	free(s->user_agent);
	free(s->origin);
	free(s->root_path);
	free(s->root_dec);
	free(s->root_url);
	free(s->uploads_url);
	curl_slist_free_all(s->resolve);
	free(s);
}

const char *dav_error(dav_session *s)
{
	return s->errmsg;
}

int dav_is_nextcloud(dav_session *s)
{
	return s->nextcloud;
}

int dav_last_kind(dav_session *s)
{
	return s->last_kind;
}

long dav_last_status(dav_session *s)
{
	return s->last_status;
}

int dav_set_password(dav_session *s, const char *password)
{
	free(s->password);
	s->password = dupnull(password);
	return 0;
}

const char *dav_root_url(dav_session *s)
{
	return s->root_url != NULL ? s->root_url : s->url;
}

/* --- error handling -------------------------------------------------- */

static const char *status_text(long code)
{
	switch (code) {
	case 400: return "Bad Request";
	case 401: return "Unauthorized (wrong user name or password)";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict (parent folder missing)";
	case 412: return "Precondition Failed (target exists)";
	case 413: return "Request Entity Too Large";
	case 415: return "Unsupported Media Type";
	case 423: return "Locked";
	case 500: return "Internal Server Error";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Gateway Timeout";
	case 507: return "Insufficient Storage";
	default:  return "";
	}
}

/* Pulls <s:message> out of a sabre/Nextcloud error body. */
static void server_message(const char *body, char *out, size_t outlen)
{
	const char *a, *b;

	out[0] = '\0';
	if (body == NULL)
		return;
	a = strstr(body, "<s:message>");
	if (a == NULL)
		return;
	a += 11;
	b = strstr(a, "</s:message>");
	if (b == NULL || b == a)
		return;
	snprintf(out, outlen, ": %.*s", (int)(b - a), a);
}

static int http_errno(long code)
{
	if (code >= 200 && code < 300)
		return 0;
	switch (code) {
	case 401:
	case 403: return -EACCES;
	case 404:
	case 409: return -ENOENT;
	case 405:
	case 412: return -EEXIST;
	case 413: return -EFBIG;
	case 423: return -EBUSY;
	case 507: return -ENOSPC;
	case 501: return -EOPNOTSUPP;
	default:  return -EIO;
	}
}

static int set_http_error(dav_session *s, dav_req *r)
{
	char msg[200];

	s->last_status = r->status;
	s->last_kind = DAV_FAIL_HTTP;
	server_message(r->errbody.data, msg, sizeof(msg));
	snprintf(s->errmsg, sizeof(s->errmsg), "%s %s: HTTP %ld %s%s", r->method,
	         r->path != NULL ? r->path : (r->url != NULL ? r->url : ""),
	         r->status, status_text(r->status), msg);
	plat_log(LOG_INFO, "%s\n", s->errmsg);
	return http_errno(r->status);
}

/* --- curl callbacks -------------------------------------------------- */

static size_t on_write(char *data, size_t size, size_t n, void *ud)
{
	dav_req *r = ud;
	size_t len = size * n, take, used = 0;

	if (r->status < 200 || r->status >= 300) {
		if (r->errbody.len < ERRBODY_MAX)
			sb_append(&r->errbody, data, len);
		return len;
	}
	if (r->xml != NULL) {
		if (ms_feed(r->xml, data, len, 0) != 0) {
			r->sink_failed = 1;
			return 0;
		}
		return len;
	}
	if (r->buf != NULL) {
		if (!r->skip_checked) {
			/* 200 instead of 206: the server ignored Range */
			r->skip_checked = 1;
			if (r->status == 200)
				r->skip = r->range_off;
		}
		if (r->skip > 0) {
			take = (int64_t)len < r->skip ? len : (size_t)r->skip;
			r->skip -= (int64_t)take;
			used = take;
		}
		take = len - used;
		if (take > r->buf_len - r->got)
			take = r->buf_len - r->got;
		memcpy(r->buf + r->got, data + used, take);
		r->got += take;
		if (r->got == r->buf_len && used + take < len)
			return 0; /* buffer full: stop the transfer */
		return len;
	}
	if (r->download != NULL) {
		if (tmpf_pwrite(r->download, data, len, r->dl_pos) != (int)len) {
			r->sink_failed = 1;
			return 0;
		}
		r->dl_pos += (int64_t)len;
		return len;
	}
	return len;
}

/* Records the status line before any body data is passed on. */
static size_t on_header(char *data, size_t size, size_t n, void *ud)
{
	dav_req *r = ud;
	size_t len = size * n;

	if (len > 9 && strncmp(data, "HTTP/", 5) == 0) {
		const char *sp = memchr(data, ' ', len);
		if (sp != NULL)
			r->status = strtol(sp + 1, NULL, 10);
		r->etag[0] = '\0';
	} else if (len > 5 && strncasecmp(data, "ETag:", 5) == 0) {
		size_t i = 5, k = 0;
		while (i < len && (data[i] == ' ' || data[i] == '\t'))
			i++;
		while (i < len && data[i] != '\r' && data[i] != '\n' && k + 1 < sizeof(r->etag))
			r->etag[k++] = data[i++];
		r->etag[k] = '\0';
	}
	return len;
}

static size_t on_read(char *buf, size_t size, size_t n, void *ud)
{
	dav_req *r = ud;
	size_t want = size * n;
	int got;

	if (r->upload_pos >= r->upload_size)
		return 0;
	if ((int64_t)want > r->upload_size - r->upload_pos)
		want = (size_t)(r->upload_size - r->upload_pos);
	got = tmpf_pread(r->upload, buf, want, r->upload_base + r->upload_pos);
	if (got < 0)
		return CURL_READFUNC_ABORT;
	r->upload_pos += got;
	return (size_t)got;
}

static int on_seek(void *ud, curl_off_t off, int origin)
{
	dav_req *r = ud;

	if (origin != SEEK_SET || off < 0 || off > r->upload_size)
		return CURL_SEEKFUNC_FAIL;
	r->upload_pos = off;
	return CURL_SEEKFUNC_OK;
}

/* --- name lookup ----------------------------------------------------- */

#define RESOLVE_MS (10 * 60 * 1000)

static int is_numeric_host(const char *h)
{
	if (h[0] == '[')
		return 1; /* IPv6 literal */
	for (; *h != '\0'; h++) {
		if ((*h < '0' || *h > '9') && *h != '.')
			return 0;
	}
	return 1;
}

/* Adds "host:port:address" for the host of url ("scheme://host[:port]..."). */
static void add_resolve(dav_session *s, const char *url)
{
	const char *p = strstr(url, "://"), *h, *e;
	char host[256], ip[64], entry[400];
	long port;

	if (p == NULL)
		return;
	h = p + 3;
	if (strchr(h, '@') != NULL && strchr(h, '@') < (strchr(h, '/') ? strchr(h, '/') : h + strlen(h)))
		h = strchr(h, '@') + 1;
	e = h;
	while (*e != '\0' && *e != ':' && *e != '/')
		e++;
	if ((size_t)(e - h) >= sizeof(host) || e == h)
		return;
	memcpy(host, h, (size_t)(e - h));
	host[e - h] = '\0';
	port = strncmp(url, "https", 5) == 0 ? 443 : 80;
	if (*e == ':')
		port = strtol(e + 1, NULL, 10);
	if (is_numeric_host(host))
		return;
	if (plat_resolve(host, ip, sizeof(ip)) != 0)
		return;
	snprintf(entry, sizeof(entry), "%s:%ld:%s", host, port, ip);
	s->resolve = curl_slist_append(s->resolve, entry);
	plat_log(LOG_DEBUG, "resolved %s\n", entry);
}

static void refresh_resolve(dav_session *s, int force)
{
	if (!force && s->resolved_at != 0 && plat_ms() - s->resolved_at < RESOLVE_MS)
		return;
	curl_slist_free_all(s->resolve);
	s->resolve = NULL;
	add_resolve(s, s->url);
	if (s->proxy != NULL) {
		char purl[300];
		snprintf(purl, sizeof(purl), "%s%s", strstr(s->proxy, "://") ? "" : "http://", s->proxy);
		add_resolve(s, purl);
	}
	s->resolved_at = plat_ms();
}

/* --- request --------------------------------------------------------- */

static char *make_url(dav_session *s, const char *path, int dir)
{
	strbuf sb;

	sb_init(&sb);
	sb_puts(&sb, s->origin);
	sb_puts(&sb, s->root_path);
	if (path == NULL || strcmp(path, "/") == 0) {
		sb_puts(&sb, "/");
	} else {
		url_encode_path(&sb, path);
		if (dir)
			sb_puts(&sb, "/");
	}
	return sb_steal(&sb);
}

static long xfer_calls;

static int on_xferinfo(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
	(void)ud; (void)dt; (void)dn; (void)ut; (void)un;
	xfer_calls++;
	return 0;
}

/* Runs the request. Returns 0 when an HTTP response arrived (any status,
 * see r->status) or -errno when the transfer failed. */
static int perform(dav_session *s, dav_req *r)
{
	CURL *c = s->curl;
	CURLcode rc;
	char *url;
	long timeout = r->timeout > 0 ? r->timeout : s->io_timeout;

	url = r->url != NULL ? xstrdup(r->url) : make_url(s, r->path, r->dir);
	if (url == NULL)
		return -ENOMEM;

	curl_easy_reset(c);
	s->last_status = 0;
	s->last_kind = DAV_FAIL_NONE;
	refresh_resolve(s, 0);
	s->curlerr[0] = '\0';
	if (s->resolve != NULL)
		curl_easy_setopt(c, CURLOPT_RESOLVE, s->resolve);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, s->curlerr);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, r->method);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, s->connect_timeout);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, timeout);
	curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
	/* big buffers: on AmigaOS every wait for the socket is expensive */
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 512L * 1024);
	curl_easy_setopt(c, CURLOPT_UPLOAD_BUFFERSIZE, 1024L * 1024);
	if (s->user_agent != NULL)
		curl_easy_setopt(c, CURLOPT_USERAGENT, s->user_agent);
	if (s->user != NULL) {
		curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
		curl_easy_setopt(c, CURLOPT_USERNAME, s->user);
		curl_easy_setopt(c, CURLOPT_PASSWORD, s->password != NULL ? s->password : "");
	}
	if (s->insecure) {
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (s->ca_file != NULL)
		curl_easy_setopt(c, CURLOPT_CAINFO, s->ca_file);
	if (s->ca_path != NULL)
		curl_easy_setopt(c, CURLOPT_CAPATH, s->ca_path);
	if (s->proxy != NULL)
		curl_easy_setopt(c, CURLOPT_PROXY, s->proxy);
	if (r->headers != NULL)
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, r->headers);
	if (strcmp(r->method, "HEAD") == 0)
		curl_easy_setopt(c, CURLOPT_NOBODY, 1L);

	if (r->upload != NULL) {
		curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(c, CURLOPT_READFUNCTION, on_read);
		curl_easy_setopt(c, CURLOPT_READDATA, r);
		curl_easy_setopt(c, CURLOPT_SEEKFUNCTION, on_seek);
		curl_easy_setopt(c, CURLOPT_SEEKDATA, r);
		curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)r->upload_size);
	} else if (r->body != NULL) {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
	}

	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, r);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, r);

	plat_log(LOG_DEBUG, "%s %s\n", r->method, url);
	if (plat_loglevel >= LOG_DEBUG) {
		xfer_calls = 0;
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_xferinfo);
	}
	r->status = 0;
	rc = curl_easy_perform(c);
	free(url);

	if (rc == CURLE_WRITE_ERROR && r->buf != NULL && !r->sink_failed &&
	    r->got == r->buf_len)
		rc = CURLE_OK; /* we stopped a range read on purpose */

	if (rc != CURLE_OK) {
		snprintf(s->errmsg, sizeof(s->errmsg), "%s %s: %s", r->method,
		         r->path != NULL ? r->path : "",
		         s->curlerr[0] != '\0' ? s->curlerr : curl_easy_strerror(rc));
		plat_log(LOG_INFO, "%s\n", s->errmsg);
		s->last_kind = DAV_FAIL_OTHER;
		if (r->sink_failed)
			return -EIO;
		switch (rc) {
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_COULDNT_CONNECT:
			s->resolved_at = 0; /* look the name up again next time */
			s->last_kind = DAV_FAIL_NETWORK;
			return -ENODEV;   /* AmigaDOS shows "no disk" with Retry */
		case CURLE_OPERATION_TIMEDOUT:
		case CURLE_SEND_ERROR:
		case CURLE_RECV_ERROR:
		case CURLE_GOT_NOTHING:
			s->last_kind = DAV_FAIL_NETWORK;
			return -ENODEV;   /* AmigaDOS shows "no disk" with Retry */
		case CURLE_PEER_FAILED_VERIFICATION:
		case CURLE_SSL_CONNECT_ERROR:
		case CURLE_SSL_CACERT_BADFILE:
		case CURLE_SSL_CERTPROBLEM:
			s->last_kind = DAV_FAIL_TLS;
			return -EACCES;
		case CURLE_OUT_OF_MEMORY:
			return -ENOMEM;
		default:
			return -EIO;
		}
	}
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->status);
	if (plat_loglevel >= LOG_DEBUG) {
		curl_off_t down = 0, up = 0;
		double secs = 0;
		curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &down);
		curl_easy_getinfo(c, CURLINFO_SIZE_UPLOAD_T, &up);
		curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &secs);
		plat_log(LOG_DEBUG, "  -> %ld, %lld down, %lld up, %d ms, %ld loops\n", r->status,
		         (long long)down, (long long)up, (int)(secs * 1000), xfer_calls);
	}
	return 0;
}

static void req_init(dav_req *r, const char *method, const char *path)
{
	memset(r, 0, sizeof(*r));
	r->method = method;
	r->path = path;
	sb_init(&r->errbody);
}

static void req_done(dav_req *r)
{
	curl_slist_free_all(r->headers);
	sb_free(&r->errbody);
}

/* --- PROPFIND -------------------------------------------------------- */

static const char propfind_body[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" xmlns:a=\"urn:amiga:\">"
	"<d:prop>"
	"<d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:getetag/>"
	"<d:quota-used-bytes/><d:quota-available-bytes/>"
	"<oc:permissions/><a:comment/>"
	"</d:prop></d:propfind>\n";

static const char principal_body[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>\n";

/* Turns an href into a path relative to the root ("/a/b"), or NULL when
 * it lies outside the root. */
static char *href_to_path(dav_session *s, const char *href)
{
	const char *p = href, *slash;
	char *dec, *rel, *norm;
	size_t rlen;

	if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0) {
		slash = strchr(strstr(p, "://") + 3, '/');
		p = slash != NULL ? slash : "/";
	}
	dec = url_decode(p, strlen(p));
	if (dec == NULL)
		return NULL;
	rlen = strlen(s->root_dec);
	if (strncmp(dec, s->root_dec, rlen) != 0 ||
	    (dec[rlen] != '\0' && dec[rlen] != '/')) {
		free(dec);
		return NULL;
	}
	rel = dec + rlen;
	norm = path_normalize(rel);
	free(dec);
	return norm;
}

static int perm_readonly(const char *perms)
{
	/* Nextcloud: W = file writable, CK = can create files/folders inside. */
	if (perms == NULL)
		return 0;
	return strchr(perms, 'W') == NULL && strchr(perms, 'C') == NULL &&
	       strchr(perms, 'K') == NULL;
}

static int run_propfind(dav_session *s, const char *path, const char *url,
                        int depth, const char *body, ms_parser **out)
{
	dav_req r;
	char hdr[32];
	int err;

	*out = ms_new();
	if (*out == NULL)
		return -ENOMEM;
	req_init(&r, "PROPFIND", path);
	r.url = url;
	r.xml = *out;
	r.body = body;
	r.body_len = strlen(body);
	snprintf(hdr, sizeof(hdr), "Depth: %d", depth);
	r.headers = curl_slist_append(NULL, hdr);
	r.headers = curl_slist_append(r.headers, "Content-Type: application/xml; charset=utf-8");
	err = perform(s, &r);
	if (err == 0 && r.status != 207)
		err = set_http_error(s, &r);
	if (err == 0 && ms_feed(*out, "", 0, 1) != 0) {
		snprintf(s->errmsg, sizeof(s->errmsg), "PROPFIND %s: %s",
		         path != NULL ? path : "", ms_error(*out));
		err = -EIO;
	}
	req_done(&r);
	if (err != 0) {
		ms_free(*out);
		*out = NULL;
	}
	return err;
}

int dav_propfind(dav_session *s, const char *path, int depth, dav_list *out)
{
	ms_parser *mp;
	dav_resp *res;
	size_t n, i;
	char *want;
	int err;

	memset(out, 0, sizeof(*out));
	out->quota_used = -1;
	out->quota_avail = -1;
	err = run_propfind(s, path, NULL, depth, propfind_body, &mp);
	if (err != 0)
		return err;
	want = path_normalize(path);
	res = ms_results(mp, &n);
	out->items = calloc(n > 0 ? n : 1, sizeof(dav_item));
	if (out->items == NULL || want == NULL) {
		free(want);
		ms_free(mp);
		return -ENOMEM;
	}
	for (i = 0; i < n; i++) {
		dav_item *it;
		char *rel;

		if (res[i].href == NULL || !res[i].found)
			continue;
		rel = href_to_path(s, res[i].href);
		if (rel == NULL)
			continue;
		it = &out->items[out->count++];
		it->is_self = strcmp(rel, want) == 0;
		it->name = xstrdup(it->is_self && strcmp(rel, "/") == 0 ? "" : path_basename(rel));
		it->is_dir = res[i].is_dir;
		it->size = it->is_dir ? 0 : (res[i].size >= 0 ? res[i].size : 0);
		it->mtime = res[i].mtime;
		it->etag = res[i].etag != NULL ? xstrdup(res[i].etag) : NULL;
		it->comment = res[i].comment != NULL ? xstrdup(res[i].comment) : NULL;
		it->readonly = perm_readonly(res[i].permissions);
		if (it->is_self) {
			out->quota_used = res[i].quota_used;
			out->quota_avail = res[i].quota_avail;
		}
		free(rel);
	}
	free(want);
	ms_free(mp);
	return 0;
}

void dav_list_free(dav_list *l)
{
	size_t i;

	for (i = 0; i < l->count; i++) {
		free(l->items[i].name);
		free(l->items[i].etag);
		free(l->items[i].comment);
	}
	free(l->items);
	memset(l, 0, sizeof(*l));
}

/* --- connect --------------------------------------------------------- */

static int split_url(dav_session *s, const char *url, char **path)
{
	const char *p = strstr(url, "://");
	const char *slash;

	if (p == NULL || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
		snprintf(s->errmsg, sizeof(s->errmsg),
		         "The URL must start with https:// or http:// (got \"%s\")", url);
		return -EINVAL;
	}
	slash = strchr(p + 3, '/');
	free(s->origin);
	if (slash == NULL) {
		s->origin = xstrdup(url);
		*path = xstrdup("");
	} else {
		s->origin = malloc((size_t)(slash - url) + 1);
		if (s->origin != NULL) {
			memcpy(s->origin, url, (size_t)(slash - url));
			s->origin[slash - url] = '\0';
		}
		*path = xstrdup(slash);
	}
	if (s->origin == NULL || *path == NULL)
		return -ENOMEM;
	/* drop trailing slashes */
	while ((*path)[0] != '\0' && (*path)[strlen(*path) - 1] == '/')
		(*path)[strlen(*path) - 1] = '\0';
	return 0;
}

static int set_root(dav_session *s, const char *root_path)
{
	strbuf sb;

	free(s->root_path);
	free(s->root_dec);
	free(s->root_url);
	s->root_path = xstrdup(root_path);
	s->root_dec = url_decode(root_path, strlen(root_path));
	sb_init(&sb);
	sb_printf(&sb, "%s%s/", s->origin, root_path);
	s->root_url = sb_steal(&sb);

	/* /remote.php/dav/files/<user>[/...] -> /remote.php/dav/uploads/<user> */
	free(s->uploads_url);
	s->uploads_url = NULL;
	{
		const char *key = "/remote.php/dav/files/";
		const char *k = strstr(root_path, key);
		if (k != NULL) {
			const char *uid = k + strlen(key);
			const char *end = strchr(uid, '/');
			size_t ulen = end != NULL ? (size_t)(end - uid) : strlen(uid);
			sb_init(&sb);
			sb_printf(&sb, "%s%.*s/remote.php/dav/uploads/%.*s", s->origin,
			          (int)(k - root_path), root_path, (int)ulen, uid);
			s->uploads_url = sb_steal(&sb);
		}
	}
	return s->root_path && s->root_dec && s->root_url ? 0 : -ENOMEM;
}

/* Asks Nextcloud for the user id: the files of a user live under
 * /remote.php/dav/files/<user id>/, and the id can differ from the login
 * name (for example when logging in with an e-mail address). */
static int nextcloud_root(dav_session *s, const char *base)
{
	ms_parser *mp;
	dav_resp *res;
	size_t n;
	strbuf sb;
	char *url, *dec, *uid;
	const char *key = "/remote.php/dav/principals/users/";
	int err;

	sb_init(&sb);
	sb_printf(&sb, "%s%s/remote.php/dav/", s->origin, base);
	url = sb_steal(&sb);
	if (url == NULL)
		return -ENOMEM;
	err = run_propfind(s, NULL, url, 0, principal_body, &mp);
	free(url);
	if (err != 0)
		return err;
	res = ms_results(mp, &n);
	if (n == 0 || res[0].principal == NULL) {
		ms_free(mp);
		return -ENOENT;
	}
	dec = url_decode(res[0].principal, strlen(res[0].principal));
	ms_free(mp);
	if (dec == NULL)
		return -ENOMEM;
	uid = strstr(dec, key);
	if (uid == NULL) {
		free(dec);
		return -ENOENT;
	}
	uid += strlen(key);
	if (uid[0] != '\0' && uid[strlen(uid) - 1] == '/')
		uid[strlen(uid) - 1] = '\0';
	sb_init(&sb);
	sb_printf(&sb, "%s/remote.php/dav/files/", base);
	url_encode_path(&sb, uid);
	free(dec);
	if (sb.failed) {
		sb_free(&sb);
		return -ENOMEM;
	}
	err = set_root(s, sb.data);
	sb_free(&sb);
	return err;
}

int dav_connect(dav_session *s)
{
	char *path = NULL;
	dav_list l;
	int err;

	err = split_url(s, s->url, &path);
	if (err != 0)
		goto out;

	if (s->kind == DAV_KIND_WEBDAV) {
		/* plain WebDAV, also on a Nextcloud server: the URL is the folder */
		s->nextcloud = 0;
		err = set_root(s, path);
	} else if (strstr(path, "/remote.php/") != NULL) {
		/* a full Nextcloud (or ownCloud) WebDAV URL was given */
		s->nextcloud = 1;
		err = set_root(s, path);
	} else {
		err = nextcloud_root(s, path);
		if (err == 0) {
			s->nextcloud = 1;
		} else if (err == -EACCES || err == -ENODEV || err == -ENOMEM) {
			goto out;
		} else if (s->kind == DAV_KIND_NEXTCLOUD) {
			snprintf(s->errmsg, sizeof(s->errmsg),
			         "No Nextcloud server at %s%s (the type is set to Nextcloud)",
			         s->origin, path);
			s->last_kind = DAV_FAIL_OTHER;
			err = -ENOENT;
			goto out;
		} else {
			/* not Nextcloud: use the URL as a plain WebDAV folder */
			s->nextcloud = 0;
			err = set_root(s, path);
		}
	}
	if (err != 0)
		goto out;
	if (!s->nextcloud) {
		/* no Nextcloud chunked upload on plain WebDAV */
		free(s->uploads_url);
		s->uploads_url = NULL;
	}

	err = dav_propfind(s, "/", 0, &l);
	if (err == 0) {
		if (l.count == 0 || !l.items[0].is_dir) {
			snprintf(s->errmsg, sizeof(s->errmsg),
			         "%s is not a WebDAV folder", s->root_url);
			err = -ENOTDIR;
		}
		dav_list_free(&l);
	}
	if (err == 0) {
		s->errmsg[0] = '\0';
		plat_log(LOG_INFO, "Connected to %s (%s)\n", s->root_url,
		         s->nextcloud ? "Nextcloud" : "WebDAV");
	}
out:
	free(path);
	return err;
}

/* --- file transfer --------------------------------------------------- */

int dav_get_range(dav_session *s, const char *path, int64_t off, void *buf,
                  size_t len, size_t *got, char *etag, size_t etag_size)
{
	dav_req r;
	char range[64], match[200];
	int err;

	*got = 0;
	if (len == 0)
		return 0;
	req_init(&r, "GET", path);
	r.buf = buf;
	r.buf_len = len;
	snprintf(range, sizeof(range), "Range: bytes=%lld-%lld", (long long)off,
	         (long long)(off + (int64_t)len - 1));
	r.headers = curl_slist_append(NULL, range);
	/* the rest of a file must come from the same version of it */
	if (etag != NULL && etag[0] != '\0') {
		snprintf(match, sizeof(match), "If-Match: %s", etag);
		r.headers = curl_slist_append(r.headers, match);
		plat_log(LOG_DEBUG, "%s, %s\n", range, match);
	} else {
		plat_log(LOG_DEBUG, "%s\n", range);
	}
	r.range_off = off;
	err = perform(s, &r);
	if (err == 0 && r.status == 416)
		r.status = 206; /* reading at or past the end */
	if (err == 0 && r.status == 412 && etag != NULL && etag[0] != '\0') {
		snprintf(s->errmsg, sizeof(s->errmsg), "%s was changed on the server while it was read",
		         path);
		plat_log(LOG_INFO, "%s\n", s->errmsg);
		s->last_status = r.status;
		err = -ETXTBSY;
	} else if (err == 0 && (r.status < 200 || r.status >= 300)) {
		err = set_http_error(s, &r);
	}
	/* A weak ETag (W/"...") never matches If-Match; Apache, for one, gives
	 * weak ETags for files changed within the last second. Such a read
	 * goes on without the check. */
	if (err == 0 && etag != NULL && etag[0] == '\0' && r.etag[0] != '\0' &&
	    strncmp(r.etag, "W/", 2) != 0)
		snprintf(etag, etag_size, "%s", r.etag);
	*got = r.got;
	req_done(&r);
	return err;
}

int dav_get_file(dav_session *s, const char *path, tmpf *f)
{
	dav_req r;
	int err;

	req_init(&r, "GET", path);
	r.download = f;
	err = perform(s, &r);
	if (err == 0 && (r.status < 200 || r.status >= 300))
		err = set_http_error(s, &r);
	if (err == 0 && tmpf_truncate(f, r.dl_pos) != 0)
		err = -EIO;
	req_done(&r);
	return err;
}

static int put_single(dav_session *s, const char *path, tmpf *f, int64_t size,
                      time_t mtime)
{
	dav_req r;
	char hdr[64];
	int err;

	req_init(&r, "PUT", path);
	r.upload = f;
	r.upload_size = size;
	/* the server may need a while to store a big file after receiving it */
	r.timeout = s->io_timeout > 300 ? s->io_timeout : 300;
	r.headers = curl_slist_append(NULL, "Content-Type: application/octet-stream");
	if (mtime != 0 && s->nextcloud) {
		snprintf(hdr, sizeof(hdr), "X-OC-Mtime: %lld", (long long)mtime);
		r.headers = curl_slist_append(r.headers, hdr);
	}
	err = perform(s, &r);
	if (err == 0 && (r.status < 200 || r.status >= 300))
		err = set_http_error(s, &r);
	req_done(&r);
	return err;
}

/* Nextcloud chunked upload (v2): MKCOL an upload folder, PUT numbered
 * parts into it, then MOVE its ".file" to the destination. Servers and
 * proxies often refuse single requests above some size (the Nextcloud
 * Docker image: 1 GiB), the official clients upload like this. */
static int put_chunked(dav_session *s, const char *path, tmpf *f, int64_t size,
                       time_t mtime, int *unsupported)
{
	char *dest = NULL, *folder = NULL;
	struct curl_slist *h;
	strbuf sb;
	dav_req r;
	char hdr[64];
	int64_t off;
	int err, n;

	*unsupported = 0;
	dest = make_url(s, path, 0);
	sb_init(&sb);
	sb_printf(&sb, "%s/nch-%lx-%x-%x", s->uploads_url, (unsigned long)plat_time(),
	          ++s->upload_seq, (unsigned)(plat_ms() & 0xFFFFF));
	folder = sb_steal(&sb);
	if (dest == NULL || folder == NULL) {
		err = -ENOMEM;
		goto out;
	}

#define DEST_HEADER(list) do { \
		strbuf hb; sb_init(&hb); sb_printf(&hb, "Destination: %s", dest); \
		list = curl_slist_append(list, hb.data); sb_free(&hb); } while (0)

	req_init(&r, "MKCOL", NULL);
	r.url = folder;
	h = NULL;
	DEST_HEADER(h);
	r.headers = h;
	err = perform(s, &r);
	if (err == 0 && r.status != 201) {
		/* no chunked upload here (old server, or uploads disabled) */
		*unsupported = (r.status == 404 || r.status == 405 || r.status == 501);
		err = set_http_error(s, &r);
	}
	req_done(&r);
	if (err != 0)
		goto out;

	for (off = 0, n = 1; off < size; off += s->chunk_size, n++) {
		char url[1024];
		int64_t len = size - off < s->chunk_size ? size - off : s->chunk_size;

		snprintf(url, sizeof(url), "%s/%05d", folder, n);
		req_init(&r, "PUT", NULL);
		r.url = url;
		r.upload = f;
		r.upload_base = off;
		r.upload_size = len;
		h = curl_slist_append(NULL, "Content-Type: application/octet-stream");
		DEST_HEADER(h);
		snprintf(hdr, sizeof(hdr), "OC-Total-Length: %lld", (long long)size);
		h = curl_slist_append(h, hdr);
		r.headers = h;
		plat_log(LOG_DEBUG, "chunk %d of %s (%lld bytes)\n", n, path, (long long)len);
		err = perform(s, &r);
		if (err == 0 && (r.status < 200 || r.status >= 300))
			err = set_http_error(s, &r);
		req_done(&r);
		if (err != 0)
			goto cancel;
	}

	{
		char url[1024];

		snprintf(url, sizeof(url), "%s/.file", folder);
		req_init(&r, "MOVE", NULL);
		r.url = url;
		/* assembling a big file can take the server a while */
		r.timeout = s->io_timeout > 600 ? s->io_timeout : 600;
		h = NULL;
		DEST_HEADER(h);
		h = curl_slist_append(h, "Overwrite: T");
		snprintf(hdr, sizeof(hdr), "OC-Total-Length: %lld", (long long)size);
		h = curl_slist_append(h, hdr);
		if (mtime != 0) {
			snprintf(hdr, sizeof(hdr), "X-OC-Mtime: %lld", (long long)mtime);
			h = curl_slist_append(h, hdr);
		}
		r.headers = h;
		err = perform(s, &r);
		if (err == 0 && (r.status < 200 || r.status >= 300))
			err = set_http_error(s, &r);
		req_done(&r);
		if (err == 0)
			goto out;
	}
#undef DEST_HEADER

cancel:
	{
		char saved[sizeof(s->errmsg)];

		/* keep the first error, remove the unfinished upload */
		memcpy(saved, s->errmsg, sizeof(saved));
		req_init(&r, "DELETE", NULL);
		r.url = folder;
		perform(s, &r);
		req_done(&r);
		memcpy(s->errmsg, saved, sizeof(saved));
	}
out:
	free(dest);
	free(folder);
	return err;
}

int dav_put_file(dav_session *s, const char *path, tmpf *f, time_t mtime)
{
	int64_t size = tmpf_size(f);
	int unsupported = 0, err;

	if (size < 0)
		return -EIO;
	if (s->uploads_url != NULL && size > s->chunk_size) {
		err = put_chunked(s, path, f, size, mtime, &unsupported);
		if (!unsupported)
			return err;
		plat_log(LOG_INFO, "chunked upload not available, using one request\n");
		free(s->uploads_url);
		s->uploads_url = NULL;
	}
	return put_single(s, path, f, size, mtime);
}

/* --- simple methods -------------------------------------------------- */

static int simple(dav_session *s, const char *method, const char *path, int dir,
                  struct curl_slist *headers, const char *body)
{
	dav_req r;
	int err;

	req_init(&r, method, path);
	r.dir = dir;
	r.headers = headers;
	if (body != NULL) {
		r.body = body;
		r.body_len = strlen(body);
	}
	err = perform(s, &r);
	if (err == 0 && r.status == 207) {
		/* Multi-Status here means that some members failed */
		snprintf(s->errmsg, sizeof(s->errmsg),
		         "%s %s: the server could not process all objects", method, path);
		err = -EIO;
	} else if (err == 0 && (r.status < 200 || r.status >= 300)) {
		err = set_http_error(s, &r);
	}
	req_done(&r);
	return err;
}

int dav_mkcol(dav_session *s, const char *path)
{
	return simple(s, "MKCOL", path, 1, NULL, NULL);
}

int dav_delete(dav_session *s, const char *path)
{
	return simple(s, "DELETE", path, 0, NULL, NULL);
}

int dav_move(dav_session *s, const char *from, const char *to, int overwrite)
{
	struct curl_slist *h;
	strbuf sb;
	char *dest;
	int err;

	dest = make_url(s, to, 0);
	if (dest == NULL)
		return -ENOMEM;
	sb_init(&sb);
	sb_printf(&sb, "Destination: %s", dest);
	free(dest);
	if (sb.failed) {
		sb_free(&sb);
		return -ENOMEM;
	}
	h = curl_slist_append(NULL, sb.data);
	h = curl_slist_append(h, overwrite ? "Overwrite: T" : "Overwrite: F");
	sb_free(&sb);
	err = simple(s, "MOVE", from, 0, h, NULL);
	return err;
}

static void xml_escape(strbuf *sb, const char *s)
{
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '<': sb_puts(sb, "&lt;"); break;
		case '>': sb_puts(sb, "&gt;"); break;
		case '&': sb_puts(sb, "&amp;"); break;
		case '"': sb_puts(sb, "&quot;"); break;
		default:  sb_append(sb, s, 1); break;
		}
	}
}

static int proppatch(dav_session *s, const char *path, const char *xml)
{
	ms_parser *mp = ms_new();
	dav_req r;
	dav_resp *res;
	size_t n, i;
	int err;

	if (mp == NULL)
		return -ENOMEM;
	req_init(&r, "PROPPATCH", path);
	r.xml = mp;
	r.body = xml;
	r.body_len = strlen(xml);
	r.headers = curl_slist_append(NULL, "Content-Type: application/xml; charset=utf-8");
	err = perform(s, &r);
	if (err == 0 && r.status != 207 && (r.status < 200 || r.status >= 300))
		err = set_http_error(s, &r);
	if (err == 0 && r.status == 207) {
		/* 207: a property may have been refused (403/409) */
		ms_feed(mp, "", 0, 1);
		res = ms_results(mp, &n);
		for (i = 0; i < n; i++) {
			if (!res[i].found && res[i].status == 0) {
				/* no 200 propstat: refused */
				snprintf(s->errmsg, sizeof(s->errmsg),
				         "PROPPATCH %s: the server refused the change", path);
				err = -EOPNOTSUPP;
			}
		}
	}
	req_done(&r);
	ms_free(mp);
	return err;
}

int dav_set_mtime(dav_session *s, const char *path, time_t mtime)
{
	strbuf sb;
	int err;

	if (!s->nextcloud)
		return -EOPNOTSUPP;
	sb_init(&sb);
	sb_printf(&sb,
	          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	          "<d:propertyupdate xmlns:d=\"DAV:\"><d:set><d:prop>"
	          "<d:lastmodified>%lld</d:lastmodified>"
	          "</d:prop></d:set></d:propertyupdate>\n",
	          (long long)mtime);
	if (sb.failed) {
		sb_free(&sb);
		return -ENOMEM;
	}
	err = proppatch(s, path, sb.data);
	sb_free(&sb);
	return err;
}

int dav_set_comment(dav_session *s, const char *path, const char *comment)
{
	strbuf sb;
	int err;

	sb_init(&sb);
	sb_puts(&sb, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	             "<d:propertyupdate xmlns:d=\"DAV:\" xmlns:a=\"urn:amiga:\">");
	if (comment != NULL && comment[0] != '\0') {
		sb_puts(&sb, "<d:set><d:prop><a:comment>");
		xml_escape(&sb, comment);
		sb_puts(&sb, "</a:comment></d:prop></d:set>");
	} else {
		sb_puts(&sb, "<d:remove><d:prop><a:comment/></d:prop></d:remove>");
	}
	sb_puts(&sb, "</d:propertyupdate>\n");
	if (sb.failed) {
		sb_free(&sb);
		return -ENOMEM;
	}
	err = proppatch(s, path, sb.data);
	sb_free(&sb);
	return err;
}
