/*
 * DirectDraw Fast Screen Capture Implementation
 * Windows 2000 SP1+ Compatible - C89 Safe - Production Grade
 * 
 * Recursive resource management, comprehensive error handling,
 * automatic fallback to GDI if DirectDraw unavailable
 */

#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "screen_capture_ddraw.h"

/* ========================================================================
 * DIAGNOSTIC LOGGING & DEBUG UTILITIES
 * ======================================================================== */

#define DDRAW_LOG_BUFFER_SIZE 512

static void WINAPI DDraw_Log(const char *pszFormat, ...)
{
    #if DDRAW_ENABLE_DETAILED_LOGGING
    va_list args;
    char szBuffer[DDRAW_LOG_BUFFER_SIZE];
    FILE *pLogFile = NULL;
    
    va_start(args, pszFormat);
    _vsnprintf(szBuffer, DDRAW_LOG_BUFFER_SIZE - 1, pszFormat, args);
    va_end(args);
    
    szBuffer[DDRAW_LOG_BUFFER_SIZE - 1] = '\0';
    
    /* Log to file */
    pLogFile = fopen("remotedebug\\ddraw.log", "a");
    if (pLogFile) {
        fprintf(pLogFile, "[DDRAW] %s\n", szBuffer);
        fclose(pLogFile);
    }
    #else
    (void)pszFormat;  /* Suppress unused parameter warning */
    #endif
}

/* ========================================================================
 * WINDOWS 2000 SP1 QUIRKS & COMPATIBILITY
 * ======================================================================== */

/*
 * Windows 2000 SP1 has specific DirectDraw driver requirements
 * Some AGP chipsets need special handling for surface locks
 */
#define W2K_SP1_DRIVER_DELAY_MS     1
#define W2K_SP1_MAX_RETRY_COUNT     5
#define W2K_SP1_SURFACE_LOST_CODE   0x80004007L  /* DDERR_SURFACELOST */

/* ========================================================================
 * DYNAMIC FUNCTION POINTER TYPES (for runtime loading)
 * ======================================================================== */

typedef HRESULT (WINAPI *LPFNDIRECTDRAWCREATE)(
    GUID FAR *lpGUID,
    LPDIRECTDRAW FAR *lplpDD,
    IUnknown FAR *pUnkOuter
);

/* ========================================================================
 * FORWARD DECLARATIONS (Recursive Cleanup Pattern)
 * ======================================================================== */

static DDRAW_STATUS WINAPI DDraw_InternalCleanupSurface(DDRAW_CONTEXT *pContext);
static DDRAW_STATUS WINAPI DDraw_InternalCleanupDirectDraw(DDRAW_CONTEXT *pContext);
static DDRAW_STATUS WINAPI DDraw_InternalCleanupLibrary(DDRAW_CONTEXT *pContext);
static DDRAW_STATUS WINAPI DDraw_InternalCreateSurface(DDRAW_CONTEXT *pContext);
static DDRAW_STATUS WINAPI DDraw_InternalRetryLock(DDRAW_CONTEXT *pContext,
                                                     LPDDSURFACEDESC pSurfaceDesc,
                                                     DWORD dwAttempt);

/* ========================================================================
 * ERROR CODE CONVERSION UTILITIES
 * ======================================================================== */

static const char *WINAPI DDraw_HResultToString(HRESULT hr)
{
    /* Windows 2000 SP1 Generic Error Handler */
    /* Minimal error codes to avoid header conflicts */
    if (hr == 0 || hr == DD_OK) {
        return "Success";
    }
    if (hr == E_OUTOFMEMORY) {
        return "Out of memory";
    }
    if (hr == E_POINTER) {
        return "Invalid pointer";
    }
    if (hr == DDERR_GENERIC) {
        return "DirectDraw generic error";
    }
    if (hr == DDERR_INVALIDOBJECT) {
        return "Invalid DirectDraw object";
    }
    if (hr == DDERR_INVALIDRECT) {
        return "Invalid rectangle";
    }
    if (hr == DDERR_OUTOFVIDEOMEMORY) {
        return "Out of video memory";
    }
    if (hr == DDERR_SURFACEBUSY) {
        return "Surface busy - try again";
    }
    if (hr == DDERR_SURFACELOST) {
        return "Surface lost (Windows 2000 SP1)";
    }
    if (hr == DDERR_WASSTILLDRAWING) {
        return "GPU still drawing";
    }
    if (hr == DDERR_NOTINITIALIZED) {
        return "DirectDraw not initialized";
    }
    if (hr == DDERR_NOTLOCKED) {
        return "Surface not locked";
    }
    if (hr == DDERR_NOFOCUSWINDOW) {
        return "No focus window";
    }
    if (hr == DDERR_NOCOOPERATIVELEVELSET) {
        return "No cooperative level set";
    }
    
    /* All other errors - generic hex code */
    {
        static char szBuffer[48];
        sprintf(szBuffer, "Error 0x%08lx", hr);
        return szBuffer;
    }
}

