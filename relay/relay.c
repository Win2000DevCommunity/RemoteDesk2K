/*
 * RemoteDesk2K - Relay Server Implementation
 * Windows 2000 compatible relay server
 * With XOR encryption support for secure message passing
 */

#include "relay.h"
#include "crypto.h"

/* TCP keepalive structure for SIO_KEEPALIVE_VALS (not in old SDK headers) */
#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
#endif

struct tcp_keepalive {
    u_long onoff;
    u_long keepalivetime;
    u_long keepaliveinterval;
};

/* Connection keepalive - only for detecting truly dead sockets
 * No restrictive timeouts - clients can join/leave/rejoin freely */

/* Hard close timeout only for completely dead connections (5 minutes) */
#define DEAD_CONNECTION_TIMEOUT_MS  300000  /* 5 minutes for truly dead sockets */

/* Logging callback for GUI console output */
typedef void (*RELAY_LOG_CALLBACK)(const char* message);
static RELAY_LOG_CALLBACK g_pfnLogCallback = NULL;
static CRITICAL_SECTION g_csLog;
static BOOL g_bLogInitialized = FALSE;

/* Set logging callback */
void Relay_SetLogCallback(RELAY_LOG_CALLBACK pfnCallback)
{
    if (!g_bLogInitialized) {
        InitializeCriticalSection(&g_csLog);
        g_bLogInitialized = TRUE;
    }
    EnterCriticalSection(&g_csLog);
    g_pfnLogCallback = pfnCallback;
    LeaveCriticalSection(&g_csLog);
}

/* Internal logging function */
static void RelayLog(const char* format, ...)
{
    char buffer[512];
    va_list args;
    
    if (!g_bLogInitialized) return;
    
    va_start(args, format);
    _vsnprintf(buffer, sizeof(buffer)-1, format, args);
    buffer[sizeof(buffer)-1] = '\0';
    va_end(args);
    
    EnterCriticalSection(&g_csLog);
    if (g_pfnLogCallback) {
        g_pfnLogCallback(buffer);
    }
    LeaveCriticalSection(&g_csLog);
}

/* Relay server context */
typedef struct _RELAY_SERVER {
    SOCKET              listenSocket;
    SOCKET              *connectionSockets;
    PRELAY_CONNECTION   *connections;
    DWORD               maxConnections;
    DWORD               activeConnections;
    WORD                port;
    HANDLE              hServerThread;
    HANDLE              hStopEvent;
    CRITICAL_SECTION    csConnections;
    BOOL                bRunning;
    
    /* Session management */
    PRELAY_SESSION      *sessions;          /* Array of session pointers */
    DWORD               maxSessions;
    DWORD               activeSessions;
    DWORD               nextSessionId;      /* Counter for unique session IDs */
    CRITICAL_SECTION    csSessions;
    HANDLE              hSessionCleanupThread;
} RELAY_SERVER_CONTEXT;

/* Helper: Format client ID as friendly string "XXX XXX XXX XXX" */
static void FormatClientId(DWORD id, char *buffer)
{
    /* Convert DWORD to 4 bytes displayed as decimals with spaces */
    sprintf(buffer, "%03d %03d %03d %03d",
            (id >> 24) & 0xFF,
            (id >> 16) & 0xFF,
            (id >> 8) & 0xFF,
            id & 0xFF);
}

/* Helper: Send a relay packet to a socket with XOR encryption */
static int SendRelayPacket(SOCKET sock, BYTE msgType, const BYTE *data, DWORD dataLength)
{
    RELAY_HEADER header;
    BYTE *packet;
    DWORD packetSize;
    int result;
    
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    packetSize = sizeof(RELAY_HEADER) + dataLength;
    packet = (BYTE*)malloc(packetSize);
    if (!packet) return RD2K_ERR_MEMORY;
    
    header.msgType = msgType;
    header.flags = 0x01;  /* Flag: encrypted */
    header.reserved = 0;
    header.dataLength = dataLength;
    
    CopyMemory(packet, &header, sizeof(RELAY_HEADER));
    if (data && dataLength > 0) {
        CopyMemory(packet + sizeof(RELAY_HEADER), data, dataLength);
        /* XOR encrypt the data portion */
        Crypto_Encrypt(packet + sizeof(RELAY_HEADER), dataLength);
    }
    
    result = send(sock, (const char*)packet, packetSize, 0);
    free(packet);
    
    if (result == SOCKET_ERROR) {
        return RD2K_ERR_SEND;
    }
    
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
    
    if (bWrite) {
        result = select(0, NULL, &fds, NULL, &tv);
    } else {
        result = select(0, &fds, NULL, NULL, &tv);
    }
    
    if (result == SOCKET_ERROR) return -1;
    if (result == 0) return 0;
    return 1;
}

/* Find a connection by clientId */
static PRELAY_CONNECTION FindConnectionById(PRELAY_SERVER pServer, DWORD clientId)
{
    DWORD i;
    
    if (!pServer) return NULL;
    
    EnterCriticalSection(&pServer->csConnections);
    
    /* IMPORTANT: Iterate ALL slots, not just activeConnections count
     * because the array can be sparse after disconnections */
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i] && 
            pServer->connections[i]->clientId == clientId &&
            pServer->connections[i]->state != RELAY_STATE_DISCONNECTED) {
            
            LeaveCriticalSection(&pServer->csConnections);
            return pServer->connections[i];
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    return NULL;
}

/* ============================================================
 * SESSION MANAGEMENT FUNCTIONS
 * 
 * Sessions are the PRIMARY KEY for paired connections.
 * When clients pair, a session is created. When one leaves, 
 * they can rejoin the same session instead of creating a new one.
 * ============================================================ */

/* Create a new session for pairing two clients */
static PRELAY_SESSION CreateSession(PRELAY_SERVER pServer, PRELAY_CONNECTION pClient1, PRELAY_CONNECTION pClient2)
{
    PRELAY_SESSION pSession;
    DWORD i;
    
    if (!pServer || !pClient1 || !pClient2) return NULL;
    
    /* Allocate session */
    pSession = (PRELAY_SESSION)malloc(sizeof(RELAY_SESSION));
    if (!pSession) return NULL;
    
    ZeroMemory(pSession, sizeof(RELAY_SESSION));
    
    EnterCriticalSection(&pServer->csSessions);
    
    /* Find free slot */
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i] == NULL) {
            /* Found slot - initialize session */
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
            
            /* Link session to clients */
            pClient1->pSession = pSession;
            pClient2->pSession = pSession;
            pClient1->pPartner = pClient2;
            pClient2->pPartner = pClient1;
            
            /* Store in array */
            pServer->sessions[i] = pSession;
            pServer->activeSessions++;
            
            LeaveCriticalSection(&pServer->csSessions);
            return pSession;
        }
    }
    
    LeaveCriticalSection(&pServer->csSessions);
    free(pSession);
    return NULL;  /* No free slots */
}

