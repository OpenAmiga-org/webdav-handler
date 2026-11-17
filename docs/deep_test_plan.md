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

## Speed test on an emulated Intel 82574L (2026-11-15, handler 0.36)

The guest now has `-device e1000e` (e1000e.device 0.2 from
github.com/derfsss/e1000e) instead of the rtl8139, still on slirp;
`NIC=rtl8139 ./run.sh` brings the old card back. Same 20 MB file, Roadshow
tcp.sendspace at its default 32768 unless noted.

| Transfer | rtl8139 (above) | e1000e |
|---|---|---|
| Bare recv(), HTTP (3 runs) | 1.5-11.7 s, 1.7-13.8 MB/s | 0.77-0.93 s, 21.5-25.9 MB/s |
| Plain curl GET whole file, HTTP (3 runs) | 1.5-45 s | 1.14-1.17 s, 17.1-17.6 MB/s |
| Plain curl GET whole file, HTTPS | 45.1 s, 0.44 MB/s | 2.86 s, 7.0 MB/s |
| Plain curl range GETs as the handler, HTTP / HTTPS | 41.6 / 13.7-17.8 s | 1.97 / 3.69 s, 10.2 / 5.4 MB/s |
| **Handler download, HTTPS (3 runs)** | 6.7-30.7 s, 0.65-3.0 MB/s | 3.27-3.57 s, 5.6-6.1 MB/s |
| **Handler download, HTTP (2 runs)** | 26.0-32.3 s, 0.6-0.8 MB/s | 1.53-1.66 s, 12.1-13.1 MB/s |
| Handler upload, HTTPS (3 runs) | 3.0-3.15 MB/s | 2.85-3.15 MB/s |
| Handler upload, HTTP (2 runs) | 3.1-3.2 MB/s | 3.3-3.5 MB/s |
| Plain curl PUT, HTTP / HTTPS | 3.7 / 3.3 MB/s | 3.9 / 3.3 MB/s |
| Handler upload, tcp.sendspace 65536, HTTP / HTTPS | - | 4.1 / 2.9 MB/s |

- No gaps of 0.5 s or more in any download: the stalls were the rtl8139
  path. All copies read back byte-identical.
- Range downloads lose some speed to the server's ~0.1 s until the first
  byte of each request (it grows to 4 MB requests, so this stays small).
- Uploads: the handler matches a plain curl PUT. Over HTTPS the emulated
  CPU (TLS) is the limit; over HTTP a bigger Roadshow send buffer helps a
  little (bare TCP upload from the guest: 7.3 MB/s, sftp-handler session).

Open: speed on real hardware; tests against the user's own Nextcloud and
with public certificates (AmiSSL:Certs).

## One device for all servers (2026-11-17, handler 0.38 and 0.39)

0.38 replaced the one-server-per-DOSDriver setup with the device NCH:
and a server list (nchprefs, `ENV:NextcloudHandler/servers.xml`). The guest
scripts above still use NC1:; with a test list in `RAM:nch/config`
(`Startup = "CONFIGDIR=RAM:nch/config DEBUG"`) they run unchanged after
`Assign NC1: NCH:Test`.

Linux (`tests/run_tests.py`, 26 tests): the 20 file tests now run inside a
server's folder; new tests cover the root listing without connecting, the
read-only root, a rename between servers (EXDEV), changes to the server
list (and a broken list), the password question (asked, remembered in the
list, asked again after a wrong password) and a cancelled question (not
asked again).

AmigaOS 4 guest (QEMU AmigaOne, e1000e):

| Check | Result |
|---|---|
| Root | `Dir NCH:` lists the servers and a drawer icon each (from ENV:Sys/def_drawer.info), no connection; root entries show `----r-e-` |
| Servers | HTTPS and HTTP server folders listed; functional.script and nchtest (240/240) pass through `NC1: = NCH:Test` |
| Root is read-only | MakeDir, Rename, Echo > at the root: "write protected"; Delete of a server folder refused |
| Between servers | Copy works; Rename gives "packet request type unknown" (EXDEV, documented) |
| Unreachable server | one requester with the reason, then "no disk in drive" without new tries for 10 s |
| Password | requester "nextcloud-handler: Ask"; Cancel gives "no disk" and no new requester; DiskChange NCH: asks again |
| List changes | servers added to `servers.xml` appear at once; a name with umlauts and the euro sign works |
| Old DOSDriver | Startup with URL=: requester explaining the change, the device does not start |
| nchprefs | takes over two old DOSDrivers (password from the Startup line and from ENV:NextcloudHandler/NC1.auth, CAFILE, INSECURE, an escaped-quote VOLUME), Test connection (works / fails with the server's reason), Use writes ENV:NextcloudHandler/servers.xml and deletes the old DOSDrivers and .auth files on request |
| Memory | 2 x 5 mount cycles with two servers connected each: free memory back to the same value |

**Found and fixed in 0.39:** the handler read the character set with
clib4's getenv(), which finds nothing in a handler process, so names were
always converted as ISO-8859-1 (the euro sign showed as %u20AC on an
ISO-8859-15 system; the earlier tests set CHARSET= in the DOSDriver).
ENV:Charset is now read with dos.library's GetVar(). Long server messages
(a Nextcloud 401 text is over 300 characters) made requesters wider than
the screen; requester texts are now broken at 76 characters.

## Install script, upgrade from 0.37 (2026-11-17, release archive 0.39)

Guest set up as a 0.37 user: L:nextcloud-handler 0.37 (built from
c575a14), DEVS:DOSDrivers/NC0 (+ icon, Activate=1, Startup with URL=,
USER=, VOLUME=Nextcloud, CAFILE=), the password in
ENV(ARC):NextcloudHandler/NC0.auth, NC0: mounted and in use; clib4 2.5
first in LIBS:. The 0.39 archive was unpacked in RAM: and Install started
from its icon (WBRun), Intermediate user.

| Run | Result |
|---|---|
| 1: upgrade | L:nextcloud-handler 0.39 (the old one, in use by NC0:, moved to L:nextcloud-handler.old as planned), SYS:Prefs/nchprefs + icon, DEVS:DOSDrivers/NCH + icon, docs in SYS:Documentation/NextcloudHandler, NCH: mounted, nchprefs started: took over DEVS:DOSDrivers/NC0 as server "Nextcloud", Save wrote ENV: and ENVARC:servers.xml, Delete removed NC0, NC0.info and both .auth files; `Dir NCH:Nextcloud` lists the files with the taken-over password; no install log file left |
| 2: again, NCH: mounted and in use | "NCH: is mounted ... stops it" message, NCH: dismounted, files replaced without .old, "DEVS:DOSDrivers/NCH exists already. Replace it?", NCH: mounted again and working |

Not tested: mounting NCH: at boot (the guest's own clib4.library is 2.0).
Everything was removed from the guest afterwards.
