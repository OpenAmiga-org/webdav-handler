/* nextcloud-handler version; tools/build_os4.sh bumps NCH_REVISION. */
#ifndef NCH_VERSION_H
#define NCH_VERSION_H

#define NCH_VERSION  0
#define NCH_REVISION 36
#define NCH_DATE     "15.11.2026"

#define NCH_STR2(x) #x
#define NCH_STR(x)  NCH_STR2(x)
#define NCH_VERS    "nextcloud-handler " NCH_STR(NCH_VERSION) "." NCH_STR(NCH_REVISION)
#define NCH_VERSTAG "\0$VER: " NCH_VERS " (" NCH_DATE ")"

#endif
