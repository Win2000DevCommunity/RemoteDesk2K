/*
 * RemoteDesk2K - Relay Server Header
 * Windows 2000 compatible relay server for Internet connectivity
 * Allows two clients behind NAT to connect through a central relay server
 */


#ifndef _REMOTEDESK2K_RELAY_H_
#define _REMOTEDESK2K_RELAY_H_

// Always include the central project header first
#include "common.h"

/* Relay server configuration */
#define RELAY_DEFAULT_PORT          5900
#define RELAY_MAX_CONNECTIONS       1024
#define RELAY_CONNECTION_TIMEOUT    300000  /* 5 minutes in milliseconds */
#define RELAY_BUFFER_SIZE           (64 * 1024)  /* 64 KB per connection */
#define RELAY_QUEUE_SIZE            16

/* Relay protocol message types */
#define RELAY_MSG_REGISTER          0x50    /* Register with relay server (send your ID) */
#define RELAY_MSG_CONNECT_REQUEST   0x51    /* Request connection to partner (send partner ID) */
#define RELAY_MSG_CONNECT_RESPONSE  0x52    /* Relay response: accepted/rejected */
#define RELAY_MSG_DATA              0x53    /* Tunneled data from partner */
#define RELAY_MSG_DISCONNECT        0x54    /* Graceful disconnect */
#define RELAY_MSG_PING              0x55    /* Keep-alive ping */
#define RELAY_MSG_PONG              0x56    /* Keep-alive response */
#define RELAY_MSG_PARTNER_DISCONNECTED 0x57 /* Partner has disconnected */

/* Relay protocol header for relay-specific messages */
#pragma pack(push, 1)

typedef struct _RELAY_HEADER {
    BYTE    msgType;        /* RELAY_MSG_* */
    BYTE    flags;          /* Reserved */
    WORD    reserved;
    DWORD   dataLength;
} RELAY_HEADER, *PRELAY_HEADER;

/* Register message: send your ID to relay server */
typedef struct _RELAY_REGISTER_MSG {
    DWORD   clientId;       /* Your encrypted ID */
    DWORD   reserved;
} RELAY_REGISTER_MSG, *PRELAY_REGISTER_MSG;

/* Connect request: ask relay to connect you to partner */
typedef struct _RELAY_CONNECT_REQUEST {
    DWORD   partnerId;      /* Partner's encrypted ID */
    DWORD   password;       /* Password for partner verification */
} RELAY_CONNECT_REQUEST, *PRELAY_CONNECT_REQUEST;

/* Connect response from relay server */
typedef struct _RELAY_CONNECT_RESPONSE {
    DWORD   status;         /* 0 = accepted, non-zero = error code */
    DWORD   reserved;
} RELAY_CONNECT_RESPONSE, *PRELAY_CONNECT_RESPONSE;

/* Disconnect reason codes */
#define RELAY_DISCONNECT_NORMAL         0   /* Normal graceful disconnect */
#define RELAY_DISCONNECT_ERROR          1   /* Connection error */
#define RELAY_DISCONNECT_TIMEOUT        2   /* Connection timeout */
#define RELAY_DISCONNECT_PARTNER_LEFT   3   /* Partner disconnected */
#define RELAY_DISCONNECT_SERVER_STOP    4   /* Relay server stopping */

/* Partner disconnected notification */
typedef struct _RELAY_PARTNER_DISCONNECTED {
    DWORD   reason;         /* RELAY_DISCONNECT_* */
    DWORD   partnerId;      /* ID of partner that disconnected */
} RELAY_PARTNER_DISCONNECTED, *PRELAY_PARTNER_DISCONNECTED;

#pragma pack(pop)

/* Relay connection state machine */
#define RELAY_STATE_CONNECTED       0    /* Socket connected to relay */
#define RELAY_STATE_REGISTERED      1    /* Registered (ID known) */
#define RELAY_STATE_WAITING         2    /* Waiting for partner */
#define RELAY_STATE_PAIRED          3    /* Connected to partner */
#define RELAY_STATE_DISCONNECTED    4    /* Disconnected */

/* Relay server handle (opaque) */
typedef struct _RELAY_SERVER* PRELAY_SERVER;

