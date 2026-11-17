/* webdav-handler version; tools/build_os4.sh bumps WDV_REVISION. */
#ifndef WDV_VERSION_H
#define WDV_VERSION_H

#define WDV_VERSION  0
#define WDV_REVISION 41
#define WDV_DATE     "17.11.2026"

#define WDV_STR2(x) #x
#define WDV_STR(x)  WDV_STR2(x)
#define WDV_VERS    "webdav-handler " WDV_STR(WDV_VERSION) "." WDV_STR(WDV_REVISION)
#define WDV_VERSTAG "\0$VER: " WDV_VERS " (" WDV_DATE ")"

#endif
