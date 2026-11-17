/* nextcloud-handler - the root of servers. Each server has its own WebDAV
 * session and file system (ncfs), made when its folder is first used.
 * Public domain. */
#include "ncroot.h"
#include "davclient.h"
#include "platform.h"
#include "servers.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CFG_CHECK_MS 1000         /* how often the server file is looked at */
#define RETRY_MS     10000        /* between failed connection attempts */
#define ICON_MAX     (256 * 1024)
#define BIG_DISK     ((int64_t)1 << 40)
/* a server that cannot be used: AmigaDOS shows "no disk" */
#define ERR_OFFLINE  (-ENODEV)

typedef struct server {
	struct server *next;      /* on the list of removed servers */
	server_cfg     cfg;
	char           display[SERVER_NAME_MAX * 3 + 140];  /* "Nextcloud:Home/" */
	dav_session   *dav;
	ncfs          *fs;
	int            connected;
	int64_t        last_fail;
	int            refused;   /* password cancelled: no new tries until reset */
	int            reported;  /* a connection problem was shown */
	char           password[256];   /* typed into the requester */
	int            have_password;
	int            nfiles;    /* open files */
	int            gone;      /* removed from the server list */
} server;

struct ncroot_file {
	ncroot_file *next;
	server      *s;
	ncfs_file   *f;
	int          icon;        /* a server's icon at the root */
};

struct ncroot {
	ncroot_options opt;
	server       **server;
	int            nservers;
	server        *gone;
	ncroot_file   *files;
	char           servers_file[400];
	time_t         cfg_mtime;
	int64_t        cfg_size;
	int            cfg_exists;
	int64_t        cfg_checked;
	int            cfg_reported;
	time_t         started;
	char          *icon;
	size_t         iconlen;
	int            icon_loaded;
};

/* --- servers ------------------------------------------------------------- */

static void set_display(ncroot *r, server *s)
{
	snprintf(s->display, sizeof(s->display), "%s%s/", r->opt.display, s->cfg.name);
}

static void disconnect(server *s)
{
	if (s->fs != NULL)
		ncfs_free(s->fs);   /* uploads what open files still hold */
	if (s->dav != NULL)
		dav_free(s->dav);
	s->fs = NULL;
	s->dav = NULL;
	s->connected = 0;
}

static void server_free(server *s)
{
	if (s->connected)
		plat_log(LOG_INFO, "%s: disconnected\n", s->cfg.name);
	disconnect(s);
	memset(s->password, 0, sizeof(s->password));
	memset(s->cfg.password, 0, sizeof(s->cfg.password));
	free(s);
}

/* a removed server goes once its last file is closed */
static void server_release(ncroot *r, server *s)
{
	server **pp;

	if (s->nfiles > 0 || !s->gone)
		return;
	for (pp = &r->gone; *pp != NULL && *pp != s; pp = &(*pp)->next)
		;
	if (*pp == s)
		*pp = s->next;
	server_free(s);
}

static void retire(ncroot *r, server *s)
{
	if (s->nfiles > 0) {
		/* files are open on it: keep it for them */
		s->gone = 1;
		s->next = r->gone;
		r->gone = s;
	} else {
		server_free(s);
	}
}

static void report(ncroot *r, server *s, const char *text)
{
	char title[400];

	snprintf(title, sizeof(title), "%s %s", r->opt.display, s != NULL ? s->cfg.name : "");
	plat_report(title, text);
}

