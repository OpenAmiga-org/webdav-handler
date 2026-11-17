/* nchprefs - edits the server list of nextcloud-handler
 * (ENV:NextcloudHandler/servers.xml, saved in ENVARC:). Built with clib4:
 * the "Test connection" button uses the handler's own WebDAV code.
 * Public domain. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/listbrowser.h>
#include <proto/layout.h>
#include <classes/window.h>
#include <gadgets/layout.h>
#include <gadgets/button.h>
#include <gadgets/string.h>
#include <gadgets/checkbox.h>
#include <gadgets/listbrowser.h>
#include <images/label.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <intuition/gadgetclass.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "charset.h"
#include "davclient.h"
#include "platform.h"
#include "servers.h"
#include "util.h"
#include "os4.h"
#include "version.h"

static const char __attribute__((used)) verstag[] =
	"\0$VER: nchprefs " NCH_STR(NCH_VERSION) "." NCH_STR(NCH_REVISION) " (" NCH_DATE ")";

#define PROGNAME   "nchprefs"
#define ENV_DIR    "ENV:" CFG_DIR
#define ENVARC_DIR "ENVARC:" CFG_DIR
#define MAX_OLD    16

/* from requesters.c */
extern struct IntuitionIFace *IIntuition;
extern struct LayoutIFace *ILayout;

struct ListBrowserIFace *IListBrowser;
struct AslIFace *IAsl;
struct Library *AslBase;
static struct ClassLibrary *WinLib, *LayLib, *BtnLib, *StrLib, *ChkLib, *LbLib, *LblLib;
static Class *WinCls, *LayCls, *BtnCls, *StrCls, *ChkCls, *LbCls, *LblCls;

enum {
	G_LIST = 1, G_ADD, G_DUP, G_REMOVE,
	G_NAME, G_URL, G_USER, G_PASS, G_CAFILE, G_CAPICK, G_INSECURE,
	G_TEST, G_SAVE, G_USE, G_CANCEL, G_COUNT
};

enum { M_ABOUT = 1, M_QUIT, M_LASTSAVED, M_RESTORE, M_RESET };

