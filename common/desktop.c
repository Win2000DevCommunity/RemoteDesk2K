/*
 * RemoteDesk2K - Desktop Enumeration & Switching Implementation
 * UltraVNC Pattern for Windows 2000 SP1
 */

#include "desktop.h"
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include <winuser.h>

/* Window station access constants - not defined in W2K DDK headers */
#ifndef WINSTA_ALL_ACCESS
#define WINSTA_ALL_ACCESS  0x000F037FL
#endif

/* ============================================================================
 * Native API definitions for token acquisition.
 * NtOpenProcessToken provides better error diagnostics than Win32 API.
 * Brute-force DuplicateHandle scan bypasses both process and token DACLs.
 * ============================================================================ */

typedef LONG NTSTATUS;
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#define STATUS_ACCESS_DENIED        ((NTSTATUS)0xC0000022L)

/* NtOpenProcessToken - Native API equivalent of OpenProcessToken.
 * Available since Windows 2000 in ntdll.dll.
 * Same DACL check as Win32 OpenProcessToken but returns NTSTATUS
 * for better error diagnostics. */
typedef NTSTATUS (NTAPI *PFN_NtOpenProcessToken)(
    HANDLE ProcessHandle,
    ACCESS_MASK DesiredAccess,
    PHANDLE TokenHandle
);

static void DebugLog(const char *msg)
{
#ifdef RD2K_DEBUG
    FILE *f = fopen("rd2k_debug.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [DESKTOP] %s",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
#else
    (void)msg;
#endif
}

/* ============================================================================
 * HELPER: Get desktop name
 * ============================================================================ */

static BOOL GetDesktopName(HDESK hDesktop, char *szNameOut, size_t cchMax)
{
    DWORD dwNameLen = 0;
    
    if (!hDesktop) {
        szNameOut[0] = '\0';
        return FALSE;
    }
    
    if (!GetUserObjectInformation(hDesktop, UOI_NAME, szNameOut, (DWORD)cchMax, &dwNameLen)) {
        szNameOut[0] = '\0';
        return FALSE;
    }
    
    return TRUE;
}

/* ============================================================================
 * HELPER: Compare two desktop names
 * ============================================================================ */

static BOOL DesktopsEqual(const char *sz1, const char *sz2)
{
    if (!sz1 || !sz2) return FALSE;
    return strcmp(sz1, sz2) == 0;
}

/* ============================================================================
 * RECURSIVE PATTERN: Desktop State Detection
 * ============================================================================ */

DESKTOP_STATE Desktop_DetectState(PDESKTOP_CONTEXT pDesktop)
{
    HDESK hInput = NULL;
    DWORD dwErr;
    char szInputDesktopName[256];
    char buf[512];
    
    if (!pDesktop) return DESKTOP_STATE_UNAVAILABLE;
    
    /* CRITICAL FIX: Close previous input desktop handle to prevent leak.
     * OpenInputDesktop() returns a NEW handle each call. Without closing the old
     * one, we leak ~10 desktop handles/sec. Windows desktop handle limit is low
     * (~500-1000), so exhaustion crashes the process within seconds in release mode.
     * In debug mode, logging I/O slowed the loop enough to delay the crash. */
    if (pDesktop->hInputDesktop) {
        CloseDesktop(pDesktop->hInputDesktop);
        pDesktop->hInputDesktop = NULL;
    }
    
    /* Save thread desktop (current context) */
    pDesktop->hThreadDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (pDesktop->hThreadDesktop) {
        GetDesktopName(pDesktop->hThreadDesktop, pDesktop->szThreadDesktopName, sizeof(pDesktop->szThreadDesktopName));
    } else {
        strcpy(pDesktop->szThreadDesktopName, "");
    }
    
    /* Try to open input desktop - THIS IS THE KEY DETECTION */
    hInput = OpenInputDesktop(0, FALSE,
        DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE |
        DESKTOP_HOOKCONTROL | DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS |
        DESKTOP_SWITCHDESKTOP);
    
    if (hInput == NULL) {
        dwErr = GetLastError();
        
        /* Windows 2000 OpenInputDesktop error codes during Winlogon/secure desktop:
           - Error 170 (CANNOT_OPEN_CHANNEL) = Winlogon (login screen)
           - Error 183 (ALREADY_EXISTS) = Locked/secure desktop (Winlogon or UAC equivalent)
           - Error 624 (INVALID_HANDLE) = UAC/Secure Desktop
           - Error 5 (ACCESS_DENIED) = No permission to access input desktop
        */
        
        if (dwErr == RD2K_ERROR_CANNOT_OPEN_CHANNEL) {
            /* 170 = Winlogon desktop (login screen) */
            sprintf(buf, "[DetectState] Error 170=CANNOT_OPEN_CHANNEL, Winlogon detected\r\n");
            DebugLog(buf);
            pDesktop->dwWinlogonDetections++;
            pDesktop->eCurrentState = DESKTOP_STATE_WINLOGON;
            return DESKTOP_STATE_WINLOGON;
        } else if (dwErr == RD2K_ERROR_INVALID_HANDLE) {
            /* 624 = UAC/Secure Desktop active */
            sprintf(buf, "[DetectState] Error 624=INVALID_HANDLE, UAC/Secure Desktop detected\r\n");
            DebugLog(buf);
            pDesktop->dwUACDetections++;
            pDesktop->eCurrentState = DESKTOP_STATE_UAC_ACTIVE;
            return DESKTOP_STATE_UAC_ACTIVE;
        } else if (dwErr == 183 || dwErr == 5 || dwErr == 2) {
            /* 183 = ERROR_ALREADY_EXISTS (W2K Winlogon indicator)
               5 = ERROR_ACCESS_DENIED (secure desktop)
               2 = ERROR_FILE_NOT_FOUND (sometimes returned on Winlogon) */
            if (dwErr == 183) {
                sprintf(buf, "[DetectState] Error 183=ALREADY_EXISTS, W2K Winlogon/Secure Desktop detected\r\n");
            } else if (dwErr == 5) {
                sprintf(buf, "[DetectState] Error 5=ACCESS_DENIED, Secure desktop detected\r\n");
            } else {
                sprintf(buf, "[DetectState] Error 2=FILE_NOT_FOUND, Possible Winlogon condition\r\n");
            }
            DebugLog(buf);
            pDesktop->dwWinlogonDetections++;
            pDesktop->eCurrentState = DESKTOP_STATE_WINLOGON;
            return DESKTOP_STATE_WINLOGON;
        } else {
            /* Other unknown error - log but treat as unavailable for safety */
            sprintf(buf, "[DetectState] OpenInputDesktop failed with unknown error %lu\r\n", dwErr);
            DebugLog(buf);
            pDesktop->eCurrentState = DESKTOP_STATE_UNAVAILABLE;
            return DESKTOP_STATE_UNAVAILABLE;
        }
    }
    
    /* SUCCESS: Got input desktop handle */
    pDesktop->hInputDesktop = hInput;
    GetDesktopName(hInput, szInputDesktopName, sizeof(szInputDesktopName));
    strcpy(pDesktop->szInputDesktopName, szInputDesktopName);
    
    /* Compare desktop names to detect switch */
    if (!DesktopsEqual(pDesktop->szThreadDesktopName, szInputDesktopName)) {
        sprintf(buf, "[DetectState] Desktop switch detected: '%s' -> '%s'\r\n",
                pDesktop->szThreadDesktopName, szInputDesktopName);
        DebugLog(buf);
        pDesktop->dwDesktopSwitches++;
    }
    
    /* CRITICAL: Even when OpenInputDesktop succeeds (e.g. after SYSTEM impersonation),
     * if the input desktop name IS "Winlogon", we are still on the Winlogon desktop.
     * Without this check, state oscillates WINLOGON->NORMAL->WINLOGON every frame,
     * preventing the input thread from staying switched to the Winlogon desktop. */
    if (_stricmp(szInputDesktopName, "Winlogon") == 0) {
        DebugLog("[DetectState] Input desktop is 'Winlogon' (handle opened OK) - returning WINLOGON\r\n");
        CloseDesktop(hInput);
        pDesktop->hInputDesktop = NULL;
        pDesktop->dwWinlogonDetections++;
        pDesktop->eCurrentState = DESKTOP_STATE_WINLOGON;
        return DESKTOP_STATE_WINLOGON;
    }
    
    pDesktop->eCurrentState = DESKTOP_STATE_NORMAL;
    return DESKTOP_STATE_NORMAL;
}

/* ============================================================================
 * WINLOGON TOKEN IMPERSONATION (UltraVNC Pattern)
 * 
 * On Windows 2000/XP, OpenDesktop("Winlogon") returns ERROR_ACCESS_DENIED (5)
 * because the Winlogon desktop DACL denies access to regular processes.
 * UltraVNC solves this by impersonating winlogon.exe's security token:
 *   1. Enable SE_DEBUG_NAME privilege (to open other processes)
 *   2. Find winlogon.exe PID via CreateToolhelp32Snapshot
 *   3. OpenProcess -> OpenProcessToken -> DuplicateTokenEx
 *   4. ImpersonateLoggedOnUser (now we ARE winlogon.exe)
 *   5. OpenDesktop("Winlogon") succeeds
 *   6. SetThreadDesktop -> GetDC(NULL) -> BitBlt
 *   7. RevertToSelf after capture
 * ============================================================================ */

BOOL Desktop_EnableDebugPrivilege(void)
{
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    char buf[256];
    
    if (!OpenProcessToken(GetCurrentProcess(), 
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        sprintf(buf, "[Privilege] OpenProcessToken failed: %lu\r\n", GetLastError());
        DebugLog(buf);
        return FALSE;
    }
    
    if (!LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &luid)) {
        sprintf(buf, "[Privilege] LookupPrivilegeValue(SeDebugPrivilege) failed: %lu\r\n", GetLastError());
        DebugLog(buf);
        CloseHandle(hToken);
        return FALSE;
    }
    
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        sprintf(buf, "[Privilege] AdjustTokenPrivileges failed: %lu\r\n", GetLastError());
        DebugLog(buf);
        CloseHandle(hToken);
        return FALSE;
    }
    
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        DebugLog("[Privilege] WARNING: SeDebugPrivilege not assigned (not admin?)\r\n");
        CloseHandle(hToken);
        return FALSE;
    }
    
    DebugLog("[Privilege] SeDebugPrivilege enabled successfully\r\n");
    CloseHandle(hToken);
    return TRUE;
}