/* Find existing session involving a client ID */
static PRELAY_SESSION FindSessionByClientId(PRELAY_SERVER pServer, DWORD clientId)
{
    DWORD i;
    
    if (!pServer) return NULL;
    
    EnterCriticalSection(&pServer->csSessions);
    
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i] != NULL) {
            PRELAY_SESSION pSession = pServer->sessions[i];
            if ((pSession->clientId1 == clientId || pSession->clientId2 == clientId) &&
                pSession->state != SESSION_STATE_EMPTY) {
                LeaveCriticalSection(&pServer->csSessions);
                return pSession;
            }
        }
    }
    
    LeaveCriticalSection(&pServer->csSessions);
    return NULL;
}

/* Mark a client as disconnected from session (but session stays alive) */
static void SessionClientDisconnected(PRELAY_SERVER pServer, PRELAY_SESSION pSession, PRELAY_CONNECTION pClient)
{
    if (!pServer || !pSession || !pClient) return;
    
    EnterCriticalSection(&pServer->csSessions);
    
    if (pSession->pClient1 == pClient) {
        pSession->pClient1 = NULL;
        pSession->client1DisconnectTime = GetTickCount();
    } else if (pSession->pClient2 == pClient) {
        pSession->pClient2 = NULL;
        pSession->client2DisconnectTime = GetTickCount();
    }
    
    /* Update session state */
    if (!pSession->pClient1 && !pSession->pClient2) {
        /* Both clients gone - mark for cleanup */
        pSession->state = SESSION_STATE_CLOSING;
    } else {
        /* One client remains - wait for rejoin */
        pSession->state = SESSION_STATE_PARTIAL;
    }
    
    /* Update remaining client's partner pointer */
    if (pSession->pClient1) {
        pSession->pClient1->pPartner = NULL;
    }
    if (pSession->pClient2) {
        pSession->pClient2->pPartner = NULL;
    }
    
    pClient->pSession = NULL;
    pClient->pPartner = NULL;
    
    LeaveCriticalSection(&pServer->csSessions);
}

/* Rejoin a client to an existing session */
static BOOL SessionClientRejoined(PRELAY_SERVER pServer, PRELAY_SESSION pSession, PRELAY_CONNECTION pClient)
{
    BOOL success = FALSE;
    PRELAY_CONNECTION pOldConnection = NULL;
    
    if (!pServer || !pSession || !pClient) return FALSE;
    
    EnterCriticalSection(&pServer->csSessions);
    
    if (pSession->state == SESSION_STATE_PARTIAL || pSession->state == SESSION_STATE_ACTIVE) {
        /* Find which slot this client should fill */
        if (pSession->clientId1 == pClient->clientId) {
            /* Client1 slot - check if we need to replace old connection */
            if (pSession->pClient1 != NULL && pSession->pClient1 != pClient) {
                /* Old connection still in slot - kick it and replace */
                pOldConnection = pSession->pClient1;
                pOldConnection->pSession = NULL;
                pOldConnection->pPartner = NULL;
                pOldConnection->state = RELAY_STATE_DISCONNECTED;
                if (pOldConnection->hDisconnectEvent) {
                    SetEvent(pOldConnection->hDisconnectEvent);
                }
                RelayLog("[SESSION] Replacing stale connection in slot 1\r\n");
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
                pOldConnection = pSession->pClient2;
                pOldConnection->pSession = NULL;
                pOldConnection->pPartner = NULL;
                pOldConnection->state = RELAY_STATE_DISCONNECTED;
                if (pOldConnection->hDisconnectEvent) {
                    SetEvent(pOldConnection->hDisconnectEvent);
                }
                RelayLog("[SESSION] Replacing stale connection in slot 2\r\n");
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
        
        /* Update state if both clients now connected */
        if (pSession->pClient1 && pSession->pClient2) {
            pSession->state = SESSION_STATE_ACTIVE;
        }
        
        pSession->lastActivity = GetTickCount();
    }
    
    LeaveCriticalSection(&pServer->csSessions);
    return success;
}

/* Destroy a session and cleanup */
static void DestroySession(PRELAY_SERVER pServer, PRELAY_SESSION pSession)
{
    DWORD i;
    
    if (!pServer || !pSession) return;
    
    EnterCriticalSection(&pServer->csSessions);
    
    /* Remove from array */
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i] == pSession) {
            pServer->sessions[i] = NULL;
            if (pServer->activeSessions > 0) {
                pServer->activeSessions--;
            }
            break;
        }
    }
    
    /* Unlink from any remaining clients */
    if (pSession->pClient1) {
        pSession->pClient1->pSession = NULL;
        pSession->pClient1->pPartner = NULL;
    }
    if (pSession->pClient2) {
        pSession->pClient2->pSession = NULL;
        pSession->pClient2->pPartner = NULL;
    }
    
    LeaveCriticalSection(&pServer->csSessions);
    
    free(pSession);
}

/* Session cleanup and activity monitoring thread
 * - Checks lastActivity timestamp for all connections
 * - If no activity for 30 seconds, connection is considered dead
 * - No PING/PONG - just monitors actual data flow
 * - For PAIRED connections, data relay IS the heartbeat */
static DWORD WINAPI SessionCleanupThread(LPVOID lpParam)
{
    PRELAY_SERVER pServer = (PRELAY_SERVER)lpParam;
    DWORD i;
    char idStr[20], idStr1[20], idStr2[20];
    DWORD now;
    
    /* Activity timeout - if no data for this long, connection is dead
     * 30 seconds is generous - active sessions have constant traffic */
    /* Set to 0 or a very large value to disable inactivity timeout */
    #define ACTIVITY_TIMEOUT_MS     0 /* Infinite timeout: never disconnect for inactivity */
    #define CHECK_INTERVAL_MS       5000    /* Check every 5 seconds */
    
    while (pServer->bRunning) {
        Sleep(CHECK_INTERVAL_MS);
        
        if (!pServer->bRunning) break;
        
        now = GetTickCount();
        
        /* === PHASE 1: Check all connections for activity timeout === */
        EnterCriticalSection(&pServer->csConnections);
        
        for (i = 0; i < pServer->maxConnections; i++) {
            PRELAY_CONNECTION pConn = pServer->connections[i];
            PRELAY_CONNECTION pPartner;
            PRELAY_SESSION pSession;
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
            RelayLog("[TIMEOUT] Client %s inactive for %lu ms - connection dead\r\n", 
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
                if (pPartner->hDisconnectEvent) {
                    SetEvent(pPartner->hDisconnectEvent);
                }
                closesocket(pPartner->socket);
                pPartner->socket = INVALID_SOCKET;
                pPartner->state = RELAY_STATE_DISCONNECTED;
                
                RelayLog("[TIMEOUT] Partner %s notified and disconnected\r\n", partnerIdStr);
            }
            
            /* Clear dead client's references */
            pConn->pPartner = NULL;
            pConn->pSession = NULL;
            
            /* Destroy session BEFORE closing socket (synchronous cleanup) */
            if (pSession) {
                DWORD sessionId = pSession->sessionId;
                LeaveCriticalSection(&pServer->csConnections);
                DestroySession(pServer, pSession);
                RelayLog("[TIMEOUT] Session %lu destroyed\r\n", sessionId);
                EnterCriticalSection(&pServer->csConnections);
            }
            
            /* Close socket and signal worker thread */
            if (pConn->hDisconnectEvent) {
                SetEvent(pConn->hDisconnectEvent);
            }
            if (pConn->socket != INVALID_SOCKET) {
                closesocket(pConn->socket);
                pConn->socket = INVALID_SOCKET;
            }
            pConn->state = RELAY_STATE_DISCONNECTED;
        }
        
        LeaveCriticalSection(&pServer->csConnections);
        
        /* === PHASE 2: Cleanup dead sessions === */
        EnterCriticalSection(&pServer->csSessions);
        
        for (i = 0; i < pServer->maxSessions; i++) {
            PRELAY_SESSION pSession = pServer->sessions[i];
            if (!pSession) continue;
            
            FormatClientId(pSession->clientId1, idStr1);
            FormatClientId(pSession->clientId2, idStr2);
            
            /* Only clean up sessions where BOTH clients are gone */
            if (pSession->state == SESSION_STATE_CLOSING) {
                /* Both clients gone - remove session */
                LeaveCriticalSection(&pServer->csSessions);
                RelayLog("[SESSION] Cleanup: Session %lu (%s <-> %s) - both clients gone\r\n",
                        pSession->sessionId, idStr1, idStr2);
                DestroySession(pServer, pSession);
                EnterCriticalSection(&pServer->csSessions);
            }
        }
        
        LeaveCriticalSection(&pServer->csSessions);
    }
    
    return 0;
}

