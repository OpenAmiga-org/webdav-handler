/* nextcloud-handler - filesysbox callbacks. They run in the filesysbox
 * process and in the tasks of programs using the volume, so they use Exec
 * only and pass every call to the handler process (see rpc.h).
 *
 * Compiled with -fno-builtin -fno-tree-loop-distribute-patterns, so that
 * the compiler does not turn loops into C library calls. Public domain. */
#include <time.h>
#define __timespec_defined   /* clib4's struct timespec has the same layout */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/filesysbox.h>
#include <dos/dosextens.h>
#include <errno.h>

#include "rpc.h"

#define ID_NXC_DISK   0x4E584301UL   /* 'NXC\1' */
#define COMMENT_XATTR "user.amiga_comment"

/* errno values as filesysbox.library (built with newlib) reads them */
#define NEWLIB_ENODATA    1061
#define NEWLIB_EOPNOTSUPP 45

struct MsgPort         *rpc_worker_port;
int                     fbx_utf8_names;
struct FileSysBoxIFace *IFileSysBox;
struct Library         *FileSysBoxBase;
struct fuse_context    *_fuse_context_;

static struct DosPacket *startup_pkt;
static char  volname[CONN_VOLUME_NAME_BYTES];
static char  procname[80];
static int   readonly_vol;

static const struct rpc     rpc_zero;
static const struct fbx_stat stat_zero;

/* --- tiny helpers (no C library) ------------------------------------- */

static int streq(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void strcopy(char *dst, const char *src, uint32 size)
{
	uint32 i = 0;

	if (size == 0)
		return;
	while (i + 1 < size && src[i] != '\0') {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int fbxerr(int err)
{
	if (err == -ENODATA)
		return -NEWLIB_ENODATA;
	if (err == -ENOTSUP)
		return -NEWLIB_EOPNOTSUPP;
	return err;
}

static void rpc_init(struct rpc *r, int op, const char *path)
{
	IExec->CopyMem((APTR)&rpc_zero, r, sizeof(*r));
	r->op = op;
	r->path = path;
}

static int rpc_call(struct rpc *r)
{
	struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);

	if (port == NULL)
		return -ENOMEM;
	r->msg.mn_Node.ln_Type = NT_MESSAGE;
	r->msg.mn_ReplyPort = port;
	r->msg.mn_Length = sizeof(*r);
	IExec->PutMsg(rpc_worker_port, &r->msg);
	IExec->WaitPort(port);
	IExec->GetMsg(port);
	IExec->FreeSysObject(ASOT_PORT, port);
	return r->result;
}

static void fill_stat(const ncfs_stat *ns, struct fbx_stat *st)
{
	IExec->CopyMem((APTR)&stat_zero, st, sizeof(*st));
	if (ns->is_dir)
		st->st_mode = S_IFDIR | (ns->readonly || readonly_vol ? 0555 : 0755);
	else
		st->st_mode = S_IFREG | (ns->readonly || readonly_vol ? 0444 : 0644);
	st->st_nlink = 1;
	st->st_size = ns->size;
	st->st_mtime = ns->mtime;
	st->st_ctime = ns->mtime;
	st->st_atime = ns->mtime;
	st->st_blksize = 4096;
	st->st_blocks = (ns->size + 511) >> 9;
}

#define FH(fi) ((ncfs_file *)(uint32)(fi)->fh)

/* --- callbacks --------------------------------------------------------- */

static void *nc_init(struct fuse_conn_info *conn)
{
	struct rpc r;

	/* after "DiskChange" the handler may ask for the password again */
	rpc_init(&r, OP_RESET, NULL);
	rpc_call(&r);
	strcopy(conn->volume_name, volname, CONN_VOLUME_NAME_BYTES);
	return &rpc_worker_port; /* NULL would mean "no disk in drive" */
}

static int nc_getattr(const char *path, struct fbx_stat *st)
{
	struct rpc r;
	int err;

	rpc_init(&r, OP_GETATTR, path);
	err = rpc_call(&r);
	if (err == 0)
		fill_stat(&r.st, st);
	return fbxerr(err);
}

static int nc_fgetattr(const char *path, struct fbx_stat *st, struct fuse_file_info *fi)
{
	struct rpc r;
	int err;

	rpc_init(&r, OP_FGETATTR, path);
	r.fh = FH(fi);
	err = rpc_call(&r);
	if (err == 0)
		fill_stat(&r.st, st);
	return fbxerr(err);
}

static int nc_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      fbx_off_t off, struct fuse_file_info *fi)
{
	struct rpc r;
	struct fbx_stat st;
	int err;

	(void)off;
	(void)fi;
	rpc_init(&r, OP_READDIR, path);
	err = rpc_call(&r);
	if (err == 0 && r.dir != NULL) {
		uint32 pos = 0;

		for (;;) {
			struct rpc_dirent *e = (struct rpc_dirent *)(r.dir + pos);

			fill_stat(&e->st, &st);
			if (filler(buf, e->name, &st, 0) != 0 || e->next == 0)
				break;
			pos = e->next;
		}
	}
	if (r.dir != NULL)
		IExec->FreeVec(r.dir);
	return fbxerr(err);
}

static int nc_open(const char *path, struct fuse_file_info *fi)
{
	struct rpc r;
	int err;

	/* filesysbox truncates separately; fi->flags is O_RDWR or O_RDONLY */
	rpc_init(&r, OP_OPEN, path);
	err = rpc_call(&r);
	if (err == 0)
		fi->fh = (long long)(uint32)r.fh;
	return fbxerr(err);
}

static int nc_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct rpc r;
	int err;

	(void)mode;
	rpc_init(&r, OP_CREATE, path);
	err = rpc_call(&r);
	if (err == 0)
		fi->fh = (long long)(uint32)r.fh;
	return fbxerr(err);
}

