/* nextcloud-handler - conversion between the Amiga's 8-bit character set
 * and UTF-8 (used on the server). Public domain.
 *
 * Characters that the local character set cannot show become "%uXXXX"
 * (or "%UXXXXXX" above U+FFFF), so that every server name can be shown,
 * opened and renamed on the Amiga and converts back to the same name. */
#ifndef NCH_CHARSET_H
#define NCH_CHARSET_H

#include <stddef.h>

enum { CS_ISO_8859_1 = 0, CS_ISO_8859_15 = 1 };

/* Picks the character set from a name such as "ISO-8859-15"; unknown
 * names give ISO-8859-1. */
int  cs_from_name(const char *name);

/* Both return a malloc'ed string, or NULL when out of memory. */
char *cs_local_to_utf8(int cs, const char *local);
char *cs_utf8_to_local(int cs, const char *utf8);
/* For file names: also escapes ':', which AmigaDOS cannot have in a name. */
char *cs_utf8_to_name(int cs, const char *utf8);

#endif
