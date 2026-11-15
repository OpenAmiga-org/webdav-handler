/* nextcloud-handler - filesystem operations on top of WebDAV, with a
 * metadata cache. The OS4 (filesysbox) and Linux (FUSE) front ends call
 * these. Paths are UTF-8, "/" is the root. Errors are -errno.
 * Public domain. */
#ifndef NCH_NCFS_H
#define NCH_NCFS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "davclient.h"

typedef struct {
	int     is_dir;
	int64_t size;
	time_t  mtime;       /* UTC */
	int     readonly;
} ncfs_stat;

typedef struct {
	const char *tempdir;     /* upload buffers */
	const char *keepdir;     /* failed uploads are saved here */
	int         cache_ms;    /* how long server listings are trusted */
	int         readonly;    /* refuse all changes */
	size_t      read_block;  /* smallest range request */
	size_t      read_max;    /* largest range request */
	const char *display;     /* shown before paths in messages, e.g. "Nextcloud:" */
} ncfs_options;

typedef struct ncfs ncfs;
typedef struct ncfs_file ncfs_file;

typedef int (*ncfs_fill_fn)(void *ud, const char *name, const ncfs_stat *st);

ncfs *ncfs_new(dav_session *dav, const ncfs_options *opt);
void  ncfs_free(ncfs *fs);
dav_session *ncfs_dav(ncfs *fs);

int ncfs_getattr(ncfs *fs, const char *path, ncfs_stat *st);
int ncfs_readdir(ncfs *fs, const char *path, ncfs_fill_fn fill, void *ud);
int ncfs_statfs(ncfs *fs, int64_t *total, int64_t *avail);

int ncfs_open(ncfs *fs, const char *path, int truncate, ncfs_file **fh);
int ncfs_create(ncfs *fs, const char *path, ncfs_file **fh);
int ncfs_read(ncfs *fs, ncfs_file *fh, char *buf, size_t size, int64_t off);
int ncfs_write(ncfs *fs, ncfs_file *fh, const char *buf, size_t size, int64_t off);
int ncfs_fgetattr(ncfs *fs, ncfs_file *fh, ncfs_stat *st);
int ncfs_ftruncate(ncfs *fs, ncfs_file *fh, int64_t size);
int ncfs_flush(ncfs *fs, ncfs_file *fh);      /* uploads pending data */
int ncfs_file_written(ncfs_file *fh);         /* data was written to it */
int ncfs_release(ncfs *fs, ncfs_file *fh);    /* uploads and frees */

int ncfs_truncate(ncfs *fs, const char *path, int64_t size);
int ncfs_mkdir(ncfs *fs, const char *path);
int ncfs_unlink(ncfs *fs, const char *path);
int ncfs_rmdir(ncfs *fs, const char *path);
int ncfs_rename(ncfs *fs, const char *from, const char *to);
int ncfs_utimens(ncfs *fs, const char *path, time_t mtime);

/* Returns the comment length, -ENODATA when there is none. */
int ncfs_getcomment(ncfs *fs, const char *path, char *buf, size_t size);
int ncfs_setcomment(ncfs *fs, const char *path, const char *comment);

/* Drops all cached server data (for example after a reconnect). */
void ncfs_flush_cache(ncfs *fs);

#endif
