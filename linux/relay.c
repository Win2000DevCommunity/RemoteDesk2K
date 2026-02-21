/*
 * relay.c - RemoteDesk2K Linux Relay Server
 * 
 * POSIX/pthread implementation of the relay server
 * Compatible with Windows RemoteDesk2K clients
 */

#include "common.h"
#include "crypto.h"
#include <netinet/tcp.h>

/* Inactivity timeout - disconnect clients that don't send any data */
#define CLIENT_INACTIVITY_TIMEOUT_MS  5000  /* 5 seconds - fast timeout for relay */

/* ============================================================
 * LOGGING
 * ============================================================ */

typedef void (*RELAY_LOG_CALLBACK)(const char* message);
static RELAY_LOG_CALLBACK g_pfnLogCallback = NULL;
static pthread_mutex_t g_logMutex = PTHREAD_MUTEX_INITIALIZER;

void Relay_SetLogCallback(RELAY_LOG_CALLBACK pfnCallback)
{
    pthread_mutex_lock(&g_logMutex);
    g_pfnLogCallback = pfnCallback;
    pthread_mutex_unlock(&g_logMutex);
}

static void RelayLog(const char* format, ...)
{
    char buffer[512];
    va_list args;
    
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer)-1, format, args);
    buffer[sizeof(buffer)-1] = '\0';
    va_end(args);
    
    pthread_mutex_lock(&g_logMutex);
    if (g_pfnLogCallback) {
        g_pfnLogCallback(buffer);
    }
    pthread_mutex_unlock(&g_logMutex);
}

/* ============================================================
 * DATA STRUCTURES
 * ============================================================ */

typedef struct _RELAY_CONNECTION {
    SOCKET              socket;
    DWORD               clientId;
    DWORD               state;
    pthread_t           thread;
    int                 threadValid;
    volatile int        shouldStop;
    struct _RELAY_CONNECTION* pPartner;
    struct _RELAY_SERVER* pServer;
    BYTE*               recvBuffer;
    DWORD               recvBufferSize;
    DWORD               lastActivity;
} RELAY_CONNECTION;

typedef struct _RELAY_SERVER {
    SOCKET              listenSocket;
    RELAY_CONNECTION**  connections;
    DWORD               maxConnections;
    DWORD               activeConnections;
    WORD                port;
    pthread_t           acceptThread;
    int                 acceptThreadValid;
    pthread_mutex_t     connMutex;
    volatile int        bRunning;
} RELAY_SERVER;

/* ============================================================
 * HELPER FUNCTIONS
 * ============================================================ */

static void FormatClientId(DWORD id, char *buffer)
{
    sprintf(buffer, "%03d %03d %03d %03d",
            (id >> 24) & 0xFF,
            (id >> 16) & 0xFF,
            (id >> 8) & 0xFF,
            id & 0xFF);
}

static int SendRelayPacket(SOCKET sock, BYTE msgType, const BYTE *data, DWORD dataLength)
{
    RELAY_HEADER header;
    BYTE *packet;
    DWORD packetSize;
    ssize_t result;
    
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    packetSize = sizeof(RELAY_HEADER) + dataLength;
    packet = (BYTE*)malloc(packetSize);
    if (!packet) return RD2K_ERR_MEMORY;
    
    header.msgType = msgType;
    header.flags = 0x01;  /* Encrypted */
    header.reserved = 0;
    header.dataLength = dataLength;
    
    memcpy(packet, &header, sizeof(RELAY_HEADER));
    if (data && dataLength > 0) {
        memcpy(packet + sizeof(RELAY_HEADER), data, dataLength);
        Crypto_Encrypt(packet + sizeof(RELAY_HEADER), dataLength);
    }
    
    result = send(sock, packet, packetSize, MSG_NOSIGNAL);
    free(packet);
    
    return (result == (ssize_t)packetSize) ? RD2K_SUCCESS : RD2K_ERR_SEND;
}

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
    
    if (bWrite)
        result = select(sock + 1, NULL, &fds, NULL, &tv);
    else
        result = select(sock + 1, &fds, NULL, NULL, &tv);
    
    if (result < 0) return -1;
    if (result == 0) return 0;
    return 1;
}

