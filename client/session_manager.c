/*
 * session_manager.c - Centralized Session and Connection State Management
 * 
 * RemoteDesk2K - Remote Desktop for Windows 2000/XP/7/10/11
 * 
 * This module implements the state machine defined in session_manager.h
 * See that file for state diagrams and sequence diagrams.
 */

#include "session_manager.h"
#include "relay_client.h"
#include "network.h"
#include "filetransfer.h"
#include <stdio.h>

/* Debug logging to file */
static void SMDebugLog(const char *msg) {
#ifdef RD2K_DEBUG
    FILE *f = fopen("rd2k_debug.log", "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [SM] %s", 
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fclose(f);
    }
#else
    (void)msg;
#endif
}

/* ============================================================================
 * GLOBAL SESSION STATE
 * ============================================================================ */

static SESSION_INFO g_session = {0};
static BOOL g_bInitialized = FALSE;

/* Callbacks */
static SESSION_STATE_CALLBACK g_stateCallback = NULL;
static SESSION_ERROR_CALLBACK g_errorCallback = NULL;
static SESSION_PARTNER_CALLBACK g_partnerCallback = NULL;

/* External references to network structures (from remotedesk2k.c) */
/* These will be passed in via Session_SetNetworkStructures() */
static PRD2K_NETWORK *g_ppServerNet = NULL;
static PRD2K_NETWORK *g_ppClientNet = NULL;
static HWND g_hMainWnd = NULL;

/* Timer IDs (same as in remotedesk2k.c) */
#define TIMER_NETWORK       1
#define TIMER_SCREEN        2
#define TIMER_PING          3
#define TIMER_LISTEN_CHECK  4
#define TIMER_RELAY_CHECK   7
#define NETWORK_INTERVAL    50
#define RELAY_CHECK_INTERVAL 2000

/* ============================================================================
 * PRIVATE HELPER FUNCTIONS
 * ============================================================================ */

/*
 * Change state and notify callback
 */
static void SetState(SESSION_STATE newState)
{
    SESSION_STATE oldState;
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    g_session.state = newState;
    LeaveCriticalSection(&g_session.csSession);
    
    if (g_stateCallback && oldState != newState) {
        g_stateCallback(oldState, newState);
    }
}

/*
 * Log state transition for debugging
 */
static void LogTransition(const char *event, SESSION_STATE from, SESSION_STATE to)
{
    char buf[256];
    sprintf(buf, "[SESSION] %s: %s -> %s\n", 
            event, Session_StateToString(from), Session_StateToString(to));
#ifdef RD2K_DEBUG
    OutputDebugStringA(buf);
#endif
}

/*
 * Notify error callback
 */
static void NotifyError(DISCONNECT_REASON reason, const char *message)
{
    if (g_errorCallback) {
        g_errorCallback(reason, message);
    }
}

/*
 * Notify partner callback
 */
static void NotifyPartner(BOOL connected, DWORD partnerId, SESSION_ROLE role)
{
    if (g_partnerCallback) {
        g_partnerCallback(connected, partnerId, role);
    }
}

/*
 * Stop all session-related timers
 */
static void StopSessionTimers(void)
{
    if (g_hMainWnd) {
        KillTimer(g_hMainWnd, TIMER_NETWORK);
        KillTimer(g_hMainWnd, TIMER_PING);
        KillTimer(g_hMainWnd, TIMER_SCREEN);
        KillTimer(g_hMainWnd, TIMER_RELAY_CHECK);
    }
}

/*
 * Start timers for REGISTERED state (waiting for partner)
 */
static void StartRegisteredTimers(void)
{
    if (g_hMainWnd) {
        SetTimer(g_hMainWnd, TIMER_NETWORK, NETWORK_INTERVAL, NULL);
        SetTimer(g_hMainWnd, TIMER_RELAY_CHECK, RELAY_CHECK_INTERVAL, NULL);
    }
}

/*
 * Start timers for PAIRED state (active session)
 */
static void StartPairedTimers(void)
{
    if (g_hMainWnd) {
        SetTimer(g_hMainWnd, TIMER_NETWORK, NETWORK_INTERVAL, NULL);
        SetTimer(g_hMainWnd, TIMER_SCREEN, 100, NULL);  /* Screen updates */
        SetTimer(g_hMainWnd, TIMER_PING, 5000, NULL);   /* Keepalive to peer */
        /* CRITICAL: Keep sending relay keepalive during paired sessions!
         * Without this, relay server times out if screen is static. */
        SetTimer(g_hMainWnd, TIMER_RELAY_CHECK, RELAY_CHECK_INTERVAL, NULL);
    }
}