/* the server file changed: keep what is the same, start again for the rest */
static void apply_config(ncroot *r, server_list *l)
{
	server **n = calloc((size_t)(l->count > 0 ? l->count : 1), sizeof(*n));
	int i, j, k;

	if (n == NULL)
		return;
	for (i = 0; i < l->count; i++) {
		server *s = NULL;

		for (j = 0; j < r->nservers; j++) {
			if (r->server[j] != NULL &&
			    name_casecmp(r->server[j]->cfg.name, l->server[i].name) == 0) {
				s = r->server[j];
				r->server[j] = NULL;
				break;
			}
		}
		if (s != NULL && !server_cfg_equal(&s->cfg, &l->server[i])) {
			if (server_cfg_same_account(&s->cfg, &l->server[i])) {
				/* only the name's case or the password: keep the session */
				int new_password = strcmp(s->cfg.password, l->server[i].password) != 0;

				s->cfg = l->server[i];
				if (new_password && !s->connected) {
					s->have_password = 0;
					s->refused = 0;
					s->reported = 0;
					s->last_fail = 0;
				}
			} else {
				retire(r, s);
				s = NULL;
			}
		}
		if (s == NULL) {
			s = calloc(1, sizeof(*s));
			if (s == NULL)
				continue;
			s->cfg = l->server[i];
		}
		set_display(r, s);
		n[i] = s;
	}
	for (j = 0; j < r->nservers; j++)
		if (r->server[j] != NULL)
			retire(r, r->server[j]);
	free(r->server);
	/* leave out servers that could not be made */
	for (i = 0, k = 0; i < l->count; i++)
		if (n[i] != NULL)
			n[k++] = n[i];
	r->server = n;
	r->nservers = k;
	plat_log(LOG_INFO, "%d servers\n", r->nservers);
}

static void load_config(ncroot *r, int force)
{
	struct stat st;
	server_list l;
	char err[600];
	int rc, exists, skipped = 0;

	if (!force && r->cfg_checked != 0 && plat_ms() - r->cfg_checked < CFG_CHECK_MS)
		return;
	r->cfg_checked = plat_ms();
	exists = stat(r->servers_file, &st) == 0;
	if (!force && exists == r->cfg_exists &&
	    (!exists || (st.st_mtime == r->cfg_mtime && (int64_t)st.st_size == r->cfg_size)))
		return;
	r->cfg_exists = exists;
	r->cfg_mtime = exists ? st.st_mtime : 0;
	r->cfg_size = exists ? (int64_t)st.st_size : 0;
	servers_init(&l);
	rc = exists ? servers_load(r->servers_file, &l, err, sizeof(err), &skipped) : -ENOENT;
	if (rc == -EINVAL) {
		/* keep the servers we have */
		if (!r->cfg_reported) {
			char msg[800];

			r->cfg_reported = 1;
			snprintf(msg, sizeof(msg), "The server list cannot be read:\n%s\n\n"
			         "Use SYS:Prefs/nchprefs to make a new one.", err);
			report(r, NULL, msg);
		}
		servers_free(&l);
		return;
	}
	r->cfg_reported = 0;
	if (rc != 0 && rc != -ENOENT) {
		servers_free(&l);
		return;
	}
	if (skipped > 0)
		plat_log(LOG_ERR, "%d servers in %s were skipped (bad or repeated names)\n",
		         skipped, r->servers_file);
	apply_config(r, &l);
	servers_free(&l);
}

/* the password typed into the requester goes into the server file */
static void save_password(ncroot *r, server *s)
{
	const char *dirs[2];
	struct stat st;
	int i;

	dirs[0] = r->opt.config_dir;
	dirs[1] = r->opt.save_dir;
	snprintf(s->cfg.password, sizeof(s->cfg.password), "%s", s->password);
	for (i = 0; i < 2; i++) {
		char file[400];
		server_list l;
		int k;

		if (dirs[i] == NULL)
			continue;
		snprintf(file, sizeof(file), "%s/%s", dirs[i], CFG_SERVERS);
		servers_init(&l);
		if (servers_load(file, &l, NULL, 0, NULL) == 0 &&
		    (k = servers_find(&l, s->cfg.name)) >= 0) {
			snprintf(l.server[k].password, sizeof(l.server[k].password), "%s", s->password);
			if (servers_save(file, &l) != 0)
				plat_log(LOG_ERR, "cannot save the password in %s\n", file);
		}
		servers_free(&l);
	}
	/* that change is ours: no need to start the server again */
	if (stat(r->servers_file, &st) == 0) {
		r->cfg_mtime = st.st_mtime;
		r->cfg_size = (int64_t)st.st_size;
	}
}