const char *WINAPI DDraw_StatusToString(DDRAW_STATUS status)
{
    switch (status) {
        case DDRAW_STATUS_SUCCESS:
            return "Success";
        case DDRAW_STATUS_NOT_INITIALIZED:
            return "Not initialized";
        case DDRAW_STATUS_LIBRARY_NOT_FOUND:
            return "DirectDraw library not found";
        case DDRAW_STATUS_PROC_NOT_FOUND:
            return "DirectDraw function not found";
        case DDRAW_STATUS_OBJECT_CREATE_FAILED:
            return "DirectDraw object creation failed";
        case DDRAW_STATUS_SURFACE_CREATE_FAILED:
            return "Surface creation failed";
        case DDRAW_STATUS_SURFACE_LOCK_FAILED:
            return "Surface lock failed";
        case DDRAW_STATUS_INVALID_PARAMETERS:
            return "Invalid parameters";
        case DDRAW_STATUS_MEMORY_ERROR:
            return "Memory error";
        case DDRAW_STATUS_DISPLAY_MODE_FAILED:
            return "Display mode query failed";
        case DDRAW_STATUS_UNSUPPORTED_FORMAT:
            return "Unsupported pixel format";
        case DDRAW_STATUS_RESOURCE_BUSY:
            return "Resource busy (retry available)";
        case DDRAW_STATUS_WINDOW_NOT_FOUND:
            return "Window not found";
        case DDRAW_STATUS_FALLBACK_ACTIVE:
            return "Fallback to GDI active";
        case DDRAW_STATUS_RECOVERED:
            return "Recovered from error";
        default:
            return "Unknown status";
    }
}

/* ========================================================================
 * CONTEXT MANAGEMENT (RAII PATTERN IN C)
 * ======================================================================== */

DDRAW_CONTEXT *WINAPI DDraw_CreateContext(void)
{
    DDRAW_CONTEXT *pContext = NULL;
    
    pContext = (DDRAW_CONTEXT *)malloc(sizeof(DDRAW_CONTEXT));
    if (!pContext) {
        DDraw_Log("ERROR: Cannot allocate context (out of memory)");
        return NULL;
    }
    
    /* Zero-initialize entire structure */
    ZeroMemory(pContext, sizeof(DDRAW_CONTEXT));
    
    /* Create mutex for thread safety */
    pContext->hMutex = CreateMutexA(NULL, FALSE, NULL);
    if (!pContext->hMutex) {
        DDraw_Log("ERROR: Cannot create mutex");
        free(pContext);
        return NULL;
    }
    
    DDraw_Log("Context created successfully");
    return pContext;
}

void WINAPI DDraw_DestroyContext(DDRAW_CONTEXT **ppContext)
{
    DDRAW_CONTEXT *pContext = NULL;
    
    if (!ppContext || !*ppContext) {
        return;
    }
    
    pContext = *ppContext;
    
    /* Recursive cleanup pattern - cleanup in reverse order */
    DDraw_Cleanup(pContext);
    
    /* Close mutex */
    if (pContext->hMutex) {
        CloseHandle(pContext->hMutex);
        pContext->hMutex = NULL;
    }
    
    /* Free context memory */
    free(pContext);
    *ppContext = NULL;
    
    DDraw_Log("Context destroyed");
}

/* ========================================================================
 * RECURSIVE RESOURCE CLEANUP FUNCTIONS
 * ======================================================================== */

static DDRAW_STATUS WINAPI DDraw_InternalCleanupSurface(DDRAW_CONTEXT *pContext)
{
    HRESULT hr = S_OK;
    
    if (!pContext) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    if (pContext->pPrimarySurface) {
        DDraw_Log("Releasing primary surface...");
        
        /* Ensure surface is unlocked before releasing */
        if (pContext->bSurfaceLocked) {
            hr = IDirectDrawSurface_Unlock(pContext->pPrimarySurface, NULL);
            if (hr != DD_OK) {
                DDraw_Log("WARNING: Surface unlock failed with 0x%08lx", hr);
            }
            pContext->bSurfaceLocked = FALSE;
        }
        
        /* Release surface */
        hr = IDirectDrawSurface_Release(pContext->pPrimarySurface);
        pContext->pPrimarySurface = NULL;
        
        if (hr != DD_OK) {
            DDraw_Log("ERROR: Surface release failed with 0x%08lx (%s)",
                      hr, DDraw_HResultToString(hr));
            pContext->dwLastErrorCode = hr;
            pContext->dwErrorCount++;
            return DDRAW_STATUS_SURFACE_CREATE_FAILED;
        }
        
        DDraw_Log("Primary surface released successfully");
    }
    
    return DDRAW_STATUS_SUCCESS;
}

