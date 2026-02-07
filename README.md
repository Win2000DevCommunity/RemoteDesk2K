# RemoteDesk2K - Remote Desktop for Windows 2000

A remote desktop application with UltraViewer/AnyDesk-like interface, designed for Windows 2000.

## Features

### 🖥️ UltraViewer-Style Interface
- **Left Panel**: "Allow Remote Control" - Shows your ID and Password
- **Right Panel**: "Control a Remote Computer" - Enter partner's ID to connect
- Clean, intuitive UI similar to UltraViewer

### 📁 File Transfer
- Send files to remote computer via menu
- Files are saved to Desktop automatically
- Transfer progress indication

### 📋 Clipboard Sharing
- Sync clipboard between computers
- Copy text on one PC, paste on the other
- Use Tools menu or Ctrl+Shift+C

### 🖼️ Display Options
- **Full Screen** (F11) - Borderless full screen mode
- **Stretch to Fit** - Scales remote desktop to window size
- **Actual Size** - 100% zoom, scroll if needed
- **Refresh Screen** (F5) - Force full screen refresh

### 🔒 Security
- Password-protected connections
- Auto-generated 5-digit password
- Custom password option
- Password refresh button


## Build Environment

- Windows 2000 DDK (Driver Development Kit)
- Windows 2000 SDK (Software Development Kit)
- Microsoft Visual C++ (cl.exe) from DDK
- Standard C (C89/C90) language

> **Note:** The build uses `build.bat` (Windows batch) and does not require a Makefile. All code is written in portable C for Windows 2000.
## Building

### Requirements
- Windows 2000 SDK
- Microsoft Visual C++ compiler (cl.exe)

### Build Steps
```batch
cd RemoteDesk2K
build.bat
```

## Usage

### As Host (Being Controlled)
1. Run `RemoteDesk2K.exe`
2. Note your **ID** and **Password** shown on left panel
3. Share these with your partner
4. Wait for connection

### As Viewer (Controlling)
1. Run `RemoteDesk2K.exe`
2. Enter partner's **IP address** in "Partner ID" field
3. Enter partner's **Password**
4. Click "Connect to partner"
5. Use the viewer window to control remote PC

### Viewer Controls
| Key/Action | Function |
|------------|----------|
| F11 | Toggle full screen |
| F5 | Refresh screen |
| View Menu | Display scaling options |
| Tools > Send File | Send file to remote |
| Tools > Sync Clipboard | Send clipboard to remote |

## Network Requirements

- Both computers must be on same network (or port forwarded)
- Default port: **5901**
- Firewall must allow incoming TCP on port 5901

## File Structure

```
RemoteDesk2K/
├── common.h         - Protocol definitions
├── screen.h/c       - Screen capture module
├── network.h/c      - Network communication
├── remotedesk2k.c   - Main application (unified UI)
├── Makefile         - Build configuration
├── build.bat        - Build script
└── README.md        - This file
```

## Protocol Overview

### Message Types
- `MSG_HANDSHAKE` - Initial connection with password
- `MSG_SCREEN_UPDATE` - Screen region update (RLE compressed)
- `MSG_MOUSE_EVENT` - Mouse input
- `MSG_KEYBOARD_EVENT` - Keyboard input
- `MSG_CLIPBOARD_TEXT` - Clipboard sync
- `MSG_FILE_*` - File transfer messages

## Comparison with AnyDesk/UltraViewer

| Feature | RemoteDesk2K | AnyDesk | UltraViewer |
|---------|--------------|---------|-------------|
| Combined UI | ✓ | ✓ | ✓ |
| ID/Password | ✓ | ✓ | ✓ |
| File Transfer | ✓ | ✓ | ✓ |
| Clipboard | ✓ | ✓ | ✓ |
| Full Screen | ✓ | ✓ | ✓ |
| Encryption | ✗ | ✓ | ✓ |
| Relay Server | ✗ | ✓ | ✓ |
| Win2000 Support | ✓ | ✗ | ✗ |

## Limitations

- Direct connection only (no relay server)
- Single connection at a time
- No encryption (use VPN for security)
- No audio streaming
- 24-bit color depth

## Version History

- **1.0.0** - Initial release
  - UltraViewer-like unified interface
  - ID and password authentication
  - File transfer support
  - Clipboard sharing
  - Full screen and scaling modes
