/* webdav-handler - stand-ins for AmiSSL functions that libcurl calls but an
 * AmiSSL version does not have. Compiled with the AmiSSL 6 headers first
 * (tools/os4.mk); src/os4/amissl.c puts these into amissl_fn[] in place of
 * the missing interface entries.
 *
 * AmiSSL 6 (OpenSSL 4) left these out of its interface (their OpenSSL
 * declarations are deprecated). Where curl calls them:
 *   ASN1_STRING_length       host name check of every https connection
 *   X509_get_pubkey          certificate info, client certificates
 *   EVP_PKEY_get1_RSA, RSA_flags, RSA_free
 *                            client certificates (RSA key check)
 *   MD5_Init/Update/Final    digest authentication
 *   SSL_CTX_set_srp_*        TLS-SRP (never used by the handler)
 * AmiSSL 5 before 5.13 (OpenSSL older than 3.2) has no SSL_get0_group_name,
 * which curl only calls for its verbose output.
 *
 * Each stand-in gets the interface (Self) first, as the stubs of
 * amissl_glue.c pass it. Public domain. */
#define OPENSSL_SUPPRESS_DEPRECATED
#include <proto/amissl.h>
#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <limits.h>
#include <stddef.h>
#include <string.h>

/* --- AmiSSL 6 --------------------------------------------------------------- */

int wdv_v4_ASN1_STRING_length(struct AmiSSLIFace *self, const ASN1_STRING *x)
{
	size_t n = self->ASN1_STRING_get_length(x);

	return n > INT_MAX ? INT_MAX : (int)n;
}

EVP_PKEY *wdv_v4_X509_get_pubkey(struct AmiSSLIFace *self, const X509 *x)
{
	EVP_PKEY *k = self->X509_get0_pubkey(x);

	/* the old function hands out a reference of its own */
	if (k != NULL && self->EVP_PKEY_up_ref(k) != 1)
		k = NULL;
	return k;
}

/* No RSA object is handed out, so curl checks the key as for any other
 * key type. */
void *wdv_v4_EVP_PKEY_get1_RSA(struct AmiSSLIFace *self, EVP_PKEY *pkey)
{
	(void)self;
	(void)pkey;
	return NULL;
}

int wdv_v4_RSA_flags(struct AmiSSLIFace *self, const void *rsa)
{
	(void)self;
	(void)rsa;
	return 0;
}

void wdv_v4_RSA_free(struct AmiSSLIFace *self, void *rsa)
{
	(void)self;
	(void)rsa;
}

/* MD5 through EVP. The caller's MD5_CTX (OpenSSL 3 layout, 92 bytes) only
 * holds the pointer to the EVP context. */
static EVP_MD_CTX *md5_ctx(const void *c)
{
	EVP_MD_CTX *ctx;

	memcpy(&ctx, c, sizeof(ctx));
	return ctx;
}

int wdv_v4_MD5_Init(struct AmiSSLIFace *self, void *c)
{
	EVP_MD_CTX *ctx = self->EVP_MD_CTX_new();

	if (ctx == NULL)
		return 0;
	if (self->EVP_DigestInit_ex(ctx, self->EVP_md5(), NULL) != 1) {
		self->EVP_MD_CTX_free(ctx);
		return 0;
	}
	memcpy(c, &ctx, sizeof(ctx));
	return 1;
}

int wdv_v4_MD5_Update(struct AmiSSLIFace *self, void *c, const void *data, size_t len)
{
	EVP_MD_CTX *ctx = md5_ctx(c);

	return ctx != NULL && self->EVP_DigestUpdate(ctx, data, len) == 1;
}

int wdv_v4_MD5_Final(struct AmiSSLIFace *self, unsigned char *md, void *c)
{
	EVP_MD_CTX *ctx = md5_ctx(c);
	int ok;

	if (ctx == NULL)
		return 0;
	ok = self->EVP_DigestFinal_ex(ctx, md, NULL) == 1;
	self->EVP_MD_CTX_free(ctx);
	memset(c, 0, sizeof(ctx));
	return ok;
}

/* TLS-SRP: refused */
int wdv_v4_SSL_CTX_set_srp_username(struct AmiSSLIFace *self, void *ctx, char *name)
{
	(void)self;
	(void)ctx;
	(void)name;
	return 0;
}

int wdv_v4_SSL_CTX_set_srp_password(struct AmiSSLIFace *self, void *ctx, char *password)
{
	(void)self;
	(void)ctx;
	(void)password;
	return 0;
}

/* --- AmiSSL 5 before 5.13 ----------------------------------------------------- */

const char *wdv_v3_SSL_get0_group_name(struct AmiSSLIFace *self, const void *ssl)
{
	(void)self;
	(void)ssl;
	return NULL;
}
