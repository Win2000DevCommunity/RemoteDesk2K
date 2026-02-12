/*
 * RemoteDesk2K - Input Module Implementation
 * Windows 2000 Compatible Input Simulation
 * 
 * Uses legacy mouse_event() and keybd_event() APIs which are
 * fully supported on Windows 2000 and later.
 * 
 * ASYNC INPUT PROCESSING:
 * Input events are queued and processed in a separate thread.
 * This prevents UI freeze when clicking on window title bars,
 * minimize/maximize/close buttons, or system menus - which
 * cause Windows to enter modal tracking loops.
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

/* ============ ASYNC INPUT QUEUE ============ */

/* Input event types for the queue */
#define INPUT_EVT_MOUSE     1
#define INPUT_EVT_KEY       2

/* Input event structure */
typedef struct _INPUT_EVENT {
    BYTE    type;           /* INPUT_EVT_MOUSE or INPUT_EVT_KEY */
    union {
        struct {
            WORD    x;
            WORD    y;
            BYTE    buttons;
            BYTE    flags;
            SHORT   wheelDelta;
        } mouse;
        struct {
            WORD    vk;
            WORD    scan;
            BYTE    flags;
        } key;
    };
} INPUT_EVENT, *PINPUT_EVENT;

/* Circular queue for input events */
#define INPUT_QUEUE_SIZE    256
static INPUT_EVENT      g_inputQueue[INPUT_QUEUE_SIZE];
static volatile LONG    g_inputQueueHead = 0;   /* Write position */
static volatile LONG    g_inputQueueTail = 0;   /* Read position */

/* Thread synchronization */
static HANDLE           g_hInputThread = NULL;
static HANDLE           g_hInputEvent = NULL;   /* Signaled when queue has data */
static HANDLE           g_hInputStopEvent = NULL;
static CRITICAL_SECTION g_csInputQueue;
static BOOL             g_bInputInitialized = FALSE;

/* Forward declarations for internal functions */
static void DoMouseMove(int x, int y);
static void DoMouseButton(int button, BOOL down);
static void DoMouseWheel(int delta);
static void DoKeyPress(BYTE vk, BYTE scan, BOOL down, BOOL extended);
static DWORD WINAPI InputThreadProc(LPVOID lpParam);

/* ============ INITIALIZATION/SHUTDOWN ============ */

/*
 * Input_Initialize - Start the async input thread
 * Returns TRUE on success
 */
BOOL Input_Initialize(void)
{
    if (g_bInputInitialized) return TRUE;
    
    InitializeCriticalSection(&g_csInputQueue);
    
    /* Create events */
    g_hInputEvent = CreateEventA(NULL, FALSE, FALSE, NULL);  /* Auto-reset */
    g_hInputStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);  /* Manual reset */
    
    if (!g_hInputEvent || !g_hInputStopEvent) {
        if (g_hInputEvent) CloseHandle(g_hInputEvent);
        if (g_hInputStopEvent) CloseHandle(g_hInputStopEvent);
        DeleteCriticalSection(&g_csInputQueue);
        return FALSE;
    }
    
    /* Create the input processing thread */
    g_hInputThread = CreateThread(NULL, 0, InputThreadProc, NULL, 0, NULL);
    if (!g_hInputThread) {
        CloseHandle(g_hInputEvent);
        CloseHandle(g_hInputStopEvent);
        DeleteCriticalSection(&g_csInputQueue);
        return FALSE;
    }
    
    g_bInputInitialized = TRUE;
    return TRUE;
}

/*
 * Input_Shutdown - Stop the async input thread
 */
void Input_Shutdown(void)
{
    if (!g_bInputInitialized) return;
    
    /* Signal thread to stop */
    SetEvent(g_hInputStopEvent);
    
    /* Wait for thread to finish (max 2 seconds) */
    if (g_hInputThread) {
        WaitForSingleObject(g_hInputThread, 2000);
        CloseHandle(g_hInputThread);
        g_hInputThread = NULL;
    }
    
    /* Cleanup */
    if (g_hInputEvent) {
        CloseHandle(g_hInputEvent);
        g_hInputEvent = NULL;
    }
    if (g_hInputStopEvent) {
        CloseHandle(g_hInputStopEvent);
        g_hInputStopEvent = NULL;
    }
    
    DeleteCriticalSection(&g_csInputQueue);
    g_bInputInitialized = FALSE;
}

