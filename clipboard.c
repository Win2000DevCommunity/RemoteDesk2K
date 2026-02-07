/*
 * RemoteDesk2K - Clipboard Module Implementation
 * Windows 2000 compatible clipboard synchronization
 * 
 * Works like AnyDesk:
 * - Ctrl+C in viewer: Sends Ctrl+C to remote, then syncs remote clipboard to local
 * - Ctrl+V in viewer: Syncs local clipboard to remote, then sends Ctrl+V
 * - Files paste to currently selected location on remote
 * 
 * IMPORTANT: This module is careful about clipboard ownership.
 * OpenClipboard can steal ownership from Explorer, which can cause
 * issues with drag-drop and multi-select. We minimize clipboard
 * open time and release modifier keys before operations.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include "clipboard.h"
#include "common.h"
#include "filetransfer.h"
#include "input.h"

/* Global clipboard state */
static RD2K_CLIPBOARD_STATE g_clipState = {0};

/* GetClipboardSequenceNumber - may not exist on all Win2K */
typedef DWORD (WINAPI *PFN_GetClipboardSequenceNumber)(void);
static PFN_GetClipboardSequenceNumber pfnGetClipboardSeqNum = NULL;
static BOOL g_bSeqNumChecked = FALSE;

/* Get clipboard sequence number (with fallback) */
static DWORD GetClipSeqNum(void)
{
    HMODULE hUser32;
    
    if (!g_bSeqNumChecked) {
        hUser32 = GetModuleHandleA("user32.dll");
        if (hUser32) {
            pfnGetClipboardSeqNum = (PFN_GetClipboardSequenceNumber)
                GetProcAddress(hUser32, "GetClipboardSequenceNumber");
        }
        g_bSeqNumChecked = TRUE;
    }
    
    if (pfnGetClipboardSeqNum) {
        return pfnGetClipboardSeqNum();
    }
    
    /* Fallback: use tick count (less reliable but works) */
    return GetTickCount();
}

/* Initialize clipboard module */
BOOL Clipboard_Init(HWND hOwnerWnd)
{
    ZeroMemory(&g_clipState, sizeof(g_clipState));
    g_clipState.hOwnerWnd = hOwnerWnd;
    g_clipState.bEnabled = TRUE;
    g_clipState.lastLocalSeq = GetClipSeqNum();
    return TRUE;
}

/* Cleanup clipboard module */
void Clipboard_Cleanup(void)
{
    g_clipState.hOwnerWnd = NULL;
    g_clipState.bEnabled = FALSE;
}

/* Enable/disable clipboard sync */
void Clipboard_SetEnabled(BOOL bEnabled)
{
    g_clipState.bEnabled = bEnabled;
}

/* Check if clipboard sync is enabled */
BOOL Clipboard_IsEnabled(void)
{
    return g_clipState.bEnabled;
}

/* Get current clipboard type */
int Clipboard_GetLocalType(void)
{
    int type = CLIP_TYPE_NONE;
    
    /* CRITICAL: Release all modifier keys before opening clipboard.
       This prevents the "stuck Ctrl key" problem that causes
       Explorer to think Ctrl is held (multi-select mode). */
    Input_ReleaseAllModifiers();
    
    if (OpenClipboard(g_clipState.hOwnerWnd)) {
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            type = CLIP_TYPE_FILES;
        } else if (IsClipboardFormatAvailable(CF_TEXT) || 
                   IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            type = CLIP_TYPE_TEXT;
        } else if (IsClipboardFormatAvailable(CF_DIB) ||
                   IsClipboardFormatAvailable(CF_BITMAP)) {
            type = CLIP_TYPE_IMAGE;
        }
        CloseClipboard();
    }
    
    return type;
}

/* Check if we have text in clipboard */
BOOL Clipboard_HasText(void)
{
    return IsClipboardFormatAvailable(CF_TEXT) || 
           IsClipboardFormatAvailable(CF_UNICODETEXT);
}

/* Check if we have files in clipboard */
BOOL Clipboard_HasFiles(void)
{
    return IsClipboardFormatAvailable(CF_HDROP);
}

