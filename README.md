# nextcloud-handler

An AmigaOS 4 filesystem handler that shows Nextcloud accounts (and other
WebDAV servers) on one volume, `Nextcloud:` (device `NCH:`): every server
in the server list is a drawer, connected when it is first used. The
server list (`ENV:NextcloudHandler/servers.xml`) is kept by the prefs
program `nchprefs`. Built on filesysbox.library, clib4, libcurl and AmiSSL.

End-user documentation: `package/NextcloudHandler/NextcloudHandler.guide`.
Release archive: `tools/make_lha.sh` -> `dist/NextcloudHandler.lha`.

## Layout

- `src/core/`   WebDAV client, cache, file logic, charset conversion, the
                server list (`servers.c`) and the root of servers
                (`ncroot.c`) (portable C)
- `src/os4/`    AmigaOS 4 handler: startup, worker process, filesysbox glue,
                AmiSSL, requesters
- `src/prefs/`  nchprefs, the prefs program (ReAction, clib4)
- `src/linux/`  Linux FUSE build of the same core, for fast tests
- `tests/`      `run_tests.py` (Linux build against the Docker server, with a
                server list of several servers),
                `unit_charset.c`, `os4/` (guest scripts and test tools)
- `testserver/` throwaway Nextcloud in Docker (`setup.sh`, `reset.sh`)
- `package/`    files of the release archive (guide, Install script, DOSDriver,
                readme, icons)
- `tools/`      build scripts (see below), icon tools
- `docs/`       notes, including the review of openamiga project 63

## Building

    make linux                     # Linux FUSE test build
    make unit                      # unit tests (charset)
    make test                      # ... unit tests and the tests against testserver/
    tools/build_curl_amissl.sh     # once: curl headers (its library is not linked)
    tools/build_os4.sh             # AmigaOS 4 handler and nchprefs (walkero/amigagccondocker)
    tools/make_lha.sh              # release archive

The OS4 build needs the cross-compiler image
`walkero/amigagccondocker:os4-gcc15-3.6.1` (GCC 15, SDK 54.25, clib4 2.5).

## Design notes

- The root lists the servers of `servers.xml` without connecting
  (`src/core/ncroot.c`, the same design as sftp-handler's sites). Each
  server gets its own WebDAV session and cache (`ncfs`) when its folder is
  first used; the list is read again when the file changes (checked at
  most once a second), and a server whose settings changed is kept for its
  open files until they are closed. The root and the server folders are
  read-only; a rename from one server to another gives EXDEV.
- filesysbox.library calls the filesystem callbacks in its own process and,
  on AmigaOS 4, in the tasks of the programs using the volume. clib4, curl
  and the sockets only work in the handler process, so the callbacks
  (`src/os4/fbxglue.c`) only pass each call to the handler process
  (`src/os4/main.c`) as an Exec message.
- clib4's startup code would take the DOS startup packet for a Workbench
  message; `nch_entry` runs first (renamed to `_start` after linking).
- curl's threaded resolver does not work with clib4; the handler resolves
  names itself and passes them with CURLOPT_RESOLVE.
- The handler links the cross-compiler image's libcurl.a (clib4 sockets,
  TLS through the AmiSSL stubs): `-mcrt=clib4` searches the SDK's clib4
  lib directory before any `-L` directory. A curl of our own that calls
  bsdsocket.library directly needs fixes first (tools/build_curl_amissl.sh).
- filesysbox 54.9 does not convert file names to the local character set,
  so the handler converts Latin-1/ISO-8859-15 <-> UTF-8 itself.
- Writes go to a temp file and are uploaded on Close (Nextcloud chunked
  upload above 16 MB). filesysbox ignores errors from Close, so a failed
  upload opens a requester and the data is kept.
- Reads use HTTP ranges. The ETag of the first block goes with every later
  block as If-Match, so a file changed on the server during a read gives
  an error (ETXTBSY, "object is in use") instead of mixed data.
- DOS keeps a handler's code in `dn_SegList` and never unloads it when the
  device is removed. The handler takes the seglist out of the device node
  at startup and sets PRF_FREESEGLIST, so its code goes when it ends; the
  filesysbox process is a child (`NP_Child`) that the handler waits for.
- FFS lets files grow past 2 GiB but cannot seek there, and clib4's
  pread/pwrite seek on every call; the temp files keep their own position
  and only seek when an access does not follow the previous one.
- Deep tests in the AmigaOS 4 guest: `docs/deep_test_plan.md`,
  `tests/os4/` (nchtest, nametest, gentree, scripts in `deep/`) and
  `tests/hostmount.sh` to check the server side from Linux.

Public domain, see `UNLICENSE`.
