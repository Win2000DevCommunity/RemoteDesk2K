/*
 * RemoteDesk2K - File Transfer Module Implementation v3
 * Windows 2000 compatible file transfer with worker thread
 * 
 * Key Points for Windows 2000:
 * - Uses CreateThread for async file transfer (kernel32.dll)
 * - Worker thread does network I/O, main thread shows progress
 * - PostMessage for thread-safe UI updates
 * - Uses ReadFile/WriteFile (kernel32.dll)
 * - Memory allocation via HeapAlloc (more reliable than malloc on Win2K)
 * - SHGetFolderPathA for special folders (shell32.dll)
 * - SHGetPathFromIDListA for PIDL to path conversion (language-independent)
 * - IShellWindows COM interface for Explorer window enumeration
 * - All file operations use ANSI APIs (not Unicode)
 * - Releases modifier keys before dialogs to prevent stuck keys
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS  /* Enable COM macros for C */
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <exdisp.h>  /* Shell automation: IShellWindows, IWebBrowser2 */
#include "filetransfer.h"
#include "progress.h"
#include "common.h"
#include "network.h"
#include "input.h"

/* GUIDs for Shell Automation - Windows 2000 SDK */
/* IShellWindows CLSID: 9BA05972-F6A8-11CF-A442-00A0C90A8F39 */
static const GUID CLSID_ShellWindowsLocal = 
    {0x9BA05972, 0xF6A8, 0x11CF, {0xA4, 0x42, 0x00, 0xA0, 0xC9, 0x0A, 0x8F, 0x39}};

/* IShellWindows IID: 85CB6900-4D95-11CF-960C-0080C7F4EE85 */
static const GUID IID_IShellWindowsLocal = 
    {0x85CB6900, 0x4D95, 0x11CF, {0x96, 0x0C, 0x00, 0x80, 0xC7, 0xF4, 0xEE, 0x85}};

/* IWebBrowser2 IID: D30C1661-CDAF-11D0-8A3E-00C04FC9E26E */
static const GUID IID_IWebBrowser2Local = 
    {0xD30C1661, 0xCDAF, 0x11D0, {0x8A, 0x3E, 0x00, 0xC0, 0x4F, 0xC9, 0xE2, 0x6E}};

/* Global file transfer context */
static FT_CONTEXT g_ftContext = {0};
static char g_lastError[256] = {0};

/* Remembered active folder - captured when viewer loses focus */
static char g_rememberedFolder[MAX_PATH] = {0};

/* Critical section for thread-safe access */
static CRITICAL_SECTION g_csTransfer;
static BOOL g_bCsInitialized = FALSE;

/* Set error message (thread-safe) */
static void SetLastFTError(const char *msg)
{
    if (g_bCsInitialized) EnterCriticalSection(&g_csTransfer);
    if (msg) {
        lstrcpynA(g_lastError, msg, sizeof(g_lastError) - 1);
    } else {
        g_lastError[0] = '\0';
    }
    if (g_bCsInitialized) LeaveCriticalSection(&g_csTransfer);
}

/* Initialize file transfer module */
BOOL FileTransfer_Init(HWND hNotifyWnd, HINSTANCE hInstance)
{
    /* Initialize critical section */
    InitializeCriticalSection(&g_csTransfer);
    g_bCsInitialized = TRUE;
    
    ZeroMemory(&g_ftContext, sizeof(g_ftContext));
    g_ftContext.state = FT_STATE_IDLE;
    g_ftContext.hFile = INVALID_HANDLE_VALUE;
    g_ftContext.hThread = NULL;
    g_ftContext.hNotifyWnd = hNotifyWnd;
    g_ftContext.hInstance = hInstance;
    g_ftContext.bShowProgress = FALSE;
    g_ftContext.bCancelRequested = FALSE;
    g_lastError[0] = '\0';
    
    /* Initialize progress dialog module */
    Progress_Init(hInstance);
    
    return TRUE;
}

/* Cleanup file transfer module */
void FileTransfer_Cleanup(void)
{
    FileTransfer_Cancel();
    Progress_Cleanup();
    g_ftContext.hNotifyWnd = NULL;
    
    if (g_bCsInitialized) {
        DeleteCriticalSection(&g_csTransfer);
        g_bCsInitialized = FALSE;
    }
}

/* Cancel current transfer */
void FileTransfer_Cancel(void)
{
    /* Signal cancel to worker thread */
    g_ftContext.bCancelRequested = TRUE;
    
    /* Wait for worker thread to finish (with timeout) */
    if (g_ftContext.hThread) {
        WaitForSingleObject(g_ftContext.hThread, 3000);
        CloseHandle(g_ftContext.hThread);
        g_ftContext.hThread = NULL;
    }
    
    if (g_ftContext.hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ftContext.hFile);
        g_ftContext.hFile = INVALID_HANDLE_VALUE;
    }
    
    /* If we were receiving and file exists partially, delete it */
    if (g_ftContext.state == FT_STATE_RECEIVING && g_ftContext.filePath[0]) {
        DeleteFileA(g_ftContext.filePath);
    }
    
    /* Hide progress dialog */
    Progress_Hide();
    
    g_ftContext.state = FT_STATE_IDLE;
    g_ftContext.fileSize = 0;
    g_ftContext.bytesTransferred = 0;
    g_ftContext.chunkIndex = 0;
    g_ftContext.fileName[0] = '\0';
    g_ftContext.filePath[0] = '\0';
    g_ftContext.bShowProgress = FALSE;
    g_ftContext.bCancelRequested = FALSE;
}

/* Check if transfer is in progress */
BOOL FileTransfer_IsBusy(void)
{
    return (g_ftContext.state != FT_STATE_IDLE);
}

/* Get current transfer progress (0-100) */
int FileTransfer_GetProgress(void)
{
    if (g_ftContext.fileSize == 0) return 0;
    return (int)((g_ftContext.bytesTransferred * 100) / g_ftContext.fileSize);
}

/* Get last error message */
const char* FileTransfer_GetLastError(void)
{
    return g_lastError;
}

/*
 * GetExplorerFolderViaCOM - Get folder path from Explorer using Shell Automation COM
 * 
 * This is the proper Windows 2000 method using IShellWindows interface.
 * It's language-independent and works with any localized Windows.
 * 
 * hwndTarget: specific Explorer window HWND to query (or NULL for foreground)
 * pathOut: buffer to receive folder path
 * pathSize: size of buffer
 * 
 * Returns TRUE if path found, FALSE otherwise
 */