static BOOL IsSocketAlive(SOCKET sock)
{
    char buf;
    int result;
    fd_set readfds;
    struct timeval tv;
    
    if (sock == INVALID_SOCKET) return FALSE;
    
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    
    result = select(sock + 1, &readfds, NULL, NULL, &tv);
    
    if (result < 0) return FALSE;
    
    if (result > 0) {
        result = recv(sock, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) return FALSE;
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return FALSE;
    }
    
    return TRUE;
}

static RELAY_CONNECTION* FindConnectionById(RELAY_SERVER *pServer, DWORD clientId)
{
    DWORD i;
    
    if (!pServer) return NULL;
    
    pthread_mutex_lock(&pServer->connMutex);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i] && 
            pServer->connections[i]->clientId == clientId &&
            pServer->connections[i]->state != RELAY_STATE_DISCONNECTED) {
            
            pthread_mutex_unlock(&pServer->connMutex);
            return pServer->connections[i];
        }
    }
    
    pthread_mutex_unlock(&pServer->connMutex);
    return NULL;
}

/* Remove stale connections with same clientId (for reconnection handling) 
 * IMPORTANT: Protect PAIRED connections always, REGISTERED with timeout.
 * Dead sockets will be cleaned up naturally when recv() fails.
 * Returns TRUE if ID is available for registration, FALSE if already in use */
static BOOL RemoveStaleConnections(RELAY_SERVER *pServer, DWORD clientId, RELAY_CONNECTION *pExclude)
{
    DWORD i;
    BOOL bIdAvailable = TRUE;
    DWORD currentTime = GetTickCount();
    
    /* Short timeout for registered connections (5 seconds).
     * This allows legitimate reconnections while preventing rapid loops.
     * Dead sockets are detected when recv() fails, which triggers cleanup. */
    #define REGISTERED_TIMEOUT_MS 5000
    
    if (!pServer) return FALSE;
    
    pthread_mutex_lock(&pServer->connMutex);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        RELAY_CONNECTION *pConn = pServer->connections[i];
        if (pConn && pConn != pExclude && pConn->clientId == clientId) {
            char idStr[20];
            DWORD idleTime;
            FormatClientId(clientId, idStr);
            
            /* Check how long since last activity */
            idleTime = currentTime - pConn->lastActivity;
            
            /* PAIRED connections - always protect, never kick out */
            if (pConn->state == RELAY_STATE_PAIRED) {
                RelayLog("[PROTECT] ID %s is in active session (PAIRED) - rejecting duplicate\n", idStr);
                bIdAvailable = FALSE;
                continue;
            }
            
            /* REGISTERED connections - protect unless timed out */
            if (pConn->state == RELAY_STATE_REGISTERED) {
                if (idleTime < REGISTERED_TIMEOUT_MS) {
                    RelayLog("[PROTECT] ID %s is recently registered (%u ms ago) - rejecting duplicate\n", 
                            idStr, idleTime);
                    bIdAvailable = FALSE;
                    continue;
                }
                /* Timed out REGISTERED connection - allow removal */
                RelayLog("[TIMEOUT] ID %s was REGISTERED but idle for %u ms - allowing reconnect\n", 
                        idStr, idleTime);
            }
            
            /* Remove: DISCONNECTED, CONNECTED(zombie), or timed-out REGISTERED */
            RelayLog("[CLEANUP] Removing connection for ID %s (state=%d, idle=%u ms)\n", 
                    idStr, pConn->state, idleTime);
            
            /* Signal stop and close socket */
            pConn->shouldStop = 1;
            if (pConn->socket != INVALID_SOCKET) {
                close(pConn->socket);
                pConn->socket = INVALID_SOCKET;
            }
            pConn->state = RELAY_STATE_DISCONNECTED;
            
            /* Clear slot and update count */
            pServer->connections[i] = NULL;
            if (pServer->activeConnections > 0) {
                pServer->activeConnections--;
            }
        }
    }
    
    pthread_mutex_unlock(&pServer->connMutex);
    return bIdAvailable;
    
    #undef REGISTERED_TIMEOUT_MS
}

