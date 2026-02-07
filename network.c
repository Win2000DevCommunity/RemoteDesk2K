/*
 * RemoteDesk2K - Network Module Implementation
 * Windows 2000 compatible network I/O with proper socket options
 */

#include "network.h"

/* Socket timeout values (milliseconds) */
#define SOCKET_SEND_TIMEOUT     60000   /* 60 seconds for large files */
#define SOCKET_RECV_TIMEOUT     60000   /* 60 seconds for large files */
#define SOCKET_BUFFER_SIZE      (512 * 1024)  /* 512 KB socket buffer for large transfers */
#define SOCKET_SELECT_TIMEOUT   200     /* 200ms select timeout */
#define MAX_RETRY_COUNT         600     /* 600 * 200ms = 120 seconds for large file transfers */

/*
 * WaitForSocket - Wait for socket to be ready for read or write
 * Returns: 1 if ready, 0 if timeout, -1 if error
 */
static int WaitForSocket(SOCKET sock, BOOL bWrite, int timeoutMs)
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
    if (result == 0) return 0;  /* Timeout */
    return 1;  /* Ready */
}

/* Configure socket options for reliable file transfer */
static void ConfigureSocket(SOCKET sock)
{
    int opt;
    struct linger ling;
    
    if (sock == INVALID_SOCKET) return;
    
    /* Set send timeout (backup, we use select() primarily) */
    opt = SOCKET_SEND_TIMEOUT;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&opt, sizeof(opt));
    
    /* Set receive timeout (backup, we use select() primarily) */
    opt = SOCKET_RECV_TIMEOUT;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&opt, sizeof(opt));
    
    /* Increase send buffer size */
    opt = SOCKET_BUFFER_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&opt, sizeof(opt));
    
    /* Increase receive buffer size */
    opt = SOCKET_BUFFER_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&opt, sizeof(opt));
    
    /* Disable Nagle's algorithm for lower latency (TCP_NODELAY) */
    opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    
    /* Enable keep-alive to detect dead connections */
    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&opt, sizeof(opt));
    
    /* Set linger to ensure data is sent before close - longer for large files */
    ling.l_onoff = 1;
    ling.l_linger = 30;  /* Wait up to 30 seconds for large files */
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char*)&ling, sizeof(ling));
}

PRD2K_NETWORK Network_Create(WORD port)
{
    PRD2K_NETWORK pNet = (PRD2K_NETWORK)calloc(1, sizeof(RD2K_NETWORK));
    if (!pNet) return NULL;
    
    pNet->listenSocket = INVALID_SOCKET;
    pNet->socket = INVALID_SOCKET;
    pNet->state = STATE_DISCONNECTED;
    pNet->port = port;
    
    pNet->recvBufferSize = RD2K_BUFFER_SIZE;
    pNet->recvBuffer = (BYTE*)malloc(pNet->recvBufferSize);
    pNet->sendBufferSize = RD2K_BUFFER_SIZE;
    pNet->sendBuffer = (BYTE*)malloc(pNet->sendBufferSize);
    
    if (!pNet->recvBuffer || !pNet->sendBuffer) {
        Network_Destroy(pNet);
        return NULL;
    }
    
    return pNet;
}

void Network_Destroy(PRD2K_NETWORK pNet)
{
    if (!pNet) return;
    Network_Disconnect(pNet);
    SAFE_FREE(pNet->recvBuffer);
    SAFE_FREE(pNet->sendBuffer);
    free(pNet);
}

