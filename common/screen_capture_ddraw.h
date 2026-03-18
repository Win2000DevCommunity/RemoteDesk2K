/*
 * RemoteDesk2K - Screen Capture via DirectDraw GPU Acceleration
 * Layer 1: GPU-Accelerated DirectDraw (Windows 2000 optimized)
 * Layer 2: GDI Fallback (software-based)
 */

#ifndef __SCREEN_CAPTURE_DDRAW_H__
#define __SCREEN_CAPTURE_DDRAW_H__

#include <windows.h>
#include <ddraw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * FEATURE FLAGS & CONFIGURATION
 * ======================================================================== */

#define DDRAW_ENABLE_CAPTURE            1  /* Enable DirectDraw capture */
#define DDRAW_ENABLE_FALLBACK           1  /* Fallback to GDI if DirectDraw fails */
#define DDRAW_ENABLE_DETAILED_LOGGING   1  /* Enable debug logging */
#define DDRAW_MAX_INIT_ATTEMPTS         3  /* Retry count for init */
#define DDRAW_TIMEOUT_MS                100 /* Max time for lock operations */

/* ========================================================================
 * ERROR CODES & STATUS
 * ======================================================================== */

typedef enum _DDRAW_STATUS {
    DDRAW_STATUS_SUCCESS = 0x00000000L,
    DDRAW_STATUS_NOT_INITIALIZED = 0x80000001L,
    DDRAW_STATUS_LIBRARY_NOT_FOUND = 0x80000002L,
    DDRAW_STATUS_PROC_NOT_FOUND = 0x80000003L,
    DDRAW_STATUS_OBJECT_CREATE_FAILED = 0x80000004L,
    DDRAW_STATUS_SURFACE_CREATE_FAILED = 0x80000005L,
    DDRAW_STATUS_SURFACE_LOCK_FAILED = 0x80000006L,
    DDRAW_STATUS_INVALID_PARAMETERS = 0x80000007L,
    DDRAW_STATUS_MEMORY_ERROR = 0x80000008L,
    DDRAW_STATUS_DISPLAY_MODE_FAILED = 0x80000009L,
    DDRAW_STATUS_UNSUPPORTED_FORMAT = 0x8000000AL,
    DDRAW_STATUS_RESOURCE_BUSY = 0x8000000BL,
    DDRAW_STATUS_WINDOW_NOT_FOUND = 0x8000000CL,
    DDRAW_STATUS_FALLBACK_ACTIVE = 0x00000100L,  /* Success but using GDI fallback */
    DDRAW_STATUS_RECOVERED = 0x00000200L         /* Status recovered from error */
} DDRAW_STATUS;

/* ========================================================================
 * SCREEN INFORMATION STRUCTURE
 * ======================================================================== */

typedef struct _DDRAW_SCREEN_INFO {
    DWORD dwWidth;              /* Screen width in pixels */
    DWORD dwHeight;             /* Screen height in pixels */
    DWORD dwBitCount;           /* Bits per pixel (8, 16, 24, 32) */
    DWORD dwPitch;              /* Bytes per scanline (stride) */
    DWORD dwRMask;              /* Red channel mask */
    DWORD dwGMask;              /* Green channel mask */
    DWORD dwBMask;              /* Blue channel mask */
    DWORD dwAMask;              /* Alpha channel mask */
    BOOL bHasAlpha;             /* TRUE if alpha channel exists */
    BOOL bIsRGB;                /* TRUE if RGB format, FALSE if YUV/other */
} DDRAW_SCREEN_INFO;

/* ========================================================================
 * RECURSIVE CONTEXT STRUCTURE (RAII Pattern in C)
 * ======================================================================== */

typedef struct _DDRAW_CONTEXT {
    /* Library management (DirectDraw GPU Acceleration) */
    HMODULE hDDrawLibrary;
    BOOL bLibraryLoaded;
    
    /* COM Interface pointers */
    LPDIRECTDRAW pDirectDraw;
    LPDIRECTDRAWSURFACE pPrimarySurface;
    
    /* Display information */
    DDRAW_SCREEN_INFO screenInfo;
    BOOL bScreenInfoValid;
    
    /* Capture state */
    BOOL bInitialized;
    BOOL bSurfaceLocked;
    DWORD dwInitAttempts;
    
    /* Performance tracking */
    DWORD dwLastErrorCode;
    DWORD dwErrorCount;
    DWORD dwSuccessCount;
    
    /* Fallback mode */
    BOOL bUsingFallback;
    
    /* Thread safety */
    HANDLE hMutex;
    
} DDRAW_CONTEXT;

