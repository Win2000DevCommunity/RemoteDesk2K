/*
 * RemoteDesk2K - File Transfer Module v3
 * Windows 2000 compatible file transfer with progress dialog
 * 
 * ARCHITECTURE:
 * =============
 * 
 * Uses a WORKER THREAD for file transfer to prevent UI freezing.
 * The main thread shows progress dialog while worker thread does the transfer.
 * Communication between threads via PostMessage.
 * 
 * The file transfer works in these scenarios:
 * 
 * 1. EXPLICIT TRANSFER (Menu/Drag-Drop/Toolbar):
 *    - User selects "Send File" or drags files onto viewer
 *    - Worker thread transfers file asynchronously
 *    - Progress dialog shows transfer status (non-blocking)
 *    - Files saved to remote's Desktop or active Explorer folder
 * 
 * 2. CLIPBOARD PASTE TO REMOTE (Ctrl+V in viewer with files):
 *    - Local files are transferred to remote via worker thread
 *    - Files saved to remote's active folder at TIME OF PASTE
 * 
 * 3. TEXT CLIPBOARD:
 *    - Text is synced bidirectionally as before (synchronous, small data)
 */

#ifndef FILETRANSFER_H
#define FILETRANSFER_H

#include <windows.h>
#include "network.h"

/* Maximum file size (100GB) - using 64-bit arithmetic */
#define FT_MAX_FILE_SIZE        (100ULL * 1024ULL * 1024ULL * 1024ULL)

/* Chunk size for transfer (32KB) */
#define FT_CHUNK_SIZE           (32 * 1024)

/* ACK interval - send ACK after this many chunks for flow control */
#define FT_ACK_INTERVAL         8

/* File transfer states */
#define FT_STATE_IDLE           0
#define FT_STATE_SENDING        1
#define FT_STATE_RECEIVING      2
#define FT_STATE_SENDING_FOLDER 3
#define FT_STATE_RECEIVING_FOLDER 4

/* File transfer results */
#define FT_SUCCESS              0
#define FT_ERR_FILE_NOT_FOUND   1
#define FT_ERR_FILE_TOO_LARGE   2
#define FT_ERR_MEMORY           3
#define FT_ERR_READ             4
#define FT_ERR_WRITE            5
#define FT_ERR_NETWORK          6
#define FT_ERR_BUSY             7
#define FT_ERR_CREATE_FILE      8
#define FT_ERR_CANCELLED        9

/* Window messages for progress updates (WM_USER + offset) */
#define WM_FT_PROGRESS          (WM_USER + 100)
#define WM_FT_COMPLETE          (WM_USER + 101)
#define WM_FT_ERROR             (WM_USER + 102)

/* File transfer context - supports 64-bit file sizes for up to 100GB */
typedef struct _FT_CONTEXT {
    int         state;
    HANDLE      hFile;
    HANDLE      hThread;            /* Worker thread handle */
    DWORD       dwThreadId;         /* Worker thread ID */
    volatile BOOL bCancelRequested; /* Cancel flag (thread-safe) */
    char        fileName[260];
    char        filePath[MAX_PATH];
    char        destFolder[MAX_PATH];
    ULONGLONG   fileSize;           /* 64-bit for files up to 100GB */
    ULONGLONG   bytesTransferred;   /* 64-bit for files up to 100GB */
    DWORD       chunkIndex;
    DWORD       totalChunks;        /* Pre-calculated total chunk count */
    HWND        hNotifyWnd;
    HINSTANCE   hInstance;
    BOOL        bShowProgress;
    PRD2K_NETWORK pNet;             /* Network connection for worker thread */
} FT_CONTEXT;

/* Initialize file transfer module */
BOOL FileTransfer_Init(HWND hNotifyWnd, HINSTANCE hInstance);

/* Cleanup file transfer module */
void FileTransfer_Cleanup(void);

/* Send a file to remote (ASYNC - returns immediately, uses worker thread) */
/* If bShowProgress is TRUE, shows progress dialog */
int FileTransfer_SendFile(PRD2K_NETWORK pNet, const char *filePath, BOOL bShowProgress);

/* Start receiving a file (called when MSG_FILE_START received) */
/* destFolder = folder where file will be saved (use NULL for auto-detect) */
int FileTransfer_StartReceive(const BYTE *data, DWORD length, const char *destFolder);

/* Receive file data chunk (called when MSG_FILE_DATA received) */
int FileTransfer_ReceiveData(const BYTE *data, DWORD length);

/* Complete file receive (called when MSG_FILE_END received) */
int FileTransfer_EndReceive(void);

/* Cancel current transfer */
void FileTransfer_Cancel(void);

/* Check if transfer is in progress */
BOOL FileTransfer_IsBusy(void);

/* Get current transfer progress (0-100) */
int FileTransfer_GetProgress(void);

/* Get last error message */
const char* FileTransfer_GetLastError(void);

/* Get active Explorer folder path for paste destination - ALWAYS queries current state */
/* This is the preferred method when receiving files - call it at receive time */
BOOL FileTransfer_GetActiveFolder(char *pathOut, int pathSize);

/* Capture and remember the currently active Explorer folder */
/* Call this when the viewer gets activated. Optionally pass the viewer HWND to exclude it. */
void FileTransfer_CaptureActiveFolder(HWND hwndExclude);

/* Get the remembered active folder (set by CaptureActiveFolder) */
/* Returns Desktop if no folder was captured */
BOOL FileTransfer_GetRememberedFolder(char *pathOut, int pathSize);

/* Send a folder and all its contents to remote (ASYNC) */
int FileTransfer_SendFolder(PRD2K_NETWORK pNet, const char *folderPath, BOOL bShowProgress);

/* Check if path is a directory */
BOOL FileTransfer_IsDirectory(const char *path);

/* Send ACK for flow control */
int FileTransfer_SendAck(PRD2K_NETWORK pNet, DWORD chunkIndex, DWORD status);

/* Wait for ACK from receiver */
int FileTransfer_WaitForAck(PRD2K_NETWORK pNet, DWORD expectedChunk, DWORD timeoutMs);

/* Send files from clipboard to remote (ASYNC - uses worker thread, for client/viewer side) */
int FileTransfer_SendClipboardFiles(PRD2K_NETWORK pNet, HWND hClipOwner, BOOL bShowProgress);

/* Send files from clipboard to remote (SYNC - blocks until complete, for server side) */
/* Use this when sending from server back to client to avoid socket threading issues */
int FileTransfer_SendClipboardFilesSync(PRD2K_NETWORK pNet, HWND hClipOwner, HWND hNotifyWnd);

/* Send a single file SYNCHRONOUSLY (blocks until complete) */
/* Use this for server-side transfers to avoid socket threading issues */
int FileTransfer_SendFileSync(PRD2K_NETWORK pNet, const char *filePath, HWND hNotifyWnd);

#endif /* FILETRANSFER_H */