static const char *fail_hint(dav_session *dav)
{
	long status = dav_last_status(dav);

	switch (dav_last_kind(dav)) {
	case DAV_FAIL_TLS:
		return "The server's certificate could not be checked. If the server\n"
		       "uses a certificate of its own, give it as the certificate file\n"
		       "in nchprefs; otherwise check that AmiSSL:Certs is up to date.";
	case DAV_FAIL_NETWORK:
		return "Check the address in nchprefs and that the network is up.";
	}
	if (status == 401 || status == 403)
		return "The server did not accept the user name or password.";
	return "Check the address in nchprefs.";
}

/* Connects on first use. Returns 0 or -errno. */
static int need_server(ncroot *r, server *s)
{
	char msg[1600];
	const char *problem = NULL;
	int tries, err = ERR_OFFLINE, remember = 0, asked = 0, ask;

	if (s->connected)
		return 0;
	if (s->refused)
		return ERR_OFFLINE;
	if (s->last_fail != 0 && plat_ms() - s->last_fail < RETRY_MS)
		return ERR_OFFLINE;
	if (s->cfg.url[0] == '\0') {
		if (!s->reported) {
			s->reported = 1;
			report(r, s, "The server has no address. Set it in SYS:Prefs/nchprefs.");
		}
		s->last_fail = plat_ms();
		return ERR_OFFLINE;
	}
	if (s->dav == NULL) {
		dav_config dc;
		ncfs_options fo;

		memset(&dc, 0, sizeof(dc));
		dc.url = s->cfg.url;
		dc.user = s->cfg.user[0] != '\0' ? s->cfg.user : NULL;
		dc.password = s->cfg.password;
		/* a server's own certificate replaces the usual ones */
		dc.ca_file = s->cfg.cafile[0] != '\0' ? s->cfg.cafile : NULL;
		dc.ca_path = s->cfg.cafile[0] != '\0' ? NULL : r->opt.ca_path;
		dc.insecure = s->cfg.insecure;
		dc.proxy = r->opt.proxy;
		dc.connect_timeout = r->opt.connect_timeout;
		dc.io_timeout = r->opt.io_timeout;
		dc.chunk_size = r->opt.chunk_size;
		dc.user_agent = r->opt.user_agent;
		memset(&fo, 0, sizeof(fo));
		fo.tempdir = r->opt.tempdir;
		fo.keepdir = r->opt.keepdir;
		fo.cache_ms = r->opt.cache_ms;
		fo.readonly = r->opt.readonly;
		fo.display = s->display;
		s->dav = dav_new(&dc);
		s->fs = s->dav != NULL ? ncfs_new(s->dav, &fo) : NULL;
		if (s->fs == NULL) {
			disconnect(s);
			return -ENOMEM;
		}
	}
	/* a password is needed first when the server has a user but none */
	ask = s->cfg.user[0] != '\0' && s->cfg.password[0] == '\0';
	for (tries = 0; tries < 4; tries++) {
		if (ask && !s->have_password) {
			if (asked >= 3 ||
			    !plat_ask_password(s->cfg.name, s->cfg.user, s->cfg.url, problem,
			                       s->password, sizeof(s->password), &remember)) {
				/* Workbench looks at open folders all the time: do not ask
				 * again until "DiskChange" or a change of the server */
				s->refused = 1;
				memset(s->password, 0, sizeof(s->password));
				return ERR_OFFLINE;
			}
			asked++;
			s->have_password = 1;
		}
		dav_set_password(s->dav, s->have_password ? s->password : s->cfg.password);
		plat_log(LOG_INFO, "%s: connecting to %s\n", s->cfg.name, s->cfg.url);
		err = dav_connect(s->dav);
		if (err == 0) {
			/* saved only once the server has accepted it */
			if (remember && s->have_password)
				save_password(r, s);
			s->connected = 1;
			s->reported = 0;
			s->last_fail = 0;
			plat_log(LOG_INFO, "%s: connected, %s\n", s->cfg.name, dav_root_url(s->dav));
			return 0;
		}
		plat_log(LOG_ERR, "%s: %s\n", s->cfg.name, dav_error(s->dav));
		if (dav_last_status(s->dav) == 401 && s->cfg.user[0] != '\0') {
			/* wrong or missing password: ask (again) */
			s->have_password = 0;
			ask = 1;
			problem = "The server did not accept the password.";
			continue;
		}
		break;
	}
	if (!s->reported) {
		s->reported = 1;
		snprintf(msg, sizeof(msg), "Cannot connect to %s\n(%s):\n%s\n\n%s\n\n"
		         "The folder shows \"no disk\" until this is fixed\n"
		         "(DiskChange tries again at once).",
		         s->cfg.name, s->cfg.url, dav_error(s->dav), fail_hint(s->dav));
		report(r, s, msg);
	}
	s->last_fail = plat_ms();
	return ERR_OFFLINE;
}