/* Handle Ctrl+C in viewer window */
/* This is called when user presses Ctrl+C while viewer is active */
/* The Ctrl+C is sent to remote, then we request clipboard TEXT/metadata from remote */
/* NOTE: We do NOT automatically transfer files here - user must explicitly request */
/* file transfer using Ctrl+Shift+V or menu option after navigating to destination folder */
void Clipboard_HandleCopy(PRD2K_NETWORK pNet)
{
    if (!pNet || !g_clipState.bEnabled) return;
    
    /* Mark that we're expecting clipboard from remote */
    g_clipState.pendingRequest = CLIP_SYNC_FROM_REMOTE;
    
    /* Request clipboard TEXT/metadata from remote (NOT file transfer) */
    /* This syncs file paths for display, but doesn't transfer actual files */
    Clipboard_RequestFromRemote(pNet);
    
    /* DO NOT request file transfer here - user needs to:
       1. Switch to Host desktop
       2. Navigate to destination folder in Explorer  
       3. Press Ctrl+Shift+V (or use menu) to receive files from remote */
}

/* Handle Ctrl+V in viewer window */
/* This is called when user presses Ctrl+V while viewer is active */
/* We sync local clipboard to remote, then Ctrl+V is sent to paste */
void Clipboard_HandlePaste(PRD2K_NETWORK pNet)
{
    if (!pNet || !g_clipState.bEnabled) return;
    
    /* Sync local clipboard to remote before paste */
    Clipboard_SendToRemote(pNet);
    
    /* The actual Ctrl+V key will be sent by the caller after this */
}

/* Send local clipboard to remote */
BOOL Clipboard_SendToRemote(PRD2K_NETWORK pNet)
{
    int clipType;
    BOOL result = FALSE;
    
    if (!pNet || !g_clipState.bEnabled || g_clipState.bSyncing) {
        return FALSE;
    }
    
    g_clipState.bSyncing = TRUE;
    
    clipType = Clipboard_GetLocalType();
    
    switch (clipType) {
        case CLIP_TYPE_TEXT:
            result = Clipboard_SendText(pNet);
            break;
        case CLIP_TYPE_FILES:
            result = Clipboard_SendFileList(pNet);
            break;
        case CLIP_TYPE_IMAGE:
            /* TODO: Image clipboard support */
            result = FALSE;
            break;
        default:
            result = FALSE;
            break;
    }
    
    g_clipState.bSyncing = FALSE;
    return result;
}

/* Request clipboard from remote */
BOOL Clipboard_RequestFromRemote(PRD2K_NETWORK pNet)
{
    if (!pNet || !g_clipState.bEnabled) {
        return FALSE;
    }
    
    return Network_SendPacket(pNet, MSG_CLIPBOARD_REQ, NULL, 0);
}

/* Receive clipboard data from remote */
BOOL Clipboard_ReceiveFromRemote(const BYTE *data, DWORD length)
{
    RD2K_CLIPBOARD *pClip;
    
    if (!data || length < sizeof(RD2K_CLIPBOARD) || !g_clipState.bEnabled) {
        return FALSE;
    }
    
    if (g_clipState.bSyncing) {
        return FALSE;  /* Prevent loops */
    }
    
    pClip = (RD2K_CLIPBOARD*)data;
    
    if (pClip->isFile) {
        return Clipboard_ReceiveFileList(data, length);
    } else {
        return Clipboard_ReceiveText(data, length);
    }
}

/* Send text clipboard to remote */
BOOL Clipboard_SendText(PRD2K_NETWORK pNet)
{
    HGLOBAL hMem;
    char *pText;
    DWORD textLen;
    BYTE *buffer;
    RD2K_CLIPBOARD clipHeader;
    BOOL result = FALSE;
    
    if (!pNet) return FALSE;
    
    /* Release modifier keys to prevent stuck keys */
    Input_ReleaseAllModifiers();
    
    if (!OpenClipboard(g_clipState.hOwnerWnd)) {
        return FALSE;
    }
    
    hMem = GetClipboardData(CF_TEXT);
    if (!hMem) {
        CloseClipboard();
        return FALSE;
    }
    
    pText = (char*)GlobalLock(hMem);
    if (!pText) {
        CloseClipboard();
        return FALSE;
    }
    
    textLen = lstrlenA(pText);
    
    /* Limit clipboard size - increased to 10MB for large text content */
    if (textLen > 10 * 1024 * 1024) {  /* 10MB max */
        textLen = 10 * 1024 * 1024;
    }
    
    /* Allocate buffer for header + text */
    buffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, sizeof(RD2K_CLIPBOARD) + textLen);
    if (buffer) {
        ZeroMemory(&clipHeader, sizeof(clipHeader));
        clipHeader.isFile = FALSE;
        clipHeader.dataLength = textLen;
        
        CopyMemory(buffer, &clipHeader, sizeof(RD2K_CLIPBOARD));
        CopyMemory(buffer + sizeof(RD2K_CLIPBOARD), pText, textLen);
        
        result = Network_SendPacket(pNet, MSG_CLIPBOARD_TEXT, buffer, 
                                    sizeof(RD2K_CLIPBOARD) + textLen);
        
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    
    GlobalUnlock(hMem);
    CloseClipboard();
    
    g_clipState.lastLocalSeq = GetClipSeqNum();
    
    return result;
}

