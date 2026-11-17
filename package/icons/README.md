# Release icons

These `.info` files were saved by AmigaOS 4's icon.library (native format,
64x64 true colour picture, no planar picture). AmigaOS 4 does not read PNG
icons, so they cannot be written on the host.

The pictures are drawn by `tools/make_icons.py` (public domain, like the
rest of the project). To change an icon:

1. `python3 -I tools/make_icons.py OUT` writes `OUT/*.argb` and
   `OUT/mkicons.script`.
2. Build `tools/os4/mkicon.c` (ppc-amigaos-gcc -mcrt=clib4) and copy it,
   the `.argb` files and the script to a drawer in the OS4 guest.
3. Run `Execute mkicons.script` in that drawer; the icons appear in `out/`.
4. Copy them back here. `Devs_DOSDrivers_NCH.info` becomes
   `Devs/DOSDrivers/NCH.info` and `Devs_DOSDrivers.info` becomes
   `Devs/DOSDrivers.info`, `Prefs_nchprefs.info` becomes `Prefs/nchprefs.info`
   in the archive (tools/make_lha.sh does this).