static BOOL GetExplorerFolderViaCOM(HWND hwndTarget, char *pathOut, int pathSize)
{
    HRESULT hr;
    IShellWindows *pShellWindows = NULL;
    IDispatch *pDisp = NULL;
    IWebBrowser2 *pWebBrowser = NULL;
    BSTR bstrLocationURL = NULL;
    VARIANT v;
    long count, i;
    long hwndBrowser;  /* Use long, as per ExDisp.h on Windows 2000 */
    BOOL found = FALSE;
    char urlPath[MAX_PATH * 2];
    DWORD urlPathSize;
    
    if (!pathOut || pathSize < MAX_PATH) return FALSE;
    pathOut[0] = '\0';
    
    /* Initialize COM if not already done */
    hr = CoInitialize(NULL);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return FALSE;
    }
    
    /* Create ShellWindows object */
    hr = CoCreateInstance(&CLSID_ShellWindowsLocal, NULL, CLSCTX_ALL,
                          &IID_IShellWindowsLocal, (void**)&pShellWindows);
    if (FAILED(hr) || !pShellWindows) {
        goto cleanup;
    }
    
    /* Get count of shell windows */
    hr = pShellWindows->lpVtbl->get_Count(pShellWindows, &count);
    if (FAILED(hr)) {
        goto cleanup;
    }
    
    /* Iterate through all shell windows */
    for (i = 0; i < count && !found; i++) {
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        
        hr = pShellWindows->lpVtbl->Item(pShellWindows, v, &pDisp);
        if (FAILED(hr) || !pDisp) {
            continue;
        }
        
        /* Query for IWebBrowser2 interface */
        hr = pDisp->lpVtbl->QueryInterface(pDisp, &IID_IWebBrowser2Local, (void**)&pWebBrowser);
        pDisp->lpVtbl->Release(pDisp);
        pDisp = NULL;
        
        if (FAILED(hr) || !pWebBrowser) {
            continue;
        }
        
        /* Get window handle to check if this is the target window */
        hr = pWebBrowser->lpVtbl->get_HWND(pWebBrowser, &hwndBrowser);
        if (SUCCEEDED(hr)) {
            /* If we have a target window, only match that one */
            /* If no target, match any Explorer window (prefer foreground) */
            if (hwndTarget == NULL || (HWND)hwndBrowser == hwndTarget) {
                /* Get the location URL (file:///path format) */
                hr = pWebBrowser->lpVtbl->get_LocationURL(pWebBrowser, &bstrLocationURL);
                if (SUCCEEDED(hr) && bstrLocationURL) {
                    /* Convert BSTR (wide) URL to ANSI path */
                    /* URL format: file:///C:/path or file:///C:\path */
                    urlPathSize = sizeof(urlPath);
                    if (WideCharToMultiByte(CP_ACP, 0, bstrLocationURL, -1, urlPath, urlPathSize, NULL, NULL) > 0) {
                        /* Check if it's a file:// URL */
                        if (_strnicmp(urlPath, "file:///", 8) == 0) {
                            char *src = urlPath + 8;  /* Skip "file:///" */
                            char *dst = pathOut;
                            int remaining = pathSize - 1;
                            
                            /* URL decode and convert forward slashes to backslashes */
                            while (*src && remaining > 0) {
                                if (*src == '%' && src[1] && src[2]) {
                                    /* URL decode %XX */
                                    char hex[3] = {src[1], src[2], 0};
                                    *dst++ = (char)strtol(hex, NULL, 16);
                                    src += 3;
                                } else if (*src == '/') {
                                    *dst++ = '\\';
                                    src++;
                                } else {
                                    *dst++ = *src++;
                                }
                                remaining--;
                            }
                            *dst = '\0';
                            
                            /* Verify it's a valid directory */
                            if (pathOut[0]) {
                                DWORD attrs = GetFileAttributesA(pathOut);
                                if (attrs != 0xFFFFFFFF && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                                    found = TRUE;
                                }
                            }
                        }
                    }
                    SysFreeString(bstrLocationURL);
                    bstrLocationURL = NULL;
                }
                
                /* If we had a target and found it, we're done */
                if (hwndTarget != NULL) {
                    pWebBrowser->lpVtbl->Release(pWebBrowser);
                    pWebBrowser = NULL;
                    break;
                }
            }
        }
        
        pWebBrowser->lpVtbl->Release(pWebBrowser);
        pWebBrowser = NULL;
    }
    
cleanup:
    if (bstrLocationURL) SysFreeString(bstrLocationURL);
    if (pWebBrowser) pWebBrowser->lpVtbl->Release(pWebBrowser);
    if (pDisp) pDisp->lpVtbl->Release(pDisp);
    if (pShellWindows) pShellWindows->lpVtbl->Release(pShellWindows);
    CoUninitialize();
    
    return found;
}

/* Fallback: Get path from window title (for non-COM cases) */
static BOOL GetExplorerPathFromTitle(HWND hwndExplorer, char *pathOut, int pathSize)
{
    char windowTitle[MAX_PATH];
    DWORD attrs;
    
    if (!hwndExplorer || !pathOut || pathSize < MAX_PATH) return FALSE;
    pathOut[0] = '\0';
    
    if (GetWindowTextA(hwndExplorer, windowTitle, sizeof(windowTitle)) > 0) {
        /* Check if title looks like a full path (starts with drive letter) */
        if (windowTitle[0] != '\0' && windowTitle[1] == ':' && windowTitle[2] == '\\') {
            attrs = GetFileAttributesA(windowTitle);
            if (attrs != 0xFFFFFFFF && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                lstrcpynA(pathOut, windowTitle, pathSize);
                return TRUE;
            }
        }
        
        /* Check for UNC path */
        if (windowTitle[0] == '\\' && windowTitle[1] == '\\') {
            attrs = GetFileAttributesA(windowTitle);
            if (attrs != 0xFFFFFFFF && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                lstrcpynA(pathOut, windowTitle, pathSize);
                return TRUE;
            }
        }
    }
    
    return FALSE;
}

/* Combined function: Try COM first, then fallback to window title */
static BOOL GetExplorerPathFromHwnd(HWND hwndExplorer, char *pathOut, int pathSize)
{
    /* First try COM-based approach (language-independent) */
    if (GetExplorerFolderViaCOM(hwndExplorer, pathOut, pathSize)) {
        return TRUE;
    }
    
    /* Fallback to window title parsing */
    return GetExplorerPathFromTitle(hwndExplorer, pathOut, pathSize);
}

/* Get active Explorer folder path for paste destination */
BOOL FileTransfer_GetActiveFolder(char *pathOut, int pathSize)
{
    HWND hwndFG;
    char className[64];
    
    if (!pathOut || pathSize < MAX_PATH) return FALSE;
    pathOut[0] = '\0';
    
    /* FIRST check the foreground window - this is the most recently active window */
    hwndFG = GetForegroundWindow();
    if (hwndFG) {
        if (GetClassNameA(hwndFG, className, sizeof(className)) > 0) {
            /* Check if foreground window is an Explorer window */
            if (lstrcmpiA(className, "CabinetWClass") == 0 || 
                lstrcmpiA(className, "ExploreWClass") == 0) {
                /* Try COM first for this specific window */
                if (GetExplorerFolderViaCOM(hwndFG, pathOut, pathSize)) {
                    return TRUE;
                }
                /* Fallback to window title */
                if (GetExplorerPathFromTitle(hwndFG, pathOut, pathSize)) {
                    return TRUE;
                }
            }
        }
    }
    
    /* If foreground is not Explorer, try COM to find any Explorer window */
    if (GetExplorerFolderViaCOM(NULL, pathOut, pathSize)) {
        return TRUE;
    }
    
    /* Default to Desktop folder */
    if (SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, pathOut) == S_OK) {
        return TRUE;
    }
    
    lstrcpyA(pathOut, "C:\\");
    return TRUE;
}

/* Helper structure for EnumWindows callback */
typedef struct _FIND_EXPLORER_DATA {
    HWND hwndExclude;
    HWND hwndFound;
    char folderPath[MAX_PATH];
    HWND hwndForeground;
} FIND_EXPLORER_DATA;

/* EnumWindows callback to find Explorer window */
static BOOL CALLBACK FindExplorerWindowProc(HWND hwnd, LPARAM lParam)
{
    FIND_EXPLORER_DATA *pData = (FIND_EXPLORER_DATA*)lParam;
    char className[64];
    
    if (hwnd == pData->hwndExclude) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetClassNameA(hwnd, className, sizeof(className)) == 0) return TRUE;
    
    if (lstrcmpiA(className, "CabinetWClass") != 0 && 
        lstrcmpiA(className, "ExploreWClass") != 0) {
        return TRUE;
    }
    
    /* Try to get path using COM first, then window title */
    if (hwnd == pData->hwndForeground) {
        if (GetExplorerPathFromHwnd(hwnd, pData->folderPath, sizeof(pData->folderPath))) {
            pData->hwndFound = hwnd;
            return FALSE;
        }
    }
    
    if (!pData->hwndFound) {
        if (GetExplorerPathFromHwnd(hwnd, pData->folderPath, sizeof(pData->folderPath))) {
            pData->hwndFound = hwnd;
        }
    }
    
    return TRUE;
}

