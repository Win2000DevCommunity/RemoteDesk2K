/*
 * relay.c - RemoteDesk2K Linux Relay Server
 * 
 * POSIX/pthread implementation of the relay server
 * Compatible with Windows RemoteDesk2K clients
 */

#include "common.h"
#include "crypto.h"
#include <netinet/tcp.h>

/* Connection keepalive - only for detecting truly dead sockets
 * No restrictive timeouts - clients can join/leave/rejoin freely */

/* Hard close timeout only for completely dead connections (5 minutes) */
#define DEAD_CONNECTION_TIMEOUT_MS  300000  /* 5 minutes for truly dead sockets */

/* Session state machine */
#define SESSION_STATE_EMPTY         0    /* No session */
#define SESSION_STATE_ACTIVE        1    /* Both clients connected */
#define SESSION_STATE_PARTIAL       2    /* One client disconnected, can rejoin anytime */
#define SESSION_STATE_CLOSING       3    /* Both clients gone, cleanup pending */

/* Session cleanup interval */
#define SESSION_CLEANUP_INTERVAL_MS 5000   /* Check every 5 seconds */

/* Maximum number of concurrent sessions */
#define RELAY_MAX_SESSIONS          512

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

/* Forward declarations */
typedef struct _RELAY_SESSION RELAY_SESSION;

typedef struct _RELAY_CONNECTION {
    SOCKET              socket;
    DWORD               clientId;
    DWORD               state;
    pthread_t           thread;
    int                 threadValid;
    volatile int        shouldStop;
    struct _RELAY_CONNECTION* pPartner;
    RELAY_SESSION*      pSession;        /* Session this connection belongs to */
    struct _RELAY_SERVER* pServer;
    BYTE*               recvBuffer;
    DWORD               recvBufferSize;
    DWORD               lastActivity;
    DWORD               pendingKeepalive;   /* 1 if waiting for PONG response */
    DWORD               lastKeepaliveSent;  /* When PING was sent (0 if none pending) */
} RELAY_CONNECTION;

/* Session structure - PRIMARY KEY for paired connections */
struct _RELAY_SESSION {
    DWORD               sessionId;
    DWORD               state;
    DWORD               clientId1;
    DWORD               clientId2;
    RELAY_CONNECTION*   pClient1;
    RELAY_CONNECTION*   pClient2;
    DWORD               createdTime;
    DWORD               lastActivity;
    DWORD               client1DisconnectTime;
    DWORD               client2DisconnectTime;
};

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
    /* Session management */
    RELAY_SESSION**     sessions;
    DWORD               maxSessions;
    DWORD               activeSessions;
    DWORD               nextSessionId;
    pthread_mutex_t     sessionMutex;
    pthread_t           sessionCleanupThread;
    int                 sessionCleanupThreadValid;
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

/* FIX (v6.9): Replaced malloc+free per packet with stack buffer for small
 * messages and a proper send loop for partial sends.  During screen updates
 * the relay forwards 100-500+ rects per frame — malloc+free for each was a
 * major source of throughput loss.  The send loop prevents truncated writes
 * from corrupting the TCP stream. */
static int SendRelayPacket(SOCKET sock, BYTE msgType, const BYTE *data, DWORD dataLength)
{
    RELAY_HEADER header;
    BYTE stackBuf[4096];  /* Stack buffer for small control messages */
    BYTE *packet;
    DWORD packetSize;
    DWORD totalSent;
    ssize_t sent;
    
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    packetSize = sizeof(RELAY_HEADER) + dataLength;
    
    if (packetSize <= sizeof(stackBuf)) {
        packet = stackBuf;
    } else {
        packet = (BYTE*)malloc(packetSize);
        if (!packet) return RD2K_ERR_MEMORY;
    }
    
    header.msgType = msgType;
    header.flags = 0x01;  /* Encrypted */
    header.reserved = 0;
    header.dataLength = dataLength;
    
    memcpy(packet, &header, sizeof(RELAY_HEADER));
    if (data && dataLength > 0) {
        memcpy(packet + sizeof(RELAY_HEADER), data, dataLength);
        Crypto_Encrypt(packet + sizeof(RELAY_HEADER), dataLength);
    }
    
    /* Send with proper partial-send handling */
    totalSent = 0;
    while (totalSent < packetSize) {
        sent = send(sock, packet + totalSent,
                    (size_t)(packetSize - totalSent), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);  /* 10ms */
                continue;
            }
            if (packet != stackBuf) free(packet);
            return RD2K_ERR_SEND;
        }
        if (sent == 0) {
            if (packet != stackBuf) free(packet);
            return RD2K_ERR_SEND;
        }
        totalSent += (DWORD)sent;
    }
    
    if (packet != stackBuf) free(packet);
    return RD2K_SUCCESS;
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