static DDRAW_STATUS WINAPI DDraw_InternalCleanupDirectDraw(DDRAW_CONTEXT *pContext)
{
    HRESULT hr = S_OK;
    
    if (!pContext) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    if (pContext->pDirectDraw) {
        DDraw_Log("Releasing DirectDraw object...");
        
        /* Release DirectDraw interface */
        hr = IDirectDraw_Release(pContext->pDirectDraw);
        pContext->pDirectDraw = NULL;
        
        if (hr != DD_OK) {
            DDraw_Log("ERROR: DirectDraw release failed with 0x%08lx (%s)",
                      hr, DDraw_HResultToString(hr));
            pContext->dwLastErrorCode = hr;
            pContext->dwErrorCount++;
            return DDRAW_STATUS_OBJECT_CREATE_FAILED;
        }
        
        DDraw_Log("DirectDraw object released successfully");
    }
    
    return DDRAW_STATUS_SUCCESS;
}

static DDRAW_STATUS WINAPI DDraw_InternalCleanupLibrary(DDRAW_CONTEXT *pContext)
{
    if (!pContext) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    if (pContext->hDDrawLibrary) {
        DDraw_Log("Unloading DirectDraw library...");
        
        if (!FreeLibrary(pContext->hDDrawLibrary)) {
            DDraw_Log("ERROR: Failed to unload ddraw.dll");
            return DDRAW_STATUS_LIBRARY_NOT_FOUND;
        }
        
        pContext->hDDrawLibrary = NULL;
        pContext->bLibraryLoaded = FALSE;
        
        DDraw_Log("DirectDraw library unloaded successfully");
    }
    
    return DDRAW_STATUS_SUCCESS;
}

/* ========================================================================
 * INITIALIZATION & CLEANUP
 * ======================================================================== */