DWORD Desktop_FindWinlogonPID(void)
{
    HANDLE hSnapshot;
    PROCESSENTRY32 pe;
    DWORD dwWinlogonPID = 0;
    char buf[256];
    
    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        sprintf(buf, "[FindWinlogon] CreateToolhelp32Snapshot failed: %lu\r\n", GetLastError());
        DebugLog(buf);
        return 0;
    }
    
    pe.dwSize = sizeof(PROCESSENTRY32);
    
    if (Process32First(hSnapshot, &pe)) {
        do {
            /* Case-insensitive compare for "winlogon.exe" */
            if (_stricmp(pe.szExeFile, "winlogon.exe") == 0) {
                dwWinlogonPID = pe.th32ProcessID;
                sprintf(buf, "[FindWinlogon] Found winlogon.exe PID=%lu\r\n", dwWinlogonPID);
                DebugLog(buf);
                break;
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    
    CloseHandle(hSnapshot);
    
    if (dwWinlogonPID == 0) {
        DebugLog("[FindWinlogon] winlogon.exe NOT found in process list\r\n");
    }
    
    return dwWinlogonPID;
}

BOOL Desktop_ImpersonateWinlogon(PDESKTOP_CONTEXT pDesktop)
{
    DWORD dwWinlogonPID;
    HANDLE hProcess = NULL;
    HANDLE hToken = NULL;
    HANDLE hDupToken = NULL;
    char buf[512];
    
    /* Native API variables */
    HMODULE hNtdll = NULL;
    PFN_NtOpenProcessToken pfnNtOpenProcessToken = NULL;
    NTSTATUS ntStatus;
    BOOL bGotToken = FALSE;
    
    /* Multi-process scanning variables */
    HANDLE hSnap = INVALID_HANDLE_VALUE;
    PROCESSENTRY32 pe32;
    DWORD dwSystemPIDs[64];
    HANDLE hSystemProcs[64];
    int nSystemProcs = 0;
    int p;
    
    /* Known SYSTEM process names - ordered by likelihood of having token handles.
     * lsass.exe = Local Security Authority, manages ALL logon tokens.
     * services.exe = Service Control Manager, opens tokens for service accounts.
     * winlogon.exe = manages interactive logon, may hold logon tokens.
     * csrss.exe / smss.exe = core subsystem processes, run as SYSTEM. */
    static const char *szSystemNames[] = {
        "lsass.exe", "services.exe", "winlogon.exe", 
        "csrss.exe", "smss.exe", NULL
    };
    
    if (!pDesktop) return FALSE;
    
    /* Already impersonating? */
    if (pDesktop->bImpersonating) {
        return TRUE;
    }
    
    DebugLog("[Impersonate] Starting SYSTEM token acquisition...\r\n");
    
    /* Step 1: Enable SE_DEBUG_NAME privilege */
    if (!Desktop_EnableDebugPrivilege()) {
        DebugLog("[Impersonate] WARNING: Could not enable SeDebugPrivilege (continuing anyway)\r\n");
    }
    
    /* Step 2: Find winlogon.exe PID (needed for desktop switch) */
    dwWinlogonPID = Desktop_FindWinlogonPID();
    if (dwWinlogonPID == 0) {
        DebugLog("[Impersonate] FAILED: Cannot find winlogon.exe\r\n");
        return FALSE;
    }
    
    /* Load ntdll.dll once - needed for multiple approaches below */
    hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) {
        DebugLog("[Impersonate] WARNING: ntdll.dll not loaded\r\n");
    }
    
    /* ===================================================================
     * FAST PATH 1: Win32 OpenProcessToken
     * Works on Vista+ but fails on W2K/XP due to SYSTEM token DACL.
     * =================================================================== */
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, dwWinlogonPID);
    if (hProcess) {
        if (OpenProcessToken(hProcess, TOKEN_DUPLICATE | TOKEN_QUERY, &hToken)) {
            DebugLog("[Impersonate] OpenProcessToken succeeded (Win32 fast path)\r\n");
            bGotToken = TRUE;
        } else {
            sprintf(buf, "[Impersonate] Win32 OpenProcessToken failed: %lu\r\n", GetLastError());
            DebugLog(buf);
        }
        CloseHandle(hProcess);
        hProcess = NULL;
    } else {
        sprintf(buf, "[Impersonate] OpenProcess(QUERY_INFO, %lu) failed: %lu\r\n",
                dwWinlogonPID, GetLastError());
        DebugLog(buf);
    }
    
    /* ===================================================================
     * FAST PATH 2: NtOpenProcessToken (Native API from ntdll.dll)
     * Same underlying syscall as OpenProcessToken but returns NTSTATUS
     * for better diagnostics. On some NT/2000 builds, the native path
     * may have different DACL evaluation behavior.
     * Try multiple access mask combinations.
     * =================================================================== */
    if (!bGotToken && hNtdll) {
        pfnNtOpenProcessToken = (PFN_NtOpenProcessToken)GetProcAddress(hNtdll, "NtOpenProcessToken");
        if (pfnNtOpenProcessToken) {
            ACCESS_MASK amTry;
            ACCESS_MASK amGot = 0;
            HANDLE hQueryToken = NULL; /* token with TOKEN_QUERY only (fallback) */
            HANDLE hUpgraded = NULL;  /* for DuplicateHandle upgrade attempt */
            HANDLE hTestDup = NULL;   /* for immediate DuplicateTokenEx validation */
            int m;
            DWORD dwTryPIDs[64];
            int nTryPIDs = 0;
            
            DebugLog("[Impersonate] Trying NtOpenProcessToken (Native API)...\r\n");
            
            {
                
                /* Add winlogon first */
                dwTryPIDs[nTryPIDs++] = dwWinlogonPID;
                
                /* Also find lsass, services, etc. */
                hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (hSnap != INVALID_HANDLE_VALUE) {
                    pe32.dwSize = sizeof(pe32);
                    if (Process32First(hSnap, &pe32)) {
                        do {
                            int j;
                            if (pe32.th32ProcessID == dwWinlogonPID) continue;
                            for (j = 0; szSystemNames[j] != NULL; j++) {
                                if (_stricmp(pe32.szExeFile, szSystemNames[j]) == 0) {
                                    if (nTryPIDs < 64) {
                                        dwTryPIDs[nTryPIDs++] = pe32.th32ProcessID;
                                    }
                                    break;
                                }
                            }
                        } while (Process32Next(hSnap, &pe32));
                    }
                    CloseHandle(hSnap);
                    hSnap = INVALID_HANDLE_VALUE;
                }
                
                sprintf(buf, "[Impersonate] NtOpenProcessToken: trying %d PIDs\r\n", nTryPIDs);
                DebugLog(buf);
                
                for (p = 0; p < nTryPIDs && !bGotToken; p++) {
                    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, dwTryPIDs[p]);
                    if (!hProcess) {
                        hProcess = OpenProcess(MAXIMUM_ALLOWED, FALSE, dwTryPIDs[p]);
                    }
                    if (!hProcess) continue;
                    
                    /* Try access masks that include TOKEN_DUPLICATE (needed for DuplicateTokenEx).
                     * Order: most useful first. If we get TOKEN_DUPLICATE we can proceed.
                     * IMPORTANT: After success, we VALIDATE the handle by attempting
                     * DuplicateTokenEx immediately. If it fails, the handle is useless
                     * (e.g. MAXIMUM_ALLOWED may only grant TOKEN_QUERY). */
                    for (m = 0; m < 7 && !bGotToken; m++) {
                        hTestDup = NULL;
                        switch (m) {
                            case 0: amTry = TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_QUERY; break;
                            case 1: amTry = TOKEN_DUPLICATE | TOKEN_QUERY; break;
                            case 2: amTry = TOKEN_ALL_ACCESS; break;
                            case 3: amTry = TOKEN_DUPLICATE | TOKEN_IMPERSONATE; break;
                            case 4: amTry = TOKEN_DUPLICATE; break;
                            case 5: amTry = MAXIMUM_ALLOWED; break;
                            default: amTry = TOKEN_IMPERSONATE | TOKEN_QUERY; break;
                        }
                        hToken = NULL;
                        ntStatus = pfnNtOpenProcessToken(hProcess, amTry, &hToken);
                        if (NT_SUCCESS(ntStatus) && hToken) {
                            sprintf(buf, "[Impersonate] NtOpenProcessToken opened PID=%lu mask=0x%08lX\r\n",
                                    dwTryPIDs[p], (DWORD)amTry);
                            DebugLog(buf);
                            
                            /* VALIDATE: can we actually duplicate this token? */
                            if (DuplicateTokenEx(hToken, TOKEN_IMPERSONATE | TOKEN_QUERY,
                                                NULL, SecurityImpersonation,
                                                TokenImpersonation, &hTestDup)) {
                                sprintf(buf, "[Impersonate] NtOpenProcessToken VALIDATED PID=%lu mask=0x%08lX\r\n",
                                        dwTryPIDs[p], (DWORD)amTry);
                                DebugLog(buf);
                                CloseHandle(hTestDup);
                                amGot = amTry;
                                bGotToken = TRUE;
                                break;
                            }
                            
                            /* Validation failed - this handle lacks TOKEN_DUPLICATE.
                             * Save it as a QUERY-only fallback and try next mask/PID. */
                            sprintf(buf, "[Impersonate] NtOpenProcessToken PID=%lu mask=0x%08lX opened but DuplicateTokenEx failed: %lu (handle useless)\r\n",
                                    dwTryPIDs[p], (DWORD)amTry, GetLastError());
                            DebugLog(buf);
                            
                            if (!hQueryToken) {
                                hQueryToken = hToken;
                                hToken = NULL;
                            } else {
                                CloseHandle(hToken);
                                hToken = NULL;
                            }
                        } else {
                            sprintf(buf, "[Impersonate] NtOpenProcessToken PID=%lu mask=0x%08lX => 0x%08lX\r\n",
                                    dwTryPIDs[p], (DWORD)amTry, (DWORD)ntStatus);
                            DebugLog(buf);
                        }
                    }
                    
                    /* If mask loop found nothing useful, also try TOKEN_QUERY explicitly
                     * and attempt DuplicateHandle to upgrade access rights.
                     * DuplicateHandle on OUR OWN process's handle checks OUR process DACL,
                     * not the token's DACL, so it may grant more access. */
                    if (!bGotToken) {
                        hToken = NULL;
                        ntStatus = pfnNtOpenProcessToken(hProcess, TOKEN_QUERY, &hToken);
                        if (NT_SUCCESS(ntStatus) && hToken) {
                            sprintf(buf, "[Impersonate] Got TOKEN_QUERY-only handle for PID=%lu, trying DuplicateHandle upgrade...\r\n",
                                    dwTryPIDs[p]);
                            DebugLog(buf);
                            
                            hUpgraded = NULL;
                            if (DuplicateHandle(GetCurrentProcess(), hToken,
                                               GetCurrentProcess(), &hUpgraded,
                                               TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_QUERY,
                                               FALSE, 0)) {
                                DebugLog("[Impersonate] DuplicateHandle upgrade SUCCESS!\r\n");
                                CloseHandle(hToken);
                                hToken = hUpgraded;
                                amGot = TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_QUERY;
                                bGotToken = TRUE;
                            } else {
                                sprintf(buf, "[Impersonate] DuplicateHandle upgrade failed: %lu\r\n",
                                        GetLastError());
                                DebugLog(buf);
                                
                                /* Keep the QUERY-only handle as last resort */
                                if (!hQueryToken) {
                                    hQueryToken = hToken;
                                    hToken = NULL;
                                } else {
                                    CloseHandle(hToken);
                                    hToken = NULL;
                                }
                            }
                        }
                    }
                    
                    CloseHandle(hProcess);
                    hProcess = NULL;
                }
                
                /* If we got a TOKEN_QUERY-only handle, try using it directly
                 * with SetThreadToken (may work with just IMPERSONATE+QUERY, 
                 * or even QUERY alone on some NT versions) */
                if (!bGotToken && hQueryToken) {
                    DebugLog("[Impersonate] Trying SetThreadToken with QUERY-only token...\r\n");
                    if (SetThreadToken(NULL, hQueryToken)) {
                        DebugLog("[Impersonate] SetThreadToken with QUERY-only SUCCESS!\r\n");
                        pDesktop->hImpersonationToken = hQueryToken;
                        pDesktop->bImpersonating = TRUE;
                        return TRUE;
                    } else {
                        sprintf(buf, "[Impersonate] SetThreadToken failed: %lu\r\n", GetLastError());
                        DebugLog(buf);
                    }
                    CloseHandle(hQueryToken);
                    hQueryToken = NULL;
                }
            }
        } else {
            DebugLog("[Impersonate] NtOpenProcessToken not found in ntdll.dll\r\n");
        }
    }
    
    /* ===================================================================
     * ROBUST PATH: Brute-force DuplicateHandle scan of SYSTEM processes
     * 
     * Instead of NtQuerySystemInformation(SystemHandleInformation) which
     * has struct alignment issues on WoW64 (32-bit binary on 64-bit OS,
     * PVOID is 8 bytes natively but 4 in our struct), we directly try
     * DuplicateHandle with sequential handle values (4, 8, 12, ...).
     *
     * This is architecture-independent and works on any Windows version.
     * Handle values are always multiples of 4. Typical SYSTEM processes
     * have <2000 handles, so scanning up to handle 0x4000 is sufficient.
     * DuplicateHandle bypasses the token DACL (only checks process DACL,
     * which SeDebugPrivilege satisfies).
     * =================================================================== */
    if (!bGotToken) {
        DWORD dwMaxHandle = 0x4000; /* scan handle values 4..16384 */
        DWORD hVal;
        ULONG nTotalDuped = 0;
        ULONG nTotalTokens = 0;
        
        DebugLog("[Impersonate] Using brute-force DuplicateHandle scan (arch-independent)...\r\n");
        
        /* Build list of SYSTEM process PIDs */
        hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) {
            DebugLog("[Impersonate] FAILED: CreateToolhelp32Snapshot\r\n");
            return FALSE;
        }
        
        nSystemProcs = 0;
        pe32.dwSize = sizeof(pe32);
        if (Process32First(hSnap, &pe32)) {
            do {
                int j;
                for (j = 0; szSystemNames[j] != NULL; j++) {
                    if (_stricmp(pe32.szExeFile, szSystemNames[j]) == 0) {
                        if (nSystemProcs < 64) {
                            dwSystemPIDs[nSystemProcs] = pe32.th32ProcessID;
                            hSystemProcs[nSystemProcs] = NULL;
                            nSystemProcs++;
                        }
                        break;
                    }
                }
            } while (Process32Next(hSnap, &pe32));
        }
        CloseHandle(hSnap);
        
        sprintf(buf, "[Impersonate] Found %d SYSTEM processes to scan\r\n", nSystemProcs);
        DebugLog(buf);
        
        if (nSystemProcs == 0) {
            DebugLog("[Impersonate] FAILED: No SYSTEM processes found\r\n");
            return FALSE;
        }
        
        /* Open each SYSTEM process with PROCESS_DUP_HANDLE */
        for (p = 0; p < nSystemProcs; p++) {
            hSystemProcs[p] = OpenProcess(PROCESS_DUP_HANDLE, FALSE, dwSystemPIDs[p]);
            if (!hSystemProcs[p]) {
                /* Try with less specific rights */
                hSystemProcs[p] = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION, 
                                              FALSE, dwSystemPIDs[p]);
            }
            if (hSystemProcs[p]) {
                sprintf(buf, "[Impersonate]   PID %lu opened for DUP_HANDLE\r\n", dwSystemPIDs[p]);
            } else {
                sprintf(buf, "[Impersonate]   PID %lu OpenProcess failed: %lu\r\n", 
                        dwSystemPIDs[p], GetLastError());
            }
            DebugLog(buf);
        }
        
        /* For each SYSTEM process, try DuplicateHandle with handle values
         * 4, 8, 12, ..., dwMaxHandle. Check each dup'd handle for token. */
        for (p = 0; p < nSystemProcs && !bGotToken; p++) {
            ULONG nProcDuped = 0;
            ULONG nProcTokens = 0;
            
            if (!hSystemProcs[p]) continue;
            
            for (hVal = 4; hVal <= dwMaxHandle && !bGotToken; hVal += 4) {
                HANDLE hDup = NULL;
                TOKEN_STATISTICS ts;
                DWORD dwLen = 0;
                
                if (!DuplicateHandle(hSystemProcs[p], 
                                    (HANDLE)(ULONG_PTR)hVal,
                                    GetCurrentProcess(), &hDup,
                                    0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    continue;
                }
                nProcDuped++;
                
                /* Check if this is a token by calling GetTokenInformation */
                if (GetTokenInformation(hDup, TokenStatistics, &ts, sizeof(ts), &dwLen)) {
                    nProcTokens++;
                    sprintf(buf, "[Impersonate] Found %s token (handle 0x%lX) in PID %lu\r\n",
                            (ts.TokenType == TokenPrimary) ? "PRIMARY" : "IMPERSONATION",
                            (DWORD)hVal, dwSystemPIDs[p]);
                    DebugLog(buf);
                    hToken = hDup;
                    bGotToken = TRUE;
                    break;
                }
                
                CloseHandle(hDup);
            }
            
            sprintf(buf, "[Impersonate]   PID %lu: duped %lu handles, found %lu tokens\r\n",
                    dwSystemPIDs[p], nProcDuped, nProcTokens);
            DebugLog(buf);
            nTotalDuped += nProcDuped;
            nTotalTokens += nProcTokens;
        }
        
        sprintf(buf, "[Impersonate] TOTALS: %lu duped, %lu tokens\r\n",
                nTotalDuped, nTotalTokens);
        DebugLog(buf);
        
        /* Cleanup process handles */
        for (p = 0; p < nSystemProcs; p++) {
            if (hSystemProcs[p]) {
                CloseHandle(hSystemProcs[p]);
                hSystemProcs[p] = NULL;
            }
        }
        
        if (!bGotToken) {
            DebugLog("[Impersonate] FAILED: No SYSTEM token found in any process\r\n");
            return FALSE;
        }
    }
    
    /* Step 5: Duplicate token for impersonation */
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, 
                          SecurityImpersonation, TokenImpersonation, &hDupToken)) {
        DWORD dwDupErr = GetLastError();
        sprintf(buf, "[Impersonate] DuplicateTokenEx(MAXIMUM_ALLOWED) failed: %lu\r\n", dwDupErr);
        DebugLog(buf);
        
        /* Try with explicit minimum rights */
        if (!DuplicateTokenEx(hToken, TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_DUPLICATE,
                              NULL, SecurityImpersonation, TokenImpersonation, &hDupToken)) {
            sprintf(buf, "[Impersonate] DuplicateTokenEx(explicit) failed: %lu\r\n", GetLastError());
            DebugLog(buf);
            
            /* Last resort: try SetThreadToken with the raw token handle.
             * This may work if the token is already an impersonation token,
             * or if the system is lenient about primary tokens. */
            DebugLog("[Impersonate] Trying SetThreadToken with raw token...\r\n");
            if (SetThreadToken(NULL, hToken)) {
                DebugLog("[Impersonate] SetThreadToken with raw token SUCCESS!\r\n");
                pDesktop->hImpersonationToken = hToken;
                pDesktop->bImpersonating = TRUE;
                return TRUE;
            }
            sprintf(buf, "[Impersonate] SetThreadToken failed: %lu\r\n", GetLastError());
            DebugLog(buf);
            
            /* Try ImpersonateLoggedOnUser directly with the token */
            DebugLog("[Impersonate] Trying ImpersonateLoggedOnUser with raw token...\r\n");
            if (ImpersonateLoggedOnUser(hToken)) {
                DebugLog("[Impersonate] ImpersonateLoggedOnUser with raw token SUCCESS!\r\n");
                pDesktop->hImpersonationToken = hToken;
                pDesktop->bImpersonating = TRUE;
                return TRUE;
            }
            sprintf(buf, "[Impersonate] ImpersonateLoggedOnUser raw failed: %lu\r\n", GetLastError());
            DebugLog(buf);
            
            CloseHandle(hToken);
            return FALSE;
        }
    } else {
        DebugLog("[Impersonate] DuplicateTokenEx(MAXIMUM_ALLOWED) SUCCESS\r\n");
    }
    
    /* ---- Enable ALL privileges on hDupToken BEFORE impersonation ----
     * CRITICAL: Privileges must be enabled on hDupToken itself, not just on
     * the thread's impersonation token copy. When worker/input threads later
     * call ImpersonateLoggedOnUser(hDupToken), they inherit the privilege state
     * of hDupToken. If we only enable privileges on the thread copy (via
     * OpenThreadToken), those other threads get a token WITHOUT SeTcbPrivilege,
     * causing mouse_event/keybd_event to silently fail on the Winlogon desktop. */
    {
        TOKEN_PRIVILEGES tp;
        BOOL bEnabledTcb = FALSE;
        
        /* SeTcbPrivilege - THE key privilege for secure desktop access + input */
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueA(NULL, "SeTcbPrivilege", &tp.Privileges[0].Luid)) {
            if (AdjustTokenPrivileges(hDupToken, FALSE, &tp, 0, NULL, NULL)) {
                if (GetLastError() == ERROR_SUCCESS) {
                    DebugLog("[Impersonate] SeTcbPrivilege ENABLED on hDupToken\r\n");
                    bEnabledTcb = TRUE;
                } else {
                    DebugLog("[Impersonate] SeTcbPrivilege not held in hDupToken\r\n");
                }
            }
        }
        
        /* SeAssignPrimaryTokenPrivilege */
        if (LookupPrivilegeValueA(NULL, "SeAssignPrimaryTokenPrivilege", &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(hDupToken, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS) {
                DebugLog("[Impersonate] SeAssignPrimaryTokenPrivilege ENABLED on hDupToken\r\n");
            }
        }
        
        /* SeDebugPrivilege */
        if (LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (AdjustTokenPrivileges(hDupToken, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS) {
                DebugLog("[Impersonate] SeDebugPrivilege ENABLED on hDupToken\r\n");
            }
        }
        
        if (!bEnabledTcb) {
            DebugLog("[Impersonate] WARNING: SeTcbPrivilege could NOT be enabled on hDupToken - input injection may fail\r\n");
        }
    }
    
    /* Step 6: Impersonate! Thread now has SYSTEM security context
     * ImpersonateLoggedOnUser creates a COPY of hDupToken with the same
     * privilege state (including the privileges we just enabled above). */
    if (!ImpersonateLoggedOnUser(hDupToken)) {
        DWORD dwImpErr = GetLastError();
        sprintf(buf, "[Impersonate] ImpersonateLoggedOnUser failed: %lu, trying SetThreadToken...\r\n", dwImpErr);
        DebugLog(buf);
        
        /* Fallback: SetThreadToken */
        if (!SetThreadToken(NULL, hDupToken)) {
            sprintf(buf, "[Impersonate] SetThreadToken failed: %lu\r\n", GetLastError());
            DebugLog(buf);
            CloseHandle(hDupToken);
            CloseHandle(hToken);
            return FALSE;
        }
        DebugLog("[Impersonate] SetThreadToken SUCCESS (fallback)\r\n");
    } else {
        DebugLog("[Impersonate] ImpersonateLoggedOnUser SUCCESS\r\n");
    }
    
    DebugLog("[Impersonate] SUCCESS - Thread is now impersonating SYSTEM\r\n");
    
    /* Save token for later use by worker/input threads */
    pDesktop->hImpersonationToken = hDupToken;
    pDesktop->bImpersonating = TRUE;
    
    /* ---- Also enable privileges on the thread's impersonation token copy ----
     * Belt-and-suspenders: the thread copy was made from hDupToken (which now
     * has privileges enabled), but some OS versions may not copy the enabled
     * state. Explicitly enable on the thread token too. */
    {
        HANDLE hThreadToken = NULL;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, 
                            FALSE, &hThreadToken)) {
            TOKEN_PRIVILEGES tp;
            BOOL bEnabledTcb = FALSE;
            
            /* SeTcbPrivilege */
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueA(NULL, "SeTcbPrivilege", &tp.Privileges[0].Luid)) {
                if (AdjustTokenPrivileges(hThreadToken, FALSE, &tp, 0, NULL, NULL)) {
                    if (GetLastError() == ERROR_SUCCESS) {
                        DebugLog("[Impersonate] SeTcbPrivilege ENABLED on thread token\r\n");
                        bEnabledTcb = TRUE;
                    } else {
                        DebugLog("[Impersonate] SeTcbPrivilege not held in thread token\r\n");
                    }
                } else {
                    sprintf(buf, "[Impersonate] AdjustTokenPrivileges(SeTcb) failed: %lu\r\n", GetLastError());
                    DebugLog(buf);
                }
            }
            
            /* SeAssignPrimaryTokenPrivilege */
            if (LookupPrivilegeValueA(NULL, "SeAssignPrimaryTokenPrivilege", &tp.Privileges[0].Luid)) {
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                if (AdjustTokenPrivileges(hThreadToken, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS) {
                    DebugLog("[Impersonate] SeAssignPrimaryTokenPrivilege ENABLED on thread token\r\n");
                }
            }
            
            /* SeDebugPrivilege */
            if (LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid)) {
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                if (AdjustTokenPrivileges(hThreadToken, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS) {
                    DebugLog("[Impersonate] SeDebugPrivilege ENABLED\r\n");
                }
            }
            
            if (!bEnabledTcb) {
                DebugLog("[Impersonate] WARNING: SeTcbPrivilege could NOT be enabled - desktop access may fail\r\n");
            }
            
            CloseHandle(hThreadToken);
        } else {
            sprintf(buf, "[Impersonate] WARNING: OpenThreadToken for privilege adjust failed: %lu\r\n", GetLastError());
            DebugLog(buf);
        }
    }
    
    /* Clean up intermediate handles */
    CloseHandle(hToken);
    
    return TRUE;
}