/* Check if a socket is still connected (not dead) 
 * Returns TRUE if socket appears alive, FALSE only if definitely dead.
 * Uses conservative checks to avoid false negatives on Wine/Linux. */
static BOOL IsSocketAlive(SOCKET sock)
{
    int optval;
    int optlen = sizeof(optval);
    
    if (sock == INVALID_SOCKET) return FALSE;
    
    /* Check socket error status - most reliable cross-platform method */
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen) == SOCKET_ERROR) {
        return FALSE;  /* Cannot query socket - probably dead */
    }
    
    if (optval != 0) {
        return FALSE;  /* Socket has error pending - dead */
    }
    
    /* Socket appears healthy */
    return TRUE;
}

/* Remove stale connections with same clientId (for reconnection handling)
 * With session architecture, we allow free reconnection:
 * - PAIRED in active session: let old connection die naturally, allow new one
 * - REGISTERED: close old connection immediately, allow new one
 * - Always returns TRUE - reconnection is always allowed */
static BOOL RemoveStaleConnections(PRELAY_SERVER pServer, DWORD clientId, PRELAY_CONNECTION pExclude)
{
    DWORD i;
    
    if (!pServer) return FALSE;
    
    EnterCriticalSection(&pServer->csConnections);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        PRELAY_CONNECTION pConn = pServer->connections[i];
        if (pConn && pConn != pExclude && pConn->clientId == clientId) {
            char idStr[20];
            FormatClientId(clientId, idStr);
            
            /* Close any existing connection for this ID - allow reconnection */
            RelayLog("[RECONNECT] Client %s reconnecting, closing old connection (state=%d)\r\n", 
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
            
            /* Signal disconnect and close socket */
            if (pConn->hDisconnectEvent) {
                SetEvent(pConn->hDisconnectEvent);
            }
            if (pConn->socket != INVALID_SOCKET) {
                closesocket(pConn->socket);
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
    
    LeaveCriticalSection(&pServer->csConnections);
    return TRUE;  /* Always allow reconnection */
}

/* Configure client socket for optimal relay performance */
static void ConfigureClientSocket(SOCKET sock)
{
    int opt;
    struct tcp_keepalive keepalive;
    DWORD bytesReturned;
    
    /* Disable Nagle's algorithm for lower latency */
    opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    
    /* Increase socket buffers for large transfers */
    opt = 512 * 1024;  /* 512KB */
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&opt, sizeof(opt));
    
    /* Enable keep-alive with aggressive settings */
    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&opt, sizeof(opt));
    
    /* Configure keepalive: probe after 30s, interval 5s (detect dead connection in ~45s) */
    keepalive.onoff = 1;
    keepalive.keepalivetime = 30000;    /* 30 seconds before first probe */
    keepalive.keepaliveinterval = 5000; /* 5 seconds between probes */
    WSAIoctl(sock, SIO_KEEPALIVE_VALS, &keepalive, sizeof(keepalive),
             NULL, 0, &bytesReturned, NULL, NULL);
}

/* Add a new connection to the server */
static PRELAY_CONNECTION AddConnection(PRELAY_SERVER pServer, SOCKET sock)
{
    PRELAY_CONNECTION pConn;
    DWORD i;
    
    if (!pServer || pServer->activeConnections >= pServer->maxConnections) {
        return NULL;
    }
    
    /* Configure socket options for optimal performance */
    ConfigureClientSocket(sock);
    
    pConn = (PRELAY_CONNECTION)calloc(1, sizeof(RELAY_CONNECTION));
    if (!pConn) return NULL;
    
    pConn->socket = sock;
    pConn->state = RELAY_STATE_CONNECTED;
    pConn->pServer = pServer;  /* Store server reference for worker thread */
    pConn->recvBufferSize = RELAY_BUFFER_SIZE;
    pConn->recvBuffer = (BYTE*)malloc(RELAY_BUFFER_SIZE);
    pConn->recvPos = 0;
    pConn->hDisconnectEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    
    if (!pConn->recvBuffer || !pConn->hDisconnectEvent) {
        if (pConn->recvBuffer) free(pConn->recvBuffer);
        if (pConn->hDisconnectEvent) CloseHandle(pConn->hDisconnectEvent);
        free(pConn);
        return NULL;
    }
    
    EnterCriticalSection(&pServer->csConnections);
    
    /* Find empty slot */
    for (i = 0; i < pServer->maxConnections; i++) {
        if (!pServer->connections[i]) {
            pServer->connections[i] = pConn;
            pServer->activeConnections++;
            LeaveCriticalSection(&pServer->csConnections);
            return pConn;
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    
    /* No slot available */
    if (pConn->recvBuffer) free(pConn->recvBuffer);
    if (pConn->hDisconnectEvent) CloseHandle(pConn->hDisconnectEvent);
    free(pConn);
    return NULL;
}

/* Remove connection from server */
static void RemoveConnection(PRELAY_SERVER pServer, PRELAY_CONNECTION pConn)
{
    DWORD i;
    PRELAY_SESSION pSession;
    
    if (!pServer || !pConn) return;
    
    /* Clear partner's session reference if exists */
    if (pConn->pPartner) {
        pConn->pPartner->pSession = NULL;
        pConn->pPartner->pPartner = NULL;
    }
    
    /* Destroy session (not preserve) */
    pSession = FindSessionByClientId(pServer, pConn->clientId);
    if (pSession) {
        DestroySession(pServer, pSession);
    }
    pConn->pSession = NULL;
    
    EnterCriticalSection(&pServer->csConnections);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i] == pConn) {
            pServer->connections[i] = NULL;
            if (pServer->activeConnections > 0) {
                pServer->activeConnections--;
            }
            break;
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    
    /* Cleanup connection resources */
    if (pConn->socket != INVALID_SOCKET) {
        closesocket(pConn->socket);
    }
    if (pConn->recvBuffer) {
        free(pConn->recvBuffer);
    }
    if (pConn->hDisconnectEvent) {
        CloseHandle(pConn->hDisconnectEvent);
    }
    if (pConn->hThread) {
        CloseHandle(pConn->hThread);
    }
    
    free(pConn);
}

/* Handle relay protocol messages on a connection with XOR decryption */
static int ProcessRelayMessage(PRELAY_SERVER pServer, PRELAY_CONNECTION pConn,
                              BYTE *buffer, DWORD length)
{
    RELAY_HEADER header;
    PRELAY_CONNECTION pPartner;
    RELAY_CONNECT_RESPONSE response;
    
    if (length < sizeof(RELAY_HEADER)) {
        RelayLog("[ERROR] Invalid message size: %lu < %d\r\n", 
                (unsigned long)length, (int)sizeof(RELAY_HEADER));
        return -1;  /* Invalid message */
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    /* XOR decrypt if data is encrypted (flag 0x01) */
    if ((header.flags & 0x01) && header.dataLength > 0) {
        Crypto_Decrypt(buffer + sizeof(RELAY_HEADER), header.dataLength);
    }
    
    switch (header.msgType) {
        case RELAY_MSG_REGISTER: {
            RELAY_REGISTER_MSG reg;
            RELAY_REGISTER_RESPONSE response;
            char idStr[20];
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_REGISTER_MSG)) {
                return -1;
            }
            CopyMemory(&reg, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_REGISTER_MSG));
            
            /* Close any existing connections with this ID - allows instant reconnection */
            RemoveStaleConnections(pServer, reg.clientId, pConn);
            
            FormatClientId(reg.clientId, idStr);
            
            pConn->clientId = reg.clientId;
            pConn->state = RELAY_STATE_REGISTERED;
            pConn->lastActivity = GetTickCount();
            
            /* Send success response */
            response.status = RELAY_REGISTER_OK;
            response.reserved = 0;
            SendRelayPacket(pConn->socket, RELAY_MSG_REGISTER_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            
            /* Log client registration with friendly ID */
            RelayLog("[REGISTER] Client ID: %s registered\r\n", idStr);
            return 0;
        }
        
        case RELAY_MSG_CONNECT_REQUEST: {
            RELAY_CONNECT_REQUEST req;
            char clientIdStr[20], partnerIdStr[20];
            PRELAY_SESSION pSession;
            PRELAY_SESSION pExistingSession;
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_REQUEST)) {
                return -1;
            }
            CopyMemory(&req, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_REQUEST));
            
            /* Format IDs for logging */
            FormatClientId(pConn->clientId, clientIdStr);
            FormatClientId(req.partnerId, partnerIdStr);
            
            /* Check for existing session */
            pExistingSession = FindSessionByClientId(pServer, pConn->clientId);
            
            /* If we have an existing session with a DIFFERENT partner, destroy it first */
            if (pExistingSession && 
                pExistingSession->clientId1 != req.partnerId && 
                pExistingSession->clientId2 != req.partnerId) {
                /* Destroy old session to connect to new partner */
                RelayLog("[SESSION] Client %s leaving session %lu to connect to new partner %s\r\n",
                        clientIdStr, pExistingSession->sessionId, partnerIdStr);
                DestroySession(pServer, pExistingSession);
                pExistingSession = NULL;  /* No longer in a session */
            }
            
            /* Can we rejoin an existing session with this partner? */
            if (pExistingSession && 
                (pExistingSession->clientId1 == req.partnerId || 
                 pExistingSession->clientId2 == req.partnerId)) {
                /* This client can rejoin their existing session with this partner */
                response.status = RD2K_SUCCESS;
                
                /* Rejoin the session */
                pPartner = FindConnectionById(pServer, req.partnerId);
                if (SessionClientRejoined(pServer, pExistingSession, pConn)) {
                    RELAY_PARTNER_CONNECTED partnerNotify;
                    
                    pConn->state = RELAY_STATE_PAIRED;
                    
                    /* Notify partner if they're still connected */
                    if (pPartner && pPartner->socket != INVALID_SOCKET) {
                        pPartner->state = RELAY_STATE_PAIRED;
                        partnerNotify.partnerId = pConn->clientId;
                        partnerNotify.reserved = 0;
                        SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_CONNECTED,
                                       (const BYTE*)&partnerNotify, sizeof(partnerNotify));
                        pPartner->lastActivity = GetTickCount();
                        RelayLog("[CONNECT] Client %s REJOINED session %lu with %s\r\n", 
                                clientIdStr, pExistingSession->sessionId, partnerIdStr);
                    }
                } else {
                    response.status = RD2K_ERR_CONNECT;
                    RelayLog("[CONNECT] Client %s -> Partner %s: REJOIN FAILED\r\n", 
                            clientIdStr, partnerIdStr);
                }
            } else {
                /* No existing session - look for partner to create new session */
                pPartner = FindConnectionById(pServer, req.partnerId);
                if (!pPartner) {
                    /* Partner not connected to relay server */
                    response.status = RD2K_ERR_CONNECT;
                    RelayLog("[CONNECT] Client %s -> Partner %s: NOT ONLINE\r\n", 
                            clientIdStr, partnerIdStr);
                } else if (pPartner->state == RELAY_STATE_PAIRED) {
                    /* Partner is already in a session - check if it's with a different client */
                    PRELAY_SESSION pPartnerSession = FindSessionByClientId(pServer, req.partnerId);
                    if (pPartnerSession && 
                        (pPartnerSession->clientId1 == pConn->clientId || 
                         pPartnerSession->clientId2 == pConn->clientId)) {
                        /* Partner is in session with US - rejoin allowed */
                        response.status = RD2K_SUCCESS;
                        if (SessionClientRejoined(pServer, pPartnerSession, pConn)) {
                            RELAY_PARTNER_CONNECTED partnerNotify;
                            pConn->state = RELAY_STATE_PAIRED;
                            partnerNotify.partnerId = pConn->clientId;
                            partnerNotify.reserved = 0;
                            SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_CONNECTED,
                                           (const BYTE*)&partnerNotify, sizeof(partnerNotify));
                            pPartner->lastActivity = GetTickCount();
                            RelayLog("[CONNECT] Client %s REJOINED session with PAIRED partner %s\r\n", 
                                    clientIdStr, partnerIdStr);
                        } else {
                            response.status = RD2K_ERR_CONNECT;
                        }
                    } else {
                        /* Partner is in session with someone else - BUSY */
                        response.status = RD2K_ERR_CONNECT;
                        RelayLog("[CONNECT] Client %s -> Partner %s: BUSY (in another session)\r\n", 
                                clientIdStr, partnerIdStr);
                    }
                } else if (pPartner->state != RELAY_STATE_REGISTERED) {
                    /* Partner in unexpected state */
                    response.status = RD2K_ERR_CONNECT;
                    RelayLog("[CONNECT] Client %s -> Partner %s: NOT READY (state=%d)\r\n", 
                            clientIdStr, partnerIdStr, pPartner->state);
                } else {
                    /* Partner available - Create new session and pair */
                    pSession = CreateSession(pServer, pPartner, pConn);
                    if (pSession) {
                        RELAY_PARTNER_CONNECTED partnerNotify;
                        
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
                        
                        RelayLog("[CONNECT] Client %s <-> Partner %s: NEW SESSION %lu\r\n", 
                                clientIdStr, partnerIdStr, pSession->sessionId);
                        RelayLog("[NOTIFY] Sent PARTNER_CONNECTED to %s\r\n", partnerIdStr);
                    } else {
                        response.status = RD2K_ERR_CONNECT;
                        RelayLog("[CONNECT] Client %s -> Partner %s: SESSION CREATION FAILED\r\n", 
                                clientIdStr, partnerIdStr);
                    }
                }
            }
            
            /* Send response to requesting client */
            SendRelayPacket(pConn->socket, RELAY_MSG_CONNECT_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            
            pConn->lastActivity = GetTickCount();
            
            /* Don't disconnect client on failed connect - let them try again */
            return 0;
        }
        
        case RELAY_MSG_DATA: {
            DWORD now = GetTickCount();
            PRELAY_CONNECTION pTarget = NULL;
            
            /* Find partner - prefer pPartner, fallback to session lookup */
            if (pConn->pPartner && pConn->pPartner->socket != INVALID_SOCKET) {
                pTarget = pConn->pPartner;
            } else if (pConn->pSession) {
                /* Check session for partner */
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
            
            /* Relay data to partner if found */
            if (pTarget) {
                SendRelayPacket(pTarget->socket, RELAY_MSG_DATA,
                               buffer + sizeof(RELAY_HEADER), header.dataLength);
                /* CRITICAL: Update BOTH partners' activity - data flows both ways! */
                pTarget->lastActivity = now;
            }
            pConn->lastActivity = now;
            return 0;
        }
        
        case RELAY_MSG_DISCONNECT: {
            char idStr[20];
            PRELAY_SESSION pSession;
            PRELAY_CONNECTION pPartner;
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[DISCONNECT] Client %s graceful disconnect\r\n", idStr);
            
            /* Lock before accessing/modifying partner state */
            EnterCriticalSection(&pServer->csConnections);
            pPartner = pConn->pPartner;
            
            /* When one client disconnects gracefully, notify partner and return them to REGISTERED */
            if (pPartner) {
                RELAY_PARTNER_DISCONNECTED notification;
                char partnerIdStr[20];
                FormatClientId(pPartner->clientId, partnerIdStr);
                
                /* Notify partner FIRST before we change state */
                notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
                notification.partnerId = pConn->clientId;
                SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                               (const BYTE*)&notification, sizeof(notification));
                
                /* Partner returns to REGISTERED - stays connected waiting for new pair */
                pPartner->pPartner = NULL;
                pPartner->pSession = NULL;
                pPartner->state = RELAY_STATE_REGISTERED;
                pPartner->lastActivity = GetTickCount();
                
                RelayLog("[SESSION] Partner %s returned to REGISTERED\r\n", partnerIdStr);
            }
            
            /* Clear this client's references */
            pConn->pPartner = NULL;
            pConn->pSession = NULL;
            pConn->state = RELAY_STATE_DISCONNECTED;
            
            LeaveCriticalSection(&pServer->csConnections);
            
            /* Destroy the session (has its own locking) */
            pSession = FindSessionByClientId(pServer, pConn->clientId);
            if (pSession) {
                DWORD sessionId = pSession->sessionId;
                DestroySession(pServer, pSession);
                RelayLog("[SESSION] Session %lu destroyed\r\n", sessionId);
            }
            
            return 1;  /* Signal disconnection */
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
            PRELAY_SESSION pSession;
            PRELAY_CONNECTION pPartner;
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[UNPAIR] Client %s ending session\r\n", idStr);
            
            /* Lock before accessing/modifying partner state */
            EnterCriticalSection(&pServer->csConnections);
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
                
                RelayLog("[SESSION] Partner %s returned to REGISTERED\r\n", partnerIdStr);
            }
            
            /* Clear this client's references before destroying session */
            pConn->pPartner = NULL;
            pConn->pSession = NULL;
            /* CRITICAL FIX: Keep client REGISTERED instead of disconnecting!
             * This allows them to immediately connect to another partner
             * without having to re-register with the relay server. */
            pConn->state = RELAY_STATE_REGISTERED;
            pConn->lastActivity = GetTickCount();
            
            LeaveCriticalSection(&pServer->csConnections);
            
            /* Destroy the session completely (has its own locking) */
            pSession = FindSessionByClientId(pServer, pConn->clientId);
            if (pSession) {
                DWORD sessionId = pSession->sessionId;
                DestroySession(pServer, pSession);
                RelayLog("[SESSION] Session %lu destroyed\r\n", sessionId);
            }
            
            RelayLog("[SESSION] Client %s returned to REGISTERED (unpair)\r\n", idStr);
            return 0;  /* Stay connected - keep processing messages */
        }
        
        default:
            RelayLog("[ERROR] Unknown message type: 0x%02X (flags=0x%02X, dataLen=%lu)\r\n",
                    header.msgType, header.flags, (unsigned long)header.dataLength);
            return -1;  /* Unknown message */
    }
}