static struct NewMenu menus[] = {
	{ NM_TITLE, (STRPTR)"Project",       NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"About...",      (STRPTR)"?", 0, 0, (APTR)M_ABOUT },
	{ NM_ITEM,  NM_BARLABEL,             NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Quit",          (STRPTR)"Q", 0, 0, (APTR)M_QUIT },
	{ NM_TITLE, (STRPTR)"Edit",          NULL, 0, 0, NULL },
	{ NM_ITEM,  (STRPTR)"Reset to defaults", (STRPTR)"D", 0, 0, (APTR)M_RESET },
	{ NM_ITEM,  (STRPTR)"Last saved",    (STRPTR)"L", 0, 0, (APTR)M_LASTSAVED },
	{ NM_ITEM,  (STRPTR)"Restore",       (STRPTR)"R", 0, 0, (APTR)M_RESTORE },
	{ NM_END,   NULL,                    NULL, 0, 0, NULL }
};

static struct ColumnInfo columns[] = {
	{ 30, (STRPTR)"Name", 0 },
	{ 50, (STRPTR)"Address", 0 },
	{ 20, (STRPTR)"User", 0 },
	{ -1, NULL, 0 }
};

static struct {
	Object        *win;
	struct Window *w;
	Object        *g[G_COUNT];
	struct List    labels;
	server_list    servers;   /* UTF-8, as in the file */
	server_list    start;     /* what was there when the program started */
	int            sel;       /* selected server, -1 = none */
	int            cs;        /* local character set */
	int            amissl;    /* 1 = open, -1 = failed */
	char           old[MAX_OLD][300];   /* DOSDrivers of 0.37 or older taken over */
	char           olddev[MAX_OLD][64];
	int            nold;
} g;

/* --- character sets --------------------------------------------------------- */

static void to_local(const char *utf8, char *out, size_t outlen)
{
	char *l = cs_utf8_to_local(g.cs, utf8);

	snprintf(out, outlen, "%s", l != NULL ? l : utf8);
	free(l);
}

/* for messages: keeps line breaks */
static void to_local_msg(const char *utf8, char *out, size_t outlen)
{
	char *l = cs_utf8_to_message(g.cs, utf8);

	snprintf(out, outlen, "%s", l != NULL ? l : utf8);
	free(l);
}

static void to_utf8(const char *local, char *out, size_t outlen)
{
	char *u = cs_local_to_utf8(g.cs, local);
	const char *src = u != NULL ? u : local;
	size_t n = strlen(src);

	if (n >= outlen)
		n = outlen - 1;
	memcpy(out, src, n);
	out[n] = '\0';
	if (u != NULL)
		memset(u, 0, strlen(u));   /* may be a password */
	free(u);
}

/* --- messages ---------------------------------------------------------------- */

static int ask(const char *text, const char *buttons)
{
	struct EasyStruct es;
	char *wrapped = text_wrap(text, REQ_WIDTH);
	int rc;

	memset(&es, 0, sizeof(es));
	es.es_StructSize = sizeof(es);
	es.es_Title = (STRPTR)PROGNAME;
	es.es_TextFormat = (STRPTR)"%s";
	es.es_GadgetFormat = (STRPTR)buttons;
	rc = (int)IIntuition->EasyRequest(g.w, &es, NULL, wrapped != NULL ? wrapped : text);
	free(wrapped);
	return rc;
}

static void tell(const char *text)
{
	ask(text, "OK");
}

static void busy(int on)
{
	if (g.w != NULL)
		IIntuition->SetWindowPointer(g.w, WA_BusyPointer, on, WA_PointerDelay, on, TAG_END);
}

/* --- gadget helpers ------------------------------------------------------------ */

static char  *strbufs[G_COUNT];
static size_t strsizes[G_COUNT];

/* Setting a string gadget to "" left the text of the gadget set before it
 * on display; the empty value is written into its buffer instead. */
static void set_text(int id, const char *text)
{
	struct TagItem tags[2];

	tags[0].ti_Tag = STRINGA_TextVal;
	tags[0].ti_Data = (uint32)(text[0] != '\0' ? text : " ");
	tags[1].ti_Tag = TAG_END;
	IIntuition->SetGadgetAttrsA((struct Gadget *)g.g[id], g.w, NULL, tags);
	if (text[0] == '\0') {
		struct StringInfo *si = (struct StringInfo *)((struct Gadget *)g.g[id])->SpecialInfo;

		if (strbufs[id] != NULL)
			strbufs[id][0] = '\0';
		if (si != NULL) {
			si->NumChars = 0;
			si->BufferPos = 0;
			si->DispPos = 0;
		}
	}
	if (g.w != NULL)
		IIntuition->RefreshGList((struct Gadget *)g.g[id], g.w, NULL, 1);
}

static void set_attrs(int id, uint32 tag, uint32 val)
{
	struct TagItem tags[2];

	tags[0].ti_Tag = tag;
	tags[0].ti_Data = val;
	tags[1].ti_Tag = TAG_END;
	if (IIntuition->SetGadgetAttrsA((struct Gadget *)g.g[id], g.w, NULL, tags) && g.w != NULL)
		IIntuition->RefreshGList((struct Gadget *)g.g[id], g.w, NULL, 1);
}

static const char *get_str(int id)
{
	STRPTR s = NULL;

	IIntuition->GetAttr(STRINGA_TextVal, g.g[id], (uint32 *)&s);
	return s != NULL ? (const char *)s : "";
}

static uint32 get_num(int id, uint32 tag)
{
	uint32 v = 0;

	IIntuition->GetAttr(tag, g.g[id], &v);
	return v;
}

/* --- the list ---------------------------------------------------------------- */

static void list_rebuild(void)
{
	int i;

	set_attrs(G_LIST, LISTBROWSER_Labels, (uint32)~0);
	IListBrowser->FreeListBrowserList(&g.labels);
	for (i = 0; i < g.servers.count; i++) {
		const server_cfg *s = &g.servers.server[i];
		char name[400], url[600], user[200];
		struct Node *n;

		to_local(s->name, name, sizeof(name));
		to_local(s->url, url, sizeof(url));
		to_local(s->user, user, sizeof(user));
		n = IListBrowser->AllocListBrowserNode(3,
			LBNA_Column, 0, LBNCA_CopyText, TRUE, LBNCA_Text, name,
			LBNA_Column, 1, LBNCA_CopyText, TRUE, LBNCA_Text, url,
			LBNA_Column, 2, LBNCA_CopyText, TRUE, LBNCA_Text, user,
			TAG_END);
		if (n != NULL)
			IExec->AddTail(&g.labels, n);
	}
	{
		struct TagItem tags[4];

		tags[0].ti_Tag = LISTBROWSER_Labels;
		tags[0].ti_Data = (uint32)&g.labels;
		tags[1].ti_Tag = LISTBROWSER_Selected;
		tags[1].ti_Data = (uint32)g.sel;
		tags[2].ti_Tag = LISTBROWSER_MakeVisible;
		tags[2].ti_Data = (uint32)(g.sel >= 0 ? g.sel : 0);
		tags[3].ti_Tag = TAG_END;
		if (IIntuition->SetGadgetAttrsA((struct Gadget *)g.g[G_LIST], g.w, NULL, tags) && g.w != NULL)
			IIntuition->RefreshGList((struct Gadget *)g.g[G_LIST], g.w, NULL, 1);
	}
}

/* --- the form ------------------------------------------------------------------ */

static const int form_ids[] = {
	G_NAME, G_URL, G_USER, G_PASS, G_CAFILE, G_CAPICK, G_INSECURE,
	G_TEST, G_DUP, G_REMOVE, 0
};

static void form_fill(void)
{
	/* one buffer per field: the gadgets may keep the pointer until they
	 * have copied the text */
	static char name[400], url[1600], user[400], pass[800], cafile[800];
	const server_cfg *s = g.sel >= 0 ? &g.servers.server[g.sel] : NULL;
	int i;

	for (i = 0; form_ids[i] != 0; i++)
		set_attrs(form_ids[i], GA_Disabled, s == NULL);
	if (s == NULL) {
		name[0] = url[0] = user[0] = pass[0] = cafile[0] = '\0';
		set_attrs(G_INSECURE, GA_Selected, FALSE);
	} else {
		to_local(s->name, name, sizeof(name));
		to_local(s->url, url, sizeof(url));
		to_local(s->user, user, sizeof(user));
		to_local(s->password, pass, sizeof(pass));
		to_local(s->cafile, cafile, sizeof(cafile));
		set_attrs(G_INSECURE, GA_Selected, s->insecure ? TRUE : FALSE);
	}
	set_text(G_NAME, name);
	set_text(G_URL, url);
	set_text(G_USER, user);
	set_text(G_PASS, pass);
	set_text(G_CAFILE, cafile);
}

/* Takes the form into the selected server. A bad name is refused (the old
 * one comes back) and 0 is returned. */
static int form_commit(void)
{
	server_cfg *s;
	char name[sizeof(s->name)];
	const char *lname, *problem;
	int k;

	if (g.sel < 0)
		return 1;
	s = &g.servers.server[g.sel];
	lname = get_str(G_NAME);
	problem = server_name_problem(lname);
	to_utf8(lname, name, sizeof(name));
	if (problem == NULL && strlen(name) >= sizeof(s->name) - 1)
		problem = "The name is too long.";
	if (problem == NULL && (k = servers_find(&g.servers, name)) >= 0 && k != g.sel)
		problem = "Another server has that name.";
	to_utf8(get_str(G_URL), s->url, sizeof(s->url));
	to_utf8(get_str(G_USER), s->user, sizeof(s->user));
	to_utf8(get_str(G_PASS), s->password, sizeof(s->password));
	to_utf8(get_str(G_CAFILE), s->cafile, sizeof(s->cafile));
	s->insecure = get_num(G_INSECURE, GA_Selected) != 0;
	if (problem != NULL) {
		char msg[300], old[400];

		to_local(s->name, old, sizeof(old));
		snprintf(msg, sizeof(msg), "\"%s\" cannot be used as a server name:\n%s", lname, problem);
		tell(msg);
		set_text(G_NAME, old);
		list_rebuild();
		return 0;
	}
	snprintf(s->name, sizeof(s->name), "%s", name);
	list_rebuild();
	return 1;
}

static void select_server(int i)
{
	g.sel = i >= 0 && i < g.servers.count ? i : -1;
	list_rebuild();
	form_fill();
}

/* "New server", "New server 2", ... */
static void unique_name(server_list *l, const char *base, char *out, size_t outlen)
{
	int n;

	snprintf(out, outlen, "%s", base);
	for (n = 2; servers_find(l, out) >= 0 && n < 1000; n++)
		snprintf(out, outlen, "%.90s %d", base, n);
}

static void add_server(const server_cfg *copy)
{
	server_cfg s;
	char base[200];
	int i;

	if (copy != NULL) {
		s = *copy;
		snprintf(base, sizeof(base), "%.90s copy", copy->name);
	} else {
		memset(&s, 0, sizeof(s));
		snprintf(s.url, sizeof(s.url), "https://");
		snprintf(base, sizeof(base), "New server");
	}
	unique_name(&g.servers, base, s.name, sizeof(s.name));
	i = servers_add(&g.servers, &s);
	memset(&s, 0, sizeof(s));
	if (i < 0)
		return;
	select_server(i);
	if (ILayout != NULL)
		ILayout->ActivateLayoutGadget((struct Gadget *)g.g[0], g.w, NULL, (uint32)g.g[G_NAME]);
}

/* --- files ---------------------------------------------------------------- */

static int load_from(const char *dir, server_list *l, int quiet)
{
	char file[300], err[600], msg[800];
	int rc, skipped = 0;

	snprintf(file, sizeof(file), "%s/%s", dir, CFG_SERVERS);
	rc = servers_load(file, l, err, sizeof(err), &skipped);
	if (rc == -EINVAL && !quiet) {
		snprintf(msg, sizeof(msg), "The server list cannot be read:\n%s", err);
		tell(msg);
	} else if (rc == 0 && skipped > 0 && !quiet) {
		snprintf(msg, sizeof(msg), "%d servers in %s were left out:\n"
		         "bad or repeated names.", skipped, file);
		tell(msg);
	}
	return rc;
}

static int save_to(const char *dir)
{
	char file[300], msg[500];
	int rc;

	mkdir(dir, 0700);
	snprintf(file, sizeof(file), "%s/%s", dir, CFG_SERVERS);
	rc = servers_save(file, &g.servers);
	if (rc != 0) {
		snprintf(msg, sizeof(msg), "Cannot save %s:\n%s", file, strerror(-rc));
		tell(msg);
	}
	return rc;
}

/* servers that the handler could not use: say so before saving */
static int check_all(void)
{
	char msg[600], name[400];
	int i;

	for (i = 0; i < g.servers.count; i++) {
		const server_cfg *s = &g.servers.server[i];

		to_local(s->name, name, sizeof(name));
		if (strncasecmp(s->url, "http://", 7) != 0 && strncasecmp(s->url, "https://", 8) != 0) {
			snprintf(msg, sizeof(msg), "The address of \"%s\" must start with\n"
			         "https:// (or http://).", name);
			tell(msg);
			select_server(i);
			return 0;
		}
		if (s->url[strcspn(s->url, ":") + 3] == '\0') {
			snprintf(msg, sizeof(msg), "The server \"%s\" has no address.", name);
			tell(msg);
			select_server(i);
			return 0;
		}
	}
	return 1;
}

static void replace_servers(const server_list *from)
{
	int i;

	g.servers.count = 0;
	for (i = 0; i < from->count; i++)
		servers_add(&g.servers, &from->server[i]);
	select_server(g.servers.count > 0 ? 0 : -1);
}

/* --- DOSDrivers of version 0.37 and older ----------------------------------- */

/* Until 0.37 every server had a DOSDriver of its own, with the server in
 * its Startup line: "URL=https://... USER=name VOLUME=Nextcloud", the
 * password in PASSWORD= or in ENV(ARC):NextcloudHandler/<device>.auth. */

/* the value of KEY= in a Startup line, quotes removed */
static int startup_value(const char *line, const char *key, char *out, size_t outlen)
{
	size_t kl = strlen(key);
	const char *p = line;

	for (; *p != '\0'; p++) {
		if ((p == line || isspace((unsigned char)p[-1]) || p[-1] == '"') &&
		    strncasecmp(p, key, kl) == 0 && p[kl] == '=') {
			const char *v = p + kl + 1;
			size_t n;

			if ((v[0] == '\\' || v[0] == '*') && v[1] == '"') {
				/* an escaped quote inside the quoted Startup line */
				const char *e;

				v += 2;
				for (e = v; *e != '\0' && !((e[0] == '\\' || e[0] == '*') && e[1] == '"'); e++)
					;
				n = (size_t)(e - v);
			} else if (*v == '"') {
				v++;
				n = strcspn(v, "\"");
			} else {
				n = strcspn(v, " \t\"");
			}
			if (n >= outlen)
				n = outlen - 1;
			memcpy(out, v, n);
			out[n] = '\0';
			return 1;
		}
	}
	return 0;
}

static int startup_switch(const char *line, const char *key)
{
	size_t kl = strlen(key);
	const char *p;

	for (p = line; *p != '\0'; p++)
		if ((p == line || isspace((unsigned char)p[-1]) || p[-1] == '"') &&
		    strncasecmp(p, key, kl) == 0 &&
		    (p[kl] == '\0' || isspace((unsigned char)p[kl]) || p[kl] == '"'))
			return 1;
	return 0;
}

static void read_auth(const char *device, char *out, size_t outlen)
{
	static const char *dirs[] = { "ENV:", "ENVARC:" };
	char path[256];
	int i;

	out[0] = '\0';
	for (i = 0; i < 2 && out[0] == '\0'; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%s%s/%s.auth", dirs[i], CFG_DIR, device);
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		if (fgets(out, (int)outlen, f) == NULL)
			out[0] = '\0';
		out[strcspn(out, "\r\n")] = '\0';
		fclose(f);
	}
}

/* takes the server over from one DOSDriver; 1 when it was one of ours */
static int import_dosdriver(const char *path, const char *device, server_list *l)
{
	char buf[8192], startup[2048], val[600], *p, *q;
	server_cfg s;
	FILE *f = fopen(path, "rb");
	size_t n;
	int handler = 0;

	if (f == NULL)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	/* comments go */
	while ((p = strstr(buf, "/*")) != NULL) {
		q = strstr(p + 2, "*/");
		if (q == NULL) {
			*p = '\0';
			break;
		}
		memmove(p, q + 2, strlen(q + 2) + 1);
	}
	startup[0] = '\0';
	for (p = strtok(buf, "\n"); p != NULL; p = strtok(NULL, "\n")) {
		while (isspace((unsigned char)*p))
			p++;
		if (strncasecmp(p, "Handler", 7) == 0 && strstr(p, "nextcloud-handler") != NULL)
			handler = 1;
		else if (strncasecmp(p, "Startup", 7) == 0 && (q = strchr(p, '=')) != NULL)
			snprintf(startup, sizeof(startup), "%s", q + 1);
	}
	if (!handler || !startup_value(startup, "URL", val, sizeof(val)))
		return 0;
	memset(&s, 0, sizeof(s));
	to_utf8(val, s.url, sizeof(s.url));
	if (startup_value(startup, "USER", val, sizeof(val)))
		to_utf8(val, s.user, sizeof(s.user));
	if (startup_value(startup, "PASSWORD", val, sizeof(val)))
		to_utf8(val, s.password, sizeof(s.password));
	else
		read_auth(device, s.password, sizeof(s.password));
	if (startup_value(startup, "CAFILE", val, sizeof(val)))
		to_utf8(val, s.cafile, sizeof(s.cafile));
	s.insecure = startup_switch(startup, "INSECURE");
	/* the volume's name, else the device's */
	if (!startup_value(startup, "VOLUME", val, sizeof(val)) || server_name_problem(val) != NULL)
		snprintf(val, sizeof(val), "%s", device);
	{
		char name[sizeof(s.name)];

		to_utf8(val, name, sizeof(name));
		unique_name(l, name, s.name, sizeof(s.name));
	}
	servers_add(l, &s);
	memset(&s, 0, sizeof(s));
	memset(buf, 0, sizeof(buf));
	memset(startup, 0, sizeof(startup));
	return 1;
}

static void import_old(server_list *l)
{
	static const char *dirs[] = { "DEVS:DOSDrivers", "SYS:Storage/DOSDrivers" };
	int i;

	for (i = 0; i < 2; i++) {
		APTR ctx = IDOS->ObtainDirContextTags(EX_StringNameInput, dirs[i],
		                                      EX_DataFields, EXF_NAME | EXF_TYPE, TAG_END);
		struct ExamineData *dat;

		if (ctx == NULL)
			continue;
		while ((dat = IDOS->ExamineDir(ctx)) != NULL && g.nold < MAX_OLD) {
			char path[300];
			size_t n = strlen(dat->Name);

			if (!EXD_IS_FILE(dat) || (n > 5 && strcasecmp(dat->Name + n - 5, ".info") == 0) ||
			    n >= sizeof(g.olddev[0]))
				continue;
			snprintf(path, sizeof(path), "%s/%s", dirs[i], dat->Name);
			if (import_dosdriver(path, dat->Name, l)) {
				snprintf(g.old[g.nold], sizeof(g.old[0]), "%s", path);
				snprintf(g.olddev[g.nold], sizeof(g.olddev[0]), "%s", dat->Name);
				g.nold++;
			}
		}
		IDOS->ReleaseDirContext(ctx);
	}
}

static void tell_imported(void)
{
	char msg[2000];
	int i;

	snprintf(msg, sizeof(msg), "These DOSDrivers were made for nextcloud-handler 0.37\n"
	         "or older, with one server each:\n\n");
	for (i = 0; i < g.nold; i++)
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "  %s\n", g.old[i]);
	snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg),
	         "\nTheir servers are in the list now; Save keeps them.\n"
	         "nextcloud-handler now shows all servers in one device, NCH:.");
	tell(msg);
}

