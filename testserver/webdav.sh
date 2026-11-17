#!/bin/bash
# A plain WebDAV test server (Apache httpd with mod_dav) next to the
# Nextcloud one, to test webdav-handler with a server that is not
# Nextcloud.
#
#   http://127.0.0.1:8081/dav/   (from the OS4 guest: http://10.0.2.2:8081/dav/)
#
# User amiga; the password goes to testserver/local/env as WEBDAV_PASS.
# Usage: webdav.sh            start (again, with empty files)
#        webdav.sh --remove   remove the container
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LOCAL="$HERE/local"
NAME=wdv-test
IMAGE=httpd:2.4

docker rm -f "$NAME" >/dev/null 2>&1 || true
if [ "${1:-}" = "--remove" ]; then
	echo "WebDAV test server removed."
	exit 0
fi
mkdir -p "$LOCAL"
touch "$LOCAL/env"
if ! grep -q '^WEBDAV_PASS=' "$LOCAL/env"; then
	echo "WEBDAV_URL=http://127.0.0.1:8081/dav" >> "$LOCAL/env"
	echo "WEBDAV_USER=amiga" >> "$LOCAL/env"
	echo "WEBDAV_PASS=$(openssl rand -hex 12)" >> "$LOCAL/env"
fi
# shellcheck disable=SC1091
. "$LOCAL/env"

docker run -d --name "$NAME" -p 127.0.0.1:8081:80 "$IMAGE" >/dev/null
docker cp "$HERE/webdav/httpd.conf" "$NAME":/usr/local/apache2/conf/httpd.conf
docker exec "$NAME" sh -c "htpasswd -cbB /usr/local/apache2/conf/dav.passwd '$WEBDAV_USER' '$WEBDAV_PASS' >/dev/null 2>&1 &&
	mkdir -p /usr/local/apache2/htdocs/dav /usr/local/apache2/var &&
	chown -R daemon:daemon /usr/local/apache2/htdocs/dav /usr/local/apache2/var"
docker restart "$NAME" >/dev/null
for _ in $(seq 1 30); do
	if curl -s -o /dev/null -u "$WEBDAV_USER:$WEBDAV_PASS" -X PROPFIND -H "Depth: 0" "$WEBDAV_URL/"; then
		break
	fi
	sleep 1
done
code=$(curl -s -o /dev/null -w '%{http_code}' -u "$WEBDAV_USER:$WEBDAV_PASS" -X PROPFIND -H "Depth: 0" "$WEBDAV_URL/")
echo "WebDAV test server: $WEBDAV_URL/ (user $WEBDAV_USER, PROPFIND -> $code)"
