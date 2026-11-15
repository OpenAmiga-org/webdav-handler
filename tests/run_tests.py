#!/usr/bin/env python3
"""Tests the core (through the Linux FUSE build) against the Docker test
server. Every result is checked on the server directly over WebDAV, not
only through the mount.

    python3 -I tests/run_tests.py [--big] [--keep] [-k NAME]

--big also writes and reads a file larger than 2 GiB.
"""
import base64
import errno
import hashlib
import os
import random
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "linux", "nextcloud-fuse")
ENVFILE = os.path.join(ROOT, "testserver", "local", "env")
CAFILE = os.path.join(ROOT, "testserver", "local", "ca.crt")
WORK = os.environ.get("NCH_TESTDIR", "/tmp/nch-test")
MNT = os.path.join(WORK, "mnt")
KEEP = os.path.join(WORK, "kept")
TMP = os.path.join(WORK, "tmp")
CACHE_MS = 1000


def load_env():
    env = {}
    with open(ENVFILE) as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                env[k] = v
    return env


ENV = load_env()
USER = ENV["NC_USER"]
BASE = ENV["NC_HTTPS"] + "/remote.php/dav/files/" + USER
CTX = ssl.create_default_context(cafile=CAFILE)
AUTH = "Basic " + base64.b64encode(f"{USER}:{ENV['NC_APP_PASS']}".encode()).decode()


# --- direct WebDAV access (the reference) -----------------------------------

def dav(method, path, data=None, headers=None):
    url = BASE + urllib.parse.quote(path)
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", AUTH)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, context=CTX, timeout=120) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def srv_get(path):
    st, body = dav("GET", path)
    return body if st == 200 else None


def srv_put(path, data):
    st, _ = dav("PUT", path, data)
    assert st in (201, 204), st


def srv_props(path):
    body = ('<?xml version="1.0"?><d:propfind xmlns:d="DAV:" xmlns:a="urn:amiga:">'
            '<d:prop><d:getcontentlength/><d:getlastmodified/><d:resourcetype/>'
            '<a:comment/></d:prop></d:propfind>').encode()
    st, data = dav("PROPFIND", path, body, {"Depth": "0"})
    if st == 404:
        return None
    assert st == 207, st
    ns = {"d": "DAV:", "a": "urn:amiga:"}
    root = ET.fromstring(data)
    props = {}
    for ps in root.iter("{DAV:}propstat"):
        if "200" not in ps.find("d:status", ns).text:
            continue
        p = ps.find("d:prop", ns)
        for child in p:
            props[child.tag] = child
    out = {
        "dir": props.get("{DAV:}resourcetype") is not None and
               props["{DAV:}resourcetype"].find("d:collection", ns) is not None,
        "size": int(props["{DAV:}getcontentlength"].text)
                if "{DAV:}getcontentlength" in props else 0,
        "mtime": props["{DAV:}getlastmodified"].text if "{DAV:}getlastmodified" in props else None,
        "comment": props["{urn:amiga:}comment"].text if "{urn:amiga:}comment" in props else None,
    }
    return out


def srv_list(path):
    st, data = dav("PROPFIND", path, None, {"Depth": "1"})
    assert st == 207, st
    root = ET.fromstring(data)
    names = []
    for r in root.findall("{DAV:}response"):
        href = urllib.parse.unquote(r.find("{DAV:}href").text).rstrip("/")
        name = href.rsplit("/", 1)[-1]
        names.append(name)
    return sorted(names[1:])


def occ(*args):
    return subprocess.run(["docker", "exec", "-u", "www-data", "nc-test", "php", "occ", *args],
                          capture_output=True, text=True, check=True).stdout


# --- mount ---------------------------------------------------------------------

class Mount:
    def __init__(self, *extra):
        self.extra = extra
        self.proc = None
        self.log = os.path.join(WORK, "fuse.log")

    def __enter__(self):
        os.makedirs(MNT, exist_ok=True)
        os.makedirs(TMP, exist_ok=True)
        env = dict(os.environ, NCH_PASSWORD=ENV["NC_APP_PASS"])
        self.logf = open(self.log, "a")
        self.proc = subprocess.Popen(
            [BIN, "--url", ENV["NC_HTTPS"], "--user", USER, "--cafile", CAFILE,
             "--cache-ms", str(CACHE_MS), "--tempdir", TMP, "--keepdir", KEEP,
             "--debug" if os.environ.get("NCH_DEBUG") else "--verbose",
             *self.extra, MNT, "-f"],
            env=env, stdin=subprocess.DEVNULL, stdout=self.logf, stderr=self.logf)
        for _ in range(100):
            if os.path.ismount(MNT):
                return self
            if self.proc.poll() is not None:
                raise RuntimeError("mount failed, see " + self.log)
            time.sleep(0.1)
        raise RuntimeError("mount timed out")

    def __exit__(self, *a):
        subprocess.run(["fusermount3", "-u", MNT], capture_output=True)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.logf.close()


