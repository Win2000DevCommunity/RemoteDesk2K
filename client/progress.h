/*
 * RemoteDesk2K - Progress Dialog Module
 * Windows 2000 compatible progress dialog for file transfers
 * 
 * Features:
 * - Shows file name being transferred
 * - Shows progress bar with percentage
 * - Shows transfer speed (KB/s or MB/s)
 * - Shows time remaining estimate
 * - Cancel button to abort transfer
 * - Supports 64-bit file sizes (up to 100GB)
 */

#ifndef PROGRESS_H
#define PROGRESS_H

#include <windows.h>

/* Progress dialog window class */
#define PROGRESS_WND_CLASS "RD2KProgressClass"

/* Progress dialog state - uses 64-bit sizes for large file support */
typedef struct _PROGRESS_STATE {
    HWND        hDlgWnd;            /* Dialog window handle */
    HWND        hProgressBar;       /* Progress bar control */
    HWND        hLabelFile;         /* Filename label */
    HWND        hLabelStatus;       /* Status label (X of Y bytes) */
    HWND        hLabelSpeed;        /* Speed label */
    HWND        hLabelTime;         /* Time remaining label */
    HWND        hCancelBtn;         /* Cancel button */
    BOOL        bCancelled;         /* User pressed cancel */
    BOOL        bVisible;           /* Dialog is visible */
    DWORD       dwStartTime;        /* Transfer start time (GetTickCount) */
    ULONGLONG   qwTotalBytes;       /* Total bytes to transfer (64-bit) */
    ULONGLONG   qwTransferred;      /* Bytes transferred so far (64-bit) */
    char        szFileName[260];    /* Current file name */
    char        szOperation[64];    /* "Sending" or "Receiving" */
} PROGRESS_STATE, *PPROGRESS_STATE;

/* Initialize progress dialog module (registers window class) */
BOOL Progress_Init(HINSTANCE hInstance);

/* Cleanup progress dialog module */
void Progress_Cleanup(void);

/* Show progress dialog for a file transfer (64-bit size) */
/* Returns TRUE if dialog created successfully */
BOOL Progress_Show64(HWND hParent, const char *fileName, ULONGLONG totalBytes, BOOL bSending);

/* Legacy 32-bit version for compatibility */
BOOL Progress_Show(HWND hParent, const char *fileName, DWORD totalBytes, BOOL bSending);

/* Update progress with 64-bit byte count */
void Progress_Update64(ULONGLONG bytesTransferred);

/* Legacy 32-bit version for compatibility */
void Progress_Update(DWORD bytesTransferred);

/* Hide and close progress dialog */
void Progress_Hide(void);

/* Check if user cancelled the transfer */
BOOL Progress_IsCancelled(void);

/* Process window messages (call from main message loop) */
void Progress_ProcessMessages(void);

#endif /* PROGRESS_H */
