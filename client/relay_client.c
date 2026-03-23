/*
 * relay_client.c - Client-side Relay Connection Functions
 * 
 * This file contains ONLY the client-side functions for connecting
 * TO an external relay server. The relay server itself (relay.exe)
 * is built separately with relay.c.
 * 
 * Used by: RemoteDesk2K.exe (client)
 * NOT used by: relay.exe (server)
 */

#include "relay_client.h"
#include "relay.h"       /* For RELAY_HEADER, RELAY_MSG_* constants */
#include "crypto.h"

/* Helper: Wait for socket to be ready */
static int WaitForSocketReady(SOCKET sock, BOOL bWrite, int timeoutMs)
{
    fd_set fds;
    struct timeval tv;
    int result;
    
    if (sock == INVALID_SOCKET) return -1;
    
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    
    if (bWrite) {
        result = select(0, NULL, &fds, NULL, &tv);
    } else {
        result = select(0, &fds, NULL, NULL, &tv);
    }
    
    if (result == SOCKET_ERROR) return -1;
    if (result == 0) return 0;
    return 1;
}

/* Helper: Send a relay packet with XOR encryption */
static int SendRelayPacket(SOCKET sock, BYTE msgType, const BYTE *data, DWORD dataLength)
{
    RELAY_HEADER header;
    DWORD packetSize;
    int result;
    /* Reusable send buffer — avoids malloc+free per packet. During screen
     * updates, this is called 9-500+ times per frame (once per dirty rect).
     * Malloc overhead was a major source of lag when browsing Explorer. */
    static BYTE *s_sendBuf = NULL;
    static DWORD s_sendBufSize = 0;
    
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    packetSize = sizeof(RELAY_HEADER) + dataLength;
    if (packetSize > s_sendBufSize) {
        BYTE *pNew = (BYTE*)realloc(s_sendBuf, packetSize);
        if (!pNew) return RD2K_ERR_MEMORY;
        s_sendBuf = pNew;
        s_sendBufSize = packetSize;
    }
    
    header.msgType = msgType;
    header.flags = 0x01;  /* Flag: encrypted */
    header.reserved = 0;
    header.dataLength = dataLength;
    
    CopyMemory(s_sendBuf, &header, sizeof(RELAY_HEADER));
    if (data && dataLength > 0) {
        CopyMemory(s_sendBuf + sizeof(RELAY_HEADER), data, dataLength);
        /* XOR encrypt the data portion */
        Crypto_Encrypt(s_sendBuf + sizeof(RELAY_HEADER), dataLength);
    }
    
    result = send(sock, (const char*)s_sendBuf, packetSize, 0);
    
    if (result == SOCKET_ERROR) {
        return RD2K_ERR_SEND;
    }
    
    return RD2K_SUCCESS;
}

/* ===== Client-side relay API ===== */

/* Configure socket for optimal relay performance */
static void ConfigureRelaySocket(SOCKET sock)
{
    int opt;
    
    /* Disable Nagle's algorithm for lower latency */
    opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    
    /* Increase socket buffers for large transfers */
    opt = 512 * 1024;  /* 512KB */
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&opt, sizeof(opt));
    
    /* Set recv timeout to prevent indefinite blocking.
     * Without this, if select says readable but recv blocks (TCP edge case),
     * the main thread hangs forever. 2 seconds is long enough for legitimate
     * data but short enough to not freeze the app or trigger relay UNPAIR. */
    opt = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&opt, sizeof(opt));
    
    /* Set send timeout to prevent send() from blocking forever
     * when the relay connection becomes congested or drops. */
    opt = 5000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&opt, sizeof(opt));
    
    /* Enable keep-alive */
    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&opt, sizeof(opt));
}

