# nextcloud-handler

An AmigaOS 4 filesystem handler that mounts a Nextcloud account (or another
WebDAV server) as a volume, for example `Nextcloud:`. It is built on
filesysbox.library and libcurl.

Status: in development.

- `src/core/`  WebDAV client and filesystem logic (portable C)
- `src/os4/`   AmigaOS 4 handler (filesysbox)
- `src/linux/` Linux FUSE build of the same core, for fast tests
- `tests/`     tests against the Docker test server
- `testserver/` throwaway Nextcloud in Docker (`setup.sh`, `reset.sh`)
- `docs/`      design notes and end-user documentation

Public domain, see `UNLICENSE`.