void Desktop_RevertImpersonation(PDESKTOP_CONTEXT pDesktop)
{
    if (!pDesktop) return;
    
    if (pDesktop->bImpersonating) {
        RevertToSelf();
        DebugLog("[Impersonate] Reverted to self\r\n");
        pDesktop->bImpersonating = FALSE;
    }
    
    if (pDesktop->hImpersonationToken) {
        CloseHandle(pDesktop->hImpersonationToken);
        pDesktop->hImpersonationToken = NULL;
    }
}

/* ============================================================================
 * RECURSIVE PATTERN: Desktop Switching
 * ============================================================================ */

BOOL Desktop_SwitchToInput(PDESKTOP_CONTEXT pDesktop)
{
    char buf[512];
    
    if (!pDesktop || !pDesktop->hInputDesktop) {
        DebugLog("[SwitchToInput] No input desktop available\r\n");
        return FALSE;
    }
    
    if (!SetThreadDesktop(pDesktop->hInputDesktop)) {
        DWORD dwErr = GetLastError();
        
        /* PROFESSIONAL FIX (v5.4): ERROR 183 = ALREADY_EXISTS
         * This is BENIGN - thread is already on this desktop
         * Suppress logging to reduce noise (was causing 247K redundant log entries!)
         * Only log actual unexpected errors */
        if (dwErr != 183) {  /* ERROR_ALREADY_EXISTS = 183 */
            sprintf(buf, "[SwitchToInput] SetThreadDesktop failed with error %lu\r\n", dwErr);
            DebugLog(buf);
        }
        return FALSE;
    }
    
    sprintf(buf, "[SwitchToInput] Successfully switched to desktop '%s'\r\n",
            pDesktop->szInputDesktopName);
    DebugLog(buf);
    return TRUE;
}