/* Capture and remember the currently active Explorer folder */
void FileTransfer_CaptureActiveFolder(HWND hwndExclude)
{
    FIND_EXPLORER_DATA data;
    char pathBuffer[MAX_PATH];
    
    ZeroMemory(&data, sizeof(data));
    data.hwndExclude = hwndExclude;
    data.hwndForeground = GetForegroundWindow();
    
    /* First try COM-based approach - most reliable */
    if (GetExplorerFolderViaCOM(NULL, pathBuffer, sizeof(pathBuffer))) {
        lstrcpynA(g_rememberedFolder, pathBuffer, sizeof(g_rememberedFolder));
        return;
    }
    
    /* Fallback: Enumerate windows to find Explorer */
    EnumWindows(FindExplorerWindowProc, (LPARAM)&data);
    
    if (data.hwndFound && data.folderPath[0] != '\0') {
        lstrcpynA(g_rememberedFolder, data.folderPath, sizeof(g_rememberedFolder));
    }
}

/* Get the remembered active folder */
BOOL FileTransfer_GetRememberedFolder(char *pathOut, int pathSize)
{
    if (!pathOut || pathSize < MAX_PATH) return FALSE;
    
    if (g_rememberedFolder[0] != '\0') {
        DWORD attrs = GetFileAttributesA(g_rememberedFolder);
        if (attrs != 0xFFFFFFFF && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            lstrcpynA(pathOut, g_rememberedFolder, pathSize);
            return TRUE;
        }
    }
    
    if (SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, pathOut) == S_OK) {
        return TRUE;
    }
    
    lstrcpyA(pathOut, "C:\\");
    return TRUE;
}

/* Send ACK to sender for flow control */
int FileTransfer_SendAck(PRD2K_NETWORK pNet, DWORD chunkIndex, DWORD status)
{
    RD2K_FILE_ACK ack;
    ack.chunkIndex = chunkIndex;
    ack.status = status;
    return Network_SendPacket(pNet, MSG_FILE_ACK, (const BYTE*)&ack, sizeof(ack));
}

/* Check if path is a directory */
BOOL FileTransfer_IsDirectory(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != 0xFFFFFFFF && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

/* Worker thread function for sending file - 64-bit file size support */
static DWORD WINAPI SendFileThreadProc(LPVOID lpParam)
{
    HANDLE hFile;
    LARGE_INTEGER liFileSize;
    ULONGLONG fileSize;
    DWORD bytesRead;
    ULONGLONG totalSent;
    BYTE *chunkBuffer;
    RD2K_FILE_HEADER fileHeader;
    RD2K_FILE_CHUNK chunkHeader;
    BYTE *sendBuffer;
    DWORD chunkIndex;
    DWORD totalChunks;
    int result;
    DWORD lastUpdateTime;
    
    /* Open file for reading */
    hFile = CreateFileA(g_ftContext.filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        SetLastFTError("Cannot open file");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_FILE_NOT_FOUND, 0);
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Get file size using 64-bit API */
    if (!GetFileSizeEx(hFile, &liFileSize)) {
        CloseHandle(hFile);
        SetLastFTError("Cannot get file size");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_READ, 0);
        return FT_ERR_READ;
    }
    
    fileSize = (ULONGLONG)liFileSize.QuadPart;
    
    if (fileSize == 0) {
        CloseHandle(hFile);
        SetLastFTError("File is empty");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_READ, 0);
        return FT_ERR_READ;
    }
    
    if (fileSize > FT_MAX_FILE_SIZE) {
        CloseHandle(hFile);
        SetLastFTError("File too large (max 100GB)");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_FILE_TOO_LARGE, 0);
        return FT_ERR_FILE_TOO_LARGE;
    }
    
    g_ftContext.fileSize = fileSize;
    totalChunks = (DWORD)((fileSize + FT_CHUNK_SIZE - 1) / FT_CHUNK_SIZE);
    g_ftContext.totalChunks = totalChunks;
    
    /* Allocate chunk buffer */
    chunkBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, FT_CHUNK_SIZE);
    if (!chunkBuffer) {
        CloseHandle(hFile);
        SetLastFTError("Out of memory");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_MEMORY, 0);
        return FT_ERR_MEMORY;
    }
    
    /* Allocate send buffer for chunk header + data */
    sendBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, sizeof(RD2K_FILE_CHUNK) + FT_CHUNK_SIZE);
    if (!sendBuffer) {
        HeapFree(GetProcessHeap(), 0, chunkBuffer);
        CloseHandle(hFile);
        SetLastFTError("Out of memory for send buffer");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_MEMORY, 0);
        return FT_ERR_MEMORY;
    }
    
    /* Prepare and send file header with 64-bit size */
    ZeroMemory(&fileHeader, sizeof(fileHeader));
    lstrcpynA(fileHeader.fileName, g_ftContext.fileName, sizeof(fileHeader.fileName) - 1);
    fileHeader.fileSizeLow = (DWORD)(fileSize & 0xFFFFFFFF);
    fileHeader.fileSizeHigh = (DWORD)(fileSize >> 32);
    fileHeader.fileCount = 1;
    fileHeader.totalChunks = totalChunks;
    
    result = Network_SendPacket(g_ftContext.pNet, MSG_FILE_START, (const BYTE*)&fileHeader, sizeof(fileHeader));
    if (result != RD2K_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, sendBuffer);
        HeapFree(GetProcessHeap(), 0, chunkBuffer);
        CloseHandle(hFile);
        SetLastFTError("Network error sending header");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_NETWORK, 0);
        return FT_ERR_NETWORK;
    }
    
    /* Send file data in chunks with flow control */
    totalSent = 0;
    chunkIndex = 0;
    lastUpdateTime = GetTickCount();
    
    while (totalSent < fileSize) {
        DWORD chunkSize = (DWORD)((fileSize - totalSent > FT_CHUNK_SIZE) ? FT_CHUNK_SIZE : (fileSize - totalSent));
        
        /* Check for cancel */
        if (g_ftContext.bCancelRequested || Progress_IsCancelled()) {
            g_ftContext.bCancelRequested = TRUE;
            HeapFree(GetProcessHeap(), 0, sendBuffer);
            HeapFree(GetProcessHeap(), 0, chunkBuffer);
            CloseHandle(hFile);
            SetLastFTError("Transfer cancelled by user");
            PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_CANCELLED, 0);
            return FT_ERR_CANCELLED;
        }
        
        /* Read chunk from file */
        if (!ReadFile(hFile, chunkBuffer, chunkSize, &bytesRead, NULL) || bytesRead != chunkSize) {
            HeapFree(GetProcessHeap(), 0, sendBuffer);
            HeapFree(GetProcessHeap(), 0, chunkBuffer);
            CloseHandle(hFile);
            SetLastFTError("Failed to read file");
            PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_READ, 0);
            return FT_ERR_READ;
        }
        
        /* Prepare chunk header */
        chunkHeader.chunkIndex = chunkIndex;
        chunkHeader.chunkSize = chunkSize;
        
        /* Copy header and data to send buffer */
        CopyMemory(sendBuffer, &chunkHeader, sizeof(RD2K_FILE_CHUNK));
        CopyMemory(sendBuffer + sizeof(RD2K_FILE_CHUNK), chunkBuffer, chunkSize);
        
        /* Send chunk with retry logic for network errors */
        {
            int retryCount = 0;
            const int maxRetries = 3;
            
            while (retryCount < maxRetries) {
                result = Network_SendPacket(g_ftContext.pNet, MSG_FILE_DATA, sendBuffer, sizeof(RD2K_FILE_CHUNK) + chunkSize);
                if (result == RD2K_SUCCESS) {
                    break;
                }
                retryCount++;
                if (retryCount < maxRetries) {
                    Sleep(100 * retryCount);  /* Exponential backoff */
                }
            }
            
            if (result != RD2K_SUCCESS) {
                HeapFree(GetProcessHeap(), 0, sendBuffer);
                HeapFree(GetProcessHeap(), 0, chunkBuffer);
                CloseHandle(hFile);
                SetLastFTError("Network error sending data");
                PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_NETWORK, 0);
                return FT_ERR_NETWORK;
            }
        }
        
        totalSent += chunkSize;
        g_ftContext.bytesTransferred = totalSent;
        chunkIndex++;
        
        /* Update progress using 64-bit function (throttled to every 100ms to avoid flooding UI) */
        if (GetTickCount() - lastUpdateTime >= 100) {
            Progress_Update64(totalSent);
            lastUpdateTime = GetTickCount();
        }
        
        /* Flow control: Adaptive delay based on file size.
           Larger files need less aggressive sending to prevent buffer overflow.
           This is critical for Windows 2000's network stack.
           
           The key insight: Windows 2000's TCP stack has limited buffer space.
           If we send too fast, the receiver's buffers overflow and data is lost.
           We need to give the receiver time to process and write data to disk. */
        if (fileSize > (ULONGLONG)100 * 1024 * 1024) {
            /* Very large files (>100MB): aggressive flow control */
            /* Add delay after every 4 chunks (128KB) to prevent buffer overflow */
            if ((chunkIndex % 4) == 0) {
                Sleep(30);
            }
        } else if (fileSize > (ULONGLONG)10 * 1024 * 1024) {
            /* Large files (10-100MB): moderate flow control */
            /* Add delay after every 8 chunks (256KB) */
            if ((chunkIndex % FT_ACK_INTERVAL) == 0) {
                Sleep(20);
            }
        } else {
            /* Smaller files (<10MB): minimal delay, still need some breathing room */
            if ((chunkIndex % 16) == 0) {
                Sleep(5);
            }
        }
    }
    
    /* Free buffers */
    HeapFree(GetProcessHeap(), 0, sendBuffer);
    HeapFree(GetProcessHeap(), 0, chunkBuffer);
    CloseHandle(hFile);
    
    /* Delay before end marker to ensure all data is processed by receiver.
       For large files on Windows 2000, we need extra time for the receiver
       to process all the buffered data and write it to disk. */
    {
        DWORD delayMs;
        if (fileSize > (ULONGLONG)100 * 1024 * 1024) {
            delayMs = 500;  /* 500ms for files >100MB */
        } else if (fileSize > (ULONGLONG)10 * 1024 * 1024) {
            delayMs = 200;  /* 200ms for files >10MB */
        } else {
            delayMs = 100;  /* 100ms for smaller files */
        }
        Sleep(delayMs);
    }
    
    /* Send file end marker */
    result = Network_SendPacket(g_ftContext.pNet, MSG_FILE_END, NULL, 0);
    if (result != RD2K_SUCCESS) {
        SetLastFTError("Network error sending end marker");
        PostMessage(g_ftContext.hNotifyWnd, WM_FT_ERROR, FT_ERR_NETWORK, 0);
        return FT_ERR_NETWORK;
    }
    
    /* Signal completion */
    PostMessage(g_ftContext.hNotifyWnd, WM_FT_COMPLETE, FT_SUCCESS, 0);
    SetLastFTError(NULL);
    
    return FT_SUCCESS;
}

