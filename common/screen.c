/*
 * RemoteDesk2K - Screen Capture Implementation
 */

#include "screen.h"
#include <stdio.h>

static void ScreenLog(const char *msg)
{
    FILE *f = fopen("rd2k_debug.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [SCREEN] %s", 
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
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
    
    ScreenCapture_GetDimensions(&pCapture->width, &pCapture->height);
    pCapture->bitsPerPixel = ScreenCapture_GetColorDepth();
    
    sprintf(buf, "[CREATE] Screen dimensions: %dx%d, BPP=%d\r\n", 
            pCapture->width, pCapture->height, pCapture->bitsPerPixel);
    ScreenLog(buf);
    
    hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        ScreenLog("[CREATE] FAILED GetDC(NULL)\r\n");
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Got screen DC\r\n");
    
    pCapture->hdcScreen = hdcScreen;
    pCapture->hdcMemory = CreateCompatibleDC(hdcScreen);
    if (!pCapture->hdcMemory) {
        ScreenLog("[CREATE] FAILED CreateCompatibleDC\r\n");
        ReleaseDC(NULL, hdcScreen);
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Created compatible DC for memory\r\n");
    
    ZeroMemory(&pCapture->bmpInfo, sizeof(BITMAPINFO));
    pCapture->bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pCapture->bmpInfo.bmiHeader.biWidth = pCapture->width;
    pCapture->bmpInfo.bmiHeader.biHeight = -pCapture->height;
    pCapture->bmpInfo.bmiHeader.biPlanes = 1;
    pCapture->bmpInfo.bmiHeader.biBitCount = 24;
    pCapture->bmpInfo.bmiHeader.biCompression = BI_RGB;
    
    pCapture->hBitmap = CreateDIBSection(
        pCapture->hdcMemory, &pCapture->bmpInfo, DIB_RGB_COLORS,
        (void**)&pCapture->pPixelData, NULL, 0);
    
    if (!pCapture->hBitmap) {
        ScreenLog("[CREATE] FAILED CreateDIBSection\r\n");
        DeleteDC(pCapture->hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Created DIB section\r\n");
    
    pCapture->hBitmapOld = (HBITMAP)SelectObject(pCapture->hdcMemory, pCapture->hBitmap);
    pCapture->pixelDataSize = ((pCapture->width * 3 + 3) & ~3) * pCapture->height;
    pCapture->pPrevFrame = (BYTE*)calloc(1, pCapture->pixelDataSize);
    pCapture->compressBufferSize = pCapture->pixelDataSize + (pCapture->pixelDataSize / 8) + 256;
    pCapture->pCompressBuffer = (BYTE*)calloc(1, pCapture->compressBufferSize);
    
    if (!pCapture->pPrevFrame || !pCapture->pCompressBuffer) {
        ScreenLog("[CREATE] FAILED to allocate frame/compress buffers\r\n");
        if (pCapture->pPrevFrame) free(pCapture->pPrevFrame);
        if (pCapture->pCompressBuffer) free(pCapture->pCompressBuffer);
        DeleteDC(pCapture->hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        DeleteObject(pCapture->hBitmap);
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Screen capture initialization COMPLETE\r\n");
    return pCapture;
}

void ScreenCapture_Destroy(PSCREEN_CAPTURE pCapture)
{
    if (!pCapture) return;
    
    SAFE_FREE(pCapture->pPrevFrame);
    SAFE_FREE(pCapture->pCompressBuffer);
    
    if (pCapture->hdcMemory) {
        if (pCapture->hBitmapOld) {
            SelectObject(pCapture->hdcMemory, pCapture->hBitmapOld);
        }
        DeleteDC(pCapture->hdcMemory);
    }
    
    if (pCapture->hBitmap) DeleteObject(pCapture->hBitmap);
    if (pCapture->hdcScreen) ReleaseDC(NULL, pCapture->hdcScreen);
    
    free(pCapture);
}

int ScreenCapture_CaptureScreen(PSCREEN_CAPTURE pCapture)
{
    HDC hdcScreen, hdcMemory;
    HBITMAP hBitmapOld;
    int result = RD2K_ERR_SCREEN;
    
    if (!pCapture || !pCapture->pPixelData) {
        ScreenLog("[CAPTURE] Invalid pCapture or pPixelData\r\n");
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Starting screen capture\r\n");
    
    /* Recreate device contexts on each capture to handle desktop switches */
    hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        ScreenLog("[CAPTURE] FAILED GetDC(NULL)\r\n");
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Got screen DC\r\n");
    
    hdcMemory = CreateCompatibleDC(hdcScreen);
    if (!hdcMemory) {
        ScreenLog("[CAPTURE] FAILED CreateCompatibleDC\r\n");
        ReleaseDC(NULL, hdcScreen);
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Created memory DC\r\n");
    
    /* Select the bitmap into the memory DC */
    hBitmapOld = (HBITMAP)SelectObject(hdcMemory, pCapture->hBitmap);
    if (!hBitmapOld) {
        ScreenLog("[CAPTURE] FAILED SelectObject\r\n");
        DeleteDC(hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Selected bitmap into DC\r\n");
    
    /* Perform the screen capture */
    if (!BitBlt(hdcMemory, 0, 0, 
                pCapture->width, pCapture->height,
                hdcScreen, 0, 0, SRCCOPY)) {
        ScreenLog("[CAPTURE] FAILED BitBlt - access violation likely here\r\n");
        SelectObject(hdcMemory, hBitmapOld);
        DeleteDC(hdcMemory);
        ReleaseDC(NULL, hdcScreen);
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] BitBlt successful\r\n");
    
    /* Restore and cleanup */
    SelectObject(hdcMemory, hBitmapOld);
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
    
    if (!pOldFrame || !pNewFrame || !pRects || maxRects <= 0) return 0;
    
    for (by = 0; by < height && numRects < maxRects; by += blockSize) {
        for (bx = 0; bx < width && numRects < maxRects; bx += blockSize) {
            int blockW = (bx + blockSize < width) ? blockSize : (width - bx);
            int blockH = (by + blockSize < height) ? blockSize : (height - by);
            int dirty = 0, y;
            
            for (y = 0; y < blockH && !dirty; y++) {
                int offset = (by + y) * stride + bx * bytesPerPixel;
                if (memcmp(pOldFrame + offset, pNewFrame + offset, blockW * bytesPerPixel) != 0) {
                    dirty = 1;
                }
            }
            
            if (dirty) {
                pRects[numRects].left = bx;
                pRects[numRects].top = by;
                pRects[numRects].right = bx + blockW;
                pRects[numRects].bottom = by + blockH;
                numRects++;
            }
        }
    }
    
    return numRects;
}
