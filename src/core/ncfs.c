/* nextcloud-handler - filesystem operations on top of WebDAV, with a
 * metadata cache. Public domain. */
#include "ncfs.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENODATA
#define ENODATA ENOENT
#endif

#define NBUCKETS     256
#define MAX_LISTINGS 256
#define MAX_STATS    4096
#define QUOTA_MS     30000
#define TRUNC_MS     30000
#define LIST_MAX_MS  60000  /* longest a listing is kept while we change it */
#define UNLIMITED    ((int64_t)1 << 40)  /* free space shown without a quota */

/* One cached object: the result of a depth 0 PROPFIND, or an entry of a
 * directory listing. */
typedef struct {
	char      *name;
	ncfs_stat  st;
	char      *comment;
} entry;

typedef struct listing {
	struct listing *next;
	char           *path;
	int64_t         when;     /* valid from (refreshed by our own changes) */
	int64_t         fetched;  /* read from the server */
	entry          *e;
	size_t          n;
	size_t          cap;
} listing;

typedef struct statent {
	struct statent *next;
	char           *path;
	int64_t         when;
	int             exists;
	entry           obj;
} statent;

struct ncfs_file {
	struct ncfs_file *next;
	char             *path;
	int64_t           size;
	time_t            mtime;
	int               ro_obj;      /* the server says the file is read-only */

	/* reading straight from the server */
	char             *blk;
	size_t            blk_cap;
	int64_t           blk_off;
	size_t            blk_len;
	size_t            blk_want;
	int64_t           next_off;
	char              etag[160];   /* version being read, from the first block */

	/* writing through a local temp file */
	tmpf             *tf;
	int               dirty;
	int               written;     /* write() or ftruncate() was called */
	int               is_new;      /* not on the server yet */
	time_t            set_mtime;   /* date to send with the upload */
};

struct ncfs {
	dav_session  *dav;
	ncfs_options  opt;
	listing      *lists[NBUCKETS];
	statent      *stats[NBUCKETS];
	size_t        nlists;
	size_t        nstats;
	ncfs_file    *files;           /* open files */
	char         *trunc_path;      /* truncated, waiting for the open */
	int64_t       trunc_when;
	int64_t       quota_used;
	int64_t       quota_avail;
	int64_t       quota_when;
	int           keep_count;
};

static unsigned hash(const char *s)
{
	unsigned h = 5381;

	while (*s != '\0')
		h = h * 33 + (unsigned char)*s++;
	return h % NBUCKETS;
}

static int fresh(ncfs *fs, int64_t when)
{
	return plat_ms() - when < (int64_t)fs->opt.cache_ms;
}

/* The root's own attributes hardly matter and Workbench asks for them
 * every few seconds, so they are kept longer. */
#define ROOT_MS 30000

/* --- cache ------------------------------------------------------------ */

static void entry_free(entry *e)
{
	free(e->name);
	free(e->comment);
	e->name = NULL;
	e->comment = NULL;
}

static void listing_free(listing *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		entry_free(&l->e[i]);
	free(l->e);
	free(l->path);
	free(l);
}

static void statent_free(statent *s)
{
	entry_free(&s->obj);
	free(s->path);
	free(s);
}

void ncfs_flush_cache(ncfs *fs)
{
	unsigned i;

	for (i = 0; i < NBUCKETS; i++) {
		while (fs->lists[i] != NULL) {
			listing *l = fs->lists[i];
			fs->lists[i] = l->next;
			listing_free(l);
		}
		while (fs->stats[i] != NULL) {
			statent *s = fs->stats[i];
			fs->stats[i] = s->next;
			statent_free(s);
		}
	}
	fs->nlists = 0;
	fs->nstats = 0;
	fs->quota_when = 0;
}

static void purge_stale(ncfs *fs)
{
	unsigned i;

	for (i = 0; i < NBUCKETS; i++) {
		listing **lp = &fs->lists[i];
		statent **sp = &fs->stats[i];

		while (*lp != NULL) {
			if (!fresh(fs, (*lp)->when)) {
				listing *l = *lp;
				*lp = l->next;
				listing_free(l);
				fs->nlists--;
			} else {
				lp = &(*lp)->next;
			}
		}
		while (*sp != NULL) {
			if (!fresh(fs, (*sp)->when)) {
				statent *s = *sp;
				*sp = s->next;
				statent_free(s);
				fs->nstats--;
			} else {
				sp = &(*sp)->next;
			}
		}
	}
	if (fs->nlists > MAX_LISTINGS || fs->nstats > MAX_STATS)
		ncfs_flush_cache(fs);
}

static listing *list_find(ncfs *fs, const char *path)
{
	listing *l;

	for (l = fs->lists[hash(path)]; l != NULL; l = l->next) {
		if (strcmp(l->path, path) == 0)
			return fresh(fs, l->when) && plat_ms() - l->fetched < LIST_MAX_MS ?
			       l : NULL;
	}
	return NULL;
}

