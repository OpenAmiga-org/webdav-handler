/* patfile - writes or checks a big file with a known pattern, in order.
 * Test tool for the deep tests (files over 2 GiB), not part of the release.
 * Public domain.
 *
 *   patfile make FILE SIZE     ("-" writes to stdout)
 *   patfile check FILE SIZE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill(unsigned char *buf, long n, long long off)
{
	long i;

	for (i = 0; i < n; i++) {
		unsigned long long x = (unsigned long long)(off + i);
		buf[i] = (unsigned char)((x * 2654435761u + (x >> 20) * 97) >> 7);
	}
}

int main(int argc, char **argv)
{
	static unsigned char buf[65536], want[65536];
	long long size, off = 0;
	int make;
	FILE *f;

	if (argc < 4)
		return 5;
	make = strcmp(argv[1], "make") == 0;
	size = strtoll(argv[3], NULL, 10);
	f = strcmp(argv[2], "-") == 0 ? stdout : fopen(argv[2], make ? "wb" : "rb");
	if (f == NULL) {
		printf("cannot open %s\n", argv[2]);
		return 10;
	}
	while (off < size) {
		long n = size - off < (long long)sizeof(buf) ? (long)(size - off) : (long)sizeof(buf);
		if (make) {
			fill(buf, n, off);
			if (fwrite(buf, 1, n, f) != (size_t)n) {
				fprintf(stderr, "write failed at %lld\n", off);
				return 10;
			}
		} else {
			fill(want, n, off);
			if (fread(buf, 1, n, f) != (size_t)n || memcmp(buf, want, n) != 0) {
				printf("differs or ends near %lld\n", off);
				return 10;
			}
		}
		off += n;
	}
	if (!make && fread(buf, 1, 1, f) != 0) {
		printf("longer than %lld\n", size);
		return 10;
	}
	if (fclose(f) != 0) {
		fprintf(stderr, "close failed\n");
		return 10;
	}
	if (f != stdout)
		printf("%s: %lld bytes ok\n", argv[2], size);
	return 0;
}