/* 
 * Helper function to receive exact number of bytes from socket.
 * Handles TCP fragmentation by looping until all data is received.
 * Returns: number of bytes received, 0 on connection close, -1 on error
 */
static int RecvExact(SOCKET sock, BYTE *buffer, int length, int timeoutMs)
{
    int totalRecv = 0;
    int recvLen;
    int waitResult;
    int retries = 0;
    int maxRetries = (timeoutMs / 100) + 10;  /* Calculate retries based on timeout */
    
    while (totalRecv < length) {
        /* Wait for data */
        waitResult = WaitForSocketReady(sock, FALSE, 100);  /* 100ms poll */
        
        if (waitResult < 0) {
            return -1;  /* Socket error */
        }
        
        if (waitResult == 0) {
            /* Timeout on this poll - check if we've exceeded total timeout */
            retries++;
            if (retries > maxRetries) {
                if (totalRecv > 0) {
                    /* Partial data received, return what we have */
                    return totalRecv;
                }
                return 0;  /* Full timeout, no data */
            }
            continue;
        }
        
        /* Receive available data */
        recvLen = recv(sock, (char*)(buffer + totalRecv), length - totalRecv, 0);
        
        if (recvLen == 0) {
            return 0;  /* Connection closed */
        }
        
        if (recvLen == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                continue;  /* No data available, retry */
            }
            return -1;  /* Real error */
        }
        
        totalRecv += recvLen;
        retries = 0;  /* Reset timeout counter on successful recv */
    }
    
    return totalRecv;
}

