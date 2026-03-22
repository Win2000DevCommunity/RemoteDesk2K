/*
 * RemoteDesk2K - Screen Capture Implementation
 */

#include "screen.h"
#include <stdio.h>

/* Declared in client/input.c - switch input thread to/from Winlogon desktop */
extern void Input_SetWinlogonDesktop(HDESK hDesktop, HANDLE hToken);
extern void Input_ClearWinlogonDesktop(void);
/* Drain pending input events from worker thread already on Winlogon desktop */
extern int Input_DrainQueueDirect(void);

/* PROFESSIONAL FIX (v5.4): Three-tier logging system
 * Currently set to DEBUG (all logs). For production, set to WARNING or ERROR */
#define SCREEN_LOG_ERROR    1
#define SCREEN_LOG_WARNING  2
#define SCREEN_LOG_DEBUG    3

static int g_ScreenLogLevel = SCREEN_LOG_DEBUG;  /* Production: change to SCREEN_LOG_WARNING */

static void ScreenLog(const char *msg)
{
#ifdef RD2K_DEBUG
    /* Log at DEBUG level for backward compatibility with existing calls */
    if (g_ScreenLogLevel >= SCREEN_LOG_DEBUG) {
        FILE *f = fopen("rd2k_debug.log", "a");
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "[%02d:%02d:%02d.%03d] [SCREEN] %s", 
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
            fclose(f);
        }
    }
#else
    (void)msg;
#endif
}

/* ERROR-level logging for critical issues only */
static void ScreenLogError(const char *msg)
{
#ifdef RD2K_DEBUG
    if (g_ScreenLogLevel >= SCREEN_LOG_ERROR) {
        FILE *f = fopen("rd2k_debug.log", "a");
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "[%02d:%02d:%02d.%03d] [SCREEN] [ERROR] %s", 
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
            fclose(f);
        }
    }
#else
    (void)msg;
#endif
}

/* ============================================================================
 * HELPER: Draw message on black screen
 * ============================================================================ */

static void ScreenCapture_DrawMessageOnBlack(BYTE *pPixelData, int width, int height, 
                                             int bytesPerPixel, const char *szMessage)
{
    int x, y;
    
    if (!pPixelData || !szMessage) return;
    
    /* Fill entire screen with black */
    memset(pPixelData, 0x00, width * height * bytesPerPixel);
    
    /* Simple message: draw by filling a rectangle with white pixels in upper-left */
    if (bytesPerPixel >= 3) {
        /* Draw white text box (RGB 255,255,255) */
        for (y = 10; y < 100 && y < height; y++) {
            for (x = 10; x < 300 && x < width; x++) {
                int offset = (y * width + x) * bytesPerPixel;
                pPixelData[offset + 0] = 255;      /* B */
                pPixelData[offset + 1] = 255;      /* G */
                pPixelData[offset + 2] = 255;      /* R */
            }
        }
    }
}

void ScreenCapture_GetDimensions(int *pWidth, int *pHeight)
{
    if (pWidth) *pWidth = GetSystemMetrics(SM_CXSCREEN);
    if (pHeight) *pHeight = GetSystemMetrics(SM_CYSCREEN);
}

int ScreenCapture_GetColorDepth(void)
{
    HDC hdc = GetDC(NULL);
    int depth = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);
    ReleaseDC(NULL, hdc);
    return depth;
}

/* ============================================================================
 * WORKER THREAD CAPTURE: For Winlogon desktop when main thread can't switch
 * 
 * SetThreadDesktop fails with ERROR_ALREADY_EXISTS (183) when the calling
 * thread has windows/hooks. Solution: spawn a clean worker thread with NO 
 * windows that calls SetThreadDesktop(hWinlogonDesktop), then GetDC(NULL) 
 * returns the Winlogon desktop's DC. The worker does the full BitBlt capture
 * and writes pixels into the caller's buffer.
 * ============================================================================ */

typedef struct _WINLOGON_CAPTURE_PARAMS {
    HDESK   hDesktop;       /* Winlogon desktop handle */
    HANDLE  hToken;         /* Impersonation token (NULL if not needed) */
    int     width;          /* Screen width */
    int     height;         /* Screen height */
    int     bitsPerPixel;   /* Color depth */
    BYTE   *pPixelData;     /* Output: caller's pixel buffer */
    DWORD   pixelDataSize;  /* Size of pixel buffer */
    int     result;         /* Output: 0=success, -1=failure */
    char    szError[256];   /* Output: error description */
} WINLOGON_CAPTURE_PARAMS;

