/* nextcloud-handler - AmigaOS 4 filesystem handler for Nextcloud and other
 * WebDAV servers, built on filesysbox.library and clib4. Public domain.
 *
 * DOSDriver:
 *   Handler   = L:nextcloud-handler
 *   StackSize = 262144
 *   Priority  = 5
 *   GlobVec   = -1
 *   Startup   = "URL=https://cloud.example.com USER=name VOLUME=Nextcloud"
 *
 * This process (the handler process DOS starts) does all the work: it owns
 * the clib4 context, the network connection and the cache. A second process
 * runs filesysbox; its callbacks send every call here (fbxglue.c, rpc.h).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "charset.h"
#include "ncfs.h"
#include "platform.h"
#include "os4.h"
#include "rpc.h"
#include "version.h"

static const char __attribute__((used)) verstag[] = NCH_VERSTAG;

#define NCH_MAGIC     0x4E434853UL   /* 'NCHS' */
#define RETRY_MS      10000          /* between failed connection attempts */

/* ------------------------------------------------------------------------
 * Startup
 *
 * clib4's startup code takes the first message on the process port as a
 * Workbench startup message when there is no CLI, and replies it when the
 * program ends. A handler is started by DOS with a startup packet there
 * instead, and filesysbox replies that packet itself. So this function runs
 * first (tools/os4.mk renames it to _start, where AmigaOS starts a program):
 * it takes the packet off the port and puts a harmless Workbench message
 * there for clib4, one without a reply port. It must not use the C library.
 * ------------------------------------------------------------------------ */

struct nch_start {
	struct WBStartup  wbs;
	struct WBArg      arg;
	struct DosPacket *pkt;
	uint32            magic;
};

/* clib4's _start; renamed to clib4_crt_start after linking (tools/os4.mk) */
extern int _start(STRPTR args, int32 arglen, struct Library *sysbase);

int32 nch_entry(STRPTR args, int32 arglen, struct Library *sysbase)
{
	struct ExecIFace *iexec = (struct ExecIFace *)((struct ExecBase *)sysbase)->MainInterface;
	struct Process *me = (struct Process *)iexec->FindTask(NULL);
	struct nch_start st;

	if (me->pr_CLI == ZERO) {
		struct Message *msg;
		struct DosPacket *pkt;

		iexec->WaitPort(&me->pr_MsgPort);
		msg = iexec->GetMsg(&me->pr_MsgPort);
		pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
		if (pkt != NULL && pkt->dp_Link == msg) {
			/* every field set by hand: no memset() before the C library
			 * is ready */
			st.pkt = pkt;
			st.magic = NCH_MAGIC;
			st.arg.wa_Lock = ZERO;
			st.arg.wa_Name = (STRPTR)"nextcloud-handler";
			st.wbs.sm_Message.mn_Node.ln_Succ = NULL;
			st.wbs.sm_Message.mn_Node.ln_Pred = NULL;
			st.wbs.sm_Message.mn_Node.ln_Type = NT_MESSAGE;
			st.wbs.sm_Message.mn_Node.ln_Pri = 0;
			st.wbs.sm_Message.mn_Node.ln_Name = NULL;
			st.wbs.sm_Message.mn_ReplyPort = NULL;
			st.wbs.sm_Message.mn_Length = sizeof(struct WBStartup);
			st.wbs.sm_Process = &me->pr_MsgPort;
			st.wbs.sm_Segment = ZERO;
			st.wbs.sm_NumArgs = 1;
			st.wbs.sm_ToolWindow = (STRPTR)"NIL:";
			st.wbs.sm_ArgList = &st.arg;
			msg = &st.wbs.sm_Message;
		}
		iexec->PutMsg(&me->pr_MsgPort, msg);
	}
	return _start(args, arglen, sysbase);
}

/* ------------------------------------------------------------------------
 * Settings
 * ------------------------------------------------------------------------ */