/* Worker thread for client connection */
static DWORD WINAPI ClientWorkerThread(LPVOID lpParam)
{
    PRELAY_CONNECTION pConn = (PRELAY_CONNECTION)lpParam;
    PRELAY_SERVER pServer;
    BYTE* buffer;  /* Heap allocated - main packet buffer */
    int result;
    
    if (!pConn) return 1;
    
    /* Get server reference from connection */
    pServer = (PRELAY_SERVER)pConn->pServer;
    if (!pServer) {
        RelayLog("[ERROR] Worker thread: no server reference!\r\n");
        return 1;
    }
    
    /* Allocate receive buffer on heap (4MB is too large for stack!) */
    buffer = (BYTE*)malloc(RELAY_BUFFER_SIZE);
    if (!buffer) {
        RelayLog("[ERROR] Worker thread: failed to allocate buffer!\r\n");
        return 1;
    }
    
    RelayLog("[INFO] Client worker thread started for socket %d\r\n", (int)pConn->socket);
    
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
    
    /* Connection loop - simple and clean */
    while (pConn->state != RELAY_STATE_DISCONNECTED && pServer->bRunning) {
        RELAY_HEADER header;
        DWORD dataLength;
        DWORD totalPacketSize;
        int recvLen;
        
        /* Check for disconnect event (non-blocking) */
        if (WaitForSingleObject(pConn->hDisconnectEvent, 0) == WAIT_OBJECT_0) {
            RelayLog("[INFO] Disconnect event signaled\r\n");
            break;
        }
        
        /* Wait for data - 5 second timeout just to check disconnect event periodically */
        recvLen = RecvExact(pConn->socket, (BYTE*)&header, sizeof(RELAY_HEADER), 5000);
        
        if (recvLen == 0) {
            /* Timeout with no data - that's fine, just loop and wait more.
             * Session logic means we don't need to ping - client is free to be idle. */
            continue;
        }
        
        if (recvLen < 0) {
            /* Socket error - connection is dead, clean exit */
            break;
        }
        
        if (recvLen < (int)sizeof(RELAY_HEADER)) {
            RelayLog("[WARN] Incomplete header: got %d bytes\r\n", recvLen);
            break;  /* Protocol error - incomplete header after timeout */
        }
        
        /* Copy header to buffer for later processing */
        CopyMemory(buffer, &header, sizeof(RELAY_HEADER));
        dataLength = header.dataLength;
        totalPacketSize = sizeof(RELAY_HEADER) + dataLength;
        
        /* Sanity check on data length */
        if (dataLength > RELAY_BUFFER_SIZE - sizeof(RELAY_HEADER)) {
            RelayLog("[ERROR] Invalid data length: %lu bytes\r\n", (unsigned long)dataLength);
            break;  /* Protocol error or corrupted stream */
        }
        
        /* Step 2: Receive the data payload if any */
        if (dataLength > 0) {
            /* Wait up to 30 seconds for large data transfers */
            recvLen = RecvExact(pConn->socket, buffer + sizeof(RELAY_HEADER), 
                               dataLength, 30000);
            
            if (recvLen < 0) {
                RelayLog("[ERROR] Socket error receiving data\r\n");
                break;  /* Socket error */
            }
            
            if (recvLen < (int)dataLength) {
                RelayLog("[WARN] Incomplete data: got %d, expected %lu\r\n", 
                        recvLen, (unsigned long)dataLength);
                break;  /* Protocol error - incomplete data after timeout */
            }
        }
        
        /* Update activity timestamp */
        pConn->lastActivity = GetTickCount();
        
        /* Step 3: Process the message */
        result = ProcessRelayMessage(pServer, pConn, buffer, totalPacketSize);
        if (result != 0 && result != RD2K_SUCCESS) {
            /* result = 1 means graceful disconnect (RELAY_MSG_DISCONNECT)
             * result = -1 means protocol error */
            break;
        }
    }
    
    /* Cleanup - IMPORTANT: Notify partner and destroy session */
    {
        char idStr[20];
        PRELAY_SESSION pSession;
        PRELAY_CONNECTION pPartner;
        FormatClientId(pConn->clientId, idStr);
        
        /* Log proper disconnect message */
        if (pConn->clientId != 0) {
            RelayLog("[DISCONNECT] Client %s connection closed\r\n", idStr);
        } else {
            RelayLog("[DISCONNECT] Unregistered client connection closed\r\n");
        }
        
        /* Lock before accessing/modifying partner state */
        EnterCriticalSection(&pServer->csConnections);
        pPartner = pConn->pPartner;
        
        /* If we have a partner, notify them and return to REGISTERED */
        if (pPartner && pPartner->socket != INVALID_SOCKET) {
            RELAY_PARTNER_DISCONNECTED notification;
            char partnerIdStr[20];
            
            FormatClientId(pPartner->clientId, partnerIdStr);
            
            notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
            notification.partnerId = pConn->clientId;
            
            /* Send notification to partner - don't wait for success */
            SendRelayPacket(pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                           (const BYTE*)&notification, sizeof(notification));
            
            RelayLog("[NOTIFY] Sent disconnect notification to partner %s\r\n", partnerIdStr);
            
            /* Partner goes back to REGISTERED state with no session */
            pPartner->pPartner = NULL;
            pPartner->pSession = NULL;
            pPartner->state = RELAY_STATE_REGISTERED;
            pPartner->lastActivity = GetTickCount();
            
            RelayLog("[SESSION] Partner %s returned to REGISTERED\r\n", partnerIdStr);
        }
        
        /* Clear this client's references */
        pConn->pPartner = NULL;
        pConn->pSession = NULL;
        
        LeaveCriticalSection(&pServer->csConnections);
        
        /* Destroy the session (has its own locking) */
        pSession = FindSessionByClientId(pServer, pConn->clientId);
        if (pSession) {
            DWORD sessionId = pSession->sessionId;
            DestroySession(pServer, pSession);
            RelayLog("[SESSION] Session %lu destroyed\r\n", sessionId);
        }
    }
    
    pConn->state = RELAY_STATE_DISCONNECTED;
    pConn->pPartner = NULL;
    pConn->pSession = NULL;
    
    free(buffer);  /* Free heap buffer */
    
    /* IMPORTANT: Remove connection from server's array to allow slot reuse.
     * This prevents stale entries from piling up and causing lookup issues. */
    {
        DWORD i;
        EnterCriticalSection(&pServer->csConnections);
        for (i = 0; i < pServer->maxConnections; i++) {
            if (pServer->connections[i] == pConn) {
                pServer->connections[i] = NULL;
                if (pServer->activeConnections > 0) {
                    pServer->activeConnections--;
                }
                break;
            }
        }
        LeaveCriticalSection(&pServer->csConnections);
        
        /* Close socket and free resources */
        if (pConn->socket != INVALID_SOCKET) {
            closesocket(pConn->socket);
            pConn->socket = INVALID_SOCKET;
        }
        if (pConn->recvBuffer) {
            free(pConn->recvBuffer);
            pConn->recvBuffer = NULL;
        }
        if (pConn->hDisconnectEvent) {
            CloseHandle(pConn->hDisconnectEvent);
            pConn->hDisconnectEvent = NULL;
        }
        /* Close thread handle (safe from within the thread - just releases reference).
         * Then free the connection struct to prevent memory leak. */
        if (pConn->hThread) {
            CloseHandle(pConn->hThread);
            pConn->hThread = NULL;
        }
        free(pConn);
    }
    
    return 0;
}

