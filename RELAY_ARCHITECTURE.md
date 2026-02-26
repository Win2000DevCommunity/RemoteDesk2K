# RemoteDesk2K Relay Architecture

## Overview
This document describes the relay connection architecture used for NAT traversal.
The relay server acts as a middleman to connect clients behind different NATs.

---

## 1. State Machine Diagram

### Client States
```mermaid
stateDiagram-v2
    [*] --> DISCONNECTED
    DISCONNECTED --> CONNECTING_RELAY : ConnectToRelay()
    CONNECTING_RELAY --> REGISTERED : Registration OK
    CONNECTING_RELAY --> DISCONNECTED : Registration Failed
    
    REGISTERED --> PAIRING : Partner connects OR we request partner
    REGISTERED --> DISCONNECTED : Server lost / User disconnect
    
    PAIRING --> PAIRED : Handshake success
    PAIRING --> REGISTERED : Partner left during handshake
    PAIRING --> DISCONNECTED : Server lost
    
    PAIRED --> REGISTERED : UNPAIR (session end, stay connected)
    PAIRED --> DISCONNECTED : DISCONNECT (full cleanup)
```

### Server States (per connection)
```mermaid
stateDiagram-v2
    [*] --> CONNECTED : Accept connection
    CONNECTED --> REGISTERED : RELAY_MSG_REGISTER
    CONNECTED --> DISCONNECTED : Invalid registration
    
    REGISTERED --> PAIRED : RELAY_MSG_CONNECT_REQUEST success
    REGISTERED --> DISCONNECTED : Timeout / Error
    
    PAIRED --> REGISTERED : RELAY_MSG_UNPAIR (end session)
    PAIRED --> DISCONNECTED : RELAY_MSG_DISCONNECT / Error
```

---

## 2. Sequence Diagram: Connection Flow

```mermaid
sequenceDiagram
    participant Viewer as Client A (Viewer)
    participant Relay as Relay Server
    participant Server as Client B (Server/Host)
    
    Note over Viewer,Server: Phase 1: Registration
    
    Viewer->>Relay: Connect TCP
    Relay-->>Viewer: Accept
    Viewer->>Relay: RELAY_MSG_REGISTER (ID=033)
    Relay-->>Viewer: RELAY_MSG_REGISTER_RESPONSE (OK)
    Note right of Viewer: State: REGISTERED
    
    Server->>Relay: Connect TCP
    Relay-->>Server: Accept
    Server->>Relay: RELAY_MSG_REGISTER (ID=193)
    Relay-->>Server: RELAY_MSG_REGISTER_RESPONSE (OK)
    Note right of Server: State: REGISTERED
    
    Note over Viewer,Server: Phase 2: Pairing
    
    Viewer->>Relay: RELAY_MSG_CONNECT_REQUEST (partner=193)
    Relay->>Server: RELAY_MSG_PARTNER_CONNECTED (partner=033)
    Relay-->>Viewer: RELAY_MSG_CONNECT_RESPONSE (OK)
    Note right of Viewer: State: PAIRED
    Note right of Server: State: PAIRED
    
    Note over Viewer,Server: Phase 3: Data Transfer
    
    loop Bidirectional Data
        Viewer->>Relay: RELAY_MSG_DATA (screen data)
        Relay->>Server: RELAY_MSG_DATA (forwarded)
        Server->>Relay: RELAY_MSG_DATA (input events)
        Relay->>Viewer: RELAY_MSG_DATA (forwarded)
    end
    
    loop Keepalive (every 10s)
        Relay->>Viewer: RELAY_MSG_PING
        Relay->>Server: RELAY_MSG_PING
    end
    
    Note over Viewer,Server: Phase 4: Unpair (viewer closes session)
    
    Viewer->>Relay: RELAY_MSG_UNPAIR
    Relay->>Server: RELAY_MSG_PARTNER_DISCONNECTED
    Note right of Viewer: State: REGISTERED
    Note right of Server: State: REGISTERED
    
    Note over Viewer,Server: Both can reconnect without re-registering!
```

---

## 3. Class Diagram