/* after Save or Use: the old DOSDrivers are no longer wanted */
static void offer_delete_old(void)
{
	char msg[2000];
	int i;

	if (g.nold == 0)
		return;
	snprintf(msg, sizeof(msg), "The servers of these old DOSDrivers are now in NCH:\n\n");
	for (i = 0; i < g.nold; i++)
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "  %s\n", g.old[i]);
	snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg),
	         "\nDelete them? A device that is mounted now keeps\n"
	         "working until the computer restarts.");
	if (ask(msg, "Delete|Keep") != 1)
		return;
	for (i = 0; i < g.nold; i++) {
		char path[320];

		remove(g.old[i]);
		snprintf(path, sizeof(path), "%s.info", g.old[i]);
		remove(path);
		snprintf(path, sizeof(path), "%s/%s.auth", ENV_DIR, g.olddev[i]);
		remove(path);
		snprintf(path, sizeof(path), "%s/%s.auth", ENVARC_DIR, g.olddev[i]);
		remove(path);
	}
	g.nold = 0;
}

/* --- test connection ---------------------------------------------------------- */

static void format_size(int64_t n, char *out, size_t outlen)
{
	if (n >= (int64_t)10 << 30)
		snprintf(out, outlen, "%lld GB", (long long)(n >> 30));
	else if (n >= (int64_t)10 << 20)
		snprintf(out, outlen, "%lld MB", (long long)(n >> 20));
	else
		snprintf(out, outlen, "%lld KB", (long long)(n >> 10));
}