/* Our own change keeps a listing valid a little longer, so that copying
 * many files into a folder does not read the growing folder again and
 * again. Changes made elsewhere still show up within LIST_MAX_MS. */
static void list_touch(listing *l)
{
	l->when = plat_ms();
}

static void list_remove(ncfs *fs, const char *path)
{
	listing **lp = &fs->lists[hash(path)];

	while (*lp != NULL) {
		if (strcmp((*lp)->path, path) == 0) {
			listing *l = *lp;
			*lp = l->next;
			listing_free(l);
			fs->nlists--;
			return;
		}
		lp = &(*lp)->next;
	}
}

static statent *stat_find(ncfs *fs, const char *path)
{
	statent *s;

	for (s = fs->stats[hash(path)]; s != NULL; s = s->next) {
		if (strcmp(s->path, path) == 0) {
			if (strcmp(path, "/") == 0)
				return plat_ms() - s->when < ROOT_MS ? s : NULL;
			return fresh(fs, s->when) ? s : NULL;
		}
	}
	return NULL;
}

static void stat_remove(ncfs *fs, const char *path)
{
	statent **sp = &fs->stats[hash(path)];

	while (*sp != NULL) {
		if (strcmp((*sp)->path, path) == 0) {
			statent *s = *sp;
			*sp = s->next;
			statent_free(s);
			fs->nstats--;
			return;
		}
		sp = &(*sp)->next;
	}
}

static void stat_store(ncfs *fs, const char *path, const entry *obj)
{
	statent *s;
	unsigned h;

	stat_remove(fs, path);
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return;
	s->path = xstrdup(path);
	if (s->path == NULL) {
		free(s);
		return;
	}
	s->when = plat_ms();
	if (obj != NULL) {
		s->exists = 1;
		s->obj.st = obj->st;
		s->obj.name = xstrdup(path_basename(path));
		s->obj.comment = obj->comment != NULL ? xstrdup(obj->comment) : NULL;
	}
	h = hash(path);
	s->next = fs->stats[h];
	fs->stats[h] = s;
	fs->nstats++;
}

static entry *list_entry(listing *l, const char *name)
{
	size_t i;

	for (i = 0; i < l->n; i++) {
		if (strcmp(l->e[i].name, name) == 0)
			return &l->e[i];
	}
	return NULL;
}

static void list_put(listing *l, const char *name, const ncfs_stat *st,
                     const char *comment, int keep_comment)
{
	entry *e = list_entry(l, name);

	if (e == NULL) {
		if (l->n == l->cap) {
			size_t ncap = l->cap ? l->cap * 2 : 16;
			entry *ne = realloc(l->e, ncap * sizeof(*ne));
			if (ne == NULL)
				return;
			l->e = ne;
			l->cap = ncap;
		}
		e = &l->e[l->n];
		memset(e, 0, sizeof(*e));
		e->name = xstrdup(name);
		if (e->name == NULL)
			return;
		l->n++;
	}
	e->st = *st;
	if (!keep_comment) {
		free(e->comment);
		e->comment = comment != NULL ? xstrdup(comment) : NULL;
	}
}

static void list_del(listing *l, const char *name)
{
	size_t i;

	for (i = 0; i < l->n; i++) {
		if (strcmp(l->e[i].name, name) == 0) {
			entry_free(&l->e[i]);
			memmove(&l->e[i], &l->e[i + 1], (l->n - i - 1) * sizeof(entry));
			l->n--;
			return;
		}
	}
}

/* Records that path now exists with st (after our own change). */
static void cache_set(ncfs *fs, const char *path, const ncfs_stat *st,
                      const char *comment, int keep_comment)
{
	char *parent = path_parent(path);
	listing *l = parent != NULL ? list_find(fs, parent) : NULL;
	statent *s = stat_find(fs, path);
	const char *src = comment;
	entry obj;

	if (keep_comment) {
		src = NULL;
		if (s != NULL && s->exists)
			src = s->obj.comment;
		else if (l != NULL) {
			entry *e = list_entry(l, path_basename(path));
			if (e != NULL)
				src = e->comment;
		}
	}
	memset(&obj, 0, sizeof(obj));
	obj.st = *st;
	/* copy first: stat_store() frees the old cache entry */
	obj.comment = src != NULL ? xstrdup(src) : NULL;
	if (l != NULL) {
		list_put(l, path_basename(path), st, comment, keep_comment);
		list_touch(l);
	}
	stat_store(fs, path, &obj);
	free(obj.comment);
	free(parent);
	fs->quota_when = 0;
}

