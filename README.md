# RemoteDesk2K - Remote Desktop for Windows 2000

A remote desktop application with UltraViewer/AnyDesk-like interface, designed for Windows 2000.  
See and control remote desktops — including the **Winlogon (login) screen** — over LAN or Internet.

## Downloads

| File | Description | Size |
|------|-------------|------|
| **RD2K_Setup.exe** | Client installer (includes RemoteDesk2K.exe) | ~130 KB |
| **relay.exe** | Relay server - GUI (separate, for admins only) | ~33 KB |
| **relay_cmd.exe** | Relay server - CLI (separate, for admins only) | ~25 KB |

> **Important:** The relay server (`relay.exe` / `relay_cmd.exe`) is **NOT included** in the installer.  
> It is distributed separately for server administrators only.

### No Cloud Server Yet

Currently, there is **no public cloud-hosted relay server**. To use relay mode:

1. **You or someone in your network** must run `relay.exe` on a server/PC with a public IP or port forwarding
2. Start the relay server and copy the generated **Server ID**
3. Distribute the Server ID to client users
4. Clients can then connect through the relay

> **Direct Connection** still works without any relay server if both PCs are on the same network.

## Features

### UltraViewer-Style Interface
- **Left Panel**: "Allow Remote Control" — Shows your ID and Password
- **Right Panel**: "Control a Remote Computer" — Enter partner's ID to connect
- Clean, intuitive UI similar to UltraViewer

### Winlogon / Login Screen Remote Control
- **See and interact with the Windows login screen** (Ctrl+Alt+Del, password entry)
- Automatic desktop switching between user desktop, Winlogon, and secure desktops
- SYSTEM token impersonation for full Winlogon access
- Worker-thread capture for reliable screen grabbing across all desktop states
- Works in both debug and release builds

### DirectDraw GPU-Accelerated Screen Capture
- **Layer 0**: Direct framebuffer access via kernel IOCTL (hardware-dependent)
- **Layer 1**: DirectDraw surface locking — GPU-accelerated, works on Windows 2000 SP1+
- **Automatic GDI fallback** if DirectDraw is unavailable
- Captures Winlogon desktop where GDI `BitBlt` cannot

### File Transfer
- Send files to remote computer via menu
- Files saved to Desktop automatically
- Transfer progress indication

### Clipboard Sharing
- Sync clipboard between computers
- Copy text on one PC, paste on the other
- Use Tools menu or Ctrl+Shift+C

### Display Options
- **Full Screen** (F11) — Borderless full screen mode
- **Stretch to Fit** — Scales remote desktop to window size
- **Actual Size** — 100% zoom, scroll if needed
- **Refresh Screen** (F5) — Force full screen refresh

### Security
- **Encrypted connections** — All traffic encrypted with multi-layer cipher
- Password-protected connections
- Auto-generated 5-digit password
- Custom password option
- Password refresh button

### Relay Server Support
- Connect through NAT/firewalls via relay server
- Auto-reconnection on connection loss (5 attempts)
- Partner disconnect detection with notification
- Works alongside direct connections
- **Three relay flavors**: Windows GUI, Windows CLI, and Linux

### Server ID System
- **Encrypted Server IDs** — Relay IP:port encoded as `XXXX-XXXX-XXXX` format
- **Privacy Protection** — Client users never see the real server IP address
- **Admin-Only Generation** — Only relay server admin can generate Server IDs
- **One-Click Copy** — Copy Server ID to clipboard for easy distribution
- **Simple Client Experience** — Users just enter the Server ID to connect
- **Domain:Port Support** — Clients can also connect using `domain.com:port` format

### Stability & Protection
- **Single Instance** — Only one instance of each app can run at a time
- **Duplicate ID Detection** — Warning when your ID is already connected on the server
- **Graceful Shutdown** — Clean resource cleanup when closing apps
- **Memory Safety** — Proper initialization prevents display artifacts
- **Session Manager** — Centralized connection state machine for reliable reconnection


## Build Environment

- **Client**: Windows 2000 DDK compiler (`3790.1830\bin\x86\cl.exe`) — produces Win2000-compatible binary
- **Relay (Windows)**: MSVC `cl.exe` from system PATH
- **Relay (Linux)**: GCC with `-std=c99`
- **Language**: Standard C (C89/C90 for client, C99 for Linux relay)

> **Note:** The build uses `.bat` scripts (no Makefile needed on Windows). All code is written in portable C.

## Building

### Requirements
- Windows 2000 DDK (for client — provides cl.exe targeting Win2000)
- Windows 2000 SDK (headers and libs)
- MSVC cl.exe on PATH (for relay builds)