def m(path):
    return os.path.join(MNT, path.lstrip("/"))


def expect_errno(code, fn, *args):
    try:
        fn(*args)
    except OSError as e:
        assert e.errno == code, f"expected {errno.errorcode[code]}, got {e}"
        return
    raise AssertionError(f"expected {errno.errorcode[code]}, but it worked")


# --- tests -----------------------------------------------------------------------
# Each test works in its own folder /T/<name> created by the runner.

def t_small_file(d):
    with open(m(d + "/a.txt"), "w") as f:
        f.write("hello\n")
    assert srv_get(d + "/a.txt") == b"hello\n"
    assert open(m(d + "/a.txt")).read() == "hello\n"


def t_names(d):
    names = ["åäö ÅÄÖ.txt", "space name.txt", "hash#percent%plus+.txt",
             "amp&semi;q?.txt", "quote'brackets[1](2).txt", "€uro.txt"]
    for n in names:
        with open(m(d + "/" + n), "wb") as f:
            f.write(n.encode())
    assert srv_list(d) == sorted(names), srv_list(d)
    for n in names:
        assert srv_get(d + "/" + n) == n.encode(), n
    assert sorted(os.listdir(m(d))) == sorted(names)


def t_case_insensitive(d):
    srv_put(d + "/ReadMe.TXT", b"case")
    time.sleep(CACHE_MS / 1000 + 0.2)
    assert open(m(d + "/readme.txt"), "rb").read() == b"case"
    assert os.path.exists(m(d + "/README.txt"))
    # writing with another case replaces the existing file, as on an Amiga
    with open(m(d + "/readme.txt"), "wb") as f:
        f.write(b"new")
    assert srv_list(d) == ["ReadMe.TXT"], srv_list(d)
    assert srv_get(d + "/ReadMe.TXT") == b"new"
    os.mkdir(m(d + "/SubDir"))
    with open(m(d + "/subdir/x"), "wb") as f:
        f.write(b"x")
    assert srv_get(d + "/SubDir/x") == b"x"


def t_overwrite(d):
    srv_put(d + "/o.txt", b"old content that is longer")
    with open(m(d + "/o.txt"), "wb") as f:
        f.write(b"new")
    assert srv_get(d + "/o.txt") == b"new"


def t_update_in_place(d):
    srv_put(d + "/u.bin", b"0123456789")
    time.sleep(CACHE_MS / 1000 + 0.2)
    with open(m(d + "/u.bin"), "r+b") as f:
        assert f.read(3) == b"012"
        f.seek(5)
        f.write(b"XY")
    assert srv_get(d + "/u.bin") == b"01234XY789"
    with open(m(d + "/u.bin"), "ab") as f:
        f.write(b"END")
    assert srv_get(d + "/u.bin") == b"01234XY789END"


def wait_for(fn, secs=5):
    """Linux FUSE runs release() after close() returns; filesysbox does not."""
    end = time.time() + secs
    while True:
        v = fn()
        if v or time.time() > end:
            return v
        time.sleep(0.1)


def t_empty_file(d):
    open(m(d + "/empty"), "wb").close()
    p = wait_for(lambda: srv_props(d + "/empty"))
    assert p is not None and p["size"] == 0, p


def t_truncate(d):
    srv_put(d + "/t.bin", b"abcdefghij")
    time.sleep(CACHE_MS / 1000 + 0.2)
    os.truncate(m(d + "/t.bin"), 4)
    assert srv_get(d + "/t.bin") == b"abcd"


def t_large_and_ranges(d):
    data = random.randbytes(40 * 1024 * 1024 + 123)
    with open(m(d + "/big.bin"), "wb") as f:
        for i in range(0, len(data), 1 << 20):
            f.write(data[i:i + (1 << 20)])
    got = srv_get(d + "/big.bin")
    assert got is not None and hashlib.sha256(got).digest() == hashlib.sha256(data).digest()
    with open(m(d + "/big.bin"), "rb") as f:
        assert hashlib.sha256(f.read()).digest() == hashlib.sha256(data).digest()
        for _ in range(50):
            off = random.randrange(0, len(data))
            n = random.randrange(1, 300000)
            f.seek(off)
            assert f.read(n) == data[off:off + n], (off, n)
        f.seek(len(data) - 10)
        assert f.read(100) == data[-10:]
        assert f.read(10) == b""