/* ============================================================================
 * HELPER: Explicitly open Winlogon desktop by name (for secure desktop access)
 * This is different from OpenInputDesktop - it opens the NAMED Winlogon desktop
 * ============================================================================ */

/* Full access rights needed for BOTH screen capture AND input injection.
 * CRITICAL: Without DESKTOP_JOURNALPLAYBACK, mouse_event/keybd_event silently fail!
 * Without DESKTOP_WRITEOBJECTS, some input APIs may not work. */
#define DESKTOP_INPUT_ACCESS (DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | \
    DESKTOP_SWITCHDESKTOP | DESKTOP_JOURNALPLAYBACK | DESKTOP_JOURNALRECORD | \
    DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL)

/* EnumDesktops callback - logs each desktop name */
static BOOL CALLBACK EnumDesktopProc(LPSTR lpszDesktop, LPARAM lParam)
{
    char buf[512];
    (void)lParam;
    sprintf(buf, "[SwitchToWinlogon] EnumDesktops found: '%s'\r\n", lpszDesktop);
    DebugLog(buf);
    return TRUE;  /* continue enumeration */
}

BOOL Desktop_SwitchToWinlogon(PDESKTOP_CONTEXT pDesktop)
{
    HDESK hWinlogonDesktop = NULL;
    HWINSTA hWinSta = NULL;
    HWINSTA hOldWinSta = NULL;
    char buf[512];
    BOOL bNeedImpersonation = FALSE;
    BOOL bSwitchedWinSta = FALSE;
    DWORD dwErr;
    
    if (!pDesktop) {
        DebugLog("[SwitchToWinlogon] Invalid pDesktop\r\n");
        return FALSE;
    }
    
    /* Already on Winlogon? Skip redundant switch */
    if (pDesktop->bOnWinlogonDesktop && pDesktop->hWinlogonDesktop) {
        return TRUE;
    }
    
    /* Allow OS desktop transition to settle before attempting to access
     * the Winlogon desktop. In debug builds, logging I/O (fopen/fprintf/fclose
     * per DebugLog call, ~50 calls in this function) provides ~200ms of implicit
     * delay distributed throughout. In release builds, this function runs in
     * microseconds, often outrunning the OS desktop switch. This brief pause
     * gives the OS time to finalize the Winlogon desktop transition. */
    Sleep(200);
    
    /* ---- Attempt 1: Direct OpenDesktop (no impersonation, no winsta switch) ---- */
    hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, DESKTOP_INPUT_ACCESS);
    if (hWinlogonDesktop) {
        DebugLog("[SwitchToWinlogon] Direct OpenDesktop succeeded (full access)\r\n");
        goto got_desktop;
    }
    dwErr = GetLastError();
    sprintf(buf, "[SwitchToWinlogon] Direct OpenDesktop failed (error %lu)\r\n", dwErr);
    DebugLog(buf);
    
    /* ---- Attempt 2: Try OpenInputDesktop (opens whatever desktop has input focus) ---- */
    hWinlogonDesktop = OpenInputDesktop(0, FALSE, DESKTOP_INPUT_ACCESS);
    if (hWinlogonDesktop) {
        DebugLog("[SwitchToWinlogon] OpenInputDesktop succeeded (full access)\r\n");
        goto got_desktop;
    }
    dwErr = GetLastError();
    sprintf(buf, "[SwitchToWinlogon] OpenInputDesktop failed (error %lu)\r\n", dwErr);
    DebugLog(buf);
    
    /* ---- Impersonate SYSTEM with SeTcbPrivilege for remaining attempts ---- */
    if (!Desktop_ImpersonateWinlogon(pDesktop)) {
        DebugLog("[SwitchToWinlogon] Impersonation failed\r\n");
        return FALSE;
    }
    bNeedImpersonation = TRUE;
    
    /* ---- Attempt 3: OpenDesktop after impersonation (SeTcbPrivilege now enabled) ---- */
    hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, DESKTOP_INPUT_ACCESS);
    if (hWinlogonDesktop) {
        DebugLog("[SwitchToWinlogon] OpenDesktop after impersonation succeeded (full access)\r\n");
        goto got_desktop;
    }
    dwErr = GetLastError();
    sprintf(buf, "[SwitchToWinlogon] OpenDesktop after impersonation (full) failed (error %lu)\r\n", dwErr);
    DebugLog(buf);
    
    /* If full access fails, try GENERIC_ALL (may succeed where specific flags fail) */
    hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, GENERIC_ALL);
    if (hWinlogonDesktop) {
        DebugLog("[SwitchToWinlogon] OpenDesktop(GENERIC_ALL) after impersonation succeeded\r\n");
        goto got_desktop;
    }
    /* Try MAXIMUM_ALLOWED */
    hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, MAXIMUM_ALLOWED);
    if (hWinlogonDesktop) {
        DebugLog("[SwitchToWinlogon] OpenDesktop(MAXIMUM_ALLOWED) after impersonation succeeded\r\n");
        goto got_desktop;
    }
    dwErr = GetLastError();
    sprintf(buf, "[SwitchToWinlogon] All Attempt 3 variants failed (error %lu)\r\n", dwErr);
    DebugLog(buf);
    
    /* ---- Attempt 4: Open WinSta0 with SYSTEM, switch window station, then desktop ---- */
    hOldWinSta = GetProcessWindowStation();
    
    /* Try opening WinSta0 with full access as SYSTEM */
    hWinSta = OpenWindowStationA("WinSta0", FALSE, WINSTA_ALL_ACCESS);
    if (!hWinSta) {
        dwErr = GetLastError();
        sprintf(buf, "[SwitchToWinlogon] OpenWindowStation(WinSta0, ALL_ACCESS) failed: %lu\r\n", dwErr);
        DebugLog(buf);
        
        /* Try with less access */
        hWinSta = OpenWindowStationA("WinSta0", FALSE,
            WINSTA_READATTRIBUTES | WINSTA_ENUMERATE | WINSTA_READSCREEN |
            WINSTA_ACCESSCLIPBOARD | WINSTA_CREATEDESKTOP | WINSTA_WRITEATTRIBUTES |
            WINSTA_ACCESSGLOBALATOMS | WINSTA_EXITWINDOWS | WINSTA_ENUMDESKTOPS);
        if (!hWinSta) {
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] OpenWindowStation(WinSta0, specific) failed: %lu\r\n", dwErr);
            DebugLog(buf);
            
            /* Last try: MAXIMUM_ALLOWED */
            hWinSta = OpenWindowStationA("WinSta0", FALSE, MAXIMUM_ALLOWED);
        }
    }
    
    if (hWinSta) {
        DebugLog("[SwitchToWinlogon] OpenWindowStation(WinSta0) succeeded\r\n");
        
        if (SetProcessWindowStation(hWinSta)) {
            bSwitchedWinSta = TRUE;
            DebugLog("[SwitchToWinlogon] SetProcessWindowStation succeeded\r\n");
            
            /* Enumerate desktops in WinSta0 for diagnostics */
            DebugLog("[SwitchToWinlogon] Enumerating desktops in WinSta0:\r\n");
            if (!EnumDesktopsA(hWinSta, EnumDesktopProc, 0)) {
                sprintf(buf, "[SwitchToWinlogon] EnumDesktops failed: %lu\r\n", GetLastError());
                DebugLog(buf);
            }
            
            /* Now try OpenDesktop in the context of WinSta0 */
            hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, DESKTOP_INPUT_ACCESS);
            if (hWinlogonDesktop) {
                DebugLog("[SwitchToWinlogon] OpenDesktop after WinSta switch succeeded (full access)!\r\n");
                goto got_desktop;
            }
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] OpenDesktop after WinSta switch failed: %lu\r\n", dwErr);
            DebugLog(buf);
            
            /* Try with MAXIMUM_ALLOWED */
            hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, MAXIMUM_ALLOWED);
            if (hWinlogonDesktop) {
                DebugLog("[SwitchToWinlogon] OpenDesktop(MAXIMUM_ALLOWED) succeeded!\r\n");
                goto got_desktop;
            }
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] OpenDesktop(MAXIMUM_ALLOWED) failed: %lu\r\n", dwErr);
            DebugLog(buf);
            
            /* Try OpenInputDesktop in context of WinSta0 */
            hWinlogonDesktop = OpenInputDesktop(0, FALSE, DESKTOP_INPUT_ACCESS);
            if (hWinlogonDesktop) {
                DebugLog("[SwitchToWinlogon] OpenInputDesktop after WinSta switch succeeded (full access)!\r\n");
                goto got_desktop;
            }
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] OpenInputDesktop after WinSta switch failed: %lu\r\n", dwErr);
            DebugLog(buf);
            
            /* Try broader flags */
            hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, DESKTOP_INPUT_ACCESS);
            if (hWinlogonDesktop) {
                DebugLog("[SwitchToWinlogon] OpenDesktop(broad+input) after WinSta switch succeeded!\r\n");
                goto got_desktop;
            }
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] OpenDesktop(broad) after WinSta switch failed: %lu\r\n", dwErr);
            DebugLog(buf);
            
            /* ---- Try GENERIC_ALL (different from MAXIMUM_ALLOWED on some OS) ---- */
            hWinlogonDesktop = OpenDesktopA("Winlogon", 0, FALSE, GENERIC_ALL);
            if (hWinlogonDesktop) {
                DebugLog("[SwitchToWinlogon] OpenDesktop(GENERIC_ALL) succeeded!\r\n");
                goto got_desktop;
            }
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] OpenDesktop(GENERIC_ALL) failed: %lu\r\n", dwErr);
            DebugLog(buf);
        } else {
            dwErr = GetLastError();
            sprintf(buf, "[SwitchToWinlogon] SetProcessWindowStation failed: %lu\r\n", dwErr);
            DebugLog(buf);
        }
    }
    
    /* ---- Attempt 6: Brute-force DuplicateHandle for desktop handle ----
     * The Winlogon desktop DACL denies access even to SYSTEM tokens that
     * lack SeTcbPrivilege (only the actual winlogon.exe process gets it).
     * But winlogon.exe MUST hold an open handle to its "Winlogon" desktop.
     * We can DuplicateHandle that desktop handle into our process, just
     * like we did for the token. GetUserObjectInformationA(UOI_TYPE)
     * tells us if a handle is a "Desktop", and UOI_NAME gives the name.
     * This bypasses the desktop DACL entirely. */
    {
        HANDLE hWinlogonProc = NULL;
        DWORD dwWinlogonPID = 0;
        DWORD hVal;
        DWORD dwMaxHandle = 0x4000;
        HANDLE hSnap2;
        PROCESSENTRY32 pe;
        HDESK hFoundDesktop = NULL;
        char szType[128];
        char szName[256];
        DWORD dwLen;
        int nDesktopsFound = 0;
        int nDuped = 0;
        
        DebugLog("[SwitchToWinlogon] Trying brute-force DuplicateHandle for desktop handle...\r\n");
        
        /* Find winlogon.exe PID */
        hSnap2 = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap2 != INVALID_HANDLE_VALUE) {
            pe.dwSize = sizeof(pe);
            if (Process32First(hSnap2, &pe)) {
                do {
                    if (_stricmp(pe.szExeFile, "winlogon.exe") == 0) {
                        dwWinlogonPID = pe.th32ProcessID;
                        break;
                    }
                } while (Process32Next(hSnap2, &pe));
            }
            CloseHandle(hSnap2);
        }
        
        if (dwWinlogonPID == 0) {
            DebugLog("[SwitchToWinlogon] Cannot find winlogon.exe PID for desktop scan\r\n");
            goto all_failed;
        }
        
        sprintf(buf, "[SwitchToWinlogon] Scanning winlogon.exe PID %lu for desktop handles...\r\n", dwWinlogonPID);
        DebugLog(buf);
        
        hWinlogonProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, dwWinlogonPID);
        if (!hWinlogonProc) {
            sprintf(buf, "[SwitchToWinlogon] OpenProcess(winlogon) failed: %lu\r\n", GetLastError());
            DebugLog(buf);
            goto all_failed;
        }
        
        for (hVal = 4; hVal <= dwMaxHandle; hVal += 4) {
            HANDLE hDup = NULL;
            
            if (!DuplicateHandle(hWinlogonProc, (HANDLE)(ULONG_PTR)hVal,
                                GetCurrentProcess(), &hDup,
                                0, FALSE, DUPLICATE_SAME_ACCESS)) {
                continue;
            }
            nDuped++;
            
            /* Check if this is a Desktop object */
            szType[0] = '\0';
            dwLen = 0;
            if (!GetUserObjectInformationA(hDup, UOI_TYPE, szType, sizeof(szType), &dwLen)) {
                CloseHandle(hDup);
                continue;
            }
            
            if (_stricmp(szType, "Desktop") != 0) {
                /* Not a desktop - could be a WindowStation or other user object.
                 * Close and continue. Note: non-user-objects just fail the 
                 * GetUserObjectInformation call above. */
                if (_stricmp(szType, "WindowStation") == 0) {
                    /* Log window stations for info */
                    szName[0] = '\0';
                    GetUserObjectInformationA(hDup, UOI_NAME, szName, sizeof(szName), &dwLen);
                    sprintf(buf, "[SwitchToWinlogon]   Handle 0x%lX: WindowStation '%s'\r\n", hVal, szName);
                    DebugLog(buf);
                }
                CloseHandle(hDup);
                continue;
            }
            
            /* It's a Desktop! Get its name */
            szName[0] = '\0';
            dwLen = 0;
            GetUserObjectInformationA(hDup, UOI_NAME, szName, sizeof(szName), &dwLen);
            
            nDesktopsFound++;
            sprintf(buf, "[SwitchToWinlogon]   Handle 0x%lX: Desktop '%s'\r\n", hVal, szName);
            DebugLog(buf);
            
            if (_stricmp(szName, "Winlogon") == 0) {
                DebugLog("[SwitchToWinlogon] *** FOUND Winlogon desktop handle! ***\r\n");
                hFoundDesktop = (HDESK)hDup;
                break;
            }
            
            CloseHandle(hDup);
        }
        
        CloseHandle(hWinlogonProc);
        
        sprintf(buf, "[SwitchToWinlogon] Desktop scan: %d handles duped, %d desktops found\r\n", 
                nDuped, nDesktopsFound);
        DebugLog(buf);
        
        if (hFoundDesktop) {
            /* The brute-force handle may lack DESKTOP_JOURNALPLAYBACK, which is
             * required for mouse_event/keybd_event to work. Now that we're
             * impersonating SYSTEM with SeTcbPrivilege, try to re-open the
             * desktop by name with full input access rights. */
            {
                HDESK hUpgraded = OpenDesktopA("Winlogon", 0, FALSE, DESKTOP_INPUT_ACCESS);
                if (hUpgraded) {
                    DebugLog("[SwitchToWinlogon] Upgraded brute-force handle to DESKTOP_INPUT_ACCESS\r\n");
                    CloseHandle(hFoundDesktop);
                    hFoundDesktop = hUpgraded;
                } else {
                    /* Try GENERIC_ALL as second attempt */
                    hUpgraded = OpenDesktopA("Winlogon", 0, FALSE, GENERIC_ALL);
                    if (hUpgraded) {
                        DebugLog("[SwitchToWinlogon] Upgraded brute-force handle to GENERIC_ALL\r\n");
                        CloseHandle(hFoundDesktop);
                        hFoundDesktop = hUpgraded;
                    } else {
                        sprintf(buf, "[SwitchToWinlogon] WARNING: Could not upgrade brute-force handle (error %lu) - input injection may fail\r\n", GetLastError());
                        DebugLog(buf);
                    }
                }
            }
            hWinlogonDesktop = hFoundDesktop;
            DebugLog("[SwitchToWinlogon] Using brute-force Winlogon desktop handle\r\n");
            goto got_desktop;
        }
    }

