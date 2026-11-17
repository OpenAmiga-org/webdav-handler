/* nextcloud-handler - the volume the handler shows: the root lists the
 * servers of the server list (servers.h), each server is a folder that
 * shows the files of that account. A server is connected when its folder
 * is first used. The OS4 (filesysbox) and Linux (FUSE) front ends call
 * these. Paths are UTF-8, "/" is the root. Errors are -errno.
 * Public domain. */
#ifndef NCH_NCROOT_H
#define NCH_NCROOT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "ncfs.h"

typedef struct {
	const char *config_dir;   /* servers.xml: "ENV:NextcloudHandler" */
	const char *save_dir;     /* remembered passwords also go here
	                             ("ENVARC:NextcloudHandler"), may be NULL */
	const char *icon_file;    /* drawer icon shown for each server, NULL = none */
	const char *tempdir;      /* upload buffers */
	const char *keepdir;      /* failed uploads are saved here */
	const char *display;      /* shown before paths in messages: "Nextcloud:" */
	const char *ca_path;      /* trusted certificates, may be NULL */
	const char *proxy;        /* "host:port", may be NULL */
	const char *user_agent;
	int         cache_ms;     /* how long server listings are trusted */
	long        connect_timeout;  /* seconds */
	long        io_timeout;       /* seconds without progress */
	int64_t     chunk_size;   /* Nextcloud chunked upload above this, 0 = 16 MB */
	int         readonly;     /* refuse all changes */
} ncroot_options;

typedef struct ncroot ncroot;
typedef struct ncroot_file ncroot_file;

ncroot *ncroot_new(const ncroot_options *opt);
void    ncroot_free(ncroot *r);

int ncroot_getattr(ncroot *r, const char *path, ncfs_stat *st);
int ncroot_readdir(ncroot *r, const char *path, ncfs_fill_fn fill, void *ud);
int ncroot_statfs(ncroot *r, const char *path, int64_t *total, int64_t *avail);

int ncroot_open(ncroot *r, const char *path, int truncate, ncroot_file **fh);
int ncroot_create(ncroot *r, const char *path, ncroot_file **fh);
int ncroot_read(ncroot *r, ncroot_file *fh, char *buf, size_t size, int64_t off);
int ncroot_write(ncroot *r, ncroot_file *fh, const char *buf, size_t size, int64_t off);
int ncroot_fgetattr(ncroot *r, ncroot_file *fh, ncfs_stat *st);
int ncroot_ftruncate(ncroot *r, ncroot_file *fh, int64_t size);
int ncroot_flush(ncroot *r, ncroot_file *fh);
int ncroot_file_written(ncroot_file *fh);
int ncroot_release(ncroot *r, ncroot_file *fh);

int ncroot_truncate(ncroot *r, const char *path, int64_t size);
int ncroot_mkdir(ncroot *r, const char *path);
int ncroot_unlink(ncroot *r, const char *path);
int ncroot_rmdir(ncroot *r, const char *path);
int ncroot_rename(ncroot *r, const char *from, const char *to);
int ncroot_utimens(ncroot *r, const char *path, time_t mtime);
int ncroot_getcomment(ncroot *r, const char *path, char *buf, size_t size);
int ncroot_setcomment(ncroot *r, const char *path, const char *comment);

/* "DiskChange": reads the server list again, asks again for passwords
 * that were cancelled and shows connection problems again. */
void ncroot_reset(ncroot *r);
/* Number of connected servers. */
int  ncroot_connected(ncroot *r);

#endif
