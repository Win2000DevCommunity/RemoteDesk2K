/*
 * RemoteDesk2K - Input Module Header
 * Windows 2000 Compatible Input Handling
 */

#ifndef _RD2K_INPUT_H_
#define _RD2K_INPUT_H_

#include <windows.h>

/* Input Event Types */
#define RD2K_INPUT_MOUSE_MOVE       0x01
#define RD2K_INPUT_MOUSE_LDOWN      0x02
#define RD2K_INPUT_MOUSE_LUP        0x04
#define RD2K_INPUT_MOUSE_RDOWN      0x08
#define RD2K_INPUT_MOUSE_RUP        0x10
#define RD2K_INPUT_MOUSE_MDOWN      0x20
#define RD2K_INPUT_MOUSE_MUP        0x40
#define RD2K_INPUT_MOUSE_WHEEL      0x80

/* Keyboard flags */
#define RD2K_INPUT_KEY_DOWN         0x01
#define RD2K_INPUT_KEY_UP           0x02
#define RD2K_INPUT_KEY_EXTENDED     0x04

/*
 * Release all held modifier keys (Ctrl, Shift, Alt, Win)
 * Call this before clipboard operations, file transfers,
 * showing dialogs, or when losing focus.
 * 
 * This prevents the "stuck key" problem where Explorer thinks
 * Ctrl is still held (causing multi-select behavior).
 */
void Input_ReleaseAllModifiers(void);

/*
 * Sync our modifier tracking with actual keyboard state
 * Call this when viewer window regains focus
 */
void Input_SyncModifierState(void);

/*
 * Check if any modifier key is currently held
 */
BOOL Input_IsModifierHeld(void);

/*
 * Simulate mouse movement to absolute screen position
 * x, y: Screen coordinates (0 to screen width/height)
 */
void Input_MouseMove(int x, int y);

/*
 * Simulate mouse button press
 * button: 1=left, 2=right, 3=middle
 * down: TRUE for press, FALSE for release
 */
void Input_MouseButton(int button, BOOL down);

/*
 * Simulate mouse wheel scroll
 * delta: Positive = scroll up, Negative = scroll down
 */
void Input_MouseWheel(int delta);

/*
 * Simulate keyboard key press
 * vk: Virtual key code
 * scan: Hardware scan code
 * down: TRUE for press, FALSE for release
 * extended: TRUE if extended key (arrows, numpad, etc.)
 */
void Input_KeyPress(BYTE vk, BYTE scan, BOOL down, BOOL extended);

/*
 * Process mouse event from network packet
 * Returns TRUE on success
 */
BOOL Input_ProcessMouseEvent(WORD x, WORD y, BYTE buttons, BYTE flags, SHORT wheelDelta);

/*
 * Process keyboard event from network packet
 * Returns TRUE on success
 */
BOOL Input_ProcessKeyEvent(WORD vk, WORD scan, BYTE flags);

#endif /* _RD2K_INPUT_H_ */
