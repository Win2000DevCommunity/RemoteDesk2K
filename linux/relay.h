/*
 * relay.h - RemoteDesk2K Linux Relay Server Header
 */

#ifndef _RELAY_H_
#define _RELAY_H_

#include "common.h"

/* Forward declaration */
typedef struct _RELAY_SERVER RELAY_SERVER;

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/*
 * Create a relay server instance
 * port: Port to listen on
 * ipAddr: IP address to bind to (NULL or "0.0.0.0" for all interfaces)
 * Returns: Server instance or NULL on failure
 */
RELAY_SERVER* Relay_Create(WORD port, const char* ipAddr);

/*
 * Start the relay server
 * Returns: RD2K_SUCCESS or RD2K_ERROR
 */
int Relay_Start(RELAY_SERVER *pServer);

/*
 * Stop the relay server gracefully
 */
void Relay_Stop(RELAY_SERVER *pServer);

/*
 * Destroy and free a relay server instance
 */
void Relay_Destroy(RELAY_SERVER *pServer);

/*
 * Set log callback for receiving log messages
 */
void Relay_SetLogCallback(void (*callback)(const char*));

/*
 * Get current server statistics
 */
void Relay_GetStats(RELAY_SERVER *pServer, DWORD *activeConnections);

#endif /* _RELAY_H_ */