/* Send a file to remote (async with worker thread) */
int FileTransfer_SendFile(PRD2K_NETWORK pNet, const char *filePath, BOOL bShowProgress)
{
    const char *fileName;
    ULONGLONG fileSize;
    LARGE_INTEGER liFileSize;
    HANDLE hFile;
    
    if (!pNet || !filePath) {
        SetLastFTError("Invalid parameters");
        return FT_ERR_NETWORK;
    }
    
    if (FileTransfer_IsBusy()) {
        SetLastFTError("Transfer already in progress");
        return FT_ERR_BUSY;
    }
    
    /* Check if this is a directory - use folder transfer instead */
    if (FileTransfer_IsDirectory(filePath)) {
        return FileTransfer_SendFolder(pNet, filePath, bShowProgress);
    }
    
    /* Quick check - can we open the file? */
    hFile = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        SetLastFTError("Cannot open file");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Get file size using 64-bit API for large file support */
    if (!GetFileSizeEx(hFile, &liFileSize)) {
        CloseHandle(hFile);
        SetLastFTError("Cannot get file size");
        return FT_ERR_READ;
    }
    CloseHandle(hFile);
    
    fileSize = (ULONGLONG)liFileSize.QuadPart;
    
    if (fileSize == 0) {
        SetLastFTError("File is empty");
        return FT_ERR_READ;
    }
    
    if (fileSize > FT_MAX_FILE_SIZE) {
        SetLastFTError("File too large (max 100GB)");
        return FT_ERR_FILE_TOO_LARGE;
    }
    
    /* Get filename from path */
    fileName = strrchr(filePath, '\\');
    if (fileName) {
        fileName++;
    } else {
        fileName = filePath;
    }
    
    /* Setup context for worker thread */
    g_ftContext.state = FT_STATE_SENDING;
    g_ftContext.fileSize = fileSize;
    g_ftContext.bytesTransferred = 0;
    g_ftContext.totalChunks = (DWORD)((fileSize + FT_CHUNK_SIZE - 1) / FT_CHUNK_SIZE);
    g_ftContext.bShowProgress = bShowProgress;
    g_ftContext.bCancelRequested = FALSE;
    g_ftContext.pNet = pNet;
    lstrcpynA(g_ftContext.fileName, fileName, sizeof(g_ftContext.fileName));
    lstrcpynA(g_ftContext.filePath, filePath, sizeof(g_ftContext.filePath));
    
    /* Show progress dialog if requested - use 64-bit version */
    if (bShowProgress) {
        /* CRITICAL: Release modifier keys before showing progress dialog.
           This prevents the "stuck Ctrl key" problem that causes Explorer
           to behave as if Ctrl is held (multi-select mode). */
        Input_ReleaseAllModifiers();
        Progress_Show64(g_ftContext.hNotifyWnd, fileName, fileSize, TRUE);
    }
    
    /* Create worker thread */
    g_ftContext.hThread = CreateThread(NULL, 0, SendFileThreadProc, NULL, 0, &g_ftContext.dwThreadId);
    if (!g_ftContext.hThread) {
        g_ftContext.state = FT_STATE_IDLE;
        Progress_Hide();
        SetLastFTError("Failed to create worker thread");
        return FT_ERR_MEMORY;
    }
    
    /* Return immediately - transfer happens in background */
    /* Caller should handle WM_FT_PROGRESS, WM_FT_COMPLETE, WM_FT_ERROR messages */
    return FT_SUCCESS;
}

