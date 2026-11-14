/* nextcloud-handler - what the core needs from the operating system.
 * Implemented in src/os4/platform_os4.c and src/linux/platform_linux.c.
 * Public domain. */
#ifndef NCH_PLATFORM_H
#define NCH_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Temporary file used to buffer uploads. 64-bit offsets. */
typedef struct tmpf tmpf;

tmpf   *tmpf_create(const char *dir, int *err);        /* err = -errno */
int     tmpf_pread(tmpf *f, void *buf, size_t len, int64_t off);
int     tmpf_pwrite(tmpf *f, const void *buf, size_t len, int64_t off);
int     tmpf_truncate(tmpf *f, int64_t size);
int64_t tmpf_size(tmpf *f);
/* Moves the file to dir/name instead of deleting it (used when an upload
 * failed). The final path is written to out. Returns 0 or -errno. */
int     tmpf_keep(tmpf *f, const char *dir, const char *name, char *out, size_t outlen);
void    tmpf_close(tmpf *f);                            /* deletes the file */

/* Wall clock (UTC seconds since 1970) and a monotonic millisecond clock. */
time_t  plat_time(void);
int64_t plat_ms(void);

enum { LOG_ERR = 0, LOG_INFO = 1, LOG_DEBUG = 2 };
extern int plat_loglevel;
void    plat_log(int level, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

/* Tells the user about a problem that cannot be returned as an error code,
 * for example a failed upload when a file is closed. */
void    plat_report(const char *title, const char *message);

#endif
