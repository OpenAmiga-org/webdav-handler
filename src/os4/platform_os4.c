/* nextcloud-handler - platform functions for AmigaOS 4 (clib4).
 * Public domain. */
#include "platform.h"
#include "charset.h"
#include "os4.h"

#include <proto/exec.h>
#include <proto/dos.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int plat_loglevel = LOG_ERR;
int plat_debug_serial = 0;
int plat_charset = -1;   /* convert messages from UTF-8 when >= 0 */

struct tmpf {
	int     fd;
	int64_t pos;   /* file position, -1 when not known */
	char    path[512];
};

static unsigned tmp_counter;

tmpf *tmpf_create(const char *dir, int *err)
{
	tmpf *f = calloc(1, sizeof(*f));
	size_t n;
	int i;

	if (f == NULL) {
		*err = -ENOMEM;
		return NULL;
	}
	n = strlen(dir);
	for (i = 0; i < 100; i++) {
		snprintf(f->path, sizeof(f->path), "%s%snch_%lx_%u.tmp", dir,
		         (n > 0 && dir[n - 1] != ':' && dir[n - 1] != '/') ? "/" : "",
		         (unsigned long)IExec->FindTask(NULL), ++tmp_counter);
		f->fd = open(f->path, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (f->fd >= 0) {
			f->pos = 0;
			return f;
		}
		if (errno != EEXIST)
			break;
	}
	*err = -(errno != 0 ? errno : EIO);
	plat_log(LOG_ERR, "cannot create temp file %s\n", f->path);
	free(f);
	return NULL;
}

/* FFS lets a file grow past 2 GiB but cannot seek to a position at or
 * above it, and clib4's pread()/pwrite() seek on every call (there and
 * back). So the position is kept here and only changed when an access
 * does not continue where the last one ended: files are written and read
 * in order, which then works at any size. */
static int tmpf_seek(tmpf *f, int64_t off)
{
	struct stat st;

	if (f->pos == off)
		return 0;
	f->pos = -1;
	/* AmigaDOS cannot seek past the end: grow the file first (zeros) */
	if (fstat(f->fd, &st) != 0)
		return -errno;
	if (off > (int64_t)st.st_size && ftruncate(f->fd, (off_t)off) != 0)
		return -errno;
	if (lseek(f->fd, (off_t)off, SEEK_SET) < 0)
		return -errno;
	f->pos = off;
	return 0;
}

int tmpf_pread(tmpf *f, void *buf, size_t len, int64_t off)
{
	int err = tmpf_seek(f, off);
	ssize_t n;

	if (err != 0)
		return err;
	n = read(f->fd, buf, len);
	if (n < 0) {
		f->pos = -1;
		return -errno;
	}
	f->pos += n;
	return (int)n;
}

int tmpf_pwrite(tmpf *f, const void *buf, size_t len, int64_t off)
{
	int err = tmpf_seek(f, off);
	ssize_t n;

	if (err != 0)
		return err;
	n = write(f->fd, buf, len);
	if (n < 0) {
		f->pos = -1;
		return -errno;
	}
	f->pos += n;
	return (int)n;
}

int tmpf_truncate(tmpf *f, int64_t size)
{
	f->pos = -1; /* a shorter file may move the position */
	return ftruncate(f->fd, (off_t)size) == 0 ? 0 : -errno;
}

int64_t tmpf_size(tmpf *f)
{
	struct stat st;

	if (fstat(f->fd, &st) != 0)
		return -errno;
	return (int64_t)st.st_size;
}

/* reads from the start in order (see tmpf_seek) */
static int copy_file(int from, const char *to)
{
	char buf[32768];
	ssize_t n;
	int out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0600);

	if (out < 0)
		return -errno;
	while ((n = read(from, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			close(out);
			unlink(to);
			return -EIO;
		}
	}
	close(out);
	return n < 0 ? -EIO : 0;
}

int tmpf_keep(tmpf *f, const char *dir, const char *uname, char *out, size_t outlen)
{
	/* the name comes from the server (UTF-8); files here use the local set */
	char *lname = plat_charset >= 0 ? cs_utf8_to_name(plat_charset, uname) : NULL;
	const char *name = lname != NULL ? lname : uname;
	size_t n = strlen(dir);
	const char *sep = (n > 0 && dir[n - 1] != ':' && dir[n - 1] != '/') ? "/" : "";
	int i, err;

	mkdir(dir, 0700);
	/* never replace an earlier kept file */
	snprintf(out, outlen, "%s%s%s", dir, sep, name);
	for (i = 1; access(out, F_OK) == 0 && i < 1000; i++)
		snprintf(out, outlen, "%s%s%s.%d", dir, sep, name, i);
	free(lname);
	close(f->fd);
	if (rename(f->path, out) == 0) {
		free(f);
		return 0;
	}
	/* another volume: copy, then delete the temp file */
	f->fd = open(f->path, O_RDONLY);
	if (f->fd < 0)
		return -EIO;
	err = copy_file(f->fd, out);
	if (err != 0)
		return err;
	close(f->fd);
	unlink(f->path);
	free(f);
	return 0;
}

void tmpf_close(tmpf *f)
{
	close(f->fd);
	unlink(f->path);
	free(f);
}

time_t plat_time(void)
{
	return time(NULL);
}

int64_t plat_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void plat_log(int level, const char *fmt, ...)
{
	char buf[600];
	va_list ap;

	if (level > plat_loglevel)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	IExec->DebugPrintF("[nextcloud-handler] %s", buf);
}

int plat_resolve(const char *host, char *ip, size_t iplen)
{
	struct hostent *he = gethostbyname(host);

	if (he == NULL || he->h_addrtype != AF_INET || he->h_addr_list[0] == NULL) {
		plat_log(LOG_INFO, "cannot look up %s\n", host);
		return -1;
	}
	snprintf(ip, iplen, "%s", inet_ntoa(*(struct in_addr *)he->h_addr_list[0]));
	return 0;
}

void plat_report(const char *title, const char *message)
{
	char *local = plat_charset >= 0 ? cs_utf8_to_message(plat_charset, message) : NULL;

	plat_log(LOG_ERR, "%s: %s\n", title, message);
	req_message(title, local != NULL ? local : message);
	free(local);
}
