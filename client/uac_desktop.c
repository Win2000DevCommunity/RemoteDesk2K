/*
 * UAC/Secure Desktop Support Implementation for RemoteDesk2K
 * Windows 2000/XP/7/10/11 compatible
 * 
 * Provides thread-safe desktop switching for input injection on:
 * - Secure/Winlogon desktop (UAC prompts)
 * - Default desktop (normal Windows session)
 * - Any accessible input desktop
 */

#include "uac_desktop.h"
#include <stdio.h>
#include <string.h>

/* Debug logging */
static void DesktopLog(const char *msg)
{
    FILE *f = fopen("rd2k_debug.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [DESKTOP] %s", 
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
}

/* Global desktop context */
static struct {
    BOOL bInitialized;
    char szCurrentDesktop[64];
    BOOL bOnSecureDesktop;
    CRITICAL_SECTION cs;
} g_desktopState = {0};

/* Initialize UAC/desktop support */
BOOL UacDesktop_Initialize(void)
{
    if (g_desktopState.bInitialized) return TRUE;
    
    InitializeCriticalSection(&g_desktopState.cs);
    
    ZeroMemory(&g_desktopState, sizeof(g_desktopState));
    g_desktopState.bInitialized = TRUE;
    strcpy(g_desktopState.szCurrentDesktop, "Default");
    g_desktopState.bOnSecureDesktop = FALSE;
    
    DesktopLog("[INIT] UAC/Desktop support initialized\r\n");
    return TRUE;
}

/* Shutdown UAC/desktop support */
void UacDesktop_Shutdown(void)
{
    if (!g_desktopState.bInitialized) return;
    
    DeleteCriticalSection(&g_desktopState.cs);
    g_desktopState.bInitialized = FALSE;
    
    DesktopLog("[SHUTDOWN] UAC/Desktop support shut down\r\n");
}

/* Get desktop name (thread-safe) */
const char* UacDesktop_GetDesktopName(void)
{
    const char *name;
    
    if (!g_desktopState.bInitialized) return "Unknown";
    
    EnterCriticalSection(&g_desktopState.cs);
    name = g_desktopState.szCurrentDesktop;
    LeaveCriticalSection(&g_desktopState.cs);
    
    return name;
}

/* Check if running on secure desktop */
BOOL UacDesktop_IsSecureDesktop(void)
{
    BOOL bSecure;
    
    if (!g_desktopState.bInitialized) return FALSE;
    
    EnterCriticalSection(&g_desktopState.cs);
    bSecure = g_desktopState.bOnSecureDesktop;
    LeaveCriticalSection(&g_desktopState.cs);
    
    return bSecure;
}

/*
 * Detect and return the name of the current desktop
 * 
 * Returns: pointer to static buffer with desktop name
 * (e.g., "Default", "Winlogon", "Disconnect", etc.)
 */
static const char* GetCurrentDesktopName(void)
{
    static char szDesktop[64] = {0};
    HDESK hDesktop;
    DWORD dwLen = 0;
    BOOL result;
    
    ZeroMemory(szDesktop, sizeof(szDesktop));
    
    /* Get the current thread's desktop */
    hDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!hDesktop) {
        strcpy(szDesktop, "Unknown");
        return szDesktop;
    }
    
    /* Get desktop name */
    result = GetUserObjectInformationA(hDesktop, UOI_NAME, szDesktop, sizeof(szDesktop)-1, &dwLen);
    
    if (!result || dwLen == 0) {
        strcpy(szDesktop, "Unknown");
    }
    
    return szDesktop;
}

/*
 * Check if a desktop is the secure desktop (Winlogon desktop)
 * 
 * The secure desktop is used for:
 * - UAC elevation prompts
 * - Secure Ctrl+Alt+Delete logon screen
 * - Windows Lock screen
 * 
 * Returns: TRUE if desktop name starts with "Winlogon"
 */
static BOOL IsSecureDesktopName(const char *szName)
{
    if (!szName) return FALSE;
    
    /* Secure desktop names in Windows: "Winlogon", "ScreenSaverDesktop", etc. */
    if (strstr(szName, "Winlogon") != NULL) return TRUE;
    if (strstr(szName, "winlogon") != NULL) return TRUE;
    if (strstr(szName, "Disconnect") != NULL) return TRUE;
    
    return FALSE;
}

