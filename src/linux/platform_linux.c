/* nextcloud-handler - platform functions for the Linux test build.
 * Public domain. */
#define _GNU_SOURCE
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int plat_loglevel = LOG_ERR;

struct tmpf {
	int  fd;
	char path[512];
};

tmpf *tmpf_create(const char *dir, int *err)
{
	tmpf *f = calloc(1, sizeof(*f));

	if (f == NULL) {
		*err = -ENOMEM;
		return NULL;
	}
	snprintf(f->path, sizeof(f->path), "%s/nch-XXXXXX", dir != NULL ? dir : "/tmp");
	f->fd = mkstemp(f->path);
	if (f->fd < 0) {
		*err = -errno;
		free(f);
		return NULL;
	}
	return f;
}

int tmpf_pread(tmpf *f, void *buf, size_t len, int64_t off)
{
	ssize_t n = pread(f->fd, buf, len, (off_t)off);

	return n < 0 ? -errno : (int)n;
}

int tmpf_pwrite(tmpf *f, const void *buf, size_t len, int64_t off)
{
	ssize_t n = pwrite(f->fd, buf, len, (off_t)off);

	return n < 0 ? -errno : (int)n;
}

int tmpf_truncate(tmpf *f, int64_t size)
{
	return ftruncate(f->fd, (off_t)size) == 0 ? 0 : -errno;
}

int64_t tmpf_size(tmpf *f)
{
	struct stat st;

	if (fstat(f->fd, &st) != 0)
		return -errno;
	return (int64_t)st.st_size;
}

int tmpf_keep(tmpf *f, const char *dir, const char *name, char *out, size_t outlen)
{
	int i;

	mkdir(dir, 0700);
	/* never replace an earlier kept file */
	snprintf(out, outlen, "%s/%s", dir, name);
	for (i = 1; access(out, F_OK) == 0 && i < 1000; i++)
		snprintf(out, outlen, "%s/%s.%d", dir, name, i);
	if (rename(f->path, out) != 0)
		return -errno;
	close(f->fd);
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
	va_list ap;

	if (level > plat_loglevel)
		return;
	va_start(ap, fmt);
	fputs("[nch] ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

int plat_resolve(const char *host, char *ip, size_t iplen)
{
	(void)host;
	(void)ip;
	(void)iplen;
	return -1; /* curl resolves by itself on Linux */
}

void plat_report(const char *title, const char *message)
{
	fprintf(stderr, "[nch] *** %s ***\n%s\n", title, message);
}

/* No requester on Linux: the tests give the answer in NCH_ASK_PASSWORD
 * (unset = Cancel) and NCH_REMEMBER=1. Every question is logged. */
int plat_ask_password(const char *server, const char *user, const char *url,
                      const char *problem, char *buf, size_t buflen, int *remember)
{
	const char *pw = getenv("NCH_ASK_PASSWORD");
	const char *rem = getenv("NCH_REMEMBER");

	fprintf(stderr, "[nch] password requester: %s (%s at %s)%s%s\n", server, user, url,
	        problem != NULL ? " - " : "", problem != NULL ? problem : "");
	*remember = rem != NULL && rem[0] == '1';
	if (pw == NULL)
		return 0;
	snprintf(buf, buflen, "%s", pw);
	return 1;
}
