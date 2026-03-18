/*
 * RemoteDesk2K - Desktop Enumeration & Switching (UltraVNC Pattern)
 * Windows 2000 SP1 SDK - Professional C89
 *
 * EXCLUSIVE LOGIC:
 * 1. Detect desktop switches via OpenInputDesktop() + desktop name comparison
 * 2. Handle Winlogon (ERROR_CANNOT_OPEN_CHANNEL = 170)
 * 3. Handle UAC/Secure Desktop (ERROR_INVALID_HANDLE = 624)
 * 4. Recursive state machine for desktop enumeration
 * 5. Always restore home desktop on exit
 */

#ifndef _REMOTEDESK2K_DESKTOP_H_
#define _REMOTEDESK2K_DESKTOP_H_

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ============================================================================ */

#define RD2K_ERROR_CANNOT_OPEN_CHANNEL    170  /* Winlogon desktop */
#define RD2K_ERROR_INVALID_HANDLE         624  /* UAC/Secure Desktop */

/* Desktop states for state machine */
typedef enum {
    DESKTOP_STATE_NORMAL = 0,        /* Normal user desktop */
    DESKTOP_STATE_UAC_ACTIVE = 1,    /* UAC/Secure desktop active */
    DESKTOP_STATE_WINLOGON = 2,      /* Winlogon (login screen) */
    DESKTOP_STATE_UNAVAILABLE = 3    /* Desktop not accessible */
} DESKTOP_STATE;

/* Capture mode fallback levels (multi-layer strategy) */
typedef enum {
    CAPTURE_MODE_TOKEN_HIJACKING = 0,   /* Layer 1: Token hijacking (RustDesk pattern) */
    CAPTURE_MODE_DIRECT_CAPTURE = 1,    /* Layer 2: Direct capture with pixelDataSize validation */
    CAPTURE_MODE_BLACK_WITH_MSG = 2,    /* Layer 3: Black screen with user notification */
    CAPTURE_MODE_DISABLED = 3           /* Layer 4: Completely disabled (fallback only) */
} CAPTURE_MODE;

/* ============================================================================
 * STRUCTURES
 * ============================================================================ */

typedef struct _DESKTOP_CONTEXT {
    /* Home desktop - saved at startup, restored at shutdown */
    HDESK         hHomeDesktop;
    
    /* Current desktop handles */
    HDESK         hThreadDesktop;
    HDESK         hInputDesktop;
    
    /* Winlogon desktop handle (for explicit Winlogon access) */
    HDESK         hWinlogonDesktop;
    BOOL          bOnWinlogonDesktop;
    
    /* Token impersonation (UltraVNC pattern for Winlogon access) */
    HANDLE        hImpersonationToken;   /* Duplicated winlogon.exe token */
    BOOL          bImpersonating;        /* TRUE while impersonating winlogon */
    
    /* Window station handles (for Winlogon desktop access) */
    HWINSTA       hSavedWinSta;          /* Original process window station */
    HWINSTA       hWinSta0;              /* WinSta0 handle (opened with SYSTEM) */
    
    /* Desktop names for detection */
    char          szThreadDesktopName[256];
    char          szInputDesktopName[256];
    
    /* Current state */
    DESKTOP_STATE ePreviousState;
    DESKTOP_STATE eCurrentState;
    
    /* Capture mode (multi-layer fallback) */
    CAPTURE_MODE  eCaptureMode;
    DWORD         dwCaptureFailures;

    
    /* Statistics */
    DWORD         dwDesktopSwitches;
    DWORD         dwUACDetections;
    DWORD         dwWinlogonDetections;
    
    /* Message buffer (for displaying text on secure desktop) */
    char          szLastErrorMsg[256];
    
} DESKTOP_CONTEXT, *PDESKTOP_CONTEXT;

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/* Initialize desktop context at server startup */
PDESKTOP_CONTEXT Desktop_Init(void);

/* Clean up desktop context - RESTORES HOME DESKTOP */
void Desktop_Shutdown(PDESKTOP_CONTEXT pDesktop);

/* Detect current desktop state (normal/UAC/Winlogon/unavailable) */
DESKTOP_STATE Desktop_DetectState(PDESKTOP_CONTEXT pDesktop);

/* Switch to input desktop if different from thread desktop */
BOOL Desktop_SwitchToInput(PDESKTOP_CONTEXT pDesktop);

/* Switch to Winlogon desktop explicitly by name (for secure desktop access) */
BOOL Desktop_SwitchToWinlogon(PDESKTOP_CONTEXT pDesktop);

/* Restore home desktop */
BOOL Desktop_RestoreHome(PDESKTOP_CONTEXT pDesktop);

/* Check if desktop switched since last check */
BOOL Desktop_HasSwitched(PDESKTOP_CONTEXT pDesktop);

/* Get human-readable desktop state name */
const char* Desktop_StateToString(DESKTOP_STATE eState);

/* ============================================================================
 * WINLOGON TOKEN IMPERSONATION (UltraVNC Pattern)
 * ============================================================================ */

/* Enable SE_DEBUG_NAME privilege for process access */
BOOL Desktop_EnableDebugPrivilege(void);

/* Find winlogon.exe PID */
DWORD Desktop_FindWinlogonPID(void);

/* Impersonate winlogon.exe token to access Winlogon desktop */
BOOL Desktop_ImpersonateWinlogon(PDESKTOP_CONTEXT pDesktop);

/* Revert impersonation after capture */
void Desktop_RevertImpersonation(PDESKTOP_CONTEXT pDesktop);

/* ============================================================================
 * SAS (Secure Attention Sequence - Ctrl+Alt+Del)
 * ============================================================================ */

/* Send SAS (Ctrl+Alt+Del) via registry + SendSAS API */
BOOL Desktop_SendSAS(void);

/* Check and set SoftwareSASGeneration registry for SAS support */
BOOL Desktop_ConfigureSASSupport(BOOL bEnable);

/* ============================================================================
 * CAPTURE FALLBACK MODES
 * ============================================================================ */

/* Try next fallback mode if current fails */
BOOL Desktop_TryNextCaptureMode(PDESKTOP_CONTEXT pDesktop);

/* Get human-readable capture mode name */
const char* Desktop_CaptureModeToString(CAPTURE_MODE eMode);

#endif /* _REMOTEDESK2K_DESKTOP_H_ */
