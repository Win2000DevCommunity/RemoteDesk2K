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
    
    /* DIB section will be created fresh on each frame capture */
    ScreenLog("[CREATE] DIB creation deferred to frame capture\r\n");
    
    /* CRITICAL: Calculate pixelDataSize BEFORE allocating buffers that use it! */
    pCapture->pixelDataSize = ((pCapture->width * 3 + 3) & ~3) * pCapture->height;
    
    sprintf(buf, "[CREATE] Calculated pixelDataSize = %d bytes\r\n", pCapture->pixelDataSize);
    ScreenLog(buf);
    
    /* Allocate persistent pixel buffer to copy data into from each frame's DIB */
    pCapture->pPixelData = (BYTE*)calloc(1, pCapture->pixelDataSize);
    if (!pCapture->pPixelData) {
        ScreenLog("[CREATE] FAILED to allocate pPixelData buffer\r\n");
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Allocated persistent pixel buffer\r\n");
    
    pCapture->pPrevFrame = (BYTE*)calloc(1, pCapture->pixelDataSize);
    pCapture->compressBufferSize = pCapture->pixelDataSize + (pCapture->pixelDataSize / 8) + 256;
    pCapture->pCompressBuffer = (BYTE*)calloc(1, pCapture->compressBufferSize);
    
    if (!pCapture->pPrevFrame || !pCapture->pCompressBuffer) {
        ScreenLog("[CREATE] FAILED to allocate frame/compress buffers\r\n");
        if (pCapture->pPixelData) free(pCapture->pPixelData);
        if (pCapture->pPrevFrame) free(pCapture->pPrevFrame);
        if (pCapture->pCompressBuffer) free(pCapture->pCompressBuffer);
        free(pCapture);
        return NULL;
    }
    
    ScreenLog("[CREATE] Screen capture initialization COMPLETE\r\n");
    return pCapture;
}

void ScreenCapture_Destroy(PSCREEN_CAPTURE pCapture)
{
    if (!pCapture) return;
    
    /* Free all allocated buffers */
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
    
    if (!pCapture) {
        ScreenLog("[CAPTURE] Invalid pCapture\r\n");
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Starting screen capture\r\n");
    
    /* Get fresh screen DC on each frame */
    hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        ScreenLog("[CAPTURE] FAILED GetDC(NULL)\r\n");
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Got screen DC\r\n");
    
    /* Create fresh memory DC from current screen DC */
    hdcMemory = CreateCompatibleDC(hdcScreen);
    if (!hdcMemory) {
        ScreenLog("[CAPTURE] FAILED CreateCompatibleDC\r\n");
        ReleaseDC(NULL, hdcScreen);
        return RD2K_ERR_SCREEN;
    }
    
    ScreenLog("[CAPTURE] Created memory DC\r\n");
    
    /* Create FRESH DIB section on each frame (not reusing) */
    ZeroMemory(&pCapture->bmpInfo, sizeof(BITMAPINFO));
    pCapture->bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pCapture->bmpInfo.bmiHeader.biWidth = pCapture->width;
    pCapture->bmpInfo.bmiHeader.biHeight = -pCapture->height;
    pCapture->bmpInfo.bmiHeader.biPlanes = 1;
    pCapture->bmpInfo.bmiHeader.biBitCount = 24;
    pCapture->bmpInfo.bmiHeader.biCompression = BI_RGB;
    
    hBitmap = CreateDIBSection(
        hdcMemory, &pCapture->bmpInfo, DIB_RGB_COLORS,
        (void**)&pPixelData, NULL, 0);
    
    if (!hBitmap || !pPixelData) {
        ScreenLog("[CAPTURE] FAILED CreateDIBSection\r\n");
        DeleteDC(hdcMemory);
        ReleaseDC(NULL, hdcScreen);
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
    } else {
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