int Network_Listen(PRD2K_NETWORK pNet)
{
    struct sockaddr_in addr;
    int opt = 1;
    
    if (!pNet) return RD2K_ERR_SOCKET;
    
    pNet->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pNet->listenSocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    setsockopt(pNet->listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(pNet->port);
    
    if (bind(pNet->listenSocket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        SAFE_CLOSE_SOCKET(pNet->listenSocket);
        return RD2K_ERR_SOCKET;
    }
    
    if (listen(pNet->listenSocket, 5) == SOCKET_ERROR) {
        SAFE_CLOSE_SOCKET(pNet->listenSocket);
        return RD2K_ERR_SOCKET;
    }
    
    pNet->state = STATE_LISTENING;
    return RD2K_SUCCESS;
}

SOCKET Network_Accept(PRD2K_NETWORK pNet)
{
    struct sockaddr_in clientAddr;
    int addrLen = sizeof(clientAddr);
    SOCKET clientSocket;
    
    if (!pNet || pNet->listenSocket == INVALID_SOCKET) return INVALID_SOCKET;
    
    clientSocket = accept(pNet->listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
    if (clientSocket != INVALID_SOCKET) {
        strcpy(pNet->remoteHost, inet_ntoa(clientAddr.sin_addr));
        pNet->socket = clientSocket;
        pNet->state = STATE_HANDSHAKE;
        
        /* Configure socket for reliable transfer */
        ConfigureSocket(clientSocket);
    }
    
    return clientSocket;
}

int Network_Connect(PRD2K_NETWORK pNet, const char *host, WORD port)
{
    struct sockaddr_in addr;
    struct hostent *pHost;
    unsigned long ipAddr;
    
    if (!pNet || !host) return RD2K_ERR_CONNECT;
    
    strncpy(pNet->remoteHost, host, sizeof(pNet->remoteHost) - 1);
    pNet->port = port;
    
    pNet->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pNet->socket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    ipAddr = inet_addr(host);
    if (ipAddr != INADDR_NONE) {
        addr.sin_addr.s_addr = ipAddr;
    } else {
        pHost = gethostbyname(host);
        if (!pHost) {
            SAFE_CLOSE_SOCKET(pNet->socket);
            return RD2K_ERR_CONNECT;
        }
        memcpy(&addr.sin_addr, pHost->h_addr, pHost->h_length);
    }
    
    pNet->state = STATE_CONNECTING;
    
    if (connect(pNet->socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        SAFE_CLOSE_SOCKET(pNet->socket);
        pNet->state = STATE_DISCONNECTED;
        return RD2K_ERR_CONNECT;
    }
    
    /* Configure socket for reliable transfer */
    ConfigureSocket(pNet->socket);
    
    pNet->state = STATE_HANDSHAKE;
    return RD2K_SUCCESS;
}

void Network_Disconnect(PRD2K_NETWORK pNet)
{
    if (!pNet) return;
    
    if (pNet->socket != INVALID_SOCKET) {
        shutdown(pNet->socket, SD_BOTH);
        SAFE_CLOSE_SOCKET(pNet->socket);
    }
    
    if (pNet->listenSocket != INVALID_SOCKET) {
        SAFE_CLOSE_SOCKET(pNet->listenSocket);
    }
    
    pNet->state = STATE_DISCONNECTED;
}

int Network_Send(PRD2K_NETWORK pNet, const BYTE *data, DWORD length)
{
    int totalSent = 0, sent;
    int retryCount = 0;
    int lastError;
    int selectResult;
    
    if (!pNet || pNet->socket == INVALID_SOCKET || !data) return RD2K_ERR_SEND;
    
    while (totalSent < (int)length) {
        /* Wait for socket to be ready for writing */
        selectResult = WaitForSocket(pNet->socket, TRUE, SOCKET_SELECT_TIMEOUT);
        if (selectResult < 0) {
            return RD2K_ERR_SEND;  /* Socket error */
        }
        if (selectResult == 0) {
            /* Timeout - retry up to max count */
            if (++retryCount >= MAX_RETRY_COUNT) {
                return RD2K_ERR_SEND;  /* Total timeout exceeded */
            }
            continue;
        }
        
        /* Calculate how much to send in this call - limit to avoid overwhelming */
        {
            int toSend = length - totalSent;
            if (toSend > 16384) toSend = 16384;  /* Send max 16KB at a time */
            
            sent = send(pNet->socket, (const char*)(data + totalSent), toSend, 0);
        }
        
        if (sent == SOCKET_ERROR) {
            lastError = WSAGetLastError();
            
            /* Handle WSAEWOULDBLOCK - socket busy, retry */
            if (lastError == WSAEWOULDBLOCK) {
                if (++retryCount >= MAX_RETRY_COUNT) {
                    return RD2K_ERR_SEND;
                }
                Sleep(10);
                continue;
            }
            /* Handle timeout - may be recoverable */
            else if (lastError == WSAETIMEDOUT) {
                if (++retryCount >= MAX_RETRY_COUNT) {
                    return RD2K_ERR_SEND;
                }
                Sleep(100);
                continue;
            }
            /* Handle connection reset - not recoverable */
            else if (lastError == WSAECONNRESET || lastError == WSAECONNABORTED) {
                return RD2K_ERR_SEND;
            }
            /* Handle no buffer space - wait and retry */
            else if (lastError == WSAENOBUFS) {
                if (++retryCount >= MAX_RETRY_COUNT) {
                    return RD2K_ERR_SEND;
                }
                Sleep(50);  /* Wait for buffers to drain */
                continue;
            }
            return RD2K_ERR_SEND;
        }
        
        if (sent == 0) return RD2K_ERR_SEND;
        
        totalSent += sent;
        retryCount = 0;  /* Reset retry count on successful send */
        
        /* Small yield between sends for Windows 2000 network stack */
        if (totalSent < (int)length) {
            Sleep(0);
        }
    }
    
    return RD2K_SUCCESS;
}

int Network_RecvExact(PRD2K_NETWORK pNet, BYTE *buffer, DWORD length)
{
    int totalRecv = 0, recv_bytes;
    int retryCount = 0;
    int lastError;
    int selectResult;
    
    if (!pNet || pNet->socket == INVALID_SOCKET || !buffer) return RD2K_ERR_RECV;
    
    while (totalRecv < (int)length) {
        /* Wait for socket to have data available */
        selectResult = WaitForSocket(pNet->socket, FALSE, SOCKET_SELECT_TIMEOUT);
        if (selectResult < 0) {
            return RD2K_ERR_RECV;  /* Socket error */
        }
        if (selectResult == 0) {
            /* Timeout - retry up to max count */
            if (++retryCount >= MAX_RETRY_COUNT) {
                return RD2K_ERR_RECV;  /* Total timeout exceeded */
            }
            continue;
        }
        
        /* Limit receive size to avoid issues on Windows 2000 */
        {
            int toRecv = length - totalRecv;
            if (toRecv > 16384) toRecv = 16384;  /* Receive max 16KB at a time */
            
            recv_bytes = recv(pNet->socket, (char*)(buffer + totalRecv), toRecv, 0);
        }
        
        if (recv_bytes == SOCKET_ERROR) {
            lastError = WSAGetLastError();
            
            /* Handle WSAEWOULDBLOCK - no data yet, retry */
            if (lastError == WSAEWOULDBLOCK) {
                if (++retryCount >= MAX_RETRY_COUNT) {
                    return RD2K_ERR_RECV;
                }
                Sleep(10);
                continue;
            }
            /* Handle timeout - may be recoverable */
            else if (lastError == WSAETIMEDOUT) {
                if (++retryCount >= MAX_RETRY_COUNT) {
                    return RD2K_ERR_RECV;
                }
                Sleep(100);
                continue;
            }
            /* Handle connection reset - not recoverable */
            else if (lastError == WSAECONNRESET || lastError == WSAECONNABORTED) {
                return RD2K_ERR_RECV;
            }
            return RD2K_ERR_RECV;
        }
        
        if (recv_bytes == 0) return RD2K_ERR_RECV;  /* Connection closed */
        
        totalRecv += recv_bytes;
        retryCount = 0;  /* Reset retry count on successful receive */
    }
    
    return RD2K_SUCCESS;
}

int Network_SendPacket(PRD2K_NETWORK pNet, BYTE msgType, const BYTE *data, DWORD dataLength)
{
    RD2K_HEADER header;
    int result;
    
    if (!pNet) return RD2K_ERR_SEND;
    
    header.msgType = msgType;
    header.flags = 0;
    header.reserved = 0;
    header.dataLength = dataLength;
    header.checksum = data ? CalculateChecksum(data, dataLength) : 0;
    
    result = Network_Send(pNet, (const BYTE*)&header, sizeof(header));
    if (result != RD2K_SUCCESS) return result;
    
    if (data && dataLength > 0) {
        result = Network_Send(pNet, data, dataLength);
    }
    
    return result;
}

int Network_RecvPacket(PRD2K_NETWORK pNet, RD2K_HEADER *pHeader, BYTE *data, DWORD maxDataLength)
{
    int result;
    
    if (!pNet || !pHeader) return RD2K_ERR_RECV;
    
    result = Network_RecvExact(pNet, (BYTE*)pHeader, sizeof(RD2K_HEADER));
    if (result != RD2K_SUCCESS) return result;
    
    if (pHeader->dataLength > 0) {
        if (pHeader->dataLength > maxDataLength) return RD2K_ERR_PROTOCOL;
        
        result = Network_RecvExact(pNet, data, pHeader->dataLength);
        if (result != RD2K_SUCCESS) return result;
        
        if (CalculateChecksum(data, pHeader->dataLength) != pHeader->checksum) {
            return RD2K_ERR_PROTOCOL;
        }
    }
    
    return RD2K_SUCCESS;
}

BOOL Network_DataAvailable(PRD2K_NETWORK pNet)
{
    fd_set readSet;
    struct timeval timeout;
    
    if (!pNet || pNet->socket == INVALID_SOCKET) return FALSE;
    
    FD_ZERO(&readSet);
    FD_SET(pNet->socket, &readSet);
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000;
    
    return (select(0, &readSet, NULL, NULL, &timeout) > 0);
}
