/* webdav-handler - opens AmiSSL for curl's TLS. AmiSSL is set up per
 * task; all network work happens in the handler process (main.c), so this
 * is called there. Public domain. */
#include <proto/exec.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

#include <errno.h>

#include "os4.h"

/* used by the AmiSSL stubs that curl calls */
struct Library            *AmiSSLMasterBase;
struct AmiSSLMasterIFace  *IAmiSSLMaster;
struct Library            *AmiSSLBase;
struct AmiSSLIFace        *IAmiSSL;

static struct Library     *SockBase;
static struct Interface   *ISock;
static int                 initialised;

/* tools/gen_amissl_glue.py: the stubs that libcurl calls go through
 * amissl_fn[], filled here from the opened interface. AmiSSL 5 (OpenSSL 3)
 * and AmiSSL 6 (OpenSSL 4) have the functions at other places in their
 * interfaces (with the same prototypes, apart from const: checked by
 * tools/check_amissl_api.py); the few that one of them does not have are
 * replaced by src/os4/amissl_compat.c. Done as in sftp-handler. */
extern void *amissl_fn[];
extern const int amissl_nfn;
extern const unsigned int amissl_off_v3[], amissl_off_v4[];
extern void (*const amissl_repl_v4[])(void);
extern const int amissl_v4xx;
extern const int amissl_idx_SSL_get0_group_name;
const char *wdv_v3_SSL_get0_group_name(struct AmiSSLIFace *self, const void *ssl);

static int amissl_api; /* 5 or 6 */

static int open_api(int api)
{
	return IAmiSSLMaster->OpenAmiSSLTags(api,
	                                     AmiSSL_UsesOpenSSLStructs, FALSE,
	                                     AmiSSL_InitAmiSSL, TRUE,
	                                     AmiSSL_GetIAmiSSL, &IAmiSSL,
	                                     AmiSSL_GetAmiSSLBase, &AmiSSLBase,
	                                     AmiSSL_ISocket, ISock,
	                                     AmiSSL_ErrNoPtr, &errno,
	                                     TAG_DONE);
}

int amissl_open(void)
{
	int i;

	SockBase = IExec->OpenLibrary("bsdsocket.library", 4);
	if (SockBase == NULL)
		return -1;
	ISock = IExec->GetInterface(SockBase, "main", 1, NULL);
	if (ISock == NULL)
		return -1;
	AmiSSLMasterBase = IExec->OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
	if (AmiSSLMasterBase == NULL)
		return -2;
	IAmiSSLMaster = (struct AmiSSLMasterIFace *)IExec->GetInterface(AmiSSLMasterBase, "main", 1, NULL);
	if (IAmiSSLMaster == NULL)
		return -2;
	/* AmiSSL 6 when it is there (amisslmaster 6 comes with it), else the
	 * oldest AmiSSL 5 API (AMISSL_V3xx = AmiSSL 5.1), not the SDK's
	 * AMISSL_CURRENT_VERSION (5.27): amisslmaster then opens the newest
	 * AmiSSL 5 installed, and an older one such as 5.25 is not refused.
	 * curl only uses the opaque OpenSSL API, so no structure layouts are
	 * shared. */
	amissl_api = 0;
	if (AmiSSLMasterBase->lib_Version >= 6 && open_api(amissl_v4xx) == 0)
		amissl_api = 6;
	else if (open_api(AMISSL_V3xx) == 0)
		amissl_api = 5;
	else
		return -2;
	initialised = 1;
	for (i = 0; i < amissl_nfn; i++) {
		if (amissl_api == 6 && amissl_off_v4[i] == 0)
			amissl_fn[i] = (void *)amissl_repl_v4[i];
		else
			amissl_fn[i] = *(void **)((char *)IAmiSSL +
			               (amissl_api == 6 ? amissl_off_v4[i] : amissl_off_v3[i]));
	}
	/* Each AmiSSL 5 interface only grows at the end. Everything used is in
	 * AmiSSL 5.1 except SSL_get0_group_name, which came with 5.13. */
	if (amissl_api == 5 && (AmiSSLBase == NULL ||
	    (AmiSSLBase->lib_Version == 5 && AmiSSLBase->lib_Revision < 13)))
		amissl_fn[amissl_idx_SSL_get0_group_name] = (void *)wdv_v3_SSL_get0_group_name;
	return 0;
}

/* 5 or 6 while AmiSSL is open, else 0 */
int amissl_version(void)
{
	return initialised ? amissl_api : 0;
}

/* the opened AmiSSL library, e.g. "amissl_v41x.library 6.1 (14.11.2026)",
 * or "" (its IdString without the line end) */
const char *amissl_name(void)
{
	static char name[80];
	const char *id;
	size_t n = 0;

	name[0] = '\0';
	if (!initialised || AmiSSLBase == NULL || AmiSSLBase->lib_IdString == NULL)
		return name;
	id = (const char *)AmiSSLBase->lib_IdString;
	while (id[n] != '\0' && id[n] != '\r' && id[n] != '\n' && n + 1 < sizeof(name)) {
		name[n] = id[n];
		n++;
	}
	name[n] = '\0';
	return name;
}

void amissl_close(void)
{
	if (initialised)
		IAmiSSLMaster->CloseAmiSSL(); /* also cleans up and drops IAmiSSL */
	initialised = 0;
	IAmiSSL = NULL;
	AmiSSLBase = NULL;
	if (IAmiSSLMaster != NULL)
		IExec->DropInterface((struct Interface *)IAmiSSLMaster);
	IAmiSSLMaster = NULL;
	if (AmiSSLMasterBase != NULL)
		IExec->CloseLibrary(AmiSSLMasterBase);
	AmiSSLMasterBase = NULL;
	if (ISock != NULL)
		IExec->DropInterface(ISock);
	ISock = NULL;
	if (SockBase != NULL)
		IExec->CloseLibrary(SockBase);
	SockBase = NULL;
}
