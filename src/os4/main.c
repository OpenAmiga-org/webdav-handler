/* webdav-handler - AmigaOS 4 filesystem handler for WebDAV servers,
 * with the extras of Nextcloud, built on filesysbox.library and clib4. Public domain.
 *
 * DOSDriver (device WDV:):
 *   Handler   = L:webdav-handler
 *   StackSize = 262144
 *   Priority  = 5
 *   GlobVec   = -1
 *
 * The root of the volume lists the servers of the server list (wdvprefs,
 * ENV:WebDAVHandler/servers.xml); a server is connected when its folder
 * is first used.
 *
 * This process (the handler process DOS starts) does all the work: it owns
 * the clib4 context, the network connections and the caches. A second
 * process runs filesysbox; its callbacks send every call here (fbxglue.c,
 * rpc.h).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "charset.h"
#include "ncroot.h"
#include "servers.h"
#include "platform.h"
#include "os4.h"
#include "rpc.h"
#include "version.h"

static const char __attribute__((used)) verstag[] = WDV_VERSTAG;

#define WDV_MAGIC     0x57445653UL   /* 'WDVS' */

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

struct wdv_start {
	struct WBStartup  wbs;
	struct WBArg      arg;
	struct DosPacket *pkt;
	uint32            magic;
};

/* clib4's _start; renamed to clib4_crt_start after linking (tools/os4.mk) */
extern int _start(STRPTR args, int32 arglen, struct Library *sysbase);

/* the clib4.library the program was built against */
#define CLIB4_VERSION  2
#define CLIB4_REVISION 5

/* clib4's startup code ends the program when clib4.library is too old,
 * and DOS would then wait for the handler's answer for ever: a DOSDriver
 * with Activate = 1 stops the boot. So the version is checked here first;
 * without the C library: Exec, DOS and Intuition only. (The same check as
 * in sftp-handler.) */
static int clib4_ok(struct ExecIFace *iexec, struct DosPacket *pkt)
{
	struct Library *lib = iexec->OpenLibrary("clib4.library", 0);
	struct Library *ibase, *dbase;
	struct IntuitionIFace *iint;
	struct DOSIFace *idos;
	int ok = lib != NULL && (lib->lib_Version > CLIB4_VERSION ||
	         (lib->lib_Version == CLIB4_VERSION && lib->lib_Revision >= CLIB4_REVISION));

	if (lib != NULL)
		iexec->CloseLibrary(lib);
	if (ok)
		return 1;
	ibase = iexec->OpenLibrary("intuition.library", 50);
	if (ibase != NULL) {
		iint = (struct IntuitionIFace *)iexec->GetInterface(ibase, "main", 1, NULL);
		if (iint != NULL) {
			struct EasyStruct es;

			es.es_StructSize = sizeof(es);
			es.es_Flags = 0;
			es.es_Title = (STRPTR)WDV_VERS;
			es.es_TextFormat = (STRPTR)"webdav-handler needs clib4.library 2.5 or newer.\n"
			                           "OS4Depot: development/library/misc/clib4.lha";
			es.es_GadgetFormat = (STRPTR)"OK";
			es.es_Screen = NULL;
			es.es_TagList = NULL;
			iint->EasyRequestArgs(NULL, &es, NULL, NULL);
			iexec->DropInterface((struct Interface *)iint);
		}
		iexec->CloseLibrary(ibase);
	}
	dbase = iexec->OpenLibrary("dos.library", 50);
	if (dbase != NULL) {
		idos = (struct DOSIFace *)iexec->GetInterface(dbase, "main", 1, NULL);
		if (idos != NULL) {
			idos->ReplyPkt(pkt, DOSFALSE, ERROR_INVALID_RESIDENT_LIBRARY);
			iexec->DropInterface((struct Interface *)idos);
		}
		iexec->CloseLibrary(dbase);
	}
	return 0;
}