int Relay_ConnectToServer(const char *relayServerAddr, WORD relayPort,
                         DWORD clientId, SOCKET *pRelaySocket)
{
    struct sockaddr_in addr;
    struct hostent *pHost;
    unsigned long ipAddr;
    SOCKET sock;
    unsigned long nonBlocking;
    int connectResult;
    int waitResult;
    int sockError;
    int sockErrorLen;
    
    if (!relayServerAddr || !pRelaySocket) return RD2K_ERR_SOCKET;
    
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(relayPort);
    
    /* Resolve hostname/IP */
    ipAddr = inet_addr(relayServerAddr);
    if (ipAddr != INADDR_NONE) {
        addr.sin_addr.s_addr = ipAddr;
    } else {
        pHost = gethostbyname(relayServerAddr);
        if (!pHost) {
            closesocket(sock);
            return RD2K_ERR_CONNECT;
        }
        CopyMemory(&addr.sin_addr, pHost->h_addr, pHost->h_length);
    }
    
    /* Set non-blocking mode for connect with timeout */
    nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    
    /* Connect to relay server (non-blocking) */
    connectResult = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (connectResult == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closesocket(sock);
            return RD2K_ERR_CONNECT;
        }
        
        /* Wait for connection with 5 second timeout */
        waitResult = WaitForSocketReady(sock, TRUE, 5000);
        if (waitResult <= 0) {
            closesocket(sock);
            return RD2K_ERR_TIMEOUT;  /* Connection timeout */
        }
        
        /* Check if connect succeeded */
        sockErrorLen = sizeof(sockError);
        sockError = 0;
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&sockError, &sockErrorLen);
        if (sockError != 0) {
            closesocket(sock);
            return RD2K_ERR_CONNECT;
        }
    }
    
    /* Set back to blocking mode */
    nonBlocking = 0;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    
    /* Configure socket options for performance */
    ConfigureRelaySocket(sock);
    
    *pRelaySocket = sock;
    return RD2K_SUCCESS;
}

int Relay_Register(SOCKET relaySocket, DWORD clientId)
{
    RELAY_REGISTER_MSG msg;
    BYTE buffer[256];
    RELAY_HEADER header;
    RELAY_REGISTER_RESPONSE response;
    int result;
    DWORD recvLen;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    msg.clientId = clientId;
    msg.reserved = 0;
    
    /* Send registration request */
    result = SendRelayPacket(relaySocket, RELAY_MSG_REGISTER, (const BYTE*)&msg, sizeof(msg));
    if (result != RD2K_SUCCESS) {
        return result;
    }
    
    /* Wait for registration response (5 second timeout) */
    result = WaitForSocketReady(relaySocket, FALSE, 5000);
    if (result <= 0) {
        return RD2K_ERR_TIMEOUT;
    }
    
    /* Receive response */
    recvLen = recv(relaySocket, (char*)buffer, sizeof(buffer), 0);
    if (recvLen == 0 || recvLen == SOCKET_ERROR) {
        return RD2K_ERR_RECV;
    }
    
    if (recvLen < sizeof(RELAY_HEADER)) {
        return RD2K_ERR_PROTOCOL;
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    /* Decrypt if encrypted */
    if ((header.flags & 0x01) && header.dataLength > 0) {
        Crypto_Decrypt(buffer + sizeof(RELAY_HEADER), header.dataLength);
    }
    
    if (header.msgType != RELAY_MSG_REGISTER_RESPONSE) {
        return RD2K_ERR_PROTOCOL;
    }
    
    if (recvLen < sizeof(RELAY_HEADER) + sizeof(RELAY_REGISTER_RESPONSE)) {
        return RD2K_ERR_PROTOCOL;
    }
    
    CopyMemory(&response, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_REGISTER_RESPONSE));
    
    /* Check registration result */
    if (response.status == RELAY_REGISTER_DUPLICATE) {
        return RD2K_ERR_DUPLICATE_ID;
    }
    
    if (response.status != RELAY_REGISTER_OK) {
        return RD2K_ERR_CONNECT;
    }
    
    return RD2K_SUCCESS;
}

