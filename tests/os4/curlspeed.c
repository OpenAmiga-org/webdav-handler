/* curlspeed - times downloads with plain libcurl, to tell the network and
 * TLS speed of a machine apart from the handler's own costs.
 * Test tool, not part of the release. Public domain.
 *
 *   curlspeed URL CREDFILE CAFILE full      one GET of the whole file
 *   curlspeed URL CREDFILE CAFILE ranges    range GETs like the handler
 *                                            (256 KB, doubling to 4 MB)
 *   curlspeed URL CREDFILE CAFILE put FILE  one PUT of a local file
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

/* src/os4/amissl.c, the same AmiSSL setup as the handler */
int  amissl_open(void);
void amissl_close(void);

static size_t on_data(char *p, size_t s, size_t n, void *ud)
{
	(void)p;
	*(curl_off_t *)ud += (curl_off_t)(s * n);
	return s * n;
}

static int rcvbuf;

static int on_sockopt(void *ud, curl_socket_t fd, curlsocktype purpose)
{
	int v = 0;
	socklen_t len = sizeof(v);

	(void)ud;
	(void)purpose;
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

static int get(CURL *c, const char *range, curl_off_t *got, int nr)
{
	double tt = 0, tc = 0, ta = 0, ts = 0;
	long code = 0;
	CURLcode rc;

	*got = 0;
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
	curl_global_init(CURL_GLOBAL_DEFAULT);
	c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_URL, argv[1]);
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
	curl_easy_cleanup(c);
	curl_global_cleanup();
	amissl_close();
	return 0;
}