/* ============ QUEUE OPERATIONS ============ */

/* Add event to queue (thread-safe) */
static BOOL QueueInputEvent(PINPUT_EVENT pEvent)
{
    LONG nextHead;
    
    if (!g_bInputInitialized) {
        /* Fallback: process immediately if not initialized */
        if (pEvent->type == INPUT_EVT_MOUSE) {
            DoMouseMove((int)pEvent->mouse.x, (int)pEvent->mouse.y);
            if (pEvent->mouse.flags & 0x02) {
                if (pEvent->mouse.buttons & 0x01) DoMouseButton(1, TRUE);
                if (pEvent->mouse.buttons & 0x02) DoMouseButton(2, TRUE);
                if (pEvent->mouse.buttons & 0x04) DoMouseButton(3, TRUE);
            }
            if (pEvent->mouse.flags & 0x04) {
                if (pEvent->mouse.buttons & 0x01) DoMouseButton(1, FALSE);
                if (pEvent->mouse.buttons & 0x02) DoMouseButton(2, FALSE);
                if (pEvent->mouse.buttons & 0x04) DoMouseButton(3, FALSE);
            }
            if (pEvent->mouse.flags & 0x08) {
                DoMouseWheel((int)pEvent->mouse.wheelDelta);
            }
        }
        return TRUE;
    }
    
    EnterCriticalSection(&g_csInputQueue);
    
    nextHead = (g_inputQueueHead + 1) % INPUT_QUEUE_SIZE;
    if (nextHead == g_inputQueueTail) {
        /* Queue full - drop oldest event */
        g_inputQueueTail = (g_inputQueueTail + 1) % INPUT_QUEUE_SIZE;
    }
    
    g_inputQueue[g_inputQueueHead] = *pEvent;
    g_inputQueueHead = nextHead;
    
    LeaveCriticalSection(&g_csInputQueue);
    
    /* Signal the input thread */
    SetEvent(g_hInputEvent);
    
    return TRUE;
}

/* Get event from queue (thread-safe) */
static BOOL DequeueInputEvent(PINPUT_EVENT pEvent)
{
    BOOL hasEvent = FALSE;
    
    EnterCriticalSection(&g_csInputQueue);
    
    if (g_inputQueueHead != g_inputQueueTail) {
        *pEvent = g_inputQueue[g_inputQueueTail];
        g_inputQueueTail = (g_inputQueueTail + 1) % INPUT_QUEUE_SIZE;
        hasEvent = TRUE;
    }
    
    LeaveCriticalSection(&g_csInputQueue);
    
    return hasEvent;
}

/* ============ INPUT THREAD ============ */

/*
 * Input processing thread
 * Processes events from queue in order
 * This runs separately from the main/network thread,
 * so modal loops (system menus, title bar clicks) don't block networking
 */
static DWORD WINAPI InputThreadProc(LPVOID lpParam)
{
    HANDLE handles[2];
    INPUT_EVENT evt;
    DWORD waitResult;
    
    (void)lpParam;  /* Unused */
    
    handles[0] = g_hInputStopEvent;
    handles[1] = g_hInputEvent;
    
    while (1) {
        /* Wait for either stop signal or new event */
        waitResult = WaitForMultipleObjects(2, handles, FALSE, 100);
        
        if (waitResult == WAIT_OBJECT_0) {
            /* Stop event signaled */
            break;
        }
        
        /* Process all queued events */
        while (DequeueInputEvent(&evt)) {
            if (evt.type == INPUT_EVT_MOUSE) {
                /* Move mouse first */
                if (evt.mouse.flags & 0x01) {
                    DoMouseMove((int)evt.mouse.x, (int)evt.mouse.y);
                }
                
                /* Button down */
                if (evt.mouse.flags & 0x02) {
                    if (evt.mouse.buttons & 0x01) DoMouseButton(1, TRUE);
                    if (evt.mouse.buttons & 0x02) DoMouseButton(2, TRUE);
                    if (evt.mouse.buttons & 0x04) DoMouseButton(3, TRUE);
                }
                
                /* Button up */
                if (evt.mouse.flags & 0x04) {
                    if (evt.mouse.buttons & 0x01) DoMouseButton(1, FALSE);
                    if (evt.mouse.buttons & 0x02) DoMouseButton(2, FALSE);
                    if (evt.mouse.buttons & 0x04) DoMouseButton(3, FALSE);
                }
                
                /* Wheel */
                if (evt.mouse.flags & 0x08) {
                    DoMouseWheel((int)evt.mouse.wheelDelta);
                }
            }
            else if (evt.type == INPUT_EVT_KEY) {
                BOOL down = (evt.key.flags & 0x01) ? TRUE : FALSE;
                BOOL up = (evt.key.flags & 0x02) ? TRUE : FALSE;
                BOOL extended = (evt.key.flags & 0x04) ? TRUE : FALSE;
                
                if (!down && !up) down = TRUE;
                
                if (down && !up) {
                    DoKeyPress((BYTE)evt.key.vk, (BYTE)evt.key.scan, TRUE, extended);
                }
                if (up) {
                    DoKeyPress((BYTE)evt.key.vk, (BYTE)evt.key.scan, FALSE, extended);
                }
            }
        }
    }
    
    return 0;
}

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