static const char *test_hint(dav_session *d)
{
	long status = dav_last_status(d);

	switch (dav_last_kind(d)) {
	case DAV_FAIL_TLS:
		return "The server's certificate could not be checked. If the server\n"
		       "uses a certificate of its own, give it as the certificate file;\n"
		       "otherwise check that AmiSSL:Certs is up to date.";
	case DAV_FAIL_NETWORK:
		return "Check the address and that the network is up.";
	}
	if (status == 401 || status == 403)
		return "The server did not accept the user name or password.\n"
		       "With two-factor login, use an app password.";
	if (status == 404)
		return "No WebDAV folder at that address. For Nextcloud, enter the\n"
		       "address you open in the browser.";
	return "";
}

static void test_connection(void)
{
	server_cfg s;
	dav_config dc;
	dav_session *d;
	dav_list dl;
	char err[700], msg[2000], local[2000], typed[256], free_txt[40];
	const char *pass, *capath = NULL;
	int rc, remember = 0, asked = 0, dirs = 0, files = 0, have_list = 0;
	size_t i;
	int64_t t0;

	if (g.sel < 0 || !form_commit())
		return;
	s = g.servers.server[g.sel];
	if (strncasecmp(s.url, "http://", 7) != 0 && strncasecmp(s.url, "https://", 8) != 0) {
		tell("Enter the server's address first, for example\nhttps://cloud.example.com");
		return;
	}
	if (g.amissl == 0)
		g.amissl = amissl_open() == 0 ? 1 : -1;
	if (g.amissl < 0) {
		tell("nextcloud-handler needs AmiSSL 5 or newer (and a TCP/IP stack).\n"
		     "AmiSSL: OS4Depot library/misc/amissl.lha");
		return;
	}
	pass = s.password;
	typed[0] = '\0';
	if (s.user[0] != '\0' && s.password[0] == '\0') {
		if (!plat_ask_password(s.name, s.user, s.url, NULL, typed, sizeof(typed), &remember))
			return;
		pass = typed;
		asked = 1;
	}
	if (s.cafile[0] == '\0') {
		BPTR l = IDOS->Lock("AmiSSL:Certs", SHARED_LOCK);

		if (l != ZERO) {
			IDOS->UnLock(l);
			capath = "AmiSSL:Certs";
		}
	}
	memset(&dc, 0, sizeof(dc));
	dc.url = s.url;
	dc.user = s.user[0] != '\0' ? s.user : NULL;
	dc.password = pass;
	dc.ca_file = s.cafile[0] != '\0' ? s.cafile : NULL;
	dc.ca_path = capath;
	dc.insecure = s.insecure;
	dc.connect_timeout = 15;
	dc.io_timeout = 20;
	dc.user_agent = "nchprefs " NCH_STR(NCH_VERSION) "." NCH_STR(NCH_REVISION) " (AmigaOS 4)";
	busy(1);
	t0 = plat_ms();
	d = dav_new(&dc);
	if (d == NULL) {
		busy(0);
		tell("Out of memory.");
		return;
	}
	rc = dav_connect(d);
	memset(&dl, 0, sizeof(dl));
	if (rc == 0 && dav_propfind(d, "/", 1, &dl) == 0) {
		have_list = 1;
		for (i = 0; i < dl.count; i++) {
			if (dl.items[i].is_self)
				continue;
			if (dl.items[i].is_dir)
				dirs++;
			else
				files++;
		}
	}
	busy(0);
	if (rc == 0) {
		if (!have_list)
			snprintf(free_txt, sizeof(free_txt), "?");
		else if (dl.quota_avail >= 0)
			format_size(dl.quota_avail, free_txt, sizeof(free_txt));
		else
			snprintf(free_txt, sizeof(free_txt), "no limit");
		snprintf(msg, sizeof(msg),
		         "The connection works.\n\n"
		         "Address: %s\n"
		         "Files at: %s\n"
		         "Nextcloud: %s\n"
		         "User: %s\n"
		         "Contents: %d folder%s, %d file%s\n"
		         "Free space: %s\n"
		         "Time: %d.%d s",
		         s.url, dav_root_url(d), dav_is_nextcloud(d) ? "yes" : "no (plain WebDAV)",
		         s.user[0] != '\0' ? s.user : "(none)", dirs, dirs == 1 ? "" : "s",
		         files, files == 1 ? "" : "s", free_txt,
		         (int)((plat_ms() - t0) / 1000), (int)((plat_ms() - t0) / 100 % 10));
		if (asked && remember) {
			snprintf(g.servers.server[g.sel].password, sizeof(s.password), "%s", typed);
			form_fill();
		}
	} else {
		snprintf(err, sizeof(err), "%s", dav_error(d));
		snprintf(msg, sizeof(msg), "The connection failed.\n\n%s\n\n%s", err, test_hint(d));
	}
	if (have_list)
		dav_list_free(&dl);
	dav_free(d);
	memset(typed, 0, sizeof(typed));
	memset(&s, 0, sizeof(s));
	to_local_msg(msg, local, sizeof(local));
	tell(local);
}

