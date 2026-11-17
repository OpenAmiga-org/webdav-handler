/* nchtest - checks AmigaDOS behaviour of a webdav-handler volume.
 * Test tool, not part of the release. Public domain.
 *
 *   nchtest DIR       DIR is created on the volume, e.g. NC1:nchtest
 *
 * Prints one FAIL line per failed check and a summary. Uses dos.library
 * directly, the way programs do. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/notify.h>
#include <dos/obsolete.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
static char base[256];

#define CHECK(cond, ...) do { \
	int ok_ = (cond); long err_ = (long)IDOS->IoErr(); \
	if (ok_) { pass++; } else { \
		fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
		printf(" (IoErr %ld)\n", err_); } } while (0)

static char *P(const char *name)
{
	static char buf[4][600];
	static int i;
	char *b = buf[i++ & 3];

	snprintf(b, 600, "%s/%s", base, name);
	return b;
}

static unsigned char pat(int64 i, int seed)
{
	return (unsigned char)((i * 31 + seed * 7 + (i >> 9)) & 0xFF);
}

/* writes size bytes of pattern seed, in pieces of chunk bytes */
static int write_pattern(const char *name, int64 size, int seed, int chunk)
{
	BPTR fh = IDOS->Open(name, MODE_NEWFILE);
	unsigned char *buf;
	int64 done = 0;
	int ok = 1;

	if (fh == ZERO)
		return 0;
	buf = malloc(chunk);
	while (done < size) {
		int n = size - done < chunk ? (int)(size - done) : chunk;
		int i;
		for (i = 0; i < n; i++)
			buf[i] = pat(done + i, seed);
		if (IDOS->Write(fh, buf, n) != n) {
			ok = 0;
			break;
		}
		done += n;
	}
	free(buf);
	if (!IDOS->Close(fh))
		ok = 0;
	return ok;
}