static void ConfigureClientSocket(SOCKET sock)
{
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    opt = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));
    
    /* Enable keep-alive with aggressive settings */
    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    
    /* TCP keepalive: start probing after 30s, probe every 5s, fail after 3 probes */
    opt = 30;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &opt, sizeof(opt));
    opt = 5;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &opt, sizeof(opt));
    opt = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &opt, sizeof(opt));
}

static RELAY_CONNECTION* AddConnection(RELAY_SERVER *pServer, SOCKET sock)
{
    RELAY_CONNECTION *pConn;
    DWORD i;
    
    if (!pServer || pServer->activeConnections >= pServer->maxConnections)
        return NULL;
    
    ConfigureClientSocket(sock);
    
    pConn = (RELAY_CONNECTION*)calloc(1, sizeof(RELAY_CONNECTION));
    if (!pConn) return NULL;
    
    pConn->socket = sock;
    pConn->state = RELAY_STATE_CONNECTED;
    pConn->pServer = pServer;
    pConn->recvBufferSize = RELAY_BUFFER_SIZE;
    pConn->recvBuffer = (BYTE*)malloc(RELAY_BUFFER_SIZE);
    pConn->shouldStop = 0;
    pConn->threadValid = 0;
    
    if (!pConn->recvBuffer) {
        free(pConn);
        return NULL;
    }
    
    pthread_mutex_lock(&pServer->connMutex);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (!pServer->connections[i]) {
            pServer->connections[i] = pConn;
            pServer->activeConnections++;
            pthread_mutex_unlock(&pServer->connMutex);
            return pConn;
        }
    }
    
    pthread_mutex_unlock(&pServer->connMutex);
    
    free(pConn->recvBuffer);
    free(pConn);
    return NULL;
}

static void RemoveConnection(RELAY_SERVER *pServer, RELAY_CONNECTION *pConn)
{
    DWORD i;
    
    if (!pServer || !pConn) return;
    
    pthread_mutex_lock(&pServer->connMutex);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i] == pConn) {
            pServer->connections[i] = NULL;
            if (pServer->activeConnections > 0)
                pServer->activeConnections--;
            break;
        }
    }
    
    pthread_mutex_unlock(&pServer->connMutex);
    
    if (pConn->socket != INVALID_SOCKET)
        close(pConn->socket);
    if (pConn->recvBuffer)
        free(pConn->recvBuffer);
    free(pConn);
}

/* ============================================================
 * MESSAGE PROCESSING
 * ============================================================ */