DDRAW_STATUS WINAPI DDraw_Initialize(DDRAW_CONTEXT *pContext)
{
    HMODULE hDDraw = NULL;
    LPFNDIRECTDRAWCREATE fnDirectDrawCreate = NULL;
    HRESULT hr = S_OK;
    DDRAW_STATUS ddStatus = DDRAW_STATUS_SUCCESS;
    DWORD dwAttempt = 0;
    
    if (!pContext) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    /* Already initialized? */
    if (pContext->bInitialized) {
        DDraw_Log("DirectDraw already initialized");
        return DDRAW_STATUS_SUCCESS;
    }
    
    /* Acquire mutex for thread safety */
    if (WaitForSingleObject(pContext->hMutex, INFINITE) != WAIT_OBJECT_0) {
        DDraw_Log("ERROR: Cannot acquire mutex");
        return DDRAW_STATUS_MEMORY_ERROR;
    }
    
    /* ========================================================================
     * DirectDraw GPU Acceleration Initialization
     * ======================================================================== */
    
    /* Retry logic for Windows 2000 SP1 quirks */
    for (dwAttempt = 0; dwAttempt < DDRAW_MAX_INIT_ATTEMPTS; dwAttempt++) {
        
        if (dwAttempt > 0) {
            DDraw_Log("Retry attempt %lu after delay", dwAttempt);
            Sleep(W2K_SP1_DRIVER_DELAY_MS);
        }
        
        pContext->dwInitAttempts = dwAttempt + 1;
        
        /* Load DirectDraw library */
        hDDraw = LoadLibraryA("ddraw.dll");
        if (!hDDraw) {
            DDraw_Log("WARNING: Cannot load ddraw.dll (attempt %lu) - using GDI fallback", 
                      dwAttempt + 1);
            
            if (dwAttempt == DDRAW_MAX_INIT_ATTEMPTS - 1) {
                /* Final attempt failed - enable fallback */
                pContext->bUsingFallback = TRUE;
                pContext->bInitialized = FALSE;
                ReleaseMutex(pContext->hMutex);
                return DDRAW_STATUS_FALLBACK_ACTIVE;
            }
            continue;
        }
        
        /* Get DirectDrawCreate function pointer */
        fnDirectDrawCreate = (LPFNDIRECTDRAWCREATE)GetProcAddress(hDDraw, "DirectDrawCreate");
        if (!fnDirectDrawCreate) {
            DDraw_Log("ERROR: Cannot find DirectDrawCreate in ddraw.dll");
            FreeLibrary(hDDraw);
            hDDraw = NULL;
            
            if (dwAttempt == DDRAW_MAX_INIT_ATTEMPTS - 1) {
                pContext->bUsingFallback = TRUE;
                ReleaseMutex(pContext->hMutex);
                return DDRAW_STATUS_PROC_NOT_FOUND;
            }
            continue;
        }
        
        /* Create DirectDraw object */
        hr = fnDirectDrawCreate(NULL, &pContext->pDirectDraw, NULL);
        if (hr != DD_OK) {
            DDraw_Log("ERROR: DirectDrawCreate failed with 0x%08lx (%s) on attempt %lu",
                      hr, DDraw_HResultToString(hr), dwAttempt + 1);
            FreeLibrary(hDDraw);
            hDDraw = NULL;
            pContext->dwLastErrorCode = hr;
            pContext->dwErrorCount++;
            
            if (dwAttempt == DDRAW_MAX_INIT_ATTEMPTS - 1) {
                pContext->bUsingFallback = TRUE;
                ReleaseMutex(pContext->hMutex);
                return DDRAW_STATUS_OBJECT_CREATE_FAILED;
            }
            continue;
        }
        
        /* Set cooperative level (non-exclusive for SERVICE context) */
        hr = IDirectDraw_SetCooperativeLevel(pContext->pDirectDraw, 
                                             GetDesktopWindow(), 
                                             DDSCL_NORMAL);
        if (hr != DD_OK) {
            DDraw_Log("ERROR: SetCooperativeLevel failed with 0x%08lx (%s)",
                      hr, DDraw_HResultToString(hr));
            IDirectDraw_Release(pContext->pDirectDraw);
            pContext->pDirectDraw = NULL;
            FreeLibrary(hDDraw);
            hDDraw = NULL;
            pContext->dwLastErrorCode = hr;
            pContext->dwErrorCount++;
            
            if (dwAttempt == DDRAW_MAX_INIT_ATTEMPTS - 1) {
                pContext->bUsingFallback = TRUE;
                ReleaseMutex(pContext->hMutex);
                return DDRAW_STATUS_OBJECT_CREATE_FAILED;
            }
            continue;
        }
        
        /* Success! */
        pContext->hDDrawLibrary = hDDraw;
        pContext->bLibraryLoaded = TRUE;
        pContext->bInitialized = TRUE;
        pContext->bUsingFallback = FALSE;
        pContext->dwErrorCount = 0;
        pContext->dwSuccessCount = 0;
        
        DDraw_Log("DirectDraw initialized successfully on attempt %lu", dwAttempt + 1);
        
        /* Get display mode */
        ddStatus = DDraw_GetDisplayMode(pContext);
        if (ddStatus != DDRAW_STATUS_SUCCESS) {
            DDraw_Log("WARNING: Could not get display mode: %s", 
                      DDraw_StatusToString(ddStatus));
            /* Continue anyway - we'll try on first capture */
        }
        
        ReleaseMutex(pContext->hMutex);
        return DDRAW_STATUS_SUCCESS;
    }
    
    ReleaseMutex(pContext->hMutex);
    return DDRAW_STATUS_OBJECT_CREATE_FAILED;
}

DDRAW_STATUS WINAPI DDraw_Cleanup(DDRAW_CONTEXT *pContext)
{
    DDRAW_STATUS status1 = DDRAW_STATUS_SUCCESS;
    DDRAW_STATUS status2 = DDRAW_STATUS_SUCCESS;
    DDRAW_STATUS status3 = DDRAW_STATUS_SUCCESS;
    
    if (!pContext) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    /* Acquire mutex */
    if (pContext->hMutex) {
        WaitForSingleObject(pContext->hMutex, INFINITE);
    }
    
    /* Recursive cleanup pattern - cleanup in reverse order of initialization */
    
    /* Cleanup DirectDraw resources */
    status1 = DDraw_InternalCleanupSurface(pContext);
    status2 = DDraw_InternalCleanupDirectDraw(pContext);
    status3 = DDraw_InternalCleanupLibrary(pContext);
    
    pContext->bInitialized = FALSE;
    
    /* Release mutex last */
    if (pContext->hMutex) {
        ReleaseMutex(pContext->hMutex);
    }
    
    DDraw_Log("Cleanup complete - Surface:%s, DirectDraw:%s, Library:%s",
              DDraw_StatusToString(status1),
              DDraw_StatusToString(status2),
              DDraw_StatusToString(status3));
    
    /* Return most critical error, or success if all OK */
    if (status1 != DDRAW_STATUS_SUCCESS) return status1;
    if (status2 != DDRAW_STATUS_SUCCESS) return status2;
    if (status3 != DDRAW_STATUS_SUCCESS) return status3;
    return DDRAW_STATUS_SUCCESS;
}

/* ========================================================================
 * DISPLAY MODE & SURFACE CREATION
 * ======================================================================== */

