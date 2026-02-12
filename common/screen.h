/*
 * RemoteDesk2K - Screen Capture Module
 */

#ifndef _REMOTEDESK2K_SCREEN_H_
#define _REMOTEDESK2K_SCREEN_H_

#include "common.h"

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
} SCREEN_CAPTURE, *PSCREEN_CAPTURE;

PSCREEN_CAPTURE ScreenCapture_Create(void);
void ScreenCapture_Destroy(PSCREEN_CAPTURE pCapture);
int ScreenCapture_CaptureScreen(PSCREEN_CAPTURE pCapture);
void ScreenCapture_GetDimensions(int *pWidth, int *pHeight);
int ScreenCapture_GetColorDepth(void);
DWORD CompressRLE(const BYTE *pSrc, DWORD srcSize, BYTE *pDst, DWORD dstMaxSize);
DWORD DecompressRLE(const BYTE *pSrc, DWORD srcSize, BYTE *pDst, DWORD dstMaxSize);
int FindDirtyRects(const BYTE *pOldFrame, const BYTE *pNewFrame, 
                   int width, int height, int bytesPerPixel,
                   RECT *pRects, int maxRects);

#endif