/* Forgets path and everything below it, and records that it is gone. */
static void cache_del(ncfs *fs, const char *path)
{
	char *parent = path_parent(path);
	listing *l = parent != NULL ? list_find(fs, parent) : NULL;
	unsigned i;

	if (l != NULL) {
		list_del(l, path_basename(path));
		list_touch(l);
	}
	free(parent);
	for (i = 0; i < NBUCKETS; i++) {
		listing **lp = &fs->lists[i];
		statent **sp = &fs->stats[i];

		while (*lp != NULL) {
			if (path_is_under((*lp)->path, path)) {
				listing *x = *lp;
				*lp = x->next;
				listing_free(x);
				fs->nlists--;
			} else {
				lp = &(*lp)->next;
			}
		}
		while (*sp != NULL) {
			if (path_is_under((*sp)->path, path)) {
				statent *x = *sp;
				*sp = x->next;
				statent_free(x);
				fs->nstats--;
			} else {
				sp = &(*sp)->next;
			}
		}
	}
	stat_store(fs, path, NULL);
	fs->quota_when = 0;
}

static void to_stat(const dav_item *it, ncfs_stat *st)
{
	st->is_dir = it->is_dir;
	st->size = it->size;
	st->mtime = it->mtime;
	st->readonly = it->readonly;
}

/* Reads a directory from the server into the cache. */
static int list_fetch(ncfs *fs, const char *path, listing **out)
{
	dav_list dl;
	listing *l;
	size_t i;
	unsigned h;
	int err;

	*out = NULL;
	err = dav_propfind(fs->dav, path, 1, &dl);
	if (err == -ENOENT)
		stat_store(fs, path, NULL);
	if (err != 0)
		return err;

	purge_stale(fs);
	list_remove(fs, path);
	l = calloc(1, sizeof(*l));
	if (l == NULL || (l->path = xstrdup(path)) == NULL) {
		free(l);
		dav_list_free(&dl);
		return -ENOMEM;
	}
	l->when = plat_ms();
	l->fetched = l->when;
	for (i = 0; i < dl.count; i++) {
		ncfs_stat st;

		to_stat(&dl.items[i], &st);
		if (dl.items[i].is_self) {
			entry self;
			if (!st.is_dir) {
				/* asked to list a file */
				memset(&self, 0, sizeof(self));
				self.st = st;
				stat_store(fs, path, &self);
				listing_free(l);
				dav_list_free(&dl);
				return -ENOTDIR;
			}
			memset(&self, 0, sizeof(self));
			self.st = st;
			self.comment = dl.items[i].comment;
			stat_store(fs, path, &self);
			if (strcmp(path, "/") == 0) {
				fs->quota_used = dl.quota_used;
				fs->quota_avail = dl.quota_avail;
				fs->quota_when = plat_ms();
			}
			continue;
		}
		list_put(l, dl.items[i].name, &st, dl.items[i].comment, 0);
	}
	dav_list_free(&dl);
	h = hash(path);
	l->next = fs->lists[h];
	fs->lists[h] = l;
	fs->nlists++;
	*out = l;
	return 0;
}

static ncfs_file *open_file(ncfs *fs, const char *path)
{
	ncfs_file *f;

	for (f = fs->files; f != NULL; f = f->next) {
		if (f->tf != NULL && strcmp(f->path, path) == 0)
			return f;
	}
	return NULL;
}

static void file_stat(ncfs_file *f, ncfs_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->size = f->tf != NULL ? tmpf_size(f->tf) : f->size;
	st->mtime = f->set_mtime != 0 ? f->set_mtime : f->mtime;
}

static void check_trunc(ncfs *fs)
{
	if (fs->trunc_path != NULL && plat_ms() - fs->trunc_when > TRUNC_MS) {
		plat_log(LOG_INFO, "dropping unused truncate of %s\n", fs->trunc_path);
		free(fs->trunc_path);
		fs->trunc_path = NULL;
	}
}

