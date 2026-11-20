#!/usr/bin/env python3
"""Copy the History of the OS4Depot readme into README.md as its
"Changelog" section (between the changelog markers). Public domain.

Reference copy: ~/openamiga-rules/tools/update_readme.py (see
readme_rules.md). Copy it into a project's tools/ unchanged.

    tools/update_readme.py [--check] [package/<Name>.readme]

Without a readme argument it takes the only package/*.readme. The readme
may be UTF-8 or ISO-8859-1 (Markdown body). Its History looks like this:

    ## History

    ### 1.2 (16.11.2026)

    - First change, wrapped lines indented
      by two spaces.

--check only reports (exit 1) when README.md is out of date.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
END = "<!-- changelog end -->"


def read_text(path):
    raw = open(path, "rb").read()
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("latin-1")


def history(path):
    text = read_text(path)
    m = re.search(r"^## History\n(.*)", text, re.S | re.M)
    if not m:
        sys.exit(path + ": no \"## History\" section")
    versions = []
    for line in m.group(1).splitlines():
        v = re.match(r"### (\d+(?:\.\d+)+(?: beta)?) \((\d+)\.(\d+)\.(\d+)\)\s*$", line)
        if v:
            versions.append((v.group(1), "%s-%s-%s" % (v.group(4), v.group(3), v.group(2)), []))
        elif line.startswith("- ") and versions:
            versions[-1][2].append(line[2:].strip())
        elif line.startswith("  ") and line.strip() and versions and versions[-1][2]:
            versions[-1][2][-1] += " " + line.strip()
        elif line.startswith("#"):
            break                     # the next section
    if not versions:
        sys.exit(path + ": History has no \"x.y (DD.MM.YYYY)\" entries")
    out = []
    for ver, date, items in versions:
        if len(items) == 1:
            out.append("- **%s** (%s): %s" % (ver, date, items[0]))
        else:
            out.append("- **%s** (%s):" % (ver, date))
            out.extend("  - " + it for it in items)
    return "\n".join(out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if args:
        readme = os.path.abspath(args[0])
    else:
        found = glob.glob(os.path.join(ROOT, "package", "*.readme"))
        if len(found) != 1:
            sys.exit("give the readme: found %d package/*.readme" % len(found))
        readme = found[0]
    rel = os.path.relpath(readme, ROOT)
    start = "<!-- changelog start: made by tools/update_readme.py from %s -->" % rel
    path = os.path.join(ROOT, "README.md")
    text = open(path, encoding="utf-8").read()
    block = start + "\n" + history(readme) + "\n" + END
    m = re.search(r"<!-- changelog start:[^>]*-->.*?" + re.escape(END), text, re.S)
    if not m:
        sys.exit("README.md has no changelog markers (see readme_rules.md)")
    new = text[:m.start()] + block + text[m.end():]
    if new == text:
        return 0
    if "--check" in sys.argv:
        print("README.md changelog is out of date: run tools/update_readme.py", file=sys.stderr)
        return 1
    open(path, "w", encoding="utf-8").write(new)
    print("README.md changelog updated")
    return 0


sys.exit(main())
