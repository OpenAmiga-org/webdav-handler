/* gentree - creates or checks a test tree of files with known contents.
 * Test tool for the deep tests, not part of the release. Public domain.
 *
 *   gentree make DIR [BIG]     create the tree in DIR (BIG adds a 20 MB file)
 *   gentree check DIR [BIG]    check that DIR holds exactly that tree
 *
 * The same program is built for AmigaOS 4 and for Linux. Names use
 * Latin-1 bytes; the Linux build writes them as UTF-8 (-DNAMES_UTF8), so
 * that it can check what the Amiga wrote through the FUSE front end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#define NFILES 500
#define NDIRS  24

static int failures;

static unsigned hash(const char *s)
{
	unsigned h = 2166136261u;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static void name_out(char *out, size_t outlen, const char *latin1)
{
#ifdef NAMES_UTF8
	size_t n = 0;

	for (; *latin1 && n + 3 < outlen; latin1++) {
		unsigned char c = (unsigned char)*latin1;
		if (c < 0x80) {
			out[n++] = (char)c;
		} else {
			out[n++] = (char)(0xC0 | (c >> 6));
			out[n++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[n] = '\0';
#else
	snprintf(out, outlen, "%s", latin1);
#endif
}

/* the drawers: a few levels deep, some with Latin-1 names */
static void dir_name(int d, char *out, size_t outlen)
{
	static const char *part[] = { "Drawer", "M\xe5nad", "Gr\xfcn", "with space", "deep" };
	char tmp[256];

	if (d < 6)
		snprintf(tmp, sizeof(tmp), "%s%d", part[d % 5], d);
	else if (d < 14)
		snprintf(tmp, sizeof(tmp), "%s%d/%s%d", part[(d - 6) % 6 % 5], (d - 6) % 6,
		         part[d % 5], d);
	else if (d < 20)
		snprintf(tmp, sizeof(tmp), "%s%d/%s%d/sub%d", part[(d - 14) % 6 % 5], (d - 14) % 6,
		         part[(d - 8) % 5], d - 8, d);
	else
		snprintf(tmp, sizeof(tmp), "%s%d/%s%d/sub%d/level4_%d", part[(d - 20) % 6 % 5],
		         (d - 20) % 6, part[(d - 14) % 5], d - 14, d - 6, d);
	snprintf(out, outlen, "%s", tmp);
}

static long file_size_for(int i, unsigned h)
{
	if (i % 50 == 0)
		return 0;
	if (i % 37 == 0)
		return 4096L * (1 + i % 5);
	if (i % 97 == 0)
		return 300000 + (long)(h % 700000);
	return (long)(h % 20000);
}

static void file_name(int i, char *out, size_t outlen)
{
	char tmp[256], dir[200];
	const char *fmt;

	switch (i % 7) {
	case 0: fmt = "file%03d.txt"; break;
	case 1: fmt = "R\xe4ksm\xf6rg\xe5s %03d"; break;
	case 2: fmt = "Picture%03d.iff"; break;
	case 3: fmt = "Picture%03d.iff.info"; break;
	case 4: fmt = ".dot%03d"; break;
	case 5: fmt = "na\xefve caf\xe9 #%03d+x"; break;
	default: fmt = "a_rather_long_name_to_see_that_long_names_work_%03d.dat"; break;
	}
	snprintf(tmp, sizeof(tmp), fmt, i);
	if (i % 3 == 0) {
		snprintf(out, outlen, "%s", tmp);
		return;
	}
	dir_name(i % NDIRS, dir, sizeof(dir));
	snprintf(out, outlen, "%s/%s", dir, tmp);
}

static void fill(unsigned char *buf, long n, long off, unsigned seed)
{
	long i;

	for (i = 0; i < n; i++) {
		unsigned x = (unsigned)(off + i) * 2654435761u + seed;
		buf[i] = (unsigned char)(x >> 24);
	}
}