/* --- window ---------------------------------------------------------------- */

static Object *button(int id, const char *text)
{
	return g.g[id] = IIntuition->NewObject(BtnCls, NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		GA_Text, text,
		TAG_END);
}

/* every string gadget gets buffers of its own: the default ones hold only
 * 128 bytes */
static Object *string(int id, int max, int password)
{
	char *b = calloc(3, (size_t)max + 1);

	if (b == NULL)
		return NULL;
	strbufs[id] = b;
	strsizes[id] = 3 * ((size_t)max + 1);
	return g.g[id] = IIntuition->NewObject(StrCls, NULL,
		GA_ID, id,
		GA_RelVerify, TRUE,
		GA_TabCycle, TRUE,
		STRINGA_MaxChars, max + 1,
		STRINGA_Buffer, b,
		STRINGA_UndoBuffer, b + max + 1,
		STRINGA_WorkBuffer, b + 2 * (max + 1),
		password ? STRINGA_HookType : TAG_IGNORE, SHK_PASSWORD,
		TAG_END);
}

static Object *label(const char *text)
{
	return IIntuition->NewObject(LblCls, NULL, LABEL_Text, text, TAG_END);
}

static int make_window(void)
{
	g.win = IIntuition->NewObject(WinCls, NULL,
		WA_Title, "Nextcloud servers",
		WA_ScreenTitle, PROGNAME " " NCH_STR(NCH_VERSION) "." NCH_STR(NCH_REVISION),
		WA_Activate, TRUE,
		WA_DragBar, TRUE,
		WA_DepthGadget, TRUE,
		WA_CloseGadget, TRUE,
		WA_SizeGadget, TRUE,
		WA_InnerWidth, 600,
		WINDOW_Position, WPOS_CENTERSCREEN,
		WINDOW_NewMenu, menus,
		WINDOW_Layout, g.g[0] = IIntuition->NewObject(LayCls, NULL,
			LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
			LAYOUT_SpaceOuter, TRUE,
			LAYOUT_DeferLayout, TRUE,

			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
				LAYOUT_BevelStyle, BVS_GROUP,
				LAYOUT_Label, "Servers (folders in NCH:)",
				LAYOUT_AddChild, g.g[G_LIST] = IIntuition->NewObject(LbCls, NULL,
					GA_ID, G_LIST,
					GA_RelVerify, TRUE,
					LISTBROWSER_ColumnInfo, columns,
					LISTBROWSER_ColumnTitles, TRUE,
					LISTBROWSER_ShowSelected, TRUE,
					LISTBROWSER_Labels, &g.labels,
					TAG_END),
				CHILD_MinHeight, 120,
				CHILD_MinWidth, 560,
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_EvenSize, TRUE,
					LAYOUT_AddChild, button(G_ADD, "_Add"),
					LAYOUT_AddChild, button(G_DUP, "_Duplicate"),
					LAYOUT_AddChild, button(G_REMOVE, "Re_move"),
					TAG_END),
				CHILD_WeightedHeight, 0,
				TAG_END),

			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
				LAYOUT_BevelStyle, BVS_GROUP,
				LAYOUT_Label, "Server",
				LAYOUT_AddChild, string(G_NAME, SERVER_NAME_MAX, 0),
				CHILD_Label, label("_Name"),
				LAYOUT_AddChild, string(G_URL, 511, 0),
				CHILD_Label, label("Add_ress"),
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_AddChild, string(G_USER, 127, 0),
					LAYOUT_AddChild, string(G_PASS, 255, 1),
					CHILD_Label, label("Pass_word"),
					TAG_END),
				CHILD_Label, label("_User"),
				LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
					LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
					LAYOUT_AddChild, string(G_CAFILE, 255, 0),
					LAYOUT_AddChild, button(G_CAPICK, "..."),
					CHILD_WeightedWidth, 0,
					TAG_END),
				CHILD_Label, label("_Certificate"),
				LAYOUT_AddChild, g.g[G_INSECURE] = IIntuition->NewObject(ChkCls, NULL,
					GA_ID, G_INSECURE,
					GA_RelVerify, TRUE,
					GA_Text, "Do _not check the server's certificate (unsafe)",
					TAG_END),
				LAYOUT_AddChild, button(G_TEST, "_Test connection"),
				CHILD_WeightedHeight, 0,
				TAG_END),
			CHILD_WeightedHeight, 0,

			LAYOUT_AddChild, IIntuition->NewObject(LayCls, NULL,
				LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ,
				LAYOUT_EvenSize, TRUE,
				LAYOUT_AddChild, button(G_SAVE, "Sa_ve"),
				LAYOUT_AddChild, button(G_USE, "U_se"),
				LAYOUT_AddChild, button(G_CANCEL, "_Cancel"),
				TAG_END),
			CHILD_WeightedHeight, 0,
			TAG_END),
		TAG_END);
	if (g.win == NULL)
		return 0;
	g.w = (struct Window *)IIntuition->IDoMethod(g.win, WM_OPEN, NULL);
	return g.w != NULL;
}