/*
 * Cleanup network structures but preserve relay socket if specified
 */
static void CleanupNetworkStructures(BOOL preserveRelay)
{
    /* Cancel any pending file transfer */
    FileTransfer_Cancel();
    
    if (g_ppServerNet && *g_ppServerNet) {
        PRD2K_NETWORK pNet = *g_ppServerNet;
        
        /* Detach relay socket if preserving relay connection */
        if (preserveRelay && pNet->bRelayMode && 
            pNet->relaySocket == g_session.relaySocket) {
            pNet->relaySocket = INVALID_SOCKET;
            pNet->socket = INVALID_SOCKET;
            pNet->bRelayMode = FALSE;
        }
        
        Network_Disconnect(pNet);
    }
    
    if (g_ppClientNet && *g_ppClientNet) {
        PRD2K_NETWORK pNet = *g_ppClientNet;
        
        /* Detach relay socket if preserving relay connection */
        if (preserveRelay && pNet->bRelayMode && 
            pNet->relaySocket == g_session.relaySocket) {
            pNet->relaySocket = INVALID_SOCKET;
            pNet->bRelayMode = FALSE;
        }
        
        Network_Destroy(pNet);
        *g_ppClientNet = NULL;
    }
}

/*
 * Full cleanup - close relay socket and reset everything
 */
static void FullCleanup(void)
{
    StopSessionTimers();
    CleanupNetworkStructures(FALSE);
    
    EnterCriticalSection(&g_session.csSession);
    
    if (g_session.relaySocket != INVALID_SOCKET) {
        Relay_SendDisconnect(g_session.relaySocket);
        closesocket(g_session.relaySocket);
        g_session.relaySocket = INVALID_SOCKET;
    }
    
    g_session.bConnectedToRelay = FALSE;
    g_session.bPaired = FALSE;
    g_session.bConnecting = FALSE;
    g_session.role = SESSION_ROLE_NONE;
    g_session.partnerId = 0;
    
    LeaveCriticalSection(&g_session.csSession);
    
    SetState(SESSION_STATE_DISCONNECTED);
}

/* ============================================================================
 * PUBLIC API - INITIALIZATION
 * ============================================================================ */

BOOL Session_Initialize(void)
{
    if (g_bInitialized) return TRUE;
    
    ZeroMemory(&g_session, sizeof(g_session));
    InitializeCriticalSection(&g_session.csSession);
    
    g_session.state = SESSION_STATE_DISCONNECTED;
    g_session.relaySocket = INVALID_SOCKET;
    g_session.role = SESSION_ROLE_NONE;
    
    g_bInitialized = TRUE;
    return TRUE;
}

void Session_Shutdown(void)
{
    if (!g_bInitialized) return;
    
    FullCleanup();
    DeleteCriticalSection(&g_session.csSession);
    
    g_bInitialized = FALSE;
}

/* Set external references */
void Session_SetExternals(HWND hMainWnd, PRD2K_NETWORK *ppServerNet, PRD2K_NETWORK *ppClientNet)
{
    g_hMainWnd = hMainWnd;
    g_ppServerNet = ppServerNet;
    g_ppClientNet = ppClientNet;
}

/* ============================================================================
 * PUBLIC API - STATE GETTERS
 * ============================================================================ */

SESSION_STATE Session_GetState(void)
{
    SESSION_STATE state;
    EnterCriticalSection(&g_session.csSession);
    state = g_session.state;
    LeaveCriticalSection(&g_session.csSession);
    return state;
}

SESSION_ROLE Session_GetRole(void)
{
    SESSION_ROLE role;
    EnterCriticalSection(&g_session.csSession);
    role = g_session.role;
    LeaveCriticalSection(&g_session.csSession);
    return role;
}

BOOL Session_IsConnected(void)
{
    BOOL connected;
    EnterCriticalSection(&g_session.csSession);
    connected = g_session.bConnectedToRelay;
    LeaveCriticalSection(&g_session.csSession);
    return connected;
}

BOOL Session_IsPaired(void)
{
    BOOL paired;
    EnterCriticalSection(&g_session.csSession);
    paired = g_session.bPaired;
    LeaveCriticalSection(&g_session.csSession);
    return paired;
}