static void mkdirs(const char *base, const char *rel)
{
	char path[1200];
	char *p;

	snprintf(path, sizeof(path), "%s/%s", base, rel);
	for (p = path + strlen(base) + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(path, 0755);
			*p = '/';
		}
	}
	mkdir(path, 0755);
}

static int write_file(const char *path, long size, unsigned seed)
{
	static unsigned char buf[65536];
	FILE *f = fopen(path, "wb");
	long off = 0;

	if (f == NULL) {
		printf("FAIL cannot create %s (%s)\n", path, strerror(errno));
		failures++;
		return -1;
	}
	while (off < size) {
		long n = size - off < (long)sizeof(buf) ? size - off : (long)sizeof(buf);
		fill(buf, n, off, seed);
		if (fwrite(buf, 1, n, f) != (size_t)n) {
			printf("FAIL write %s\n", path);
			failures++;
			break;
		}
		off += n;
	}
	if (fclose(f) != 0) {
		printf("FAIL close %s\n", path);
		failures++;
		return -1;
	}
	return 0;
}

static void check_file(const char *path, long size, unsigned seed)
{
	static unsigned char buf[65536], want[65536];
	FILE *f = fopen(path, "rb");
	long off = 0;
	size_t n;

	if (f == NULL) {
		printf("FAIL missing %s\n", path);
		failures++;
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		fill(want, (long)n, off, seed);
		if (memcmp(buf, want, n) != 0) {
			size_t k = 0;
			while (buf[k] == want[k])
				k++;
			printf("FAIL %s differs at %ld\n", path, off + (long)k);
			failures++;
			fclose(f);
			return;
		}
		off += (long)n;
	}
	fclose(f);
	if (off != size) {
		printf("FAIL %s has %ld bytes, expected %ld\n", path, off, size);
		failures++;
	}
}

/* counts the entries in the tree, so that extra files show up */
static long count_entries(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	long n = 0;

	if (d == NULL)
		return 0;
	while ((e = readdir(d)) != NULL) {
		char sub[700];
		struct stat st;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		n++;
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
		if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
			n += count_entries(sub);
	}
	closedir(d);
	return n;
}

int main(int argc, char **argv)
{
	int make, big, i;
	const char *base;
	char rel[400], conv[800], path[900];
	long total = 0, expected_entries;

	if (argc < 3 || (strcmp(argv[1], "make") != 0 && strcmp(argv[1], "check") != 0)) {
		printf("usage: gentree make|check DIR [BIG]\n");
		return 5;
	}
	make = strcmp(argv[1], "make") == 0;
	base = argv[2];
	big = argc > 3 && strcmp(argv[3], "BIG") == 0;
	if (make)
		mkdir(base, 0755);

	/* drawers: count each one once (the deeper ones include their parents) */
	for (i = 0; i < NDIRS; i++) {
		dir_name(i, rel, sizeof(rel));
		name_out(conv, sizeof(conv), rel);
		if (make)
			mkdirs(base, conv);
	}
	for (i = 0; i < NFILES + big; i++) {
		long size;
		unsigned h;
		if (i == NFILES) {
			snprintf(rel, sizeof(rel), "big_20MB.bin");
		} else {
			file_name(i, rel, sizeof(rel));
		}
		/* the Latin-1 name decides size and content on every system */
		h = hash(rel);
		size = i == NFILES ? 20L * 1024 * 1024 + 12345 : file_size_for(i, h);
		name_out(conv, sizeof(conv), rel);
		snprintf(path, sizeof(path), "%s/%s", base, conv);
		if (make)
			write_file(path, size, h);
		else
			check_file(path, size, h);
		total += size;
	}
	/* 24 drawer paths make 24 + 6 + ... drawers; count them on make and
	 * compare on check */
	expected_entries = count_entries(base);
	printf("%s: %d files, %ld bytes, %ld entries in the tree\n", base, NFILES + big, total,
	       expected_entries);
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures ? 10 : 0;
}
