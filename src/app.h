#ifndef NUWORLD_APP_H
#define NUWORLD_APP_H

#define NW_APP_NAME "CrashWOC: NuWorld"
/* The name the UI and the window show: the Xbox build is a beta of its own. */
#ifdef NW_UWP
#define NW_APP_TITLE NW_APP_NAME " - Xbox Port Beta"
#else
#define NW_APP_TITLE NW_APP_NAME
#endif
/* The Discord application ID*/
#define NW_DISCORD_APP_ID "1568198671697182720"

#endif
