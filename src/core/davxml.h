/* webdav-handler - WebDAV multistatus (PROPFIND) parser. Public domain. */
#ifndef WDV_DAVXML_H
#define WDV_DAVXML_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* One <response> of a 207 Multi-Status body. Only properties from a
 * propstat with status 200 are filled in. */
typedef struct {
	char    *href;        /* as sent by the server, still percent-encoded */
	int      status;      /* response-level <status>, 0 when absent */
	int      found;       /* at least one property came with status 200 */
	int      is_dir;      /* resourcetype contains collection */
	int64_t  size;        /* getcontentlength, -1 when unknown */
	time_t   mtime;       /* getlastmodified, 0 when unknown */
	char    *etag;
	char    *comment;     /* {urn:amiga:}comment */
	char    *permissions; /* {http://owncloud.org/ns}permissions */
	int64_t  quota_used;  /* -1 when unknown */
	int64_t  quota_avail; /* -1 unknown; Nextcloud sends -3 for unlimited */
	char    *principal;   /* href inside current-user-principal */
} dav_resp;

typedef struct ms_parser ms_parser;

ms_parser *ms_new(void);
/* Feeds body data. Returns 0, or -1 for malformed XML or no memory. */
int  ms_feed(ms_parser *p, const char *data, size_t len, int final);
/* Results stay owned by the parser. */
dav_resp *ms_results(ms_parser *p, size_t *count);
const char *ms_error(ms_parser *p);
void ms_free(ms_parser *p);

#endif
