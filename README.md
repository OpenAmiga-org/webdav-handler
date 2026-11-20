# webdav-handler

An AmigaOS 4 filesystem handler that shows WebDAV servers on one volume,
`WebDAV:` (device `WDV:`): every server in the server list is a drawer,
connected when it is first used. On Nextcloud servers it also keeps file
dates and uploads big files in parts. webdav-handler is an
[OpenAmiga.org project](http://openamiga.org/?function=gitproject&name=webdav-handler).

The server list (`ENV:WebDAVHandler/servers.xml`) is kept by the prefs
program `wdvprefs`. Up to 0.39 it was called Nextcloud Handler
(`nextcloud-handler`, device `NCH:`, `nchprefs`); wdvprefs and the Install
script take an installation of it over.

Built on filesysbox.library, clib4, libcurl and AmiSSL.

Download: not on OS4Depot yet (`tools/make_lha.sh` makes
`dist/WebDAVHandler.lha`: handler, wdvprefs, DOSDriver, Install script and
guide).

## Layout

- `src/core/`   WebDAV client, cache, file logic, charset conversion, the
                server list (`servers.c`) and the root of servers
                (`ncroot.c`) (portable C)
- `src/os4/`    AmigaOS 4 handler: startup, worker process, filesysbox glue,
                AmiSSL, requesters
- `src/prefs/`  wdvprefs, the prefs program (ReAction, clib4)
- `src/linux/`  Linux FUSE build of the same core, for fast tests
- `tests/`      `run_tests.py` (Linux build against the Docker server, with a
                server list of several servers),
                `unit_charset.c`, `os4/` (guest scripts and test tools)
- `testserver/` throwaway Nextcloud in Docker (`setup.sh`, `reset.sh`) and a
                plain WebDAV server, Apache mod_dav (`webdav.sh`)
- `package/`    files of the release archive (guide, Install script, DOSDriver,
                readme, icons)
- `tools/`      build scripts (see below), icon tools, `update_readme.py`
                (the changelog below, from the readme)
- `docs/`       notes, including the review of openamiga project 63

## Using it

The end-user documentation is `package/WebDAVHandler/WebDAVHandler.guide`;
the OS4Depot readme is `package/WebDAVHandler.readme`.

## Building

    make linux                     # Linux FUSE test build
    make unit                      # unit tests (charset)
    make test                      # ... unit tests and the tests against testserver/
    tools/build_curl_amissl.sh     # once: curl headers (its library is not linked)
    tools/build_os4.sh             # AmigaOS 4 handler and wdvprefs (walkero/amigagccondocker)
    tools/make_lha.sh              # release archive

The OS4 build needs the cross-compiler image
`walkero/amigagccondocker:os4-gcc15-3.6.1` (GCC 15, SDK 54.25, clib4 2.5).

## Design notes

- Server types (`dav_config.kind`, the Type in wdvprefs): Automatic asks
  `<address>/remote.php/dav/` for the current user principal; a Nextcloud
  answer gives the user's files folder and Nextcloud mode (X-OC-Mtime and
  `lastmodified` PROPPATCH for dates, chunked upload v2 above CHUNKSIZE),
  anything else makes the address a plain WebDAV folder. An address with
  `/remote.php/` is taken as Nextcloud. Type WebDAV skips all of it, type
  Nextcloud fails when no Nextcloud answers. On plain WebDAV, setting a
  date is accepted without effect. Weak ETags (Apache, the first second
  after a change) are not used for If-Match.
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
  TLS through AmiSSL): `-mcrt=clib4` searches the SDK's clib4 lib
  directory before any `-L` directory. A curl of our own that calls
  bsdsocket.library directly needs fixes first (tools/build_curl_amissl.sh).
- AmiSSL 5 and 6 with one build (as in sftp-handler): no `-lamisslstubs`.
  `tools/gen_amissl_glue.py` makes a stub for each AmiSSL function in
  `tools/amissl_functions.txt` (the 234 that libcurl and the handler call)
  that calls through a table; `src/os4/amissl.c` opens AMISSL_V4xx when
  amisslmaster is 6 or newer, else AMISSL_V3xx (5.1), and fills the table
  with the offsets of that interface. AmiSSL 6 (OpenSSL 4) dropped 10 of
  the functions from its interface (ASN1_STRING_length, X509_get_pubkey,
  MD5_*, RSA_*, SRP); `src/os4/amissl_compat.c` stands in for them, and for
  SSL_get0_group_name on AmiSSL 5 before 5.13. `tools/check_amissl_api.py`
  checks every used function against the 5.1, 5.27 and 6.1 interfaces.
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

## Changelog

<!-- changelog start: made by tools/update_readme.py from package/WebDAVHandler.readme -->
- **0.43** (2026-11-18): Clearer messages for some server answers: "file is write protected" for changes the server does not allow (read-only shares), "object is in use" for locked files, "disk is full" for files too big for the server (they showed "Error -1").
- **0.42** (2026-11-17): Works with AmiSSL 6 alone and with every AmiSSL 5 from 5.1 on (before, it needed AmiSSL 5.27). wdvprefs' Test connection names the AmiSSL used.
- **0.41** (2026-11-17):
  - Nextcloud Handler is now WebDAV Handler (`webdav-handler`, device `WDV:`, volume `WebDAV:`, prefs program wdvprefs); any WebDAV server works and Nextcloud servers keep their extras.
  - A Type for every server (Automatic, Nextcloud, WebDAV).
  - Install removes Nextcloud Handler on request and wdvprefs takes its servers over.
  - AmigaOS 4.1 style icons and a volume icon for `WDV:`.
  - Setting a date on a server that cannot keep it is accepted without effect. Reading files with a weak ETag works.
  - With clib4.library older than 2.5 the handler shows a requester instead of stopping the boot.
- **0.39** (2026-11-17):
  - Nextcloud Handler: one device, `NCH:`, for all servers: every server is a drawer, connected when it is first used. The new prefs program nchprefs keeps the server list (with "Test connection") and takes over the servers of the old DOSDrivers.
  - Remembered passwords go into the server list. The character set of file names comes from `ENV:Charset` when CHARSET is not given. Long server messages are broken into lines in requesters.
- **0.37** (2026-11-15): Nextcloud Handler: converting an empty text between the Amiga character set and UTF-8 could give leftover memory instead of an empty text (no case is known where the handler showed it).
- **0.32** (2026-11-15):
  - Nextcloud Handler: reading stops with an error when the file is changed on the server during the read, instead of giving a mix of both versions.
  - The handler's code is freed when the volume is dismounted (about 3 MB were lost with every mount). Problems that need the user, such as certificate errors, are shown in a requester once.
  - Names with ":" from the server can be used (shown as `%u003A`). Files show the e bit, so programs on the volume can be started. Line breaks in requesters are shown correctly. Files over 2 GiB can be written with TEMPDIR on an FFS partition.
- **0.25** (2026-11-15): Nextcloud Handler: first test version.
<!-- changelog end -->

## History

Started in November 2026 as Nextcloud Handler (`orgin/nextcloud_client`)
by Björn Hagström with Claude, first test version 0.25 on 15.11.2026.
Renamed to webdav-handler with 0.41 (17.11.2026), when it became a
handler for any WebDAV server, and moved to the OpenAmiga-org GitHub
organization in November 2026 (GitHub forwards the old addresses).

Public domain, see `UNLICENSE` (The Unlicense). The handler contains
libcurl, Expat, zlib and parts of clib4 under their own licences
(`Third-party.txt` in the archive).
