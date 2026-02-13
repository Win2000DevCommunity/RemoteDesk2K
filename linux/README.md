# RemoteDesk2K Linux Relay Server

A Linux port of the RemoteDesk2K relay server. This allows running the relay server on Linux servers, which can act as a relay point for Windows 2000 RemoteDesk2K clients to connect to each other.

## Features

- **Full Protocol Compatibility**: Works with Windows RemoteDesk2K clients
- **Professional Terminal UI**: Color-coded output with timestamps
- **Daemon Mode**: Run as a background service
- **Signal Handling**: Graceful shutdown on SIGINT/SIGTERM
- **Logging**: File-based logging support
- **Same Crypto**: XOR encryption with S-Box compatible with Windows version

## Building

### Prerequisites

- GCC (gcc)
- POSIX-compatible system (Linux, BSD, macOS)
- pthread library (typically bundled with glibc)

### Compile

```bash
# Release build
make

# Debug build
make debug

# Clean
make clean
```

### Install (Optional)

```bash
# Install to /usr/local/bin (requires root)
sudo make install

# Uninstall
sudo make uninstall
```

## Usage

```
Usage: ./relay_server [OPTIONS]

Options:
  -p, --port PORT      Listen port (default: 5000)
  -b, --bind IP        Bind to specific IP (default: 0.0.0.0)
  -d, --daemon         Run as daemon (background)
  -l, --log FILE       Log output to file
  -n, --no-color       Disable colored output
  -h, --help           Show this help message
  -v, --version        Show version information
```

### Examples

```bash
# Listen on default port (5000), all interfaces
./relay_server

# Listen on port 5900
./relay_server -p 5900

# Bind to specific IP
./relay_server -b 192.168.1.100 -p 5000

# Run as daemon with logging
./relay_server -d -l /var/log/relay.log

# No colors (for log capture or old terminals)
./relay_server -n
```

## How It Works

1. Windows RemoteDesk2K clients connect to the relay server
2. Each client registers with a Server ID (generated from IP:Port)
3. Clients can connect to each other using Server IDs
4. The relay forwards encrypted data between paired clients

## Protocol

The relay uses a custom binary protocol with encrypted payloads:

| Message Type | Description |
|--------------|-------------|
| REGISTER | Client registers with server |
| REGISTER_RESPONSE | Server responds with Server ID |
| CONNECT_REQUEST | Client requests to connect to another client |
| CONNECT_RESPONSE | Server responds with connection status |
| DATA | Encrypted data relayed between clients |
| DISCONNECT | Client disconnects |
| PING | Keep-alive message |

All messages are encrypted with XOR + S-Box transformation, compatible with the Windows implementation.

## Signal Handling

- **SIGINT (Ctrl+C)**: Graceful shutdown
- **SIGTERM**: Graceful shutdown
- **SIGPIPE**: Ignored (broken pipe handled in socket code)

## Running as a Service

### systemd Service

Create `/etc/systemd/system/remotedesk2k-relay.service`:

```ini
[Unit]
Description=RemoteDesk2K Relay Server
After=network.target

[Service]
Type=forking
ExecStart=/usr/local/bin/remotedesk2k-relay -d -l /var/log/remotedesk2k-relay.log -p 5000
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable remotedesk2k-relay
sudo systemctl start remotedesk2k-relay

# Check status
sudo systemctl status remotedesk2k-relay

# View logs
sudo journalctl -u remotedesk2k-relay -f
```

## Firewall

Make sure to allow the relay port in your firewall:

```bash
# iptables
sudo iptables -A INPUT -p tcp --dport 5000 -j ACCEPT

# ufw
sudo ufw allow 5000/tcp

# firewalld
sudo firewall-cmd --permanent --add-port=5000/tcp
sudo firewall-cmd --reload
```

## Files

| File | Description |
|------|-------------|
| relay_main.c | CLI entry point, argument parsing, signal handling |
| relay.c | Core relay server logic, connection management |
| relay.h | Relay server public API |
| crypto.c | Encryption/decryption, Server ID encoding |
| crypto.h | Crypto function declarations |
| common.h | Platform compatibility, type definitions |
| Makefile | Build system |

## Compatibility

- **Linux**: Fully tested
- **macOS**: Should work (untested)
- **BSD**: Should work (untested)
- **Windows**: Use the native Windows version instead

## License

Same license as the parent RemoteDesk2K project.