```mermaid
classDiagram
    class RelayServer {
        -SOCKET listenSocket
        -CRITICAL_SECTION csConnections
        -PRELAY_CONNECTION[] connections
        -DWORD maxConnections
        -DWORD activeConnections
        -BOOL bRunning
        +Relay_Start(port, maxConn)
        +Relay_Stop()
        +ProcessRelayMessage(pConn, buffer, length)
        -ClientWorkerThread(pConn)
    }
    
    class RelayConnection {
        -SOCKET socket
        -DWORD clientId
        -RELAY_STATE state
        -PRELAY_CONNECTION pPartner
        -DWORD lastActivity
        -HANDLE hThread
        -HANDLE hDisconnectEvent
        -BYTE* recvBuffer
    }
    
    class RelayClient {
        +Relay_ConnectToServer(addr, port, id) SOCKET
        +Relay_Register(socket, id) int
        +Relay_RequestPartner(socket, partnerId, pwd) int
        +Relay_SendData(socket, data, len) int
        +Relay_SendUnpair(socket) int
        +Relay_SendDisconnect(socket) int
        +Relay_CheckConnection(socket) int
    }
    
    class SessionManager {
        -SESSION_STATE state
        -SOCKET relaySocket
        -DWORD myId
        -DWORD partnerId
        -BOOL bPaired
        -SESSION_ROLE role
        +Session_Initialize()
        +Session_OnRelayConnected(socket)
        +Session_OnRelayRegistered(id)
        +Session_OnPartnerPaired(isServer)
        +Session_OnPartnerLeft(reason)
        +Session_OnServerLost()
        +Session_IsConnected() BOOL
        +Session_IsPaired() BOOL
    }
    
    RelayServer "1" --> "*" RelayConnection : manages
    RelayConnection "0..1" --> "0..1" RelayConnection : pPartner
    RelayClient ..> RelayServer : connects to
    SessionManager --> RelayClient : uses
```

---

## 4. Message Protocol

### Relay Header (8 bytes)
```
+----------+-------+----------+------------+
| msgType  | flags | reserved | dataLength |
| (1 byte) |(1 byte)| (2 bytes)| (4 bytes)  |
+----------+-------+----------+------------+
```

### Message Types
| Code | Name | Direction | Description |
|------|------|-----------|-------------|
| 0x50 | REGISTER | C→S | Register client ID with relay |
| 0x51 | CONNECT_REQUEST | C→S | Request pairing with partner |
| 0x52 | CONNECT_RESPONSE | S→C | Pairing result |
| 0x53 | DATA | C↔C | Tunneled application data |
| 0x54 | DISCONNECT | C→S | Full disconnect (close socket) |
| 0x55 | PING | S→C / C→S | Keepalive |
| 0x56 | PONG | S→C / C→S | Keepalive response |
| 0x57 | PARTNER_DISCONNECTED | S→C | Partner left notification |
| 0x58 | REGISTER_RESPONSE | S→C | Registration result |
| 0x59 | PARTNER_CONNECTED | S→C | Partner connected notification |
| **0x5A** | **UNPAIR** | C→S | **End session, stay REGISTERED** |

---

## 5. Key Design Decisions

### 5.1 UNPAIR vs DISCONNECT
- **DISCONNECT (0x54)**: Completely leave relay server (socket closes)
- **UNPAIR (0x5A)**: End current pairing but stay REGISTERED

When viewer closes, we send UNPAIR so both clients return to REGISTERED
and can immediately reconnect without re-registering.

### 5.2 Keepalive Strategy
1. **Server-initiated**: Server sends PING every 10 seconds
2. **Dead detection**: If send() fails, socket is dead → cleanup
3. **Timeout**: 30 seconds for REGISTERED, 5 minutes hard limit for PAIRED

### 5.3 State Transitions
All state changes go through SessionManager to ensure:
- Thread-safe state updates
- Timers started/stopped appropriately
- Callbacks notify UI
- No duplicate states

---

## 6. Error Handling

| Error | Detection | Action |
|-------|-----------|--------|
| Network drop | send() fails | Cleanup, notify partner |
| Client crash | Keepalive timeout | Server detects, unpairs |
| Server crash | recv() returns 0 | Client goes DISCONNECTED |
| Partner left | PARTNER_DISCONNECTED msg | Return to REGISTERED |
| Duplicate ID | REGISTER_RESPONSE error | Reject new connection |

---

## 7. Files

| File | Role |
|------|------|
| `relay.c` | Server implementation (Windows/Linux) |
| `relay_client.c` | Client-side relay API |
| `session_manager.c` | Client state machine |
| `common/relay.h` | Protocol definitions |
| `network.c` | Low-level socket operations |

---

*Last updated: February 2026*