int Relay_RequestPartner(SOCKET relaySocket, DWORD partnerId, DWORD password)
{
    RELAY_CONNECT_REQUEST msg;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    msg.partnerId = partnerId;
    msg.password = password;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_CONNECT_REQUEST, (const BYTE*)&msg, sizeof(msg));
}

int Relay_WaitForConnection(SOCKET relaySocket, DWORD timeoutMs)
{
    BYTE buffer[512];
    DWORD recvLen;
    RELAY_HEADER header;
    RELAY_CONNECT_RESPONSE response;
    int result;
    DWORD startTime = GetTickCount();
    DWORD elapsed;
    DWORD remainingMs;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    /* Loop until we get CONNECT_RESPONSE or timeout */
    while (1) {
        elapsed = GetTickCount() - startTime;
        if (elapsed >= timeoutMs) {
            return RD2K_ERR_TIMEOUT;
        }
        remainingMs = timeoutMs - elapsed;
        
        /* Wait for data */
        result = WaitForSocketReady(relaySocket, FALSE, remainingMs > 1000 ? 1000 : remainingMs);
        if (result < 0) {
            return RD2K_ERR_RECV;
        }
        if (result == 0) {
            continue;  /* Timeout on this wait, try again */
        }
        
        recvLen = recv(relaySocket, (char*)buffer, sizeof(buffer), 0);
        if (recvLen == 0) {
            return RD2K_ERR_SERVER_LOST;
        }
        if (recvLen == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) continue;
            return RD2K_ERR_RECV;
        }
        
        if (recvLen < (int)sizeof(RELAY_HEADER)) {
            continue;  /* Incomplete packet, try again */
        }
        
        CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
        
        /* Decrypt if encrypted */
        if ((header.flags & 0x01) && header.dataLength > 0 && 
            recvLen >= (int)(sizeof(RELAY_HEADER) + header.dataLength)) {
            Crypto_Decrypt(buffer + sizeof(RELAY_HEADER), header.dataLength);
        }
        
        /* Skip non-CONNECT_RESPONSE messages (like PONG, PARTNER_DISCONNECTED, etc.) */
        if (header.msgType != RELAY_MSG_CONNECT_RESPONSE) {
            /* Just skip this message and continue waiting */
            continue;
        }
        
        /* Got CONNECT_RESPONSE! */
        if (recvLen < (int)(sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_RESPONSE))) {
            return RD2K_ERR_PROTOCOL;
        }
        
        CopyMemory(&response, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_RESPONSE));
        return response.status;
    }
}

/* Send graceful disconnect message to relay server.
 * MUST be called BEFORE closing the socket!
 * This tells the server to immediately unregister our ID
 * and properly terminate any session with our partner. */
int Relay_SendDisconnect(SOCKET relaySocket)
{
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    /* Send DISCONNECT message - server will clean up our connection
     * and notify our partner if we're in a session. */
    return SendRelayPacket(relaySocket, RELAY_MSG_DISCONNECT, NULL, 0);
}

/* End current pairing but stay REGISTERED with relay server.
 * Use this when closing the viewer but wanting to reconnect later.
 * Both this client and partner will return to REGISTERED state. */
int Relay_SendUnpair(SOCKET relaySocket)
{
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    /* Send UNPAIR message - server will end our pairing but keep us registered.
     * Partner will receive PARTNER_DISCONNECTED notification. */
    return SendRelayPacket(relaySocket, RELAY_MSG_UNPAIR, NULL, 0);
}

/* Check if a partner has connected to us (non-blocking).
 * Call this periodically when registered but not yet paired.
 * Returns: partner ID if someone connected, 0 if no partner yet, negative on error */