static const char template[] =
	"URL/A,USER/K,PASSWORD/K,VOLUME/K,CACHETIME/K/N,TEMPDIR/K,KEEPDIR/K,"
	"CAFILE/K,CAPATH/K,INSECURE/S,PROXY/K,TIMEOUT/K/N,CHUNKSIZE/K/N,"
	"READONLY/S,NOPASSWORDREQ/S,CHARSET/K,DEBUG/S";

enum {
	A_URL, A_USER, A_PASSWORD, A_VOLUME, A_CACHETIME, A_TEMPDIR, A_KEEPDIR,
	A_CAFILE, A_CAPATH, A_INSECURE, A_PROXY, A_TIMEOUT, A_CHUNKSIZE,
	A_READONLY, A_NOPASSWORDREQ, A_CHARSET, A_DEBUG, A_COUNT
};

static struct {
	char         device[64];
	char         volume[128];
	char         display[130];
	char         url[512];
	char         user[128];
	char         password[256];
	int          have_password;
	int          ask_password;
	int          cancelled;  /* the password requester was cancelled */
	int          readonly;
	int          convert;   /* names arrive in the local character set */
	int          cs;        /* that character set */
	dav_config   dc;
	ncfs_options fo;
	dav_session *dav;
	ncfs        *fs;
	int          connected;
	int64_t      last_try;
	int          last_err;
} g;

static void strip_quotes(char *s)
{
	size_t n;
	char *p = s;

	while (isspace((unsigned char)*p))
		p++;
	memmove(s, p, strlen(p) + 1);
	n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';
	}
}

static char *dupstr(const char *s)
{
	return s != NULL ? strdup(s) : NULL;
}

/* ENV:NextcloudHandler/<device>.auth, then ENVARC: */
static int load_password(void)
{
	static const char *dirs[] = { "ENV:", "ENVARC:" };
	char path[256];
	int i;

	for (i = 0; i < 2; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%sNextcloudHandler/%s.auth", dirs[i], g.device);
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		if (fgets(g.password, sizeof(g.password), f) != NULL) {
			g.password[strcspn(g.password, "\r\n")] = '\0';
			fclose(f);
			return g.password[0] != '\0';
		}
		fclose(f);
	}
	return 0;
}

static void save_password(void)
{
	static const char *dirs[] = { "ENVARC:", "ENV:" };
	char path[256];
	int i;

	for (i = 0; i < 2; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%sNextcloudHandler", dirs[i]);
		mkdir(path, 0700);
		snprintf(path, sizeof(path), "%sNextcloudHandler/%s.auth", dirs[i], g.device);
		f = fopen(path, "w");
		if (f == NULL) {
			plat_log(LOG_ERR, "cannot save the password in %s\n", path);
			continue;
		}
		fprintf(f, "%s\n", g.password);
		fclose(f);
	}
}

/* Connects on first use, so that the volume can be mounted at boot before
 * the network is up. Returns 0 or -errno. */
static int online(void)
{
	const char *problem = NULL;
	int tries, err = -ENODEV, remember = 0;

	if (g.connected)
		return 0;
	if (g.last_try != 0 && plat_ms() - g.last_try < RETRY_MS)
		return g.last_err;

	for (tries = 0; tries < 3; tries++) {
		if (!g.have_password && g.user[0] != '\0') {
			if (!g.ask_password || g.cancelled) {
				err = -EACCES;
				goto fail;
			}
			if (!req_password(g.device, g.user, g.url, problem, g.password,
			                  sizeof(g.password), &remember)) {
				/* Workbench looks at the volume all the time: do not ask
				 * again until "DiskChange" or a new mount */
				g.cancelled = 1;
				err = -EACCES;
				goto fail;
			}
			g.have_password = 1;
		}
		dav_set_password(g.dav, g.password);
		err = dav_connect(g.dav);
		if (err == 0) {
			/* saved only once the server has accepted it */
			if (remember)
				save_password();
			g.connected = 1;
			g.last_try = 0;
			g.last_err = 0;
			return 0;
		}
		if (dav_last_status(g.dav) == 401 && g.ask_password) {
			/* wrong password: ask again */
			g.have_password = 0;
			problem = "The server did not accept the password.";
			continue;
		}
		break;
	}
fail:
	g.last_try = plat_ms();
	/* AmigaDOS shows "no disk": the volume is there but not reachable or
	 * not logged in; the next access tries again */
	g.last_err = -ENODEV;
	plat_log(LOG_ERR, "cannot connect to %s: %s\n", g.url, dav_error(g.dav));
	return g.last_err;
}