/* Send file list clipboard to remote - sends file PATHS only, NOT actual content */
/* This allows the remote side to know what files are available in clipboard. */
/* Actual file transfer only happens when MSG_FILE_REQ is received. */
BOOL Clipboard_SendFileList(PRD2K_NETWORK pNet)
{
    HGLOBAL hDrop;
    HDROP hDropInfo;
    UINT fileCount;
    UINT i;
    char filePath[MAX_PATH];
    BYTE *buffer;
    DWORD bufferSize;
    DWORD offset;
    RD2K_CLIPBOARD clipHeader;
    BOOL result = FALSE;
    
    if (!pNet) return FALSE;
    
    /* Release modifier keys before opening clipboard */
    Input_ReleaseAllModifiers();
    
    if (!OpenClipboard(g_clipState.hOwnerWnd)) {
        return FALSE;
    }
    
    hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) {
        CloseClipboard();
        return FALSE;
    }
    
    hDropInfo = (HDROP)hDrop;
    fileCount = DragQueryFileA(hDropInfo, 0xFFFFFFFF, NULL, 0);
    
    if (fileCount == 0) {
        CloseClipboard();
        return FALSE;
    }
    
    /* Calculate buffer size for file paths */
    bufferSize = sizeof(RD2K_CLIPBOARD) + sizeof(UINT);  /* Header + file count */
    for (i = 0; i < fileCount && i < 100; i++) {  /* Limit to 100 files */
        if (DragQueryFileA(hDropInfo, i, filePath, MAX_PATH)) {
            bufferSize += lstrlenA(filePath) + 1;
        }
    }
    
    /* Allocate buffer */
    buffer = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bufferSize);
    if (!buffer) {
        CloseClipboard();
        return FALSE;
    }
    
    /* Build packet: header + fileCount + null-terminated paths */
    ZeroMemory(&clipHeader, sizeof(clipHeader));
    clipHeader.isFile = TRUE;
    clipHeader.dataLength = bufferSize - sizeof(RD2K_CLIPBOARD);
    
    CopyMemory(buffer, &clipHeader, sizeof(RD2K_CLIPBOARD));
    offset = sizeof(RD2K_CLIPBOARD);
    
    /* Store file count */
    CopyMemory(buffer + offset, &fileCount, sizeof(UINT));
    offset += sizeof(UINT);
    
    /* Store file paths */
    for (i = 0; i < fileCount && i < 100; i++) {
        if (DragQueryFileA(hDropInfo, i, filePath, MAX_PATH)) {
            DWORD pathLen = lstrlenA(filePath) + 1;
            CopyMemory(buffer + offset, filePath, pathLen);
            offset += pathLen;
        }
    }
    
    CloseClipboard();
    
    /* Send file list (paths only) - NOT actual file data */
    result = Network_SendPacket(pNet, MSG_CLIPBOARD_FILES, buffer, offset);
    
    HeapFree(GetProcessHeap(), 0, buffer);
    g_clipState.lastLocalSeq = GetClipSeqNum();
    
    return result;
}

/* Receive text clipboard from remote */
BOOL Clipboard_ReceiveText(const BYTE *data, DWORD length)
{
    RD2K_CLIPBOARD *pClip;
    const char *pText;
    HGLOBAL hMem;
    char *pDst;
    
    if (!data || length < sizeof(RD2K_CLIPBOARD)) {
        return FALSE;
    }
    
    pClip = (RD2K_CLIPBOARD*)data;
    if (pClip->isFile) {
        return FALSE;  /* Not text */
    }
    
    /* Validate data length - make sure we have enough data */
    if (pClip->dataLength == 0 || 
        pClip->dataLength > length - sizeof(RD2K_CLIPBOARD) ||
        pClip->dataLength > 10 * 1024 * 1024) {  /* Max 10MB */
        return FALSE;
    }
    
    pText = (const char*)(data + sizeof(RD2K_CLIPBOARD));
    
    g_clipState.bSyncing = TRUE;
    
    /* Release modifier keys before clipboard operations */
    Input_ReleaseAllModifiers();
    
    if (OpenClipboard(g_clipState.hOwnerWnd)) {
        EmptyClipboard();
        
        hMem = GlobalAlloc(GMEM_MOVEABLE, pClip->dataLength + 1);
        if (hMem) {
            pDst = (char*)GlobalLock(hMem);
            if (pDst) {
                CopyMemory(pDst, pText, pClip->dataLength);
                pDst[pClip->dataLength] = '\0';
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            } else {
                GlobalFree(hMem);
            }
        }
        
        CloseClipboard();
        g_clipState.lastLocalSeq = GetClipSeqNum();
    }
    
    g_clipState.bSyncing = FALSE;
    return TRUE;
}