/* --- paths ----------------------------------------------------------------- */

enum { AT_ROOT, AT_SERVER, AT_ICON };

typedef struct {
	int         kind;
	server     *s;
	const char *rest;    /* path in the server ("/" for its top) */
} where;

static server *find_server(ncroot *r, const char *name, size_t len)
{
	char tmp[sizeof(((server_cfg *)0)->name)];
	int i;

	if (len >= sizeof(tmp))
		return NULL;
	memcpy(tmp, name, len);
	tmp[len] = '\0';
	for (i = 0; i < r->nservers; i++)
		if (name_casecmp(r->server[i]->cfg.name, tmp) == 0)
			return r->server[i];
	return NULL;
}

static void load_icon(ncroot *r)
{
	FILE *f;

	if (r->icon_loaded)
		return;
	r->icon_loaded = 1;
	free(r->icon);
	r->icon = NULL;
	r->iconlen = 0;
	if (r->opt.icon_file == NULL || (f = fopen(r->opt.icon_file, "rb")) == NULL)
		return;
	r->icon = malloc(ICON_MAX);
	if (r->icon != NULL) {
		r->iconlen = fread(r->icon, 1, ICON_MAX, f);
		if (r->iconlen == 0 || r->iconlen == ICON_MAX) {
			free(r->icon);
			r->icon = NULL;
			r->iconlen = 0;
		}
	}
	fclose(f);
}

static const char top_path[] = "/";

/* path must be normalised ("/a/b") */
static int locate(ncroot *r, const char *path, where *w)
{
	const char *p = path + 1, *slash;
	size_t len;

	w->s = NULL;
	w->rest = NULL;
	if (path[1] == '\0') {
		w->kind = AT_ROOT;
		return 0;
	}
	load_config(r, 0);
	slash = strchr(p, '/');
	len = slash != NULL ? (size_t)(slash - p) : strlen(p);
	w->s = find_server(r, p, len);
	if (w->s != NULL) {
		w->kind = AT_SERVER;
		w->rest = slash != NULL ? slash : top_path;
		return 0;
	}
	if (slash == NULL && len > 5 && strcasecmp(p + len - 5, ".info") == 0) {
		load_icon(r);
		w->s = find_server(r, p, len - 5);
		if (w->s != NULL && r->icon != NULL) {
			w->kind = AT_ICON;
			return 0;
		}
	}
	return -ENOENT;
}

/* the root, its entries and a server's top folder cannot be changed */
static int fixed(const where *w)
{
	return w->kind != AT_SERVER || strcmp(w->rest, "/") == 0;
}

/* finds the place of a change; a new entry at the root is refused */
static int locate_change(ncroot *r, const char *path, where *w, char **norm)
{
	int err;

	*norm = path_normalize(path);
	if (*norm == NULL)
		return -ENOMEM;
	err = locate(r, *norm, w);
	if (err == -ENOENT && strchr(*norm + 1, '/') == NULL)
		return -EROFS;
	if (err != 0)
		return err;
	if (fixed(w))
		return -EROFS;
	return need_server(r, w->s);
}