/* Looks up one real path. *comment (optional) points into the cache. */
static int stat_lookup(ncfs *fs, const char *path, ncfs_stat *st, const char **comment)
{
	ncfs_file *f;
	char *parent;
	listing *l;
	statent *s;
	dav_list dl;
	entry obj;
	int err;

	if (comment != NULL)
		*comment = NULL;
	f = open_file(fs, path);
	if (f != NULL) {
		file_stat(f, st);
		return 0;
	}
	check_trunc(fs);

	if (strcmp(path, "/") != 0) {
		parent = path_parent(path);
		l = parent != NULL ? list_find(fs, parent) : NULL;
		free(parent);
		if (l != NULL) {
			entry *e = list_entry(l, path_basename(path));
			if (e == NULL)
				return -ENOENT;
			*st = e->st;
			if (comment != NULL)
				*comment = e->comment;
			goto found;
		}
	}
	s = stat_find(fs, path);
	if (s != NULL) {
		if (!s->exists)
			return -ENOENT;
		*st = s->obj.st;
		if (comment != NULL)
			*comment = s->obj.comment;
		goto found;
	}

	err = dav_propfind(fs->dav, path, 0, &dl);
	if (err == -ENOENT) {
		stat_store(fs, path, NULL);
		return err;
	}
	if (err != 0)
		return err;
	if (dl.count == 0) {
		dav_list_free(&dl);
		return -ENOENT;
	}
	memset(&obj, 0, sizeof(obj));
	to_stat(&dl.items[0], &obj.st);
	obj.comment = dl.items[0].comment;
	stat_store(fs, path, &obj);
	if (strcmp(path, "/") == 0) {
		fs->quota_used = dl.quota_used;
		fs->quota_avail = dl.quota_avail;
		fs->quota_when = plat_ms();
	}
	*st = obj.st;
	dav_list_free(&dl);
	if (comment != NULL) {
		s = stat_find(fs, path);
		*comment = s != NULL ? s->obj.comment : NULL;
	}
found:
	if (fs->trunc_path != NULL && strcmp(fs->trunc_path, path) == 0)
		st->size = 0;
	return 0;
}

/* Maps a path as typed on the Amiga to the name on the server. AmigaDOS
 * ignores case, the server does not, so a name that does not exist as
 * typed is looked up in the folder without regard to case.
 *
 * Returns 0 when the object exists (*real set, st filled), -ENOENT when
 * only the last part is missing (*real is where it would be created) or
 * another -errno (*real NULL). */
static int resolve(ncfs *fs, const char *path_in, char **real, ncfs_stat *st)
{
	char *norm, *cur, *p, *comp, *next;
	ncfs_stat cst;
	int err = 0;

	*real = NULL;
	norm = path_normalize(path_in);
	if (norm == NULL)
		return -ENOMEM;
	cur = xstrdup("/");
	if (cur == NULL) {
		free(norm);
		return -ENOMEM;
	}
	err = stat_lookup(fs, "/", &cst, NULL);
	if (err != 0)
		goto fail;

	p = norm + 1;
	while (*p != '\0') {
		char *cand;
		int last;

		comp = p;
		next = strchr(p, '/');
		if (next != NULL) {
			*next = '\0';
			p = next + 1;
		} else {
			p = comp + strlen(comp);
		}
		last = (*p == '\0');
		if (!cst.is_dir) {
			err = -ENOTDIR;
			goto fail;
		}
		cand = path_join(cur, comp);
		if (cand == NULL) {
			err = -ENOMEM;
			goto fail;
		}
		err = stat_lookup(fs, cand, &cst, NULL);
		if (err == -ENOENT) {
			/* try without regard to case */
			listing *l = list_find(fs, cur);
			ncfs_file *f;
			const char *match = NULL;
			size_t i;

			if (l == NULL) {
				err = list_fetch(fs, cur, &l);
				if (err != 0) {
					free(cand);
					goto fail;
				}
			}
			for (i = 0; i < l->n; i++) {
				if (name_casecmp(l->e[i].name, comp) == 0) {
					match = l->e[i].name;
					cst = l->e[i].st;
					break;
				}
			}
			for (f = fs->files; match == NULL && f != NULL; f = f->next) {
				char *fp = path_parent(f->path);
				if (f->tf != NULL && fp != NULL && strcmp(fp, cur) == 0 &&
				    name_casecmp(path_basename(f->path), comp) == 0) {
					match = path_basename(f->path);
					file_stat(f, &cst);
				}
				free(fp);
			}
			if (match == NULL) {
				err = -ENOENT;
				if (last) {
					free(cur);
					free(norm);
					*real = cand;
					return err;
				}
				free(cand);
				goto fail;
			}
			free(cand);
			cand = path_join(cur, match);
			if (cand == NULL) {
				err = -ENOMEM;
				goto fail;
			}
			err = 0;
		}
		if (err != 0) {
			free(cand);
			goto fail;
		}
		free(cur);
		cur = cand;
	}
	free(norm);
	*real = cur;
	if (st != NULL)
		*st = cst;
	return 0;

fail:
	free(cur);
	free(norm);
	return err;
}

/* --- setup ------------------------------------------------------------ */

ncfs *ncfs_new(dav_session *dav, const ncfs_options *opt)
{
	ncfs *fs = calloc(1, sizeof(*fs));

	if (fs == NULL)
		return NULL;
	fs->dav = dav;
	fs->opt = *opt;
	if (fs->opt.cache_ms <= 0)
		fs->opt.cache_ms = 5000;
	if (fs->opt.read_block == 0)
		fs->opt.read_block = 256 * 1024;
	if (fs->opt.read_max < fs->opt.read_block)
		fs->opt.read_max = 4 * 1024 * 1024;
	fs->quota_used = -1;
	fs->quota_avail = -1;
	return fs;
}