static void pick_cafile(void)
{
	struct FileRequester *fr;
	char path[600], drawer[300];
	const char *file;

	if (IAsl == NULL)
		return;
	snprintf(drawer, sizeof(drawer), "%s", get_str(G_CAFILE));
	file = drawer[0] != '\0' ? (const char *)IDOS->FilePart(drawer) : "";
	snprintf(path, sizeof(path), "%s", file);
	if (drawer[0] != '\0')
		*(STRPTR)IDOS->PathPart(drawer) = '\0';
	else
		snprintf(drawer, sizeof(drawer), "%s", ENVARC_DIR);
	fr = IAsl->AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, "Certificate of the server (PEM file)",
		ASLFR_InitialDrawer, drawer,
		ASLFR_InitialFile, path,
		TAG_END);
	if (fr == NULL)
		return;
	if (IAsl->AslRequestTags(fr, ASLFR_Window, g.w, TAG_END) &&
	    fr->fr_File != NULL && fr->fr_File[0] != '\0') {
		snprintf(path, sizeof(path), "%s", fr->fr_Drawer != NULL ? (const char *)fr->fr_Drawer : "");
		IDOS->AddPart(path, fr->fr_File, sizeof(path));
		set_text(G_CAFILE, path);
		form_commit();
	}
	IAsl->FreeAslRequest(fr);
}