/* the root and its entries carry the date of the server list */
static void root_dir_stat(ncroot *r, ncfs_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->is_dir = 1;
	st->mtime = r->cfg_mtime != 0 ? r->cfg_mtime : r->started;
	st->readonly = 1;
}

static void icon_stat(ncroot *r, ncfs_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->size = (int64_t)r->iconlen;
	st->mtime = r->cfg_mtime != 0 ? r->cfg_mtime : r->started;
	st->readonly = 1;
}

/* --- set-up ------------------------------------------------------------------- */

ncroot *ncroot_new(const ncroot_options *opt)
{
	ncroot *r = calloc(1, sizeof(*r));

	if (r == NULL)
		return NULL;
	r->opt = *opt;
	if (r->opt.display == NULL)
		r->opt.display = "Nextcloud:";
	r->started = plat_time();
	snprintf(r->servers_file, sizeof(r->servers_file), "%s/%s", opt->config_dir, CFG_SERVERS);
	load_config(r, 1);
	return r;
}

void ncroot_free(ncroot *r)
{
	int i;

	if (r == NULL)
		return;
	while (r->files != NULL)
		ncroot_release(r, r->files);
	for (i = 0; i < r->nservers; i++)
		server_free(r->server[i]);
	while (r->gone != NULL) {
		server *s = r->gone;
		r->gone = s->next;
		server_free(s);
	}
	free(r->server);
	free(r->icon);
	free(r);
}

int ncroot_connected(ncroot *r)
{
	int i, n = 0;

	for (i = 0; i < r->nservers; i++)
		if (r->server[i]->connected)
			n++;
	return n;
}

void ncroot_reset(ncroot *r)
{
	int i;

	load_config(r, 1);
	for (i = 0; i < r->nservers; i++) {
		server *s = r->server[i];

		s->refused = 0;
		s->reported = 0;
		s->last_fail = 0;
		if (s->fs != NULL)
			ncfs_flush_cache(s->fs);
	}
	r->cfg_reported = 0;
	r->icon_loaded = 0;
}

/* --- reading ------------------------------------------------------------------ */

int ncroot_getattr(ncroot *r, const char *path, ncfs_stat *st)
{
	char *norm = path_normalize(path);
	where w;
	int err;

	if (norm == NULL)
		return -ENOMEM;
	err = locate(r, norm, &w);
	if (err == 0) {
		if (w.kind == AT_ROOT || (w.kind == AT_SERVER && strcmp(w.rest, "/") == 0))
			/* no connection for the root and the servers' own folders */
			root_dir_stat(r, st);
		else if (w.kind == AT_ICON)
			icon_stat(r, st);
		else if ((err = need_server(r, w.s)) == 0)
			err = ncfs_getattr(w.s->fs, w.rest, st);
	}
	free(norm);
	return err;
}

int ncroot_readdir(ncroot *r, const char *path, ncfs_fill_fn fill, void *ud)
{
	char *norm = path_normalize(path);
	where w;
	int err, k;

	if (norm == NULL)
		return -ENOMEM;
	err = locate(r, norm, &w);
	if (err == 0 && w.kind == AT_ICON)
		err = -ENOTDIR;
	if (err == 0 && w.kind == AT_ROOT) {
		ncfs_stat st;

		load_config(r, 1);
		load_icon(r);
		for (k = 0; k < r->nservers; k++) {
			root_dir_stat(r, &st);
			if (fill(ud, r->server[k]->cfg.name, &st) != 0)
				goto out;
		}
		if (r->icon != NULL) {
			for (k = 0; k < r->nservers; k++) {
				char name[sizeof(r->server[k]->cfg.name) + 6];

				icon_stat(r, &st);
				snprintf(name, sizeof(name), "%s.info", r->server[k]->cfg.name);
				if (fill(ud, name, &st) != 0)
					goto out;
			}
		}
	} else if (err == 0 && (err = need_server(r, w.s)) == 0) {
		err = ncfs_readdir(w.s->fs, w.rest, fill, ud);
	}
out:
	free(norm);
	return err;
}