void ncfs_free(ncfs *fs)
{
	if (fs == NULL)
		return;
	while (fs->files != NULL)
		ncfs_release(fs, fs->files);
	ncfs_flush_cache(fs);
	free(fs->trunc_path);
	free(fs);
}

dav_session *ncfs_dav(ncfs *fs)
{
	return fs->dav;
}

/* --- metadata --------------------------------------------------------- */

int ncfs_getattr(ncfs *fs, const char *path, ncfs_stat *st)
{
	char *real;
	int err = resolve(fs, path, &real, st);

	free(real);
	return err;
}

int ncfs_readdir(ncfs *fs, const char *path, ncfs_fill_fn fill, void *ud)
{
	ncfs_stat st;
	listing *l;
	ncfs_file *f;
	char *real;
	size_t i;
	int err;

	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	if (!st.is_dir) {
		free(real);
		return -ENOTDIR;
	}
	l = list_find(fs, real);
	if (l == NULL) {
		err = list_fetch(fs, real, &l);
		if (err != 0) {
			free(real);
			return err;
		}
	}
	for (i = 0; i < l->n; i++) {
		ncfs_stat est = l->e[i].st;
		char *full = path_join(real, l->e[i].name);

		if (full != NULL) {
			ncfs_file *of = open_file(fs, full);
			if (of != NULL)
				file_stat(of, &est);
			free(full);
		}
		if (fill(ud, l->e[i].name, &est) != 0)
			break;
	}
	/* files being written that are not on the server yet */
	for (f = fs->files; f != NULL; f = f->next) {
		char *fp;

		if (!f->is_new)
			continue;
		fp = path_parent(f->path);
		if (fp != NULL && strcmp(fp, real) == 0 &&
		    list_entry(l, path_basename(f->path)) == NULL) {
			ncfs_stat fst;
			file_stat(f, &fst);
			fill(ud, path_basename(f->path), &fst);
		}
		free(fp);
	}
	free(real);
	return 0;
}

int ncfs_statfs(ncfs *fs, int64_t *total, int64_t *avail)
{
	int64_t used, free_bytes;

	if (fs->quota_when == 0 || plat_ms() - fs->quota_when > QUOTA_MS) {
		dav_list dl;
		int err = dav_propfind(fs->dav, "/", 0, &dl);

		if (err != 0)
			return err;
		fs->quota_used = dl.quota_used;
		fs->quota_avail = dl.quota_avail;
		fs->quota_when = plat_ms();
		dav_list_free(&dl);
	}
	used = fs->quota_used > 0 ? fs->quota_used : 0;
	free_bytes = fs->quota_avail >= 0 ? fs->quota_avail : UNLIMITED;
	*total = used + free_bytes;
	*avail = free_bytes;
	return 0;
}

/* --- files ------------------------------------------------------------ */

static ncfs_file *file_new(ncfs *fs, const char *real, const ncfs_stat *st)
{
	ncfs_file *f = calloc(1, sizeof(*f));

	if (f == NULL)
		return NULL;
	f->path = xstrdup(real);
	if (f->path == NULL) {
		free(f);
		return NULL;
	}
	f->size = st->size;
	f->mtime = st->mtime;
	f->ro_obj = st->readonly;
	f->blk_want = fs->opt.read_block;
	f->next = fs->files;
	fs->files = f;
	return f;
}

static void file_unlink(ncfs *fs, ncfs_file *f)
{
	ncfs_file **fp;

	for (fp = &fs->files; *fp != NULL; fp = &(*fp)->next) {
		if (*fp == f) {
			*fp = f->next;
			return;
		}
	}
}

/* Switches a file to writing through a temp file. */
static int make_writable(ncfs *fs, ncfs_file *f, int download)
{
	tmpf *tf;
	int err = 0;

	if (f->tf != NULL)
		return 0;
	if (fs->opt.readonly || f->ro_obj)
		return -EROFS;
	if (open_file(fs, f->path) != NULL)
		return -EBUSY; /* another handle is already writing */
	tf = tmpf_create(fs->opt.tempdir, &err);
	if (tf == NULL)
		return err != 0 ? err : -EIO;
	if (download && f->size > 0) {
		plat_log(LOG_DEBUG, "downloading %s for writing\n", f->path);
		err = dav_get_file(fs->dav, f->path, tf);
		if (err != 0) {
			tmpf_close(tf);
			return err;
		}
		f->size = tmpf_size(tf);
	}
	f->tf = tf;
	free(f->blk);
	f->blk = NULL;
	f->blk_cap = 0;
	f->blk_len = 0;
	return 0;
}