all_failed:
    /* All attempts exhausted */
    DebugLog("[SwitchToWinlogon] All attempts failed.\r\n");
    if (bSwitchedWinSta && hOldWinSta) {
        SetProcessWindowStation(hOldWinSta);
    }
    if (hWinSta) {
        CloseWindowStation(hWinSta);
    }
    if (bNeedImpersonation) {
        Desktop_RevertImpersonation(pDesktop);
    }
    return FALSE;

got_desktop:
    
    DebugLog("[SwitchToWinlogon] Got desktop handle, calling SetThreadDesktop...\r\n");
    
    /* ---- Ensure impersonation token is available for worker threads ----
     * On Win7, OpenInputDesktop may succeed at Attempt 2 (before impersonation),
     * skipping Desktop_ImpersonateWinlogon(). But the worker thread that actually 
     * captures the screen needs an impersonation token with SeTcbPrivilege to
     * successfully call SetThreadDesktop on the Winlogon desktop. Without it,
     * SetThreadDesktop "succeeds" but the thread stays on the Default desktop.
     * Always ensure we have an impersonation token, regardless of which attempt
     * obtained the desktop handle. */
    if (!pDesktop->hImpersonationToken) {
        DebugLog("[SwitchToWinlogon] No impersonation token yet - acquiring for worker thread...\r\n");
        if (Desktop_ImpersonateWinlogon(pDesktop)) {
            bNeedImpersonation = TRUE;
            DebugLog("[SwitchToWinlogon] Impersonation token acquired for worker threads\r\n");
        } else {
            DebugLog("[SwitchToWinlogon] WARNING: Could not acquire impersonation token - worker thread may fail\r\n");
        }
    }
    
    /* Try to switch thread to Winlogon desktop */
    if (!SetThreadDesktop(hWinlogonDesktop)) {
        dwErr = GetLastError();
        sprintf(buf, "[SwitchToWinlogon] SetThreadDesktop failed with error %lu\r\n", dwErr);
        DebugLog(buf);
        
        /* SetThreadDesktop commonly fails because the main GUI thread has windows
         * (error 183 = ALREADY_EXISTS on some OS versions, but other error codes
         * can also occur depending on OS version and timing).
         * The desktop handle is still VALID regardless of this error.
         * The worker thread capture path in screen.c will spawn a clean thread
         * that CAN call SetThreadDesktop successfully. */
        
        /* Store handles - the desktop handle is still valid */
        if (bSwitchedWinSta) {
            pDesktop->hSavedWinSta = hOldWinSta;
            pDesktop->hWinSta0 = hWinSta;
        }
        pDesktop->hWinlogonDesktop = hWinlogonDesktop;
        pDesktop->bOnWinlogonDesktop = TRUE;
        pDesktop->dwDesktopSwitches++;
        
        DebugLog("[SwitchToWinlogon] SUCCESS (worker-thread mode) - desktop handle stored\r\n");
        return TRUE;
    }
    
    DebugLog("[SwitchToWinlogon] SUCCESS - thread is now on Winlogon desktop\r\n");
    
    /* Store window station handles for cleanup/restore */
    if (bSwitchedWinSta) {
        pDesktop->hSavedWinSta = hOldWinSta;
        pDesktop->hWinSta0 = hWinSta;
    }
    
    /* Store for later restoration. Keep impersonation active for capture! */
    pDesktop->hWinlogonDesktop = hWinlogonDesktop;
    pDesktop->bOnWinlogonDesktop = TRUE;
    pDesktop->dwDesktopSwitches++;
    
    return TRUE;
}