### Build Steps
```batch
cd RemoteDesk2K

REM Build client application (uses DDK compiler)
cd client
build.bat              REM Creates RemoteDesk2K.exe (release)
build.bat debug        REM Creates RemoteDesk2K.exe (debug with logging)

REM Build relay server - GUI (uses MSVC)
cd ..\relay
build_relay.bat        REM Creates relay.exe

REM Build relay server - CLI (uses MSVC)
cd CMD
build.bat              REM Creates relay_cmd.exe

REM Build installer (optional, embeds client)
cd ..\..\installer
build_installer.bat    REM Creates RD2K_Setup.exe
```

### Linux Relay Build
```bash
cd RemoteDesk2K/linux
make                   # Creates relay_server
./relay_server 5000    # Start relay on port 5000
```

## Project Structure

```
RemoteDesk2K/
├── client/                      # Client application
│   ├── remotedesk2k.c           # Main client (GUI + event loop)
│   ├── session_manager.c/h      # Connection state machine
│   ├── relay_client.c/h         # Relay client protocol
│   ├── screen_capture_ddraw.c/h # DirectDraw GPU screen capture
│   ├── clipboard.c/h            # Clipboard sharing
│   ├── filetransfer.c/h         # File transfer
│   ├── input.c/h                # Mouse/keyboard input
│   ├── server_config_tab.c/h    # Server config UI tab
│   ├── progress.c/h             # Progress dialogs
│   ├── nogs.c                   # Security cookie stub
│   └── build.bat                # Client build script (DDK)
├── common/                      # Shared code (client + relay)
│   ├── common.h                 # Protocol definitions & messages
│   ├── network.c/h              # Network I/O (send/recv helpers)
│   ├── screen.c/h               # Screen capture (GDI + Winlogon worker)
│   ├── desktop.c/h              # Desktop detection & Winlogon switching
│   ├── crypto.c/h               # Multi-layer encryption
│   ├── relay.h                  # Relay protocol definitions
│   ├── screen_capture_ddraw.h   # DirectDraw shared header
│   └── screen_capture_framebuffer.c  # Kernel framebuffer capture (Layer 0)
├── relay/                       # Relay server (Windows GUI)
│   ├── relay.c                  # Relay server logic
│   ├── relay_gui.c/h            # GUI (server ID, config, log)
│   ├── relay_gui_main.c         # WinMain entry point
│   ├── security_cookie_stub.c   # MSVC security stub
│   ├── CMD/                     # Command-line relay
│   │   ├── relay_cmd.c          # CLI relay entry point
│   │   └── build.bat            # CLI relay build script
│   └── build_relay.bat          # GUI relay build script
├── linux/                       # Relay server (Linux)
│   ├── relay.c                  # Linux relay logic
│   ├── relay_main.c             # Linux main entry point
│   ├── common.h                 # Linux protocol definitions
│   ├── relay.h                  # Linux relay header
│   ├── crypto.c/h               # Linux crypto
│   └── Makefile                 # Build with 'make'
├── installer/                   # Installer builder
│   ├── installer.c              # Self-extracting installer
│   ├── installer.rc             # Resource script
│   └── build_installer.bat      # Installer build script
└── README.md
```

## Usage

### As Host (Being Controlled)
1. Run `RemoteDesk2K.exe`
2. Note your **ID** and **Password** shown on left panel
3. Share these with your partner
4. Wait for connection

### As Viewer (Controlling)
1. Run `RemoteDesk2K.exe`
2. Enter either:
   - **Server ID** (e.g., `A7K2-M9PL-X3QR`) provided by admin, or
   - **Domain:Port** (e.g., `relay.example.com:5000`) directly
3. Enter partner's **Password**
4. Click "Connect to partner"
5. Use the viewer window to control remote PC

### Relay Server Admin (Windows)
1. Run `relay.exe` (GUI) or `relay_cmd.exe` (CLI)
2. Configure relay IP and port
3. Click **Start Server** (or just start for CLI)
4. **Server ID** is auto-generated and displayed
5. Click **Copy** to copy Server ID
6. Distribute Server ID to your client users

### Relay Server Admin (Linux)
1. Build with `make` in the `linux/` folder
2. Run `./relay_server <port>` (e.g., `./relay_server 5000`)
3. Copy the generated **Server ID** from console output
4. Distribute Server ID to your client users

> **Note:** Clients only see the Server ID, never the real IP:port. This protects your server infrastructure.

### Viewer Controls
| Key/Action | Function |
|------------|----------|
| F11 | Toggle full screen |
| F5 | Refresh screen |
| View Menu | Display scaling options |
| Tools > Send File | Send file to remote |
| Tools > Sync Clipboard | Send clipboard to remote |

## Network Requirements