static int nc_read(const char *path, char *buf, size_t size, fbx_off_t off,
                   struct fuse_file_info *fi)
{
	struct rpc r;

	rpc_init(&r, OP_READ, path);
	r.fh = FH(fi);
	r.buf = buf;
	r.size = size;
	r.off = off;
	return fbxerr(rpc_call(&r));
}

static int nc_write(const char *path, const char *buf, size_t size, fbx_off_t off,
                    struct fuse_file_info *fi)
{
	struct rpc r;

	rpc_init(&r, OP_WRITE, path);
	r.fh = FH(fi);
	r.cbuf = buf;
	r.size = size;
	r.off = off;
	return fbxerr(rpc_call(&r));
}

static int fh_op(int op, const char *path, struct fuse_file_info *fi)
{
	struct rpc r;

	rpc_init(&r, op, path);
	r.fh = FH(fi);
	return fbxerr(rpc_call(&r));
}

static int nc_flush(const char *path, struct fuse_file_info *fi)
{
	return fh_op(OP_FLUSH, path, fi);
}

static int nc_release(const char *path, struct fuse_file_info *fi)
{
	/* filesysbox ignores the result; a failed upload opens a requester */
	return fh_op(OP_RELEASE, path, fi);
}

static int nc_truncate(const char *path, fbx_off_t size)
{
	struct rpc r;

	rpc_init(&r, OP_TRUNCATE, path);
	r.off = size;
	return fbxerr(rpc_call(&r));
}

static int nc_ftruncate(const char *path, fbx_off_t size, struct fuse_file_info *fi)
{
	struct rpc r;

	rpc_init(&r, OP_FTRUNCATE, path);
	r.fh = FH(fi);
	r.off = size;
	return fbxerr(rpc_call(&r));
}

static int path_op(int op, const char *path)
{
	struct rpc r;

	rpc_init(&r, op, path);
	return fbxerr(rpc_call(&r));
}

static int nc_mkdir(const char *path, mode_t mode)
{
	(void)mode;
	return path_op(OP_MKDIR, path);
}