DWORD Session_GetMyId(void)
{
    DWORD id;
    EnterCriticalSection(&g_session.csSession);
    id = g_session.myId;
    LeaveCriticalSection(&g_session.csSession);
    return id;
}

DWORD Session_GetPartnerId(void)
{
    DWORD id;
    EnterCriticalSection(&g_session.csSession);
    id = g_session.partnerId;
    LeaveCriticalSection(&g_session.csSession);
    return id;
}

SOCKET Session_GetRelaySocket(void)
{
    SOCKET sock;
    EnterCriticalSection(&g_session.csSession);
    sock = g_session.relaySocket;
    LeaveCriticalSection(&g_session.csSession);
    return sock;
}

/* ============================================================================
 * PUBLIC API - USER ACTIONS
 * ============================================================================ */

int Session_ConnectToRelay(const char *serverAddr, WORD port, DWORD clientId)
{
    SOCKET sock = INVALID_SOCKET;
    int result;
    
    if (!g_bInitialized) return RD2K_ERR_SOCKET;
    
    EnterCriticalSection(&g_session.csSession);
    
    /* Can only connect from DISCONNECTED state */
    if (g_session.state != SESSION_STATE_DISCONNECTED) {
        LeaveCriticalSection(&g_session.csSession);
        return RD2K_ERR_CONNECT;
    }
    
    g_session.state = SESSION_STATE_CONNECTING_RELAY;
    g_session.bConnecting = TRUE;
    g_session.myId = clientId;
    
    LeaveCriticalSection(&g_session.csSession);
    
    LogTransition("ConnectToRelay", SESSION_STATE_DISCONNECTED, SESSION_STATE_CONNECTING_RELAY);
    
    /* Connect to relay server */
    result = Relay_ConnectToServer(serverAddr, port, clientId, &sock);
    if (result != RD2K_SUCCESS) {
        NotifyError(DISCONNECT_REASON_NETWORK_ERROR, "Failed to connect to relay server");
        FullCleanup();
        return result;
    }
    
    /* Register with relay */
    result = Relay_Register(sock, clientId);
    if (result != RD2K_SUCCESS) {
        closesocket(sock);
        
        if (result == RD2K_ERR_DUPLICATE_ID) {
            NotifyError(DISCONNECT_REASON_UNKNOWN, "ID already in use on this server");
        } else {
            NotifyError(DISCONNECT_REASON_NETWORK_ERROR, "Failed to register with relay");
        }
        
        FullCleanup();
        return result;
    }
    
    /* Success! */
    EnterCriticalSection(&g_session.csSession);
    g_session.relaySocket = sock;
    g_session.bConnectedToRelay = TRUE;
    g_session.bConnecting = FALSE;
    g_session.state = SESSION_STATE_REGISTERED;
    LeaveCriticalSection(&g_session.csSession);
    
    LogTransition("RelayRegistered", SESSION_STATE_CONNECTING_RELAY, SESSION_STATE_REGISTERED);
    
    /* Start timers to poll for partner connections */
    StartRegisteredTimers();
    
    if (g_stateCallback) {
        g_stateCallback(SESSION_STATE_CONNECTING_RELAY, SESSION_STATE_REGISTERED);
    }
    
    return RD2K_SUCCESS;
}

int Session_DisconnectFromRelay(void)
{
    SESSION_STATE oldState;
    
    if (!g_bInitialized) return RD2K_ERR_SOCKET;
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    LeaveCriticalSection(&g_session.csSession);
    
    if (oldState == SESSION_STATE_DISCONNECTED) {
        return RD2K_SUCCESS;  /* Already disconnected */
    }
    
    LogTransition("UserDisconnect", oldState, SESSION_STATE_DISCONNECTED);
    
    FullCleanup();
    
    return RD2K_SUCCESS;
}

