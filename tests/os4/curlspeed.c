/* curlspeed - times downloads with plain libcurl, to tell the network and
 * TLS speed of a machine apart from the handler's own costs.
 * Test tool, not part of the release. Public domain.
 *
 *   curlspeed URL CREDFILE CAFILE full      one GET of the whole file
 *   curlspeed URL CREDFILE CAFILE ranges    range GETs like the handler
 *                                            (256 KB, doubling to 4 MB)
 *   curlspeed URL CREDFILE CAFILE put FILE  one PUT of a local file
 *   curlspeed URL CREDFILE - raw            no curl: one GET (http:// with
 *                                            a numeric host only), blocking
 *                                            recv() on a bsdsocket socket
 *
 * Linked like the handler, with the cross-compiler image's libcurl.a
 * (clib4 sockets, waits with clib4's poll()); see tools/os4.mk.
 *
 * Every download prints the gaps of 0.5 s or more between two pieces of
 * data (when, how long, after how many bytes).
 *
 * CREDFILE holds "user:password" on one line (kept off the command line).
 * A trailing "RCVBUF n" sets the socket receive buffer (TCP window) to n
 * bytes; the default is printed either way.
 */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <proto/exec.h>
#include <interfaces/bsdsocket.h>

/* src/os4/amissl.c, the same AmiSSL setup as the handler */
int  amissl_open(void);
void amissl_close(void);

/* raw mode only: bsdsocket.library directly, without clib4 */
static struct SocketIFace *ISock;

static double now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* gaps between two pieces of data */
static double t_start, t_last;
static long long gap_bytes;
static int gaps;
static double gap_sum;

static void gap_reset(void)
{
	t_start = t_last = now();
	gap_bytes = 0;
}

static void gap_data(size_t n)
{
	double t = now();

	if (t - t_last >= 0.5) {
		gaps++;
		gap_sum += t - t_last;
		printf("    gap %.2fs at %.2fs after %lld bytes\n", t - t_last, t_last - t_start, gap_bytes);
	}
	t_last = t;
	gap_bytes += n;
}

static size_t on_data(char *p, size_t s, size_t n, void *ud)
{
	(void)p;
	gap_data(s * n);
	*(curl_off_t *)ud += (curl_off_t)(s * n);
	return s * n;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const char *in, char *out)
{
	size_t i, n = strlen(in);

	for (i = 0; i < n; i += 3) {
		unsigned v = (unsigned char)in[i] << 16;
		if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
		if (i + 2 < n) v |= (unsigned char)in[i + 2];
		*out++ = b64[(v >> 18) & 63];
		*out++ = b64[(v >> 12) & 63];
		*out++ = i + 1 < n ? b64[(v >> 6) & 63] : '=';
		*out++ = i + 2 < n ? b64[v & 63] : '=';
	}
	*out = 0;
}

/* one GET without curl: bsdsocket.library socket, blocking recv() */
static int raw_get(const char *url, const char *cred)
{
	static char buf[65536];
	char host[64], auth[700], req[1024];
	const char *p, *path;
	unsigned a, b, c, d, port = 80;
	struct sockaddr_in sa;
	struct Library *sb;
	long long total = 0;
	double t0, t1;
	int fd, n, rb = 0;
	socklen_t len = sizeof(rb);

	if (strncmp(url, "http://", 7) != 0)
		return -1;
	p = url + 7;
	path = strchr(p, '/');
	if (path == NULL || (size_t)(path - p) >= sizeof(host))
		return -1;
	memcpy(host, p, path - p);
	host[path - p] = 0;
	if (sscanf(host, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) < 4)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
	base64(cred, auth);
	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Basic %s\r\n"
	         "Connection: close\r\n\r\n", path, host, auth);
	sb = IExec->OpenLibrary("bsdsocket.library", 4);
	if (sb == NULL)
		return -1;
	ISock = (struct SocketIFace *)IExec->GetInterface(sb, "main", 1, NULL);
	if (ISock == NULL)
		return -1;
	t0 = now();
	fd = ISock->socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || ISock->connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		printf("cannot connect\n");
		return -1;
	}
	ISock->getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, &len);
	ISock->send(fd, req, strlen(req), 0);
	gap_reset();
	while ((n = ISock->recv(fd, buf, sizeof(buf), 0)) > 0) {
		gap_data(n);
		total += n;
	}
	t1 = now();
	ISock->CloseSocket(fd);
	IExec->DropInterface((struct Interface *)ISock);
	IExec->CloseLibrary(sb);
	printf("raw GET (SO_RCVBUF %d): %lld bytes with headers in %.2fs = %.2f MB/s\n", rb, total,
	       t1 - t0, total / 1048576.0 / (t1 - t0));
	return 0;
}

static int rcvbuf;