int Relay_CheckForPartner(SOCKET relaySocket, DWORD *pPartnerId)
{
    BYTE buffer[64];
    RELAY_HEADER header;
    RELAY_PARTNER_CONNECTED partnerConnected;
    int result;
    int recvLen;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!pPartnerId) return RD2K_ERR_SOCKET;
    
    *pPartnerId = 0;
    
    /* Non-blocking check for data */
    result = WaitForSocketReady(relaySocket, FALSE, 0);
    if (result < 0) {
        return RD2K_ERR_SERVER_LOST;
    }
    if (result == 0) {
        return 0;  /* No data available - no partner yet */
    }
    
    /* Data available - receive header */
    recvLen = recv(relaySocket, (char*)buffer, sizeof(buffer), 0);
    if (recvLen == 0) {
        return RD2K_ERR_SERVER_LOST;  /* Server closed connection */
    }
    if (recvLen == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return 0;  /* No data */
        }
        return RD2K_ERR_RECV;
    }
    
    if (recvLen < (int)sizeof(RELAY_HEADER)) {
        return 0;  /* Incomplete data, try again later */
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    /* Decrypt if encrypted */
    if ((header.flags & 0x01) && header.dataLength > 0 && 
        recvLen >= (int)(sizeof(RELAY_HEADER) + header.dataLength)) {
        Crypto_Decrypt(buffer + sizeof(RELAY_HEADER), header.dataLength);
    }
    
    /* Check for partner connected notification */
    if (header.msgType == RELAY_MSG_PARTNER_CONNECTED) {
        if (recvLen >= (int)(sizeof(RELAY_HEADER) + sizeof(RELAY_PARTNER_CONNECTED))) {
            CopyMemory(&partnerConnected, buffer + sizeof(RELAY_HEADER), sizeof(partnerConnected));
            *pPartnerId = partnerConnected.partnerId;
            return RD2K_SUCCESS;  /* Partner connected! */
        }
    }
    
    /* Check for partner disconnected */
    if (header.msgType == RELAY_MSG_PARTNER_DISCONNECTED) {
        return RD2K_ERR_PARTNER_LEFT;
    }
    
    /* Check for disconnect */
    if (header.msgType == RELAY_MSG_DISCONNECT) {
        return RD2K_ERR_SERVER_LOST;
    }
    
    /* Other message - ignore */
    return 0;
}

int Relay_SendData(SOCKET relaySocket, const BYTE *data, DWORD length)
{
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!data) return RD2K_ERR_SOCKET;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_DATA, data, length);
}

/* 
 * Helper to receive exact number of bytes, handling TCP fragmentation.
 * Returns: bytes received, 0 on connection closed, -1 on socket error, -2 on timeout
 */
static int RecvExactClient(SOCKET sock, BYTE *buffer, int length, int timeoutMs)
{
    int totalRecv = 0;
    int recvLen;
    int waitResult;
    int retries = 0;
    int maxRetries = (timeoutMs / 50) + 5;  /* 50ms polling intervals */
    
    while (totalRecv < length) {
        waitResult = WaitForSocketReady(sock, FALSE, 50);
        
        if (waitResult < 0) {
            return -1;  /* Socket error */
        }
        
        if (waitResult == 0) {
            retries++;
            if (retries > maxRetries) {
                return -2;  /* Timeout */
            }
            continue;
        }
        
        recvLen = recv(sock, (char*)(buffer + totalRecv), length - totalRecv, 0);
        
        if (recvLen == 0) {
            return 0;  /* Connection closed - relay server stopped */
        }
        
        if (recvLen == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                continue;
            }
            return -1;  /* Real error */
        }
        
        totalRecv += recvLen;
        retries = 0;
    }
    
    return totalRecv;
}