int ncroot_statfs(ncroot *r, const char *path, int64_t *total, int64_t *avail)
{
	char *norm = path != NULL ? path_normalize(path) : NULL;
	where w;

	/* the volume as a whole has no size: show plenty of room, so that
	 * copying is not refused; inside a connected server its quota */
	*total = BIG_DISK;
	*avail = r->opt.readonly ? 0 : BIG_DISK;
	if (norm != NULL && locate(r, norm, &w) == 0 && w.kind == AT_SERVER && w.s->connected) {
		int64_t t, a;

		if (ncfs_statfs(w.s->fs, &t, &a) == 0 && t > 0) {
			*total = t;
			*avail = r->opt.readonly ? 0 : a;
		}
	}
	free(norm);
	return 0;
}

int ncroot_getcomment(ncroot *r, const char *path, char *buf, size_t size)
{
	char *norm = path_normalize(path);
	where w;
	int err;

	if (norm == NULL)
		return -ENOMEM;
	err = locate(r, norm, &w);
	if (err == 0 && fixed(&w))
		err = -ENODATA;
	else if (err == 0 && (err = need_server(r, w.s)) == 0)
		err = ncfs_getcomment(w.s->fs, w.rest, buf, size);
	free(norm);
	return err;
}

/* --- files ---------------------------------------------------------------------- */

static ncroot_file *file_add(ncroot *r, server *s, ncfs_file *f, int icon)
{
	ncroot_file *rf = calloc(1, sizeof(*rf));

	if (rf == NULL)
		return NULL;
	rf->s = s;
	rf->f = f;
	rf->icon = icon;
	rf->next = r->files;
	r->files = rf;
	s->nfiles++;
	return rf;
}

int ncroot_open(ncroot *r, const char *path, int truncate, ncroot_file **fh)
{
	char *norm = path_normalize(path);
	ncfs_file *f;
	where w;
	int err;

	if (norm == NULL)
		return -ENOMEM;
	err = locate(r, norm, &w);
	if (err != 0)
		goto out;
	if (w.kind == AT_ROOT || (w.kind == AT_SERVER && strcmp(w.rest, "/") == 0)) {
		err = -EISDIR;
	} else if (w.kind == AT_ICON) {
		if (truncate)
			err = -EROFS;
		else if ((*fh = file_add(r, w.s, NULL, 1)) == NULL)
			err = -ENOMEM;
	} else if ((err = need_server(r, w.s)) == 0 &&
	           (err = ncfs_open(w.s->fs, w.rest, truncate, &f)) == 0) {
		*fh = file_add(r, w.s, f, 0);
		if (*fh == NULL) {
			ncfs_release(w.s->fs, f);
			err = -ENOMEM;
		}
	}
out:
	free(norm);
	return err;
}

int ncroot_create(ncroot *r, const char *path, ncroot_file **fh)
{
	char *norm;
	ncfs_file *f;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == 0 && (err = ncfs_create(w.s->fs, w.rest, &f)) == 0) {
		*fh = file_add(r, w.s, f, 0);
		if (*fh == NULL) {
			ncfs_release(w.s->fs, f);
			err = -ENOMEM;
		}
	}
	free(norm);
	return err;
}

int ncroot_read(ncroot *r, ncroot_file *fh, char *buf, size_t size, int64_t off)
{
	if (fh->icon) {
		if (off < 0)
			return -EINVAL;
		if (r->icon == NULL || off >= (int64_t)r->iconlen)
			return 0;
		if ((int64_t)size > (int64_t)r->iconlen - off)
			size = (size_t)((int64_t)r->iconlen - off);
		memcpy(buf, r->icon + off, size);
		return (int)size;
	}
	return ncfs_read(fh->s->fs, fh->f, buf, size, off);
}

