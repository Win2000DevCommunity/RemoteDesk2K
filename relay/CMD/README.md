# RemoteDesk2K Relay Server - Command Line Edition

Professional console-based relay server for Windows 2000 and later.

## Overview

This is a pure command-line version of the RemoteDesk2K relay server. It provides the same functionality as the GUI version but runs entirely in the console, making it ideal for:

- **Server deployments** (Windows Server, headless systems)
- **Scripting and automation**
- **Running as a Windows Service**
- **Docker/container environments**
- **SSH/remote administration**

## Building

```cmd
cd relay\CMD
build.bat
```

To clean build artifacts:
```cmd
build.bat clean
```

## Usage

```
relay_cmd.exe [options]

Options:
  -p, --port PORT     Listen port (default: 5000)
  -i, --ip IP         Bind IP address (default: 0.0.0.0)
  -h, --help          Show help message
  -v, --version       Show version
  -q, --quiet         Suppress startup banner

Examples:
  relay_cmd.exe                      # Start on default port 5000
  relay_cmd.exe -p 5900              # Start on port 5900
  relay_cmd.exe -i 192.168.1.10      # Bind to specific IP
  relay_cmd.exe -p 5000 -i 0.0.0.0   # Full configuration
```

## Output

The server provides color-coded log output:

- **[INFO]** - General information (white)
- **[REGISTER]** - Client registrations (green)
- **[CONNECT]** - Successful connections (green)
- **[DISCONNECT]** - Disconnections (cyan)
- **[WARN]** - Warnings (yellow)
- **[ERROR]** - Errors (red)

Example output:
```
[20:45:12] [INFO]        Starting server on 0.0.0.0:5000...
[20:45:12] [OK]          Relay server is running on port 5000
[20:45:12] [INFO]        Press Ctrl+C to stop the server

  Server Status: RUNNING
  Listening on:  0.0.0.0:5000

  --- Activity Log ---

[20:45:30] [INFO]        New client connection accepted
[20:45:30] [INFO]        Worker thread created for new client
[20:45:30] [REGISTER]    Client ID: 192 168 001 100 registered
[20:45:45] [CONNECT]     Client 192 168 001 100 <-> Partner 192 168 001 101: PAIRED
```

## Stopping the Server

Press **Ctrl+C** to stop the server gracefully. The server will:
1. Stop accepting new connections
2. Notify connected clients
3. Clean up all resources
4. Exit cleanly

## Running as a Windows Service

To run as a service, use a service wrapper like NSSM:

```cmd
nssm install RemoteDesk2KRelay "C:\path\to\relay_cmd.exe" "-p 5000"
nssm start RemoteDesk2KRelay
```

## Firewall Configuration

Make sure to allow the relay port through Windows Firewall:

```cmd
netsh firewall add portopening TCP 5000 "RemoteDesk2K Relay"
```

Or on Windows Vista+:
```cmd
netsh advfirewall firewall add rule name="RemoteDesk2K Relay" dir=in action=allow protocol=TCP localport=5000
```

## Compatibility

- Windows 2000 (SP4)
- Windows XP
- Windows Server 2003
- Windows Vista, 7, 8, 10, 11
- Windows Server 2008, 2012, 2016, 2019, 2022

## License

(C) 2026 RemoteDesk2K Project