/* ============================================================
 * SESSION MANAGEMENT
 * Sessions are the primary key for paired connections.
 * Clients can join/leave/rejoin freely without restrictions.
 * ============================================================ */

/* Create a new session for pairing two clients */
static RELAY_SESSION* CreateSession(RELAY_SERVER *pServer, RELAY_CONNECTION *pClient1, RELAY_CONNECTION *pClient2)
{
    RELAY_SESSION *pSession;
    DWORD i;
    
    if (!pServer || !pClient1 || !pClient2) return NULL;
    
    pSession = (RELAY_SESSION*)malloc(sizeof(RELAY_SESSION));
    if (!pSession) return NULL;
    
    memset(pSession, 0, sizeof(RELAY_SESSION));
    
    pthread_mutex_lock(&pServer->sessionMutex);
    
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i] == NULL) {
            pServer->nextSessionId++;
            pSession->sessionId = pServer->nextSessionId;
            pSession->state = SESSION_STATE_ACTIVE;
            pSession->clientId1 = pClient1->clientId;
            pSession->clientId2 = pClient2->clientId;
            pSession->pClient1 = pClient1;
            pSession->pClient2 = pClient2;
            pSession->createdTime = GetTickCount();
            pSession->lastActivity = GetTickCount();
            pSession->client1DisconnectTime = 0;
            pSession->client2DisconnectTime = 0;
            
            pClient1->pSession = pSession;
            pClient2->pSession = pSession;
            pClient1->pPartner = pClient2;
            pClient2->pPartner = pClient1;
            
            pServer->sessions[i] = pSession;
            pServer->activeSessions++;
            
            pthread_mutex_unlock(&pServer->sessionMutex);
            return pSession;
        }
    }
    
    pthread_mutex_unlock(&pServer->sessionMutex);
    free(pSession);
    return NULL;
}

/* Find existing session involving a client ID */
static RELAY_SESSION* FindSessionByClientId(RELAY_SERVER *pServer, DWORD clientId)
{
    DWORD i;
    
    if (!pServer) return NULL;
    
    pthread_mutex_lock(&pServer->sessionMutex);
    
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i] != NULL) {
            RELAY_SESSION *pSession = pServer->sessions[i];
            if ((pSession->clientId1 == clientId || pSession->clientId2 == clientId) &&
                pSession->state != SESSION_STATE_EMPTY) {
                pthread_mutex_unlock(&pServer->sessionMutex);
                return pSession;
            }
        }
    }
    
    pthread_mutex_unlock(&pServer->sessionMutex);
    return NULL;
}

/* Mark a client as disconnected from session (but session stays alive) */
static void SessionClientDisconnected(RELAY_SERVER *pServer, RELAY_SESSION *pSession, RELAY_CONNECTION *pClient)
{
    if (!pServer || !pSession || !pClient) return;
    
    pthread_mutex_lock(&pServer->sessionMutex);
    
    if (pSession->pClient1 == pClient) {
        pSession->pClient1 = NULL;
        pSession->client1DisconnectTime = GetTickCount();
    } else if (pSession->pClient2 == pClient) {
        pSession->pClient2 = NULL;
        pSession->client2DisconnectTime = GetTickCount();
    }
    
    if (!pSession->pClient1 && !pSession->pClient2) {
        pSession->state = SESSION_STATE_CLOSING;
    } else {
        pSession->state = SESSION_STATE_PARTIAL;
    }
    
    if (pSession->pClient1) {
        pSession->pClient1->pPartner = NULL;
    }
    if (pSession->pClient2) {
        pSession->pClient2->pPartner = NULL;
    }
    
    pClient->pSession = NULL;
    pClient->pPartner = NULL;
    
    pthread_mutex_unlock(&pServer->sessionMutex);
}

