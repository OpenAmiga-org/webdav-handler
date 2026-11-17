/* nextcloud-handler - the server list (shared by the handler and nchprefs).
 *
 * ENV:NextcloudHandler/servers.xml (saved copy in ENVARC:):
 *
 *   <?xml version="1.0" encoding="UTF-8"?>
 *   <nextcloudhandler version="1">
 *     <server name="Home" url="https://cloud.example.com" user="me"
 *             password="app-password" cafile="" insecure="0"/>
 *   </nextcloudhandler>
 *
 * An empty password means: ask when connecting. All strings are UTF-8.
 * Public domain. */
#ifndef NCH_SERVERS_H
#define NCH_SERVERS_H

#include <stddef.h>

#define CFG_DIR     "NextcloudHandler"
#define CFG_SERVERS "servers.xml"

/* Longest server name in bytes in the local character set: the FFS limit
 * for names, so that a server's folder can always be copied to a disk. */
#define SERVER_NAME_MAX 107

typedef struct {
	char name[SERVER_NAME_MAX * 3 + 1];  /* UTF-8; a valid AmigaDOS name */
	char url[512];                        /* Nextcloud address or WebDAV URL */
	char user[128];                       /* empty: no login */
	char password[256];                   /* empty: asked for when needed */
	char cafile[256];                     /* own certificate (PEM), Amiga path */
	int  insecure;                        /* do not check the certificate */
} server_cfg;

typedef struct {
	server_cfg *server;
	int         count;
	int         cap;
} server_list;

void servers_init(server_list *l);
void servers_free(server_list *l);   /* also clears the passwords */
/* Adds a copy; returns its index or -1 (out of memory). */
int  servers_add(server_list *l, const server_cfg *s);
void servers_remove(server_list *l, int index);
/* Index of the server with that name (case-insensitive), or -1. */
int  servers_find(const server_list *l, const char *name);
int  servers_equal(const server_list *a, const server_list *b);
int  server_cfg_equal(const server_cfg *a, const server_cfg *b);
/* The same account on the same server: only the name's case or the
 * password differ. */
int  server_cfg_same_account(const server_cfg *a, const server_cfg *b);

/* Loads a server file. Returns 0, -ENOENT when there is none, -EINVAL for
 * a broken file (message in err), -ENOMEM. Invalid or repeated servers are
 * skipped; their number goes to *skipped when not NULL. */
int  servers_load(const char *file, server_list *l, char *err, size_t errlen, int *skipped);
/* Writes the file (through a temp file). 0 or -errno. */
int  servers_save(const char *file, const server_list *l);

/* Checks a server name given in the local character set (8-bit). Returns
 * NULL when it is fine, else why not (English text). */
const char *server_name_problem(const char *local_name);

#endif