DDRAW_STATUS WINAPI DDraw_GetDisplayMode(DDRAW_CONTEXT *pContext)
{
    DDSURFACEDESC ddsd;
    HRESULT hr = S_OK;
    DWORD dwBytesPerPixel = 0;
    
    if (!pContext || !pContext->bInitialized) {
        return DDRAW_STATUS_NOT_INITIALIZED;
    }
    
    ZeroMemory(&ddsd, sizeof(DDSURFACEDESC));
    ddsd.dwSize = sizeof(DDSURFACEDESC);
    
    hr = IDirectDraw_GetDisplayMode(pContext->pDirectDraw, &ddsd);
    if (hr != DD_OK) {
        DDraw_Log("ERROR: GetDisplayMode failed with 0x%08lx (%s)",
                  hr, DDraw_HResultToString(hr));
        pContext->dwLastErrorCode = hr;
        pContext->dwErrorCount++;
        return DDRAW_STATUS_DISPLAY_MODE_FAILED;
    }
    
    /* Store screen info */
    pContext->screenInfo.dwWidth = ddsd.dwWidth;
    pContext->screenInfo.dwHeight = ddsd.dwHeight;
    pContext->screenInfo.dwPitch = ddsd.lPitch;
    pContext->screenInfo.dwBitCount = ddsd.ddpfPixelFormat.dwRGBBitCount;
    pContext->screenInfo.bIsRGB = (ddsd.ddpfPixelFormat.dwFlags & DDPF_RGB) ? TRUE : FALSE;
    pContext->screenInfo.bHasAlpha = (ddsd.ddpfPixelFormat.dwRGBBitCount == 32) ? TRUE : FALSE;
    
    pContext->screenInfo.dwRMask = ddsd.ddpfPixelFormat.dwRBitMask;
    pContext->screenInfo.dwGMask = ddsd.ddpfPixelFormat.dwGBitMask;
    pContext->screenInfo.dwBMask = ddsd.ddpfPixelFormat.dwBBitMask;
    
    if (pContext->screenInfo.bHasAlpha) {
        pContext->screenInfo.dwAMask = ddsd.ddpfPixelFormat.dwRGBAlphaBitMask;
    } else {
        pContext->screenInfo.dwAMask = 0;
    }
    
    pContext->bScreenInfoValid = TRUE;
    
    DDraw_Log("Display Mode: %lux%lu, %lu-bit, Pitch=%lu bytes",
              pContext->screenInfo.dwWidth,
              pContext->screenInfo.dwHeight,
              pContext->screenInfo.dwBitCount,
              pContext->screenInfo.dwPitch);
    
    /* Log detailed pixel format information */
    DDraw_Log("Pixel Format Masks: R=0x%08lx G=0x%08lx B=0x%08lx A=0x%08lx",
              pContext->screenInfo.dwRMask,
              pContext->screenInfo.dwGMask,
              pContext->screenInfo.dwBMask,
              pContext->screenInfo.dwAMask);
    
    /* Identify the format based on masks */
    {
        const char *formatName = "UNKNOWN";
        
        /* Standard 32-bit formats */
        if (pContext->screenInfo.dwBitCount == 32) {
            /* Check for ARGB (Alpha in top byte) */
            if (pContext->screenInfo.dwRMask == 0x00FF0000 && 
                pContext->screenInfo.dwGMask == 0x0000FF00 && 
                pContext->screenInfo.dwBMask == 0x000000FF) {
                formatName = "XRGB (or ARGB with alpha in top byte)";
            }
            /* Check for BGRA/BGR */
            else if (pContext->screenInfo.dwRMask == 0x000000FF && 
                     pContext->screenInfo.dwGMask == 0x0000FF00 && 
                     pContext->screenInfo.dwBMask == 0x00FF0000) {
                formatName = "BGR (or BGRA with alpha in top byte)";
            }
        }
        
        DDraw_Log("Detected format: %s", formatName);
    }
    
    return DDRAW_STATUS_SUCCESS;
}