static void about(void)
{
	tell(PROGNAME " " NCH_STR(NCH_VERSION) "." NCH_STR(NCH_REVISION) " (" NCH_DATE ")\n\n"
	     "nchprefs keeps the list of Nextcloud and WebDAV servers\n"
	     "that nextcloud-handler shows as folders in NCH:\n"
	     "(in ENV:NextcloudHandler).\n\n"
	     "Public domain. Uses libcurl, expat, zlib and AmiSSL.");
}

/* returns 1 when the program should end */
static int handle_menu(uint16 code)
{
	while (code != MENUNULL) {
		struct MenuItem *item = IIntuition->ItemAddress(g.w->MenuStrip, code);
		server_list l;

		if (item == NULL)
			break;
		switch ((uint32)GTMENUITEM_USERDATA(item)) {
		case M_ABOUT:
			about();
			break;
		case M_QUIT:
			return 1;
		case M_RESET:
			g.servers.count = 0;
			select_server(-1);
			break;
		case M_LASTSAVED:
			servers_init(&l);
			if (load_from(ENVARC_DIR, &l, 0) == 0 || l.count == 0)
				replace_servers(&l);
			servers_free(&l);
			break;
		case M_RESTORE:
			replace_servers(&g.start);
			break;
		}
		code = item->NextSelect;
	}
	return 0;
}