int32 wdv_entry(STRPTR args, int32 arglen, struct Library *sysbase)
{
	struct ExecIFace *iexec = (struct ExecIFace *)((struct ExecBase *)sysbase)->MainInterface;
	struct Process *me = (struct Process *)iexec->FindTask(NULL);
	struct wdv_start st;

	if (me->pr_CLI == ZERO) {
		struct Message *msg;
		struct DosPacket *pkt;

		iexec->WaitPort(&me->pr_MsgPort);
		msg = iexec->GetMsg(&me->pr_MsgPort);
		pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
		if (pkt != NULL && pkt->dp_Link == msg) {
			if (!clib4_ok(iexec, pkt))
				return RETURN_FAIL;
			/* every field set by hand: no memset() before the C library
			 * is ready */
			st.pkt = pkt;
			st.magic = WDV_MAGIC;
			st.arg.wa_Lock = ZERO;
			st.arg.wa_Name = (STRPTR)"webdav-handler";
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
 *
 * The servers are kept in ENV:WebDAVHandler/servers.xml (wdvprefs);
 * the Startup line only holds settings for the whole volume.
 * ------------------------------------------------------------------------ */

static const char template[] =
	"VOLUME/K,CONFIGDIR/K,CACHETIME/K/N,TIMEOUT/K/N,CHUNKSIZE/K/N,TEMPDIR/K,"
	"KEEPDIR/K,CAPATH/K,PROXY/K,NOICONS/S,READONLY/S,CHARSET/K,DEBUG/S";

enum {
	A_VOLUME, A_CONFIGDIR, A_CACHETIME, A_TIMEOUT, A_CHUNKSIZE, A_TEMPDIR,
	A_KEEPDIR, A_CAPATH, A_PROXY, A_NOICONS, A_READONLY, A_CHARSET, A_DEBUG,
	A_COUNT
};

static struct {
	char           device[64];
	char           volume[128];
	char           display[130];
	char           config_dir[256];
	char           save_dir[256];
	char           tempdir[256];
	char           keepdir[256];
	char           capath[256];
	char           proxy[256];
	int            readonly;
	int            convert;   /* names arrive in the local character set */
	int            cs;        /* that character set */
	ncroot_options ro;
	ncroot        *root;
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

/* A Startup line of version 0.37 or older names one server ("URL=...").
 * Those settings now live in the server list. */
static int old_startup(const char *s)
{
	for (; *s != '\0'; s++)
		if (strncasecmp(s, "URL=", 4) == 0 && (s[-1] == ' ' || s[-1] == '"' || s[-1] == '\t'))
			return 1;
	return 0;
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
		return ncroot_getcomment(g.root, path, buf, size);
	n = ncroot_getcomment(g.root, path, tmp, sizeof(tmp) - 1);
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
		err = ncroot_getattr(g.root, r->path, &r->st);
		break;
	case OP_FGETATTR:
		err = ncroot_fgetattr(g.root, r->fh, &r->st);
		break;
	case OP_READDIR: {
		struct dirbuf d;

		memset(&d, 0, sizeof(d));
		err = ncroot_readdir(g.root, r->path, dir_add, &d);
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
		err = ncroot_open(g.root, r->path, 0, &r->fh);
		break;
	case OP_CREATE:
		err = ncroot_create(g.root, r->path, &r->fh);
		break;
	case OP_READ:
		err = ncroot_read(g.root, r->fh, r->buf, r->size, r->off);
		break;
	case OP_WRITE:
		err = ncroot_write(g.root, r->fh, r->cbuf, r->size, r->off);
		break;
	case OP_FLUSH:
		err = ncroot_flush(g.root, r->fh);
		break;
	case OP_RELEASE:
		err = ncroot_release(g.root, r->fh);
		break;
	case OP_TRUNCATE:
		err = ncroot_truncate(g.root, r->path, r->off);
		break;
	case OP_FTRUNCATE:
		err = ncroot_ftruncate(g.root, r->fh, r->off);
		break;
	case OP_MKDIR:
		err = ncroot_mkdir(g.root, r->path);
		break;
	case OP_UNLINK:
		err = ncroot_unlink(g.root, r->path);
		break;
	case OP_RMDIR:
		err = ncroot_rmdir(g.root, r->path);
		break;
	case OP_RENAME:
		err = ncroot_rename(g.root, r->path, r->path2);
		break;
	case OP_UTIMENS:
		err = ncroot_utimens(g.root, r->path, r->mtime);
		break;
	case OP_STATFS:
		r->total = 0;
		r->avail = 0;
		err = ncroot_statfs(g.root, r->path, &r->total, &r->avail);
		break;
	case OP_GETCOMMENT:
		err = get_comment(r->path, r->buf, r->size);
		break;
	case OP_SETCOMMENT:
		err = ncroot_setcomment(g.root, r->path, r->cbuf);
		break;
	case OP_RESET:
		ncroot_reset(g.root);
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

/* DOS keeps a handler's code in dn_SegList for its next start, and removing
 * the device does not unload it: about 3 MB lost with every mount. Instead
 * this process frees its code when it ends, and DOS loads it again for a
 * later start. Returns 1 when the code was taken over. */
static int take_seglist(struct DeviceNode *dn)
{
	struct Process *me = (struct Process *)IExec->FindTask(NULL);
	BPTR seg = IDOS->GetProcSegList(me, GPSLF_SEG);
	int taken = 0;

	IExec->Forbid();
	if (seg != ZERO && dn->dn_SegList == seg && (me->pr_Flags & PRF_FREESEGLIST) == 0) {
		dn->dn_SegList = ZERO;
		me->pr_Flags |= PRF_FREESEGLIST;
		taken = 1;
	}
	IExec->Permit();
	return taken;
}

static int handler_main(struct DosPacket *pkt)
{
	struct DeviceNode *dn = (struct DeviceNode *)BADDR(pkt->dp_Arg3);
	struct RDArgs *rda = NULL;
	char *argbuf = NULL;
	int32 args[A_COUNT];
	const char *startup = "";
	int32 err;
	int seg_taken, noicons;

	seg_taken = take_seglist(dn);
	memset(args, 0, sizeof(args));
	memset(&g, 0, sizeof(g));
	snprintf(g.device, sizeof(g.device), "%s", (const char *)BADDR(dn->dn_Name) + 1);
	if (dn->dn_Startup != ZERO)
		startup = (const char *)BADDR(dn->dn_Startup) + 1;
	dn->dn_Startup = ZERO;

	if (old_startup(startup)) {
		char msg[900];
		snprintf(msg, sizeof(msg),
		         "The DOSDriver %s: was made for nextcloud-handler 0.37\n"
		         "or older: its Startup line names one server (URL=...).\n\n"
		         "webdav-handler shows all your servers in one device, WDV:.\n"
		         "Start SYS:Prefs/wdvprefs: it takes the server over from this\n"
		         "DOSDriver. Then delete the DOSDriver %s and mount\n"
		         "DEVS:DOSDrivers/WDV instead. See WebDAVHandler.guide.",
		         g.device, g.device);
		req_message(WDV_VERS, msg);
		err = ERROR_REQUIRED_ARG_MISSING;
		goto fail;
	}
	err = read_startup(startup, args, &rda, &argbuf);
	if (err != 0) {
		char msg[400];
		snprintf(msg, sizeof(msg),
		         "The Startup line of the DOSDriver %s: is not valid.\n\n"
		         "Expected: %s", g.device, template);
		req_message(WDV_VERS, msg);
		goto fail;
	}

	if (args[A_DEBUG])
		plat_loglevel = LOG_DEBUG;
	plat_log(LOG_DEBUG, "code %s\n", seg_taken ? "freed when the handler ends" : "kept by DOS");
	g.cs = os4_charset(args[A_CHARSET] ? (const char *)args[A_CHARSET] : NULL);
	g.readonly = args[A_READONLY] != 0;
	noicons = args[A_NOICONS] != 0;
	snprintf(g.volume, sizeof(g.volume), "%s",
	         args[A_VOLUME] ? (const char *)args[A_VOLUME] : "WebDAV");
	if (args[A_CONFIGDIR]) {
		/* a drawer of its own: ENV: and ENVARC: copies do not apply */
		snprintf(g.config_dir, sizeof(g.config_dir), "%s", (const char *)args[A_CONFIGDIR]);
	} else {
		snprintf(g.config_dir, sizeof(g.config_dir), "ENV:%s", CFG_DIR);
		snprintf(g.save_dir, sizeof(g.save_dir), "ENVARC:%s", CFG_DIR);
	}
	snprintf(g.tempdir, sizeof(g.tempdir), "%s", args[A_TEMPDIR] ? (const char *)args[A_TEMPDIR] : "T:");
	snprintf(g.keepdir, sizeof(g.keepdir), "%s", args[A_KEEPDIR] ? (const char *)args[A_KEEPDIR] : g.tempdir);
	if (args[A_CAPATH]) {
		snprintf(g.capath, sizeof(g.capath), "%s", (const char *)args[A_CAPATH]);
	} else {
		/* AmiSSL keeps the trusted root certificates in this folder */
		BPTR l = IDOS->Lock("AmiSSL:Certs", SHARED_LOCK);
		if (l != ZERO) {
			IDOS->UnLock(l);
			snprintf(g.capath, sizeof(g.capath), "AmiSSL:Certs");
		}
	}
	if (args[A_PROXY])
		snprintf(g.proxy, sizeof(g.proxy), "%s", (const char *)args[A_PROXY]);
	snprintf(g.display, sizeof(g.display), "%s:", g.volume);

	memset(&g.ro, 0, sizeof(g.ro));
	g.ro.config_dir = g.config_dir;
	g.ro.save_dir = g.save_dir[0] != '\0' ? g.save_dir : NULL;
	g.ro.icon_file = noicons ? NULL : "ENV:Sys/def_drawer.info";
	g.ro.tempdir = g.tempdir;
	g.ro.keepdir = g.keepdir;
	g.ro.display = g.display;
	g.ro.ca_path = g.capath[0] != '\0' ? g.capath : NULL;
	g.ro.proxy = g.proxy[0] != '\0' ? g.proxy : NULL;
	g.ro.user_agent = WDV_VERS " (AmigaOS 4)";
	g.ro.cache_ms = args[A_CACHETIME] ? *(int32 *)args[A_CACHETIME] * 1000 : 5000;
	g.ro.connect_timeout = 20;
	g.ro.io_timeout = args[A_TIMEOUT] ? *(int32 *)args[A_TIMEOUT] : 60;
	g.ro.chunk_size = args[A_CHUNKSIZE] ? (int64_t)*(int32 *)args[A_CHUNKSIZE] * 1024 * 1024 : 0;
	g.ro.readonly = g.readonly;

	IDOS->FreeArgs(rda);
	IDOS->FreeDosObject(DOS_RDARGS, rda);
	rda = NULL;
	free(argbuf);
	argbuf = NULL;

	if (amissl_open() != 0) {
		req_message(WDV_VERS, "This handler needs AmiSSL 5 or 6 (and a TCP/IP stack).\n"
		            "AmiSSL: https://github.com/jens-maus/amissl/releases\n"
		            "or OS4Depot: library/misc/amissl.lha");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}
	plat_log(LOG_INFO, "TLS: %s (AmiSSL %d interface)\n", amissl_name(), amissl_version());

	/* requester texts are always in the local character set */
	plat_charset = g.cs;
	g.root = ncroot_new(&g.ro);
	if (g.root == NULL) {
		err = ERROR_NO_FREE_STORE;
		goto fail;
	}
	plat_log(LOG_INFO, "%s: servers from %s/%s\n", g.device, g.config_dir, CFG_SERVERS);

	if (fbx_start(pkt, g.device, g.volume, g.readonly) != 0) {
		req_message(WDV_VERS, "This handler needs filesysbox.library 54.8 or newer.");
		err = ERROR_INVALID_RESIDENT_LIBRARY;
		goto fail;
	}
	pkt = NULL; /* the filesysbox process replies it */
	/* The network work happens here. Below the TCP/IP stack's priority so
	 * that waiting for data can never hold the stack up; the filesysbox
	 * process keeps the handler priority. */
	IExec->SetTaskPri(IExec->FindTask(NULL), -1);
	g.convert = !fbx_utf8_names;
	plat_log(LOG_INFO, "file names: %s (%s)\n", g.convert ?
	         "converted by the handler" : "converted by filesysbox",
	         g.cs == CS_ISO_8859_15 ? "ISO-8859-15" : "ISO-8859-1");
	serve_requests();
	fbx_wait(); /* its code goes away with ours */
	err = 0;

fail:
	if (pkt != NULL)
		IDOS->ReplyPkt(pkt, DOSFALSE, err);
	if (rda != NULL) {
		IDOS->FreeArgs(rda);
		IDOS->FreeDosObject(DOS_RDARGS, rda);
	}
	free(argbuf);
	ncroot_free(g.root);
	g.root = NULL;
	fbx_stop();
	amissl_close();
	req_close_libs();
	return err == 0 ? RETURN_OK : RETURN_FAIL;
}

int main(int argc, char **argv)
{
	struct wdv_start *st = (struct wdv_start *)argv;

	if (argc > 0) {
		printf("%s (%s)\n"
		       "A filesystem handler for WebDAV servers, Nextcloud included.\n"
		       "It is started by mounting a DOSDriver, see WebDAVHandler.guide.\n",
		       WDV_VERS, WDV_DATE);
		return RETURN_OK;
	}
	if (st == NULL || st->magic != WDV_MAGIC)
		return RETURN_FAIL; /* Workbench start */
	signal(SIGINT, SIG_IGN);
	return handler_main(st->pkt);
}
