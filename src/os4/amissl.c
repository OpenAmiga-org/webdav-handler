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

int amissl_open(void)
{
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
	/* curl only uses the opaque OpenSSL API, so newer AmiSSL versions
	 * may be used automatically */
	if (IAmiSSLMaster->OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
	                                  AmiSSL_UsesOpenSSLStructs, FALSE,
	                                  AmiSSL_InitAmiSSL, TRUE,
	                                  AmiSSL_GetIAmiSSL, &IAmiSSL,
	                                  AmiSSL_ISocket, ISock,
	                                  AmiSSL_ErrNoPtr, &errno,
	                                  TAG_DONE) != 0)
		return -2;
	initialised = 1;
	return 0;
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