static int nc_unlink(const char *path)
{
	return path_op(OP_UNLINK, path);
}

static int nc_rmdir(const char *path)
{
	return path_op(OP_RMDIR, path);
}

static int nc_rename(const char *from, const char *to)
{
	struct rpc r;

	rpc_init(&r, OP_RENAME, from);
	r.path2 = to;
	return fbxerr(rpc_call(&r));
}

static int nc_utimens(const char *path, const struct timespec tv[2])
{
	struct rpc r;

	rpc_init(&r, OP_UTIMENS, path);
	r.mtime = (time_t)tv[1].tv_sec;
	return fbxerr(rpc_call(&r));
}

static int nc_chmod(const char *path, mode_t mode)
{
	/* The server keeps no Amiga protection bits; accept the change so that
	 * copying with CLONE works. */
	(void)path;
	(void)mode;
	return 0;
}

/* newlib's struct statvfs, which filesysbox.library hands in; clib4's
 * struct statvfs is larger and laid out differently. */
struct newlib_statvfs {
	unsigned long f_bsize;
	unsigned long f_frsize;
	unsigned long f_blocks;
	unsigned long f_bfree;
	unsigned long f_bavail;
	unsigned long f_files;
	unsigned long f_ffree;
	unsigned long f_favail;
	unsigned long f_fsid;
	unsigned long f_flag;
	unsigned long f_namemax;
};
_Static_assert(sizeof(struct newlib_statvfs) == 44, "newlib statvfs layout");
static const struct newlib_statvfs statvfs_zero;

static int nc_statfs(const char *path, struct statvfs *sv_clib4)
{
	struct newlib_statvfs *sv = (struct newlib_statvfs *)sv_clib4;
	struct rpc r;

	IExec->CopyMem((APTR)&statvfs_zero, sv, sizeof(*sv));
	sv->f_bsize = 4096;
	sv->f_frsize = 4096;
	sv->f_namemax = 255;
	if (readonly_vol)
		sv->f_flag = 1; /* ST_RDONLY */
	rpc_init(&r, OP_STATFS, path);
	rpc_call(&r); /* fails quietly: the volume shows without a size */
	sv->f_blocks = (unsigned long)(r.total >> 12);
	sv->f_bfree = (unsigned long)(r.avail >> 12);
	sv->f_bavail = sv->f_bfree;
	return 0;
}

static int nc_getxattr(const char *path, const char *name, char *value, size_t size)
{
	struct rpc r;

	if (!streq(name, COMMENT_XATTR))
		return -NEWLIB_ENODATA;
	rpc_init(&r, OP_GETCOMMENT, path);
	r.buf = value;
	r.size = size;
	return fbxerr(rpc_call(&r));
}

static int nc_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags)
{
	struct rpc r;
	char buf[256];
	uint32 i;

	(void)flags;
	if (!streq(name, COMMENT_XATTR))
		return 0; /* protection bits h, s, p, a: not stored */
	if (size >= sizeof(buf))
		size = sizeof(buf) - 1;
	for (i = 0; i < size; i++)
		buf[i] = value[i];
	buf[size] = '\0';
	rpc_init(&r, OP_SETCOMMENT, path);
	r.cbuf = buf;
	return fbxerr(rpc_call(&r));
}

static int nc_removexattr(const char *path, const char *name)
{
	struct rpc r;

	if (!streq(name, COMMENT_XATTR))
		return 0;
	rpc_init(&r, OP_SETCOMMENT, path);
	r.cbuf = NULL;
	return fbxerr(rpc_call(&r));
}

static struct fuse_operations nc_ops = {
	.init        = nc_init,
	.getattr     = nc_getattr,
	.fgetattr    = nc_fgetattr,
	.readdir     = nc_readdir,
	.open        = nc_open,
	.create      = nc_create,
	.read        = nc_read,
	.write       = nc_write,
	.flush       = nc_flush,
	.release     = nc_release,
	.truncate    = nc_truncate,
	.ftruncate   = nc_ftruncate,
	.mkdir       = nc_mkdir,
	.unlink      = nc_unlink,
	.rmdir       = nc_rmdir,
	.rename      = nc_rename,
	.utimens     = nc_utimens,
	.chmod       = nc_chmod,
	.statfs      = nc_statfs,
	.getxattr    = nc_getxattr,
	.setxattr    = nc_setxattr,
	.removexattr = nc_removexattr,
};