/* Start receiving a file */
int FileTransfer_StartReceive(const BYTE *data, DWORD length, const char *destFolder)
{
    RD2K_FILE_HEADER *pHeader;
    char targetFolder[MAX_PATH];
    ULONGLONG fileSize;
    
    if (!data || length < sizeof(RD2K_FILE_HEADER)) {
        SetLastFTError("Invalid file header");
        return FT_ERR_NETWORK;
    }
    
    if (FileTransfer_IsBusy()) {
        SetLastFTError("Transfer already in progress");
        return FT_ERR_BUSY;
    }
    
    pHeader = (RD2K_FILE_HEADER*)data;
    
    /* Validate filename - prevent path traversal */
    if (pHeader->fileName[0] == '\0' ||
        strchr(pHeader->fileName, '\\') != NULL ||
        strchr(pHeader->fileName, '/') != NULL ||
        strstr(pHeader->fileName, "..") != NULL) {
        SetLastFTError("Invalid filename");
        return FT_ERR_NETWORK;
    }
    
    /* Reconstruct 64-bit file size from high/low parts */
    fileSize = ((ULONGLONG)pHeader->fileSizeHigh << 32) | (ULONGLONG)pHeader->fileSizeLow;
    
    /* Check file size - support up to 100GB */
    if (fileSize > FT_MAX_FILE_SIZE) {
        SetLastFTError("File too large (max 100GB)");
        return FT_ERR_FILE_TOO_LARGE;
    }
    
    /* Use provided destination folder, or use remembered folder, or fall back to Desktop */
    /* NOTE: We use GetRememberedFolder instead of GetActiveFolder because at this point
       the viewer window is likely in foreground (user is interacting with remote desktop).
       The remembered folder is captured when viewer window gets focus. */
    if (destFolder && destFolder[0]) {
        lstrcpynA(targetFolder, destFolder, sizeof(targetFolder));
    } else if (!FileTransfer_GetRememberedFolder(targetFolder, sizeof(targetFolder))) {
        if (SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, targetFolder) != S_OK) {
            lstrcpyA(targetFolder, "C:\\");
        }
    }
    
    /* Build full path */
    wsprintfA(g_ftContext.filePath, "%s\\%s", targetFolder, pHeader->fileName);
    
    /* Create the file */
    g_ftContext.hFile = CreateFileA(g_ftContext.filePath, GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_ftContext.hFile == INVALID_HANDLE_VALUE) {
        /* Try desktop as fallback */
        if (SHGetFolderPathA(NULL, CSIDL_DESKTOP, NULL, 0, targetFolder) == S_OK) {
            wsprintfA(g_ftContext.filePath, "%s\\%s", targetFolder, pHeader->fileName);
            g_ftContext.hFile = CreateFileA(g_ftContext.filePath, GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        
        if (g_ftContext.hFile == INVALID_HANDLE_VALUE) {
            SetLastFTError("Cannot create file");
            return FT_ERR_CREATE_FILE;
        }
    }
    
    /* Update state with 64-bit file size */
    g_ftContext.state = FT_STATE_RECEIVING;
    g_ftContext.fileSize = fileSize;
    g_ftContext.bytesTransferred = 0;
    g_ftContext.chunkIndex = 0;
    g_ftContext.totalChunks = pHeader->totalChunks;
    g_ftContext.bShowProgress = TRUE;
    g_ftContext.bCancelRequested = FALSE;
    lstrcpynA(g_ftContext.fileName, pHeader->fileName, sizeof(g_ftContext.fileName));
    lstrcpynA(g_ftContext.destFolder, targetFolder, sizeof(g_ftContext.destFolder));
    
    /* CRITICAL: Release modifier keys before showing progress dialog.
       This prevents the "stuck Ctrl key" problem that causes Explorer
       to behave as if Ctrl is held (multi-select mode). */
    Input_ReleaseAllModifiers();
    
    /* Show progress dialog for receiving - use 64-bit version */
    Progress_Show64(g_ftContext.hNotifyWnd, pHeader->fileName, fileSize, FALSE);
    
    SetLastFTError(NULL);
    return FT_SUCCESS;
}

/* Receive file data chunk */
int FileTransfer_ReceiveData(const BYTE *data, DWORD length)
{
    RD2K_FILE_CHUNK *pChunk;
    DWORD bytesWritten;
    const BYTE *chunkData;
    static DWORD lastAckChunk = 0;
    
    if (g_ftContext.state != FT_STATE_RECEIVING) {
        SetLastFTError("Not receiving");
        return FT_ERR_NETWORK;
    }
    
    if (!data || length < sizeof(RD2K_FILE_CHUNK)) {
        SetLastFTError("Invalid chunk data");
        return FT_ERR_NETWORK;
    }
    
    if (g_ftContext.hFile == INVALID_HANDLE_VALUE) {
        SetLastFTError("File not open");
        return FT_ERR_WRITE;
    }
    
    /* Check for user cancel */
    if (g_ftContext.bCancelRequested || Progress_IsCancelled()) {
        g_ftContext.bCancelRequested = TRUE;
        SetLastFTError("Transfer cancelled by user");
        return FT_ERR_CANCELLED;
    }
    
    pChunk = (RD2K_FILE_CHUNK*)data;
    chunkData = data + sizeof(RD2K_FILE_CHUNK);
    
    /* Validate chunk */
    if (pChunk->chunkSize > length - sizeof(RD2K_FILE_CHUNK)) {
        SetLastFTError("Invalid chunk size");
        return FT_ERR_NETWORK;
    }
    
    /* Validate chunk size is reasonable (max 64KB) */
    if (pChunk->chunkSize > 65536) {
        SetLastFTError("Chunk too large");
        return FT_ERR_NETWORK;
    }
    
    /* Write data to file */
    if (!WriteFile(g_ftContext.hFile, chunkData, pChunk->chunkSize, &bytesWritten, NULL) ||
        bytesWritten != pChunk->chunkSize) {
        SetLastFTError("Write error");
        return FT_ERR_WRITE;
    }
    
    /* Flush file buffer periodically for large files to ensure data is written */
    if ((g_ftContext.chunkIndex % 32) == 0) {
        FlushFileBuffers(g_ftContext.hFile);
    }
    
    g_ftContext.bytesTransferred += pChunk->chunkSize;
    g_ftContext.chunkIndex++;
    
    /* Update progress dialog directly (receiving is already async from network thread) */
    /* Use 64-bit version for large file support */
    if (g_ftContext.bShowProgress) {
        Progress_Update64(g_ftContext.bytesTransferred);
    }
    
    /* Reset lastAckChunk when starting new transfer */
    if (pChunk->chunkIndex == 0) {
        lastAckChunk = 0;
    }
    
    return FT_SUCCESS;
}

/* Complete file receive */
int FileTransfer_EndReceive(void)
{
    if (g_ftContext.state != FT_STATE_RECEIVING) {
        return FT_ERR_NETWORK;
    }
    
    /* Close file */
    if (g_ftContext.hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ftContext.hFile);
        g_ftContext.hFile = INVALID_HANDLE_VALUE;
    }
    
    /* Hide progress dialog */
    Progress_Hide();
    
    /* DON'T show MessageBox - it steals focus and can confuse Explorer.
       Instead, just post a message that the main window can handle
       to show a non-blocking notification or status bar update. */
    PostMessage(g_ftContext.hNotifyWnd, WM_FT_COMPLETE, FT_SUCCESS, (LPARAM)1);
    
    /* Reset state */
    g_ftContext.state = FT_STATE_IDLE;
    g_ftContext.fileSize = 0;
    g_ftContext.bytesTransferred = 0;
    g_ftContext.chunkIndex = 0;
    g_ftContext.bShowProgress = FALSE;
    g_ftContext.bCancelRequested = FALSE;
    g_ftContext.fileName[0] = '\0';
    g_ftContext.filePath[0] = '\0';
    g_ftContext.destFolder[0] = '\0';
    
    SetLastFTError(NULL);
    return FT_SUCCESS;
}

