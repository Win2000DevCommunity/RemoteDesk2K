/*
 * session_manager.h - Centralized Session and Connection State Management
 * 
 * RemoteDesk2K - Remote Desktop for Windows 2000/XP/7/10/11
 * 
 * This module centralizes all connection state management, handling both
 * direct connections and relay-based connections with a unified state machine.
 * 
 * ============================================================================
 * STATE MACHINE DIAGRAM (États du système)
 * ============================================================================
 * 
 *                          +-------------------+
 *                          |   DISCONNECTED    |<------------------+
 *                          | (Initial State)   |                   |
 *                          +--------+----------+                   |
 *                                   |                              |
 *                    User clicks    |                              |
 *                  "Connect Server" |                              |
 *                                   v                              |
 *                     +-------------+-------------+                |
 *                     |   CONNECTING_RELAY       |                |
 *                     | (Connecting to server)   |                |
 *                     +-------------+-------------+                |
 *                                   |                              |
 *              Success              |          Failure             |
 *        +-------------------------+-------------------+          |
 *        |                                             |          |
 *        v                                             v          |
 *   +----+-------------+                  +------------+----+     |
 *   |    REGISTERED    |                  |    ERROR        |-----+
 *   | (Waiting partner)|<----+            | (Show message)  |
 *   +----+-------------+     |            +-----------------+
 *        |                   |
 *        | Partner connects  | Partner leaves (stays on relay)
 *        | or user connects  |
 *        v                   |
 *   +----+-------------+     |
 *   |    PAIRING       |     |
 *   | (Handshake)      |     |
 *   +----+-------------+     |
 *        |                   |
 *        | Auth success      |
 *        v                   |
 *   +----+-------------+     |
 *   |    PAIRED        |-----+
 *   | (Active session) |
 *   +----+-------------+
 *        |
 *        | Session timeout / Server lost
 *        v
 *   +----+-------------+
 *   |   DISCONNECTED   |
 *   +------------------+
 * 
 * ============================================================================
 * SEQUENCE DIAGRAMS (Diagrammes de séquence)
 * ============================================================================
 * 
 * SCENARIO 1: Normal Connection via Relay (Client A connects to Client B)
 * ------------------------------------------------------------------------
 * 
 *   Client A          Relay Server         Client B
 *      |                   |                   |
 *      |--REGISTER-------->|                   |
 *      |<--REGISTER_OK-----|                   |
 *      |                   |<--REGISTER--------|
 *      |                   |---REGISTER_OK---->|
 *      |                   |                   |
 *      |--CONNECT(B_ID)--->|                   |
 *      |<--CONNECT_OK------|---PARTNER_CONN--->|
 *      |                   |                   |
 *      |========== DATA (via relay) ==========>|
 *      |<========= DATA (via relay) ===========|
 * 
 * 
 * SCENARIO 2: Partner Disconnects Gracefully
 * -------------------------------------------
 * 
 *   Client A          Relay Server         Client B
 *      |                   |                   |
 *      |<======= PAIRED SESSION =======>      |
 *      |                   |                   |
 *      |                   |<--DISCONNECT------|  (B clicks disconnect)
 *      |<--PARTNER_LEFT----|                   |
 *      |                   |                   |
 *      | (A returns to     |   (B fully       |
 *      |  REGISTERED)      |    disconnected)  |
 *      |                   |                   |
 *      | Can accept new    |                   |
 *      | connections       |                   |
 * 
 * 
 * SCENARIO 3: Partner Disappears (Unexpected disconnect / Timeout)
 * -----------------------------------------------------------------
 * 
 *   Client A          Relay Server         Client B
 *      |                   |                   |
 *      |<======= PAIRED SESSION =======>      |
 *      |                   |                   |
 *      |                   |        X--------- B crashes/network dies
 *      |                   |                   |
 *      |                   | (Server detects   |
 *      |                   |  inactivity)      |
 *      |                   |                   |
 *      |<--PARTNER_LEFT----|                   |
 *      |                   |                   |
 *      | (A returns to REGISTERED)             |
 * 
 * 
 * SCENARIO 4: Relay Server Lost (Network issue)
 * ----------------------------------------------
 * 
 *   Client A          Relay Server         Client B
 *      |                   |                   |
 *      |<======= PAIRED SESSION =======>      |
 *      |                   |                   |
 *      |         X-------- Server crashes      |
 *      |                   |                   |
 *      | (Socket error)    |                   |
 *      |                   |                   |
 *      | (A goes to DISCONNECTED)              |
 *      | (Must reconnect from scratch)         |
 * 
 * 
 * SCENARIO 5: User Manually Disconnects
 * --------------------------------------
 * 
 *   Client A          Relay Server         Client B
 *      |                   |                   |
 *      |<======= PAIRED SESSION =======>      |
 *      |                   |                   |
 *      | User clicks       |                   |
 *      | "Disconnect"      |                   |
 *      |                   |                   |
 *      |--DISCONNECT------>|---PARTNER_LEFT--->|
 *      |                   |                   |
 *      | (A goes to        | (B returns to     |
 *      |  DISCONNECTED)    |  REGISTERED)      |
 * 
 * ============================================================================
 * TRANSITION TABLE (Table de transition)
 * ============================================================================
 * 
 * Current State    | Event                  | Action                 | Next State
 * -----------------+------------------------+------------------------+---------------
 * DISCONNECTED     | ConnectToRelay()      | Connect socket         | CONNECTING_RELAY
 * CONNECTING_RELAY | Register success      | Update UI, start timer | REGISTERED
 * CONNECTING_RELAY | Register fail/timeout | Show error             | DISCONNECTED
 * REGISTERED       | PartnerConnected      | Setup session          | PAIRING
 * REGISTERED       | ConnectToPartner()    | Send CONNECT request   | PAIRING  
 * REGISTERED       | UserDisconnect        | Send DISCONNECT        | DISCONNECTED
 * REGISTERED       | ServerLost            | Close socket           | DISCONNECTED
 * PAIRING          | AuthSuccess           | Start data exchange    | PAIRED
 * PAIRING          | AuthFail/Timeout      | Cleanup                | REGISTERED
 * PAIRED           | PartnerLeft           | Cleanup session        | REGISTERED
 * PAIRED           | ServerLost            | Full cleanup           | DISCONNECTED
 * PAIRED           | SessionTimeout        | Notify, cleanup        | REGISTERED
 * PAIRED           | UserDisconnect        | Send DISCONNECT        | DISCONNECTED
 * ANY              | FatalError            | Full cleanup           | DISCONNECTED
 * 
 * ============================================================================
 */