static DDRAW_STATUS WINAPI DDraw_InternalCreateSurface(DDRAW_CONTEXT *pContext)
{
    DDSURFACEDESC ddsd;
    DDSCAPS ddsCaps;
    HRESULT hr = S_OK;
    
    if (!pContext || !pContext->bInitialized) {
        return DDRAW_STATUS_NOT_INITIALIZED;
    }
    
    /* Already created? */
    if (pContext->pPrimarySurface) {
        return DDRAW_STATUS_SUCCESS;
    }
    
    /* Zero-initialize structures */
    ZeroMemory(&ddsd, sizeof(DDSURFACEDESC));
    ZeroMemory(&ddsCaps, sizeof(DDSCAPS));
    
    /* Set up surface descriptor for primary surface */
    ddsd.dwSize = sizeof(DDSURFACEDESC);
    ddsd.dwFlags = DDSD_CAPS;  /* Only caps field is valid */
    ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    ddsd.ddsCaps = ddsCaps;
    
    /* Create primary surface */
    hr = IDirectDraw_CreateSurface(pContext->pDirectDraw, &ddsd, 
                                   &pContext->pPrimarySurface, NULL);
    if (hr != DD_OK) {
        DDraw_Log("ERROR: CreateSurface failed with 0x%08lx (%s)",
                  hr, DDraw_HResultToString(hr));
        pContext->pPrimarySurface = NULL;
        pContext->dwLastErrorCode = hr;
        pContext->dwErrorCount++;
        return DDRAW_STATUS_SURFACE_CREATE_FAILED;
    }
    
    DDraw_Log("Primary surface created successfully");
    return DDRAW_STATUS_SUCCESS;
}

/* ========================================================================
 * SURFACE LOCKING WITH WINDOWS 2000 SP1 RETRY LOGIC
 * ======================================================================== */

static DDRAW_STATUS WINAPI DDraw_InternalRetryLock(DDRAW_CONTEXT *pContext,
                                                     LPDDSURFACEDESC pSurfaceDesc,
                                                     DWORD dwAttempt)
{
    HRESULT hr = S_OK;
    RECT rcLock;
    DWORD dwLockFlags = DDLOCK_WAIT;
    
    if (!pContext || !pSurfaceDesc) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    /* Windows 2000 SP1: Some chipsets lose surface during lock attempts */
    /* Add small delay before retry */
    if (dwAttempt > 0) {
        Sleep(W2K_SP1_DRIVER_DELAY_MS);
    }
    
    ZeroMemory(pSurfaceDesc, sizeof(DDSURFACEDESC));
    pSurfaceDesc->dwSize = sizeof(DDSURFACEDESC);
    
    /* Lock entire surface */
    ZeroMemory(&rcLock, sizeof(RECT));
    hr = IDirectDrawSurface_Lock(pContext->pPrimarySurface, NULL, pSurfaceDesc,
                                 dwLockFlags, NULL);
    
    if (hr == DD_OK) {
        pContext->bSurfaceLocked = TRUE;
        pContext->dwSuccessCount++;
        DDraw_Log("Surface locked successfully on attempt %lu", dwAttempt + 1);
        return DDRAW_STATUS_SUCCESS;
    }
    
    pContext->dwLastErrorCode = hr;
    
    /* Handle specific Windows 2000 SP1 errors */
    if (hr == W2K_SP1_SURFACE_LOST_CODE) {
        DDraw_Log("Surface lost (W2K SP1 quirk) - attempt %lu will retry", dwAttempt + 1);
        return DDRAW_STATUS_RESOURCE_BUSY;
    }
    
    if (hr == DDERR_SURFACEBUSY || hr == DDERR_WASSTILLDRAWING) {
        DDraw_Log("Surface busy/was still drawing - attempt %lu will retry", dwAttempt + 1);
        return DDRAW_STATUS_RESOURCE_BUSY;
    }
    
    DDraw_Log("ERROR: Lock failed with 0x%08lx (%s) on attempt %lu",
              hr, DDraw_HResultToString(hr), dwAttempt + 1);
    pContext->dwErrorCount++;
    
    return DDRAW_STATUS_SURFACE_LOCK_FAILED;
}

DDRAW_STATUS WINAPI DDraw_LockSurface(DDRAW_CONTEXT *pContext,
                                       LPDDSURFACEDESC pSurfaceDesc)
{
    DDRAW_STATUS status = DDRAW_STATUS_SUCCESS;
    DWORD dwAttempt = 0;
    
    if (!pContext || !pSurfaceDesc) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    if (!pContext->bInitialized) {
        return DDRAW_STATUS_NOT_INITIALIZED;
    }
    
    /* Acquire mutex */
    if (WaitForSingleObject(pContext->hMutex, INFINITE) != WAIT_OBJECT_0) {
        return DDRAW_STATUS_MEMORY_ERROR;
    }
    
    /* Create surface if needed */
    if (!pContext->pPrimarySurface) {
        status = DDraw_InternalCreateSurface(pContext);
        if (status != DDRAW_STATUS_SUCCESS) {
            ReleaseMutex(pContext->hMutex);
            return status;
        }
    }
    
    /* Retry lock with Windows 2000 SP1 resilience */
    for (dwAttempt = 0; dwAttempt < W2K_SP1_MAX_RETRY_COUNT; dwAttempt++) {
        status = DDraw_InternalRetryLock(pContext, pSurfaceDesc, dwAttempt);
        
        if (status == DDRAW_STATUS_SUCCESS) {
            ReleaseMutex(pContext->hMutex);
            return DDRAW_STATUS_SUCCESS;
        }
        
        if (status != DDRAW_STATUS_RESOURCE_BUSY) {
            /* Permanent error, not transient */
            ReleaseMutex(pContext->hMutex);
            return status;
        }
    }
    
    ReleaseMutex(pContext->hMutex);
    return DDRAW_STATUS_SURFACE_LOCK_FAILED;
}