static int on_sockopt(void *ud, curl_socket_t fd, curlsocktype purpose)
{
	int v = 0;
	socklen_t len = sizeof(v);

	(void)ud;
	(void)purpose;
	/* curl's sockets are clib4 descriptors */
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, &len);
	if (rcvbuf > 0) {
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
		len = sizeof(rcvbuf);
		getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len);
		printf("new connection: SO_RCVBUF %d -> %d\n", v, rcvbuf);
	} else {
		printf("new connection: SO_RCVBUF %d (default)\n", v);
	}
	return CURL_SOCKOPT_OK;
}

static char errbuf[CURL_ERROR_SIZE];

static int get(CURL *c, const char *range, curl_off_t *got, int nr)
{
	double tt = 0, tc = 0, ta = 0, ts = 0;
	long code = 0;
	CURLcode rc;

	*got = 0;
	gap_reset();
	curl_easy_setopt(c, CURLOPT_RANGE, range);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, got);
	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tt);
	curl_easy_getinfo(c, CURLINFO_CONNECT_TIME, &tc);
	curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &ta);
	curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &ts);
	printf("%3d %-22s HTTP %ld %9lld bytes  connect %.2f tls %.2f first byte %.2f total %.2fs  %.2f MB/s%s%s\n",
	       nr, range ? range : "(whole file)", code, (long long)*got, tc, ta, ts, tt,
	       tt > 0 ? *got / 1048576.0 / tt : 0.0, rc ? "  " : "", rc ? curl_easy_strerror(rc) : "");
	if (rc != CURLE_OK && errbuf[0])
		printf("    %s\n", errbuf);
	return rc == CURLE_OK && code / 100 == 2 ? 0 : -1;
}

int main(int argc, char **argv)
{
	char cred[512], range[64];
	curl_off_t off = 0, got, want = 256 * 1024, total = 0;
	double t = 0, all = 0;
	FILE *f;
	CURL *c;
	int nr = 0;

	if (argc < 5)
		return 5;
	if (argc > 6 && strcmp(argv[argc - 2], "RCVBUF") == 0)
		rcvbuf = atoi(argv[argc - 1]);
	f = fopen(argv[2], "r");
	if (f == NULL || fgets(cred, sizeof(cred), f) == NULL)
		return 10;
	fclose(f);
	cred[strcspn(cred, "\r\n")] = 0;
	if (amissl_open() != 0) {
		printf("no AmiSSL\n");
		return 20;
	}
	printf("%s\n", curl_version());
	if (strcmp(argv[4], "raw") == 0) {
		raw_get(argv[1], cred);
		printf("%d gaps, %.2fs in all\n", gaps, gap_sum);
		amissl_close();
		return 0;
	}
	{
		CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
		if (rc != CURLE_OK) {
			printf("curl_global_init: %d (%s)\n", (int)rc, curl_easy_strerror(rc));
			return 20;
		}
	}
	c = curl_easy_init();
	if (c == NULL) {
		printf("curl_easy_init failed\n");
		return 20;
	}
	curl_easy_setopt(c, CURLOPT_URL, argv[1]);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
	if (strcmp(argv[argc - 1], "VERBOSE") == 0)
		curl_easy_setopt(c, CURLOPT_VERBOSE, 1L), curl_easy_setopt(c, CURLOPT_STDERR, stdout);
	curl_easy_setopt(c, CURLOPT_USERPWD, cred);
	curl_easy_setopt(c, CURLOPT_CAINFO, argv[3]);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, on_sockopt);
	if (strcmp(argv[4], "full") == 0) {
		get(c, NULL, &got, 1);
	} else if (strcmp(argv[4], "put") == 0 && argc > 5) {
		double tt = 0;
		long code = 0;
		curl_off_t up = 0;
		CURLcode rc;

		f = fopen(argv[5], "rb");
		if (f == NULL)
			return 10;
		fseek(f, 0, SEEK_END);
		up = ftell(f);
		fseek(f, 0, SEEK_SET);
		curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(c, CURLOPT_UPLOAD_BUFFERSIZE, 1024L * 1024); /* as the handler */
		curl_easy_setopt(c, CURLOPT_READDATA, f);
		curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, up);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &got);
		got = 0;
		rc = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tt);
		printf("PUT HTTP %ld %lld bytes total %.2fs  %.2f MB/s%s%s\n", code, (long long)up, tt,
		       tt > 0 ? up / 1048576.0 / tt : 0.0, rc ? "  " : "", rc ? curl_easy_strerror(rc) : "");
		fclose(f);
	} else {
		for (;;) {
			snprintf(range, sizeof(range), "%lld-%lld", (long long)off, (long long)(off + want - 1));
			if (get(c, range, &got, ++nr) != 0 || got == 0)
				break;
			curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &t);
			all += t;
			total += got;
			off += got;
			if (got < want)
				break;
			if (want < 4 * 1024 * 1024)
				want *= 2;
		}
		printf("%lld bytes in %d requests, %.2fs = %.2f MB/s\n", (long long)total, nr, all,
		       all > 0 ? total / 1048576.0 / all : 0.0);
	}
	printf("%d gaps, %.2fs in all\n", gaps, gap_sum);
	curl_easy_cleanup(c);
	curl_global_cleanup();
	amissl_close();
	return 0;
}
