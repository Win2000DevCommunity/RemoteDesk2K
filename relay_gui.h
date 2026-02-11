// relay_gui.h - GUI for relay server (Windows 2000 compatible)
// (C) 2026, RemoteDesk2K Project
#ifndef RELAY_GUI_H
#define RELAY_GUI_H

// Always include the central project header first
#include "common.h"

// All GUI-specific prototypes go here
int RelayGui_Run(HINSTANCE hInstance, int nCmdShow);

// Centralized GUI control IDs
#define IDC_TAB        1001
#define IDC_PARAM_IP   1002
#define IDC_PARAM_PORT 1003
#define IDC_PARAM_START 1004
#define IDC_CONSOLE    1005

#endif // RELAY_GUI_H