/* Rejoin a client to an existing session */
static BOOL SessionClientRejoined(RELAY_SERVER *pServer, RELAY_SESSION *pSession, RELAY_CONNECTION *pClient)
{
    BOOL success = FALSE;
    
    if (!pServer || !pSession || !pClient) return FALSE;
    
    pthread_mutex_lock(&pServer->sessionMutex);
    
    if (pSession->state == SESSION_STATE_PARTIAL || pSession->state == SESSION_STATE_ACTIVE) {
        /* Find which slot this client should fill */
        if (pSession->clientId1 == pClient->clientId) {
            /* Client1 slot - check if we need to replace old connection */
            if (pSession->pClient1 != NULL && pSession->pClient1 != pClient) {
                /* Old connection still in slot - kick it and replace */
                RELAY_CONNECTION *pOld = pSession->pClient1;
                pOld->pSession = NULL;
                pOld->pPartner = NULL;
                pOld->state = RELAY_STATE_DISCONNECTED;
                pOld->shouldStop = 1;
                RelayLog("[SESSION] Replacing stale connection in slot 1\n");
            }
            pSession->pClient1 = pClient;
            pSession->client1DisconnectTime = 0;
            pClient->pSession = pSession;
            pClient->pPartner = pSession->pClient2;
            if (pSession->pClient2) {
                pSession->pClient2->pPartner = pClient;
            }
            success = TRUE;
        } else if (pSession->clientId2 == pClient->clientId) {
            /* Client2 slot - check if we need to replace old connection */
            if (pSession->pClient2 != NULL && pSession->pClient2 != pClient) {
                /* Old connection still in slot - kick it and replace */
                RELAY_CONNECTION *pOld = pSession->pClient2;
                pOld->pSession = NULL;
                pOld->pPartner = NULL;
                pOld->state = RELAY_STATE_DISCONNECTED;
                pOld->shouldStop = 1;
                RelayLog("[SESSION] Replacing stale connection in slot 2\n");
            }
            pSession->pClient2 = pClient;
            pSession->client2DisconnectTime = 0;
            pClient->pSession = pSession;
            pClient->pPartner = pSession->pClient1;
            if (pSession->pClient1) {
                pSession->pClient1->pPartner = pClient;
            }
            success = TRUE;
        }
        
        if (pSession->pClient1 && pSession->pClient2) {
            pSession->state = SESSION_STATE_ACTIVE;
        }
        
        pSession->lastActivity = GetTickCount();
    }
    
    pthread_mutex_unlock(&pServer->sessionMutex);
    return success;
}

/* Destroy a session and cleanup */
static void DestroySession(RELAY_SERVER *pServer, RELAY_SESSION *pSession)
{
    DWORD i;
    
    if (!pServer || !pSession) return;
    
    pthread_mutex_lock(&pServer->sessionMutex);
    
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i] == pSession) {
            pServer->sessions[i] = NULL;
            if (pServer->activeSessions > 0) {
                pServer->activeSessions--;
            }
            break;
        }
    }
    
    if (pSession->pClient1) {
        pSession->pClient1->pSession = NULL;
        pSession->pClient1->pPartner = NULL;
    }
    if (pSession->pClient2) {
        pSession->pClient2->pSession = NULL;
        pSession->pClient2->pPartner = NULL;
    }
    
    pthread_mutex_unlock(&pServer->sessionMutex);
    free(pSession);
}

/* Session cleanup and activity monitoring thread
 * - Checks lastActivity timestamp for all connections
 * - If no activity for 30 seconds, connection is considered dead
 * - No PING/PONG - just monitors actual data flow
 * - For PAIRED connections, data relay IS the heartbeat */
