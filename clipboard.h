/*
 * RemoteDesk2K - Clipboard Module
 * Windows 2000 compatible clipboard synchronization
 * 
 * Behavior (like AnyDesk):
 * - Ctrl+C in viewer: Sends Ctrl+C to remote, then syncs remote clipboard to local
 * - Ctrl+V in viewer: Syncs local clipboard to remote, then sends Ctrl+V
 * - Files paste to selected location on remote (desktop, folder, etc.)
 * - Outside viewer: Normal local clipboard operations
 */

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <windows.h>
#include "network.h"

/* Clipboard data types */
#define CLIP_TYPE_NONE      0
#define CLIP_TYPE_TEXT      1
#define CLIP_TYPE_FILES     2
#define CLIP_TYPE_IMAGE     3

/* Clipboard sync direction */
#define CLIP_SYNC_TO_REMOTE     1
#define CLIP_SYNC_FROM_REMOTE   2

/* Clipboard state structure */
typedef struct {
    HWND hOwnerWnd;              /* Window that owns clipboard operations */
    BOOL bSyncing;               /* Currently syncing (prevent loops) */
    BOOL bEnabled;               /* Clipboard sync enabled */
    DWORD lastLocalSeq;          /* Last local clipboard sequence */
    DWORD lastRemoteSeq;         /* Last remote clipboard sequence */
    DWORD pendingRequest;        /* Pending clipboard request type */
} RD2K_CLIPBOARD_STATE, *PRD2K_CLIPBOARD_STATE;

/* Initialize clipboard module */
BOOL Clipboard_Init(HWND hOwnerWnd);

/* Cleanup clipboard module */
void Clipboard_Cleanup(void);

/* Enable/disable clipboard sync */
void Clipboard_SetEnabled(BOOL bEnabled);

/* Check if clipboard sync is enabled */
BOOL Clipboard_IsEnabled(void);

/* Handle Ctrl+C in viewer window - copy on remote, sync to local */
void Clipboard_HandleCopy(PRD2K_NETWORK pNet);

/* Handle Ctrl+V in viewer window - sync to remote, paste on remote */
void Clipboard_HandlePaste(PRD2K_NETWORK pNet);

/* Send local clipboard to remote */
BOOL Clipboard_SendToRemote(PRD2K_NETWORK pNet);

/* Request clipboard from remote */
BOOL Clipboard_RequestFromRemote(PRD2K_NETWORK pNet);

/* Receive clipboard data from remote and set local clipboard */
BOOL Clipboard_ReceiveFromRemote(const BYTE *data, DWORD length);

/* Get current clipboard type (text, files, image) */
int Clipboard_GetLocalType(void);

/* Check if we have text in clipboard */
BOOL Clipboard_HasText(void);

/* Check if we have files in clipboard */
BOOL Clipboard_HasFiles(void);

/* Internal: Send text clipboard */
BOOL Clipboard_SendText(PRD2K_NETWORK pNet);

/* Internal: Send file list clipboard (paths only, not content) */
BOOL Clipboard_SendFileList(PRD2K_NETWORK pNet);

/* Transfer files from local clipboard to remote */
/* This actually sends file content, not just paths */
BOOL Clipboard_TransferFiles(PRD2K_NETWORK pNet);

/* Internal: Receive text clipboard */
BOOL Clipboard_ReceiveText(const BYTE *data, DWORD length);

/* Internal: Receive file list and prepare for paste */
BOOL Clipboard_ReceiveFileList(const BYTE *data, DWORD length);

#endif /* CLIPBOARD_H */
