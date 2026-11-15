# Deep test plan (AmigaOS 4 guest)

Goal: find what a user would hit beyond the basic functional script.
Server: the Docker test Nextcloud (testserver/), mounted as NC1: over
HTTPS. Every result is also checked on the server from the host where it
matters.

| Area | How | Pass when |
|---|---|---|
| A. DOS API | `tests/os4/nchtest.c` (PASS/FAIL per check) | all checks pass, or the difference is understood and documented |
| B. Data at scale | Copy ALL CLONE of a generated tree (500 small files, nested drawers, a few large files) to the volume and back, compare checksums | identical; time noted |
| C. Server view | host lists the server after A/B: UTF-8 names, sizes, dates, comments | matches what the Amiga wrote |
| D. Changes elsewhere | host changes, adds and deletes files while the volume is mounted | seen after CACHETIME, no errors on the Amiga |
| E. Failures | server stopped during upload and during download, quota full, wrong CA certificate | no crash, requester or AmigaDOS error, kept data intact, volume recovers |
| F. Workbench | drawer on the volume with icons, snapshot, delete via Workbench | icons show, .info files written and read |
| G. Programs | LhA extracting onto and archiving from the volume, MultiView, NotePad save | works like on a local disk |
| H. Big files | file > 2 GiB with TEMPDIR on Work: (if space) | size and checksum right |
| I. Names | ISO-8859-15 Euro sign, CJK/emoji names from the server (%u escapes), ':' in server names, long names | readable, renameable, no stuck entries |
| J. Concurrency | two Shells copying at the same time; reading a file while another is written | correct data, no deadlock |
| K. Stability | 20 mount/dismount cycles, 1000 file operations | no leaked tasks, free memory back to the start level, no crash in the debug log |