int ncroot_write(ncroot *r, ncroot_file *fh, const char *buf, size_t size, int64_t off)
{
	(void)r;
	if (fh->icon)
		return -EROFS;
	return ncfs_write(fh->s->fs, fh->f, buf, size, off);
}

int ncroot_fgetattr(ncroot *r, ncroot_file *fh, ncfs_stat *st)
{
	if (fh->icon) {
		icon_stat(r, st);
		return 0;
	}
	return ncfs_fgetattr(fh->s->fs, fh->f, st);
}

int ncroot_ftruncate(ncroot *r, ncroot_file *fh, int64_t size)
{
	(void)r;
	if (fh->icon)
		return -EROFS;
	return ncfs_ftruncate(fh->s->fs, fh->f, size);
}

int ncroot_flush(ncroot *r, ncroot_file *fh)
{
	(void)r;
	if (fh->icon)
		return 0;
	return ncfs_flush(fh->s->fs, fh->f);
}

int ncroot_file_written(ncroot_file *fh)
{
	return !fh->icon && ncfs_file_written(fh->f);
}

int ncroot_release(ncroot *r, ncroot_file *fh)
{
	ncroot_file **pp;
	server *s = fh->s;
	int err = 0;

	for (pp = &r->files; *pp != NULL && *pp != fh; pp = &(*pp)->next)
		;
	if (*pp == fh)
		*pp = fh->next;
	if (!fh->icon && s->fs != NULL)
		err = ncfs_release(s->fs, fh->f);
	free(fh);
	s->nfiles--;
	server_release(r, s);
	return err;
}

/* --- changes -------------------------------------------------------------------- */

int ncroot_truncate(ncroot *r, const char *path, int64_t size)
{
	char *norm;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == 0)
		err = ncfs_truncate(w.s->fs, w.rest, size);
	free(norm);
	return err;
}

int ncroot_mkdir(ncroot *r, const char *path)
{
	char *norm;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == -EROFS && norm != NULL && locate(r, norm, &w) == 0)
		err = -EEXIST;  /* a server's folder or its icon */
	else if (err == 0)
		err = ncfs_mkdir(w.s->fs, w.rest);
	free(norm);
	return err;
}

int ncroot_unlink(ncroot *r, const char *path)
{
	char *norm;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == 0)
		err = ncfs_unlink(w.s->fs, w.rest);
	free(norm);
	return err;
}

int ncroot_rmdir(ncroot *r, const char *path)
{
	char *norm;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == 0)
		err = ncfs_rmdir(w.s->fs, w.rest);
	free(norm);
	return err;
}

int ncroot_rename(ncroot *r, const char *from, const char *to)
{
	char *nf = NULL, *nt = NULL;
	where wf, wt;
	int err = locate_change(r, from, &wf, &nf);

	if (err != 0)
		goto out;
	nt = path_normalize(to);
	if (nt == NULL) {
		err = -ENOMEM;
		goto out;
	}
	err = locate(r, nt, &wt);
	if (err == -ENOENT && strchr(nt + 1, '/') == NULL)
		err = -EROFS;  /* a new entry at the root */
	if (err != 0)
		goto out;
	if (fixed(&wt))
		err = -EROFS;
	else if (wt.s != wf.s)
		err = -EXDEV;  /* from one server to another */
	else
		err = ncfs_rename(wf.s->fs, wf.rest, wt.rest);
out:
	free(nf);
	free(nt);
	return err;
}

int ncroot_utimens(ncroot *r, const char *path, time_t mtime)
{
	char *norm;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == 0)
		err = ncfs_utimens(w.s->fs, w.rest, mtime);
	free(norm);
	return err;
}

int ncroot_setcomment(ncroot *r, const char *path, const char *comment)
{
	char *norm;
	where w;
	int err = locate_change(r, path, &w, &norm);

	if (err == 0)
		err = ncfs_setcomment(w.s->fs, w.rest, comment);
	free(norm);
	return err;
}