static DWORD WINAPI WinlogonCaptureThread(LPVOID lpParam)
{
    WINLOGON_CAPTURE_PARAMS *pParams = (WINLOGON_CAPTURE_PARAMS *)lpParam;
    HDC hdcScreen = NULL;
    HDC hdcMemory = NULL;
    HBITMAP hBitmap = NULL;
    HBITMAP hBitmapOld = NULL;
    BITMAPINFO bmpInfo;
    BYTE *pBits = NULL;
    DWORD dwErr;
    
    pParams->result = -1;
    pParams->szError[0] = '\0';
    
    /* FIX (v6.2): Wrap entire worker thread in SEH. An access violation in
     * this thread (bad DC, invalid desktop handle, etc.) would terminate the
     * entire process. With SEH, we catch the fault and return an error. */
    __try {
    
    /* Impersonate if we have a token (needed for desktop access) */
    if (pParams->hToken) {
        if (!ImpersonateLoggedOnUser(pParams->hToken)) {
            sprintf(pParams->szError, "Worker: ImpersonateLoggedOnUser failed: %lu", GetLastError());
            return 1;
        }
        
        /* Enable SeTcbPrivilege on THIS thread's impersonation token.
         * Even though hDupToken now has SeTcbPrivilege enabled, some OS
         * versions may not copy the enabled state to the thread token.
         * This is critical for mouse_event/keybd_event on Winlogon desktop. */
        {
            HANDLE hThreadTok = NULL;
            if (OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                FALSE, &hThreadTok)) {
                TOKEN_PRIVILEGES tp;
                DWORD dwTcbErr;
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                if (LookupPrivilegeValueA(NULL, "SeTcbPrivilege", &tp.Privileges[0].Luid)) {
                    AdjustTokenPrivileges(hThreadTok, FALSE, &tp, 0, NULL, NULL);
                    dwTcbErr = GetLastError();
                    ScreenLog(dwTcbErr == ERROR_SUCCESS 
                        ? "[WORKER] SeTcbPrivilege ENABLED on worker thread\r\n"
                        : "[WORKER] SeTcbPrivilege NOT available on worker thread\r\n");
                }
                CloseHandle(hThreadTok);
            } else {
                char errbuf[128];
                sprintf(errbuf, "[WORKER] OpenThreadToken failed: %lu\r\n", GetLastError());
                ScreenLog(errbuf);
            }
        }
    } else {
        ScreenLog("[WORKER] WARNING: No impersonation token provided!\r\n");
    }
    
    /* THIS IS THE KEY: clean thread with NO windows can switch desktop */
    if (!SetThreadDesktop(pParams->hDesktop)) {
        dwErr = GetLastError();
        sprintf(pParams->szError, "Worker: SetThreadDesktop failed: %lu", dwErr);
        if (pParams->hToken) RevertToSelf();
        return 1;
    }
    
    /* FIX (v6.2): Brief pause after SetThreadDesktop to let OS finalize
     * desktop graphics context. In debug mode, ScreenLog file I/O provides
     * implicit delay. In release mode, ScreenLog is a no-op. */
    Sleep(10);
    
    /* Log desktop we're now on for debugging */
    {
        char deskName[128] = {0};
        DWORD dwLen = 0;
        HDESK hCurDesk = GetThreadDesktop(GetCurrentThreadId());
        GetUserObjectInformationA(hCurDesk, 2 /* UOI_NAME */, deskName, sizeof(deskName), &dwLen);
        ScreenLog("[WORKER] SetThreadDesktop SUCCESS\r\n");
        {
            char logb[256];
            sprintf(logb, "[WORKER] Now on desktop: '%s' handle=0x%p\r\n", deskName, (void*)hCurDesk);
            ScreenLog(logb);
        }
    }
    
    /* NOW GetDC(NULL) returns the Winlogon desktop's DC! */
    hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        sprintf(pParams->szError, "Worker: GetDC(NULL) failed: %lu", GetLastError());
        if (pParams->hToken) RevertToSelf();
        return 1;
    }
    
    hdcMemory = CreateCompatibleDC(hdcScreen);
    if (!hdcMemory) {
        sprintf(pParams->szError, "Worker: CreateCompatibleDC failed");
        ReleaseDC(NULL, hdcScreen);
        if (pParams->hToken) RevertToSelf();
        return 1;
    }
    
    /* Create DIB section */
    ZeroMemory(&bmpInfo, sizeof(BITMAPINFO));
    bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmpInfo.bmiHeader.biWidth = pParams->width;
    bmpInfo.bmiHeader.biHeight = -pParams->height;  /* top-down */
    bmpInfo.bmiHeader.biPlanes = 1;
    bmpInfo.bmiHeader.biBitCount = (WORD)pParams->bitsPerPixel;
    bmpInfo.bmiHeader.biCompression = BI_RGB;
    
    hBitmap = CreateDIBSection(hdcMemory, &bmpInfo, DIB_RGB_COLORS,
                               (void**)&pBits, NULL, 0);
    if (!hBitmap || !pBits) {
        sprintf(pParams->szError, "Worker: CreateDIBSection failed");
        DeleteDC(hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        if (pParams->hToken) RevertToSelf();
        return 1;
    }
    
    hBitmapOld = (HBITMAP)SelectObject(hdcMemory, hBitmap);
    
    /* BitBlt from Winlogon desktop to our DIB.
     * FIX (v6.2): If BitBlt fails with ERROR_INVALID_HANDLE (6), the desktop
     * DC may have been invalidated by a transient OS state change. Retry once
     * after releasing and reacquiring the DC. */
    if (!BitBlt(hdcMemory, 0, 0, pParams->width, pParams->height,
                hdcScreen, 0, 0, SRCCOPY)) {
        DWORD dwBltErr = GetLastError();
        if (dwBltErr == 6) {  /* ERROR_INVALID_HANDLE - retry once */
            ScreenLog("[WORKER] BitBlt failed error 6 - retrying with fresh DC\r\n");
            ReleaseDC(NULL, hdcScreen);
            Sleep(100);
            hdcScreen = GetDC(NULL);
            if (hdcScreen && BitBlt(hdcMemory, 0, 0, pParams->width, pParams->height,
                                     hdcScreen, 0, 0, SRCCOPY)) {
                ScreenLog("[WORKER] BitBlt retry SUCCEEDED\r\n");
                goto bitblt_ok;
            }
            dwBltErr = GetLastError();
        }
        sprintf(pParams->szError, "Worker: BitBlt failed: %lu", dwBltErr);
        SelectObject(hdcMemory, hBitmapOld);
        DeleteObject(hBitmap);
        if (hdcMemory) DeleteDC(hdcMemory);
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        if (pParams->hToken) RevertToSelf();
        return 1;
    }
    
    bitblt_ok:
    
    /* Copy pixels to caller's buffer using proper DWORD-aligned stride.
     * CreateDIBSection produces DWORD-aligned rows. We must copy row-by-row
     * to ensure the destination buffer also has DWORD-aligned rows. */
    {
        int bytesPerPixel = pParams->bitsPerPixel / 8;
        DWORD dwSrcStride = (DWORD)((pParams->width * bytesPerPixel + 3) & ~3);  /* DIB stride */
        DWORD dwDstStride = dwSrcStride;  /* Same DWORD-aligned stride for dest */
        DWORD dwRowBytes = (DWORD)pParams->width * bytesPerPixel;  /* Actual pixel bytes per row */
        int row;
        DWORD dwTotalBytes = dwDstStride * (DWORD)pParams->height;
        
        if (dwTotalBytes > pParams->pixelDataSize) {
            dwTotalBytes = pParams->pixelDataSize;
        }
        /* Row-by-row copy to handle stride properly */
        for (row = 0; row < pParams->height; row++) {
            DWORD offset = (DWORD)row * dwDstStride;
            if (offset + dwRowBytes > pParams->pixelDataSize) break;
            memcpy(pParams->pPixelData + offset, pBits + (row * dwSrcStride), dwRowBytes);
        }
    }
    
    /* CRITICAL: Inject any pending input events while we're on the Winlogon desktop.
     * mouse_event/keybd_event must be called from a thread that is ON the Winlogon
     * desktop. The normal input thread's SetThreadDesktop approach doesn't work
     * (process-level security check), but THIS worker thread's context does work
     * (same context that just did BitBlt successfully). */
    ScreenLog("[WORKER] About to call Input_DrainQueueDirect()...\r\n");
    {
        int nInjected = Input_DrainQueueDirect();
        char drainLog[128];
        sprintf(drainLog, "[WORKER] Input_DrainQueueDirect returned: %d events injected\r\n", nInjected);
        ScreenLog(drainLog);
    }
    
    /* Cleanup */
    SelectObject(hdcMemory, hBitmapOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMemory);
    ReleaseDC(NULL, hdcScreen);
    
    if (pParams->hToken) RevertToSelf();
    
    pParams->result = 0;  /* SUCCESS */
    
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        /* Caught an access violation or other fault in the worker thread.
         * Without this handler, the exception would terminate the process. */
        sprintf(pParams->szError, "Worker: SEH exception 0x%08lX", GetExceptionCode());
        pParams->result = -1;
        /* Best-effort cleanup */
        if (pParams->hToken) RevertToSelf();
    }
    
    return (pParams->result == 0) ? 0 : 1;
}