#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

/* common.h handles proper include order for winsock2.h/windows.h */
#include "common.h"

/* Forward declaration for network.h types (avoid circular include) */
struct _RD2K_NETWORK;
typedef struct _RD2K_NETWORK *PRD2K_NETWORK;

/* ============================================================================
 * SESSION STATES (États de session)
 * ============================================================================ */

typedef enum {
    SESSION_STATE_DISCONNECTED = 0,   /* Not connected to anything */
    SESSION_STATE_CONNECTING_RELAY,   /* Connecting to relay server */
    SESSION_STATE_REGISTERED,         /* Registered with relay, waiting for partner */
    SESSION_STATE_PAIRING,            /* Partner found, doing handshake */
    SESSION_STATE_PAIRED,             /* Active session with partner */
    SESSION_STATE_CONNECTING_DIRECT,  /* Direct connection in progress */
    SESSION_STATE_CONNECTED_DIRECT    /* Direct connection active (no relay) */
} SESSION_STATE;

/* ============================================================================
 * SESSION EVENTS (Événements de session)
 * ============================================================================ */

typedef enum {
    /* User-initiated events */
    SESSION_EVENT_USER_CONNECT_RELAY,     /* User clicked "Connect to Server" */
    SESSION_EVENT_USER_CONNECT_PARTNER,   /* User clicked "Connect to Partner" */
    SESSION_EVENT_USER_DISCONNECT,        /* User clicked "Disconnect" */
    SESSION_EVENT_USER_CONNECT_DIRECT,    /* User doing direct IP connection */
    
    /* Network events */
    SESSION_EVENT_RELAY_CONNECTED,        /* Successfully connected to relay */
    SESSION_EVENT_RELAY_REGISTERED,       /* Successfully registered with relay */
    SESSION_EVENT_PARTNER_CONNECTED,      /* Partner connected (we're server role) */
    SESSION_EVENT_PARTNER_PAIRED,         /* Handshake completed successfully */
    SESSION_EVENT_PARTNER_LEFT,           /* Partner disconnected (graceful or timeout) */
    SESSION_EVENT_SERVER_LOST,            /* Relay server connection lost */
    SESSION_EVENT_SESSION_TIMEOUT,        /* No data for too long */
    SESSION_EVENT_DIRECT_CONNECTED,       /* Direct connection established */
    
    /* Error events */
    SESSION_EVENT_CONNECT_FAILED,         /* Connection attempt failed */
    SESSION_EVENT_AUTH_FAILED,            /* Password/authentication failed */
    SESSION_EVENT_NETWORK_ERROR,          /* Generic network error */
    SESSION_EVENT_DUPLICATE_ID            /* Our ID already registered */
} SESSION_EVENT;

/* ============================================================================
 * DISCONNECT REASONS (Raisons de déconnexion)
 * ============================================================================ */