/* Accept client connections */
static DWORD WINAPI AcceptThread(LPVOID lpParam)
{
    PRELAY_SERVER pServer = (PRELAY_SERVER)lpParam;
    struct sockaddr_in clientAddr;
    int addrLen = sizeof(clientAddr);
    SOCKET clientSocket;
    PRELAY_CONNECTION pConn;
    HANDLE hThread;
    fd_set readfds;
    struct timeval tv;
    int selectResult;
    
    if (!pServer) return 1;
    
    RelayLog("[INFO] Accept thread started, waiting for connections...\r\n");
    
    while (pServer->bRunning) {
        /* Check stop event */
        if (WaitForSingleObject(pServer->hStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }
        
        /* Use select() to check for incoming connections with timeout */
        /* This prevents blocking forever in accept() */
        FD_ZERO(&readfds);
        FD_SET(pServer->listenSocket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms timeout */
        
        selectResult = select(0, &readfds, NULL, NULL, &tv);
        if (selectResult == SOCKET_ERROR) {
            RelayLog("[ERROR] select() failed in accept thread\r\n");
            break;
        }
        if (selectResult == 0) {
            /* Timeout - no connection pending, loop again */
            continue;
        }
        
        /* Connection is pending, accept it */
        addrLen = sizeof(clientAddr);
        clientSocket = accept(pServer->listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (pServer->bRunning) {
                RelayLog("[ERROR] accept() failed\r\n");
            }
            continue;
        }
        
        /* Privacy: Don't log client IP addresses */
        RelayLog("[INFO] New client connection accepted\r\n");
        
        /* Create connection context */
        pConn = AddConnection(pServer, clientSocket);
        if (!pConn) {
            RelayLog("[ERROR] Failed to add connection (max reached?)\r\n");
            closesocket(clientSocket);
            continue;
        }
        
        /* Create worker thread for this connection */
        hThread = CreateThread(NULL, 0, ClientWorkerThread, (LPVOID)pConn, 0, NULL);
        if (hThread) {
            pConn->hThread = hThread;
            RelayLog("[INFO] Worker thread created for new client\r\n");
        } else {
            RelayLog("[ERROR] Failed to create worker thread\r\n");
            RemoveConnection(pServer, pConn);
            closesocket(clientSocket);
        }
    }
    
    RelayLog("[INFO] Accept thread stopping\r\n");
    return 0;
}

/* ===== Public API Implementation ===== */

PRELAY_SERVER Relay_Create(WORD port, const char* ipAddr)
{
    PRELAY_SERVER pServer;
    struct sockaddr_in addr;
    int result;
    
    pServer = (PRELAY_SERVER)calloc(1, sizeof(RELAY_SERVER_CONTEXT));
    if (!pServer) return NULL;
    
    pServer->port = port;
    pServer->maxConnections = RELAY_MAX_CONNECTIONS;
    pServer->activeConnections = 0;
    pServer->bRunning = FALSE;
    
    /* Initialize session management */
    pServer->maxSessions = RELAY_MAX_SESSIONS;
    pServer->activeSessions = 0;
    pServer->nextSessionId = 0;
    pServer->hSessionCleanupThread = NULL;
    
    /* Allocate connection arrays */
    pServer->connectionSockets = (SOCKET*)calloc(RELAY_MAX_CONNECTIONS, sizeof(SOCKET));
    pServer->connections = (PRELAY_CONNECTION*)calloc(RELAY_MAX_CONNECTIONS, sizeof(PRELAY_CONNECTION*));
    
    /* Allocate session array */
    pServer->sessions = (PRELAY_SESSION*)calloc(RELAY_MAX_SESSIONS, sizeof(PRELAY_SESSION));
    
    if (!pServer->connectionSockets || !pServer->connections || !pServer->sessions) {
        if (pServer->connectionSockets) free(pServer->connectionSockets);
        if (pServer->connections) free(pServer->connections);
        if (pServer->sessions) free(pServer->sessions);
        free(pServer);
        return NULL;
    }
    
    /* Initialize critical sections */
    InitializeCriticalSection(&pServer->csConnections);
    InitializeCriticalSection(&pServer->csSessions);
    
    /* Create stop event */
    pServer->hStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!pServer->hStopEvent) {
        free(pServer->connectionSockets);
        free(pServer->connections);
        free(pServer->sessions);
        DeleteCriticalSection(&pServer->csConnections);
        DeleteCriticalSection(&pServer->csSessions);
        free(pServer);
        return NULL;
    }
    
    /* Create listening socket */
    pServer->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pServer->listenSocket == INVALID_SOCKET) {
        CloseHandle(pServer->hStopEvent);
        free(pServer->connectionSockets);
        free(pServer->connections);
        free(pServer->sessions);
        DeleteCriticalSection(&pServer->csConnections);
        DeleteCriticalSection(&pServer->csSessions);
        free(pServer);
        return NULL;
    }
    
    /* Bind socket */
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    if (ipAddr && ipAddr[0]) {
        unsigned long ip = inet_addr(ipAddr);
        if (ip == INADDR_NONE) {
            ip = htonl(INADDR_ANY);
        }
        addr.sin_addr.s_addr = ip;
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    addr.sin_port = htons(port);
    result = bind(pServer->listenSocket, (struct sockaddr*)&addr, sizeof(addr));
    if (result == SOCKET_ERROR) {
        closesocket(pServer->listenSocket);
        CloseHandle(pServer->hStopEvent);
        free(pServer->connectionSockets);
        free(pServer->connections);
        free(pServer->sessions);
        DeleteCriticalSection(&pServer->csConnections);
        DeleteCriticalSection(&pServer->csSessions);
        free(pServer);
        return NULL;
    }
    
    /* Listen for connections */
    result = listen(pServer->listenSocket, SOMAXCONN);
    if (result == SOCKET_ERROR) {
        closesocket(pServer->listenSocket);
        CloseHandle(pServer->hStopEvent);
        free(pServer->connectionSockets);
        free(pServer->connections);
        free(pServer->sessions);
        DeleteCriticalSection(&pServer->csConnections);
        DeleteCriticalSection(&pServer->csSessions);
        free(pServer);
        return NULL;
    }
    
    return pServer;
}

int Relay_Start(PRELAY_SERVER pServer)
{
    HANDLE hThread;
    DWORD threadId;
    
    if (!pServer) return RD2K_ERR_SOCKET;
    
    pServer->bRunning = TRUE;
    
    /* Create accept thread */
    hThread = CreateThread(NULL, 0, AcceptThread, (LPVOID)pServer, 0, &threadId);
    if (!hThread) {
        pServer->bRunning = FALSE;
        return RD2K_ERR_SOCKET;
    }
    
    pServer->hServerThread = hThread;
    
    /* Create session cleanup thread */
    pServer->hSessionCleanupThread = CreateThread(NULL, 0, SessionCleanupThread, 
                                                   (LPVOID)pServer, 0, &threadId);
    if (!pServer->hSessionCleanupThread) {
        RelayLog("[WARNING] Failed to create session cleanup thread\r\n");
        /* Continue anyway - sessions will just pile up */
    }
    
    return RD2K_SUCCESS;
}

void Relay_Stop(PRELAY_SERVER pServer)
{
    DWORD i;
    
    if (!pServer) return;
    
    pServer->bRunning = FALSE;
    
    /* Signal stop event */
    if (pServer->hStopEvent) {
        SetEvent(pServer->hStopEvent);
    }
    
    /* Wait for session cleanup thread */
    if (pServer->hSessionCleanupThread) {
        WaitForSingleObject(pServer->hSessionCleanupThread, 3000);
        CloseHandle(pServer->hSessionCleanupThread);
        pServer->hSessionCleanupThread = NULL;
    }
    
    /* Wait for server thread */
    if (pServer->hServerThread) {
        WaitForSingleObject(pServer->hServerThread, 5000);
        CloseHandle(pServer->hServerThread);
        pServer->hServerThread = NULL;
    }
    
    /* Disconnect all clients */
    EnterCriticalSection(&pServer->csConnections);
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i]) {
            SetEvent(pServer->connections[i]->hDisconnectEvent);
        }
    }
    LeaveCriticalSection(&pServer->csConnections);
}

