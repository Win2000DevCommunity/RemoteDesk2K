
// relay_gui_main.c - Entry point for relay.exe (GUI relay server)
// (C) 2026, RemoteDesk2K Project
// Only include relay_gui.h, which includes common.h
#include "relay_gui.h"

#ifdef _MSC_VER
// Minimal stub for security cookie to allow linking with modern CRTs
__declspec(naked) void __security_check_cookie(void) { __asm { ret } }
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return RelayGui_Run(hInstance, nCmdShow);
}