/* Get input desktop handle */
HDESK UacDesktop_OpenInputDesktop(void)
{
    HDESK hInputDesktop;
    char szInfoBuffer[64] = {0};
    
    /* 
     * OpenInputDesktop gets the desktop that is currently receiving user input.
     * This might be the Default desktop, the Winlogon (secure) desktop, or another desktop.
     * 
     * Flags:
     *   0: Inheritance flag (FALSE = handle is not inheritable)
     * 
     * Access mask:
     *   ALL_DESKTOP_ACCESS: Full access including input injection
     *   We use GENERIC_ALL or the specific flags depending on what we need
     */
    hInputDesktop = OpenInputDesktop(
        0,                          /* dwFlags: 0 = not inheritable */
        FALSE,                      /* fInherit: FALSE */
        GENERIC_ALL                 /* dwDesiredAccess: Full access */
    );
    
    if (hInputDesktop) {
        /* Log which desktop we got */
        DWORD dwLen = 0;
        if (GetUserObjectInformationA(hInputDesktop, UOI_NAME, szInfoBuffer, sizeof(szInfoBuffer)-1, &dwLen)) {
            char buf[256];
            sprintf(buf, "[INPUT] Got input desktop: %s\r\n", szInfoBuffer);
            DesktopLog(buf);
        }
        
        /* Update global state */
        EnterCriticalSection(&g_desktopState.cs);
        strcpy(g_desktopState.szCurrentDesktop, szInfoBuffer);
        g_desktopState.bOnSecureDesktop = IsSecureDesktopName(szInfoBuffer);
        LeaveCriticalSection(&g_desktopState.cs);
    } else {
        DesktopLog("[ERROR] OpenInputDesktop failed\r\n");
    }
    
    return hInputDesktop;
}

/* Switch thread to a specific desktop */
BOOL UacDesktop_SwitchThreadDesktop(HDESK hDesktop, HDESK *ppOriginalDesktop)
{
    HDESK hCurrentDesktop;
    BOOL result;
    char buf[256];
    
    if (!hDesktop || !ppOriginalDesktop) {
        DesktopLog("[ERROR] Invalid parameters to SwitchThreadDesktop\r\n");
        return FALSE;
    }
    
    /* Get current thread's desktop before switching */
    hCurrentDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!hCurrentDesktop) {
        DesktopLog("[ERROR] GetThreadDesktop failed before switch\r\n");
        return FALSE;
    }
    
    /* Switch thread to new desktop */
    result = SetThreadDesktop(hDesktop);
    if (!result) {
        DWORD dwError = GetLastError();
        sprintf(buf, "[ERROR] SetThreadDesktop failed: 0x%08lX\r\n", dwError);
        DesktopLog(buf);
        return FALSE;
    }
    
    /* Store original desktop for restoration */
    *ppOriginalDesktop = hCurrentDesktop;
    
    DesktopLog("[SUCCESS] Thread switched to new desktop\r\n");
    return TRUE;
}

/* 
 * Prepare thread for input injection on the active desktop
 * 
 * This is the main function called before input simulation (mouse/keyboard)
 * to ensure the thread can inject input on the currently active desktop
 * (which might be the secure desktop if UAC prompt is showing)
 */