void Relay_Destroy(PRELAY_SERVER pServer)
{
    DWORD i;
    
    if (!pServer) return;
    
    Relay_Stop(pServer);
    
    /* Clean up all sessions */
    for (i = 0; i < pServer->maxSessions; i++) {
        if (pServer->sessions[i]) {
            free(pServer->sessions[i]);
            pServer->sessions[i] = NULL;
        }
    }
    
    /* Clean up all connections */
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i]) {
            RemoveConnection(pServer, pServer->connections[i]);
        }
    }
    
    /* Close listening socket */
    if (pServer->listenSocket != INVALID_SOCKET) {
        closesocket(pServer->listenSocket);
    }
    
    /* Clean up resources */
    if (pServer->hStopEvent) {
        CloseHandle(pServer->hStopEvent);
    }
    
    DeleteCriticalSection(&pServer->csConnections);
    DeleteCriticalSection(&pServer->csSessions);
    
    if (pServer->connectionSockets) free(pServer->connectionSockets);
    if (pServer->connections) free(pServer->connections);
    if (pServer->sessions) free(pServer->sessions);
    
    free(pServer);
}

void Relay_GetStats(PRELAY_SERVER pServer, PRELAY_STATS pStats)
{
    if (!pServer || !pStats) return;
    
    ZeroMemory(pStats, sizeof(RELAY_STATS));
    
    EnterCriticalSection(&pServer->csConnections);
    pStats->activeConnections = pServer->activeConnections;
    LeaveCriticalSection(&pServer->csConnections);
}