/* checks that the file holds exactly size bytes of pattern seed */
static int check_pattern(const char *name, int64 size, int seed)
{
	BPTR fh = IDOS->Open(name, MODE_OLDFILE);
	unsigned char buf[8192];
	int64 pos = 0;
	int n, i;

	if (fh == ZERO)
		return 0;
	while ((n = IDOS->Read(fh, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++) {
			if (buf[i] != pat(pos + i, seed)) {
				printf("  %s differs at %lld\n", name, (long long)(pos + i));
				IDOS->Close(fh);
				return 0;
			}
		}
		pos += n;
	}
	IDOS->Close(fh);
	if (pos != size)
		printf("  %s has %lld bytes, expected %lld\n", name, (long long)pos, (long long)size);
	return n == 0 && pos == size;
}

static int64 file_size(const char *name)
{
	struct ExamineData *ed = IDOS->ExamineObjectTags(EX_StringNameInput, name, TAG_END);
	int64 s;

	if (ed == NULL)
		return -1;
	s = EXD_IS_FILE(ed) ? ed->FileSize : -2;
	IDOS->FreeDosObject(DOS_EXAMINEDATA, ed);
	return s;
}

/* exact (case-sensitive) name in a directory listing */
static int dir_has(const char *dir, const char *name)
{
	APTR ctx = IDOS->ObtainDirContextTags(EX_StringNameInput, dir, EX_DataFields, EXF_NAME, TAG_END);
	struct ExamineData *ed;
	int found = 0;

	if (ctx == NULL)
		return 0;
	while ((ed = IDOS->ExamineDir(ctx)) != NULL)
		if (strcmp(ed->Name, name) == 0)
			found = 1;
	IDOS->ReleaseDirContext(ctx);
	return found;
}

/* CreateDir() returns a lock, which must be given back */
static int md(const char *name)
{
	BPTR l = IDOS->CreateDir(name);

	if (l == ZERO)
		return 0;
	IDOS->UnLock(l);
	return 1;
}

static int exists(const char *name)
{
	BPTR l = IDOS->Lock(name, SHARED_LOCK);

	if (l == ZERO)
		return 0;
	IDOS->UnLock(l);
	return 1;
}

/* ------------------------------------------------------------------ */

static void t_sizes(void)
{
	static const int64 sizes[] = { 0, 1, 4095, 4096, 65537, 1048583 };
	int i;
	char name[64];

	for (i = 0; i < 6; i++) {
		snprintf(name, sizeof(name), "size_%lld", (long long)sizes[i]);
		CHECK(write_pattern(P(name), sizes[i], i, 7777), "write %s", name);
		CHECK(file_size(P(name)) == sizes[i], "examine size of %s: %lld", name,
		      (long long)file_size(P(name)));
		CHECK(check_pattern(P(name), sizes[i], i), "content of %s", name);
	}
}

static void t_seek(void)
{
	const int64 size = 300000;
	BPTR fh;
	unsigned char buf[1000];
	uint32 r = 12345;
	int k, i, ok;

	CHECK(write_pattern(P("seek"), size, 3, 65536), "write");
	fh = IDOS->Open(P("seek"), MODE_OLDFILE);
	CHECK(fh != ZERO, "open");
	if (fh == ZERO)
		return;
	for (k = 0; k < 40; k++) {
		int64 off;
		r = r * 1103515245 + 12345;
		off = (r >> 4) % (size - 1000);
		IDOS->ChangeFilePosition(fh, off, OFFSET_BEGINNING);
		CHECK(IDOS->GetFilePosition(fh) == off, "position after seek to %lld", (long long)off);
		ok = IDOS->Read(fh, buf, 1000) == 1000;
		for (i = 0; ok && i < 1000; i++)
			ok = buf[i] == pat(off + i, 3);
		CHECK(ok, "read 1000 at %lld", (long long)off);
	}
	IDOS->ChangeFilePosition(fh, -10, OFFSET_END);
	CHECK(IDOS->Read(fh, buf, 100) == 10, "read last 10 bytes");
	CHECK(buf[9] == pat(size - 1, 3), "last byte");
	CHECK(IDOS->Read(fh, buf, 100) == 0, "read at end gives 0");
	IDOS->ChangeFilePosition(fh, 100, OFFSET_BEGINNING);
	IDOS->ChangeFilePosition(fh, 50, OFFSET_CURRENT);
	CHECK(IDOS->GetFilePosition(fh) == 150, "seek relative");
	IDOS->Close(fh);
}

static void t_modes(void)
{
	BPTR fh;
	unsigned char x[100], buf[16];

	/* MODE_READWRITE on an existing file: change the middle, append */
	CHECK(write_pattern(P("rw"), 10000, 4, 4096), "write");
	fh = IDOS->Open(P("rw"), MODE_READWRITE);
	CHECK(fh != ZERO, "open rw");
	if (fh != ZERO) {
		memset(x, 'X', sizeof(x));
		IDOS->ChangeFilePosition(fh, 5000, OFFSET_BEGINNING);
		CHECK(IDOS->Write(fh, x, 100) == 100, "write middle");
		IDOS->ChangeFilePosition(fh, 0, OFFSET_END);
		CHECK(IDOS->Write(fh, x, 50) == 50, "append");
		CHECK(IDOS->Close(fh), "close");
	}
	CHECK(file_size(P("rw")) == 10050, "size after rw: %lld", (long long)file_size(P("rw")));
	fh = IDOS->Open(P("rw"), MODE_OLDFILE);
	if (fh != ZERO) {
		IDOS->ChangeFilePosition(fh, 4998, OFFSET_BEGINNING);
		IDOS->Read(fh, buf, 4);
		CHECK(buf[0] == pat(4998, 4) && buf[1] == pat(4999, 4) && buf[2] == 'X' && buf[3] == 'X',
		      "content around the change");
		IDOS->ChangeFilePosition(fh, 5100, OFFSET_BEGINNING);
		IDOS->Read(fh, buf, 1);
		CHECK(buf[0] == pat(5100, 4), "content after the change");
		IDOS->Close(fh);
	}

	/* MODE_READWRITE on a new name creates the file */
	fh = IDOS->Open(P("rw_new"), MODE_READWRITE);
	CHECK(fh != ZERO, "rw creates");
	if (fh != ZERO) {
		IDOS->Write(fh, "abc", 3);
		IDOS->Close(fh);
	}
	CHECK(file_size(P("rw_new")) == 3, "rw_new size");

	/* MODE_OLDFILE may be written to */
	fh = IDOS->Open(P("rw_new"), MODE_OLDFILE);
	if (fh != ZERO) {
		CHECK(IDOS->Write(fh, "Z", 1) == 1, "write to oldfile");
		IDOS->Close(fh);
	}
	fh = IDOS->Open(P("rw_new"), MODE_OLDFILE);
	if (fh != ZERO) {
		IDOS->Read(fh, buf, 3);
		CHECK(memcmp(buf, "Zbc", 3) == 0, "oldfile write landed");
		IDOS->Close(fh);
	}

	/* MODE_NEWFILE truncates */
	CHECK(write_pattern(P("trunc"), 5000, 5, 4096), "write");
	fh = IDOS->Open(P("trunc"), MODE_NEWFILE);
	if (fh != ZERO) {
		IDOS->Write(fh, "0123456789", 10);
		IDOS->Close(fh);
	}
	CHECK(file_size(P("trunc")) == 10, "newfile truncates: %lld", (long long)file_size(P("trunc")));

	/* empty file */
	fh = IDOS->Open(P("empty"), MODE_NEWFILE);
	CHECK(fh != ZERO && IDOS->Close(fh), "create empty");
	CHECK(file_size(P("empty")) == 0, "empty exists with size 0");

	/* ChangeFileSize on an open file */
	CHECK(write_pattern(P("setsize"), 8000, 6, 4096), "write");
	fh = IDOS->Open(P("setsize"), MODE_OLDFILE);
	if (fh != ZERO) {
		/* the result is a boolean: non-zero (DOSTRUE is -1) for success */
		CHECK(IDOS->ChangeFileSize(fh, 1234, OFFSET_BEGINNING) != 0, "shrink");
		IDOS->Close(fh);
	}
	CHECK(file_size(P("setsize")) == 1234, "size after shrink: %lld", (long long)file_size(P("setsize")));
	CHECK(check_pattern(P("setsize"), 1234, 6), "content after shrink");
	fh = IDOS->Open(P("setsize"), MODE_READWRITE);
	if (fh != ZERO) {
		CHECK(IDOS->ChangeFileSize(fh, 5000, OFFSET_BEGINNING) != 0, "grow");
		CHECK(IDOS->GetFilePosition(fh) == 0, "position unchanged by grow");
		IDOS->Close(fh);
	}
	CHECK(file_size(P("setsize")) == 5000, "size after grow: %lld", (long long)file_size(P("setsize")));
	{
		unsigned char b2[1300];
		fh = IDOS->Open(P("setsize"), MODE_OLDFILE);
		CHECK(fh != ZERO && IDOS->Read(fh, b2, 1234) == 1234 && b2[1233] == pat(1233, 6),
		      "content kept by grow");
		if (fh != ZERO)
			IDOS->Close(fh);
	}
}

static void t_inuse(void)
{
	BPTR a, b;

	CHECK(write_pattern(P("inuse"), 100, 7, 100), "write");
	a = IDOS->Open(P("inuse"), MODE_OLDFILE);
	b = IDOS->Open(P("inuse"), MODE_OLDFILE);
	CHECK(a != ZERO && b != ZERO, "two readers");
	if (b != ZERO)
		IDOS->Close(b);
	b = IDOS->Open(P("inuse"), MODE_NEWFILE);
	CHECK(b == ZERO && IDOS->IoErr() == ERROR_OBJECT_IN_USE, "newfile while open fails with in use");
	if (b != ZERO)
		IDOS->Close(b);
	if (a != ZERO)
		IDOS->Close(a);
}

static void t_locks(void)
{
	BPTR l, d, p;
	char buf[512];

	md(P("lockdir"));
	l = IDOS->Lock(P("lockdir"), SHARED_LOCK);
	CHECK(l != ZERO, "lock dir");
	if (l == ZERO)
		return;
	d = IDOS->DupLock(l);
	CHECK(d != ZERO && IDOS->SameLock(l, d) == LOCK_SAME, "duplock same");
	p = IDOS->ParentDir(l);
	CHECK(p != ZERO, "parentdir");
	if (p != ZERO) {
		CHECK(IDOS->NameFromLock(p, buf, sizeof(buf)) && strstr(buf, "nchtest") != NULL,
		      "parent name %s", buf);
		IDOS->UnLock(p);
	}
	CHECK(IDOS->NameFromLock(l, buf, sizeof(buf)), "namefromlock");
	if (d != ZERO)
		IDOS->UnLock(d);
	IDOS->UnLock(l);
	l = IDOS->Lock(P("does_not_exist"), SHARED_LOCK);
	CHECK(l == ZERO && IDOS->IoErr() == ERROR_OBJECT_NOT_FOUND, "lock missing gives not found");
}

static void t_examinedir(void)
{
	APTR ctx;
	struct ExamineData *ed;
	char name[64];
	int i, files = 0, dirs = 0;

	md(P("many"));
	for (i = 0; i < 60; i++) {
		snprintf(name, sizeof(name), "many/f%02d", i);
		write_pattern(P(name), i * 10, i, 1000);
	}
	for (i = 0; i < 3; i++) {
		snprintf(name, sizeof(name), "many/d%d", i);
		md(P(name));
	}
	ctx = IDOS->ObtainDirContextTags(EX_StringNameInput, P("many"),
	                                 EX_DataFields, EXF_NAME | EXF_TYPE | EXF_SIZE, TAG_END);
	CHECK(ctx != NULL, "dir context");
	if (ctx == NULL)
		return;
	while ((ed = IDOS->ExamineDir(ctx)) != NULL) {
		if (EXD_IS_FILE(ed)) {
			int n = atoi(ed->Name + 1);
			files++;
			CHECK(ed->FileSize == n * 10, "size of %s", ed->Name);
		} else if (EXD_IS_DIRECTORY(ed)) {
			dirs++;
		}
	}
	CHECK(IDOS->IoErr() == ERROR_NO_MORE_ENTRIES, "end of dir");
	IDOS->ReleaseDirContext(ctx);
	CHECK(files == 60 && dirs == 3, "found %d files and %d dirs", files, dirs);
}

static void t_rename(void)
{
	md(P("ren"));
	md(P("ren/sub"));
	write_pattern(P("ren/a"), 500, 8, 500);
	write_pattern(P("ren/b"), 600, 9, 600);
	CHECK(IDOS->Rename(P("ren/a"), P("ren/a2")), "rename file");
	CHECK(!exists(P("ren/a")) && check_pattern(P("ren/a2"), 500, 8), "renamed content");
	CHECK(IDOS->Rename(P("ren/a2"), P("ren/sub/a3")), "move into drawer");
	CHECK(check_pattern(P("ren/sub/a3"), 500, 8), "moved content");
	CHECK(!IDOS->Rename(P("ren/b"), P("ren/sub/a3")) && IDOS->IoErr() == ERROR_OBJECT_EXISTS,
	      "rename onto existing fails");
	CHECK(check_pattern(P("ren/sub/a3"), 500, 8), "existing target untouched");
	CHECK(IDOS->Rename(P("ren/sub"), P("ren/sub2")), "rename drawer");
	CHECK(check_pattern(P("ren/sub2/a3"), 500, 8), "file in renamed drawer");
	/* filesysbox before 2023 (OS4 54.9) refuses case-only renames with
	 * ERROR_OBJECT_EXISTS before the handler sees them */
	if (IDOS->Rename(P("ren/b"), P("ren/B"))) {
		CHECK(dir_has(P("ren"), "B") && !dir_has(P("ren"), "b"), "new case shown");
	} else {
		CHECK(IDOS->IoErr() == ERROR_OBJECT_EXISTS, "case-only rename");
		printf("KNOWN case-only rename refused by filesysbox (object exists)\n");
		CHECK(dir_has(P("ren"), "b"), "old name kept");
	}
}

static void t_delete(void)
{
	md(P("del"));
	md(P("del/empty"));
	md(P("del/full"));
	write_pattern(P("del/full/x"), 10, 1, 10);
	write_pattern(P("del/f"), 10, 1, 10);
	CHECK(IDOS->Delete(P("del/f")) && !exists(P("del/f")), "delete file");
	CHECK(IDOS->Delete(P("del/empty")) && !exists(P("del/empty")), "delete empty drawer");
	CHECK(!IDOS->Delete(P("del/full")) && IDOS->IoErr() == ERROR_DIRECTORY_NOT_EMPTY,
	      "full drawer not deleted");
	CHECK(exists(P("del/full/x")), "content of full drawer still there");
	CHECK(!IDOS->Delete(P("del/missing")) && IDOS->IoErr() == ERROR_OBJECT_NOT_FOUND,
	      "delete missing");
	CHECK(md(P("del/new")), "createdir");
	{
		BPTR l = IDOS->CreateDir(P("del/new"));
		CHECK(l == ZERO && IDOS->IoErr() == ERROR_OBJECT_EXISTS, "createdir existing fails");
		if (l != ZERO)
			IDOS->UnLock(l);
	}
}

static void t_attributes(void)
{
	struct ExamineData *ed;
	struct DateStamp ds;
	const char *comment = "Kommentar med \xe5\xe4\xf6 \xc5\xc4\xd6 - 1234567890 1234567890 1234567890 12345";

	write_pattern(P("attr"), 100, 2, 100);
	CHECK(IDOS->SetComment(P("attr"), comment), "setcomment");
	ds.ds_Days = 8434;        /* 2001-02-03 */
	ds.ds_Minute = 4 * 60 + 5;
	ds.ds_Tick = 6 * 50;
	CHECK(IDOS->SetDate(P("attr"), &ds), "setdate");
	CHECK(IDOS->SetProtection(P("attr"), FIBF_SCRIPT), "setprotection accepted");
	ed = IDOS->ExamineObjectTags(EX_StringNameInput, P("attr"), TAG_END);
	CHECK(ed != NULL, "examine");
	if (ed != NULL) {
		CHECK(strcmp(ed->Comment, comment) == 0, "comment back: '%s'", ed->Comment);
		CHECK(ed->Date.ds_Days == ds.ds_Days && ed->Date.ds_Minute == ds.ds_Minute &&
		      ed->Date.ds_Tick / 50 == ds.ds_Tick / 50,
		      "date back: %ld %ld %ld", (long)ed->Date.ds_Days, (long)ed->Date.ds_Minute,
		      (long)ed->Date.ds_Tick);
		IDOS->FreeDosObject(DOS_EXAMINEDATA, ed);
	}
}

static void t_names(void)
{
	static const char *names[] = {
		"R\xe4ksm\xf6rg\xe5s \xc5\xc4\xd6.txt",
		"with space.txt",
		"hash#percent%plus+amp&semi;q?eq=.txt",
		".hidden",
		"caf\xe9 na\xefve \xdf.txt",
		"x123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456.txt",
		NULL
	};
	int i;

	md(P("names"));
	for (i = 0; names[i] != NULL; i++) {
		char n[300];
		snprintf(n, sizeof(n), "names/%s", names[i]);
		CHECK(write_pattern(P(n), 50 + i, i, 100), "create %s", names[i]);
		CHECK(check_pattern(P(n), 50 + i, i), "read %s", names[i]);
	}
	/* AmigaDOS ignores case: Latin-1 letters too */
	CHECK(exists(P("names/R\xc4KSM\xd6RG\xc5S \xe5\xe4\xf6.TXT")), "case-insensitive with Latin-1");
}

static void t_info(void)
{
	struct InfoData *id = IDOS->AllocDosObject(DOS_INFODATA, NULL);
	BPTR l = IDOS->Lock(base, SHARED_LOCK);

	CHECK(l != ZERO && IDOS->Info(l, id), "info");
	if (l != ZERO && id != NULL)
		CHECK(id->id_NumBlocks > 0 && id->id_BytesPerBlock > 0, "blocks %ld x %ld",
		      (long)id->id_NumBlocks, (long)id->id_BytesPerBlock);
	if (l != ZERO)
		IDOS->UnLock(l);
	IDOS->FreeDosObject(DOS_INFODATA, id);
}

static void t_notify(void)
{
	int8 sig = IExec->AllocSignal(-1);
	struct NotifyRequest *nr;
	uint32 got;

	if (sig < 0)
		return;
	md(P("notify"));
	nr = IDOS->AllocDosObjectTags(DOS_NOTIFYREQUEST,
	                              ADO_NotifyName, P("notify"),
	                              ADO_NotifyMethod, NRF_SEND_SIGNAL,
	                              ADO_NotifyTask, IExec->FindTask(NULL),
	                              ADO_NotifySignalNumber, sig,
	                              TAG_END);
	CHECK(nr != NULL && IDOS->StartNotify(nr), "startnotify");
	if (nr == NULL)
		return;
	IExec->SetSignal(0, 1UL << sig);
	write_pattern(P("notify/file"), 10, 1, 10);
	IDOS->Delay(150); /* filesysbox reports changes after a short delay */
	got = IExec->SetSignal(0, 1UL << sig);
	CHECK(got & (1UL << sig), "notification after creating a file");
	IDOS->EndNotify(nr);
	IDOS->FreeDosObject(DOS_NOTIFYREQUEST, nr);
	IExec->FreeSignal(sig);
}

static void cleanup(const char *dir)
{
	char cmd[600];

	snprintf(cmd, sizeof(cmd), "Delete \"%s\" ALL QUIET FORCE", dir);
	IDOS->SystemTags(cmd, TAG_END);
}

int main(int argc, char **argv)
{
	BPTR l;

	if (argc < 2) {
		printf("usage: nchtest DIR (DIR is created and removed again)\n");
		return 5;
	}
	snprintf(base, sizeof(base), "%s", argv[1]);
	cleanup(base);
	l = IDOS->CreateDir(base);
	if (l == ZERO) {
		printf("cannot create %s\n", base);
		return 10;
	}
	IDOS->UnLock(l);

	t_sizes();
	t_seek();
	t_modes();
	t_inuse();
	t_locks();
	t_examinedir();
	t_rename();
	t_delete();
	t_attributes();
	t_names();
	t_info();
	t_notify();

	if (argc < 3 || strcmp(argv[2], "KEEP") != 0)
		cleanup(base);
	printf("\n%d passed, %d failed\n", pass, fail);
	return fail ? 10 : 0;
}