DDRAW_STATUS WINAPI DDraw_UnlockSurface(DDRAW_CONTEXT *pContext)
{
    HRESULT hr = S_OK;
    
    if (!pContext) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    if (!pContext->bSurfaceLocked || !pContext->pPrimarySurface) {
        return DDRAW_STATUS_SUCCESS;  /* Not locked, nothing to do */
    }
    
    /* Acquire mutex */
    if (WaitForSingleObject(pContext->hMutex, INFINITE) != WAIT_OBJECT_0) {
        return DDRAW_STATUS_MEMORY_ERROR;
    }
    
    hr = IDirectDrawSurface_Unlock(pContext->pPrimarySurface, NULL);
    if (hr != DD_OK) {
        DDraw_Log("WARNING: Unlock failed with 0x%08lx (%s)",
                  hr, DDraw_HResultToString(hr));
        pContext->dwLastErrorCode = hr;
        pContext->dwErrorCount++;
        ReleaseMutex(pContext->hMutex);
        return DDRAW_STATUS_SURFACE_LOCK_FAILED;
    }
    
    pContext->bSurfaceLocked = FALSE;
    DDraw_Log("Surface unlocked");
    
    ReleaseMutex(pContext->hMutex);
    return DDRAW_STATUS_SUCCESS;
}

/* ========================================================================
 * FRAME BUFFER COPYING
 * ======================================================================== */