/* ============================================================================
 * RECURSIVE PATTERN: Restore Home Desktop
 * ============================================================================ */

BOOL Desktop_RestoreHome(PDESKTOP_CONTEXT pDesktop)
{
    char buf[512];
    
    if (!pDesktop || !pDesktop->hHomeDesktop) {
        DebugLog("[RestoreHome] No home desktop to restore\r\n");
        return FALSE;
    }
    
    /* Revert impersonation FIRST - must be our own identity to switch back */
    Desktop_RevertImpersonation(pDesktop);
    
    /* Restore original window station if we switched it */
    if (pDesktop->hSavedWinSta) {
        if (SetProcessWindowStation(pDesktop->hSavedWinSta)) {
            DebugLog("[RestoreHome] Restored original window station\r\n");
        }
    }
    if (pDesktop->hWinSta0) {
        CloseWindowStation(pDesktop->hWinSta0);
        pDesktop->hWinSta0 = NULL;
    }
    pDesktop->hSavedWinSta = NULL;
    
    if (!SetThreadDesktop(pDesktop->hHomeDesktop)) {
        DWORD dwErr = GetLastError();
        sprintf(buf, "[RestoreHome] SetThreadDesktop(home) failed with error %lu\r\n", dwErr);
        DebugLog(buf);
        return FALSE;
    }
    
    pDesktop->bOnWinlogonDesktop = FALSE;
    DebugLog("[RestoreHome] Successfully restored home desktop\r\n");
    return TRUE;
}

