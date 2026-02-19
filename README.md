# RemoteDesk2K - Remote Desktop for Windows 2000

A remote desktop application with UltraViewer/AnyDesk-like interface, designed for Windows 2000.

## Downloads

| File | Description | Size |
|------|-------------|------|
| **RD2K_Setup.exe** | Client installer (includes RemoteDesk2K.exe) | ~130 KB |
| **relay.exe** | Relay server (separate, for admins only) | ~33 KB |

> ⚠️ **Important:** The relay server (`relay.exe`) is **NOT included** in the installer.  
> It is distributed separately for server administrators only.

### No Cloud Server Yet

Currently, there is **no public cloud-hosted relay server**. To use relay mode:

1. **You or someone in your network** must run `relay.exe` on a server/PC with a public IP or port forwarding
2. Start the relay server and copy the generated **Server ID**
3. Distribute the Server ID to client users
4. Clients can then connect through the relay

> 💡 **Direct Connection** still works without any relay server if both PCs are on the same network!

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
- **Encrypted connections** - All traffic encrypted with multi-layer cipher
- Password-protected connections
- Auto-generated 5-digit password
- Custom password option
- Password refresh button

### 🌐 Relay Server Support
- Connect through NAT/firewalls via relay server
- Auto-reconnection on connection loss (5 attempts)
- Partner disconnect detection with notification
- Works alongside direct connections

### 🔐 Server ID System (NEW)
- **Encrypted Server IDs** - Relay IP:port encoded as `XXXX-XXXX-XXXX` format
- **Privacy Protection** - Client users never see the real server IP address
- **Admin-Only Generation** - Only relay server admin can generate Server IDs
- **One-Click Copy** - Copy Server ID to clipboard for easy distribution
- **Simple Client Experience** - Users just enter the Server ID to connect
- **Domain:Port Support** - Clients can also connect using `domain.com:port` format

### 🛡️ Stability & Protection
- **Single Instance** - Only one instance of each app can run at a time
- **Duplicate ID Detection** - Warning when your ID is already connected on the server
- **Graceful Shutdown** - Clean resource cleanup when closing apps
- **Memory Safety** - Proper initialization prevents display artifacts


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

# Build client application
cd client
build.bat              # Creates RemoteDesk2K.exe

# Build relay server (admin only)
cd ../relay
build_relay.bat        # Creates relay.exe (GUI) and relay_cmd.exe (CLI)

# Build installer (optional)
cd ../installer
build_installer.bat    # Creates RD2K_Setup.exe (embeds client)
```

### Linux Relay Build
```bash
cd RemoteDesk2K/linux
make                   # Creates relay (Linux binary)
./relay 5000           # Start relay on port 5000
```

## Project Structure

```
RemoteDesk2K/
├── client/              # Client application
│   ├── remotedesk2k.c   # Main client code
│   ├── clipboard.c/h    # Clipboard sharing
│   ├── filetransfer.c/h # File transfer
│   ├── input.c/h        # Input handling
│   ├── progress.c/h     # Progress dialogs
│   └── build.bat        # Client build script
├── relay/               # Relay server (Windows)
│   ├── relay.c          # Relay server logic
│   ├── relay_gui.c      # Relay server GUI (relay.exe)
│   ├── CMD/             # Command-line relay (relay_cmd.exe)
│   │   └── relay_cmd.c
│   └── build_relay.bat  # Relay build script
├── linux/               # Relay server (Linux)
│   ├── relay.c          # Linux relay logic
│   ├── relay_main.c     # Linux main entry point
│   ├── common.h         # Linux-specific definitions
│   └── Makefile         # Build with 'make'
├── common/              # Shared code
│   ├── common.h         # Protocol definitions
│   ├── network.c/h      # Network communication
│   ├── screen.c/h       # Screen capture
│   ├── crypto.c/h       # Encryption
│   └── relay.h          # Relay protocol
├── installer/           # Installer builder
│   ├── installer.c      # Installer code
│   └── build_installer.bat
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
2. Run `./relay <port>` (e.g., `./relay 5000`)
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
| Encryption | ✓ | ✓ | ✓ |
| Relay Server | ✓ | ✓ | ✓ |
| Win2000 Support | ✓ | ~* | ✗ |

*AnyDesk has unofficial Win2000 support (older versions)

## Limitations

- Single connection at a time
- No audio streaming
- 24-bit color depth

## Version History

- **1.3.0** - Stability & Protection Update
  - **Single instance protection** - Only one instance of each app can run at a time
  - **Duplicate ID detection** - Clear warning when trying to connect with an already-connected ID
  - **Graceful shutdown** - Clean resource cleanup when closing applications
  - **Fixed display artifacts** - No more random colored squares appearing in viewer
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

## ⚠️ WSL/Windows Port Binding Warning

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
