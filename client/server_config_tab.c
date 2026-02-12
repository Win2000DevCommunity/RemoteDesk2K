/* server_config_tab.c - Windows 2000 DDK/SDK compatible server config tab
 * NOTE: Server config controls are now created directly in remotedesk2k.c
 * This file is kept for compatibility with build.bat
 */
#include "server_config_tab.h"
#include <windows.h>

/* This function is now a no-op - controls are created in CreateMainControls() */
void CreateServerConfigTab(HWND hwnd, HINSTANCE hInstance, HFONT hFontNormal, HFONT hFontBold) {
    /* Suppress unused parameter warnings */
    (void)hwnd;
    (void)hInstance;
    (void)hFontNormal;
    (void)hFontBold;
}
