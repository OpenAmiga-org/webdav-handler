#!/usr/bin/env python3
"""Turns the pictures of the release icons into the raw pictures that
tools/os4/mkicon reads. Public domain.

    python3 -I tools/make_icons.py OUTDIR [PICTURES]

PICTURES (default ~/os4icons/out/webdav) holds NAME.png and
NAME.sel.png for every icon below (64x64 RGBA, normal and selected; the
selected ones are finished already, OS 4.1 style). Writes
OUTDIR/<icon>.argb and .sel.argb and OUTDIR/mkicons.script, an AmigaDOS
script that runs tools/os4/mkicon in the OS4 guest; icon.library then saves
the icons in its own format (AmigaOS 4 does not read PNG icons). The results
are kept in package/icons.
"""
import os
import sys

from PIL import Image

# icon types (workbench/workbench.h)
WBDISK, WBDRAWER, WBTOOL, WBPROJECT = 1, 2, 3, 4

SIZE = 64
PICTURES = os.path.expanduser("~/os4icons/out/webdav")


ICONS = {
    # file name (without .info): (type, default tool, tool types, stack, drawer)
    "WebDAVHandler": (WBDRAWER, None, (), None, (80, 60, 640, 180)),
    "Install": (WBPROJECT, "SYS:Utilities/Installer",
                ("APPNAME=WebDAV Handler", "MINUSER=AVERAGE", "DEFUSER=AVERAGE"), 32768, None),
    "WebDAVHandler.guide": (WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "LICENSE.txt": (WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "Third-party.txt": (WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "Prefs/wdvprefs": (WBTOOL, None, (), 131072, None),
    "Devs/DOSDrivers/WDV": (WBPROJECT, "C:Mount", (), 16384, None),
    "Prefs": (WBDRAWER, None, (), None, (120, 100, 300, 160)),
    "Devs": (WBDRAWER, None, (), None, (100, 80, 300, 160)),
    "Devs/DOSDrivers": (WBDRAWER, None, (), None, (120, 100, 300, 160)),
    "L": (WBDRAWER, None, (), None, (120, 100, 300, 160)),
    # the volume icon of WDV: (Workbench reads ENV:Sys/def_WDV.info)
    "def_WDV": (WBDISK, None, (), None, None),
}


def argb(img):
    return b"".join(bytes((a, r, g, b)) for (r, g, b, a) in img.get_flattened_data())


def picture(src, name):
    img = Image.open(os.path.join(src, name)).convert("RGBA")
    if img.size != (SIZE, SIZE):
        sys.exit("%s: %dx%d, not %dx%d" % (name, img.size[0], img.size[1], SIZE, SIZE))
    return img


def main():
    out = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else PICTURES
    os.makedirs(out, exist_ok=True)
    lines = ["; created by tools/make_icons.py; run in the drawer with the .argb files",
             "FailAt 21"]
    for name, (typ, tool, tts, stack, drawer) in ICONS.items():
        flat = name.replace("/", "_")
        for suffix in ("", ".sel"):
            with open(os.path.join(out, flat + suffix + ".argb"), "wb") as f:
                f.write(argb(picture(src, flat + suffix + ".png")))
        t = {WBDISK: "DISK", WBDRAWER: "DRAWER", WBTOOL: "TOOL", WBPROJECT: "PROJECT"}[typ]
        args = ['"out/%s"' % flat, "ARGB=%s.argb" % flat, "SELECTED=%s.sel.argb" % flat,
                "TYPE=" + t]
        if tool:
            args.append('"TOOL=%s"' % tool)
        if stack:
            args.append("STACK=%d" % stack)
        if drawer:
            args.append("WINDOW=%d,%d,%d,%d" % drawer)
        for tt in tts:
            args.append('"TT=%s"' % tt)
        lines.append("mkicon " + " ".join(args))
    with open(os.path.join(out, "mkicons.script"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