int Session_DisconnectFromPartner(void)
{
    SESSION_STATE oldState;
    
    if (!g_bInitialized) return RD2K_ERR_SOCKET;
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    LeaveCriticalSection(&g_session.csSession);
    
    /* Only makes sense if PAIRED */
    if (oldState != SESSION_STATE_PAIRED) {
        return RD2K_SUCCESS;
    }
    
    LogTransition("UserDisconnectPartner", SESSION_STATE_PAIRED, SESSION_STATE_REGISTERED);
    
    /* Stop session timers */
    StopSessionTimers();
    
    /* Cleanup network but preserve relay */
    CleanupNetworkStructures(TRUE);
    
    /* Update state */
    EnterCriticalSection(&g_session.csSession);
    g_session.bPaired = FALSE;
    g_session.role = SESSION_ROLE_NONE;
    g_session.partnerId = 0;
    g_session.state = SESSION_STATE_REGISTERED;
    LeaveCriticalSection(&g_session.csSession);
    
    /* Restart timers for REGISTERED state */
    StartRegisteredTimers();
    
    NotifyPartner(FALSE, 0, SESSION_ROLE_NONE);
    
    if (g_stateCallback) {
        g_stateCallback(SESSION_STATE_PAIRED, SESSION_STATE_REGISTERED);
    }
    
    return RD2K_SUCCESS;
}

/* ============================================================================
 * PUBLIC API - NETWORK EVENT HANDLERS
 * ============================================================================ */

/*
 * Called by old code when relay connection is established.
 * This syncs the session_manager's internal state with the relay socket.
 */
void Session_OnRelayConnected(SOCKET relaySocket)
{
    EnterCriticalSection(&g_session.csSession);
    
    g_session.relaySocket = relaySocket;
    g_session.state = SESSION_STATE_CONNECTING_RELAY;
    
    LeaveCriticalSection(&g_session.csSession);
    
    LogTransition("RelayConnected", SESSION_STATE_DISCONNECTED, SESSION_STATE_CONNECTING_RELAY);
}

/*
 * Called by old code when successfully registered with relay.
 * This completes the connection process and enables partner waiting.
 */
void Session_OnRelayRegistered(DWORD assignedId)
{
    EnterCriticalSection(&g_session.csSession);
    
    g_session.myId = assignedId;
    g_session.bConnectedToRelay = TRUE;
    g_session.bConnecting = FALSE;
    g_session.state = SESSION_STATE_REGISTERED;
    
    LeaveCriticalSection(&g_session.csSession);
    
    LogTransition("RelayRegistered", SESSION_STATE_CONNECTING_RELAY, SESSION_STATE_REGISTERED);
    
    /* Start timers to poll for partner connections */
    StartRegisteredTimers();
    
    if (g_stateCallback) {
        g_stateCallback(SESSION_STATE_CONNECTING_RELAY, SESSION_STATE_REGISTERED);
    }
}

void Session_OnPartnerConnected(DWORD partnerId)
{
    SESSION_STATE oldState;
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    
    if (oldState == SESSION_STATE_REGISTERED) {
        g_session.partnerId = partnerId;
        g_session.state = SESSION_STATE_PAIRING;
        g_session.role = SESSION_ROLE_SERVER;  /* Someone connected TO us */
    }
    
    LeaveCriticalSection(&g_session.csSession);
    
    if (oldState == SESSION_STATE_REGISTERED) {
        LogTransition("PartnerConnected", SESSION_STATE_REGISTERED, SESSION_STATE_PAIRING);
        
        if (g_stateCallback) {
            g_stateCallback(SESSION_STATE_REGISTERED, SESSION_STATE_PAIRING);
        }
    }
}

void Session_OnPartnerPaired(BOOL isServerRole)
{
    SESSION_STATE oldState;
    BOOL stateChanged = FALSE;
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    
    /* Allow transition from PAIRING (server mode) or REGISTERED (viewer mode).
     * Viewer goes directly REGISTERED → PAIRED because it initiated the connection. */
    if (oldState == SESSION_STATE_PAIRING || oldState == SESSION_STATE_REGISTERED) {
        g_session.bPaired = TRUE;
        g_session.role = isServerRole ? SESSION_ROLE_SERVER : SESSION_ROLE_CLIENT;
        g_session.sessionStartTime = GetTickCount();
        g_session.lastActivityTime = GetTickCount();
        g_session.state = SESSION_STATE_PAIRED;
        stateChanged = TRUE;
    }
    
    LeaveCriticalSection(&g_session.csSession);
    
    if (stateChanged) {
        LogTransition("PartnerPaired", oldState, SESSION_STATE_PAIRED);
        
        /* Start session timers */
        StartPairedTimers();
        
        NotifyPartner(TRUE, g_session.partnerId, g_session.role);
        
        if (g_stateCallback) {
            g_stateCallback(oldState, SESSION_STATE_PAIRED);
        }
    }
}