static void* SessionCleanupThread(void *arg)
{
    RELAY_SERVER *pServer = (RELAY_SERVER*)arg;
    DWORD i;
    char idStr[20], idStr1[20], idStr2[20];
    DWORD now;
    
    /* Activity timeout - if no data for this long, connection is dead
     * 30 seconds is generous - active sessions have constant traffic */
    /* Set to 0 or a very large value to disable inactivity timeout */
    #define ACTIVITY_TIMEOUT_MS     0 /* Infinite timeout: never disconnect for inactivity */
    #define CHECK_INTERVAL_MS       5000    /* Check every 5 seconds */
    
    while (pServer->bRunning) {
        usleep(CHECK_INTERVAL_MS * 1000);
        
        if (!pServer->bRunning) break;
        
        now = GetTickCount();
        
        /* === PHASE 1: Check all connections for activity timeout === */
        pthread_mutex_lock(&pServer->connMutex);
        
        for (i = 0; i < pServer->maxConnections; i++) {
            RELAY_CONNECTION *pConn = pServer->connections[i];
            RELAY_CONNECTION *pPartner;
            RELAY_SESSION *pSession;
            DWORD inactiveTime;
            
            if (!pConn) continue;
            if (pConn->socket == INVALID_SOCKET) continue;
            if (pConn->state == RELAY_STATE_DISCONNECTED) continue;
            
            /* Only timeout PAIRED clients (in a session sending data).
             * REGISTERED clients can wait indefinitely for a partner. */
            if (pConn->state != RELAY_STATE_PAIRED) continue;
            
            /* Must have clientId set */
            if (pConn->clientId == 0) continue;
            
            /* Calculate how long since last activity */
            inactiveTime = now - pConn->lastActivity;
            
            /* If timeout is disabled, never disconnect for inactivity */
            if (ACTIVITY_TIMEOUT_MS == 0 || inactiveTime < ACTIVITY_TIMEOUT_MS) {
                continue;
            }
            
            /* No activity for too long - connection is dead */
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[TIMEOUT] Client %s inactive for %u ms - connection dead\n", 
                    idStr, inactiveTime);
            
            /* Save partner before we clear references */
            pPartner = pConn->pPartner;
            pSession = pConn->pSession;
            
            /* Notify partner so they know to stop waiting, then disconnect them too */
            if (pPartner && pPartner->socket != INVALID_SOCKET) {
                RELAY_PARTNER_DISCONNECTED notification;
                char partnerIdStr[20];
                FormatClientId(pPartner->clientId, partnerIdStr);
                
                notification.reason = RELAY_DISCONNECT_TIMEOUT;
                notification.partnerId = pConn->clientId;
                SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                               (const BYTE*)&notification, sizeof(notification));
                
                /* Clear partner's references */
                pPartner->pPartner = NULL;
                pPartner->pSession = NULL;
                
                /* Also disconnect partner completely - they need to reconnect */
                close(pPartner->socket);
                pPartner->socket = INVALID_SOCKET;
                pPartner->state = RELAY_STATE_DISCONNECTED;
                
                RelayLog("[TIMEOUT] Partner %s notified and disconnected\n", partnerIdStr);
            }
            
            /* Clear dead client's references */
            pConn->pPartner = NULL;
            pConn->pSession = NULL;
            
            /* Destroy session BEFORE closing socket (synchronous cleanup) */
            if (pSession) {
                uint32_t sessionId = pSession->sessionId;
                pthread_mutex_unlock(&pServer->connMutex);
                DestroySession(pServer, pSession);
                RelayLog("[TIMEOUT] Session %u destroyed\n", sessionId);
                pthread_mutex_lock(&pServer->connMutex);
            }
            
            /* Close socket and signal worker thread */
            pConn->shouldStop = 1;
            if (pConn->socket != INVALID_SOCKET) {
                close(pConn->socket);
                pConn->socket = INVALID_SOCKET;
            }
            pConn->state = RELAY_STATE_DISCONNECTED;
        }
        
        pthread_mutex_unlock(&pServer->connMutex);
        
        /* === PHASE 2: Cleanup dead sessions === */
        pthread_mutex_lock(&pServer->sessionMutex);
        
        for (i = 0; i < pServer->maxSessions; i++) {
            RELAY_SESSION *pSession = pServer->sessions[i];
            if (!pSession) continue;
            
            FormatClientId(pSession->clientId1, idStr1);
            FormatClientId(pSession->clientId2, idStr2);
            
            if (pSession->state == SESSION_STATE_CLOSING) {
                pthread_mutex_unlock(&pServer->sessionMutex);
                RelayLog("[SESSION] Cleanup: Session %u (%s <-> %s) - both clients gone\n",
                        pSession->sessionId, idStr1, idStr2);
                DestroySession(pServer, pSession);
                pthread_mutex_lock(&pServer->sessionMutex);
            }
        }
        
        pthread_mutex_unlock(&pServer->sessionMutex);
    }
    
    return NULL;
}

/* Remove stale connections with same clientId (for reconnection handling)
 * With session architecture, we allow free reconnection - always returns TRUE */
