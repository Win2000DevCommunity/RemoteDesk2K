/*
 * RemoteDesk2K - Screen Capture Module
 */

#ifndef _REMOTEDESK2K_SCREEN_H_
#define _REMOTEDESK2K_SCREEN_H_

#include "common.h"
#include "desktop.h"

/* Include DirectDraw header - provides GPU-accelerated screen capture */
#include "screen_capture_ddraw.h"

typedef struct _SCREEN_CAPTURE {
    HDC         hdcScreen;
    HDC         hdcMemory;
    HBITMAP     hBitmap;
    HBITMAP     hBitmapOld;
    BITMAPINFO  bmpInfo;
    int         width;
    int         height;
    int         bitsPerPixel;
    BYTE       *pPixelData;
    DWORD       pixelDataSize;
    BYTE       *pPrevFrame;
    BYTE       *pCompressBuffer;
    DWORD       compressBufferSize;
    
    /* DirectDraw GPU-accelerated capture (optional) */
    DDRAW_CONTEXT *pDDrawContext;  /* DirectDraw context for fast capture */
    BOOL        bUseDDraw;         /* TRUE if DirectDraw is active and working */
    BOOL        bDDrawFailed;      /* TRUE if DirectDraw failed, use GDI fallback */
    
    /* Desktop context for UAC/Winlogon detection and switching */
    PDESKTOP_CONTEXT pDesktopContext;  /* Desktop enumeration and switching */
    
    /* Winlogon input drain thread - kept alive during network send so
     * input events can be injected while the main thread sends frame data */
    HANDLE hDrainThread;       /* Worker thread kept alive for input draining */
    HANDLE hDrainStopEvent;    /* Manual-reset event to signal drain loop exit */
} SCREEN_CAPTURE, *PSCREEN_CAPTURE;

/* Compilation flags for DirectDraw support */
#define DDRAW_ENABLE_CAPTURE 1  /* Set to 0 to disable DirectDraw (GDI only) */

PSCREEN_CAPTURE ScreenCapture_Create(void);
void ScreenCapture_Destroy(PSCREEN_CAPTURE pCapture);
int ScreenCapture_CaptureScreen(PSCREEN_CAPTURE pCapture);
void ScreenCapture_GetDimensions(int *pWidth, int *pHeight);
int ScreenCapture_GetColorDepth(void);
BOOL ScreenCapture_SyncDisplayMode(PSCREEN_CAPTURE pCapture);  /* Sync dimensions from DirectDraw */
void ScreenCapture_StopDrainThread(PSCREEN_CAPTURE pCapture);  /* Stop Winlogon input drain thread */
DWORD CompressRLE(const BYTE *pSrc, DWORD srcSize, BYTE *pDst, DWORD dstMaxSize);
DWORD DecompressRLE(const BYTE *pSrc, DWORD srcSize, BYTE *pDst, DWORD dstMaxSize);
int FindDirtyRects(const BYTE *pOldFrame, const BYTE *pNewFrame, 
                   int width, int height, int bytesPerPixel,
                   RECT *pRects, int maxRects);

#endif