/* ------------------------------------------------------------------------
 * Requests from the filesysbox callbacks
 * ------------------------------------------------------------------------ */

struct dirbuf {
	uint8  *data;
	uint32  len;
	uint32  cap;
	uint32  last;   /* offset of the last entry */
	int     failed;
};

static int dir_add(void *ud, const char *uname, const ncfs_stat *st)
{
	struct dirbuf *d = ud;
	char *lname = g.convert ? cs_utf8_to_name(g.cs, uname) : NULL;
	const char *name = lname != NULL ? lname : uname;
	uint32 need = (offsetof(struct rpc_dirent, name) + strlen(name) + 1 + 7) & ~7U;
	struct rpc_dirent *e;

	if (d->len + need > d->cap) {
		uint32 ncap = d->cap ? d->cap * 2 : 8192;
		uint8 *n;

		while (ncap < d->len + need)
			ncap *= 2;
		/* Exec memory: the caller frees it with FreeVec() in another task */
		n = IExec->AllocVecTags(ncap, AVT_Type, MEMF_SHARED, TAG_END);
		if (n == NULL) {
			free(lname);
			d->failed = 1;
			return 1;
		}
		if (d->data != NULL) {
			memcpy(n, d->data, d->len);
			IExec->FreeVec(d->data);
		}
		d->data = n;
		d->cap = ncap;
	}
	e = (struct rpc_dirent *)(d->data + d->len);
	e->next = 0;
	e->st = *st;
	strcpy(e->name, name);
	if (d->len > 0)
		((struct rpc_dirent *)(d->data + d->last))->next = d->len;
	d->last = d->len;
	d->len += need;
	free(lname);
	return 0;
}

static int logret(struct rpc *r, int err)
{
	if (plat_loglevel >= LOG_DEBUG)
		plat_log(LOG_DEBUG, "op %d %s -> %d\n", r->op, r->path != NULL ? r->path : "", err);
	return err;
}

static int get_comment(const char *path, char *buf, uint32 size)
{
	char tmp[512];
	char *local;
	int n;

	if (!g.convert)
		return ncfs_getcomment(g.fs, path, buf, size);
	n = ncfs_getcomment(g.fs, path, tmp, sizeof(tmp) - 1);
	if (n < 0)
		return n;
	tmp[n] = '\0';
	local = cs_utf8_to_local(g.cs, tmp);
	if (local == NULL)
		return -ENOMEM;
	n = (int)strlen(local);
	if ((uint32)n > size)
		n = (int)size;
	memcpy(buf, local, n);
	free(local);
	return n;
}