/* Send files from clipboard to remote */
int FileTransfer_SendClipboardFiles(PRD2K_NETWORK pNet, HWND hClipOwner, BOOL bShowProgress)
{
    HANDLE hDrop;
    UINT fileCount;
    UINT i;
    char filePath[MAX_PATH];
    DWORD attrs;
    int result = FT_SUCCESS;
    int filesSent = 0;
    
    if (!pNet) {
        SetLastFTError("Invalid parameters");
        return FT_ERR_NETWORK;
    }
    
    /* Open clipboard */
    if (!OpenClipboard(hClipOwner)) {
        SetLastFTError("Cannot open clipboard");
        return FT_ERR_NETWORK;
    }
    
    /* Get file drop handle */
    hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) {
        CloseClipboard();
        SetLastFTError("No files in clipboard");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Count files */
    fileCount = DragQueryFileA((HDROP)hDrop, 0xFFFFFFFF, NULL, 0);
    if (fileCount == 0) {
        CloseClipboard();
        SetLastFTError("No files in clipboard");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* For now, just send the first file/folder asynchronously */
    /* TODO: Queue multiple files */
    for (i = 0; i < fileCount && i < 1; i++) {
        if (DragQueryFileA((HDROP)hDrop, i, filePath, sizeof(filePath))) {
            attrs = GetFileAttributesA(filePath);
            if (attrs != 0xFFFFFFFF) {
                /* FileTransfer_SendFile now handles both files and directories */
                result = FileTransfer_SendFile(pNet, filePath, bShowProgress);
                if (result == FT_SUCCESS) {
                    filesSent++;
                }
                break; /* Only one file at a time with async */
            }
        }
    }
    
    CloseClipboard();
    
    if (filesSent == 0 && result == FT_SUCCESS) {
        SetLastFTError("No files were transferred");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    return result;
}

/* ============================================================================
 * FOLDER TRANSFER IMPLEMENTATION
 * ============================================================================ */

/* Helper: Count files in a directory recursively */
static DWORD CountFilesInFolder(const char *folderPath)
{
    WIN32_FIND_DATAA findData;
    HANDLE hFind;
    char searchPath[MAX_PATH];
    DWORD count = 0;
    
    wsprintfA(searchPath, "%s\\*", folderPath);
    
    hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 0;
    }
    
    do {
        /* Skip . and .. */
        if (lstrcmpA(findData.cFileName, ".") == 0 || 
            lstrcmpA(findData.cFileName, "..") == 0) {
            continue;
        }
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* Recursively count files in subdirectory */
            char subPath[MAX_PATH];
            wsprintfA(subPath, "%s\\%s", folderPath, findData.cFileName);
            count += CountFilesInFolder(subPath);
            count++; /* Count the directory itself */
        } else {
            count++;
        }
    } while (FindNextFileA(hFind, &findData));
    
    FindClose(hFind);
    return count;
}

/* Helper: Send folder entries recursively */
static int SendFolderEntries(PRD2K_NETWORK pNet, const char *basePath, const char *relativePath)
{
    WIN32_FIND_DATAA findData;
    HANDLE hFind;
    char searchPath[MAX_PATH];
    char fullPath[MAX_PATH];
    char entryRelPath[MAX_PATH];
    RD2K_FOLDER_ENTRY entry;
    LARGE_INTEGER liFileSize;
    int result;
    
    /* Build search path */
    if (relativePath[0]) {
        wsprintfA(searchPath, "%s\\%s\\*", basePath, relativePath);
    } else {
        wsprintfA(searchPath, "%s\\*", basePath);
    }
    
    hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return FT_SUCCESS; /* Empty folder is OK */
    }
    
    do {
        /* Skip . and .. */
        if (lstrcmpA(findData.cFileName, ".") == 0 || 
            lstrcmpA(findData.cFileName, "..") == 0) {
            continue;
        }
        
        /* Check for cancel */
        if (g_ftContext.bCancelRequested || Progress_IsCancelled()) {
            FindClose(hFind);
            return FT_ERR_CANCELLED;
        }
        
        /* Build paths */
        if (relativePath[0]) {
            wsprintfA(entryRelPath, "%s\\%s", relativePath, findData.cFileName);
            wsprintfA(fullPath, "%s\\%s\\%s", basePath, relativePath, findData.cFileName);
        } else {
            lstrcpynA(entryRelPath, findData.cFileName, sizeof(entryRelPath));
            wsprintfA(fullPath, "%s\\%s", basePath, findData.cFileName);
        }
        
        /* Prepare entry */
        ZeroMemory(&entry, sizeof(entry));
        lstrcpynA(entry.relativePath, entryRelPath, sizeof(entry.relativePath));
        entry.attributes = findData.dwFileAttributes;
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* Directory entry - attributes already set with FILE_ATTRIBUTE_DIRECTORY */
            entry.fileSizeHigh = 0;
            entry.fileSizeLow = 0;
            
            /* Send directory entry */
            result = Network_SendPacket(pNet, MSG_FOLDER_ENTRY, (const BYTE*)&entry, sizeof(entry));
            if (result != RD2K_SUCCESS) {
                FindClose(hFind);
                return FT_ERR_NETWORK;
            }
            
            /* Recursively send contents */
            result = SendFolderEntries(pNet, basePath, entryRelPath);
            if (result != FT_SUCCESS) {
                FindClose(hFind);
                return result;
            }
        } else {
            /* File entry - get size */
            HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                if (GetFileSizeEx(hFile, &liFileSize)) {
                    entry.fileSizeHigh = liFileSize.HighPart;
                    entry.fileSizeLow = liFileSize.LowPart;
                }
                CloseHandle(hFile);
            }
            /* attributes already set - FILE_ATTRIBUTE_DIRECTORY not present = file */
            
            /* Send file entry */
            result = Network_SendPacket(pNet, MSG_FOLDER_ENTRY, (const BYTE*)&entry, sizeof(entry));
            if (result != RD2K_SUCCESS) {
                FindClose(hFind);
                return FT_ERR_NETWORK;
            }
            
            /* Now send the actual file data - use chunked transfer */
            /* For now, we'll send the file contents inline */
            {
                HANDLE hFileRead;
                BYTE *fileBuffer;
                DWORD bytesRead;
                ULONGLONG fileSize = ((ULONGLONG)entry.fileSizeHigh << 32) | entry.fileSizeLow;
                ULONGLONG totalSent = 0;
                RD2K_FILE_CHUNK chunkHeader;
                BYTE *sendBuffer;
                DWORD chunkIndex = 0;
                
                if (fileSize > 0 && fileSize <= FT_MAX_FILE_SIZE) {
                    hFileRead = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFileRead != INVALID_HANDLE_VALUE) {
                        fileBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, FT_CHUNK_SIZE);
                        sendBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, sizeof(RD2K_FILE_CHUNK) + FT_CHUNK_SIZE);
                        
                        if (fileBuffer && sendBuffer) {
                            while (totalSent < fileSize) {
                                DWORD chunkSize = (DWORD)((fileSize - totalSent > FT_CHUNK_SIZE) ? FT_CHUNK_SIZE : (fileSize - totalSent));
                                
                                if (!ReadFile(hFileRead, fileBuffer, chunkSize, &bytesRead, NULL) || bytesRead != chunkSize) {
                                    break;
                                }
                                
                                chunkHeader.chunkIndex = chunkIndex++;
                                chunkHeader.chunkSize = chunkSize;
                                
                                CopyMemory(sendBuffer, &chunkHeader, sizeof(RD2K_FILE_CHUNK));
                                CopyMemory(sendBuffer + sizeof(RD2K_FILE_CHUNK), fileBuffer, chunkSize);
                                
                                result = Network_SendPacket(pNet, MSG_FILE_DATA, sendBuffer, sizeof(RD2K_FILE_CHUNK) + chunkSize);
                                if (result != RD2K_SUCCESS) {
                                    break;
                                }
                                
                                totalSent += chunkSize;
                                g_ftContext.bytesTransferred += chunkSize;
                                Progress_Update64(g_ftContext.bytesTransferred);
                                
                                /* Flow control */
                                if ((chunkIndex % FT_ACK_INTERVAL) == 0) {
                                    Sleep(10);
                                }
                            }
                        }
                        
                        if (fileBuffer) HeapFree(GetProcessHeap(), 0, fileBuffer);
                        if (sendBuffer) HeapFree(GetProcessHeap(), 0, sendBuffer);
                        CloseHandle(hFileRead);
                    }
                }
            }
        }
        
    } while (FindNextFileA(hFind, &findData));
    
    FindClose(hFind);
    return FT_SUCCESS;
}