static BOOL RemoveStaleConnections(RELAY_SERVER *pServer, DWORD clientId, RELAY_CONNECTION *pExclude)
{
    DWORD i;
    
    if (!pServer) return FALSE;
    
    pthread_mutex_lock(&pServer->connMutex);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        RELAY_CONNECTION *pConn = pServer->connections[i];
        if (pConn && pConn != pExclude && pConn->clientId == clientId) {
            char idStr[20];
            FormatClientId(clientId, idStr);
            
            /* Close any existing connection for this ID - allow reconnection */
            RelayLog("[RECONNECT] Client %s reconnecting, closing old connection (state=%d)\n", 
                    idStr, pConn->state);
            
            /* If it was in a session, destroy it (partner gets fresh session too) */
            if (pConn->pSession) {
                /* Clear partner's session reference */
                if (pConn->pPartner) {
                    pConn->pPartner->pSession = NULL;
                    pConn->pPartner->pPartner = NULL;
                }
                DestroySession(pServer, pConn->pSession);
                pConn->pSession = NULL;
            }
            
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
    return TRUE;  /* Always allow reconnection */
}

static void ConfigureClientSocket(SOCKET sock)
{
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    opt = RELAY_BUFFER_SIZE;
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
    RELAY_SESSION *pSession;
    
    if (!pServer || !pConn) return;
    
    /* Clear partner's references if exists */
    if (pConn->pPartner) {
        pConn->pPartner->pSession = NULL;
        pConn->pPartner->pPartner = NULL;
    }
    
    /* Destroy session */
    pSession = FindSessionByClientId(pServer, pConn->clientId);
    if (pSession) {
        DestroySession(pServer, pSession);
    }
    pConn->pSession = NULL;
    
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
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_REGISTER_MSG))
                return -1;
            
            memcpy(&reg, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_REGISTER_MSG));
            
            /* Close any existing connections with this ID - allows instant reconnection */
            RemoveStaleConnections(pServer, reg.clientId, pConn);
            FormatClientId(reg.clientId, idStr);
            
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
            RELAY_SESSION *pSession;
            RELAY_SESSION *pExistingSession;
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_REQUEST))
                return -1;
            
            memcpy(&req, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_REQUEST));
            FormatClientId(pConn->clientId, clientIdStr);
            FormatClientId(req.partnerId, partnerIdStr);
            
            /* Check for existing session */
            pExistingSession = FindSessionByClientId(pServer, pConn->clientId);
            
            /* If we have an existing session with a DIFFERENT partner, destroy it first */
            if (pExistingSession && 
                pExistingSession->clientId1 != req.partnerId && 
                pExistingSession->clientId2 != req.partnerId) {
                RelayLog("[SESSION] Client %s leaving session %u to connect to new partner %s\n",
                        clientIdStr, pExistingSession->sessionId, partnerIdStr);
                DestroySession(pServer, pExistingSession);
                pExistingSession = NULL;
            }
            
            /* Can we rejoin an existing session with this partner? */
            if (pExistingSession && 
                (pExistingSession->clientId1 == req.partnerId || 
                 pExistingSession->clientId2 == req.partnerId)) {
                /* Rejoin existing session */
                response.status = RD2K_SUCCESS;
                
                pPartner = FindConnectionById(pServer, req.partnerId);
                if (SessionClientRejoined(pServer, pExistingSession, pConn)) {
                    RELAY_PARTNER_CONNECTED partnerNotify;
                    
                    pConn->state = RELAY_STATE_PAIRED;
                    
                    if (pPartner && pPartner->socket != INVALID_SOCKET) {
                        pPartner->state = RELAY_STATE_PAIRED;
                        partnerNotify.partnerId = pConn->clientId;
                        partnerNotify.reserved = 0;
                        SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_CONNECTED,
                                       (const BYTE*)&partnerNotify, sizeof(partnerNotify));
                        pPartner->lastActivity = GetTickCount();
                        RelayLog("[CONNECT] %s REJOINED session %u with %s\n", 
                                clientIdStr, pExistingSession->sessionId, partnerIdStr);
                    }
                } else {
                    response.status = RD2K_ERR_CONNECT;
                    RelayLog("[CONNECT] %s -> %s: REJOIN FAILED\n", clientIdStr, partnerIdStr);
                }
            } else {
                /* No existing session - look for partner */
                pPartner = FindConnectionById(pServer, req.partnerId);
                
                if (!pPartner) {
                    response.status = RD2K_ERR_CONNECT;
                    RelayLog("[CONNECT] %s -> %s: NOT ONLINE\n", clientIdStr, partnerIdStr);
                } else if (pPartner->state == RELAY_STATE_PAIRED) {
                    /* Check if partner is in session with US */
                    RELAY_SESSION *pPartnerSession = FindSessionByClientId(pServer, req.partnerId);
                    if (pPartnerSession && 
                        (pPartnerSession->clientId1 == pConn->clientId || 
                         pPartnerSession->clientId2 == pConn->clientId)) {
                        /* Rejoin allowed */
                        response.status = RD2K_SUCCESS;
                        if (SessionClientRejoined(pServer, pPartnerSession, pConn)) {
                            RELAY_PARTNER_CONNECTED partnerNotify;
                            pConn->state = RELAY_STATE_PAIRED;
                            partnerNotify.partnerId = pConn->clientId;
                            partnerNotify.reserved = 0;
                            SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_CONNECTED,
                                           (const BYTE*)&partnerNotify, sizeof(partnerNotify));
                            pPartner->lastActivity = GetTickCount();
                            RelayLog("[CONNECT] %s REJOINED session with PAIRED partner %s\n", 
                                    clientIdStr, partnerIdStr);
                        } else {
                            response.status = RD2K_ERR_CONNECT;
                        }
                    } else {
                        response.status = RD2K_ERR_CONNECT;
                        RelayLog("[CONNECT] %s -> %s: BUSY (in another session)\n", clientIdStr, partnerIdStr);
                    }
                } else if (pPartner->state != RELAY_STATE_REGISTERED) {
                    response.status = RD2K_ERR_CONNECT;
                    RelayLog("[CONNECT] %s -> %s: NOT READY\n", clientIdStr, partnerIdStr);
                } else {
                    /* Partner available - Create new session */
                    pSession = CreateSession(pServer, pPartner, pConn);
                    if (pSession) {
                        RELAY_PARTNER_CONNECTED partnerNotify;
                        
                        pConn->state = RELAY_STATE_PAIRED;
                        pPartner->state = RELAY_STATE_PAIRED;
                        response.status = RD2K_SUCCESS;
                        
                        partnerNotify.partnerId = pConn->clientId;
                        partnerNotify.reserved = 0;
                        SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_CONNECTED,
                                       (const BYTE*)&partnerNotify, sizeof(partnerNotify));
                        pPartner->lastActivity = GetTickCount();
                        
                        RelayLog("[CONNECT] %s <-> %s: NEW SESSION %u\n", 
                                clientIdStr, partnerIdStr, pSession->sessionId);
                    } else {
                        response.status = RD2K_ERR_CONNECT;
                        RelayLog("[CONNECT] %s -> %s: SESSION CREATION FAILED\n", clientIdStr, partnerIdStr);
                    }
                }
            }
            
            SendRelayPacket(pConn->socket, RELAY_MSG_CONNECT_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        case RELAY_MSG_DATA: {
            DWORD now = GetTickCount();
            RELAY_CONNECTION *pTarget = NULL;
            
            /* Find partner - prefer pPartner, fallback to session lookup */
            if (pConn->pPartner && pConn->pPartner->socket != INVALID_SOCKET) {
                pTarget = pConn->pPartner;
            } else if (pConn->pSession) {
                if (pConn->pSession->pClient1 == pConn && 
                    pConn->pSession->pClient2 && 
                    pConn->pSession->pClient2->socket != INVALID_SOCKET) {
                    pTarget = pConn->pSession->pClient2;
                } else if (pConn->pSession->pClient2 == pConn && 
                           pConn->pSession->pClient1 && 
                           pConn->pSession->pClient1->socket != INVALID_SOCKET) {
                    pTarget = pConn->pSession->pClient1;
                }
            }
            
            if (pTarget) {
                SendRelayPacket(pTarget->socket, RELAY_MSG_DATA,
                               buffer + sizeof(RELAY_HEADER), header.dataLength);
                pTarget->lastActivity = now;
            }
            pConn->lastActivity = now;
            return 0;
        }
        
        case RELAY_MSG_DISCONNECT: {
            char idStr[20];
            RELAY_SESSION *pSession;
            RELAY_CONNECTION *pPartner;
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[DISCONNECT] Client %s requested disconnect\n", idStr);
            
            /* Lock before accessing/modifying partner state */
            pthread_mutex_lock(&pServer->connMutex);
            pPartner = pConn->pPartner;
            
            /* When one client disconnects gracefully, notify partner and return them to REGISTERED */
            if (pPartner) {
                char partnerIdStr[20];
                FormatClientId(pPartner->clientId, partnerIdStr);
                
                if (pPartner->socket != INVALID_SOCKET) {
                    RELAY_PARTNER_DISCONNECTED notification;
                    notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
                    notification.partnerId = pConn->clientId;
                    SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                                   (const BYTE*)&notification, sizeof(notification));
                }
                
                /* Partner returns to REGISTERED - stays connected waiting for new pair */
                pPartner->pPartner = NULL;
                pPartner->pSession = NULL;
                pPartner->state = RELAY_STATE_REGISTERED;
                pPartner->lastActivity = GetTickCount();
                
                RelayLog("[SESSION] Partner %s returned to REGISTERED\n", partnerIdStr);
            }
            
            /* Clear this client's references */
            pConn->pPartner = NULL;
            pConn->pSession = NULL;
            pConn->state = RELAY_STATE_DISCONNECTED;
            
            pthread_mutex_unlock(&pServer->connMutex);
            
            /* Destroy the session (has its own locking) */
            pSession = FindSessionByClientId(pServer, pConn->clientId);
            if (pSession) {
                uint32_t sessionId = pSession->sessionId;
                DestroySession(pServer, pSession);
                RelayLog("[SESSION] Session %u destroyed\n", sessionId);
            }
            
            return 1;
        }
        
        case RELAY_MSG_PING: {
            /* Client sent PING - respond with PONG and update activity */
            SendRelayPacket(pConn->socket, RELAY_MSG_PONG, NULL, 0);
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        case RELAY_MSG_PONG: {
            /* Client sent PONG - just update activity timestamp */
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        case RELAY_MSG_UNPAIR: {
            /* End pairing gracefully - this client disconnects, partner stays REGISTERED.
             * Partner remains online and ready for a new connection. */
            char idStr[20];
            RELAY_SESSION *pSession;
            RELAY_CONNECTION *pPartner;
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[UNPAIR] Client %s ending session\n", idStr);
            
            /* Lock before accessing/modifying partner state */
            pthread_mutex_lock(&pServer->connMutex);
            pPartner = pConn->pPartner;
            
            /* Notify partner and return them to REGISTERED */
            if (pPartner) {
                RELAY_PARTNER_DISCONNECTED notification;
                char partnerIdStr[20];
                FormatClientId(pPartner->clientId, partnerIdStr);
                
                notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
                notification.partnerId = pConn->clientId;
                SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                               (const BYTE*)&notification, sizeof(notification));
                
                /* Partner returns to REGISTERED - stays connected waiting for new pair */
                pPartner->pPartner = NULL;
                pPartner->pSession = NULL;
                pPartner->state = RELAY_STATE_REGISTERED;
                pPartner->lastActivity = GetTickCount();
                
                RelayLog("[SESSION] Partner %s returned to REGISTERED\n", partnerIdStr);
            }
            
            /* Clear this client's references before destroying session */
            pConn->pPartner = NULL;
            pConn->pSession = NULL;
            /* CRITICAL FIX: Keep client REGISTERED instead of disconnecting!
             * This allows them to immediately connect to another partner
             * without having to re-register with the relay server. */
            pConn->state = RELAY_STATE_REGISTERED;
            pConn->lastActivity = GetTickCount();
            
            pthread_mutex_unlock(&pServer->connMutex);
            
            /* Destroy the session completely (has its own locking) */
            pSession = FindSessionByClientId(pServer, pConn->clientId);
            if (pSession) {
                uint32_t sessionId = pSession->sessionId;
                DestroySession(pServer, pSession);
                RelayLog("[SESSION] Session %u destroyed\n", sessionId);
            }
            
            RelayLog("[SESSION] Client %s returned to REGISTERED (unpair)\n", idStr);
            return 0;  /* Stay connected - keep processing messages */
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
    
    /* ========================================================================
     * SESSION-BASED ARCHITECTURE - NO PINGS NEEDED!
     * ========================================================================
     * With session logic:
     * - Sessions persist even when clients disconnect
     * - Clients can rejoin anytime using session ID
     * - TCP tells us when connections die (recv returns 0 or error)
     * - No need to actively probe connections
     * 
     * This is simple and elegant - just wait for data or natural socket death.
     * ======================================================================== */
    
    while (pConn->state != RELAY_STATE_DISCONNECTED && pServer->bRunning && !pConn->shouldStop) {
        RELAY_HEADER header;
        DWORD totalPacketSize;
        int recvLen;
        DWORD dataLength;
        
        /* Wait for data - 5 second timeout just to check shouldStop periodically */
        recvLen = RecvExact(pConn->socket, (BYTE*)&header, sizeof(RELAY_HEADER), 5000);
        
        if (recvLen == 0) {
            /* Timeout with no data - that's fine, just loop and wait more.
             * Session logic means we don't need to ping - client is free to be idle. */
            continue;
        }
        if (recvLen < 0) break;  /* Socket error - connection dead */
        if (recvLen < (int)sizeof(RELAY_HEADER)) break;  /* Protocol error */
        
        /* Copy header to buffer and extract data length */
        memcpy(buffer, &header, sizeof(RELAY_HEADER));
        dataLength = header.dataLength;
        totalPacketSize = sizeof(RELAY_HEADER) + dataLength;
        
        /* Sanity check on data length */
        if (dataLength > RELAY_BUFFER_SIZE - sizeof(RELAY_HEADER)) break;
        
        /* Receive the data payload if any */
        if (dataLength > 0) {
            recvLen = RecvExact(pConn->socket, buffer + sizeof(RELAY_HEADER), 
                               dataLength, 30000);
            if (recvLen < 0 || recvLen < (int)dataLength) break;
        }
        
        /* Update activity timestamp */
        pConn->lastActivity = GetTickCount();
        
        /* Process the message */
        result = ProcessRelayMessage(pServer, pConn, buffer, totalPacketSize);
        if (result != 0 && result != RD2K_SUCCESS) break;
    }
    
    /* Cleanup - handle session and notify partner */
    {
        char idStr[20];
        RELAY_SESSION *pSession;
        RELAY_CONNECTION *pPartner;
        FormatClientId(pConn->clientId, idStr);
        
        if (pConn->clientId != 0)
            RelayLog("[DISCONNECT] Client %s connection closed\n", idStr);
        else
            RelayLog("[DISCONNECT] Unregistered client connection closed\n");
        
        /* Lock before accessing/modifying partner state */
        pthread_mutex_lock(&pServer->connMutex);
        pPartner = pConn->pPartner;
        
        /* Notify partner and return to REGISTERED */
        if (pPartner && pPartner->socket != INVALID_SOCKET) {
            RELAY_PARTNER_DISCONNECTED notification;
            char partnerIdStr[20];
            
            FormatClientId(pPartner->clientId, partnerIdStr);
            
            notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
            notification.partnerId = pConn->clientId;
            SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                           (const BYTE*)&notification, sizeof(notification));
            
            /* Partner goes back to REGISTERED state with no session */
            pPartner->pPartner = NULL;
            pPartner->pSession = NULL;
            pPartner->state = RELAY_STATE_REGISTERED;
            pPartner->lastActivity = GetTickCount();
            
            RelayLog("[SESSION] Partner %s returned to REGISTERED\n", partnerIdStr);
        }
        
        /* Clear this client's references */
        pConn->pPartner = NULL;
        pConn->pSession = NULL;
        
        pthread_mutex_unlock(&pServer->connMutex);
        
        /* Destroy the session (has its own locking) */
        pSession = FindSessionByClientId(pServer, pConn->clientId);
        if (pSession) {
            uint32_t sessionId = pSession->sessionId;
            DestroySession(pServer, pSession);
            RelayLog("[SESSION] Session %u destroyed\n", sessionId);
        }
    }
    
    pConn->state = RELAY_STATE_DISCONNECTED;
    pConn->pPartner = NULL;
    pConn->pSession = NULL;
    
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
    
    /* Initialize session management */
    pServer->sessions = (RELAY_SESSION**)calloc(RELAY_MAX_SESSIONS, sizeof(RELAY_SESSION*));
    if (!pServer->sessions) {
        free(pServer->connections);
        free(pServer);
        return NULL;
    }
    pServer->maxSessions = RELAY_MAX_SESSIONS;
    pServer->activeSessions = 0;
    pthread_mutex_init(&pServer->sessionMutex, NULL);
    
    pthread_mutex_init(&pServer->connMutex, NULL);
    
    pServer->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pServer->listenSocket == INVALID_SOCKET) {
        free(pServer->sessions);
        pthread_mutex_destroy(&pServer->sessionMutex);
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
        free(pServer->sessions);
        pthread_mutex_destroy(&pServer->sessionMutex);
        free(pServer->connections);
        pthread_mutex_destroy(&pServer->connMutex);
        free(pServer);
        return NULL;
    }
    
    if (listen(pServer->listenSocket, SOMAXCONN) < 0) {
        close(pServer->listenSocket);
        free(pServer->sessions);
        pthread_mutex_destroy(&pServer->sessionMutex);
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
    
    /* Start session cleanup thread */
    if (pthread_create(&pServer->sessionCleanupThread, NULL, SessionCleanupThread, pServer) != 0) {
        RelayLog("[WARNING] Failed to create session cleanup thread\n");
        /* Non-fatal - server can still run without session cleanup */
    } else {
        pServer->sessionCleanupThreadValid = 1;
    }
    
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
    
    if (pServer->sessionCleanupThreadValid) {
        pthread_join(pServer->sessionCleanupThread, NULL);
        pServer->sessionCleanupThreadValid = 0;
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
    
    /* Clean up all sessions */
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i]) {
            free(pServer->sessions[i]);
            pServer->sessions[i] = NULL;
        }
    }
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i])
            RemoveConnection(pServer, pServer->connections[i]);
    }
    
    if (pServer->listenSocket != INVALID_SOCKET)
        close(pServer->listenSocket);
    
    /* Clean up session management */
    pthread_mutex_destroy(&pServer->sessionMutex);
    free(pServer->sessions);
    
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