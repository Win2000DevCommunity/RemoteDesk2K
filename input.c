/*
 * RemoteDesk2K - Input Module Implementation
 * Windows 2000 Compatible Input Simulation
 * 
 * Uses legacy mouse_event() and keybd_event() APIs which are
 * fully supported on Windows 2000 and later.
 * 
 * IMPORTANT: This module tracks modifier key state to prevent
 * keys from getting "stuck" (appearing held down to the system).
 * This can happen when:
 *  - Clipboard operations steal focus during key operations
 *  - Network delays cause key-up events to be lost
 *  - File transfers interrupt input processing
 */

#include "input.h"

/* Screen dimensions cache */
static int g_screenWidth = 0;
static int g_screenHeight = 0;

/* Modifier key state tracking - to prevent stuck keys */
static BOOL g_bCtrlHeld = FALSE;
static BOOL g_bShiftHeld = FALSE;
static BOOL g_bAltHeld = FALSE;
static BOOL g_bLWinHeld = FALSE;
static BOOL g_bRWinHeld = FALSE;

/* Get screen dimensions (cached) */
static void GetScreenSize(void)
{
    if (g_screenWidth == 0 || g_screenHeight == 0) {
        g_screenWidth = GetSystemMetrics(SM_CXSCREEN);
        g_screenHeight = GetSystemMetrics(SM_CYSCREEN);
    }
}

/*
 * Input_ReleaseAllModifiers - Release all modifier keys
 * 
 * Call this function when:
 *  - Before opening clipboard
 *  - Before showing dialogs
 *  - After file transfer completes
 *  - When viewer window loses focus
 * 
 * This prevents the "stuck key" problem where Explorer
 * or other apps think Ctrl/Shift/Alt is still held.
 */
void Input_ReleaseAllModifiers(void)
{
    /* Release Ctrl if held */
    if (g_bCtrlHeld) {
        keybd_event(VK_CONTROL, 0x1D, KEYEVENTF_KEYUP, 0);
        g_bCtrlHeld = FALSE;
    }
    
    /* Release Shift if held */
    if (g_bShiftHeld) {
        keybd_event(VK_SHIFT, 0x2A, KEYEVENTF_KEYUP, 0);
        g_bShiftHeld = FALSE;
    }
    
    /* Release Alt if held */
    if (g_bAltHeld) {
        keybd_event(VK_MENU, 0x38, KEYEVENTF_KEYUP, 0);
        g_bAltHeld = FALSE;
    }
    
    /* Release Windows keys if held */
    if (g_bLWinHeld) {
        keybd_event(VK_LWIN, 0x5B, KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY, 0);
        g_bLWinHeld = FALSE;
    }
    if (g_bRWinHeld) {
        keybd_event(VK_RWIN, 0x5C, KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY, 0);
        g_bRWinHeld = FALSE;
    }
}

/*
 * Input_SyncModifierState - Sync our tracking with actual keyboard state
 * 
 * This reads the real keyboard state and updates our tracking.
 * Call this when the viewer window regains focus to resync.
 */
void Input_SyncModifierState(void)
{
    /* Read actual keyboard state */
    g_bCtrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? TRUE : FALSE;
    g_bShiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? TRUE : FALSE;
    g_bAltHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) ? TRUE : FALSE;
    g_bLWinHeld = (GetAsyncKeyState(VK_LWIN) & 0x8000) ? TRUE : FALSE;
    g_bRWinHeld = (GetAsyncKeyState(VK_RWIN) & 0x8000) ? TRUE : FALSE;
}

/*
 * Check if any modifier is currently held (according to our tracking)
 */
BOOL Input_IsModifierHeld(void)
{
    return g_bCtrlHeld || g_bShiftHeld || g_bAltHeld || g_bLWinHeld || g_bRWinHeld;
}

/*
 * Input_MouseMove - Move mouse to absolute screen position
 * 
 * Windows 2000 mouse_event() with MOUSEEVENTF_ABSOLUTE:
 * - dx and dy must be in range 0-65535
 * - 0 = left/top edge, 65535 = right/bottom edge
 * - Must combine MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
 */
void Input_MouseMove(int x, int y)
{
    DWORD dx, dy;
    
    GetScreenSize();
    
    /* Clamp to screen bounds */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_screenWidth) x = g_screenWidth - 1;
    if (y >= g_screenHeight) y = g_screenHeight - 1;
    
    /* Convert to 0-65535 range */
    /* Formula: (coord * 65535) / (screen_size - 1) */
    /* Using 65536 / screen_size gives better precision */
    dx = (DWORD)((x * 65535) / (g_screenWidth - 1));
    dy = (DWORD)((y * 65535) / (g_screenHeight - 1));
    
    /* Perform absolute mouse move */
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, dx, dy, 0, 0);
}