int ncfs_open(ncfs *fs, const char *path, int truncate, ncfs_file **fh)
{
	ncfs_stat st;
	ncfs_file *f;
	char *real;
	int err;

	*fh = NULL;
	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	if (st.is_dir) {
		free(real);
		return -EISDIR;
	}
	if (fs->trunc_path != NULL && strcmp(fs->trunc_path, real) == 0) {
		truncate = 1;
		free(fs->trunc_path);
		fs->trunc_path = NULL;
	}
	f = file_new(fs, real, &st);
	free(real);
	if (f == NULL)
		return -ENOMEM;
	if (truncate) {
		err = make_writable(fs, f, 0);
		if (err != 0) {
			file_unlink(fs, f);
			free(f->path);
			free(f);
			return err;
		}
		f->dirty = 1;
		f->size = 0;
	}
	*fh = f;
	return 0;
}

int ncfs_create(ncfs *fs, const char *path, ncfs_file **fh)
{
	ncfs_stat st;
	ncfs_file *f;
	char *real;
	int err;

	*fh = NULL;
	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err == 0) {
		free(real);
		if (st.is_dir)
			return -EISDIR;
		return ncfs_open(fs, path, 1, fh);
	}
	if (err != -ENOENT || real == NULL) {
		free(real);
		return err;
	}
	memset(&st, 0, sizeof(st));
	st.mtime = plat_time();
	f = file_new(fs, real, &st);
	free(real);
	if (f == NULL)
		return -ENOMEM;
	err = make_writable(fs, f, 0);
	if (err != 0) {
		file_unlink(fs, f);
		free(f->path);
		free(f);
		return err;
	}
	f->is_new = 1;
	f->dirty = 1;
	*fh = f;
	return 0;
}

int ncfs_read(ncfs *fs, ncfs_file *f, char *buf, size_t size, int64_t off)
{
	size_t want, got, n;
	int err;

	if (f->tf != NULL)
		return tmpf_pread(f->tf, buf, size, off);
	if (off >= f->size || size == 0)
		return 0;
	if ((int64_t)size > f->size - off)
		size = (size_t)(f->size - off);

	if (f->blk != NULL && off >= f->blk_off &&
	    off + (int64_t)size <= f->blk_off + (int64_t)f->blk_len) {
		memcpy(buf, f->blk + (off - f->blk_off), size);
		f->next_off = off + (int64_t)size;
		return (int)size;
	}

	/* sequential reads get bigger requests, random reads small ones */
	if (off == f->next_off && f->blk != NULL) {
		if (f->blk_want < fs->opt.read_max)
			f->blk_want *= 2;
		if (f->blk_want > fs->opt.read_max)
			f->blk_want = fs->opt.read_max;
	} else {
		f->blk_want = fs->opt.read_block;
	}
	want = f->blk_want > size ? f->blk_want : size;
	if ((int64_t)want > f->size - off)
		want = (size_t)(f->size - off);
	if (want > f->blk_cap) {
		char *nb = realloc(f->blk, want);
		if (nb == NULL)
			return -ENOMEM;
		f->blk = nb;
		f->blk_cap = want;
	}
	err = dav_get_range(fs->dav, f->path, off, f->blk, want, &got, f->etag, sizeof(f->etag));
	if (err != 0) {
		f->blk_len = 0;
		return err;
	}
	f->blk_off = off;
	f->blk_len = got;
	n = got < size ? got : size;
	memcpy(buf, f->blk, n);
	f->next_off = off + (int64_t)n;
	return (int)n;
}

int ncfs_write(ncfs *fs, ncfs_file *f, const char *buf, size_t size, int64_t off)
{
	int err, n;

	err = make_writable(fs, f, 1);
	if (err != 0)
		return err;
	n = tmpf_pwrite(f->tf, buf, size, off);
	if (n < 0)
		return n;
	f->dirty = 1;
	f->written = 1;
	return n;
}

int ncfs_file_written(ncfs_file *f)
{
	return f->tf != NULL && f->dirty && f->written;
}

int ncfs_fgetattr(ncfs *fs, ncfs_file *f, ncfs_stat *st)
{
	(void)fs;
	file_stat(f, st);
	st->readonly = f->ro_obj;
	return 0;
}

int ncfs_ftruncate(ncfs *fs, ncfs_file *f, int64_t size)
{
	int err = make_writable(fs, f, size > 0);

	if (err != 0)
		return err;
	err = tmpf_truncate(f->tf, size);
	if (err == 0) {
		f->dirty = 1;
		f->written = 1;
	}
	return err;
}

static int upload(ncfs *fs, ncfs_file *f)
{
	ncfs_stat st;
	int err;

	plat_log(LOG_DEBUG, "uploading %s (%lld bytes)\n", f->path,
	         (long long)tmpf_size(f->tf));
	err = dav_put_file(fs->dav, f->path, f->tf, f->set_mtime);
	if (err != 0)
		return err;
	f->dirty = 0;
	f->is_new = 0;
	memset(&st, 0, sizeof(st));
	st.size = tmpf_size(f->tf);
	st.mtime = f->set_mtime != 0 ? f->set_mtime : plat_time();
	if (f->set_mtime != 0 && !dav_is_nextcloud(fs->dav))
		st.mtime = plat_time();
	f->mtime = st.mtime;
	cache_set(fs, f->path, &st, NULL, 1);
	return 0;
}