void Session_OnPartnerLeft(DISCONNECT_REASON reason)
{
    SESSION_STATE oldState;
    BOOL wasConnectedToRelay;
    char dbgBuf[128];
    
    sprintf(dbgBuf, "Session_OnPartnerLeft called, reason=%d\n", reason);
    SMDebugLog(dbgBuf);
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    
    /* Guard: Don't process if already in REGISTERED or DISCONNECTED state.
     * This can happen when both MSG_DISCONNECT and RELAY_MSG_PARTNER_DISCONNECTED
     * arrive close together - both would try to call this function. */
    if (oldState == SESSION_STATE_REGISTERED || oldState == SESSION_STATE_DISCONNECTED) {
        sprintf(dbgBuf, "PartnerLeft: IGNORED - already in state %d\n", oldState);
        SMDebugLog(dbgBuf);
        LeaveCriticalSection(&g_session.csSession);
        return;
    }
    
    wasConnectedToRelay = g_session.bConnectedToRelay && 
                          g_session.relaySocket != INVALID_SOCKET;
    LeaveCriticalSection(&g_session.csSession);
    
    LogTransition("PartnerLeft", oldState, 
                  wasConnectedToRelay ? SESSION_STATE_REGISTERED : SESSION_STATE_DISCONNECTED);
    
    /* Stop session timers FIRST to prevent re-entry */
    StopSessionTimers();
    
    /* Cleanup network but preserve relay if still connected */
    CleanupNetworkStructures(wasConnectedToRelay);
    
    /* Notify partner callback */
    NotifyPartner(FALSE, g_session.partnerId, SESSION_ROLE_NONE);
    
    /* Update internal state */
    EnterCriticalSection(&g_session.csSession);
    g_session.bPaired = FALSE;
    g_session.role = SESSION_ROLE_NONE;
    g_session.partnerId = 0;
    
    if (wasConnectedToRelay) {
        /* Return to REGISTERED - can accept new connections */
        g_session.state = SESSION_STATE_REGISTERED;
        LeaveCriticalSection(&g_session.csSession);
        
        SMDebugLog("PartnerLeft: wasConnectedToRelay=TRUE, going to REGISTERED\n");
        
        /* Call state callback FIRST - this updates UI flags like g_bClientConnected */
        if (g_stateCallback) {
            g_stateCallback(oldState, SESSION_STATE_REGISTERED);
        }
        
        /* Restart timers AFTER state callback has updated flags */
        StartRegisteredTimers();
        
        /* Notify error callback LAST - MessageBox blocks but flags are already updated */
        NotifyError(reason, Session_ReasonToString(reason));
    } else {
        /* Relay connection also lost - go to DISCONNECTED */
        g_session.state = SESSION_STATE_DISCONNECTED;
        g_session.bConnectedToRelay = FALSE;
        
        SMDebugLog("PartnerLeft: wasConnectedToRelay=FALSE, going to DISCONNECTED\n");
        
        if (g_session.relaySocket != INVALID_SOCKET) {
            closesocket(g_session.relaySocket);
            g_session.relaySocket = INVALID_SOCKET;
        }
        
        LeaveCriticalSection(&g_session.csSession);
        
        /* Call state callback FIRST - this updates UI flags */
        if (g_stateCallback) {
            g_stateCallback(oldState, SESSION_STATE_DISCONNECTED);
        }
        
        /* Notify error callback LAST - MessageBox blocks but flags are already updated */
        NotifyError(reason, Session_ReasonToString(reason));
    }
}

void Session_OnServerLost(void)
{
    SESSION_STATE oldState;
    
    EnterCriticalSection(&g_session.csSession);
    oldState = g_session.state;
    
    /* Guard: Don't process if already DISCONNECTED */
    if (oldState == SESSION_STATE_DISCONNECTED) {
        SMDebugLog("ServerLost: IGNORED - already DISCONNECTED\n");
        LeaveCriticalSection(&g_session.csSession);
        return;
    }
    LeaveCriticalSection(&g_session.csSession);
    
    LogTransition("ServerLost", oldState, SESSION_STATE_DISCONNECTED);
    
    /* Do full cleanup FIRST - stops timers, updates state, calls state callback */
    FullCleanup();
    
    /* Notify error LAST - MessageBox blocks but state is already updated */
    NotifyError(DISCONNECT_REASON_SERVER_LOST, "Connection to relay server lost");
}

