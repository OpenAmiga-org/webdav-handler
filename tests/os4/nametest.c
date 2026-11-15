/* nametest - reads, renames and renames back every file in a drawer, to
 * check names that came from the server (escapes, ':' and so on).
 * Test tool, not part of the release. Public domain.
 *
 *   nametest DIR
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	APTR ctx;
	struct ExamineData *ed;
	char names[64][300];
	int64 sizes[64];
	int n = 0, i, fail = 0;

	if (argc < 2)
		return 5;
	ctx = IDOS->ObtainDirContextTags(EX_StringNameInput, argv[1], EX_DataFields,
	                                 EXF_NAME | EXF_SIZE | EXF_TYPE, TAG_END);
	if (ctx == NULL) {
		printf("cannot read %s\n", argv[1]);
		return 10;
	}
	while (n < 64 && (ed = IDOS->ExamineDir(ctx)) != NULL) {
		if (!EXD_IS_FILE(ed))
			continue;
		snprintf(names[n], sizeof(names[n]), "%s", ed->Name);
		sizes[n] = ed->FileSize;
		n++;
	}
	IDOS->ReleaseDirContext(ctx);

	for (i = 0; i < n; i++) {
		char path[700], path2[700], buf[400];
		BPTR fh;
		int32 got = -1;

		snprintf(path, sizeof(path), "%s/%s", argv[1], names[i]);
		snprintf(path2, sizeof(path2), "%s/r_%s", argv[1], names[i]);
		fh = IDOS->Open(path, MODE_OLDFILE);
		if (fh != ZERO) {
			got = IDOS->Read(fh, buf, sizeof(buf) - 1);
			IDOS->Close(fh);
		}
		printf("%-40.40s %3lld bytes: ", names[i], (long long)sizes[i]);
		if (got != sizes[i]) {
			printf("FAIL read %ld (IoErr %ld)\n", (long)got, (long)IDOS->IoErr());
			fail++;
			continue;
		}
		if (!IDOS->Rename(path, path2)) {
			printf("FAIL rename (IoErr %ld)\n", (long)IDOS->IoErr());
			fail++;
			continue;
		}
		if (!IDOS->Rename(path2, path)) {
			printf("FAIL rename back (IoErr %ld)\n", (long)IDOS->IoErr());
			fail++;
			continue;
		}
		printf("ok\n");
	}
	printf("%d files, %d failed\n", n, fail);
	return fail ? 10 : 0;
}