/* --- the filesysbox process -------------------------------------------- */

static int32 fbx_process(STRPTR args, int32 len, struct ExecBase *sysbase)
{
	struct FbxFS *fs;
	struct rpc r;
	uint32 flags = FBXF_ENABLE_32BIT_UIDS | FBXF_USE_FILL_DIR_STAT;

	(void)args;
	(void)len;
	(void)sysbase;
	if (fbx_utf8_names)
		flags |= FBXF_ENABLE_UTF8_NAMES;
	/* replies the startup packet, with success or failure */
	fs = IFileSysBox->FbxSetupFSTags(startup_pkt->dp_Link, &nc_ops, sizeof(nc_ops), NULL,
		FBXT_FSFLAGS, flags,
		FBXT_DOSTYPE, ID_NXC_DISK,
		FBXT_GET_CONTEXT, &_fuse_context_,
		TAG_END);
	if (fs != NULL) {
		IFileSysBox->FbxEventLoop(fs);
		IFileSysBox->FbxCleanupFS(fs);
	}
	/* tell the handler process to clean up and end */
	rpc_init(&r, OP_FSDONE, NULL);
	rpc_call(&r);
	return 0;
}

int fbx_start(struct DosPacket *pkt, const char *device, const char *volume, int readonly)
{
	struct Process *proc;
	uint32 i;

	FileSysBoxBase = IExec->OpenLibrary("filesysbox.library", 54);
	if (FileSysBoxBase == NULL || !LIB_IS_AT_LEAST(FileSysBoxBase, 54, 8))
		return -1;
	IFileSysBox = (struct FileSysBoxIFace *)IExec->GetInterface(FileSysBoxBase, "main", 1, NULL);
	if (IFileSysBox == NULL)
		return -1;
	/* filesysbox 54.9 (AmigaOS 4.1 FE Update 3) does not convert UTF-8
	 * names to the local character set: without the UTF-8 flag names pass
	 * unchanged and the handler converts them. Newer versions convert. */
	fbx_utf8_names = LIB_IS_AT_LEAST(FileSysBoxBase, 54, 10);
	rpc_worker_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
	if (rpc_worker_port == NULL)
		return -1;

	startup_pkt = pkt;
	readonly_vol = readonly;
	strcopy(volname, volume, sizeof(volname));
	strcopy(procname, device, sizeof(procname) - 20);
	for (i = 0; procname[i] != '\0'; i++)
		;
	strcopy(procname + i, " (filesystem)", sizeof(procname) - i);

	proc = IDOS->CreateNewProcTags(
		NP_Entry,       fbx_process,
		NP_Name,        procname,
		NP_StackSize,   65536,
		NP_Priority,    5,
		NP_Input,       ZERO,
		NP_Output,      ZERO,
		NP_Error,       ZERO,
		NP_CloseInput,  FALSE,
		NP_CloseOutput, FALSE,
		NP_CloseError,  FALSE,
		NP_CurrentDir,  ZERO,
		NP_Cli,         FALSE,
		NP_Child,       FALSE,
		TAG_END);
	return proc != NULL ? 0 : -1;
}

void fbx_stop(void)
{
	if (rpc_worker_port != NULL)
		IExec->FreeSysObject(ASOT_PORT, rpc_worker_port);
	rpc_worker_port = NULL;
	if (IFileSysBox != NULL)
		IExec->DropInterface((struct Interface *)IFileSysBox);
	IFileSysBox = NULL;
	if (FileSysBoxBase != NULL)
		IExec->CloseLibrary(FileSysBoxBase);
	FileSysBoxBase = NULL;
}
