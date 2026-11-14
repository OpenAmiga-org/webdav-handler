#!/bin/bash
# Creates a throwaway Nextcloud test server in Docker.
#
#   HTTP : http://127.0.0.1:8080   (from the OS4 guest: http://10.0.2.2:8080)
#   HTTPS: https://127.0.0.1:8443  (from the OS4 guest: https://10.0.2.2:8443)
#
# Both ports listen on 127.0.0.1 only. The certificate is signed by a local
# test CA (testserver/local/ca.crt). Credentials go to testserver/local/env.
#
# Usage: setup.sh [image-tag]     (default tag: apache = latest release)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LOCAL="$HERE/local"
TAG="${1:-apache}"
NAME=nc-test
VOL=nc-test-data

mkdir -p "$LOCAL"

if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then
	echo "Container $NAME exists already; run reset.sh first." >&2
	exit 1
fi

# --- passwords (kept across resets so scripts and the guest keep working) ---
if [ ! -f "$LOCAL/env" ]; then
	cat > "$LOCAL/env" <<EOF
NC_ADMIN=admin
NC_ADMIN_PASS=$(openssl rand -hex 12)
NC_USER=amiga
NC_USER_PASS=$(openssl rand -hex 12)
NC_HTTP=http://127.0.0.1:8080
NC_HTTPS=https://127.0.0.1:8443
EOF
fi
# shellcheck disable=SC1091
. "$LOCAL/env"

# --- local test CA and server certificate -----------------------------------
if [ ! -f "$LOCAL/ca.crt" ]; then
	openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
		-keyout "$LOCAL/ca.key" -out "$LOCAL/ca.crt" \
		-subj "/CN=nextcloud_client test CA" \
		-addext "basicConstraints=critical,CA:TRUE" \
		-addext "keyUsage=critical,keyCertSign,cRLSign" >/dev/null 2>&1
fi
if [ ! -f "$LOCAL/server.crt" ]; then
	openssl req -newkey rsa:2048 -nodes \
		-keyout "$LOCAL/server.key" -out "$LOCAL/server.csr" \
		-subj "/CN=localhost" >/dev/null 2>&1
	printf 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nauthorityKeyIdentifier=keyid\nsubjectKeyIdentifier=hash\n' > "$LOCAL/server.ext"
	openssl x509 -req -in "$LOCAL/server.csr" -CA "$LOCAL/ca.crt" -CAkey "$LOCAL/ca.key" \
		-CAcreateserial -days 3650 -extfile "$LOCAL/server.ext" -out "$LOCAL/server.crt" >/dev/null 2>&1
fi

# --- container ----------------------------------------------------------------
docker volume create "$VOL" >/dev/null
docker run -d --name "$NAME" \
	-p 127.0.0.1:8080:80 -p 127.0.0.1:8443:443 \
	-v "$VOL":/var/www/html \
	-e SQLITE_DATABASE=nextcloud \
	-e NEXTCLOUD_ADMIN_USER="$NC_ADMIN" \
	-e NEXTCLOUD_ADMIN_PASSWORD="$NC_ADMIN_PASS" \
	-e NEXTCLOUD_TRUSTED_DOMAINS="localhost 127.0.0.1 10.0.2.2" \
	"nextcloud:$TAG" >/dev/null

echo "Waiting for the Nextcloud installation..."
for _ in $(seq 1 120); do
	if docker exec -u www-data "$NAME" php occ status --output=json 2>/dev/null | grep -q '"installed":true'; then
		break
	fi
	sleep 3
done
docker exec -u www-data "$NAME" php occ status

occ() { docker exec -u www-data "$NAME" php occ "$@"; }

# --- HTTPS inside the container -----------------------------------------------
docker cp "$LOCAL/server.crt" "$NAME":/etc/ssl/certs/nc-test.crt
docker cp "$LOCAL/server.key" "$NAME":/etc/ssl/private/nc-test.key
docker exec "$NAME" sh -c 'cat > /etc/apache2/sites-available/nc-test-ssl.conf <<EOF
<VirtualHost *:443>
	DocumentRoot /var/www/html
	SSLEngine on
	SSLCertificateFile /etc/ssl/certs/nc-test.crt
	SSLCertificateKeyFile /etc/ssl/private/nc-test.key
</VirtualHost>
EOF
a2enmod -q ssl >/dev/null && a2ensite -q nc-test-ssl >/dev/null'
# Apache runs as the container's main process, so restart the container
# instead of reloading Apache inside it.
docker restart "$NAME" >/dev/null
for _ in $(seq 1 60); do
	if curl -s -o /dev/null --cacert "$LOCAL/ca.crt" "$NC_HTTPS/status.php"; then
		break
	fi
	sleep 1
done

# --- settings for testing -----------------------------------------------------
# Failed logins from the test scripts must not slow the server down.
occ config:system:set auth.bruteforce.protection.enabled --value=false --type=boolean >/dev/null
occ background:cron >/dev/null

# --- test user and app password -----------------------------------------------
OC_PASS="$NC_USER_PASS" docker exec -e OC_PASS -u www-data "$NAME" \
	php occ user:add --password-from-env --display-name="Amiga Tester" "$NC_USER" >/dev/null
APP_PASS=$(occ user:auth-tokens:add "$NC_USER" 2>/dev/null | tail -1 | awk '{print $NF}') || true
if [ -n "${APP_PASS:-}" ]; then
	sed -i '/^NC_APP_PASS=/d' "$LOCAL/env"
	echo "NC_APP_PASS=$APP_PASS" >> "$LOCAL/env"
fi

echo
occ app:list --enabled 2>/dev/null | grep -E 'files_lock|dav:' || true
echo "Nextcloud test server is ready:"
echo "  $NC_HTTP  and  $NC_HTTPS  (CA: $LOCAL/ca.crt)"
echo "  user $NC_USER, credentials in $LOCAL/env"
