/* nextcloud-handler - WebDAV client on top of libcurl. Public domain. */
#ifndef NCH_DAVCLIENT_H
#define NCH_DAVCLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "platform.h"

typedef struct {
	const char *url;             /* server address or full WebDAV URL */
	const char *user;
	const char *password;
	const char *ca_file;         /* PEM bundle, may be NULL */
	const char *ca_path;         /* hashed certificate directory, may be NULL */
	const char *proxy;           /* "host:port", may be NULL */
	int         insecure;        /* do not verify the server certificate */
	long        connect_timeout; /* seconds */
	long        io_timeout;      /* seconds without progress */
	const char *user_agent;
} dav_config;

typedef struct dav_session dav_session;

typedef struct {
	char    *name;      /* decoded UTF-8 name of the entry */
	int      is_self;   /* the object that was asked for */
	int      is_dir;
	int64_t  size;
	time_t   mtime;
	char    *etag;
	char    *comment;
	int      readonly;  /* the server says it cannot be changed */
} dav_item;

typedef struct {
	dav_item *items;
	size_t    count;
	int64_t   quota_used;  /* of the requested object, -1 when unknown */
	int64_t   quota_avail; /* -1 unknown, -3 unlimited (Nextcloud) */
} dav_list;

/* Paths are UTF-8, relative to the mounted root: "/", "/Photos/x.jpg". */

dav_session *dav_new(const dav_config *cfg);
/* Finds the WebDAV root (Nextcloud user folder or the URL itself) and
 * checks the login. Returns 0 or -errno; see dav_error(). */
int  dav_connect(dav_session *s);
void dav_free(dav_session *s);

const char *dav_error(dav_session *s);   /* text for the last failure */
int  dav_is_nextcloud(dav_session *s);
const char *dav_root_url(dav_session *s);

int  dav_propfind(dav_session *s, const char *path, int depth, dav_list *out);
void dav_list_free(dav_list *l);

/* Reads len bytes at off into buf (HTTP Range); *got is set to the bytes
 * read, fewer at the end of the file. */
int  dav_get_range(dav_session *s, const char *path, int64_t off, void *buf,
                   size_t len, size_t *got);
/* Downloads the whole file into f. */
int  dav_get_file(dav_session *s, const char *path, tmpf *f);
/* Uploads f as the new content of path. mtime 0 = let the server decide. */
int  dav_put_file(dav_session *s, const char *path, tmpf *f, time_t mtime);

int  dav_mkcol(dav_session *s, const char *path);
int  dav_delete(dav_session *s, const char *path);
int  dav_move(dav_session *s, const char *from, const char *to, int overwrite);
int  dav_set_mtime(dav_session *s, const char *path, time_t mtime);
/* comment NULL or "" removes it. */
int  dav_set_comment(dav_session *s, const char *path, const char *comment);

#endif
