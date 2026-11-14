/* nextcloud-handler - Linux FUSE front end of the core, used to test the
 * core quickly against a real server. It is not part of the AmigaOS
 * release. Public domain.
 *
 *   nextcloud-fuse --url URL --user NAME [options] MOUNTPOINT [fuse options]
 *
 * The password comes from the environment variable NCH_PASSWORD.
 */
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <linux/fs.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ncfs.h"
#include "platform.h"

#define COMMENT_XATTR "user.amiga_comment"

static ncfs *g_fs;

static ncfs_file *FH(struct fuse_file_info *fi)
{
	return fi != NULL ? (ncfs_file *)(uintptr_t)fi->fh : NULL;
}

static void fill_stat(const ncfs_stat *ns, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	if (ns->is_dir) {
		st->st_mode = S_IFDIR | (ns->readonly ? 0555 : 0755);
		st->st_nlink = 2;
	} else {
		st->st_mode = S_IFREG | (ns->readonly ? 0444 : 0644);
		st->st_nlink = 1;
	}
	st->st_size = (off_t)ns->size;
	st->st_mtime = ns->mtime;
	st->st_atime = ns->mtime;
	st->st_ctime = ns->mtime;
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_blksize = 4096;
	st->st_blocks = (st->st_size + 511) / 512;
}

static int op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	ncfs_stat ns;
	int err = FH(fi) != NULL ? ncfs_fgetattr(g_fs, FH(fi), &ns) : ncfs_getattr(g_fs, path, &ns);

	if (err == 0)
		fill_stat(&ns, st);
	return err;
}

struct fill_ctx {
	void            *buf;
	fuse_fill_dir_t  filler;
};

static int fill_one(void *ud, const char *name, const ncfs_stat *ns)
{
	struct fill_ctx *c = ud;
	struct stat st;

	fill_stat(ns, &st);
	return c->filler(c->buf, name, &st, 0, 0);
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
                      struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	struct fill_ctx c = { buf, filler };

	(void)off;
	(void)fi;
	(void)flags;
	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	return ncfs_readdir(g_fs, path, fill_one, &c);
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
	ncfs_file *f;
	int err = ncfs_open(g_fs, path, (fi->flags & O_TRUNC) != 0, &f);

	if (err == 0)
		fi->fh = (uint64_t)(uintptr_t)f;
	return err;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	ncfs_file *f;
	int err;

	(void)mode;
	err = ncfs_create(g_fs, path, &f);
	if (err == 0)
		fi->fh = (uint64_t)(uintptr_t)f;
	return err;
}

static int op_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi)
{
	(void)path;
	return ncfs_read(g_fs, FH(fi), buf, size, off);
}

static int op_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi)
{
	(void)path;
	return ncfs_write(g_fs, FH(fi), buf, size, off);
}

static int op_flush(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	/* close() waits for flush but not for release, so upload here to make
	 * close() wait for the upload, as Close() does with filesysbox. Linux
	 * also flushes when a duplicated descriptor is closed (the shell does
	 * that for "echo >file"), so wait until data was written; release
	 * uploads the rest. */
	if (!ncfs_file_written(FH(fi)))
		return 0;
	return ncfs_flush(g_fs, FH(fi));
}

static int op_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	(void)datasync;
	return ncfs_flush(g_fs, FH(fi));
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	ncfs_release(g_fs, FH(fi));
	return 0;
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	if (FH(fi) != NULL)
		return ncfs_ftruncate(g_fs, FH(fi), size);
	return ncfs_truncate(g_fs, path, size);
}

static int op_mkdir(const char *path, mode_t mode)
{
	(void)mode;
	return ncfs_mkdir(g_fs, path);
}

static int op_unlink(const char *path)
{
	return ncfs_unlink(g_fs, path);
}

static int op_rmdir(const char *path)
{
	return ncfs_rmdir(g_fs, path);
}

static int op_rename(const char *from, const char *to, unsigned int flags)
{
	if (flags & RENAME_EXCHANGE)
		return -EINVAL;
	return ncfs_rename(g_fs, from, to);
}

static int op_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi)
{
	time_t mtime;

	(void)fi;
	if (tv == NULL || tv[1].tv_nsec == UTIME_NOW)
		mtime = time(NULL);
	else if (tv[1].tv_nsec == UTIME_OMIT)
		return 0;
	else
		mtime = tv[1].tv_sec;
	return ncfs_utimens(g_fs, path, mtime);
}