static int ProcessRelayMessage(RELAY_SERVER *pServer, RELAY_CONNECTION *pConn,
                              BYTE *buffer, DWORD length)
{
    RELAY_HEADER header;
    RELAY_CONNECTION *pPartner;
    RELAY_CONNECT_RESPONSE response;
    
    if (length < sizeof(RELAY_HEADER)) return -1;
    
    memcpy(&header, buffer, sizeof(RELAY_HEADER));
    
    if ((header.flags & 0x01) && header.dataLength > 0)
        Crypto_Decrypt(buffer + sizeof(RELAY_HEADER), header.dataLength);
    
    switch (header.msgType) {
        case RELAY_MSG_REGISTER: {
            RELAY_REGISTER_MSG reg;
            RELAY_REGISTER_RESPONSE regResponse;
            char idStr[20];
            BOOL bIdAvailable;
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_REGISTER_MSG))
                return -1;
            
            memcpy(&reg, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_REGISTER_MSG));
            bIdAvailable = RemoveStaleConnections(pServer, reg.clientId, pConn);
            FormatClientId(reg.clientId, idStr);
            
            if (!bIdAvailable) {
                /* Send duplicate ID error to client */
                RelayLog("[REJECT] Registration rejected for ID %s - already connected\n", idStr);
                regResponse.status = RELAY_REGISTER_DUPLICATE;
                regResponse.reserved = 0;
                SendRelayPacket(pConn->socket, RELAY_MSG_REGISTER_RESPONSE,
                               (const BYTE*)&regResponse, sizeof(regResponse));
                return -1;  /* Disconnect after sending response */
            }
            
            pConn->clientId = reg.clientId;
            pConn->state = RELAY_STATE_REGISTERED;
            pConn->lastActivity = GetTickCount();
            
            /* Send success response */
            regResponse.status = RELAY_REGISTER_OK;
            regResponse.reserved = 0;
            SendRelayPacket(pConn->socket, RELAY_MSG_REGISTER_RESPONSE,
                           (const BYTE*)&regResponse, sizeof(regResponse));
            
            RelayLog("[REGISTER] Client ID: %s registered\n", idStr);
            return 0;
        }
        
        case RELAY_MSG_CONNECT_REQUEST: {
            RELAY_CONNECT_REQUEST req;
            char clientIdStr[20], partnerIdStr[20];
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_REQUEST))
                return -1;
            
            memcpy(&req, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_REQUEST));
            FormatClientId(pConn->clientId, clientIdStr);
            FormatClientId(req.partnerId, partnerIdStr);
            
            pPartner = FindConnectionById(pServer, req.partnerId);
            
            if (!pPartner) {
                response.status = RD2K_ERR_CONNECT;
                RelayLog("[CONNECT] %s -> %s: NOT ONLINE\n", clientIdStr, partnerIdStr);
            } else if (pPartner->state == RELAY_STATE_PAIRED) {
                response.status = RD2K_ERR_CONNECT;
                RelayLog("[CONNECT] %s -> %s: BUSY\n", clientIdStr, partnerIdStr);
            } else if (pPartner->state != RELAY_STATE_REGISTERED) {
                response.status = RD2K_ERR_CONNECT;
                RelayLog("[CONNECT] %s -> %s: NOT READY\n", clientIdStr, partnerIdStr);
            } else {
                /* Partner available - Pair the connections */
                RELAY_PARTNER_CONNECTED partnerNotify;
                
                pConn->pPartner = pPartner;
                pPartner->pPartner = pConn;
                pConn->state = RELAY_STATE_PAIRED;
                pPartner->state = RELAY_STATE_PAIRED;
                response.status = RD2K_SUCCESS;
                
                /* CRITICAL: Notify the partner that someone connected to them!
                 * Without this, the partner doesn't know they're paired and
                 * won't start the handshake → authentication fails! */
                partnerNotify.partnerId = pConn->clientId;
                partnerNotify.reserved = 0;
                SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_CONNECTED,
                               (const BYTE*)&partnerNotify, sizeof(partnerNotify));
                
                /* Update partner's activity too */
                pPartner->lastActivity = GetTickCount();
                
                RelayLog("[CONNECT] %s <-> %s: PAIRED\n", clientIdStr, partnerIdStr);
                RelayLog("[NOTIFY] Sent PARTNER_CONNECTED to %s\n", partnerIdStr);
            }
            
            SendRelayPacket(pConn->socket, RELAY_MSG_CONNECT_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        case RELAY_MSG_DATA: {
            DWORD now = GetTickCount();
            if (pConn->pPartner && pConn->pPartner->socket != INVALID_SOCKET) {
                SendRelayPacket(pConn->pPartner->socket, RELAY_MSG_DATA,
                               buffer + sizeof(RELAY_HEADER), header.dataLength);
                /* Update BOTH partners' activity - CRITICAL for preventing timeout */
                pConn->pPartner->lastActivity = now;
            }
            pConn->lastActivity = now;
            return 0;
        }
        
        case RELAY_MSG_DISCONNECT: {
            char idStr[20];
            char partnerIdStr[20];
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[DISCONNECT] Client %s requested disconnect\n", idStr);
            
            /* Mark this client as disconnected */
            pConn->state = RELAY_STATE_DISCONNECTED;
            
            /* If has partner, notify and disconnect partner too */
            if (pConn->pPartner) {
                CONNECTION *pPartner = pConn->pPartner;
                FormatClientId(pPartner->clientId, partnerIdStr);
                
                /* Notify partner that session ended */
                if (pPartner->socket != INVALID_SOCKET) {
                    SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED, NULL, 0);
                    RelayLog("[DISCONNECT] Sent PARTNER_DISCONNECTED to %s\n", partnerIdStr);
                }
                
                /* Signal partner's disconnect event (if using threaded mode) */
                if (pPartner->hDisconnectEvent) {
                    pthread_cond_signal(pPartner->hDisconnectEvent);
                }
                
                /* Mark partner as disconnected */
                pPartner->state = RELAY_STATE_DISCONNECTED;
                
                /* Clear partner linkage */
                pPartner->pPartner = NULL;
                pConn->pPartner = NULL;
                
                RelayLog("[DISCONNECT] Session %s <-> %s terminated\n", idStr, partnerIdStr);
            }
            
            return 1;
        }
        
        case RELAY_MSG_PING: {
            SendRelayPacket(pConn->socket, RELAY_MSG_PONG, NULL, 0);
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        default:
            RelayLog("[ERROR] Unknown message type: 0x%02X\n", header.msgType);
            return -1;
    }
}