void rpc_serve(struct rpc *r)
{
	char *conv1 = NULL, *conv2 = NULL, *convc = NULL;
	int err = 0;

	/* names from filesysbox in the local character set */
	if (g.convert) {
		if (r->path != NULL && (conv1 = cs_local_to_utf8(g.cs, r->path)) != NULL)
			r->path = conv1;
		if (r->path2 != NULL && (conv2 = cs_local_to_utf8(g.cs, r->path2)) != NULL)
			r->path2 = conv2;
		if (r->op == OP_SETCOMMENT && r->cbuf != NULL &&
		    (convc = cs_local_to_utf8(g.cs, r->cbuf)) != NULL)
			r->cbuf = convc;
	}

	switch (r->op) {
	case OP_GETATTR:
		err = online();
		if (err != 0 && r->path != NULL && strcmp(r->path, "/") == 0) {
			/* filesysbox only creates the volume when the root can be
			 * examined: show an empty root while the server is away */
			memset(&r->st, 0, sizeof(r->st));
			r->st.is_dir = 1;
			err = 0;
		} else if (err == 0) {
			err = ncfs_getattr(g.fs, r->path, &r->st);
		}
		break;
	case OP_FGETATTR:
		err = ncfs_fgetattr(g.fs, r->fh, &r->st);
		break;
	case OP_READDIR: {
		struct dirbuf d;

		memset(&d, 0, sizeof(d));
		err = online();
		if (err == 0)
			err = ncfs_readdir(g.fs, r->path, dir_add, &d);
		if (err == 0 && d.failed)
			err = -ENOMEM;
		if (err != 0 || d.len == 0) {
			if (d.data != NULL)
				IExec->FreeVec(d.data);
			d.data = NULL;
		}
		r->dir = d.data;
		break;
	}
	case OP_OPEN:
		err = online();
		if (err == 0)
			err = ncfs_open(g.fs, r->path, 0, &r->fh);
		break;
	case OP_CREATE:
		err = online();
		if (err == 0)
			err = ncfs_create(g.fs, r->path, &r->fh);
		break;
	case OP_READ:
		err = ncfs_read(g.fs, r->fh, r->buf, r->size, r->off);
		break;
	case OP_WRITE:
		err = ncfs_write(g.fs, r->fh, r->cbuf, r->size, r->off);
		break;
	case OP_FLUSH:
		err = ncfs_flush(g.fs, r->fh);
		break;
	case OP_RELEASE:
		err = ncfs_release(g.fs, r->fh);
		break;
	case OP_TRUNCATE:
		err = online();
		if (err == 0)
			err = ncfs_truncate(g.fs, r->path, r->off);
		break;
	case OP_FTRUNCATE:
		err = ncfs_ftruncate(g.fs, r->fh, r->off);
		break;
	case OP_MKDIR:
		err = online();
		if (err == 0)
			err = ncfs_mkdir(g.fs, r->path);
		break;
	case OP_UNLINK:
		err = online();
		if (err == 0)
			err = ncfs_unlink(g.fs, r->path);
		break;
	case OP_RMDIR:
		err = online();
		if (err == 0)
			err = ncfs_rmdir(g.fs, r->path);
		break;
	case OP_RENAME:
		err = online();
		if (err == 0)
			err = ncfs_rename(g.fs, r->path, r->path2);
		break;
	case OP_UTIMENS:
		err = online();
		if (err == 0)
			err = ncfs_utimens(g.fs, r->path, r->mtime);
		break;
	case OP_STATFS:
		r->total = 0;
		r->avail = 0;
		err = online();
		if (err == 0)
			err = ncfs_statfs(g.fs, &r->total, &r->avail);
		break;
	case OP_GETCOMMENT:
		err = online();
		if (err == 0)
			err = get_comment(r->path, r->buf, r->size);
		break;
	case OP_SETCOMMENT:
		err = online();
		if (err == 0)
			err = ncfs_setcomment(g.fs, r->path, r->cbuf);
		break;
	case OP_RESET:
		g.cancelled = 0;
		g.last_try = 0;
		err = 0;
		break;
	default:
		err = -ENOSYS;
		break;
	}
	r->result = logret(r, err);
	/* the caller's strings are back in place for the reply */
	free(conv1);
	free(conv2);
	free(convc);
}

/* ------------------------------------------------------------------------
 * Handler
 * ------------------------------------------------------------------------ */

