/* speedtest - times a file copy with dos.library. Open, the reads, the
 * writes and each Close are timed apart, because nextcloud-handler writes
 * to a temp file and uploads on Close.
 * Test tool, not part of the release. Public domain.
 *
 *   speedtest FROM TO [BUFKB]      (TO can be NIL: to time reading only)
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static double now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	long bufkb = argc > 3 ? atol(argv[3]) : 256;
	double t0, t1, t2, t3, t4, tr = 0, tw = 0, a, mb;
	int64 total = 0;
	int32 n, ok;
	BPTR in, out;
	char *buf;

	if (argc < 3 || bufkb <= 0)
		return 5;
	buf = malloc(bufkb * 1024);
	if (buf == NULL)
		return 20;
	t0 = now();
	in = IDOS->Open(argv[1], MODE_OLDFILE);
	if (in == ZERO) {
		printf("cannot open %s (%ld)\n", argv[1], (long)IDOS->IoErr());
		return 10;
	}
	out = IDOS->Open(argv[2], MODE_NEWFILE);
	if (out == ZERO) {
		printf("cannot open %s (%ld)\n", argv[2], (long)IDOS->IoErr());
		return 10;
	}
	t1 = now();
	for (;;) {
		a = now();
		n = IDOS->Read(in, buf, bufkb * 1024);
		tr += now() - a;
		if (n <= 0)
			break;
		a = now();
		if (IDOS->Write(out, buf, n) != n) {
			printf("write failed at %lld (%ld)\n", (long long)total, (long)IDOS->IoErr());
			return 10;
		}
		tw += now() - a;
		total += n;
	}
	if (n < 0) {
		printf("read failed at %lld (%ld)\n", (long long)total, (long)IDOS->IoErr());
		return 10;
	}
	t2 = now();
	IDOS->Close(in);
	t3 = now();
	ok = IDOS->Close(out);
	t4 = now();
	mb = total / 1048576.0;
	printf("%s -> %s: %lld bytes, %ld KB buffer\n", argv[1], argv[2], (long long)total, bufkb);
	printf("  open %.2fs, reads %.2fs, writes %.2fs, close source %.2fs, close dest %.2fs%s\n",
	       t1 - t0, tr, tw, t3 - t2, t4 - t3, ok ? "" : " (failed)");
	printf("  total %.2fs = %.2f MB/s\n", t4 - t0, mb / (t4 - t0));
	return ok ? 0 : 10;
}
