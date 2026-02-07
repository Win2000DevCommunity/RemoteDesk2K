/*
 * RemoteDesk2K - Progress Dialog Module Implementation
 * Windows 2000 compatible progress dialog for file transfers
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "progress.h"

/* Control IDs */
#define IDC_PROGRESS_BAR    1001
#define IDC_LABEL_FILE      1002
#define IDC_LABEL_STATUS    1003
#define IDC_LABEL_SPEED     1004
#define IDC_LABEL_TIME      1005
#define IDC_BTN_CANCEL      1006

/* Dialog dimensions */
#define PROG_DLG_WIDTH      400
#define PROG_DLG_HEIGHT     160

/* Global progress state */
static PROGRESS_STATE g_progState = {0};
static HINSTANCE g_hInstance = NULL;
static HFONT g_hFont = NULL;

/* Forward declaration */
static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Format bytes to human readable string - 64-bit version */
static void FormatBytes64(ULONGLONG bytes, char *buffer, int bufSize)
{
    DWORD whole, frac;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        /* GB range */
        whole = (DWORD)(bytes / (1024ULL * 1024ULL * 1024ULL));
        frac = (DWORD)(((bytes % (1024ULL * 1024ULL * 1024ULL)) * 10ULL) / (1024ULL * 1024ULL * 1024ULL));
        sprintf(buffer, "%lu.%lu GB", whole, frac);
    } else if (bytes >= 1024ULL * 1024ULL) {
        whole = (DWORD)(bytes / (1024ULL * 1024ULL));
        frac = (DWORD)(((bytes % (1024ULL * 1024ULL)) * 10ULL) / (1024ULL * 1024ULL));
        sprintf(buffer, "%lu.%lu MB", whole, frac);
    } else if (bytes >= 1024ULL) {
        whole = (DWORD)(bytes / 1024ULL);
        frac = (DWORD)(((bytes % 1024ULL) * 10ULL) / 1024ULL);
        sprintf(buffer, "%lu.%lu KB", whole, frac);
    } else {
        sprintf(buffer, "%lu bytes", (DWORD)bytes);
    }
}

/* Format bytes to human readable string - uses integer math only */
static void FormatBytes(DWORD bytes, char *buffer, int bufSize)
{
    FormatBytes64((ULONGLONG)bytes, buffer, bufSize);
}