static int op_statfs(const char *path, struct statvfs *sv)
{
	int64_t total, avail;
	int err;

	(void)path;
	err = ncfs_statfs(g_fs, &total, &avail);
	if (err != 0)
		return err;
	memset(sv, 0, sizeof(*sv));
	sv->f_bsize = 4096;
	sv->f_frsize = 4096;
	sv->f_blocks = (fsblkcnt_t)(total / 4096);
	sv->f_bfree = (fsblkcnt_t)(avail / 4096);
	sv->f_bavail = sv->f_bfree;
	sv->f_namemax = 255;
	return 0;
}

static int op_getxattr(const char *path, const char *name, char *value, size_t size)
{
	if (strcmp(name, COMMENT_XATTR) != 0)
		return -ENODATA;
	return ncfs_getcomment(g_fs, path, value, size);
}

static int op_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
	char buf[256];

	(void)flags;
	if (strcmp(name, COMMENT_XATTR) != 0)
		return -ENOTSUP;
	if (size >= sizeof(buf))
		return -E2BIG;
	memcpy(buf, value, size);
	buf[size] = '\0';
	return ncfs_setcomment(g_fs, path, buf);
}

static int op_removexattr(const char *path, const char *name)
{
	if (strcmp(name, COMMENT_XATTR) != 0)
		return -ENODATA;
	return ncfs_setcomment(g_fs, path, NULL);
}

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->attr_timeout = 0;
	cfg->entry_timeout = 0;
	cfg->negative_timeout = 0;
	cfg->direct_io = 1;
	return NULL;
}

static const struct fuse_operations ops = {
	.init        = op_init,
	.getattr     = op_getattr,
	.readdir     = op_readdir,
	.open        = op_open,
	.create      = op_create,
	.read        = op_read,
	.write       = op_write,
	.flush       = op_flush,
	.fsync       = op_fsync,
	.release     = op_release,
	.truncate    = op_truncate,
	.mkdir       = op_mkdir,
	.unlink      = op_unlink,
	.rmdir       = op_rmdir,
	.rename      = op_rename,
	.utimens     = op_utimens,
	.statfs      = op_statfs,
	.getxattr    = op_getxattr,
	.setxattr    = op_setxattr,
	.removexattr = op_removexattr,
};

static void usage(void)
{
	fprintf(stderr,
	        "usage: nextcloud-fuse --url URL [--user NAME] [--cafile FILE] [--insecure]\n"
	        "         [--cache-ms N] [--tempdir DIR] [--keepdir DIR] [--debug]\n"
	        "         MOUNTPOINT [fuse options]\n"
	        "The password is read from the environment variable NCH_PASSWORD.\n");
}

int main(int argc, char **argv)
{
	dav_config dc;
	ncfs_options fo;
	dav_session *dav;
	char *fargv[64];
	int fargc = 0, i, err, rc;

	memset(&dc, 0, sizeof(dc));
	memset(&fo, 0, sizeof(fo));
	dc.password = getenv("NCH_PASSWORD");
	dc.user_agent = "nextcloud-handler-test (Linux)";
	fo.tempdir = "/tmp";
	fo.keepdir = "/tmp/nch-kept";

	fargv[fargc++] = argv[0];
	fargv[fargc++] = "-s"; /* the core is single-threaded, like filesysbox */
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(a, "--url") == 0 && v) { dc.url = v; i++; }
		else if (strcmp(a, "--user") == 0 && v) { dc.user = v; i++; }
		else if (strcmp(a, "--cafile") == 0 && v) { dc.ca_file = v; i++; }
		else if (strcmp(a, "--insecure") == 0) { dc.insecure = 1; }
		else if (strcmp(a, "--cache-ms") == 0 && v) { fo.cache_ms = atoi(v); i++; }
		else if (strcmp(a, "--tempdir") == 0 && v) { fo.tempdir = v; i++; }
		else if (strcmp(a, "--keepdir") == 0 && v) { fo.keepdir = v; i++; }
		else if (strcmp(a, "--readonly") == 0) { fo.readonly = 1; }
		else if (strcmp(a, "--debug") == 0) { plat_loglevel = LOG_DEBUG; }
		else if (strcmp(a, "--verbose") == 0) { plat_loglevel = LOG_INFO; }
		else if (fargc < 60) { fargv[fargc++] = argv[i]; }
	}
	fargv[fargc] = NULL;
	if (dc.url == NULL || fargc < 3) {
		usage();
		return 2;
	}

	dav = dav_new(&dc);
	if (dav == NULL) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	err = dav_connect(dav);
	if (err != 0) {
		fprintf(stderr, "cannot connect: %s (%s)\n", dav_error(dav), strerror(-err));
		dav_free(dav);
		return 1;
	}
	g_fs = ncfs_new(dav, &fo);
	rc = fuse_main(fargc, fargv, &ops, NULL);
	ncfs_free(g_fs);
	dav_free(dav);
	return rc;
}