static int read_startup(const char *startup, int32 *args, struct RDArgs **rda_out,
                        char **buf_out)
{
	struct RDArgs *rda;
	char *buf;
	size_t n = strlen(startup);

	*rda_out = NULL;
	*buf_out = NULL;
	buf = malloc(n + 2);
	if (buf == NULL)
		return ERROR_NO_FREE_STORE;
	strcpy(buf, startup);
	strip_quotes(buf);
	strcat(buf, "\n");
	rda = IDOS->AllocDosObject(DOS_RDARGS, NULL);
	if (rda == NULL) {
		free(buf);
		return ERROR_NO_FREE_STORE;
	}
	rda->RDA_Source.CS_Buffer = (STRPTR)buf;
	rda->RDA_Source.CS_Length = strlen(buf);
	rda->RDA_Flags = RDAF_NOPROMPT;
	if (IDOS->ReadArgs(template, args, rda) == NULL) {
		int32 err = IDOS->IoErr();
		IDOS->FreeDosObject(DOS_RDARGS, rda);
		free(buf);
		return err != 0 ? err : ERROR_REQUIRED_ARG_MISSING;
	}
	/* keep buf until FreeArgs(): the results may point into it */
	*rda_out = rda;
	*buf_out = buf;
	return 0;
}

static void serve_requests(void)
{
	int done = 0;

	while (!done) {
		struct rpc *r;

		IExec->WaitPort(rpc_worker_port);
		while ((r = (struct rpc *)IExec->GetMsg(rpc_worker_port)) != NULL) {
			if (r->op == OP_FSDONE) {
				r->result = 0;
				done = 1;
			} else {
				rpc_serve(r);
			}
			IExec->ReplyMsg(&r->msg);
		}
	}
}