int Relay_RecvData(SOCKET relaySocket, BYTE *buffer, DWORD bufferSize, DWORD timeoutMs)
{
    RELAY_HEADER header;
    BYTE headerBuf[sizeof(RELAY_HEADER)];
    int recvLen;
    int result;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!buffer) return RD2K_ERR_SOCKET;
    
    /* Wait for data to be available */
    result = WaitForSocketReady(relaySocket, FALSE, timeoutMs);
    if (result < 0) {
        return RD2K_ERR_RECV;
    }
    if (result == 0) {
        return 0;  /* Timeout */
    }
    
    /* Step 1: Receive the relay header (8 bytes) completely */
    recvLen = RecvExactClient(relaySocket, headerBuf, sizeof(RELAY_HEADER), timeoutMs);
    
    if (recvLen == 0) {
        return RD2K_ERR_SERVER_LOST;  /* Connection to relay server closed */
    }
    if (recvLen < 0) {
        return RD2K_ERR_RECV;
    }
    if (recvLen < (int)sizeof(RELAY_HEADER)) {
        return RD2K_ERR_SERVER_LOST;  /* Partial header indicates server disconnect */
    }
    
    CopyMemory(&header, headerBuf, sizeof(RELAY_HEADER));
    
    /* Check if it's a DATA message */
    if (header.msgType != RELAY_MSG_DATA) {
        /* Not a data message - could be ping/disconnect/partner left */
        if (header.msgType == RELAY_MSG_DISCONNECT) {
            return RD2K_ERR_DISCONNECTED;
        }
        if (header.msgType == RELAY_MSG_PARTNER_DISCONNECTED) {
            /* Partner has disconnected from relay - consume payload if any */
            if (header.dataLength > 0 && header.dataLength <= sizeof(RELAY_PARTNER_DISCONNECTED)) {
                RecvExactClient(relaySocket, buffer, header.dataLength, 1000);
            }
            return RD2K_ERR_PARTNER_LEFT;
        }
        /* Skip non-data message payload if any.
         * CRITICAL: Must use <= not < to handle case where payload size equals buffer size
         * (e.g., PARTNER_CONNECTED is 8 bytes, same as RD2K_HEADER size passed by caller) */
        if (header.dataLength > 0 && header.dataLength <= bufferSize) {
            RecvExactClient(relaySocket, buffer, header.dataLength, 1000);
        } else if (header.dataLength > bufferSize) {
            /* Payload larger than buffer - must still drain to stay in sync */
            BYTE drainBuf[64];
            DWORD remaining = header.dataLength;
            while (remaining > 0) {
                DWORD chunk = (remaining > sizeof(drainBuf)) ? sizeof(drainBuf) : remaining;
                int drained = RecvExactClient(relaySocket, drainBuf, chunk, 1000);
                if (drained <= 0) break;
                remaining -= drained;
            }
        }
        return 0;  /* Ignore, caller should retry */
    }
    
    /* Validate data length */
    if (header.dataLength == 0) {
        return 0;  /* Empty data packet */
    }
    if (header.dataLength > bufferSize) {
        /* Data too large for buffer - this is a problem */
        /* Try to drain the socket to stay in sync */
        DWORD remaining = header.dataLength;
        while (remaining > 0) {
            DWORD chunk = (remaining > bufferSize) ? bufferSize : remaining;
            recvLen = RecvExactClient(relaySocket, buffer, chunk, 5000);
            if (recvLen <= 0) break;
            remaining -= recvLen;
        }
        return 0;  /* Data was too large, discarded */
    }
    
    /* Step 2: Receive the complete data payload.
     * Use shorter timeout (2s) because data should follow header immediately.
     * If header arrived but data doesn't, connection is likely bad. */
    recvLen = RecvExactClient(relaySocket, buffer, header.dataLength, 2000);
    
    if (recvLen == 0) {
        return RD2K_ERR_SERVER_LOST;  /* Connection to relay server lost */
    }
    if (recvLen < 0) {
        return RD2K_ERR_RECV;
    }
    
    if ((DWORD)recvLen < header.dataLength) {
        /* Incomplete data - corrupted stream */
        return RD2K_ERR_RECV;
    }
    
    /* Decrypt if encrypted */
    if (header.flags & 0x01) {
        Crypto_Decrypt(buffer, recvLen);
    }
    
    return recvLen;
}