PDESKTOP_CONTEXT UacDesktop_PrepareForInput(void)
{
    PDESKTOP_CONTEXT pContext = NULL;
    HDESK hInputDesktop = NULL;
    HDESK hCurrentDesktop = NULL;
    DWORD dwLen = 0;
    BOOL bNeedSwitch = FALSE;
    char szInputDesktop[64] = {0};
    char szCurrentDesktop[64] = {0};
    char buf[512];
    
    if (!g_desktopState.bInitialized) {
        DesktopLog("[ERROR] UacDesktop not initialized\r\n");
        return NULL;
    }
    
    /* Allocate context */
    pContext = (PDESKTOP_CONTEXT)malloc(sizeof(DESKTOP_CONTEXT));
    if (!pContext) {
        DesktopLog("[ERROR] Failed to allocate desktop context\r\n");
        return NULL;
    }
    
    ZeroMemory(pContext, sizeof(DESKTOP_CONTEXT));
    pContext->bDesktopSwitched = FALSE;
    
    /* Get current input desktop */
    hInputDesktop = UacDesktop_OpenInputDesktop();
    if (!hInputDesktop) {
        DesktopLog("[WARN] Could not get input desktop, will use current thread desktop\r\n");
        free(pContext);
        return NULL;  /* Not critical - input might still work without switching */
    }
    
    pContext->hInputDesktop = hInputDesktop;
    
    /* Get input desktop name */
    if (GetUserObjectInformationA(hInputDesktop, UOI_NAME, szInputDesktop, sizeof(szInputDesktop)-1, &dwLen)) {
        strcpy(pContext->szDesktopName, szInputDesktop);
        pContext->bOnSecureDesktop = IsSecureDesktopName(szInputDesktop);
    }
    
    /* Get current thread's desktop */
    hCurrentDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!hCurrentDesktop) {
        DesktopLog("[ERROR] Could not get current thread desktop\r\n");
        CloseDesktop(hInputDesktop);
        free(pContext);
        return NULL;
    }
    
    pContext->hThreadDesktop = hCurrentDesktop;
    
    /* Get current desktop name */
    GetUserObjectInformationA(hCurrentDesktop, UOI_NAME, szCurrentDesktop, sizeof(szCurrentDesktop)-1, &dwLen);
    
    /* Check if we need to switch */
    if (_stricmp(szCurrentDesktop, szInputDesktop) != 0) {
        /* Different desktops - need to switch thread to input desktop */
        bNeedSwitch = TRUE;
    }
    
    if (bNeedSwitch) {
        BOOL result;
        
        sprintf(buf, "[PREPARE] Switching from '%s' to '%s'\r\n", szCurrentDesktop, szInputDesktop);
        DesktopLog(buf);
        
        result = SetThreadDesktop(hInputDesktop);
        if (result) {
            pContext->bDesktopSwitched = TRUE;
            sprintf(buf, "[SUCCESS] Thread switched to input desktop '%s'\r\n", szInputDesktop);
            DesktopLog(buf);
        } else {
            DWORD dwError = GetLastError();
            sprintf(buf, "[ERROR] SetThreadDesktop failed: 0x%08lX\r\n", dwError);
            DesktopLog(buf);
            
            /* Even if switch fails, continue - might still work */
            pContext->bDesktopSwitched = FALSE;
        }
    } else {
        sprintf(buf, "[INFO] Already on correct desktop '%s', no switch needed\r\n", szCurrentDesktop);
        DesktopLog(buf);
        pContext->bDesktopSwitched = FALSE;
    }
    
    return pContext;
}

/* 
 * Restore thread to its original desktop after input injection
 */
BOOL UacDesktop_RestoreDesktop(PDESKTOP_CONTEXT pContext)
{
    BOOL result = TRUE;
    char buf[256];
    
    if (!pContext) {
        return TRUE;  /* Nothing to restore */
    }
    
    if (!g_desktopState.bInitialized) {
        free(pContext);
        return FALSE;
    }
    
    /* If we switched desktops, switch back */
    if (pContext->bDesktopSwitched && pContext->hThreadDesktop) {
        result = SetThreadDesktop(pContext->hThreadDesktop);
        if (result) {
            DesktopLog("[RESTORE] Thread restored to original desktop\r\n");
        } else {
            DWORD dwError = GetLastError();
            sprintf(buf, "[ERROR] Could not restore desktop: 0x%08lX\r\n", dwError);
            DesktopLog(buf);
        }
    }
    
    /* Close input desktop handle if we opened it */
    if (pContext->hInputDesktop) {
        CloseDesktop(pContext->hInputDesktop);
    }
    
    free(pContext);
    return result;
}

/* Get desktop information for logging */
const char* UacDesktop_GetInfo(void)
{
    static char szInfo[256] = {0};
    const char *szDesktopName;
    
    if (!g_desktopState.bInitialized) {
        strcpy(szInfo, "UAC/Desktop not initialized");
        return szInfo;
    }
    
    szDesktopName = UacDesktop_GetDesktopName();
    
    if (UacDesktop_IsSecureDesktop()) {
        sprintf(szInfo, "Secure Desktop (%s) - UAC Active", szDesktopName);
    } else {
        sprintf(szInfo, "Standard Desktop (%s)", szDesktopName);
    }
    
    return szInfo;
}
