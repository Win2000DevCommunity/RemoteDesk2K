/*
 * UAC/Secure Desktop Support for RemoteDesk2K
 * Windows 2000/XP/7/10/11 compatible
 * 
 * This module provides functions to:
 * 1. Detect the active input desktop (Default or Secure/Winlogon)
 * 2. Switch thread to the active desktop for input simulation
 * 3. Handle input injection on UAC prompts and secure desktops
 * 4. Restore thread desktop after input injection
 * 
 * Key API Usage:
 * - OpenInputDesktop(): Get the current input desktop (user-facing)
 * - SetThreadDesktop(): Attach thread to a specific desktop
 * - GetThreadDesktop(): Get current thread's desktop
 * - CloseDesktop(): Close desktop handle
 * 
 * Windows 2000 Desktop-specific Access Flags:
 * DESKTOP_READOBJECTS         (0x0001)
 * DESKTOP_CREATEWINDOW        (0x0002)
 * DESKTOP_CREATEMENU          (0x0004)
 * DESKTOP_HOOKCONTROL         (0x0008)
 * DESKTOP_JOURNALRECORD       (0x0010)
 * DESKTOP_JOURNALPLAYBACK     (0x0020)  <-- Required for secure desktop input
 * DESKTOP_ENUMERATE           (0x0040)
 * DESKTOP_WRITEOBJECTS        (0x0080)
 * DESKTOP_SWITCHDESKTOP       (0x0100)
 */

#ifndef UAC_DESKTOP_H
#define UAC_DESKTOP_H

#include <windows.h>

/* Desktop access flags for input injection */
#define ALL_DESKTOP_ACCESS (DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | \
                            DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL | \
                            DESKTOP_JOURNALRECORD | DESKTOP_JOURNALPLAYBACK | \
                            DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS | \
                            DESKTOP_SWITCHDESKTOP)

/* Desktop context for thread switching */
typedef struct _DESKTOP_CONTEXT {
    HDESK        hInputDesktop;      /* Current input desktop handle */
    HDESK        hThreadDesktop;     /* Thread's original desktop */
    char         szDesktopName[64];  /* Desktop name (e.g., "Default", "Winlogon") */
    BOOL         bOnSecureDesktop;   /* TRUE if on UAC/Winlogon secure desktop */
    BOOL         bDesktopSwitched;   /* TRUE if thread was switched */
} DESKTOP_CONTEXT, *PDESKTOP_CONTEXT;

/* Initialize UAC/desktop support */
BOOL UacDesktop_Initialize(void);

/* Shutdown UAC/desktop support */
void UacDesktop_Shutdown(void);

/* Get desktop name (thread-safe) */
const char* UacDesktop_GetDesktopName(void);

/* Check if running on secure desktop (Winlogon UAC prompt) */
BOOL UacDesktop_IsSecureDesktop(void);

/* 
 * Prepare thread for input injection on active desktop.
 * This detects the current input desktop and switches the thread to it if needed.
 * 
 * Returns: desktop context to pass to UacDesktop_RestoreDesktop()
 * NULL if desktop switching is not needed or fails
 * 
 * Usage:
 *   PDESKTOP_CONTEXT pCtx = UacDesktop_PrepareForInput();
 *   Input_SimulateMouseClick(...);  // Now works on secure desktop
 *   UacDesktop_RestoreDesktop(pCtx);  // Restore original desktop
 */
PDESKTOP_CONTEXT UacDesktop_PrepareForInput(void);

/* 
 * Restore thread to its original desktop after input injection.
 * 
 * pContext: Context returned from UacDesktop_PrepareForInput()
 * 
 * Returns: TRUE if successful, FALSE otherwise
 */
BOOL UacDesktop_RestoreDesktop(PDESKTOP_CONTEXT pContext);

/* 
 * Get desktop information for debugging/logging.
 * 
 * Returns: pointer to static buffer with desktop info
 * (e.g., "Default Desktop" or "Secure Desktop (Winlogon)")
 */
const char* UacDesktop_GetInfo(void);

/*
 * Advanced: Manually get input desktop handle
 * 
 * Returns: handle to current input desktop, or NULL if error
 * Caller must close handle with CloseDesktop()
 * 
 * Requires appropriate privileges (typically SYSTEM)
 */
HDESK UacDesktop_OpenInputDesktop(void);

/*
 * Advanced: Manually switch thread to a specific desktop
 * 
 * hDesktop: Desktop handle to switch to
 * ppOriginalDesktop: Receives original desktop handle (for restoration)
 * 
 * Returns: TRUE if successful, FALSE otherwise
 */
BOOL UacDesktop_SwitchThreadDesktop(HDESK hDesktop, HDESK *ppOriginalDesktop);

#endif /* UAC_DESKTOP_H */
