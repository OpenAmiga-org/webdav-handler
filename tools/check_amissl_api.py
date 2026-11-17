#!/usr/bin/env python3
"""Checks that the AmigaOS 4 builds only call AmiSSL functions that every
AmiSSL 5 and 6 can serve. Public domain. (After the check of the same name
in sftp-handler.)

    python3 -I tools/check_amissl_api.py

src/os4/amissl.c asks amisslmaster for AmiSSL 6 (AMISSL_V4xx) or else for
AMISSL_V3xx (the AmiSSL 5.1 API). The stubs (tools/gen_amissl_glue.py) use
the AmiSSL 5 prototypes and the offsets of the interface that was opened.
That is safe only while each function used

  - is in the AmiSSL 5.1 interface at the same index as in the AmiSSL 5
    SDK (the interface only grows at the end), or is one of OLD5 below,
    which amissl.c replaces on older AmiSSL 5 versions;
  - is in AmiSSL 6 with the same prototype (const does not matter for the
    call), or is replaced for AmiSSL 6 (V4_REPLACED in gen_amissl_glue.py,
    src/os4/amissl_compat.c).

The used functions are the AmiSSL functions defined in build/os4/*.debug
(the glue stubs; a function libcurl needs but the list lacks makes the link
fail). The interfaces come from the SDKs' xml/amissl.xml. The AmiSSL 5.1 SDK
is downloaded to build/deps/amissl-5.1-sdk the first time.
Exit status 0 = all fine, 1 = a function is missing, moved or different.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_amissl_glue import V4_REPLACED  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, "build", "deps")
IMAGE = os.environ.get("OS4_IMAGE", "walkero/amigagccondocker:os4-gcc15-3.6.1")
LHA = os.environ.get("LHA", os.path.expanduser("~/linux_archivers/lha/lha"))
BASE_VER = "5.1"
BASE = os.path.join(DEPS, "amissl-%s-sdk" % BASE_VER)
SDK = os.path.join(DEPS, "amissl-sdk", "AmiSSL", "Developer")
SDK6 = os.path.join(DEPS, "amissl6-sdk", "AmiSSL", "Developer")
BINARIES = ("webdav-handler.debug", "wdvprefs.debug")
# not in AmiSSL 5.1; amissl.c puts a stand-in there for AmiSSL 5 before
# the version given (lib_Revision)
OLD5 = {"SSL_get0_group_name": 13}


def methods(path):
    with open(path, encoding="latin-1") as f:
        return re.findall(r'<method\s+name="([^"]+)"', f.read())


def prototypes(path):
    """name -> (result, argument types); attributes and const dropped"""
    with open(path, encoding="latin-1") as f:
        xml = f.read()
    norm = lambda t: " ".join(w for w in t.replace("*", " * ").split()
                              if not w.startswith("OSSL_") and w != "const")
    out = {}
    for m in re.finditer(r'<method\s+name="([^"]+)"\s+result="([^"]*)"[^>]*?(/>|>(.*?)</method>)', xml, re.S):
        args = re.findall(r'<arg\s+name="[^"]*"\s+type="([^"]*)"', m.group(4) or "")
        if "<vararg" in (m.group(4) or ""):
            args.append("...")
        out[m.group(1)] = (norm(m.group(2)), tuple(norm(a) for a in args))
    return out


def nm(args):
    """ppc-amigaos-nm in the build image, paths relative to the project"""
    out = subprocess.run(["docker", "run", "--rm", "-u", "%d:%d" % (os.getuid(), os.getgid()),
                          "-v", ROOT + ":/src", "-w", "/src", IMAGE, "ppc-amigaos-nm"] + args,
                         check=True, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return out.stdout


def main():
    base_xml = os.path.join(BASE, "AmiSSL", "Developer", "xml", "amissl.xml")
    if not os.path.exists(base_xml):
        os.makedirs(BASE, exist_ok=True)
        lha = "AmiSSL-%s-SDK.lha" % BASE_VER
        subprocess.run(["gh", "release", "download", BASE_VER, "-R", "jens-maus/amissl",
                        "-p", lha, "--clobber", "-D", BASE], check=True)
        subprocess.run([LHA, "xq", lha], cwd=BASE, check=True, stdout=subprocess.DEVNULL)
    base = methods(base_xml)
    cur = methods(os.path.join(SDK, "xml", "amissl.xml"))
    base_idx = {n: i for i, n in enumerate(base)}
    cur_idx = {n: i for i, n in enumerate(cur)}
    proto5 = prototypes(os.path.join(SDK, "xml", "amissl.xml"))
    proto6 = prototypes(os.path.join(SDK6, "xml", "amissl.xml"))
    with open(os.path.join(ROOT, "tools", "amissl_functions.txt")) as f:
        listed = {line.strip() for line in f if line.strip() and not line.startswith("#")}
    bad = 0
    for b in BINARIES:
        path = os.path.join("build", "os4", b)
        if not os.path.exists(os.path.join(ROOT, path)):
            sys.exit("%s is missing; run tools/build_os4.sh" % path)
        defined = {f[2] for f in (line.split() for line in nm([path]).splitlines())
                   if len(f) == 3 and f[1] == "T"}
        used = sorted(defined & set(proto5))
        for name in used:
            if name not in listed:
                print("%s: %s is not in tools/amissl_functions.txt" % (b, name))
                bad += 1
            if name in OLD5:
                pass
            elif name not in base_idx:
                print("%s: %s is not in AmiSSL %s (add a stand-in and OLD5)" % (b, name, BASE_VER))
                bad += 1
            elif base_idx[name] != cur_idx.get(name):
                print("%s: %s moved (%d in AmiSSL %s, %s now)" % (b, name, base_idx[name], BASE_VER,
                                                               cur_idx.get(name)))
                bad += 1
            if name in V4_REPLACED:
                if name in proto6:
                    print("%s: %s is in AmiSSL 6 after all; drop its stand-in" % (b, name))
                    bad += 1
            elif name not in proto6:
                print("%s: %s is not in AmiSSL 6 (add a stand-in)" % (b, name))
                bad += 1
            elif proto6[name] != proto5.get(name):
                print("%s: %s differs in AmiSSL 6: %s / %s" % (b, name, proto5.get(name), proto6[name]))
                bad += 1
        print("%s: %d AmiSSL functions, %s" % (b, len(used),
                                             "fine for AmiSSL %s+ and 6" % BASE_VER if not bad else "PROBLEMS"))
    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
