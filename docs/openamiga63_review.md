# openamiga svn project 63: can it access Nextcloud?

Source review only (2026-11-14). Nothing was built or run.
Checkout: `~/openamiga/projects/63` (svn://openamiga.org, r1222).

## What project 63 is

Chris Young's `http-handler` family for AmigaOS 4:

- `source/http/` — http-handler v1.x/2.x: a read-only HTTP: / HTTPS: handler.
  The 2022 "AmiSSL v5 update" (r1184) touched only this one.
- `source/http3.c` + `misc3.c` (about 2500 lines) — **webdav-handler 3.3 beta 6
  (12.7.2014)**. It is an OS4 DOS handler that mounts one WebDAV URL as a volume
  (mountlist `DAV0`, `Startup = URL https://user:pass@host/path NAME Volume`).
  The code was last changed in 2014.
- `source/neon/` — a prebuilt **neon 0.29.6** static library (built 2013 against
  OpenSSL 1.0.1e). The release binary has **OpenSSL 1.0.1g (2014)** statically
  linked, so it supports TLS 1.2 at most and has no TLS 1.3.
- `rx3h/`, `utils/`, `cx/`: ARexx library and small tools for the v1 handler.

No licence is stated for the handler code. Only `contributors.txt` names Chris
Young. neon is LGPL.

## Nextcloud fit

Nextcloud's file access is WebDAV at
`https://host/remote.php/dav/files/USER/`, using Basic auth. Accounts with 2FA
need an app password. On paper the handler covers the basics:

| Operation | Handler | Nextcloud |
|---|---|---|
| Directory listing | PROPFIND depth 1: name, size, mtime, resourcetype | supported |
| Read | PROPFIND (size), then GET | supported |
| Write (MODE_NEWFILE) | buffered in RAM, one PUT on Close | supported |
| MakeDir / Delete / Rename | MKCOL / DELETE / MOVE (no overwrite) | supported |
| Comment | PROPPATCH `urn:amiga:comment` (dead property) | stored by custom-properties backend |
| Auth | neon Basic/Digest, user:pass taken from the URL | Basic challenge |
| TLS | SNI yes; certificate check **disabled** (`http_cert_verify` returns 0) | TLS 1.2 still common; TLS 1.3-only servers fail |

So the protocol matches. The handler is not usable as it stands, though, because
of the problems below. Line numbers are in `source/http3.c`.

## Blocking problems

1. **Locking is broken whichever class the server reports.** At mount time the
   handler sends OPTIONS (2109-2126) and picks a lock mode from the DAV class.
   - Current Nextcloud ships the `files_lock` app, which is enabled by default.
     Its `LockPlugin` extends `Sabre\DAV\Locks\Plugin`, so the server reports
     **class 2**. The handler then sends a real WebDAV LOCK for every AmigaDOS
     `Lock()` (1546-1559, 1774):
     - `davlock->type = ne_lockscope_shared` sets the wrong field (it should be
       `scope`), so every lock is EXCLUSIVE, even ACCESS_READ ones. A second
       `Lock()` on the same object (for example the Workbench drawer and a Shell
       CD) gets 423 and fails as "object not found". files_lock also allows only
       one lock per file id.
     - sabre's `httpLock` **creates an empty file** when the URL does not exist
       (sabre Locks/Plugin.php:203-206). Every existence check such as
       `Lock("x")` therefore leaves a 0-byte file on the server.
     - No neon lockstore is registered, so lock tokens are never sent with
       PUT/DELETE/MOVE. Writing to an object the Amiga has locked itself gets
       423.
     - Other Nextcloud clients see the files as locked while the Amiga holds
       locks. If the handler dies, the locks stay until they time out.
   - An older Nextcloud without files_lock reports class 1, which turns on
     FAKELOCKS. Then `Lock()` never contacts the server and **never fails**,
     even for names that do not exist. Also, `http_copylock` (1628-1633)
     **always fails DupLock and ParentDir** because `davlock` is NULL. The
     FAKELOCKS/S switch is commented out (2093-2099), so it cannot be forced.
   - Nextcloud's own FakeLockerPlugin is enabled only for the WebDAVFS, OneNote
     and Microsoft-WebDAV user agents (apps/dav/lib/Server.php:282-287). The
     handler sends `webdav-handler/3.3`, so it does not get it.
2. **Writes can lose data without any error.**
   - `http_close` (863-880) uses `ne_request_dispatch`, which returns NE_OK for
     any HTTP status. A 403, 423, 507 or 413 on PUT is never checked. A network
     error only goes to DebugPrintF, and Close still returns DOSTRUE.
   - MODE_OLDFILE and MODE_READWRITE handles leave `openmode` at 0, so
     `http_write` (988-999) copies into a 512-byte neon buffer at `posn`. That
     is a **heap overflow** on any in-place write.
   - MODE_READWRITE (ACTION_FINDUPDATE) goes to `http_open` (GET), so it cannot
     create a new file.
   - A 0-byte file is never created: there is no PUT when `cachemodified` is
     FALSE.
3. **Non-ASCII names break.** Amiga names are Latin-1 and Nextcloud uses UTF-8.
   No conversion is done in either direction (`ne_path_escape` at 444,
   `ne_path_unescape` at 1216), so å ä ö fail or show as mojibake. The same
   applies to comments, which are put into UTF-8 XML as raw bytes.
4. **Delete on a directory deletes the whole tree.** Nextcloud's DELETE on a
   collection is recursive, and `http_delete` (1503) does not check whether the
   directory is empty, as AmigaDOS expects (ERROR_DIRECTORY_NOT_EMPTY). The
   Nextcloud trash bin is the only safety net.

## Other problems

- Whole files are kept in RAM, both when reading (the cache grows with every
  read, 944-947) and when writing. neon aborts on out-of-memory, which kills
  the handler.
- Sizes are 32-bit: `ULONG len/posn`, `atol(getcontentlength)` (1259), and
  32-bit ACTION_SEEK only. Files over 2 GB break.
  `ACTION_GET_FILE_POSITION64` returns the file length instead of the
  position (1794).
- Nearly every packet opens a new session (`global_session` is commented out,
  189). Each one costs TCP + TLS + a 401 round trip. File-handle sessions are
  never destroyed in `http_close`, so sockets and memory leak. `filetohost`
  calls AllocVec(100) for the credentials on every call and never frees it.
- Each listing includes the directory itself (`http_props_callback_edc` does
  not skip the self `<response>`). On Nextcloud the root shows an entry named
  after the username. This is the "dummy ? entry" listed under known bugs.
- `http_setdate` exists but is not dispatched. It is also wrong: it reads the
  DateStamp pointer as a BSTR into a 12-byte struct (1481). Nextcloud would
  need `{DAV:}lastmodified` or `X-OC-Mtime` on PUT instead. SET_PROTECT,
  SAME_LOCK (commented out), CHANGE_FILE_POSITION64 and the file-size packets
  are missing.
- ACTION_INFO reports 0 blocks. No quota is read: Nextcloud offers
  `quota-available-bytes`.
- Dates are UTC and are shown as local time.
- MOVE errors are mapped wrongly: `httpcode` parses the `ne_get_error` text
  ("412 Precondition…") from the first space. Several error paths leak the
  session.
- Credentials are kept as plain text in the mountlist. user:pass is copied into
  100 bytes, and the username cannot contain ':'. DebugPrintF runs on every
  packet (2177).
- Rebuilding needs the 2013 neon binary plus old `-lssl -lcrypto` static libs,
  or a neon rebuilt for AmiSSL v5 (OpenSSL 3 API). It also uses the SDK's
  `PRIVATE_ExamineDirContext`.

## Verdict

The handler speaks the right protocol and is a useful reference: the OS4
packet loop, the ExamineData/ExamineDir handling and the neon calls. It
**cannot be used as-is** for Nextcloud. With a current server, locking
alone makes it unusable, and the unchecked PUT status means silent data loss.

Making it usable means reworking most of `http3.c`:

- Lock model: local locks only, with no server LOCK.
- UTF-8 conversion for names and comments.
- Streaming or a temp file instead of whole files in RAM, and 64-bit sizes.
- Checked PUT status, with errors returned to Close/Write.
- Session reuse.
- Directory-empty check before DELETE.
- Mtime set via `X-OC-Mtime`, and quota for Info.
- A current TLS stack (AmiSSL v5) with certificate verification.

That amounts to a fork that is mostly a rewrite, or a new handler that uses
project 63 as a reference. Before publishing anything based on it, the licence
needs to be clarified with Chris Young or openamiga.org.
