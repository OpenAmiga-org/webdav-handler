#!/usr/bin/env python3
"""A fake WebDAV server that fails on purpose, to see which message
AmigaDOS shows for each kind of server error. Test tool. Public domain.

    python3 -I testserver/errsim.py [PORT]     (default 8082)

The guest reaches it at http://10.0.2.2:PORT/. Each top-level drawer
answers every request below it (except listings) with one HTTP status:

    /s403/  403 Forbidden          (a read-only share)
    /s500/  500 Internal Error
    /s503/  503 Service Unavailable
    /s507/  507 Insufficient Storage
    /s423/  423 Locked

Each drawer lists one file, "file" (10 bytes); listings always work.
"""
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

DIRS = {"s403": 403, "s500": 500, "s503": 503, "s507": 507, "s423": 423}
DATE = "Tue, 17 Nov 2026 12:00:00 GMT"


def entry(href, is_dir, size=0):
    rt = "<d:resourcetype><d:collection/></d:resourcetype>" if is_dir else "<d:resourcetype/>"
    extra = "" if is_dir else ("<d:getcontentlength>%d</d:getcontentlength>"
                               "<d:getetag>\"e1\"</d:getetag>" % size)
    return ("<d:response><d:href>%s</d:href><d:propstat><d:prop>%s%s"
            "<d:getlastmodified>%s</d:getlastmodified></d:prop>"
            "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>" % (href, rt, extra, DATE))


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def send(self, code, body=b"", ctype="text/plain"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        while n > 0:
            n -= len(self.rfile.read(min(n, 65536)))

    def status_for(self):
        parts = [p for p in unquote(self.path).split("/") if p]
        return parts, (DIRS.get(parts[0]) if parts else None)

    def do_PROPFIND(self):
        self.body()
        parts, _ = self.status_for()
        depth = self.headers.get("Depth", "1")
        if not parts:
            items = [entry("/", True)]
            if depth != "0":
                items += [entry("/%s/" % d, True) for d in DIRS]
        elif len(parts) == 1 and parts[0] in DIRS:
            items = [entry("/%s/" % parts[0], True)]
            if depth != "0":
                items.append(entry("/%s/file" % parts[0], False, 10))
        elif len(parts) == 2 and parts[0] in DIRS and parts[1] == "file":
            items = [entry("/%s/file" % parts[0], False, 10)]
        else:
            return self.send(404)
        xml = ('<?xml version="1.0"?><d:multistatus xmlns:d="DAV:">%s</d:multistatus>'
               % "".join(items)).encode()
        self.send(207, xml, "application/xml; charset=utf-8")

    def fail(self):
        self.body()
        parts, code = self.status_for()
        self.send(code or 404, b"simulated error\n")

    do_GET = do_PUT = do_DELETE = do_MKCOL = do_MOVE = do_PROPPATCH = do_HEAD = fail

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("DAV", "1")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("errsim: " + (fmt % args) + "\n")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8082
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