def t_changed_while_reading(d):
    # a file replaced on the server during a read must give an error, not
    # a mix of the old and the new version
    old = random.randbytes(3 * 1024 * 1024)
    assert dav("PUT", d + "/c.bin", old)[0] in (201, 204)
    with open(m(d + "/c.bin"), "rb", buffering=0) as f:
        assert f.read(4096) == old[:4096]
        # Nextcloud's ETag comes from mtime (seconds), inode and size: a
        # same-size upload in the same second can keep it
        time.sleep(1.1)
        assert dav("PUT", d + "/c.bin", bytes(len(old)))[0] in (201, 204)
        f.seek(2 * 1024 * 1024)
        expect_errno(errno.ETXTBSY, f.read, 4096)
    with open(m(d + "/c.bin"), "rb") as f:
        assert f.read() == bytes(len(old)), "a new open reads the new version"


def t_mkdir_rmdir(d):
    os.mkdir(m(d + "/x"))
    assert srv_props(d + "/x")["dir"]
    expect_errno(errno.EEXIST, os.mkdir, m(d + "/x"))
    expect_errno(errno.EEXIST, os.mkdir, m(d + "/X"))
    with open(m(d + "/x/f"), "wb") as f:
        f.write(b"1")
    expect_errno(errno.ENOTEMPTY, os.rmdir, m(d + "/x"))
    assert srv_get(d + "/x/f") == b"1", "rmdir must not delete a full folder"
    os.unlink(m(d + "/x/f"))
    os.rmdir(m(d + "/x"))
    assert srv_props(d + "/x") is None


def t_rmdir_sees_server_files(d):
    os.mkdir(m(d + "/y"))
    assert os.listdir(m(d + "/y")) == []
    srv_put(d + "/y/added-elsewhere", b"1")
    expect_errno(errno.ENOTEMPTY, os.rmdir, m(d + "/y"))
    assert srv_get(d + "/y/added-elsewhere") == b"1"


def t_rename(d):
    srv_put(d + "/r1", b"r1")
    os.mkdir(m(d + "/rd"))
    with open(m(d + "/rd/inner"), "wb") as f:
        f.write(b"inner")
    time.sleep(CACHE_MS / 1000 + 0.2)
    os.rename(m(d + "/r1"), m(d + "/r2"))
    assert srv_get(d + "/r1") is None and srv_get(d + "/r2") == b"r1"
    os.rename(m(d + "/rd"), m(d + "/rd2"))
    assert srv_get(d + "/rd2/inner") == b"inner"
    assert open(m(d + "/rd2/inner"), "rb").read() == b"inner"
    expect_errno(errno.ENOENT, os.stat, m(d + "/rd/inner"))
    # AmigaDOS Rename() does not replace an existing object
    srv_put(d + "/r3", b"r3")
    time.sleep(CACHE_MS / 1000 + 0.2)
    expect_errno(errno.EEXIST, os.rename, m(d + "/r2"), m(d + "/r3"))
    assert srv_get(d + "/r3") == b"r3"
    # only the case changes
    os.rename(m(d + "/r2"), m(d + "/R2"))
    assert srv_list(d) == ["R2", "r3", "rd2"], srv_list(d)


def t_unlink(d):
    srv_put(d + "/del", b"x")
    time.sleep(CACHE_MS / 1000 + 0.2)
    os.unlink(m(d + "/DEL"))
    assert srv_get(d + "/del") is None
    expect_errno(errno.ENOENT, os.unlink, m(d + "/del"))


def t_mtime(d):
    with open(m(d + "/dated"), "wb") as f:
        f.write(b"d")
    when = 1262304000 + 3600  # 2010-01-01 01:00 UTC
    os.utime(m(d + "/dated"), (when, when))
    assert srv_props(d + "/dated")["mtime"] == "Fri, 01 Jan 2010 01:00:00 GMT", srv_props(d + "/dated")
    time.sleep(CACHE_MS / 1000 + 0.2)
    assert int(os.stat(m(d + "/dated")).st_mtime) == when


def t_comment(d):
    with open(m(d + "/c"), "wb") as f:
        f.write(b"c")
    os.setxattr(m(d + "/c"), "user.amiga_comment", "A comment <&> åäö".encode())
    assert srv_props(d + "/c")["comment"] == "A comment <&> åäö"
    time.sleep(CACHE_MS / 1000 + 0.2)
    assert os.getxattr(m(d + "/c"), "user.amiga_comment").decode() == "A comment <&> åäö"
    os.removexattr(m(d + "/c"), "user.amiga_comment")
    assert srv_props(d + "/c")["comment"] is None


