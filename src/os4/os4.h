/* webdav-handler - AmigaOS 4 parts. Public domain. */
#ifndef WDV_OS4_H
#define WDV_OS4_H

#include <stddef.h>

/* requesters.c */
int  req_open_libs(void);
void req_close_libs(void);
/* Information requester; text may contain any characters. Lines longer
 * than REQ_WIDTH characters are broken. */
#define REQ_WIDTH 76
void req_message(const char *title, const char *text);
/* Asks for a password (texts in the local character set). Returns 1 for
 * OK, 0 for Cancel. */
int  req_password(const char *title, const char *text, char *buf, size_t buflen,
                  int *remember);

/* amissl.c: 0, -1 (no bsdsocket.library) or -2 (no usable AmiSSL) */
int  amissl_open(void);
void amissl_close(void);
int  amissl_version(void);       /* 5 or 6 while open, else 0 */
const char *amissl_name(void);   /* IdString of the opened AmiSSL, or "" */

/* platform_os4.c */
extern int plat_debug_serial;
extern int plat_charset;   /* messages are converted to it, -1 = no */
/* the character set given, else the system's (ENV:Charset) */
int os4_charset(const char *given);

#endif