/* ===== Client-side relay functions ===== */

int Relay_ConnectToServer(const char *relayServerAddr, WORD relayPort,
                         DWORD clientId, SOCKET *pRelaySocket)
{
    struct sockaddr_in addr;
    struct hostent *pHost;
    unsigned long ipAddr;
    SOCKET sock;
    
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
    
    /* Connect to relay server */
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return RD2K_ERR_CONNECT;
    }
    
    *pRelaySocket = sock;
    return RD2K_SUCCESS;
}

int Relay_Register(SOCKET relaySocket, DWORD clientId)
{
    RELAY_REGISTER_MSG msg;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    msg.clientId = clientId;
    msg.reserved = 0;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_REGISTER, (const BYTE*)&msg, sizeof(msg));
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
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    /* Wait for response */
    result = WaitForSocketReady(relaySocket, FALSE, timeoutMs);
    if (result <= 0) {
        return RD2K_ERR_TIMEOUT;
    }
    
    recvLen = recv(relaySocket, (char*)buffer, sizeof(buffer), 0);
    if (recvLen == 0 || recvLen == SOCKET_ERROR) {
        return RD2K_ERR_RECV;
    }
    
    if (recvLen < sizeof(RELAY_HEADER)) {
        return RD2K_ERR_PROTOCOL;
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    if (header.msgType != RELAY_MSG_CONNECT_RESPONSE) {
        return RD2K_ERR_PROTOCOL;
    }
    
    if (recvLen < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_RESPONSE)) {
        return RD2K_ERR_PROTOCOL;
    }
    
    CopyMemory(&response, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_RESPONSE));
    
    return response.status;
}

int Relay_SendData(SOCKET relaySocket, const BYTE *data, DWORD length)
{
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!data) return RD2K_ERR_SOCKET;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_DATA, data, length);
}

/* 
 * Helper to receive exact number of bytes, handling TCP fragmentation.
 * Used by Relay_RecvData for receiving complete relay packets.
 */
static int RecvExactRelay(SOCKET sock, BYTE *buffer, int length, int timeoutMs)
{
    int totalRecv = 0;
    int recvLen;
    int waitResult;
    int retries = 0;
    int maxRetries = (timeoutMs / 50) + 5;
    
    while (totalRecv < length) {
        waitResult = WaitForSocketReady(sock, FALSE, 50);
        
        if (waitResult < 0) {
            return -1;
        }
        
        if (waitResult == 0) {
            retries++;
            if (retries > maxRetries) {
                return totalRecv;
            }
            continue;
        }
        
        recvLen = recv(sock, (char*)(buffer + totalRecv), length - totalRecv, 0);
        
        if (recvLen == 0) {
            return 0;
        }
        
        if (recvLen == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                continue;
            }
            return -1;
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
    
    /* Wait for data */
    result = WaitForSocketReady(relaySocket, FALSE, timeoutMs);
    if (result < 0) {
        return RD2K_ERR_RECV;
    }
    if (result == 0) {
        return 0;
    }
    
    /* Receive complete header */
    recvLen = RecvExactRelay(relaySocket, headerBuf, sizeof(RELAY_HEADER), timeoutMs);
    
    if (recvLen == 0) {
        return 0;
    }
    if (recvLen < 0) {
        return RD2K_ERR_RECV;
    }
    if (recvLen < (int)sizeof(RELAY_HEADER)) {
        return 0;
    }
    
    CopyMemory(&header, headerBuf, sizeof(RELAY_HEADER));
    
    /* Handle non-DATA messages */
    if (header.msgType != RELAY_MSG_DATA) {
        /* Consume payload if present */
        if (header.dataLength > 0 && header.dataLength < bufferSize) {
            RecvExactRelay(relaySocket, buffer, header.dataLength, 1000);
        }
        
        if (header.msgType == RELAY_MSG_DISCONNECT) {
            return RD2K_ERR_DISCONNECTED;
        }
        if (header.msgType == RELAY_MSG_PARTNER_DISCONNECTED) {
            return RD2K_ERR_PARTNER_LEFT;  /* Critical: inform client their partner left! */
        }
        if (header.msgType == RELAY_MSG_PARTNER_CONNECTED) {
            return 0;  /* Partner reconnected - caller can continue */
        }
        /* PONG or other control message - ignore */
        return 0;
    }
    
    /* Validate and receive data */
    if (header.dataLength == 0) {
        return 0;
    }
    if (header.dataLength > bufferSize) {
        DWORD remaining = header.dataLength;
        while (remaining > 0) {
            DWORD chunk = (remaining > bufferSize) ? bufferSize : remaining;
            recvLen = RecvExactRelay(relaySocket, buffer, chunk, 5000);
            if (recvLen <= 0) break;
            remaining -= recvLen;
        }
        return 0;
    }
    
    recvLen = RecvExactRelay(relaySocket, buffer, header.dataLength, 30000);
    
    if (recvLen <= 0) {
        return RD2K_ERR_RECV;
    }
    
    if ((DWORD)recvLen < header.dataLength) {
        return RD2K_ERR_RECV;
    }
    
    return recvLen;
}
