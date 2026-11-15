/* bigwrite - checks writing past 2 GiB on a local volume, with dos.library
 * and with the clib4 calls the handler uses for its temp files.
 * Test tool, not part of the release. Public domain.
 *
 *   bigwrite FILE
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
	const int64 off = ((int64)1 << 31) - 8;
	char path[300];
	BPTR fh;
	int fd;
	ssize_t n;

	if (argc < 2)
		return 5;

	/* dos.library, 64-bit position */
	snprintf(path, sizeof(path), "%s.dos", argv[1]);
	fh = IDOS->Open(path, MODE_NEWFILE);
	if (fh == ZERO) {
		printf("dos: cannot open %s (%ld)\n", path, (long)IDOS->IoErr());
	} else {
		int64 r1 = IDOS->ChangeFilePosition(fh, off, OFFSET_BEGINNING);
		int32 w = IDOS->Write(fh, "0123456789abcdef", 16);
		printf("dos: seek %s, write %ld, IoErr %ld, position %lld\n", r1 ? "ok" : "failed",
		       (long)w, (long)IDOS->IoErr(), (long long)IDOS->GetFilePosition(fh));
		IDOS->Close(fh);
		IDOS->Delete(path);
	}

	/* clib4, as in platform_os4.c */
	snprintf(path, sizeof(path), "%s.c", argv[1]);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		printf("clib4: cannot open %s (%s)\n", path, strerror(errno));
		return 10;
	}
	printf("clib4: sizeof(off_t) = %d\n", (int)sizeof(off_t));
	n = pwrite(fd, "0123456789abcdef", 16, (off_t)off);
	printf("clib4: pwrite at %lld -> %ld (%s)\n", (long long)off, (long)n,
	       n < 0 ? strerror(errno) : "ok");
	errno = 0;
	{
		off_t e = lseek(fd, 0, SEEK_END);
		printf("clib4: size %lld (%s)\n", (long long)e, errno ? strerror(errno) : "ok");
		/* the handler writes 64 KB pieces in order: the next one starts at
		 * the end, which is now past 2 GiB */
		n = pwrite(fd, "0123456789abcdef", 16, e);
		printf("clib4: pwrite at end %lld -> %ld (%s)\n", (long long)e, (long)n,
		       n < 0 ? strerror(errno) : "ok");
		n = pwrite(fd, "0123456789abcdef", 16, ((off_t)1 << 31));
		printf("clib4: pwrite at 2^31 -> %ld (%s)\n", (long)n, n < 0 ? strerror(errno) : "ok");
	}
	close(fd);
	/* dos.library on the same (big) file */
	fh = IDOS->Open(path, MODE_OLDFILE);
	if (fh != ZERO) {
		int64 r1 = IDOS->ChangeFilePosition(fh, 0, OFFSET_END);
		int64 pos = IDOS->GetFilePosition(fh);
		int32 w = IDOS->Write(fh, "0123456789abcdef", 16);
		printf("dos: seek to end %s, position %lld, write %ld, IoErr %ld\n", r1 ? "ok" : "failed",
		       (long long)pos, (long)w, (long)IDOS->IoErr());
		r1 = IDOS->ChangeFilePosition(fh, ((int64)1 << 31) + 4, OFFSET_BEGINNING);
		printf("dos: seek to 2^31+4 %s, IoErr %ld\n", r1 ? "ok" : "failed", (long)IDOS->IoErr());
		IDOS->Close(fh);
	}
	unlink(path);
	return 0;
}