/* ========================================================================
 * FUNCTION PROTOTYPES
 * ======================================================================== */

/*
 * Initialize DirectDraw with recursive resource management
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Initialized successfully
 *   DDRAW_STATUS_LIBRARY_NOT_FOUND - ddraw.dll not available (fallback active)
 *   DDRAW_STATUS_* - Specific error code
 */
DDRAW_STATUS WINAPI DDraw_Initialize(DDRAW_CONTEXT *pContext);

/*
 * Cleanup DirectDraw resources with recursive pattern
 * Safely releases all COM objects and libraries
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Cleanup completed
 */
DDRAW_STATUS WINAPI DDraw_Cleanup(DDRAW_CONTEXT *pContext);

/*
 * Query current display mode and screen information
 * Must be called after Initialize
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Screen info valid
 *   DDRAW_STATUS_NOT_INITIALIZED - Call Initialize first
 */
DDRAW_STATUS WINAPI DDraw_GetDisplayMode(DDRAW_CONTEXT *pContext);

/*
 * Lock primary surface for screen capture
 * 
 * PARAMETERS:
 *   pContext - DirectDraw context
 *   pSurfaceDesc - Output DDSURFACEDESC with frame buffer pointer
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Surface locked, lpSurface contains buffer
 *   DDRAW_STATUS_SURFACE_LOCK_FAILED - Could not acquire lock
 *   DDRAW_STATUS_RESOURCE_BUSY - Surface busy (try again)
 */
DDRAW_STATUS WINAPI DDraw_LockSurface(DDRAW_CONTEXT *pContext, 
                                       LPDDSURFACEDESC pSurfaceDesc);

/*
 * Unlock primary surface after capture
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Surface unlocked
 */
DDRAW_STATUS WINAPI DDraw_UnlockSurface(DDRAW_CONTEXT *pContext);

/*
 * Copy frame buffer to target memory with multiple retry logic
 * Handles pitch conversion and stride alignment
 * 
 * PARAMETERS:
 *   pContext - DirectDraw context
 *   pTargetBuffer - Output buffer for frame data
 *   dwTargetSize - Size of output buffer
 *   pdwBytesWritten - Output bytes actually written
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Frame copied successfully
 *   DDRAW_STATUS_SURFACE_LOCK_FAILED - Lock retry exceeded
 *   DDRAW_STATUS_INVALID_PARAMETERS - Invalid parameters
 */
DDRAW_STATUS WINAPI DDraw_CopyFrameBuffer(DDRAW_CONTEXT *pContext,
                                          LPVOID pTargetBuffer,
                                          DWORD dwTargetSize,
                                          LPDWORD pdwBytesWritten);

/*
 * Get last DirectDraw error code for diagnostics
 * 
 * RETURNS:
 *   DWORD - HRESULT from last DirectDraw call
 */
DWORD WINAPI DDraw_GetLastDDError(DDRAW_CONTEXT *pContext);

/*
 * Get error string description
 * Converts DDRAW_STATUS to human-readable string
 * 
 * RETURNS:
 *   Pointer to static error string
 */
const char *WINAPI DDraw_StatusToString(DDRAW_STATUS status);

/*
 * Validate DirectDraw context and recover from errors
 * Recursive health check with automatic reinitalization
 * 
 * RETURNS:
 *   DDRAW_STATUS_SUCCESS - Context valid
 *   DDRAW_STATUS_RECOVERED - Recovered from error
 *   DDRAW_STATUS_NOT_INITIALIZED - Cannot recover
 */
DDRAW_STATUS WINAPI DDraw_ValidateAndRecover(DDRAW_CONTEXT *pContext);

/*
 * Get screen information (width, height, format)
 * 
 * RETURNS:
 *   Pointer to DDRAW_SCREEN_INFO structure (read-only)
 */
const DDRAW_SCREEN_INFO *WINAPI DDraw_GetScreenInfo(DDRAW_CONTEXT *pContext);

/*
 * Create uninitialized context
 * User must call DDraw_Initialize before use
 * 
 * RETURNS:
 *   Pointer to allocated context or NULL if out of memory
 */
DDRAW_CONTEXT *WINAPI DDraw_CreateContext(void);

/*
 * Destroy context and recursively free all resources
 * 
 * PARAMETERS:
 *   ppContext - Pointer to context pointer (set to NULL on return)
 */
void WINAPI DDraw_DestroyContext(DDRAW_CONTEXT **ppContext);

#ifdef __cplusplus
}
#endif

#endif /* __SCREEN_CAPTURE_DDRAW_H__ */