### Direct Connection (Same Network)
- Both computers on same LAN, or port forwarding configured
- Default port: **5901**
- Firewall must allow incoming TCP on port 5901

### Relay Connection (Through Internet)
- No port forwarding needed on client side
- Relay server must have public IP or port forwarding
- Default relay port: **5000**
- Clients connect to relay using **Server ID** (no IP needed)

## Protocol Overview

### Message Types
- `MSG_HANDSHAKE` — Initial connection with password
- `MSG_SCREEN_UPDATE` — Screen region update (RLE compressed)
- `MSG_MOUSE_EVENT` — Mouse input
- `MSG_KEYBOARD_EVENT` — Keyboard input
- `MSG_CLIPBOARD_TEXT` — Clipboard sync
- `MSG_FILE_*` — File transfer messages

## Comparison with AnyDesk/UltraViewer

| Feature | RemoteDesk2K | AnyDesk | UltraViewer |
|---------|--------------|---------|-------------|
| Combined UI | Yes | Yes | Yes |
| ID/Password | Yes | Yes | Yes |
| File Transfer | Yes | Yes | Yes |
| Clipboard | Yes | Yes | Yes |
| Full Screen | Yes | Yes | Yes |
| Encryption | Yes | Yes | Yes |
| Relay Server | Yes | Yes | Yes |
| Winlogon Access | Yes | Yes | No |
| DirectDraw Capture | Yes | N/A | N/A |
| Cross-Platform Relay | Yes | Yes | No |
| Win2000 Support | Yes | Partial* | No |
| Open Source | Yes | No | No |

*AnyDesk dropped Win2000 support in newer versions.

## Limitations

- Single connection at a time
- No audio streaming
- 24-bit color depth

## Version History

- **6.0** - Winlogon Remote Control & DirectDraw Capture
  - **Winlogon (login screen) remote control** — see and interact with the login screen remotely
  - **Desktop switching** — automatic detection and switching between user, Winlogon, and secure desktops
  - **SYSTEM token impersonation** — duplicates winlogon.exe token for full desktop access
  - **Worker-thread screen capture** — reliable capture across all desktop states
  - **DirectDraw GPU-accelerated screen capture** — faster than GDI BitBlt, works on Winlogon
  - **Direct framebuffer access** (Layer 0) — kernel IOCTL for hardware-level capture (experimental)
  - **Session manager** — centralized connection state machine with proper state transitions
  - **Relay server fixes** — backported all GUI relay fixes to CMD and Linux versions
  - **Linux relay**: fixed stale connection replacement, session/partner cleanup, memory leaks
  - **CMD relay**: added crypto initialization and cleanup
  - **Code cleanup** — removed dead files (service, IPC, DXGI), stale docs, build artifacts

- **1.3.0** - Stability & Protection Update
  - **Single instance protection** — only one instance of each app can run at a time
  - **Duplicate ID detection** — clear warning when trying to connect with an already-connected ID
  - **Graceful shutdown** — clean resource cleanup when closing applications
  - **Fixed display artifacts** — no more random colored squares appearing in viewer
  - Improved memory initialization for screen decompression
  - Works on both Windows and Linux relay servers

- **1.2.0** - Server ID Privacy Update
  - Encrypted Server ID system (`XXXX-XXXX-XXXX` format)
  - Client users no longer see real server IP address
  - Relay server auto-generates Server ID on start
  - Copy button for easy Server ID distribution
  - Enhanced privacy for server administrators

- **1.1.0** - Security and Relay Update
  - Full encryption on all connections (direct and relay)
  - Relay server support for NAT traversal
  - Auto-reconnection on connection loss
  - Partner disconnect notifications
  - Connection health monitoring

- **1.0.0** - Initial release
  - UltraViewer-like unified interface
  - ID and password authentication
  - File transfer support
  - Clipboard sharing
  - Full screen and scaling modes

---

## WSL/Windows Port Binding Warning

If you run the relay server inside **WSL (Windows Subsystem for Linux)** on Windows:

- Any port you bind in WSL (e.g., 80, 8080, 5000, etc.) will also appear as LISTENING in Windows.
- Only one process (either in Windows or WSL) can actually receive connections on a given port at a time.
- If you run a relay server on the same port in both Windows and WSL, you will get conflicts, unpredictable behavior, or connection delays.
- Windows clients may connect to the wrong process, or connections may be delayed/confused if both environments are using the same port.

**Best Practice:**
- Always make sure the port is free in both Windows and WSL before starting your relay server.
- Never run a server on the same port in both Windows and WSL at the same time.
- Use `netstat`/`ss` in both environments to confirm only one listener.
- Prefer using a high port (e.g., 50000) for development/testing in WSL.

**This issue does NOT occur on real, separate Linux machines.**

---