/* Helper: Calculate total size of folder */
static ULONGLONG CalculateFolderSize(const char *folderPath)
{
    WIN32_FIND_DATAA findData;
    HANDLE hFind;
    char searchPath[MAX_PATH];
    ULONGLONG totalSize = 0;
    
    wsprintfA(searchPath, "%s\\*", folderPath);
    
    hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return 0;
    }
    
    do {
        if (lstrcmpA(findData.cFileName, ".") == 0 || 
            lstrcmpA(findData.cFileName, "..") == 0) {
            continue;
        }
        
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char subPath[MAX_PATH];
            wsprintfA(subPath, "%s\\%s", folderPath, findData.cFileName);
            totalSize += CalculateFolderSize(subPath);
        } else {
            totalSize += ((ULONGLONG)findData.nFileSizeHigh << 32) | findData.nFileSizeLow;
        }
    } while (FindNextFileA(hFind, &findData));
    
    FindClose(hFind);
    return totalSize;
}

/* Send a folder to remote (synchronous - called from worker thread) */
int FileTransfer_SendFolder(PRD2K_NETWORK pNet, const char *folderPath, BOOL bShowProgress)
{
    const char *folderName;
    RD2K_FOLDER_HEADER folderHeader;
    ULONGLONG totalSize;
    DWORD fileCount;
    int result;
    
    if (!pNet || !folderPath) {
        SetLastFTError("Invalid parameters");
        return FT_ERR_NETWORK;
    }
    
    if (FileTransfer_IsBusy()) {
        SetLastFTError("Transfer already in progress");
        return FT_ERR_BUSY;
    }
    
    /* Get folder name from path */
    folderName = strrchr(folderPath, '\\');
    if (folderName) {
        folderName++;
    } else {
        folderName = folderPath;
    }
    
    /* Calculate total size and file count */
    totalSize = CalculateFolderSize(folderPath);
    fileCount = CountFilesInFolder(folderPath);
    
    if (totalSize > FT_MAX_FILE_SIZE) {
        SetLastFTError("Folder too large (max 100GB total)");
        return FT_ERR_FILE_TOO_LARGE;
    }
    
    /* Setup context */
    g_ftContext.state = FT_STATE_SENDING_FOLDER;
    g_ftContext.fileSize = totalSize;
    g_ftContext.bytesTransferred = 0;
    g_ftContext.bShowProgress = bShowProgress;
    g_ftContext.bCancelRequested = FALSE;
    g_ftContext.pNet = pNet;
    lstrcpynA(g_ftContext.fileName, folderName, sizeof(g_ftContext.fileName));
    lstrcpynA(g_ftContext.filePath, folderPath, sizeof(g_ftContext.filePath));
    
    /* Show progress */
    if (bShowProgress) {
        Input_ReleaseAllModifiers();
        Progress_Show64(g_ftContext.hNotifyWnd, folderName, totalSize, TRUE);
    }
    
    /* Prepare folder header */
    ZeroMemory(&folderHeader, sizeof(folderHeader));
    lstrcpynA(folderHeader.folderName, folderName, sizeof(folderHeader.folderName));
    folderHeader.totalSizeHigh = (DWORD)(totalSize >> 32);
    folderHeader.totalSizeLow = (DWORD)(totalSize & 0xFFFFFFFF);
    folderHeader.totalFiles = fileCount;
    
    /* Send folder start */
    result = Network_SendPacket(pNet, MSG_FOLDER_START, (const BYTE*)&folderHeader, sizeof(folderHeader));
    if (result != RD2K_SUCCESS) {
        g_ftContext.state = FT_STATE_IDLE;
        Progress_Hide();
        SetLastFTError("Network error sending folder header");
        return FT_ERR_NETWORK;
    }
    
    /* Send folder entries recursively */
    result = SendFolderEntries(pNet, folderPath, "");
    if (result != FT_SUCCESS) {
        g_ftContext.state = FT_STATE_IDLE;
        Progress_Hide();
        return result;
    }
    
    /* Send folder end */
    Sleep(50);
    result = Network_SendPacket(pNet, MSG_FOLDER_END, NULL, 0);
    if (result != RD2K_SUCCESS) {
        g_ftContext.state = FT_STATE_IDLE;
        Progress_Hide();
        SetLastFTError("Network error completing folder transfer");
        return FT_ERR_NETWORK;
    }
    
    /* Complete */
    Progress_Hide();
    g_ftContext.state = FT_STATE_IDLE;
    PostMessage(g_ftContext.hNotifyWnd, WM_FT_COMPLETE, FT_SUCCESS, (LPARAM)1);
    
    SetLastFTError(NULL);
    return FT_SUCCESS;
}

/* ============================================================================
 * SYNCHRONOUS FILE TRANSFER (for server-side use)
 * ============================================================================
 * These functions send files WITHOUT using a worker thread, which is required
 * when sending from the server (being controlled) back to the client (viewer).
 * Using a worker thread on the server side causes socket access from multiple
 * threads which leads to disconnection.
 */

