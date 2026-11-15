/* icontest - prints what icon.library reads from an icon. Test tool, not
 * part of the release. Usage: icontest NAME (without .info). Public domain. */
#include <proto/exec.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <stdio.h>

struct IconIFace *IIcon;

int main(int argc, char **argv)
{
	struct Library *IconBase;
	struct DiskObject *dobj;
	int i;

	if (argc < 2)
		return 5;
	IconBase = IExec->OpenLibrary("icon.library", 50);
	if (IconBase == NULL)
		return 20;
	IIcon = (struct IconIFace *)IExec->GetInterface(IconBase, "main", 1, NULL);
	dobj = IIcon->GetDiskObject(argv[1]);
	if (dobj == NULL) {
		printf("cannot read %s.info\n", argv[1]);
		return 10;
	}
	printf("type %d, size %dx%d, stack %ld\n", dobj->do_Type, dobj->do_Gadget.Width,
	       dobj->do_Gadget.Height, (long)dobj->do_StackSize);
	{
		STRPTR fmt = NULL;
		uint32 idf = 99;
		IIcon->IconControl(dobj, ICONCTRLA_GetIconFileFormat, &fmt,
		                   ICONCTRLA_GetImageDataFormat, &idf, TAG_END);
		printf("file format: %s, image data format %lu\n", fmt ? (char *)fmt : "(none)", (unsigned long)idf);
	}
	printf("default tool: %s\n", dobj->do_DefaultTool ? (char *)dobj->do_DefaultTool : "(none)");
	for (i = 0; dobj->do_ToolTypes && dobj->do_ToolTypes[i]; i++)
		printf("tooltype: %s\n", (char *)dobj->do_ToolTypes[i]);
	if (dobj->do_DrawerData)
		printf("drawer window %d,%d %dx%d\n", dobj->do_DrawerData->dd_NewWindow.LeftEdge,
		       dobj->do_DrawerData->dd_NewWindow.TopEdge,
		       dobj->do_DrawerData->dd_NewWindow.Width,
		       dobj->do_DrawerData->dd_NewWindow.Height);
	IIcon->FreeDiskObject(dobj);
	IExec->DropInterface((struct Interface *)IIcon);
	IExec->CloseLibrary(IconBase);
	return 0;
}