static int handler_main(struct DosPacket *pkt)
{
	struct DeviceNode *dn = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
	struct RDArgs *rda = NULL;
	char *argbuf = NULL;
	int32 args[A_COUNT];
	const char *startup = "";
	char *capath_default = NULL;
	int32 err;

	memset(args, 0, sizeof(args));
	memset(&g, 0, sizeof(g));
	snprintf(g.device, sizeof(g.device), "%s", (const char *)BADDR(dn->dn_Name) + 1);
	if (dn->dn_Startup != ZERO)
		startup = (const char *)BADDR(dn->dn_Startup) + 1;
	dn->dn_Startup = ZERO;

	err = read_startup(startup, args, &rda, &argbuf);
	if (err != 0) {
		char msg[400];
		snprintf(msg, sizeof(msg),
		         "The Startup line of the DOSDriver %s: is not valid.\n\n"
		         "Expected: %s", g.device, template);
		req_message(NCH_VERS, msg);
		goto fail;
	}

	if (args[A_DEBUG])
		plat_loglevel = LOG_DEBUG;
	snprintf(g.url, sizeof(g.url), "%s", (const char *)args[A_URL]);
	if (args[A_USER])
		snprintf(g.user, sizeof(g.user), "%s", (const char *)args[A_USER]);
	if (args[A_PASSWORD]) {
		snprintf(g.password, sizeof(g.password), "%s", (const char *)args[A_PASSWORD]);
		g.have_password = 1;
	} else {
		g.have_password = load_password();
	}
	g.ask_password = !args[A_NOPASSWORDREQ];
	g.cs = cs_from_name(args[A_CHARSET] ? (const char *)args[A_CHARSET] : getenv("CHARSET"));
	g.readonly = args[A_READONLY] != 0;
	snprintf(g.volume, sizeof(g.volume), "%s",
	         args[A_VOLUME] ? (const char *)args[A_VOLUME] : "Nextcloud");

	g.dc.url = g.url;
	g.dc.user = g.user[0] != '\0' ? g.user : NULL;
	g.dc.password = g.password;
	g.dc.insecure = args[A_INSECURE] != 0;
	g.dc.ca_file = dupstr((const char *)args[A_CAFILE]);
	g.dc.ca_path = dupstr((const char *)args[A_CAPATH]);
	if (g.dc.ca_file == NULL && g.dc.ca_path == NULL) {
		/* AmiSSL keeps the trusted root certificates in this folder */
		BPTR l = IDOS->Lock("AmiSSL:Certs", SHARED_LOCK);
		if (l != ZERO) {
			IDOS->UnLock(l);
			capath_default = strdup("AmiSSL:Certs");
			g.dc.ca_path = capath_default;
		}
	}
	g.dc.proxy = dupstr((const char *)args[A_PROXY]);
	g.dc.connect_timeout = 20;
	g.dc.io_timeout = args[A_TIMEOUT] ? *(int32 *)args[A_TIMEOUT] : 60;
	g.dc.chunk_size = args[A_CHUNKSIZE] ? (int64_t)*(int32 *)args[A_CHUNKSIZE] * 1024 * 1024 : 0;
	g.dc.user_agent = NCH_VERS " (AmigaOS 4)";

	g.fo.tempdir = dupstr(args[A_TEMPDIR] ? (const char *)args[A_TEMPDIR] : "T:");
	g.fo.keepdir = dupstr(args[A_KEEPDIR] ? (const char *)args[A_KEEPDIR] : g.fo.tempdir);
	g.fo.cache_ms = args[A_CACHETIME] ? *(int32 *)args[A_CACHETIME] * 1000 : 5000;
	g.fo.readonly = g.readonly;
	snprintf(g.display, sizeof(g.display), "%s:", g.volume);
	g.fo.display = g.display;

	IDOS->FreeArgs(rda);
	IDOS->FreeDosObject(DOS_RDARGS, rda);
	rda = NULL;
	free(argbuf);
	argbuf = NULL;

	if (amissl_open() != 0) {
		req_message(NCH_VERS, "This handler needs AmiSSL 5 or newer (and a TCP/IP stack).\n"
		            "AmiSSL: https://github.com/jens-maus/amissl/releases\n"
		            "or OS4Depot: library/misc/amissl.lha");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}

	g.dav = dav_new(&g.dc);
	g.fs = g.dav != NULL ? ncfs_new(g.dav, &g.fo) : NULL;
	if (g.fs == NULL) {
		err = ERROR_NO_FREE_STORE;
		goto fail;
	}
	plat_log(LOG_INFO, "%s: %s, user %s\n", g.device, g.url, g.user);

	if (fbx_start(pkt, g.device, g.volume, g.readonly) != 0) {
		req_message(NCH_VERS, "This handler needs filesysbox.library 54.8 or newer.");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}
	pkt = NULL; /* the filesysbox process replies it */
	/* The network work happens here. Below the TCP/IP stack's priority so
	 * that waiting for data can never hold the stack up; the filesysbox
	 * process keeps the handler priority. */
	IExec->SetTaskPri(IExec->FindTask(NULL), -1);
	g.convert = !fbx_utf8_names;
	plat_charset = g.cs; /* requester texts are always in the local set */
	plat_log(LOG_INFO, "file names: %s\n", g.convert ?
	         "converted by the handler" : "converted by filesysbox");
	serve_requests();
	err = 0;

fail:
	if (pkt != NULL)
		IDOS->ReplyPkt(pkt, DOSFALSE, err);
	if (rda != NULL) {
		IDOS->FreeArgs(rda);
		IDOS->FreeDosObject(DOS_RDARGS, rda);
	}
	free(argbuf);
	ncfs_free(g.fs);
	dav_free(g.dav);
	free((char *)g.dc.ca_file);
	free(capath_default != NULL ? capath_default : (char *)g.dc.ca_path);
	free((char *)g.dc.proxy);
	free((char *)g.fo.tempdir);
	free((char *)g.fo.keepdir);
	memset(g.password, 0, sizeof(g.password));
	fbx_stop();
	amissl_close();
	req_close_libs();
	return err == 0 ? RETURN_OK : RETURN_FAIL;
}

int main(int argc, char **argv)
{
	struct nch_start *st = (struct nch_start *)argv;

	if (argc > 0) {
		printf("%s (%s)\n"
		       "A filesystem handler for Nextcloud and WebDAV servers.\n"
		       "It is started by mounting a DOSDriver, see NextcloudHandler.guide.\n",
		       NCH_VERS, NCH_DATE);
		return RETURN_OK;
	}
	if (st == NULL || st->magic != NCH_MAGIC)
		return RETURN_FAIL; /* Workbench start */
	signal(SIGINT, SIG_IGN);
	return handler_main(st->pkt);
}