/* Send a single file SYNCHRONOUSLY - blocks until complete */
int FileTransfer_SendFileSync(PRD2K_NETWORK pNet, const char *filePath, HWND hNotifyWnd)
{
    HANDLE hFile;
    LARGE_INTEGER liFileSize;
    ULONGLONG fileSize;
    DWORD bytesRead;
    ULONGLONG totalSent;
    BYTE *chunkBuffer;
    RD2K_FILE_HEADER fileHeader;
    RD2K_FILE_CHUNK chunkHeader;
    BYTE *sendBuffer;
    DWORD chunkIndex;
    DWORD totalChunks;
    int result;
    const char *fileName;
    
    if (!pNet || !filePath) {
        SetLastFTError("Invalid parameters");
        return FT_ERR_NETWORK;
    }
    
    if (FileTransfer_IsBusy()) {
        SetLastFTError("Transfer already in progress");
        return FT_ERR_BUSY;
    }
    
    /* Check if this is a directory */
    if (FileTransfer_IsDirectory(filePath)) {
        /* For now, skip directories in sync mode */
        SetLastFTError("Folder transfer not supported in sync mode");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Get filename from path */
    fileName = strrchr(filePath, '\\');
    if (fileName) {
        fileName++;
    } else {
        fileName = filePath;
    }
    
    /* Open file for reading */
    hFile = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        SetLastFTError("Cannot open file");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Get file size using 64-bit API */
    if (!GetFileSizeEx(hFile, &liFileSize)) {
        CloseHandle(hFile);
        SetLastFTError("Cannot get file size");
        return FT_ERR_READ;
    }
    
    fileSize = (ULONGLONG)liFileSize.QuadPart;
    
    if (fileSize == 0) {
        CloseHandle(hFile);
        SetLastFTError("File is empty");
        return FT_ERR_READ;
    }
    
    if (fileSize > FT_MAX_FILE_SIZE) {
        CloseHandle(hFile);
        SetLastFTError("File too large (max 100GB)");
        return FT_ERR_FILE_TOO_LARGE;
    }
    
    totalChunks = (DWORD)((fileSize + FT_CHUNK_SIZE - 1) / FT_CHUNK_SIZE);
    
    /* Mark as busy */
    g_ftContext.state = FT_STATE_SENDING;
    g_ftContext.fileSize = fileSize;
    g_ftContext.bytesTransferred = 0;
    g_ftContext.totalChunks = totalChunks;
    lstrcpynA(g_ftContext.fileName, fileName, sizeof(g_ftContext.fileName));
    lstrcpynA(g_ftContext.filePath, filePath, sizeof(g_ftContext.filePath));
    
    /* Allocate buffers */
    chunkBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, FT_CHUNK_SIZE);
    if (!chunkBuffer) {
        CloseHandle(hFile);
        g_ftContext.state = FT_STATE_IDLE;
        SetLastFTError("Out of memory");
        return FT_ERR_MEMORY;
    }
    
    sendBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, sizeof(RD2K_FILE_CHUNK) + FT_CHUNK_SIZE);
    if (!sendBuffer) {
        HeapFree(GetProcessHeap(), 0, chunkBuffer);
        CloseHandle(hFile);
        g_ftContext.state = FT_STATE_IDLE;
        SetLastFTError("Out of memory for send buffer");
        return FT_ERR_MEMORY;
    }
    
    /* Show progress dialog */
    Input_ReleaseAllModifiers();
    Progress_Show64(hNotifyWnd, fileName, fileSize, TRUE);
    
    /* Prepare and send file header */
    ZeroMemory(&fileHeader, sizeof(fileHeader));
    lstrcpynA(fileHeader.fileName, fileName, sizeof(fileHeader.fileName) - 1);
    fileHeader.fileSizeLow = (DWORD)(fileSize & 0xFFFFFFFF);
    fileHeader.fileSizeHigh = (DWORD)(fileSize >> 32);
    fileHeader.fileCount = 1;
    fileHeader.totalChunks = totalChunks;
    
    result = Network_SendPacket(pNet, MSG_FILE_START, (const BYTE*)&fileHeader, sizeof(fileHeader));
    if (result != RD2K_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, sendBuffer);
        HeapFree(GetProcessHeap(), 0, chunkBuffer);
        CloseHandle(hFile);
        Progress_Hide();
        g_ftContext.state = FT_STATE_IDLE;
        SetLastFTError("Network error sending header");
        return FT_ERR_NETWORK;
    }
    
    /* Send file data in chunks */
    totalSent = 0;
    chunkIndex = 0;
    
    while (totalSent < fileSize) {
        DWORD chunkSize = (DWORD)((fileSize - totalSent > FT_CHUNK_SIZE) ? FT_CHUNK_SIZE : (fileSize - totalSent));
        
        /* Check for cancel */
        if (Progress_IsCancelled()) {
            HeapFree(GetProcessHeap(), 0, sendBuffer);
            HeapFree(GetProcessHeap(), 0, chunkBuffer);
            CloseHandle(hFile);
            Progress_Hide();
            g_ftContext.state = FT_STATE_IDLE;
            SetLastFTError("Transfer cancelled");
            return FT_ERR_CANCELLED;
        }
        
        /* Read chunk from file */
        if (!ReadFile(hFile, chunkBuffer, chunkSize, &bytesRead, NULL) || bytesRead != chunkSize) {
            HeapFree(GetProcessHeap(), 0, sendBuffer);
            HeapFree(GetProcessHeap(), 0, chunkBuffer);
            CloseHandle(hFile);
            Progress_Hide();
            g_ftContext.state = FT_STATE_IDLE;
            SetLastFTError("Failed to read file");
            return FT_ERR_READ;
        }
        
        /* Prepare chunk header */
        chunkHeader.chunkIndex = chunkIndex;
        chunkHeader.chunkSize = chunkSize;
        
        /* Copy header and data to send buffer */
        CopyMemory(sendBuffer, &chunkHeader, sizeof(RD2K_FILE_CHUNK));
        CopyMemory(sendBuffer + sizeof(RD2K_FILE_CHUNK), chunkBuffer, chunkSize);
        
        /* Send chunk with retry */
        {
            int retryCount = 0;
            const int maxRetries = 5;
            
            while (retryCount < maxRetries) {
                result = Network_SendPacket(pNet, MSG_FILE_DATA, sendBuffer, sizeof(RD2K_FILE_CHUNK) + chunkSize);
                if (result == RD2K_SUCCESS) {
                    break;
                }
                retryCount++;
                if (retryCount < maxRetries) {
                    Sleep(100 * retryCount);
                }
            }
            
            if (result != RD2K_SUCCESS) {
                HeapFree(GetProcessHeap(), 0, sendBuffer);
                HeapFree(GetProcessHeap(), 0, chunkBuffer);
                CloseHandle(hFile);
                Progress_Hide();
                g_ftContext.state = FT_STATE_IDLE;
                SetLastFTError("Network error sending data");
                return FT_ERR_NETWORK;
            }
        }
        
        totalSent += chunkSize;
        g_ftContext.bytesTransferred = totalSent;
        chunkIndex++;
        
        /* Update progress */
        Progress_Update64(totalSent);
        
        /* Flow control - IMPORTANT: Give Windows message pump time to process */
        /* This is critical for preventing UI freeze and socket issues */
        if (fileSize > (ULONGLONG)100 * 1024 * 1024) {
            if ((chunkIndex % 4) == 0) {
                Sleep(30);
            }
        } else if (fileSize > (ULONGLONG)10 * 1024 * 1024) {
            if ((chunkIndex % 8) == 0) {
                Sleep(20);
            }
        } else {
            if ((chunkIndex % 16) == 0) {
                Sleep(5);
            }
        }
        
        /* Process Windows messages to keep UI responsive */
        {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }
    
    /* Cleanup */
    HeapFree(GetProcessHeap(), 0, sendBuffer);
    HeapFree(GetProcessHeap(), 0, chunkBuffer);
    CloseHandle(hFile);
    
    /* Delay before end marker */
    {
        DWORD delayMs;
        if (fileSize > (ULONGLONG)100 * 1024 * 1024) {
            delayMs = 500;
        } else if (fileSize > (ULONGLONG)10 * 1024 * 1024) {
            delayMs = 200;
        } else {
            delayMs = 100;
        }
        Sleep(delayMs);
    }
    
    /* Send file end marker */
    result = Network_SendPacket(pNet, MSG_FILE_END, NULL, 0);
    
    Progress_Hide();
    g_ftContext.state = FT_STATE_IDLE;
    
    if (result != RD2K_SUCCESS) {
        SetLastFTError("Network error sending end marker");
        return FT_ERR_NETWORK;
    }
    
    SetLastFTError(NULL);
    return FT_SUCCESS;
}

/* Send files from clipboard to remote SYNCHRONOUSLY */
int FileTransfer_SendClipboardFilesSync(PRD2K_NETWORK pNet, HWND hClipOwner, HWND hNotifyWnd)
{
    HANDLE hDrop;
    UINT fileCount;
    UINT i;
    char filePath[MAX_PATH];
    DWORD attrs;
    int result = FT_SUCCESS;
    
    if (!pNet) {
        SetLastFTError("Invalid parameters");
        return FT_ERR_NETWORK;
    }
    
    /* Release modifier keys before clipboard operations */
    Input_ReleaseAllModifiers();
    
    /* Open clipboard */
    if (!OpenClipboard(hClipOwner)) {
        SetLastFTError("Cannot open clipboard");
        return FT_ERR_NETWORK;
    }
    
    /* Get file drop handle */
    hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) {
        CloseClipboard();
        SetLastFTError("No files in clipboard");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Count files */
    fileCount = DragQueryFileA((HDROP)hDrop, 0xFFFFFFFF, NULL, 0);
    if (fileCount == 0) {
        CloseClipboard();
        SetLastFTError("No files in clipboard");
        return FT_ERR_FILE_NOT_FOUND;
    }
    
    /* Send the first file synchronously */
    for (i = 0; i < fileCount && i < 1; i++) {
        if (DragQueryFileA((HDROP)hDrop, i, filePath, sizeof(filePath))) {
            attrs = GetFileAttributesA(filePath);
            if (attrs != 0xFFFFFFFF && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                CloseClipboard();
                result = FileTransfer_SendFileSync(pNet, filePath, hNotifyWnd);
                return result;
            }
        }
    }
    
    CloseClipboard();
    SetLastFTError("No valid files in clipboard");
    return FT_ERR_FILE_NOT_FOUND;
}