/* ============ INTERNAL INPUT FUNCTIONS (Do*) ============ */
/* These execute input directly - called from the input thread */

/*
 * DoMouseMove - Move mouse to absolute screen position (internal)
 */
static void DoMouseMove(int x, int y)
{
    DWORD dx, dy;
    
    GetScreenSize();
    
    /* Clamp to screen bounds */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_screenWidth) x = g_screenWidth - 1;
    if (y >= g_screenHeight) y = g_screenHeight - 1;
    
    /* Convert to 0-65535 range */
    dx = (DWORD)((x * 65535) / (g_screenWidth - 1));
    dy = (DWORD)((y * 65535) / (g_screenHeight - 1));
    
    /* Perform absolute mouse move */
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, dx, dy, 0, 0);
}

/*
 * DoMouseButton - Press or release mouse button (internal)
 */
static void DoMouseButton(int button, BOOL down)
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
 * DoMouseWheel - Scroll mouse wheel (internal)
 */
static void DoMouseWheel(int delta)
{
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)delta, 0);
}

/*
 * DoKeyPress - Press or release a keyboard key (internal)
 */
static void DoKeyPress(BYTE vk, BYTE scan, BOOL down, BOOL extended)
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
        case VK_MENU:
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

/* ============ PUBLIC INPUT FUNCTIONS ============ */
/* These can be called directly if needed */

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
    DoMouseMove(x, y);
}

/*
 * Input_MouseButton - Press or release mouse button
 * 
 * button: 1=left, 2=right, 3=middle
 * down: TRUE=press, FALSE=release
 */
void Input_MouseButton(int button, BOOL down)
{
    DoMouseButton(button, down);
}

/*
 * Input_MouseWheel - Scroll mouse wheel
 * 
 * delta: Amount to scroll (positive = up, negative = down)
 * Standard delta is 120 per "click"
 */
void Input_MouseWheel(int delta)
{
    DoMouseWheel(delta);
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
    DoKeyPress(vk, scan, down, extended);
}

/*
 * Input_ProcessMouseEvent - Process mouse event from network (QUEUED)
 * 
 * Events are queued to a background thread to prevent UI freeze
 * when clicking on window title bars or system menus.
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
    INPUT_EVENT evt;
    
    evt.type = INPUT_EVT_MOUSE;
    evt.mouse.x = x;
    evt.mouse.y = y;
    evt.mouse.buttons = buttons;
    evt.mouse.flags = flags;
    evt.mouse.wheelDelta = wheelDelta;
    
    return QueueInputEvent(&evt);
}

/*
 * Input_ProcessKeyEvent - Process keyboard event from network (QUEUED)
 * 
 * Events are queued to a background thread.
 * 
 * Protocol flags:
 *   0x01 = Key down
 *   0x02 = Key up
 *   0x04 = Extended key
 */
BOOL Input_ProcessKeyEvent(WORD vk, WORD scan, BYTE flags)
{
    INPUT_EVENT evt;
    
    evt.type = INPUT_EVT_KEY;
    evt.key.vk = vk;
    evt.key.scan = scan;
    evt.key.flags = flags;
    
    return QueueInputEvent(&evt);
}