/* ============================================================================
 * HELPER: Check if desktop switched since last check
 * ============================================================================ */

BOOL Desktop_HasSwitched(PDESKTOP_CONTEXT pDesktop)
{
    DESKTOP_STATE eNewState;
    char buf[256];
    
    if (!pDesktop) return FALSE;
    
    eNewState = Desktop_DetectState(pDesktop);
    
    if (pDesktop->ePreviousState != eNewState) {
        sprintf(buf, "[HasSwitched] State change: %s -> %s\r\n",
                Desktop_StateToString(pDesktop->ePreviousState),
                Desktop_StateToString(eNewState));
        DebugLog(buf);
        pDesktop->ePreviousState = eNewState;
        return TRUE;
    }
    
    return FALSE;
}

/* ============================================================================
 * STATE TO STRING - for debugging
 * ============================================================================ */

const char* Desktop_StateToString(DESKTOP_STATE eState)
{
    switch (eState) {
        case DESKTOP_STATE_NORMAL:       return "NORMAL";
        case DESKTOP_STATE_UAC_ACTIVE:   return "UAC_ACTIVE";
        case DESKTOP_STATE_WINLOGON:     return "WINLOGON";
        case DESKTOP_STATE_UNAVAILABLE:  return "UNAVAILABLE";
        default:                         return "UNKNOWN";
    }
}

/* ============================================================================
 * INITIALIZATION - Save home desktop at startup
 * ============================================================================ */

