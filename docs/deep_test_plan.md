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

## Results (2026-11-15, handler 0.25 -> 0.31, AmigaOS 4.1 FE Update 3 in QEMU)

| Area | Result |
|---|---|
| A. DOS API | 240 checks pass. Case-only rename is refused by filesysbox 54.9 (fixed in filesysbox in 2023, no OS4 build yet): documented. |
| B. Data at scale | 501 files / 28.6 MB with Latin-1 names: up 3 min 14 s, down 1 min 25 s, byte-identical, dates kept. |
| C. Server view | Same tree checked from Linux through the server: names (UTF-8), sizes, contents, comment, date all right. |
| D. Changes elsewhere | New, changed, deleted, renamed files and drawers seen within CACHETIME. **Found:** a file replaced on the server during a read gave a mix of old and new data without an error. Fixed in 0.27 (If-Match with the ETag of the first block; error "object is in use"). |
| E. Failures | Server paused 15 s during a 100 MB upload: upload continues, checksum right. Server stopped during upload: requester, data kept in T: (checksum right), volume back once the server runs. Server paused 75 s during a download: read ends after 60 s ("no disk"), volume recovers. Wrong CA: **found** silent "no disk"; 0.29 shows one requester for problems that need the user. **Found:** requester texts showed %u000A for line breaks; fixed in 0.29. |
| F. Workbench | Drawer created with NEWDRAWER, icons copied and shown, window and icon snapshot written to the server and used again after reopening. |
| G. Programs | LhA: extract onto the volume, pack an archive on the volume, test it: all OK, dates kept. MultiView shows a PNG from the volume. NotePad saves (new file and overwrite). **Found:** files had no e bit, so programs on the volume could not be started; 0.31 shows rwed. |
| H. Big files | **Found:** writing a file over 2 GiB failed at exactly 2 GiB with TEMPDIR on FFS (FFS cannot seek past 2 GiB, clib4's pwrite seeks on every call). Fixed in 0.32: 2.2 GB written through the volume in 12 min (checksum right on the server) and read back through range requests in 43 min (pattern right). |
| I. Names | Euro, CJK, emoji, ":" (new %u003A escape, 0.26), 200-character names, AmigaDOS wildcard characters, quotes, a literal "%u": read, renamed and renamed back with ISO-8859-1 and ISO-8859-15; server names unchanged. |
| J. Concurrency | Two copies to the volume, a download and an upload at the same time: all byte-identical, no deadlock. |
| K. Stability | **Found:** about 3 MB lost per mount (the handler code was never unloaded). Fixed in 0.30: 20 mount cycles now end within 8 KB of the start. 1000 file operations: no errors, no crash, no leftover tasks. |

## Speed test (2026-11-15, handler 0.33, 20 MB file, RAM: <-> volume)

QEMU AmigaOne (G3, TCG), rtl8139, user-mode network to the Docker server
on the host. `tests/os4/speedtest` times Open, the reads/writes and Close
apart (the upload runs in Close); `tests/os4/curlspeed` does the same
transfers with plain libcurl and the handler's AmiSSL setup.

| Transfer | Time | MB/s |
|---|---|---|
| Handler upload, HTTPS (3 runs) | 6.3-6.6 s | 3.0-3.15 |
| Handler upload, HTTP (2 runs) | 6.2-6.4 s | 3.1-3.2 |
| Plain curl PUT, HTTPS / HTTP | 6.0 s / 5.5 s | 3.3 / 3.7 |
| Handler download, HTTPS (3 runs) | 6.7, 30.7, 24.7 s | 3.0, 0.65, 0.81 |
| Handler download, HTTP (2 runs) | 26.0, 32.3 s | 0.77, 0.62 |
| Plain curl GET whole file, HTTPS / HTTP | 45.1 s / 1.5 s | 0.44 / 13.2 |
| Plain curl range GETs as the handler, HTTPS (2 runs) | 13.7, 17.8 s | 1.5, 1.1 |
| Plain curl range GETs as the handler, HTTP | 41.6 s | 0.48 |
| Host (Linux FUSE build, no emulation) up / down | 0.6-1.2 s / 0.5-1.1 s | 17-34 / 19-40 |

- Uploads: the handler costs about 5-10 % over a bare PUT (chunked upload
  above 16 MB, the server joins the chunks). TLS is not the limit; the
  emulated machine's send path is.
- Downloads: requests that do not stall run at 5-6 MB/s (HTTPS) and
  10-13 MB/s (HTTP). Some requests stall, mostly for about 1.4 s or about
  14.7 s. Plain curl stalls the same way over HTTP and HTTPS, so the
  handler's own code is not the cause.
- A 32 KB socket receive buffer (default 64 KB, `curlspeed ... RCVBUF n`)
  does not help, so it is not the TCP window.
- Cause: the emulated network, not curl or the wait call. `curlspeed ...
  raw` reads with a blocking recv() straight on a bsdsocket.library socket
  (no curl, no clib4, no select/poll). Interleaved with curl in one
  session (HTTP, whole file):

  | Round | bare recv() | curl (clib4 poll) |
  |---|---|---|
  | 1 | 1.45 s, 13.8 MB/s | 1.65 s, 12.1 MB/s |
  | 2 | 1.59 s, 12.5 MB/s | 1.63 s, 12.2 MB/s |
  | 3 | 11.67 s, 1.7 MB/s (7 gaps of 1.47-1.48 s) | 1.79 s, 11.2 MB/s |

  The gaps are always about 1.48 s, with 88-366 KB between them; a long
  "stall" is a run of them (17 in a row = 25 s in one 4 MB range). That
  looks like a TCP retransmit timeout after lost packets (slirp's BSD
  TCP: 500 ms ticks, 1 s minimum). The sftp-handler session saw the same
  with its own select() code (FTP download 0.31 to 11.5 MB/s between
  runs). Good and bad phases come and go.
- Both 20 MB copies read back byte-identical (patfile check).
- Found on the way: the OS4 builds link the cross-compiler image's
  libcurl.a, not the one from tools/build_curl_amissl.sh (`-mcrt=clib4`
  searches the SDK's clib4 lib directory first). Every release was built
  and tested with the image's curl, so that stays; tools/os4.mk and the
  script now say so, and the script lists what its own curl still needs.

Open: download speed on real hardware; tests against the user's own
Nextcloud and with public certificates (AmiSSL:Certs).