/* Capture the Winlogon desktop using a worker thread.
 * Returns 0 on success, -1 on failure. */
static int ScreenCapture_CaptureWinlogonWorker(PSCREEN_CAPTURE pCapture)
{
    WINLOGON_CAPTURE_PARAMS params;
    HANDLE hThread;
    DWORD dwWait;
    char logbuf[512];
    
    if (!pCapture || !pCapture->pDesktopContext || 
        !pCapture->pDesktopContext->hWinlogonDesktop) {
        ScreenLog("[CAPTURE] Worker: No Winlogon desktop handle\r\n");
        return -1;
    }
    
    ZeroMemory(&params, sizeof(params));
    params.hDesktop = pCapture->pDesktopContext->hWinlogonDesktop;
    params.hToken = pCapture->pDesktopContext->hImpersonationToken;
    params.width = pCapture->width;
    params.height = pCapture->height;
    params.bitsPerPixel = pCapture->bitsPerPixel;
    params.pPixelData = pCapture->pPixelData;
    params.pixelDataSize = pCapture->pixelDataSize;
    params.result = -1;
    
    hThread = CreateThread(NULL, 0, WinlogonCaptureThread, &params, 0, NULL);
    if (!hThread) {
        sprintf(logbuf, "[CAPTURE] Worker: CreateThread failed: %lu\r\n", GetLastError());
        ScreenLog(logbuf);
        return -1;
    }
    
    /* Wait up to 5 seconds for capture to complete */
    dwWait = WaitForSingleObject(hThread, 5000);
    if (dwWait != WAIT_OBJECT_0) {
        ScreenLog("[CAPTURE] Worker: Thread timed out\r\n");
        TerminateThread(hThread, 1);
        CloseHandle(hThread);
        return -1;
    }
    
    CloseHandle(hThread);
    
    if (params.result != 0) {
        sprintf(logbuf, "[CAPTURE] Worker FAILED: %s\r\n", params.szError);
        ScreenLog(logbuf);
        return -1;
    }
    
    ScreenLog("[CAPTURE] Worker: Winlogon desktop captured successfully!\r\n");
    return 0;
}





BOOL ScreenCapture_SyncDisplayMode(PSCREEN_CAPTURE pCapture)
{
    const DDRAW_SCREEN_INFO *pDisplayInfo;
    int newWidth, newHeight;
    char buf[256];
    
    if (!pCapture || !pCapture->pDDrawContext) {
        return FALSE;
    }
    
    /* Get DirectDraw's display mode - this is the TRUE framebuffer size! */
    pDisplayInfo = DDraw_GetScreenInfo((DDRAW_CONTEXT *)pCapture->pDDrawContext);
    if (!pDisplayInfo) {
        ScreenLog("[SYNC_DISPLAY] ERROR: DDraw_GetScreenInfo returned NULL\r\n");
        return FALSE;
    }
    
    newWidth = pDisplayInfo->dwWidth;
    newHeight = pDisplayInfo->dwHeight;
    
    sprintf(buf, "[SYNC_DISPLAY] DirectDraw reports: %dx%d (current: %dx%d)\r\n",
            newWidth, newHeight, pCapture->width, pCapture->height);
    ScreenLog(buf);
    
    /* If dimensions match, nothing to do */
    if (newWidth == pCapture->width && newHeight == pCapture->height) {
        ScreenLog("[SYNC_DISPLAY] Dimensions match - no reallocation needed\r\n");
        return TRUE;
    }
    
    /* Dimensions mismatch - need to reallocate buffers to match DirectDraw's TRUE size */
    {
        int bytesPerPixel = pCapture->bitsPerPixel / 8;
        DWORD newPixelDataSize, newCompressBufferSize;
        BYTE *pNewPixelData, *pNewPrevFrame, *pNewCompressBuffer;
        
        if (bytesPerPixel <= 0) bytesPerPixel = 3;
        
        /* Calculate new buffer sizes based on DirectDraw dimensions */
        newPixelDataSize = ((newWidth * bytesPerPixel + 3) & ~3) * newHeight;
        newCompressBufferSize = newPixelDataSize + (newPixelDataSize / 8) + 256;
        
        sprintf(buf, "[SYNC_DISPLAY] Reallocating: old=%d bytes, new=%d bytes\r\n",
                pCapture->pixelDataSize, newPixelDataSize);
        ScreenLog(buf);
        
        /* Allocate new buffers (recursive pattern - allocate all before freeing old) */
        pNewPixelData = (BYTE*)calloc(1, newPixelDataSize);
        pNewPrevFrame = (BYTE*)calloc(1, newPixelDataSize);
        pNewCompressBuffer = (BYTE*)calloc(1, newCompressBufferSize);
        
        if (!pNewPixelData || !pNewPrevFrame || !pNewCompressBuffer) {
            ScreenLog("[SYNC_DISPLAY] ERROR: Failed to allocate new buffers\r\n");
            /* Recursive cleanup - free what we allocated before returning */
            if (pNewPixelData) free(pNewPixelData);
            if (pNewPrevFrame) free(pNewPrevFrame);
            if (pNewCompressBuffer) free(pNewCompressBuffer);
            return FALSE;
        }
        
        /* Recursive cleanup pattern: free old buffers after successful allocation */
        SAFE_FREE(pCapture->pPixelData);
        SAFE_FREE(pCapture->pPrevFrame);
        SAFE_FREE(pCapture->pCompressBuffer);
        
        /* Update structure with new buffers and dimensions */
        pCapture->pPixelData = pNewPixelData;
        pCapture->pPrevFrame = pNewPrevFrame;
        pCapture->pCompressBuffer = pNewCompressBuffer;
        pCapture->width = newWidth;
        pCapture->height = newHeight;
        pCapture->pixelDataSize = newPixelDataSize;
        pCapture->compressBufferSize = newCompressBufferSize;
        
        sprintf(buf, "[SYNC_DISPLAY] Reallocation COMPLETE: now %dx%d @ %d bytes/pixel\r\n",
                pCapture->width, pCapture->height, bytesPerPixel);
        ScreenLog(buf);
        
        return TRUE;
    }
}