int ncfs_flush(ncfs *fs, ncfs_file *f)
{
	if (f->tf == NULL || !f->dirty)
		return 0;
	return upload(fs, f);
}

int ncfs_release(ncfs *fs, ncfs_file *f)
{
	int err = 0;

	file_unlink(fs, f);
	if (f->tf != NULL && f->dirty) {
		err = upload(fs, f);
		if (err != 0) {
			char kept[512], name[128], msg[1800];
			const char *base = path_basename(f->path);
			int kerr;

			snprintf(name, sizeof(name), "%d_%s", ++fs->keep_count,
			         base[0] != '\0' ? base : "file");
			kerr = fs->opt.keepdir != NULL ?
			       tmpf_keep(f->tf, fs->opt.keepdir, name, kept, sizeof(kept)) :
			       -ENOENT;
			char shown[600];

			if (fs->opt.display != NULL)
				snprintf(shown, sizeof(shown), "%s%s", fs->opt.display, f->path + 1);
			else
				snprintf(shown, sizeof(shown), "%s", f->path);
			if (kerr == 0) {
				snprintf(msg, sizeof(msg),
				         "Could not save \"%s\" on the server.\n%s\n\n"
				         "The data was kept in\n%s",
				         shown, dav_error(fs->dav), kept);
				f->tf = NULL; /* now owned by the kept file */
			} else {
				snprintf(msg, sizeof(msg),
				         "Could not save \"%s\" on the server.\n%s\n\n"
				         "The data could not be kept either.",
				         shown, dav_error(fs->dav));
			}
			plat_report("Upload failed", msg);
		}
	}
	if (f->tf != NULL)
		tmpf_close(f->tf);
	free(f->blk);
	free(f->path);
	free(f);
	return err;
}

int ncfs_truncate(ncfs *fs, const char *path, int64_t size)
{
	ncfs_stat st;
	ncfs_file *f;
	char *real;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	if (st.is_dir) {
		free(real);
		return -EISDIR;
	}
	f = open_file(fs, real);
	if (f != NULL) {
		free(real);
		return ncfs_ftruncate(fs, f, size);
	}
	if (size == 0) {
		/* filesysbox truncates before it opens a file with MODE_NEWFILE:
		 * remember it, the new content is uploaded when it is closed. */
		free(fs->trunc_path);
		fs->trunc_path = real;
		fs->trunc_when = plat_ms();
		return 0;
	}
	/* rare: change the size of a closed file */
	err = ncfs_open(fs, real, 0, &f);
	free(real);
	if (err != 0)
		return err;
	err = ncfs_ftruncate(fs, f, size);
	if (err == 0)
		err = ncfs_flush(fs, f);
	ncfs_release(fs, f);
	return err;
}

/* --- directories and names ---------------------------------------------- */

int ncfs_mkdir(ncfs *fs, const char *path)
{
	ncfs_stat st;
	listing *l;
	char *real;
	unsigned h;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err == 0) {
		free(real);
		return -EEXIST;
	}
	if (err != -ENOENT || real == NULL) {
		free(real);
		return err;
	}
	err = dav_mkcol(fs->dav, real);
	if (err == 0) {
		memset(&st, 0, sizeof(st));
		st.is_dir = 1;
		st.mtime = plat_time();
		cache_set(fs, real, &st, NULL, 0);
		/* the new folder is empty */
		list_remove(fs, real);
		l = calloc(1, sizeof(*l));
		if (l != NULL && (l->path = xstrdup(real)) != NULL) {
			l->when = plat_ms();
			l->fetched = l->when;
			h = hash(real);
			l->next = fs->lists[h];
			fs->lists[h] = l;
			fs->nlists++;
		} else {
			free(l);
		}
	}
	free(real);
	return err;
}

int ncfs_unlink(ncfs *fs, const char *path)
{
	ncfs_stat st;
	char *real;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	if (st.is_dir) {
		free(real);
		return -EISDIR;
	}
	if (open_file(fs, real) != NULL) {
		free(real);
		return -EBUSY;
	}
	err = dav_delete(fs->dav, real);
	if (err == 0 || err == -ENOENT) {
		cache_del(fs, real);
		err = 0;
	}
	free(real);
	return err;
}