def t_server_changes_seen(d):
    srv_put(d + "/ext", b"one")
    time.sleep(CACHE_MS / 1000 + 0.2)
    assert open(m(d + "/ext"), "rb").read() == b"one"
    srv_put(d + "/ext", b"changed elsewhere")
    srv_put(d + "/ext2", b"new elsewhere")
    time.sleep(CACHE_MS / 1000 + 0.2)
    assert open(m(d + "/ext"), "rb").read() == b"changed elsewhere"
    assert "ext2" in os.listdir(m(d))


def t_many_files(d):
    for i in range(300):
        with open(m(d + f"/f{i:03d}"), "wb") as f:
            f.write(str(i).encode())
    time.sleep(CACHE_MS / 1000 + 0.2)
    names = sorted(os.listdir(m(d)))
    assert names == [f"f{i:03d}" for i in range(300)]
    assert len(srv_list(d)) == 300


def t_statfs(d):
    st = os.statvfs(m(d))
    assert st.f_blocks > 0 and st.f_bavail > 0


def t_quota_failure(d):
    os.makedirs(KEEP, exist_ok=True)
    before = set(os.listdir(KEEP))
    occ("user:setting", USER, "files", "quota", "15 MB")
    try:
        data = random.randbytes(20 * 1024 * 1024)
        # Linux reports the failed upload at close(); on AmigaOS a
        # requester does, because filesysbox ignores release() errors.
        f = open(m(d + "/too-big"), "wb")
        f.write(data)
        expect_errno(errno.ENOSPC, f.close)
        assert srv_get(d + "/too-big") is None
        new = wait_for(lambda: set(os.listdir(KEEP)) - before)
        assert len(new) == 1, new
        kept = os.path.join(KEEP, new.pop())
        assert open(kept, "rb").read() == data, "kept copy differs"
        os.unlink(kept)
    finally:
        occ("user:setting", USER, "files", "quota", "none")


def t_readonly(d):
    srv_put(d + "/ro", b"ro")
    # a second mount, read-only, on another mount point
    global MNT
    old = MNT
    MNT = os.path.join(WORK, "mnt-ro")
    try:
        with Mount("--readonly"):
            assert open(m(d + "/ro"), "rb").read() == b"ro"
            expect_errno(errno.EROFS, os.mkdir, m(d + "/nope"))
            expect_errno(errno.EROFS, os.unlink, m(d + "/ro"))
    finally:
        MNT = old


def t_big_over_2gib(d):
    size = (1 << 31) + 4096 + 17
    blk = random.randbytes(1 << 20)
    h = hashlib.sha256()
    with open(m(d + "/huge.bin"), "wb") as f:
        left = size
        while left > 0:
            n = min(left, len(blk))
            f.write(blk[:n])
            h.update(blk[:n])
            left -= n
    assert srv_props(d + "/huge.bin")["size"] == size
    with open(m(d + "/huge.bin"), "rb") as f:
        f.seek(size - 17)
        assert f.read() == blk[(size - 17) % len(blk):(size % len(blk)) or None]


TESTS = [t_small_file, t_names, t_case_insensitive, t_overwrite, t_update_in_place,
         t_empty_file, t_truncate, t_large_and_ranges, t_changed_while_reading, t_mkdir_rmdir,
         t_rmdir_sees_server_files, t_rename, t_unlink, t_mtime, t_comment,
         t_server_changes_seen, t_many_files, t_statfs, t_quota_failure, t_readonly]


def main():
    args = sys.argv[1:]
    tests = list(TESTS)
    if "--big" in args:
        tests.append(t_big_over_2gib)
    if "-k" in args:
        name = args[args.index("-k") + 1]
        tests = [t for t in tests if name in t.__name__]
    os.makedirs(WORK, exist_ok=True)
    dav("DELETE", "/T")
    st, _ = dav("MKCOL", "/T")
    assert st == 201, st
    failed = 0
    with Mount():
        for t in tests:
            d = "/T/" + t.__name__[2:]
            assert dav("MKCOL", d)[0] == 201
            start = time.time()
            try:
                t(d)
                print(f"ok    {t.__name__} ({time.time() - start:.1f}s)")
            except Exception as e:  # report and continue
                failed += 1
                print(f"FAIL  {t.__name__}: {type(e).__name__}: {e}")
    if "--keep" not in args:
        dav("DELETE", "/T")
    print(f"\n{len(tests) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