DDRAW_STATUS WINAPI DDraw_CopyFrameBuffer(DDRAW_CONTEXT *pContext,
                                          LPVOID pTargetBuffer,
                                          DWORD dwTargetSize,
                                          LPDWORD pdwBytesWritten)
{
    DDSURFACEDESC ddsd;
    DDRAW_STATUS status = DDRAW_STATUS_SUCCESS;
    DWORD dwExpectedSize = 0;
    DWORD dwRow = 0;
    LPBYTE pbSource = NULL;
    LPBYTE pbDest = NULL;
    DWORD dwCopySize = 0;
    
    if (!pContext || !pTargetBuffer || !pdwBytesWritten) {
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    if (pContext->bUsingFallback) {
        return DDRAW_STATUS_FALLBACK_ACTIVE;
    }
    
    if (!pContext->bInitialized) {
        return DDRAW_STATUS_NOT_INITIALIZED;
    }
    
    /* ========================================================================
     * DirectDraw GPU Acceleration Capture
     * ======================================================================== */
    
    DDraw_Log("Using DirectDraw GPU acceleration");
    
    /* Validate context and recover from errors if needed */
    status = DDraw_ValidateAndRecover(pContext);
    if (status != DDRAW_STATUS_SUCCESS && status != DDRAW_STATUS_RECOVERED) {
        return status;
    }
    
    /* Lock surface */
    status = DDraw_LockSurface(pContext, &ddsd);
    if (status != DDRAW_STATUS_SUCCESS) {
        *pdwBytesWritten = 0;
        return status;
    }
    
    /* Calculate expected FRAME OUTPUT size (not input!)
     * CRITICAL: DirectDraw gives 32-bit BGRA, but we output 24-bit BGR
     * So we MUST check against output size, not input pitch size */
    if (pContext->screenInfo.dwBitCount == 32) {
        /* Will convert 32-bit to 24-bit: output = width * 3 * height */
        dwExpectedSize = pContext->screenInfo.dwWidth * 3 * pContext->screenInfo.dwHeight;
    } else {
        /* Fixed bit depth: output = width * (bitcount/8) * height */
        dwExpectedSize = pContext->screenInfo.dwWidth * (pContext->screenInfo.dwBitCount / 8) * pContext->screenInfo.dwHeight;
    }
    
    if (dwTargetSize < dwExpectedSize) {
        DDraw_Log("ERROR: Target buffer too small (need %lu, have %lu)",
                  dwExpectedSize, dwTargetSize);
        DDraw_UnlockSurface(pContext);
        *pdwBytesWritten = 0;
        return DDRAW_STATUS_INVALID_PARAMETERS;
    }
    
    /* Copy frame data - handle pitch (stride) if needed */
    pbSource = (LPBYTE)ddsd.lpSurface;
    pbDest = (LPBYTE)pTargetBuffer;
    
    if (!pbSource) {
        DDraw_Log("ERROR: Surface lpSurface is NULL after lock");
        DDraw_UnlockSurface(pContext);
        *pdwBytesWritten = 0;
        return DDRAW_STATUS_SURFACE_LOCK_FAILED;
    }
    
    /* 
     * CRITICAL: DirectDraw gives us 32-bit BGRA, but the protocol expects 24-bit BGR!
     * Convert BGRA (4 bytes) to BGR (3 bytes) by stripping alpha channel
     * This reduces data size from 8,294,400 to 6,220,800 bytes
     */
    if (pContext->screenInfo.dwBitCount == 32) {
        DWORD dwPixelCount = pContext->screenInfo.dwWidth;
        DWORD dwRow;
        LPBYTE pbSrcRow, pbDstRow;
        DWORD x;
        
        DDraw_Log("Converting BGRA (32-bit) to BGR (24-bit)...");
        
        for (dwRow = 0; dwRow < pContext->screenInfo.dwHeight; dwRow++) {
            pbSrcRow = pbSource;
            pbDstRow = pbDest;
            
            /* Convert each pixel: skip alpha channel (every 4th byte) */
            for (x = 0; x < dwPixelCount; x++) {
                /* Source: [B][G][R][A] - 4 bytes */
                /* Dest: [B][G][R] - 3 bytes */
                pbDstRow[0] = pbSrcRow[0];  /* B */
                pbDstRow[1] = pbSrcRow[1];  /* G */
                pbDstRow[2] = pbSrcRow[2];  /* R */
                
                pbSrcRow += 4;  /* Move to next 32-bit pixel */
                pbDstRow += 3;  /* Move to next 24-bit pixel */
            }
            
            /* Advance to next scanline */
            pbSource += ddsd.lPitch;
            pbDest += (dwPixelCount * 3);  /* 24-bit stride */
        }
        
        /* Update bytes written to reflect 24-bit output */
        *pdwBytesWritten = pContext->screenInfo.dwWidth * 3 * pContext->screenInfo.dwHeight;
        
        DDraw_Log("Conversion complete: %lu bytes (BGRA) -> %lu bytes (BGR)",
                  dwExpectedSize, *pdwBytesWritten);
    } else {
        /* Not 32-bit, copy as-is */
        dwCopySize = pContext->screenInfo.dwWidth * (pContext->screenInfo.dwBitCount / 8);
        
        for (dwRow = 0; dwRow < pContext->screenInfo.dwHeight; dwRow++) {
            CopyMemory(pbDest, pbSource, dwCopySize);
            pbSource += ddsd.lPitch;
            pbDest += dwCopySize;
        }
        
        *pdwBytesWritten = dwExpectedSize;
    }
    
    /* Unlock surface */
    DDraw_UnlockSurface(pContext);
    
    DDraw_Log("Frame copied: %lux%lu, %lu bits/pixel, %lu bytes",
              pContext->screenInfo.dwWidth,
              pContext->screenInfo.dwHeight,
              pContext->screenInfo.dwBitCount,
              *pdwBytesWritten);
    
    return DDRAW_STATUS_SUCCESS;
}

/* ========================================================================
 * VALIDATION & RECOVERY (Recursive Error Recovery)
 * ======================================================================== */

DDRAW_STATUS WINAPI DDraw_ValidateAndRecover(DDRAW_CONTEXT *pContext)
{
    if (!pContext || !pContext->bInitialized) {
        return DDRAW_STATUS_NOT_INITIALIZED;
    }
    
    /* Check for unrecoverable errors */
    if (pContext->dwErrorCount > 100) {
        DDraw_Log("WARNING: Error count exceeded 100, attempting recovery");
        
        /* Attempt recovery by cleanup and reinit */
        DDraw_Cleanup(pContext);
        
        if (DDraw_Initialize(pContext) == DDRAW_STATUS_SUCCESS) {
            pContext->dwErrorCount = 0;
            return DDRAW_STATUS_RECOVERED;
        } else {
            pContext->bUsingFallback = TRUE;
            return DDRAW_STATUS_NOT_INITIALIZED;
        }
    }
    
    return DDRAW_STATUS_SUCCESS;
}

/* ========================================================================
 * QUERY FUNCTIONS
 * ======================================================================== */

DWORD WINAPI DDraw_GetLastDDError(DDRAW_CONTEXT *pContext)
{
    if (!pContext) {
        return 0;
    }
    return pContext->dwLastErrorCode;
}

const DDRAW_SCREEN_INFO *WINAPI DDraw_GetScreenInfo(DDRAW_CONTEXT *pContext)
{
    if (!pContext || !pContext->bScreenInfoValid) {
        return NULL;
    }
    return &pContext->screenInfo;
}

/* End of screen_capture_ddraw.c */