void Session_OnNetworkError(int errorCode)
{
    char msg[128];
    sprintf(msg, "Network error: %d", errorCode);
    
    /* Treat network error as partner left if we're paired,
     * or server lost if we're just registered.
     * These functions will call NotifyError internally. */
    if (g_session.state == SESSION_STATE_PAIRED) {
        Session_OnPartnerLeft(DISCONNECT_REASON_NETWORK_ERROR);
    } else {
        Session_OnServerLost();
    }
}

/* ============================================================================
 * PUBLIC API - POLLING
 * ============================================================================ */

int Session_CheckForEvents(void)
{
    SESSION_STATE state;
    DWORD partnerId = 0;
    int result;
    
    EnterCriticalSection(&g_session.csSession);
    state = g_session.state;
    LeaveCriticalSection(&g_session.csSession);
    
    /* Only poll when REGISTERED (waiting for partner) */
    if (state != SESSION_STATE_REGISTERED) {
        return 0;
    }
    
    if (g_session.relaySocket == INVALID_SOCKET) {
        return 0;
    }
    
    /* Check for PARTNER_CONNECTED notification */
    result = Relay_CheckForPartner(g_session.relaySocket, &partnerId);
    
    if (result == RD2K_SUCCESS && partnerId != 0) {
        Session_OnPartnerConnected(partnerId);
        return 1;  /* Event occurred */
    }
    
    if (result == RD2K_ERR_PARTNER_LEFT) {
        /* Unexpected - we weren't paired. Might be stale message. */
        return 0;
    }
    
    if (result == RD2K_ERR_SERVER_LOST) {
        Session_OnServerLost();
        return -1;  /* Error */
    }
    
    return 0;  /* No event */
}

/* ============================================================================
 * PUBLIC API - CALLBACKS
 * ============================================================================ */

void Session_SetStateCallback(SESSION_STATE_CALLBACK callback)
{
    g_stateCallback = callback;
}

void Session_SetErrorCallback(SESSION_ERROR_CALLBACK callback)
{
    g_errorCallback = callback;
}

void Session_SetPartnerCallback(SESSION_PARTNER_CALLBACK callback)
{
    g_partnerCallback = callback;
}

/* ============================================================================
 * PUBLIC API - UTILITIES
 * ============================================================================ */

const char* Session_StateToString(SESSION_STATE state)
{
    switch (state) {
        case SESSION_STATE_DISCONNECTED:      return "DISCONNECTED";
        case SESSION_STATE_CONNECTING_RELAY:  return "CONNECTING_RELAY";
        case SESSION_STATE_REGISTERED:        return "REGISTERED";
        case SESSION_STATE_PAIRING:           return "PAIRING";
        case SESSION_STATE_PAIRED:            return "PAIRED";
        case SESSION_STATE_CONNECTING_DIRECT: return "CONNECTING_DIRECT";
        case SESSION_STATE_CONNECTED_DIRECT:  return "CONNECTED_DIRECT";
        default:                              return "UNKNOWN";
    }
}

const char* Session_ReasonToString(DISCONNECT_REASON reason)
{
    switch (reason) {
        case DISCONNECT_REASON_USER_REQUEST:   return "User requested disconnect";
        case DISCONNECT_REASON_PARTNER_LEFT:   return "Partner disconnected";
        case DISCONNECT_REASON_PARTNER_TIMEOUT: return "Partner inactive (timeout)";
        case DISCONNECT_REASON_SERVER_LOST:    return "Relay server connection lost";
        case DISCONNECT_REASON_NETWORK_ERROR:  return "Network error";
        case DISCONNECT_REASON_AUTH_FAILED:    return "Authentication failed";
        case DISCONNECT_REASON_UNKNOWN:        return "Unknown reason";
        default:                               return "Unknown";
    }
}

/* ============================================================================
 * ACTIVITY TRACKING
 * ============================================================================ */

void Session_UpdateActivity(void)
{
    EnterCriticalSection(&g_session.csSession);
    g_session.lastActivityTime = GetTickCount();
    LeaveCriticalSection(&g_session.csSession);
}

DWORD Session_GetIdleTime(void)
{
    DWORD idle;
    EnterCriticalSection(&g_session.csSession);
    idle = GetTickCount() - g_session.lastActivityTime;
    LeaveCriticalSection(&g_session.csSession);
    return idle;
}
