/*
 * relay_client.h - Client-side Relay Connection API
 * 
 * RemoteDesk2K - Remote Desktop for Windows 2000/XP/7/10/11
 * 
 * This header declares the client-side functions for connecting
 * TO an external relay server. The relay server itself uses relay.h.
 * 
 * Used by: RemoteDesk2K.exe (client), session_manager.c
 * NOT used by: relay.exe (server)
 */

#ifndef RELAY_CLIENT_H
#define RELAY_CLIENT_H

/* common.h handles proper include order for winsock2.h/windows.h */
#include "common.h"

/* ============================================================================
 * RELAY CLIENT API
 * ============================================================================ */

/*
 * Connect to a relay server
 * 
 * @param relayServerAddr  Server hostname or IP address
 * @param relayPort        Server port (default 5002)
 * @param clientId         Our client ID to register
 * @param pRelaySocket     [out] Connected socket handle
 * @return RD2K_SUCCESS or error code
 */
int Relay_ConnectToServer(const char *relayServerAddr, WORD relayPort,
                          DWORD clientId, SOCKET *pRelaySocket);

/*
 * Register our ID with the relay server
 * 
 * @param relaySocket  Connected relay socket
 * @param clientId     Our client ID
 * @return RD2K_SUCCESS, RD2K_ERR_DUPLICATE_ID, or error code
 */
int Relay_Register(SOCKET relaySocket, DWORD clientId);

/*
 * Request connection to a partner via relay
 * 
 * @param relaySocket  Connected relay socket
 * @param partnerId    Partner's client ID
 * @param password     Password for authentication
 * @return RD2K_SUCCESS or error code
 */
int Relay_RequestPartner(SOCKET relaySocket, DWORD partnerId, DWORD password);

/*
 * Wait for partner connection response (blocking)
 * 
 * @param relaySocket  Connected relay socket
 * @param timeoutMs    Timeout in milliseconds
 * @return Connection status or error code
 */
int Relay_WaitForConnection(SOCKET relaySocket, DWORD timeoutMs);

/*
 * Send graceful disconnect to relay server
 * Must be called BEFORE closing the socket!
 * 
 * @param relaySocket  Connected relay socket
 * @return RD2K_SUCCESS or error code
 */
int Relay_SendDisconnect(SOCKET relaySocket);

/*
 * End pairing but stay REGISTERED with relay server.
 * Use this when closing the viewer but wanting to reconnect later.
 * The partner will be notified and both return to REGISTERED state.
 * 
 * @param relaySocket  Connected relay socket
 * @return RD2K_SUCCESS or error code
 */
int Relay_SendUnpair(SOCKET relaySocket);

/*
 * Check if a partner has connected to us (non-blocking)
 * Call periodically when registered but not paired.
 * 
 * @param relaySocket  Connected relay socket
 * @param pPartnerId   [out] Partner ID if connected, 0 otherwise
 * @return RD2K_SUCCESS if partner connected, 0 if no partner,
 *         RD2K_ERR_PARTNER_LEFT, RD2K_ERR_SERVER_LOST on error
 */
int Relay_CheckForPartner(SOCKET relaySocket, DWORD *pPartnerId);

/*
 * Send data through relay to partner
 * 
 * @param relaySocket  Connected relay socket
 * @param data         Data buffer to send
 * @param length       Data length in bytes
 * @return RD2K_SUCCESS or error code
 */
int Relay_SendData(SOCKET relaySocket, const BYTE *data, DWORD length);

/*
 * Receive data from relay (from partner)
 * 
 * @param relaySocket  Connected relay socket
 * @param buffer       Buffer to receive data into
 * @param bufferSize   Buffer size
 * @param timeoutMs    Timeout in milliseconds
 * @return Bytes received, 0 on timeout, negative on error
 *         Special values: RD2K_ERR_PARTNER_LEFT, RD2K_ERR_SERVER_LOST
 */
int Relay_RecvData(SOCKET relaySocket, BYTE *buffer, DWORD bufferSize, DWORD timeoutMs);

#endif /* RELAY_CLIENT_H */
