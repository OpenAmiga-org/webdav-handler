# Release icons

These `.info` files were saved by AmigaOS 4's icon.library (native format,
64x64 true colour picture, no planar picture). AmigaOS 4 does not read PNG
icons, so they cannot be written on the host.

The pictures are OS 4.1 style PNGs made in `~/os4icons` (`compose.py`,
output in `~/os4icons/out/webdav`): `NAME.png` and `NAME.sel.png` for every
icon, the selected picture finished already (each channel -84 and a gold
glow, as the stock OS 4.1 icons). To make the icons:

1. `python3 -I tools/make_icons.py OUT [PICTURES]` writes `OUT/*.argb`
   and `OUT/mkicons.script` (types, default tools, tool types, stacks and
   drawer windows are set in `tools/make_icons.py`).
2. Build `tools/os4/mkicon.c` (ppc-amigaos-gcc -mcrt=clib4) and copy it,
   the `.argb` files and the script to a drawer in the OS4 guest. Put
   clib4 2.5 first in LIBS: before mkicon runs the first time, or the
   guest's clib4 2.0 gets loaded.
3. Run `Execute mkicons.script` in that drawer; the icons appear in `out/`.
4. Copy them back here. tools/make_lha.sh puts them in place:
   `Devs_DOSDrivers_WDV.info` becomes `Devs/DOSDrivers/WDV.info`,
   `Devs_DOSDrivers.info` becomes `Devs/DOSDrivers.info`,
   `Prefs_wdvprefs.info` becomes `Prefs/wdvprefs.info`, and the volume icon
   `def_WDV.info` (TYPE=DISK) goes to `Prefs/Env-Archive/Sys/`, from where
   the Install script copies it to ENVARC:Sys and ENV:Sys. Workbench shows
   the WDV: volume with ENV:Sys/def_WDV.info, as it does with def_DF0 or
   def_RAD.
