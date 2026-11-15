# nextcloud-handler

An AmigaOS 4 filesystem handler that mounts a Nextcloud account (or another
WebDAV server) as a volume, for example `Nextcloud:`. Built on
filesysbox.library, clib4, libcurl and AmiSSL.

End-user documentation: `package/NextcloudHandler/NextcloudHandler.guide`.
Release archive: `tools/make_lha.sh` -> `dist/NextcloudHandler.lha`.

## Layout

- `src/core/`   WebDAV client, cache, file logic, charset conversion (portable C)
- `src/os4/`    AmigaOS 4 handler: startup, worker process, filesysbox glue,
                AmiSSL, requesters
- `src/linux/`  Linux FUSE build of the same core, for fast tests
- `tests/`      `run_tests.py` (Linux build against the Docker server),
                `unit_charset.c`, `os4/` (guest scripts and test tools)
- `testserver/` throwaway Nextcloud in Docker (`setup.sh`, `reset.sh`)
- `package/`    files of the release archive (guide, Install script, DOSDriver,
                readme, icons)
- `tools/`      build scripts (see below), icon tools
- `docs/`       notes, including the review of openamiga project 63

## Building

    make linux                     # Linux FUSE test build
    make test                      # ... and the tests against testserver/
    tools/build_curl_amissl.sh     # once: libcurl for clib4 with AmiSSL
    tools/build_os4.sh             # AmigaOS 4 handler (walkero/amigagccondocker)
    tools/make_lha.sh              # release archive

The OS4 build needs the cross-compiler image
`walkero/amigagccondocker:os4-gcc15-3.6.1` (GCC 15, SDK 54.25, clib4 2.5).

## Design notes

- filesysbox.library calls the filesystem callbacks in its own process and,
  on AmigaOS 4, in the tasks of the programs using the volume. clib4, curl
  and the sockets only work in the handler process, so the callbacks
  (`src/os4/fbxglue.c`) only pass each call to the handler process
  (`src/os4/main.c`) as an Exec message.
- clib4's startup code would take the DOS startup packet for a Workbench
  message; `nch_entry` runs first (renamed to `_start` after linking).
- curl's threaded resolver does not work with clib4; the handler resolves
  names itself and passes them with CURLOPT_RESOLVE.
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
- Deep tests in the AmigaOS 4 guest: `docs/deep_test_plan.md`,
  `tests/os4/` (nchtest, nametest, gentree, scripts in `deep/`) and
  `tests/hostmount.sh` to check the server side from Linux.

Public domain, see `UNLICENSE`.
