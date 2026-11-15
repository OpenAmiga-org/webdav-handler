#!/usr/bin/env python3
"""Draws the pictures of the release icons. The pictures are drawn here, so
the icons are as free as the rest of the project. Public domain.

    python3 -I tools/make_icons.py OUTDIR

writes OUTDIR/<icon>.argb and .sel.argb (64x64 ARGB, normal and selected)
and OUTDIR/mkicons.script, an AmigaDOS script that runs tools/os4/mkicon in
the OS4 guest; icon.library then saves the icons in its own format
(AmigaOS 4 does not read PNG icons). The results are kept in package/icons.
"""
import sys

from PIL import Image, ImageDraw

# icon types (workbench/workbench.h)
WBDRAWER, WBTOOL, WBPROJECT = 2, 3, 4

SIZE = 64
SS = 4  # supersampling for smooth edges


def canvas():
    im = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def s(*v):
    return [x * SS for x in v]


def cloud(d, cx, cy, scale, fill, outline):
    """A simple cloud: three circles on a rounded base."""
    def circle(x, y, r):
        d.ellipse(s(cx + x * scale - r * scale, cy + y * scale - r * scale,
                    cx + x * scale + r * scale, cy + y * scale + r * scale),
                  fill=fill)
    for pass_fill, grow in ((outline, 1.2), (fill, 0)):
        f = pass_fill
        g = grow / scale if scale else 0
        d.rounded_rectangle(s(cx - 15 * scale - g, cy - 2 * scale - g,
                              cx + 15 * scale + g, cy + 8 * scale + g),
                            radius=5 * scale * SS, fill=f)
        for (x, y, r) in ((-8, 0, 7), (0, -5, 9), (9, 0, 6.5)):
            d.ellipse(s(cx + x * scale - (r + g) * scale, cy + y * scale - (r + g) * scale,
                        cx + x * scale + (r + g) * scale, cy + y * scale + (r + g) * scale),
                      fill=f)


def finish(im):
    return im.resize((SIZE, SIZE), Image.LANCZOS)


BLUE = (0, 130, 201, 255)       # a calm blue
DARK = (20, 40, 70, 255)
WHITE = (250, 250, 252, 255)
PAPER = (238, 238, 232, 255)
LINE = (120, 120, 130, 255)


def img_drawer():
    im, d = canvas()
    d.rounded_rectangle(s(4, 14, 60, 56), radius=6 * SS, fill=(214, 168, 70, 255),
                        outline=DARK, width=2 * SS)
    d.rounded_rectangle(s(4, 10, 28, 20), radius=4 * SS, fill=(214, 168, 70, 255),
                        outline=DARK, width=2 * SS)
    d.rounded_rectangle(s(4, 20, 60, 56), radius=6 * SS, fill=(236, 196, 96, 255),
                        outline=DARK, width=2 * SS)
    cloud(d, 32, 37, 1.0, WHITE, BLUE)
    return finish(im)


def img_dosdriver():
    im, d = canvas()
    d.rounded_rectangle(s(6, 8, 58, 56), radius=8 * SS, fill=BLUE, outline=DARK, width=2 * SS)
    d.rectangle(s(14, 44, 50, 50), fill=(0, 90, 150, 255))
    cloud(d, 32, 26, 1.0, WHITE, DARK)
    return finish(im)


def page(d):
    d.polygon(s(14, 4, 42, 4, 52, 14, 52, 60, 14, 60), fill=PAPER, outline=DARK)
    d.line(s(14, 4, 42, 4, 52, 14, 52, 60, 14, 60, 14, 4), fill=DARK, width=2 * SS)
    d.polygon(s(42, 4, 42, 14, 52, 14), fill=(200, 200, 192, 255), outline=DARK)


def img_guide():
    im, d = canvas()
    page(d)
    cloud(d, 33, 22, 0.6, BLUE, DARK)
    for y in (36, 42, 48, 54):
        d.line(s(20, y, 46, y), fill=LINE, width=2 * SS)
    return finish(im)


def img_text():
    im, d = canvas()
    page(d)
    for y in (16, 22, 28, 34, 40, 46, 52):
        d.line(s(20, y, 46 if y != 16 else 38, y), fill=LINE, width=2 * SS)
    return finish(im)


def img_install():
    im, d = canvas()
    d.polygon(s(8, 24, 32, 14, 56, 24, 32, 34), fill=(236, 196, 96, 255), outline=DARK)
    d.polygon(s(8, 24, 32, 34, 32, 60, 8, 50), fill=(214, 168, 70, 255), outline=DARK)
    d.polygon(s(56, 24, 32, 34, 32, 60, 56, 50), fill=(190, 140, 50, 255), outline=DARK)
    d.polygon(s(26, 2, 38, 2, 38, 14, 44, 14, 32, 26, 20, 14, 26, 14), fill=BLUE, outline=DARK)
    return finish(im)


ICONS = {
    # file name (without .info): (image, type, default tool, tool types, stack, drawer)
    "NextcloudHandler": (img_drawer, WBDRAWER, None, (), None, (80, 60, 640, 180)),
    "Install": (img_install, WBPROJECT, "SYS:Utilities/Installer",
                ("APPNAME=Nextcloud Handler", "MINUSER=AVERAGE", "DEFUSER=AVERAGE"), 32768, None),
    "NextcloudHandler.guide": (img_guide, WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "LICENSE.txt": (img_text, WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "Third-party.txt": (img_text, WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "Devs/DOSDrivers/NC0": (img_dosdriver, WBPROJECT, "C:Mount", (), 16384, None),
    "Devs": (img_drawer, WBDRAWER, None, (), None, (100, 80, 300, 160)),
    "Devs/DOSDrivers": (img_drawer, WBDRAWER, None, (), None, (120, 100, 300, 160)),
    "L": (img_drawer, WBDRAWER, None, (), None, (120, 100, 300, 160)),
}


def argb(img):
    return b"".join(bytes((a, r, g, b)) for (r, g, b, a) in img.get_flattened_data())


def darker(img):
    px = [(int(r * 0.7), int(g * 0.7), int(b * 0.75), a) for (r, g, b, a) in img.get_flattened_data()]
    out = Image.new("RGBA", img.size)
    out.putdata(px)
    return out


def main():
    out = sys.argv[1]
    import os
    os.makedirs(out, exist_ok=True)
    lines = ["; created by tools/make_icons.py; run in the drawer with the .argb files",
             "FailAt 21"]
    for name, (fn, typ, tool, tts, stack, drawer) in ICONS.items():
        flat = name.replace("/", "_")
        img = fn()
        with open(os.path.join(out, flat + ".argb"), "wb") as f:
            f.write(argb(img))
        with open(os.path.join(out, flat + ".sel.argb"), "wb") as f:
            f.write(argb(darker(img)))
        t = {WBDRAWER: "DRAWER", WBTOOL: "TOOL", WBPROJECT: "PROJECT"}[typ]
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
    # also a preview sheet for checking the pictures
    sheet = Image.new("RGBA", (SIZE * 6 + 70, SIZE + 20), (200, 200, 200, 255))
    for i, fn in enumerate((img_drawer, img_dosdriver, img_guide, img_text, img_install)):
        sheet.alpha_composite(fn(), (10 + i * (SIZE + 12), 10))
    sheet.save(os.path.join(out, "icons-preview.png"))


if __name__ == "__main__":
    main()