static void event_loop(void)
{
	uint32 sigmask = 0, result;
	uint16 code;
	int done = 0;

	IIntuition->GetAttr(WINDOW_SigMask, g.win, &sigmask);
	while (!done) {
		uint32 sigs = IExec->Wait(sigmask | SIGBREAKF_CTRL_C);

		if (sigs & SIGBREAKF_CTRL_C)
			break;
		while ((result = IIntuition->IDoMethod(g.win, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG) {
			switch (result & WMHI_CLASSMASK) {
			case WMHI_CLOSEWINDOW:
				done = 1;
				break;
			case WMHI_MENUPICK:
				if (handle_menu(code))
					done = 1;
				break;
			case WMHI_GADGETUP:
				switch (result & WMHI_GADGETMASK) {
				case G_LIST: {
					int i = (int)get_num(G_LIST, LISTBROWSER_Selected);

					if (i != g.sel) {
						form_commit();
						select_server(i);
					}
					break;
				}
				case G_ADD:
					if (form_commit())
						add_server(NULL);
					break;
				case G_DUP:
					if (g.sel >= 0 && form_commit())
						add_server(&g.servers.server[g.sel]);
					break;
				case G_REMOVE:
					if (g.sel >= 0) {
						int i = g.sel;

						servers_remove(&g.servers, i);
						select_server(i < g.servers.count ? i : g.servers.count - 1);
					}
					break;
				case G_NAME: case G_URL: case G_USER: case G_PASS:
				case G_CAFILE: case G_INSECURE:
					form_commit();
					break;
				case G_CAPICK:
					pick_cafile();
					break;
				case G_TEST:
					test_connection();
					break;
				case G_SAVE:
					if (form_commit() && check_all() &&
					    save_to(ENVARC_DIR) == 0 && save_to(ENV_DIR) == 0) {
						offer_delete_old();
						done = 1;
					}
					break;
				case G_USE:
					if (form_commit() && check_all() && save_to(ENV_DIR) == 0) {
						offer_delete_old();
						done = 1;
					}
					break;
				case G_CANCEL:
					done = 1;
					break;
				}
				break;
			}
		}
	}
}

static int open_libs(void)
{
	if (!req_open_libs())
		return 0;
	WinLib = IIntuition->OpenClass("window.class", 53, &WinCls);
	LayLib = IIntuition->OpenClass("gadgets/layout.gadget", 53, &LayCls);
	BtnLib = IIntuition->OpenClass("gadgets/button.gadget", 53, &BtnCls);
	StrLib = IIntuition->OpenClass("gadgets/string.gadget", 53, &StrCls);
	ChkLib = IIntuition->OpenClass("gadgets/checkbox.gadget", 53, &ChkCls);
	LbLib  = IIntuition->OpenClass("gadgets/listbrowser.gadget", 53, &LbCls);
	LblLib = IIntuition->OpenClass("images/label.image", 53, &LblCls);
	if (LbLib != NULL)
		IListBrowser = (struct ListBrowserIFace *)IExec->GetInterface((struct Library *)LbLib,
		                                                               "main", 1, NULL);
	AslBase = IExec->OpenLibrary("asl.library", 53);
	if (AslBase != NULL)
		IAsl = (struct AslIFace *)IExec->GetInterface(AslBase, "main", 1, NULL);
	return WinCls != NULL && LayCls != NULL && BtnCls != NULL && StrCls != NULL &&
	       ChkCls != NULL && LbCls != NULL && LblCls != NULL && IListBrowser != NULL;
}

static void close_libs(void)
{
	if (IAsl != NULL)
		IExec->DropInterface((struct Interface *)IAsl);
	if (AslBase != NULL)
		IExec->CloseLibrary(AslBase);
	if (IListBrowser != NULL)
		IExec->DropInterface((struct Interface *)IListBrowser);
	if (IIntuition != NULL) {
		IIntuition->CloseClass(LblLib);
		IIntuition->CloseClass(LbLib);
		IIntuition->CloseClass(ChkLib);
		IIntuition->CloseClass(StrLib);
		IIntuition->CloseClass(BtnLib);
		IIntuition->CloseClass(LayLib);
		IIntuition->CloseClass(WinLib);
	}
	req_close_libs();
}

int main(void)
{
	int rc = RETURN_OK, have_list, i;

	memset(&g, 0, sizeof(g));
	g.sel = -1;
	IExec->NewList(&g.labels);
	g.cs = os4_charset(NULL);
	plat_charset = g.cs;
	servers_init(&g.servers);
	servers_init(&g.start);
	if (!open_libs()) {
		printf(PROGNAME ": cannot open the ReAction classes it needs.\n");
		close_libs();
		return RETURN_FAIL;
	}
	have_list = load_from(ENV_DIR, &g.servers, 1) != -ENOENT ||
	            load_from(ENVARC_DIR, &g.servers, 1) != -ENOENT;
	/* first start after 0.37 or older: take the servers over from the
	 * old DOSDrivers */
	if (!have_list)
		import_old(&g.servers);
	if (g.nold == 0)
		for (i = 0; i < g.servers.count; i++)
			servers_add(&g.start, &g.servers.server[i]);
	if (make_window()) {
		select_server(g.servers.count > 0 ? 0 : -1);
		if (g.nold > 0) {
			tell_imported();
		} else {
			/* complaints about the file only now that there is a window */
			server_list l;

			servers_init(&l);
			if (load_from(ENV_DIR, &l, 0) == -ENOENT)
				load_from(ENVARC_DIR, &l, 0);
			servers_free(&l);
		}
		event_loop();
	} else {
		printf(PROGNAME ": cannot open its window.\n");
		rc = RETURN_FAIL;
	}
	if (g.win != NULL)
		IIntuition->DisposeObject(g.win);
	for (i = 0; i < G_COUNT; i++) {
		if (strbufs[i] != NULL)
			memset(strbufs[i], 0, strsizes[i]);   /* passwords */
		free(strbufs[i]);
	}
	IListBrowser->FreeListBrowserList(&g.labels);
	servers_free(&g.servers);
	servers_free(&g.start);
	if (g.amissl > 0)
		amissl_close();
	close_libs();
	return rc;
}