typedef enum {
    DISCONNECT_REASON_USER_REQUEST,       /* User clicked disconnect */
    DISCONNECT_REASON_PARTNER_LEFT,       /* Partner disconnected normally */
    DISCONNECT_REASON_PARTNER_TIMEOUT,    /* Partner inactive too long */
    DISCONNECT_REASON_SERVER_LOST,        /* Relay server connection lost */
    DISCONNECT_REASON_NETWORK_ERROR,      /* Network failure */
    DISCONNECT_REASON_AUTH_FAILED,        /* Authentication failed */
    DISCONNECT_REASON_UNKNOWN             /* Unknown/unexpected disconnect */
} DISCONNECT_REASON;

/* ============================================================================
 * SESSION ROLE (Rôle dans la session)
 * ============================================================================ */

typedef enum {
    SESSION_ROLE_NONE = 0,                /* No active session */
    SESSION_ROLE_SERVER,                  /* Allowing control (someone connected to us) */
    SESSION_ROLE_CLIENT                   /* Controlling (we connected to someone) */
} SESSION_ROLE;

/* ============================================================================
 * SESSION INFO STRUCTURE (Structure d'information de session)
 * ============================================================================ */

typedef struct {
    SESSION_STATE state;                  /* Current session state */
    SESSION_ROLE role;                    /* Our role in current session */
    
    /* Relay connection */
    SOCKET relaySocket;                   /* Socket to relay server */
    BOOL bConnectedToRelay;               /* TRUE if registered with relay */
    DWORD myId;                           /* Our ID on relay */
    DWORD partnerId;                      /* Partner's ID (if paired) */
    
    /* Direct connection */
    BOOL bDirectMode;                     /* TRUE if direct connection (no relay) */
    
    /* Session flags */
    BOOL bConnecting;                     /* TRUE while connection in progress */
    BOOL bPaired;                         /* TRUE if actively paired with partner */
    
    /* Statistics */
    DWORD sessionStartTime;               /* When session started */
    DWORD lastActivityTime;               /* Last data sent/received */
    
    /* Thread safety */
    CRITICAL_SECTION csSession;           /* Critical section for state access */
} SESSION_INFO;

/* ============================================================================
 * CALLBACK TYPES (Types de callback)
 * ============================================================================ */

/* Called when state changes - UI should update */
typedef void (*SESSION_STATE_CALLBACK)(SESSION_STATE oldState, SESSION_STATE newState);

/* Called when error occurs */
typedef void (*SESSION_ERROR_CALLBACK)(DISCONNECT_REASON reason, const char *message);

/* Called when partner connects/disconnects */
typedef void (*SESSION_PARTNER_CALLBACK)(BOOL connected, DWORD partnerId, SESSION_ROLE role);

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/* Initialize/cleanup */
BOOL Session_Initialize(void);
void Session_Shutdown(void);

/* Get current state (thread-safe) */
SESSION_STATE Session_GetState(void);
SESSION_ROLE Session_GetRole(void);
BOOL Session_IsConnected(void);
BOOL Session_IsPaired(void);
DWORD Session_GetMyId(void);
DWORD Session_GetPartnerId(void);
SOCKET Session_GetRelaySocket(void);

/* User actions - these trigger state transitions */
int Session_ConnectToRelay(const char *serverAddr, WORD port, DWORD clientId);
int Session_ConnectToPartner(DWORD partnerId, DWORD password);
int Session_DisconnectFromPartner(void);
int Session_DisconnectFromRelay(void);
int Session_ConnectDirect(const char *ipAddr, WORD port, DWORD password);

/* Network event handlers - called by network code when events occur */
void Session_OnRelayConnected(SOCKET relaySocket);
void Session_OnRelayRegistered(DWORD assignedId);
void Session_OnPartnerConnected(DWORD partnerId);
void Session_OnPartnerPaired(BOOL isServerRole);
void Session_OnPartnerLeft(DISCONNECT_REASON reason);
void Session_OnServerLost(void);
void Session_OnNetworkError(int errorCode);

/* Polling for events - call periodically */
int Session_CheckForEvents(void);

/* Register callbacks */
void Session_SetStateCallback(SESSION_STATE_CALLBACK callback);
void Session_SetErrorCallback(SESSION_ERROR_CALLBACK callback);
void Session_SetPartnerCallback(SESSION_PARTNER_CALLBACK callback);

/* Utility */
const char* Session_StateToString(SESSION_STATE state);
const char* Session_ReasonToString(DISCONNECT_REASON reason);

/* Set external references (call after Initialize) */
void Session_SetExternals(HWND hMainWnd, PRD2K_NETWORK *ppServerNet, PRD2K_NETWORK *ppClientNet);

/* Activity tracking */
void Session_UpdateActivity(void);
DWORD Session_GetIdleTime(void);

#endif /* SESSION_MANAGER_H */