PDESKTOP_CONTEXT Desktop_Init(void)
{
    PDESKTOP_CONTEXT pDesktop = NULL;
    char buf[512];
    
    pDesktop = (PDESKTOP_CONTEXT)calloc(1, sizeof(DESKTOP_CONTEXT));
    if (!pDesktop) {
        DebugLog("[Init] FAILED to allocate DESKTOP_CONTEXT\r\n");
        return NULL;
    }
    
    /* Save home desktop */
    pDesktop->hHomeDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (!pDesktop->hHomeDesktop) {
        DebugLog("[Init] FAILED to get home desktop\r\n");
        free(pDesktop);
        return NULL;
    }
    
    GetDesktopName(pDesktop->hHomeDesktop, pDesktop->szThreadDesktopName, sizeof(pDesktop->szThreadDesktopName));
    
    pDesktop->ePreviousState = DESKTOP_STATE_UNAVAILABLE;
    pDesktop->eCurrentState = DESKTOP_STATE_UNAVAILABLE;
    
    sprintf(buf, "[Init] Desktop context created, home desktop: '%s'\r\n",
            pDesktop->szThreadDesktopName);
    DebugLog(buf);
    
    return pDesktop;
}

/* ============================================================================
 * SHUTDOWN - Restore home desktop and cleanup
 * ============================================================================ */

void Desktop_Shutdown(PDESKTOP_CONTEXT pDesktop)
{
    char buf[512];
    
    if (!pDesktop) return;
    
    /* Revert impersonation if active */
    Desktop_RevertImpersonation(pDesktop);
    
    /* CRITICAL: Always restore home desktop before exit */
    if (pDesktop->hHomeDesktop) {
        if (!SetThreadDesktop(pDesktop->hHomeDesktop)) {
            DWORD dwErr = GetLastError();
            sprintf(buf, "[Shutdown] WARNING: Failed to restore home desktop with error %lu\r\n", dwErr);
            DebugLog(buf);
        } else {
            DebugLog("[Shutdown] Home desktop restored successfully\r\n");
        }
    }
    
    /* Restore window station if switched */
    if (pDesktop->hSavedWinSta) {
        SetProcessWindowStation(pDesktop->hSavedWinSta);
        pDesktop->hSavedWinSta = NULL;
    }
    if (pDesktop->hWinSta0) {
        CloseWindowStation(pDesktop->hWinSta0);
        pDesktop->hWinSta0 = NULL;
    }
    
    /* Close Winlogon desktop handle if open */
    if (pDesktop->hWinlogonDesktop) {
        CloseDesktop(pDesktop->hWinlogonDesktop);
        pDesktop->hWinlogonDesktop = NULL;
    }
    
    /* Close input desktop handle if open */
    if (pDesktop->hInputDesktop) {
        CloseDesktop(pDesktop->hInputDesktop);
        pDesktop->hInputDesktop = NULL;
    }
    
    /* Statistics */
    sprintf(buf, "[Shutdown] Statistics: %lu desktop switches, %lu UAC detections, %lu Winlogon detections\r\n",
            pDesktop->dwDesktopSwitches, pDesktop->dwUACDetections, pDesktop->dwWinlogonDetections);
    DebugLog(buf);
    
    free(pDesktop);
}

/* ============================================================================
 * SAS (Secure Attention Sequence - Ctrl+Alt+Del)
 * ============================================================================ */

typedef void (WINAPI *PFNSENDSA)(BOOL);

BOOL Desktop_SendSAS(void)
{
    HMODULE hSasLib;
    PFNSENDSA pfnSendSAS;
    DWORD dwErr;
    char buf[512];
    
    DebugLog("[SAS] Attempting to send Secure Attention Sequence...\r\n");
    
    /* Load sas.dll (Secure Attention Sequence library) */
    hSasLib = LoadLibraryA("sas.dll");
    if (!hSasLib) {
        dwErr = GetLastError();
        sprintf(buf, "[SAS] Failed to load sas.dll: error %lu\r\n", dwErr);
        DebugLog(buf);
        return FALSE;
    }
    
    /* Get SendSAS function pointer */
    pfnSendSAS = (PFNSENDSA)GetProcAddress(hSasLib, "SendSAS");
    if (!pfnSendSAS) {
        dwErr = GetLastError();
        sprintf(buf, "[SAS] Failed to get SendSAS function: error %lu\r\n", dwErr);
        DebugLog(buf);
        FreeLibrary(hSasLib);
        return FALSE;
    }
    
    /* Call SendSAS(FALSE) - asynchronous, non-user mode */
    __try {
        pfnSendSAS(FALSE);
        DebugLog("[SAS] SendSAS(FALSE) called successfully\r\n");
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        DebugLog("[SAS] SendSAS() raised exception\r\n");
        FreeLibrary(hSasLib);
        return FALSE;
    }
    
    FreeLibrary(hSasLib);
    return TRUE;
}

BOOL Desktop_ConfigureSASSupport(BOOL bEnable)
{
    HKEY hKey;
    DWORD dwValue, dwDisposition;
    LONG lRes;
    char buf[512];
    
    if (bEnable) {
        DebugLog("[SAS-Config] Enabling SAS support via registry...\r\n");
        dwValue = 1;  /* Enable SAS for Services */
    } else {
        DebugLog("[SAS-Config] Disabling SAS support via registry...\r\n");
        dwValue = 0;
    }
    
    /* Open/Create HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Policies\System */
    lRes = RegCreateKeyExA(
        HKEY_LOCAL_MACHINE,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, NULL, REG_OPTION_NON_VOLATILE,
        KEY_WRITE, NULL,
        &hKey, &dwDisposition
    );
    
    if (lRes != ERROR_SUCCESS) {
        sprintf(buf, "[SAS-Config] Failed to open registry key: %ld\r\n", lRes);
        DebugLog(buf);
        return FALSE;
    }
    
    /* Set SoftwareSASGeneration to 1 (Services) or 0 (disable) */
    lRes = RegSetValueExA(hKey, "SoftwareSASGeneration", 0, REG_DWORD,
                          (const BYTE*)&dwValue, sizeof(DWORD));
    
    if (lRes != ERROR_SUCCESS) {
        sprintf(buf, "[SAS-Config] Failed to set registry value: %ld\r\n", lRes);
        DebugLog(buf);
        RegCloseKey(hKey);
        return FALSE;
    }
    
    RegCloseKey(hKey);
    sprintf(buf, "[SAS-Config] SoftwareSASGeneration set to %lu\r\n", dwValue);
    DebugLog(buf);
    return TRUE;
}

/* ============================================================================
 * CAPTURE FALLBACK MODES
 * ============================================================================ */

BOOL Desktop_TryNextCaptureMode(PDESKTOP_CONTEXT pDesktop)
{
    char buf[512];
    
    if (!pDesktop) return FALSE;
    
    sprintf(buf, "[Fallback] Current mode: %s, failures: %lu\r\n",
            Desktop_CaptureModeToString(pDesktop->eCaptureMode),
            pDesktop->dwCaptureFailures);
    DebugLog(buf);
    
    switch (pDesktop->eCaptureMode) {
        case CAPTURE_MODE_TOKEN_HIJACKING:
            /* Try next layer */
            DebugLog("[Fallback] Layer 1 failed, moving to Layer 2 (Direct Capture)\r\n");
            pDesktop->eCaptureMode = CAPTURE_MODE_DIRECT_CAPTURE;
            return TRUE;
            
        case CAPTURE_MODE_DIRECT_CAPTURE:
            /* Try next layer */
            DebugLog("[Fallback] Layer 2 failed, moving to Layer 3 (Black with Message)\r\n");
            pDesktop->eCaptureMode = CAPTURE_MODE_BLACK_WITH_MSG;
            strcpy(pDesktop->szLastErrorMsg, "Desktop temporarily unavailable.\nThis may occur during UAC/Winlogon.");
            return TRUE;
            
        case CAPTURE_MODE_BLACK_WITH_MSG:
            /* Final fallback */
            DebugLog("[Fallback] Layer 3 failed, moving to Layer 4 (Disabled)\r\n");
            pDesktop->eCaptureMode = CAPTURE_MODE_DISABLED;
            return FALSE;
            
        case CAPTURE_MODE_DISABLED:
        default:
            DebugLog("[Fallback] All capture modes exhausted\r\n");
            return FALSE;
    }
}

const char* Desktop_CaptureModeToString(CAPTURE_MODE eMode)
{
    switch (eMode) {
        case CAPTURE_MODE_TOKEN_HIJACKING:
            return "Token Hijacking (Layer 1)";
        case CAPTURE_MODE_DIRECT_CAPTURE:
            return "Direct Capture + Validation (Layer 2)";
        case CAPTURE_MODE_BLACK_WITH_MSG:
            return "Black Screen + Message (Layer 3)";
        case CAPTURE_MODE_DISABLED:
            return "Disabled (Layer 4)";
        default:
            return "Unknown";
    }
}