/*
 * Input_MouseButton - Press or release mouse button
 * 
 * button: 1=left, 2=right, 3=middle
 * down: TRUE=press, FALSE=release
 */
void Input_MouseButton(int button, BOOL down)
{
    DWORD dwFlags = 0;
    
    switch (button) {
        case 1: /* Left button */
            dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case 2: /* Right button */
            dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case 3: /* Middle button */
            dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default:
            return;
    }
    
    mouse_event(dwFlags, 0, 0, 0, 0);
}

/*
 * Input_MouseWheel - Scroll mouse wheel
 * 
 * delta: Amount to scroll (positive = up, negative = down)
 * Standard delta is 120 per "click"
 */
void Input_MouseWheel(int delta)
{
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)delta, 0);
}

/*
 * Input_KeyPress - Press or release a keyboard key
 * 
 * vk: Virtual key code (VK_*)
 * scan: Hardware scan code
 * down: TRUE=press, FALSE=release
 * extended: TRUE for extended keys (arrows, insert, delete, numpad, etc.)
 * 
 * This function tracks modifier key state to prevent stuck keys.
 */
void Input_KeyPress(BYTE vk, BYTE scan, BOOL down, BOOL extended)
{
    DWORD dwFlags = 0;
    
    if (!down) {
        dwFlags |= KEYEVENTF_KEYUP;
    }
    
    if (extended) {
        dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    
    /* Track modifier key state */
    switch (vk) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            g_bCtrlHeld = down;
            break;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            g_bShiftHeld = down;
            break;
        case VK_MENU:    /* Alt */
        case VK_LMENU:
        case VK_RMENU:
            g_bAltHeld = down;
            break;
        case VK_LWIN:
            g_bLWinHeld = down;
            break;
        case VK_RWIN:
            g_bRWinHeld = down;
            break;
    }
    
    keybd_event(vk, scan, dwFlags, 0);
}

/*
 * Input_ProcessMouseEvent - Process mouse event from network
 * 
 * Protocol flags:
 *   0x01 = Mouse move
 *   0x02 = Button down
 *   0x04 = Button up
 *   0x08 = Wheel scroll
 * 
 * Protocol buttons:
 *   0x01 = Left button
 *   0x02 = Right button
 *   0x04 = Middle button
 */
BOOL Input_ProcessMouseEvent(WORD x, WORD y, BYTE buttons, BYTE flags, SHORT wheelDelta)
{
    /* Always move mouse to position first if move flag is set */
    if (flags & 0x01) {
        Input_MouseMove((int)x, (int)y);
    }
    
    /* Process button down events */
    if (flags & 0x02) {
        if (buttons & 0x01) Input_MouseButton(1, TRUE);  /* Left down */
        if (buttons & 0x02) Input_MouseButton(2, TRUE);  /* Right down */
        if (buttons & 0x04) Input_MouseButton(3, TRUE);  /* Middle down */
    }
    
    /* Process button up events */
    if (flags & 0x04) {
        if (buttons & 0x01) Input_MouseButton(1, FALSE); /* Left up */
        if (buttons & 0x02) Input_MouseButton(2, FALSE); /* Right up */
        if (buttons & 0x04) Input_MouseButton(3, FALSE); /* Middle up */
    }
    
    /* Process wheel scroll */
    if (flags & 0x08) {
        Input_MouseWheel((int)wheelDelta);
    }
    
    return TRUE;
}

/*
 * Input_ProcessKeyEvent - Process keyboard event from network
 * 
 * Protocol flags:
 *   0x01 = Key down
 *   0x02 = Key up
 *   0x04 = Extended key
 */
BOOL Input_ProcessKeyEvent(WORD vk, WORD scan, BYTE flags)
{
    BOOL down = (flags & 0x01) ? TRUE : FALSE;
    BOOL up = (flags & 0x02) ? TRUE : FALSE;
    BOOL extended = (flags & 0x04) ? TRUE : FALSE;
    
    /* If neither down nor up specified, assume down */
    if (!down && !up) {
        down = TRUE;
    }
    
    /* Key down */
    if (down && !up) {
        Input_KeyPress((BYTE)vk, (BYTE)scan, TRUE, extended);
    }
    
    /* Key up */
    if (up) {
        Input_KeyPress((BYTE)vk, (BYTE)scan, FALSE, extended);
    }
    
    return TRUE;
}
