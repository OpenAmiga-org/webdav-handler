/* curltest - checks libcurl (sockets, TLS, certificates, threads) on
 * AmigaOS 4 before it goes into the handler. Not part of the release.
 *
 *   curltest URL [CAFILE file] [CAPATH dir] [USER u PASS p] [INSECURE]
 * Public domain. */
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

static size_t on_data(char *p, size_t s, size_t n, void *ud)
{
	size_t *total = ud;

	if (*total < 300)
		fwrite(p, 1, s * n < 300 ? s * n : 300, stdout);
	*total += s * n;
	return s * n;
}

int main(int argc, char **argv)
{
	CURL *c;
	CURLcode rc;
	char err[CURL_ERROR_SIZE] = "";
	size_t total = 0;
	long code = 0;
	int i;

	if (argc < 2) {
		printf("usage: curltest URL [CAFILE f] [CAPATH d] [USER u PASS p] [INSECURE] [PROPFIND]\n");
		return 5;
	}
	printf("%s\n", curl_version());
	curl_global_init(CURL_GLOBAL_DEFAULT);
	c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_URL, argv[1]);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &total);
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "CAFILE") == 0 && i + 1 < argc)
			curl_easy_setopt(c, CURLOPT_CAINFO, argv[++i]);
		else if (strcmp(argv[i], "CAPATH") == 0 && i + 1 < argc)
			curl_easy_setopt(c, CURLOPT_CAPATH, argv[++i]);
		else if (strcmp(argv[i], "USER") == 0 && i + 1 < argc)
			curl_easy_setopt(c, CURLOPT_USERNAME, argv[++i]);
		else if (strcmp(argv[i], "PASS") == 0 && i + 1 < argc)
			curl_easy_setopt(c, CURLOPT_PASSWORD, argv[++i]);
		else if (strcmp(argv[i], "INSECURE") == 0) {
			curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
		} else if (strcmp(argv[i], "PROPFIND") == 0) {
			curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PROPFIND");
		}
	}
	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	printf("\nresult %d (%s) HTTP %ld, %lu bytes %s\n", (int)rc, curl_easy_strerror(rc),
	       code, (unsigned long)total, err);
	curl_easy_cleanup(c);
	curl_global_cleanup();
	return rc == CURLE_OK ? 0 : 10;
}