/* Receive file list from remote and set to clipboard */
/* When user pastes, files will be copied to selected location */
BOOL Clipboard_ReceiveFileList(const BYTE *data, DWORD length)
{
    RD2K_CLIPBOARD *pClip;
    const char *pFileList;
    HGLOBAL hMem;
    DROPFILES *pDropFiles;
    DWORD totalSize;
    
    if (!data || length < sizeof(RD2K_CLIPBOARD)) {
        return FALSE;
    }
    
    pClip = (RD2K_CLIPBOARD*)data;
    if (!pClip->isFile) {
        return FALSE;  /* Not files */
    }
    
    /* Validate data length */
    if (pClip->dataLength == 0 ||
        pClip->dataLength > length - sizeof(RD2K_CLIPBOARD) ||
        pClip->dataLength > 10 * 1024 * 1024) {  /* Max 10MB for file paths */
        return FALSE;
    }
    
    pFileList = (const char*)(data + sizeof(RD2K_CLIPBOARD));
    
    g_clipState.bSyncing = TRUE;
    
    /* Release modifier keys before clipboard operations */
    Input_ReleaseAllModifiers();
    
    /* Create DROPFILES structure for clipboard */
    totalSize = sizeof(DROPFILES) + pClip->dataLength;
    
    if (OpenClipboard(g_clipState.hOwnerWnd)) {
        EmptyClipboard();
        
        hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize);
        if (hMem) {
            pDropFiles = (DROPFILES*)GlobalLock(hMem);
            if (pDropFiles) {
                pDropFiles->pFiles = sizeof(DROPFILES);
                pDropFiles->pt.x = 0;
                pDropFiles->pt.y = 0;
                pDropFiles->fNC = FALSE;
                pDropFiles->fWide = FALSE;  /* ANSI strings */
                
                CopyMemory((char*)pDropFiles + sizeof(DROPFILES), 
                          pFileList, pClip->dataLength);
                
                GlobalUnlock(hMem);
                SetClipboardData(CF_HDROP, hMem);
            } else {
                GlobalFree(hMem);
            }
        }
        
        CloseClipboard();
        g_clipState.lastLocalSeq = GetClipSeqNum();
    }
    
    g_clipState.bSyncing = FALSE;
    return TRUE;
}

/* Transfer files from local clipboard to remote */
/* Transfer files from local clipboard to remote - sends actual file CONTENT */
/* NOTE: Uses SYNC transfer to avoid socket race conditions. Only sends FIRST file. */
BOOL Clipboard_TransferFiles(PRD2K_NETWORK pNet)
{
    HGLOBAL hDrop;
    HDROP hDropInfo;
    UINT fileCount;
    UINT i;
    char filePath[MAX_PATH];
    int result = FT_ERR_FILE_NOT_FOUND;
    DWORD attrs;
    
    if (!pNet) return FALSE;
    
    /* Check if already transferring */
    if (FileTransfer_IsBusy()) {
        return FALSE;
    }
    
    /* Release modifier keys before clipboard operations */
    Input_ReleaseAllModifiers();
    
    if (!OpenClipboard(g_clipState.hOwnerWnd)) {
        return FALSE;
    }
    
    hDrop = GetClipboardData(CF_HDROP);
    if (!hDrop) {
        CloseClipboard();
        return FALSE;
    }
    
    hDropInfo = (HDROP)hDrop;
    fileCount = DragQueryFileA(hDropInfo, 0xFFFFFFFF, NULL, 0);
    
    if (fileCount == 0) {
        CloseClipboard();
        return FALSE;
    }
    
    /* Find first valid file and transfer it */
    for (i = 0; i < fileCount; i++) {
        if (DragQueryFileA(hDropInfo, i, filePath, MAX_PATH)) {
            attrs = GetFileAttributesA(filePath);
            if (attrs != 0xFFFFFFFF && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                /* Found a valid file - transfer it SYNC to avoid socket race */
                CloseClipboard();
                result = FileTransfer_SendFileSync(pNet, filePath, g_clipState.hOwnerWnd);
                return (result == FT_SUCCESS);
            }
        }
    }
    
    CloseClipboard();
    return FALSE;
}