static int RecvExact(SOCKET sock, BYTE *buffer, int length, int timeoutMs)
{
    int totalRecv = 0;
    ssize_t recvLen;
    int waitResult;
    int retries = 0;
    int maxRetries = (timeoutMs / 100) + 10;
    
    while (totalRecv < length) {
        waitResult = WaitForSocketReady(sock, FALSE, 100);
        
        if (waitResult < 0) return -1;
        if (waitResult == 0) {
            retries++;
            if (retries > maxRetries) return totalRecv > 0 ? totalRecv : 0;
            continue;
        }
        
        recvLen = recv(sock, buffer + totalRecv, length - totalRecv, 0);
        
        if (recvLen == 0) return 0;
        if (recvLen < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        
        totalRecv += recvLen;
        retries = 0;
    }
    
    return totalRecv;
}

/* ============================================================
 * WORKER THREAD
 * ============================================================ */

static void* ClientWorkerThread(void *arg)
{
    RELAY_CONNECTION *pConn = (RELAY_CONNECTION*)arg;
    RELAY_SERVER *pServer;
    BYTE *buffer;
    int result;
    
    if (!pConn) return NULL;
    
    pServer = pConn->pServer;
    if (!pServer) return NULL;
    
    buffer = (BYTE*)malloc(RELAY_BUFFER_SIZE);
    if (!buffer) return NULL;
    
    RelayLog("[INFO] Client worker thread started\n");
    
    /* Initialize last activity time for timeout tracking */
    pConn->lastActivity = GetTickCount();
    
    while (pConn->state != RELAY_STATE_DISCONNECTED && pServer->bRunning && !pConn->shouldStop) {
        RELAY_HEADER header;
        DWORD totalPacketSize;
        int recvLen;
        DWORD currentTime;
        DWORD inactiveTime;
        
        recvLen = RecvExact(pConn->socket, (BYTE*)&header, sizeof(RELAY_HEADER), 1000);
        
        if (recvLen == 0) {
            /* Timeout with no data - check for inactivity timeout */
            currentTime = GetTickCount();
            inactiveTime = currentTime - pConn->lastActivity;
            
            if (inactiveTime > CLIENT_INACTIVITY_TIMEOUT_MS) {
                char idStr[20];
                FormatClientId(pConn->clientId, idStr);
                RelayLog("[TIMEOUT] Client %s inactive for %u ms - disconnecting\n", 
                        idStr, inactiveTime);
                break;
            }
            continue;
        }
        if (recvLen < 0) break;
        if (recvLen < (int)sizeof(RELAY_HEADER)) break;
        
        totalPacketSize = sizeof(RELAY_HEADER) + header.dataLength;
        
        if (header.dataLength > RELAY_BUFFER_SIZE - sizeof(RELAY_HEADER)) break;
        
        memcpy(buffer, &header, sizeof(RELAY_HEADER));
        
        if (header.dataLength > 0) {
            recvLen = RecvExact(pConn->socket, buffer + sizeof(RELAY_HEADER), 
                               header.dataLength, 30000);
            if (recvLen < 0 || recvLen < (int)header.dataLength) break;
        }
        
        result = ProcessRelayMessage(pServer, pConn, buffer, totalPacketSize);
        if (result != 0 && result != RD2K_SUCCESS) break;
    }
    
    /* Cleanup */
    {
        char idStr[20];
        FormatClientId(pConn->clientId, idStr);
        
        if (pConn->clientId != 0)
            RelayLog("[DISCONNECT] Client %s connection closed\n", idStr);
        else
            RelayLog("[DISCONNECT] Unregistered client connection closed\n");
        
        if (pConn->pPartner && pConn->pPartner->socket != INVALID_SOCKET) {
            RELAY_PARTNER_DISCONNECTED notification;
            notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
            notification.partnerId = pConn->clientId;
            SendRelayPacket(pConn->pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                           (const BYTE*)&notification, sizeof(notification));
            /* Signal partner's disconnect event so their thread exits */
            if (pConn->pPartner->hDisconnectEvent) {
                pthread_cond_signal(pConn->pPartner->hDisconnectEvent);
            }
            pConn->pPartner->pPartner = NULL;
            /* CRITICAL: Set to DISCONNECTED, not REGISTERED!
             * This ensures ID is cleaned up when they reconnect. */
            pConn->pPartner->state = RELAY_STATE_DISCONNECTED;
        }
    }
    
    pConn->state = RELAY_STATE_DISCONNECTED;
    pConn->pPartner = NULL;
    
    free(buffer);
    
    /* Remove from server array */
    pthread_mutex_lock(&pServer->connMutex);
    for (DWORD i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i] == pConn) {
            pServer->connections[i] = NULL;
            if (pServer->activeConnections > 0) pServer->activeConnections--;
            break;
        }
    }
    pthread_mutex_unlock(&pServer->connMutex);
    
    if (pConn->socket != INVALID_SOCKET) {
        close(pConn->socket);
        pConn->socket = INVALID_SOCKET;
    }
    if (pConn->recvBuffer) {
        free(pConn->recvBuffer);
        pConn->recvBuffer = NULL;
    }
    
    /* Free the connection struct to prevent memory leak.
     * Thread is detached so no join needed. */
    free(pConn);
    
    return NULL;
}