/* Relay connection state (for relay server internal use) */
typedef struct _RELAY_CONNECTION {
    SOCKET              socket;             /* Client socket */
    DWORD               clientId;           /* Registered ID */
    DWORD               state;              /* RELAY_STATE_* */
    HANDLE              hThread;            /* Worker thread handle */
    HANDLE              hDisconnectEvent;   /* Signal to stop worker thread */
    struct _RELAY_CONNECTION* pPartner;     /* Paired connection (if any) */
    struct _RELAY_SERVER* pServer;          /* Parent server reference */
    BYTE*               recvBuffer;         /* Receive buffer */
    DWORD               recvBufferSize;
    DWORD               recvPos;            /* Current position in buffer */
    DWORD               lastActivity;       /* Timestamp of last activity */
} RELAY_CONNECTION, *PRELAY_CONNECTION;

/* Relay server statistics (optional) */
typedef struct _RELAY_STATS {
    DWORD               totalConnections;   /* Cumulative connections handled */
    DWORD               activeConnections;  /* Currently active */
    DWORD               successfulPairs;    /* Successful partner connections */
    DWORD               failedConnections;  /* Connection failures */
} RELAY_STATS, *PRELAY_STATS;

/* ===== Relay Server API ===== */

/* Initialize relay server on specified port and IP */
PRELAY_SERVER Relay_Create(WORD port, const char* ipAddr);

/* Start relay server (blocking or threaded) */
int Relay_Start(PRELAY_SERVER pRelay);

/* Stop relay server gracefully */
void Relay_Stop(PRELAY_SERVER pRelay);

/* Destroy relay server and cleanup */
void Relay_Destroy(PRELAY_SERVER pRelay);

/* Get relay server statistics */
void Relay_GetStats(PRELAY_SERVER pRelay, PRELAY_STATS pStats);

/* ===== Logging Callback ===== */

/* Logging callback function type */
typedef void (*RELAY_LOG_CALLBACK)(const char* message);

/* Set logging callback for console output
   - pfnCallback: Function to call with log messages
   Pass NULL to disable logging */
void Relay_SetLogCallback(RELAY_LOG_CALLBACK pfnCallback);

/* ===== Client-side Relay Connection ===== */

/* Connect to relay server instead of direct P2P
   - relayServerAddr: IP or hostname of relay server
   - relayPort: Port relay is listening on (default 5900)
   - clientId: Your encrypted ID
   - Returns: 0 = success, non-zero = error code */
int Relay_ConnectToServer(const char *relayServerAddr, WORD relayPort, 
                          DWORD clientId, SOCKET *pRelaySocket);

/* Register with relay server (after connecting)
   - relaySocket: Socket connected to relay
   - clientId: Your encrypted ID
   Returns: 0 = success, non-zero = error */
int Relay_Register(SOCKET relaySocket, DWORD clientId);

/* Request connection to partner through relay
   - relaySocket: Socket connected to relay
   - partnerId: Target partner's encrypted ID
   - password: Connection password
   Returns: 0 = success (waiting for partner), non-zero = error */
int Relay_RequestPartner(SOCKET relaySocket, DWORD partnerId, DWORD password);

/* Wait for relay connection response
   - relaySocket: Socket connected to relay
   - timeoutMs: Timeout in milliseconds
   Returns: 0 = connected, non-zero = error or timeout */
int Relay_WaitForConnection(SOCKET relaySocket, DWORD timeoutMs);

/* Send data through relay tunnel
   - relaySocket: Socket to relay server
   - data: Pointer to data to send
   - length: Length of data
   Returns: bytes sent or negative error code */
int Relay_SendData(SOCKET relaySocket, const BYTE *data, DWORD length);

/* Check if relay connection is still alive
   - relaySocket: Socket to relay server
   Returns: RD2K_SUCCESS = connected, RD2K_ERR_SERVER_LOST = disconnected */
int Relay_CheckConnection(SOCKET relaySocket);

/* Receive data through relay tunnel (non-blocking or timeout)
   - relaySocket: Socket to relay server
   - buffer: Buffer to receive data
   - bufferSize: Size of buffer
   - timeoutMs: Timeout (0 = non-blocking)
   Returns: bytes received (0 = timeout), negative = error */
int Relay_RecvData(SOCKET relaySocket, BYTE *buffer, DWORD bufferSize, DWORD timeoutMs);

#endif /* _REMOTEDESK2K_RELAY_H_ */