/* Format time to human readable string */
static void FormatTime(DWORD seconds, char *buffer, int bufSize)
{
    if (seconds >= 3600) {
        sprintf(buffer, "%lu:%02lu:%02lu", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
    } else if (seconds >= 60) {
        sprintf(buffer, "%lu:%02lu", seconds / 60, seconds % 60);
    } else {
        sprintf(buffer, "%lu sec", seconds);
    }
}

/* Initialize progress dialog module */
BOOL Progress_Init(HINSTANCE hInstance)
{
    WNDCLASSEXA wc;
    
    g_hInstance = hInstance;
    
    /* Create font */
    g_hFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");
    
    /* Register window class */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ProgressWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = PROGRESS_WND_CLASS;
    
    if (!RegisterClassExA(&wc)) {
        return FALSE;
    }
    
    ZeroMemory(&g_progState, sizeof(g_progState));
    return TRUE;
}

/* Cleanup progress dialog module */
void Progress_Cleanup(void)
{
    Progress_Hide();
    
    if (g_hFont) {
        DeleteObject(g_hFont);
        g_hFont = NULL;
    }
    
    UnregisterClassA(PROGRESS_WND_CLASS, g_hInstance);
}

/* Show progress dialog */
BOOL Progress_Show(HWND hParent, const char *fileName, DWORD totalBytes, BOOL bSending)
{
    return Progress_Show64(hParent, fileName, (ULONGLONG)totalBytes, bSending);
}

/* Show progress dialog - 64-bit version for large files */
BOOL Progress_Show64(HWND hParent, const char *fileName, ULONGLONG totalBytes, BOOL bSending)
{
    int screenWidth, screenHeight;
    int x, y;
    
    /* Hide any existing dialog */
    Progress_Hide();
    
    /* Initialize state */
    ZeroMemory(&g_progState, sizeof(g_progState));
    g_progState.qwTotalBytes = totalBytes;
    g_progState.dwStartTime = GetTickCount();
    lstrcpynA(g_progState.szFileName, fileName, sizeof(g_progState.szFileName));
    lstrcpyA(g_progState.szOperation, bSending ? "Sending" : "Receiving");
    
    /* Center dialog on screen */
    screenWidth = GetSystemMetrics(SM_CXSCREEN);
    screenHeight = GetSystemMetrics(SM_CYSCREEN);
    x = (screenWidth - PROG_DLG_WIDTH) / 2;
    y = (screenHeight - PROG_DLG_HEIGHT) / 2;
    
    /* Create dialog window */
    g_progState.hDlgWnd = CreateWindowExA(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        PROGRESS_WND_CLASS,
        bSending ? "Sending File..." : "Receiving File...",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, PROG_DLG_WIDTH, PROG_DLG_HEIGHT,
        hParent, NULL, g_hInstance, NULL);
    
    if (!g_progState.hDlgWnd) {
        return FALSE;
    }
    
    /* Create file name label */
    g_progState.hLabelFile = CreateWindowExA(0, "STATIC", fileName,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
        10, 10, PROG_DLG_WIDTH - 20, 20,
        g_progState.hDlgWnd, (HMENU)IDC_LABEL_FILE, g_hInstance, NULL);
    SendMessage(g_progState.hLabelFile, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    /* Create progress bar */
    g_progState.hProgressBar = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        10, 35, PROG_DLG_WIDTH - 20, 20,
        g_progState.hDlgWnd, (HMENU)IDC_PROGRESS_BAR, g_hInstance, NULL);
    SendMessage(g_progState.hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    
    /* Create status label */
    g_progState.hLabelStatus = CreateWindowExA(0, "STATIC", "0 / 0 bytes (0%)",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 60, PROG_DLG_WIDTH - 20, 18,
        g_progState.hDlgWnd, (HMENU)IDC_LABEL_STATUS, g_hInstance, NULL);
    SendMessage(g_progState.hLabelStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    /* Create speed label */
    g_progState.hLabelSpeed = CreateWindowExA(0, "STATIC", "Speed: -- KB/s",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 78, 180, 18,
        g_progState.hDlgWnd, (HMENU)IDC_LABEL_SPEED, g_hInstance, NULL);
    SendMessage(g_progState.hLabelSpeed, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    /* Create time remaining label */
    g_progState.hLabelTime = CreateWindowExA(0, "STATIC", "Time remaining: --",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        200, 78, 180, 18,
        g_progState.hDlgWnd, (HMENU)IDC_LABEL_TIME, g_hInstance, NULL);
    SendMessage(g_progState.hLabelTime, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    /* Create cancel button */
    g_progState.hCancelBtn = CreateWindowExA(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        (PROG_DLG_WIDTH - 80) / 2, 100, 80, 25,
        g_progState.hDlgWnd, (HMENU)IDC_BTN_CANCEL, g_hInstance, NULL);
    SendMessage(g_progState.hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    /* Show dialog */
    ShowWindow(g_progState.hDlgWnd, SW_SHOW);
    UpdateWindow(g_progState.hDlgWnd);
    g_progState.bVisible = TRUE;
    
    return TRUE;
}

/* Update progress - legacy 32-bit version */
void Progress_Update(DWORD bytesTransferred)
{
    Progress_Update64((ULONGLONG)bytesTransferred);
}

/* Update progress - 64-bit version for large files */
void Progress_Update64(ULONGLONG bytesTransferred)
{
    char status[256], speed[64], timeStr[64];
    char bytesTransStr[32], totalStr[32];
    DWORD elapsed, remaining;
    DWORD speedKBWhole, speedKBFrac;  /* Integer speed in KB/s */
    ULONGLONG bytesPerSec;
    int percent;
    
    if (!g_progState.hDlgWnd || !g_progState.bVisible) return;
    
    g_progState.qwTransferred = bytesTransferred;
    
    /* Calculate percentage - avoid overflow with large files */
    if (g_progState.qwTotalBytes > 0) {
        /* For very large files, divide first to avoid overflow */
        if (g_progState.qwTotalBytes > 0xFFFFFFFF) {
            percent = (int)((bytesTransferred / (g_progState.qwTotalBytes / 100)));
        } else {
            percent = (int)((bytesTransferred * 100) / g_progState.qwTotalBytes);
        }
        if (percent > 100) percent = 100;
    } else {
        percent = 0;
    }
    
    /* Update progress bar */
    SendMessage(g_progState.hProgressBar, PBM_SETPOS, percent, 0);
    
    /* Calculate speed and time remaining using integer math */
    elapsed = GetTickCount() - g_progState.dwStartTime;
    if (elapsed > 100) {  /* Need at least 100ms for reasonable calculation */
        /* bytes per second = (bytes * 1000) / elapsed_ms */
        bytesPerSec = (bytesTransferred * 1000ULL) / elapsed;
        
        /* Convert to KB/s with one decimal place */
        speedKBWhole = (DWORD)(bytesPerSec / 1024ULL);
        speedKBFrac = (DWORD)(((bytesPerSec % 1024ULL) * 10ULL) / 1024ULL);
        
        /* Use MB/s for fast transfers */
        if (speedKBWhole >= 1024) {
            DWORD speedMBWhole = speedKBWhole / 1024;
            DWORD speedMBFrac = ((speedKBWhole % 1024) * 10) / 1024;
            sprintf(speed, "Speed: %lu.%lu MB/s", speedMBWhole, speedMBFrac);
        } else {
            sprintf(speed, "Speed: %lu.%lu KB/s", speedKBWhole, speedKBFrac);
        }
        
        if (bytesPerSec > 0 && bytesTransferred < g_progState.qwTotalBytes) {
            /* remaining seconds = remaining_bytes / bytes_per_sec */
            remaining = (DWORD)((g_progState.qwTotalBytes - bytesTransferred) / bytesPerSec);
            FormatTime(remaining, timeStr, sizeof(timeStr));
        } else {
            lstrcpyA(timeStr, "--");
        }
    } else {
        lstrcpyA(speed, "Speed: -- KB/s");
        lstrcpyA(timeStr, "--");
    }
    
    /* Format status text */
    FormatBytes64(bytesTransferred, bytesTransStr, sizeof(bytesTransStr));
    FormatBytes64(g_progState.qwTotalBytes, totalStr, sizeof(totalStr));
    sprintf(status, "%s / %s (%d%%)", bytesTransStr, totalStr, percent);
    
    /* Update labels */
    SetWindowTextA(g_progState.hLabelStatus, status);
    SetWindowTextA(g_progState.hLabelSpeed, speed);
    
    sprintf(status, "Time remaining: %s", timeStr);
    SetWindowTextA(g_progState.hLabelTime, status);
    
    /* Process messages to keep UI responsive */
    Progress_ProcessMessages();
}

/* Hide progress dialog */
void Progress_Hide(void)
{
    if (g_progState.hDlgWnd) {
        DestroyWindow(g_progState.hDlgWnd);
        g_progState.hDlgWnd = NULL;
    }
    g_progState.bVisible = FALSE;
}

/* Check if cancelled */
BOOL Progress_IsCancelled(void)
{
    return g_progState.bCancelled;
}

/* Process window messages */
void Progress_ProcessMessages(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

/* Progress dialog window procedure */
static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_CANCEL) {
                g_progState.bCancelled = TRUE;
                EnableWindow(g_progState.hCancelBtn, FALSE);
                SetWindowTextA(g_progState.hCancelBtn, "Cancelling...");
            }
            return 0;
        
        case WM_CLOSE:
            g_progState.bCancelled = TRUE;
            return 0;
        
        case WM_DESTROY:
            g_progState.hDlgWnd = NULL;
            g_progState.bVisible = FALSE;
            return 0;
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
