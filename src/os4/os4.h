/* nextcloud-handler - AmigaOS 4 parts. Public domain. */
#ifndef NCH_OS4_H
#define NCH_OS4_H

#include <stddef.h>

/* requesters.c */
int  req_open_libs(void);
void req_close_libs(void);
/* Information requester; text may contain any characters. */
void req_message(const char *title, const char *text);
/* Asks for the password. Returns 1 for OK, 0 for Cancel. */
int  req_password(const char *device, const char *user, const char *server,
                  const char *problem, char *buf, size_t buflen, int *remember);

/* platform_os4.c */
extern int plat_debug_serial;
extern int plat_charset;   /* messages are converted to it, -1 = no */

#endif