int ncfs_rmdir(ncfs *fs, const char *path)
{
	ncfs_stat st;
	listing *l;
	ncfs_file *f;
	char *real;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	if (!st.is_dir) {
		free(real);
		return -ENOTDIR;
	}
	if (strcmp(real, "/") == 0) {
		free(real);
		return -EBUSY;
	}
	/* The server would delete a folder with everything in it; AmigaDOS
	 * only deletes empty ones. Ask the server, not the cache. */
	err = list_fetch(fs, real, &l);
	if (err != 0) {
		free(real);
		return err;
	}
	if (l->n > 0) {
		free(real);
		return -ENOTEMPTY;
	}
	for (f = fs->files; f != NULL; f = f->next) {
		if (path_is_under(f->path, real) && strcmp(f->path, real) != 0) {
			free(real);
			return -ENOTEMPTY;
		}
	}
	err = dav_delete(fs->dav, real);
	if (err == 0 || err == -ENOENT) {
		cache_del(fs, real);
		err = 0;
	}
	free(real);
	return err;
}

int ncfs_rename(ncfs *fs, const char *from, const char *to)
{
	ncfs_stat st, tst;
	ncfs_file *f;
	char *rfrom = NULL, *rto = NULL, *tparent = NULL;
	const char *comment = NULL;
	char *comment_copy = NULL;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, from, &rfrom, &st);
	if (err != 0)
		goto out;
	if (strcmp(rfrom, "/") == 0) {
		err = -EBUSY;
		goto out;
	}
	err = resolve(fs, to, &rto, &tst);
	if (err == 0) {
		if (strcmp(rto, rfrom) == 0) {
			/* only the case of the name changes, or nothing at all */
			char *norm = path_normalize(to);
			tparent = path_parent(rto);
			free(rto);
			rto = (norm != NULL && tparent != NULL) ?
			      path_join(tparent, path_basename(norm)) : NULL;
			free(norm);
			if (rto == NULL) {
				err = -ENOMEM;
				goto out;
			}
			if (strcmp(rto, rfrom) == 0)
				goto out; /* same name */
		} else {
			err = -EEXIST;
			goto out;
		}
	} else if (err != -ENOENT || rto == NULL) {
		goto out;
	}
	if (st.is_dir && path_is_under(rto, rfrom)) {
		err = -EINVAL;
		goto out;
	}

	f = open_file(fs, rfrom);
	if (f != NULL && f->is_new) {
		/* not on the server yet: only the name changes */
		free(f->path);
		f->path = xstrdup(rto);
		err = f->path != NULL ? 0 : -ENOMEM;
		goto out;
	}

	stat_lookup(fs, rfrom, &st, &comment);
	if (comment != NULL)
		comment_copy = xstrdup(comment);
	err = dav_move(fs->dav, rfrom, rto, 0);
	if (err == 0) {
		size_t flen = strlen(rfrom);

		cache_del(fs, rfrom);
		cache_set(fs, rto, &st, comment_copy, 0);
		/* open files below the old name move along */
		for (f = fs->files; f != NULL; f = f->next) {
			if (path_is_under(f->path, rfrom)) {
				char *np = path_join(rto, f->path + flen + (f->path[flen] == '/'));
				if (f->path[flen] == '\0') {
					free(np);
					np = xstrdup(rto);
				}
				if (np != NULL) {
					free(f->path);
					f->path = np;
				}
			}
		}
	}
out:
	free(comment_copy);
	free(tparent);
	free(rfrom);
	free(rto);
	return err;
}

int ncfs_utimens(ncfs *fs, const char *path, time_t mtime)
{
	ncfs_stat st;
	ncfs_file *f;
	char *real;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	f = open_file(fs, real);
	if (f != NULL && f->dirty) {
		/* sent along with the upload */
		f->set_mtime = mtime;
		free(real);
		return 0;
	}
	err = dav_set_mtime(fs->dav, real, mtime);
	if (err == 0) {
		st.mtime = mtime;
		cache_set(fs, real, &st, NULL, 1);
	}
	free(real);
	return err;
}

int ncfs_getcomment(ncfs *fs, const char *path, char *buf, size_t size)
{
	ncfs_stat st;
	const char *c = NULL;
	char *real;
	size_t n;
	int err;

	err = resolve(fs, path, &real, &st);
	if (err == 0)
		err = stat_lookup(fs, real, &st, &c);
	free(real);
	if (err != 0)
		return err;
	if (c == NULL || c[0] == '\0')
		return -ENODATA;
	n = strlen(c);
	if (size == 0)
		return (int)n;
	if (n > size)
		n = size;
	memcpy(buf, c, n);
	return (int)n;
}

int ncfs_setcomment(ncfs *fs, const char *path, const char *comment)
{
	ncfs_stat st;
	char *real;
	int err;

	if (fs->opt.readonly)
		return -EROFS;
	err = resolve(fs, path, &real, &st);
	if (err != 0) {
		free(real);
		return err;
	}
	err = dav_set_comment(fs->dav, real, comment);
	if (err == 0)
		cache_set(fs, real, &st, comment, 0);
	free(real);
	return err;
}