PSCREEN_CAPTURE ScreenCapture_Create(void)
{
    PSCREEN_CAPTURE pCapture;
    HDC hdcScreen;
    char buf[256];
    
    ScreenLog("[CREATE] Starting screen capture initialization\r\n");
    
    pCapture = (PSCREEN_CAPTURE)calloc(1, sizeof(SCREEN_CAPTURE));
    if (!pCapture) {
        ScreenLog("[CREATE] FAILED to allocate PSCREEN_CAPTURE\r\n");
        return NULL;
    }
    
    ScreenLog("[CREATE] Allocated pCapture structure\r\n");
    
    /* Initialize desktop context for UAC/Winlogon detection */
    pCapture->pDesktopContext = Desktop_Init();
    if (!pCapture->pDesktopContext) {
        ScreenLog("[CREATE] WARNING: Desktop context initialization failed\r\n");
    } else {
        ScreenLog("[CREATE] Desktop context initialized for desktop switch detection\r\n");
    }
    
    ScreenCapture_GetDimensions(&pCapture->width, &pCapture->height);
    pCapture->bitsPerPixel = ScreenCapture_GetColorDepth();
    
    sprintf(buf, "[CREATE] Screen dimensions: %dx%d, BPP=%d\r\n", 
            pCapture->width, pCapture->height, pCapture->bitsPerPixel);
    ScreenLog(buf);
    
    /* DIB section will be created fresh on each frame capture */
    ScreenLog("[CREATE] DIB creation deferred to frame capture\r\n");
    
    /* CRITICAL: Calculate pixelDataSize based on ACTUAL bits per pixel! */
    {
        int bytesPerPixel = pCapture->bitsPerPixel / 8;
        if (bytesPerPixel <= 0) bytesPerPixel = 3;  /* Default to 24-bit if detection fails */
        pCapture->pixelDataSize = ((pCapture->width * bytesPerPixel + 3) & ~3) * pCapture->height;
    }
    
    sprintf(buf, "[CREATE] Calculated pixelDataSize = %d bytes (BPP=%d bytes/pixel)\r\n", 
            pCapture->pixelDataSize, pCapture->bitsPerPixel / 8);
    ScreenLog(buf);
    
    /* Allocate persistent pixel buffer to copy data into from each frame's DIB */
    pCapture->pPixelData = (BYTE*)calloc(1, pCapture->pixelDataSize);
    if (!pCapture->pPixelData) {
        ScreenLog("[CREATE] FAILED to allocate pPixelData buffer\r\n");
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Allocated persistent pixel buffer\r\n");
    
    /* CRITICAL FIX: Initialize pPrevFrame to 0xFF (white) NOT 0x00 (black)!
     * If initialized to 0x00, first frame from Winlogon/UAC (also black) 
     * will compare equal and FindDirtyRects returns 0 rectangles.
     * Initialize to 0xFF ensures first frame is ALWAYS detected as dirty.
     * Subsequent frames use real delta detection. */
    pCapture->pPrevFrame = (BYTE*)malloc(pCapture->pixelDataSize);
    if (!pCapture->pPrevFrame) {
        ScreenLog("[CREATE] FAILED to allocate pPrevFrame\r\n");
        free(pCapture->pPixelData);
        free(pCapture);
        return NULL;
    }
    /* Initialize pPrevFrame to 0xFF (white) to force first frame detection */
    memset(pCapture->pPrevFrame, 0xFF, pCapture->pixelDataSize);
    
    pCapture->compressBufferSize = pCapture->pixelDataSize + (pCapture->pixelDataSize / 8) + 256;
    pCapture->pCompressBuffer = (BYTE*)calloc(1, pCapture->compressBufferSize);
    
    if (!pCapture->pCompressBuffer) {
        ScreenLog("[CREATE] FAILED to allocate compress buffer\r\n");
        if (pCapture->pPixelData) free(pCapture->pPixelData);
        if (pCapture->pPrevFrame) free(pCapture->pPrevFrame);
        if (pCapture->pCompressBuffer) free(pCapture->pCompressBuffer);
        free(pCapture);
        return NULL;
    }
    
    /* Initialize DirectDraw GPU acceleration if available */
    pCapture->pDDrawContext = DDraw_CreateContext();
    if (pCapture->pDDrawContext) {
        DDRAW_STATUS status = DDraw_Initialize((DDRAW_CONTEXT *)pCapture->pDDrawContext);
        if (status == DDRAW_STATUS_SUCCESS) {
            pCapture->bUseDDraw = TRUE;
            ScreenLog("[CREATE] DirectDraw GPU acceleration ENABLED\r\n");
            
            /* CRITICAL: DirectDraw outputs 24-bit BGR (after BGRA->BGR conversion) */
            if (pCapture->bitsPerPixel == 32) {
                int newBpp;
                DWORD newPixelDataSize;
                ScreenLog("[CREATE] Adjusting bit depth: 32-bit -> 24-bit (DirectDraw outputs 24-bit BGR)\r\n");
                pCapture->bitsPerPixel = 24;  /* DirectDraw output is 24-bit BGR */
                
                /* CRITICAL FIX: Recalculate pixelDataSize for 24-bit DWORD-aligned rows.
                 * Must match the stride used by FindDirtyRects, SendScreenUpdate, and viewer. */
                newBpp = 3;  /* 24 bits / 8 = 3 bytes per pixel */
                newPixelDataSize = ((pCapture->width * newBpp + 3) & ~3) * pCapture->height;
                
                if (newPixelDataSize != pCapture->pixelDataSize) {
                    char buf2[256];
                    sprintf(buf2, "[CREATE] Recalculating pixelDataSize: %d -> %d (24-bit DWORD-aligned)\r\n",
                            pCapture->pixelDataSize, newPixelDataSize);
                    ScreenLog(buf2);
                    pCapture->pixelDataSize = newPixelDataSize;
                    
                    /* Reallocate buffers at new (smaller) size */
                    {
                        BYTE *pNew = (BYTE*)calloc(1, newPixelDataSize);
                        BYTE *pNewPrev = (BYTE*)malloc(newPixelDataSize);
                        if (pNew && pNewPrev) {
                            free(pCapture->pPixelData);
                            free(pCapture->pPrevFrame);
                            pCapture->pPixelData = pNew;
                            pCapture->pPrevFrame = pNewPrev;
                            memset(pCapture->pPrevFrame, 0xFF, newPixelDataSize);
                            ScreenLog("[CREATE] Reallocated pixel buffers for 24-bit\r\n");
                        } else {
                            if (pNew) free(pNew);
                            if (pNewPrev) free(pNewPrev);
                            ScreenLog("[CREATE] WARNING: realloc failed, keeping 32-bit sized buffers\r\n");
                        }
                    }
                }
            }
            
            /* CRITICAL: Sync display dimensions from DirectDraw (the SDK authoritative source!)
             * DirectDraw reports the true framebuffer size, not the Windows WORKAREA */
            if (!ScreenCapture_SyncDisplayMode(pCapture)) {
                ScreenLog("[CREATE] WARNING: Failed to sync display mode from DirectDraw\r\n");
                /* Don't fail - continue with current dimensions */
            }
        } else {
            pCapture->bDDrawFailed = TRUE;
            DDraw_DestroyContext(&pCapture->pDDrawContext);
            pCapture->pDDrawContext = NULL;
        }
    }
    
    ScreenLog("[CREATE] Screen capture initialization COMPLETE\r\n");
    return pCapture;
}

void ScreenCapture_Destroy(PSCREEN_CAPTURE pCapture)
{
    if (!pCapture) return;
    
    /* Shutdown desktop context FIRST (restores home desktop) */
    if (pCapture->pDesktopContext) {
        Desktop_Shutdown(pCapture->pDesktopContext);
        pCapture->pDesktopContext = NULL;
    }
    
    /* Cleanup DirectDraw context */
    if (pCapture->pDDrawContext) {
        DDraw_Cleanup((DDRAW_CONTEXT *)pCapture->pDDrawContext);
        DDraw_DestroyContext(&pCapture->pDDrawContext);
    }
    
    /* Free all allocated buffers (recursive cleanup pattern) */
    SAFE_FREE(pCapture->pPixelData);      /* Persistent pixel buffer */
    SAFE_FREE(pCapture->pPrevFrame);       /* Previous frame for delta detection */
    SAFE_FREE(pCapture->pCompressBuffer);  /* Compression output buffer */
    
    /* Device contexts and DIB are created/destroyed per-frame, not cached */
    
    free(pCapture);
}

int ScreenCapture_CaptureScreen(PSCREEN_CAPTURE pCapture)
{
    HDC hdcScreen, hdcMemory;
    HBITMAP hBitmap, hBitmapOld;
    BYTE *pPixelData;
    int result = RD2K_ERR_SCREEN;
    char buf[256];
    DDRAW_STATUS ddStatus;
    DWORD dwBytesWritten;
    DESKTOP_STATE eDesktopState;
    BOOL bForceGDI = FALSE;  /* TRUE = skip DirectDraw, use GDI only (Winlogon) */
    
    /* Debounce: prevent flickering between Winlogon and Normal desktop.
     * Without logging I/O delays, detection can flip NORMAL for a single frame
     * during transient OS states, causing token/desktop cleanup and re-acquisition.
     * Require NORMAL to persist for at least 2 seconds before actually switching back. */
    static DWORD dwLastWinlogonTime = 0;
    static int nConsecutiveNormal = 0;
    static int nWorkerConsecutiveFailures = 0;
    #define WINLOGON_DEBOUNCE_MS 500
    
    if (!pCapture) {
        ScreenLog("[CAPTURE] Invalid pCapture\r\n");
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Starting screen capture\r\n");
    
    /* ========================================================================
     * CRITICAL: Detect desktop switches (UAC, Winlogon) BEFORE capture
     * UltraVNC/TigerVNC pattern: switch thread to input desktop for capture
     * ======================================================================== */
    
    if (pCapture->pDesktopContext) {
        eDesktopState = Desktop_DetectState(pCapture->pDesktopContext);
        
        if (eDesktopState == DESKTOP_STATE_WINLOGON) {
            /* ============================================================
             * WINLOGON DETECTED - Switch thread to Winlogon desktop
             * 
             * UltraVNC pattern:
             * 1. OpenDesktop("Winlogon") with minimal flags
             * 2. SetThreadDesktop() to switch capture thread
             * 3. GetDC(NULL) now returns the Winlogon desktop DC
             * 4. Capture via GDI BitBlt (DirectDraw CANNOT work here)
             * 5. Restore home desktop after capture
             * ============================================================ */
            bForceGDI = TRUE;  /* DirectDraw fails on Winlogon (0x80000006) */
            dwLastWinlogonTime = GetTickCount();  /* Reset debounce timer */
            nConsecutiveNormal = 0;               /* Reset normal counter */
            
            if (!pCapture->pDesktopContext->bOnWinlogonDesktop) {
                BOOL bSwitched;
                ScreenLog("[CAPTURE] Winlogon detected - switching to Winlogon desktop\r\n");
                bSwitched = Desktop_SwitchToWinlogon(pCapture->pDesktopContext);
                if (!bSwitched) {
                    /* First attempt failed - OS desktop transition may still be settling.
                     * Retry once after a brief delay. In debug builds, the hundreds of
                     * DebugLog file I/O calls inside SwitchToWinlogon provide ~200ms of
                     * implicit delay. In release builds, we need this explicit retry. */
                    Sleep(250);
                    bSwitched = Desktop_SwitchToWinlogon(pCapture->pDesktopContext);
                }
                if (bSwitched) {
                    ScreenLog("[CAPTURE] Successfully switched to Winlogon desktop for capture\r\n");
                    /* Also switch input thread to Winlogon desktop for mouse/keyboard */
                    if (pCapture->pDesktopContext->hWinlogonDesktop &&
                        pCapture->pDesktopContext->hImpersonationToken) {
                        Input_SetWinlogonDesktop(
                            pCapture->pDesktopContext->hWinlogonDesktop,
                            pCapture->pDesktopContext->hImpersonationToken);
                        ScreenLog("[CAPTURE] Input thread switching to Winlogon desktop\r\n");
                    }
                } else {
                    ScreenLog("[CAPTURE] WARNING: Could not switch to Winlogon desktop\r\n");
                    /* Continue anyway - GDI will capture whatever desktop we're on */
                }
            } else {
                ScreenLog("[CAPTURE] Already on Winlogon desktop, capturing via GDI\r\n");
                /* Ensure input thread is also on Winlogon (idempotent - safe to call repeatedly) */
                if (pCapture->pDesktopContext->hWinlogonDesktop &&
                    pCapture->pDesktopContext->hImpersonationToken) {
                    Input_SetWinlogonDesktop(
                        pCapture->pDesktopContext->hWinlogonDesktop,
                        pCapture->pDesktopContext->hImpersonationToken);
                }
            }
        } else if (eDesktopState == DESKTOP_STATE_NORMAL) {
            /* Back to normal desktop - restore if we were on Winlogon */
            if (pCapture->pDesktopContext->bOnWinlogonDesktop) {
                /* DEBOUNCE: Don't immediately leave Winlogon mode on a single NORMAL detection.
                 * Without logging I/O delays, detection can transiently read NORMAL during
                 * OS state transitions, causing unnecessary token/desktop teardown+reacquire.
                 * Require NORMAL to persist for WINLOGON_DEBOUNCE_MS before switching back. */
                DWORD dwNow = GetTickCount();
                DWORD dwElapsed = dwNow - dwLastWinlogonTime;
                nConsecutiveNormal++;
                
                if (dwElapsed < WINLOGON_DEBOUNCE_MS) {
                    /* Still within debounce window - stay on Winlogon desktop */
                    ScreenLog("[CAPTURE] NORMAL detected but within debounce window - staying on Winlogon\r\n");
                    bForceGDI = TRUE;  /* Still need GDI path for Winlogon capture */
                } else {
                    /* Debounce expired - actually switch back to normal */
                    ScreenLog("[CAPTURE] Desktop returned to normal - restoring home desktop\r\n");
                    nConsecutiveNormal = 0;
                    /* CRITICAL ORDER: Clear input thread globals FIRST (before closing handles).
                     * This prevents a race where the input thread tries ImpersonateLoggedOnUser
                     * with a handle that Desktop_RestoreHome already closed. */
                    Input_ClearWinlogonDesktop();
                    ScreenLog("[CAPTURE] Input thread switching back to Default desktop\r\n");
                    Desktop_RestoreHome(pCapture->pDesktopContext);
                    pCapture->pDesktopContext->bOnWinlogonDesktop = FALSE;
                    /* Close the Winlogon desktop handle */
                    if (pCapture->pDesktopContext->hWinlogonDesktop) {
                        CloseDesktop(pCapture->pDesktopContext->hWinlogonDesktop);
                        pCapture->pDesktopContext->hWinlogonDesktop = NULL;
                    }
                    /* Force DirectDraw re-init since old surfaces are invalid after desktop switch */
                    pCapture->bDDrawFailed = TRUE;
                    /* CRITICAL: Reset capture mode to initial state - it may have fallen
                     * to DISABLED during Winlogon. Without this reset, all future captures
                     * stay in DISABLED mode even after returning to normal desktop. */
                    pCapture->pDesktopContext->eCaptureMode = CAPTURE_MODE_TOKEN_HIJACKING;
                    pCapture->pDesktopContext->dwCaptureFailures = 0;
                    ScreenLog("[CAPTURE] Home desktop restored, capture mode reset, DirectDraw will re-init\r\n");
                }
            }
        } else if (eDesktopState == DESKTOP_STATE_UAC_ACTIVE) {
            bForceGDI = TRUE;  /* DirectDraw also fails on UAC desktop */
            ScreenLog("[CAPTURE] UAC desktop detected - forcing GDI capture\r\n");
        }
    }
    
    /* ATTEMPT 1: Try DirectDraw GPU-accelerated capture (skip on Winlogon/UAC) */
    /* FIX (v6.2): DDraw failure counter. After 3 consecutive failures, stop
     * trying DDraw recovery — the surfaces are permanently dead (e.g., after
     * desktop switch). This eliminates the ~150ms overhead per frame of
     * ValidateAndRecover + CopyFrameBuffer failing on every single frame.
     * Counter resets on DDraw success or explicit resolution change. */
    #define DDRAW_MAX_CONSECUTIVE_FAILURES 3
    {
        static int nDDrawConsecutiveFailures = 0;
        
        if (!bForceGDI && pCapture->bUseDDraw && pCapture->pDDrawContext && pCapture->bDDrawFailed) {
            if (nDDrawConsecutiveFailures >= DDRAW_MAX_CONSECUTIVE_FAILURES) {
                /* DDraw has failed too many times in a row — skip it entirely */
            } else {
                DDRAW_STATUS recoverStatus = DDraw_ValidateAndRecover((DDRAW_CONTEXT *)pCapture->pDDrawContext);
                if (recoverStatus == DDRAW_STATUS_SUCCESS || recoverStatus == DDRAW_STATUS_RECOVERED) {
                    ScreenLog("[CAPTURE] DirectDraw recovered from previous failure\r\n");
                    pCapture->bDDrawFailed = FALSE;
                }
            }
        }
        
        if (!bForceGDI && pCapture->bUseDDraw && pCapture->pDDrawContext && !pCapture->bDDrawFailed) {
            ScreenLog("[CAPTURE] Attempting DirectDraw GPU capture...\r\n");
            
            if (!ScreenCapture_SyncDisplayMode(pCapture)) {
                ScreenLog("[CAPTURE] WARNING: Failed to sync display mode (continuing with current buffer size)\r\n");
            }
            
            ddStatus = DDraw_CopyFrameBuffer(
                (DDRAW_CONTEXT *)pCapture->pDDrawContext,
                pCapture->pPixelData,
                pCapture->pixelDataSize,
                &dwBytesWritten
            );
            
            if (ddStatus == DDRAW_STATUS_SUCCESS) {
                sprintf(buf, "[CAPTURE] DirectDraw GPU capture SUCCESS - %lu bytes\r\n", dwBytesWritten);
                ScreenLog(buf);
                nDDrawConsecutiveFailures = 0;  /* Reset on success */
                return RD2K_SUCCESS;
            } else {
                nDDrawConsecutiveFailures++;
                sprintf(buf, "[CAPTURE] DirectDraw capture failed (0x%08lx), consecutive failures: %d\r\n", ddStatus, nDDrawConsecutiveFailures);
                ScreenLog(buf);
                pCapture->bDDrawFailed = TRUE;
                /* NOTE: Don't cascade Desktop_TryNextCaptureMode here.
                 * DDraw failure is a rendering issue, not a desktop access issue.
                 * The fallback layer system is for desktop switching problems. */
            }
        }
    }
    
    ScreenLog("[CAPTURE] Using GDI fallback capture path\r\n");
    
    /* ========================================================================
     * WINLOGON WORKER THREAD CAPTURE
     * When we have the Winlogon desktop handle but SetThreadDesktop failed
     * (error 183 - thread has windows), use a clean worker thread that CAN
     * switch to the Winlogon desktop and capture from there.
     * ======================================================================== */
    if (pCapture->pDesktopContext && 
        pCapture->pDesktopContext->bOnWinlogonDesktop &&
        pCapture->pDesktopContext->hWinlogonDesktop) {
        ScreenLog("[CAPTURE] Winlogon desktop active - using worker thread capture\r\n");
        
        if (ScreenCapture_CaptureWinlogonWorker(pCapture) == 0) {
            ScreenLog("[CAPTURE] Worker thread Winlogon capture SUCCESS\r\n");
            nWorkerConsecutiveFailures = 0;
            return RD2K_SUCCESS;
        }
        
        /* FIX (v6.2): Do NOT fall through to normal GDI when worker fails.
         * The main thread is on Default desktop, so GDI would capture the
         * WRONG desktop (Default instead of Winlogon). */
        nWorkerConsecutiveFailures++;
        
        /* FIX (v6.3): If worker fails 2+ times in a row, the Winlogon desktop
         * handle is stale (OS left Winlogon but debounce kept us retrying).
         * Clear debounce so the NEXT frame exits Winlogon mode immediately
         * instead of wasting 2+ seconds on doomed retries. */
        if (nWorkerConsecutiveFailures >= 2) {
            ScreenLog("[CAPTURE] Worker failed 2x - clearing debounce, will exit Winlogon next frame\r\n");
            dwLastWinlogonTime = 0;
            nWorkerConsecutiveFailures = 0;
        } else {
            ScreenLog("[CAPTURE] Worker thread capture failed - will retry next frame\r\n");
        }
        return RD2K_ERR_SCREEN;
    }
    
    /* Normal GDI capture path (fallback if DirectDraw fails) */
    hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        ScreenLog("[CAPTURE] FAILED GetDC(NULL) - attempting fallback\r\n");
        /* Trigger capture mode fallback */
        if (pCapture->pDesktopContext) {
            if (Desktop_TryNextCaptureMode(pCapture->pDesktopContext)) {
                ScreenLog("[CAPTURE] Fallback initiated for GetDC failure\r\n");
                /* Recursively try capture again with new mode */
                return ScreenCapture_CaptureScreen(pCapture);
            } else {
                ScreenLog("[CAPTURE] All fallback modes exhausted due to GetDC failure\r\n");
            }
        }
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Got screen DC\r\n");
    
    /* Create fresh memory DC from current screen DC */
    hdcMemory = CreateCompatibleDC(hdcScreen);
    if (!hdcMemory) {
        ScreenLog("[CAPTURE] FAILED CreateCompatibleDC - attempting fallback\r\n");
        ReleaseDC(NULL, hdcScreen);
        /* Trigger capture mode fallback */
        if (pCapture->pDesktopContext) {
            if (Desktop_TryNextCaptureMode(pCapture->pDesktopContext)) {
                ScreenLog("[CAPTURE] Fallback initiated for CreateCompatibleDC failure\r\n");
                /* Recursively try capture again with new mode */
                return ScreenCapture_CaptureScreen(pCapture);
            } else {
                ScreenLog("[CAPTURE] All fallback modes exhausted due to CreateCompatibleDC failure\r\n");
            }
        }
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Created memory DC\r\n");
    
    /* Create FRESH DIB section on each frame (not reusing) */
    ZeroMemory(&pCapture->bmpInfo, sizeof(BITMAPINFO));
    pCapture->bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pCapture->bmpInfo.bmiHeader.biWidth = pCapture->width;
    pCapture->bmpInfo.bmiHeader.biHeight = -pCapture->height;
    pCapture->bmpInfo.bmiHeader.biPlanes = 1;
    pCapture->bmpInfo.bmiHeader.biBitCount = (WORD)pCapture->bitsPerPixel;  /* Use actual screen depth! */
    pCapture->bmpInfo.bmiHeader.biCompression = BI_RGB;
    
    hBitmap = CreateDIBSection(
        hdcMemory, &pCapture->bmpInfo, DIB_RGB_COLORS,
        (void**)&pPixelData, NULL, 0);
    
    if (!hBitmap || !pPixelData) {
        ScreenLog("[CAPTURE] FAILED CreateDIBSection - attempting fallback\r\n");
        DeleteDC(hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        /* Trigger capture mode fallback */
        if (pCapture->pDesktopContext) {
            if (Desktop_TryNextCaptureMode(pCapture->pDesktopContext)) {
                ScreenLog("[CAPTURE] Fallback initiated for CreateDIBSection failure\r\n");
                /* Recursively try capture again with new mode */
                return ScreenCapture_CaptureScreen(pCapture);
            } else {
                ScreenLog("[CAPTURE] All fallback modes exhausted due to CreateDIBSection failure\r\n");
            }
        }
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Created fresh DIB section\r\n");
    
    /* Select the fresh bitmap into the memory DC */
    ScreenLog("[CAPTURE] About to call SelectObject...\r\n");
    hBitmapOld = (HBITMAP)SelectObject(hdcMemory, hBitmap);
    if (!hBitmapOld) {
        ScreenLog("[CAPTURE] FAILED SelectObject on fresh DIB\r\n");
        DeleteObject(hBitmap);
        DeleteDC(hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] SelectObject succeeded, about to log DC values\r\n");
    
    {
        char buf[256];
        sprintf(buf, "[CAPTURE] DC handles: hdc_screen=%p hdc_mem=%p hBitmap=%p hBitmapOld=%p\r\n",
                (void*)hdcScreen, (void*)hdcMemory, (void*)hBitmap, (void*)hBitmapOld);
        ScreenLog(buf);
    }
    
    ScreenLog("[CAPTURE] Selected fresh bitmap into DC\r\n");
    
    /* Perform the screen capture */
    ScreenLog("[CAPTURE] About to call BitBlt...\r\n");
    ScreenLog("[CAPTURE] BitBlt params: hdc_mem=0x12345678 0,0 -> 1457x888 hdc_screen=0x87654321 SRCCOPY\r\n");
    
    {
        int result;
        DWORD lastError;
        char buf[256];
        
        result = BitBlt(hdcMemory, 0, 0, 
                        pCapture->width, pCapture->height,
                        hdcScreen, 0, 0, SRCCOPY);
        
        lastError = GetLastError();
        
        if (!result) {
            sprintf(buf, "[CAPTURE] BitBlt FAILED! GetLastError=%lu (0x%lx)\r\n", lastError, lastError);
            ScreenLog(buf);
            
            /* ERROR 6 = INVALID_HANDLE: Desktop switched (UAC appeared)! 
             * When UAC prompt appears, Windows switches to Secure Desktop
             * All old DC handles become invalid. We must reinitialize next frame. 
             * Also trigger fallback to next capture mode if available */
            if (lastError == 6) {  /* ERROR_INVALID_HANDLE */
                ScreenLog("[CAPTURE] ERROR 6 = Desktop switched (INVALID_HANDLE)!\r\n");
                /* Only cascade fallback if NOT already handling Winlogon/UAC
                 * When bForceGDI is set, we already know we're on a secure desktop
                 * and this error is expected if the switch didn't work */
                if (!bForceGDI && pCapture->pDesktopContext) {
                    if (Desktop_TryNextCaptureMode(pCapture->pDesktopContext)) {
                        ScreenLog("[CAPTURE] Fallback to next capture mode initiated\r\n");
                    } else {
                        ScreenLog("[CAPTURE] All capture mode fallbacks exhausted\r\n");
                    }
                }
                /* Force reinitialization next frame by disabling reuse */
                pCapture->bDDrawFailed = TRUE;
            }
            
            SelectObject(hdcMemory, hBitmapOld);
            DeleteObject(hBitmap);
            DeleteDC(hdcMemory);
            ReleaseDC(NULL, hdcScreen);
            return RD2K_ERR_SCREEN;
        }
    }
    
    ScreenLog("[CAPTURE] BitBlt successful\r\n");
    
    /* CRITICAL FIX: Copy pixel data from DIB to persistent buffer BEFORE cleanup
     * The DIB memory (pPixelData) will be deleted along with hBitmap, so we MUST
     * copy the data to pCapture->pPixelData (persistent malloc'd buffer) that was
     * allocated in ScreenCapture_Create(). This buffer is used later in SendScreenUpdate. */
    {
        char buf[256];
        int bytesPerPixel = (pCapture->bitsPerPixel + 7) / 8;  /* Convert bits to bytes */
        int alignedStride;
        int expectedSize;
        
        /* Validate pixelDataSize against DWORD-aligned expected size */
        alignedStride = ((pCapture->width * bytesPerPixel + 3) & ~3);
        expectedSize = alignedStride * pCapture->height;
        
        if (pCapture->pixelDataSize != expectedSize) {
            sprintf(buf, "[CAPTURE] pixelDataSize adjusted: stored=%d expected=%d (w=%d h=%d bpp=%d stride=%d)\r\n",
                    pCapture->pixelDataSize, expectedSize, pCapture->width, pCapture->height, pCapture->bitsPerPixel, alignedStride);
            ScreenLog(buf);
            pCapture->pixelDataSize = expectedSize;
        }
        
        sprintf(buf, "[CAPTURE] About to copy: src=%p dst=%p size=%d\r\n",
                (void*)pPixelData, (void*)pCapture->pPixelData, pCapture->pixelDataSize);
        ScreenLog(buf);
    }
    
    if (!pPixelData) {
        ScreenLog("[CAPTURE] ERROR: pPixelData is NULL!\r\n");
    } else if (!pCapture->pPixelData) {
        ScreenLog("[CAPTURE] ERROR: pCapture->pPixelData is NULL!\r\n");
    } else if (pCapture->pixelDataSize == 0) {
        ScreenLog("[CAPTURE] ERROR: pixelDataSize is 0!\r\n");
    } else if (pCapture->pixelDataSize > (pCapture->width * pCapture->height * 8)) {
        /* Sanity check: size shouldn't be more than 8 bytes per pixel */
        char buf[256];
        int bytesPerPixel = (pCapture->bitsPerPixel + 7) / 8;
        int alignedStride = ((pCapture->width * bytesPerPixel + 3) & ~3);
        int expectedSize = alignedStride * pCapture->height;
        sprintf(buf, "[CAPTURE] ERROR: pixelDataSize %d is unreasonable! Clamping to expected %d\r\n",
                pCapture->pixelDataSize, expectedSize);
        ScreenLog(buf);
        pCapture->pixelDataSize = expectedSize;
        /* Row-by-row copy from DIB (DWORD-aligned source) to buffer */
        {
            int bytesPerRow = pCapture->width * bytesPerPixel;
            int row;
            for (row = 0; row < pCapture->height; row++) {
                memcpy(pCapture->pPixelData + row * alignedStride,
                       pPixelData + row * alignedStride,
                       bytesPerRow);
            }
        }
    } else {
        /* DIB section produces DWORD-aligned rows - copy the full aligned size */
        memcpy(pCapture->pPixelData, pPixelData, pCapture->pixelDataSize);
        ScreenLog("[CAPTURE] Copied pixel data to persistent buffer\r\n");
    }
    
    /* Restore and cleanup */
    SelectObject(hdcMemory, hBitmapOld);
    DeleteObject(hBitmap);
    DeleteDC(hdcMemory);
    ReleaseDC(NULL, hdcScreen);
    
    GdiFlush();
    ScreenLog("[CAPTURE] Screen capture COMPLETE\r\n");
    return RD2K_SUCCESS;
}

DWORD CompressRLE(const BYTE *pSrc, DWORD srcSize, BYTE *pDst, DWORD dstMaxSize)
{
    DWORD srcPos = 0, dstPos = 0;
    
    while (srcPos < srcSize && dstPos < dstMaxSize - 3) {
        BYTE currentByte = pSrc[srcPos];
        DWORD runLength = 1;
        
        while (srcPos + runLength < srcSize && runLength < 255 &&
               pSrc[srcPos + runLength] == currentByte) {
            runLength++;
        }
        
        if (runLength >= 3 || currentByte == 0xFF) {
            if (dstPos + 3 > dstMaxSize) break;
            pDst[dstPos++] = 0xFF;
            pDst[dstPos++] = (BYTE)runLength;
            pDst[dstPos++] = currentByte;
            srcPos += runLength;
        } else {
            while (runLength-- > 0 && dstPos < dstMaxSize) {
                pDst[dstPos++] = pSrc[srcPos++];
            }
        }
    }
    
    return dstPos;
}

DWORD DecompressRLE(const BYTE *pSrc, DWORD srcSize, BYTE *pDst, DWORD dstMaxSize)
{
    DWORD srcPos = 0, dstPos = 0;
    
    while (srcPos < srcSize && dstPos < dstMaxSize) {
        if (pSrc[srcPos] == 0xFF && srcPos + 2 < srcSize) {
            BYTE count = pSrc[srcPos + 1];
            BYTE value = pSrc[srcPos + 2];
            DWORD i;
            srcPos += 3;
            for (i = 0; i < count && dstPos < dstMaxSize; i++) {
                pDst[dstPos++] = value;
            }
        } else {
            pDst[dstPos++] = pSrc[srcPos++];
        }
    }
    
    return dstPos;
}

int FindDirtyRects(const BYTE *pOldFrame, const BYTE *pNewFrame,
                   int width, int height, int bytesPerPixel,
                   RECT *pRects, int maxRects)
{
    int numRects = 0;
    int blockSize = 32;
    int stride = ((width * bytesPerPixel + 3) & ~3);
    int bx, by;
    char buf[256];
    
    sprintf(buf, "[FINDDIRTY] ENTER: old=%p new=%p w=%d h=%d bpp=%d max=%d\r\n", 
            (void*)pOldFrame, (void*)pNewFrame, width, height, bytesPerPixel, maxRects);
    ScreenLog(buf);
    
    if (!pOldFrame || !pNewFrame || !pRects || maxRects <= 0) {
        ScreenLog("[FINDDIRTY] EARLY EXIT: null pointer or invalid maxRects\r\n");
        return 0;
    }
    
    sprintf(buf, "[FINDDIRTY] Starting scan loop: stride=%d blockSize=%d\r\n", stride, blockSize);
    ScreenLog(buf);
    
    for (by = 0; by < height && numRects < maxRects; by += blockSize) {
        for (bx = 0; bx < width && numRects < maxRects; bx += blockSize) {
            int blockW = (bx + blockSize < width) ? blockSize : (width - bx);
            int blockH = (by + blockSize < height) ? blockSize : (height - by);
            int dirty = 0, y;
            int offset;
            
            for (y = 0; y < blockH && !dirty; y++) {
                offset = (by + y) * stride + bx * bytesPerPixel;
                if (memcmp(pOldFrame + offset, pNewFrame + offset, blockW * bytesPerPixel) != 0) {
                    dirty = 1;
                }
            }
            
            if (dirty) {
                sprintf(buf, "[FINDDIRTY] Found dirty block at (%d,%d) size %dx%d - rect %d\r\n", 
                        bx, by, blockW, blockH, numRects);
                ScreenLog(buf);
                
                pRects[numRects].left = bx;
                pRects[numRects].top = by;
                pRects[numRects].right = bx + blockW;
                pRects[numRects].bottom = by + blockH;
                numRects++;
            }
        }
    }
    
    sprintf(buf, "[FINDDIRTY] COMPLETE: found %d rectangles\r\n", numRects);
    ScreenLog(buf);
    
    return numRects;
}