/* ============================================================
 * ACCEPT THREAD
 * ============================================================ */

static void* AcceptThread(void *arg)
{
    RELAY_SERVER *pServer = (RELAY_SERVER*)arg;
    struct sockaddr_in clientAddr;
    socklen_t addrLen;
    SOCKET clientSocket;
    RELAY_CONNECTION *pConn;
    fd_set readfds;
    struct timeval tv;
    int selectResult;
    
    if (!pServer) return NULL;
    
    RelayLog("[INFO] Accept thread started, waiting for connections...\n");
    
    while (pServer->bRunning) {
        FD_ZERO(&readfds);
        FD_SET(pServer->listenSocket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
        selectResult = select(pServer->listenSocket + 1, &readfds, NULL, NULL, &tv);
        if (selectResult < 0) {
            if (errno == EINTR) continue;
            RelayLog("[ERROR] select() failed: %s\n", strerror(errno));
            break;
        }
        if (selectResult == 0) continue;
        
        addrLen = sizeof(clientAddr);
        clientSocket = accept(pServer->listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (pServer->bRunning && errno != EINTR)
                RelayLog("[ERROR] accept() failed: %s\n", strerror(errno));
            continue;
        }
        
        RelayLog("[INFO] New client connection accepted\n");
        
        pConn = AddConnection(pServer, clientSocket);
        if (!pConn) {
            RelayLog("[ERROR] Failed to add connection (max reached?)\n");
            close(clientSocket);
            continue;
        }
        
        if (pthread_create(&pConn->thread, NULL, ClientWorkerThread, pConn) == 0) {
            pConn->threadValid = 1;
            pthread_detach(pConn->thread);
            RelayLog("[INFO] Worker thread created for new client\n");
        } else {
            RelayLog("[ERROR] Failed to create worker thread\n");
            RemoveConnection(pServer, pConn);
        }
    }
    
    RelayLog("[INFO] Accept thread stopping\n");
    return NULL;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

RELAY_SERVER* Relay_Create(WORD port, const char* ipAddr)
{
    RELAY_SERVER *pServer;
    struct sockaddr_in addr;
    int opt = 1;
    
    pServer = (RELAY_SERVER*)calloc(1, sizeof(RELAY_SERVER));
    if (!pServer) return NULL;
    
    pServer->port = port;
    pServer->maxConnections = RELAY_MAX_CONNECTIONS;
    pServer->activeConnections = 0;
    pServer->bRunning = 0;
    pServer->acceptThreadValid = 0;
    
    pServer->connections = (RELAY_CONNECTION**)calloc(RELAY_MAX_CONNECTIONS, sizeof(RELAY_CONNECTION*));
    if (!pServer->connections) {
        free(pServer);
        return NULL;
    }
    
    pthread_mutex_init(&pServer->connMutex, NULL);
    
    pServer->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pServer->listenSocket == INVALID_SOCKET) {
        free(pServer->connections);
        pthread_mutex_destroy(&pServer->connMutex);
        free(pServer);
        return NULL;
    }
    
    setsockopt(pServer->listenSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    if (ipAddr && ipAddr[0] && strcmp(ipAddr, "0.0.0.0") != 0)
        addr.sin_addr.s_addr = inet_addr(ipAddr);
    else
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    
    if (bind(pServer->listenSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(pServer->listenSocket);
        free(pServer->connections);
        pthread_mutex_destroy(&pServer->connMutex);
        free(pServer);
        return NULL;
    }
    
    if (listen(pServer->listenSocket, SOMAXCONN) < 0) {
        close(pServer->listenSocket);
        free(pServer->connections);
        pthread_mutex_destroy(&pServer->connMutex);
        free(pServer);
        return NULL;
    }
    
    return pServer;
}

int Relay_Start(RELAY_SERVER *pServer)
{
    if (!pServer) return RD2K_ERR_SOCKET;
    
    pServer->bRunning = 1;
    
    if (pthread_create(&pServer->acceptThread, NULL, AcceptThread, pServer) != 0) {
        pServer->bRunning = 0;
        return RD2K_ERR_SOCKET;
    }
    
    pServer->acceptThreadValid = 1;
    return RD2K_SUCCESS;
}

void Relay_Stop(RELAY_SERVER *pServer)
{
    DWORD i;
    
    if (!pServer) return;
    
    pServer->bRunning = 0;
    
    if (pServer->acceptThreadValid) {
        pthread_join(pServer->acceptThread, NULL);
        pServer->acceptThreadValid = 0;
    }
    
    pthread_mutex_lock(&pServer->connMutex);
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i]) {
            pServer->connections[i]->shouldStop = 1;
            if (pServer->connections[i]->socket != INVALID_SOCKET)
                shutdown(pServer->connections[i]->socket, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&pServer->connMutex);
}

void Relay_Destroy(RELAY_SERVER *pServer)
{
    DWORD i;
    
    if (!pServer) return;
    
    Relay_Stop(pServer);
    
    /* Wait a bit for worker threads to finish */
    usleep(500000);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i])
            RemoveConnection(pServer, pServer->connections[i]);
    }
    
    if (pServer->listenSocket != INVALID_SOCKET)
        close(pServer->listenSocket);
    
    pthread_mutex_destroy(&pServer->connMutex);
    free(pServer->connections);
    free(pServer);
}

void Relay_GetStats(RELAY_SERVER *pServer, DWORD *activeConnections)
{
    if (!pServer || !activeConnections) return;
    
    pthread_mutex_lock(&pServer->connMutex);
    *activeConnections = pServer->activeConnections;
    pthread_mutex_unlock(&pServer->connMutex);
}