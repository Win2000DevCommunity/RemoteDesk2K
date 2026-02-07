/*
 * RemoteDesk2K - Network Module Header
 */

#ifndef _REMOTEDESK2K_NETWORK_H_
#define _REMOTEDESK2K_NETWORK_H_

#include "common.h"

typedef struct _RD2K_NETWORK {
    SOCKET      listenSocket;
    SOCKET      socket;
    int         state;
    char        remoteHost[256];
    WORD        port;
    BYTE       *recvBuffer;
    DWORD       recvBufferSize;
    BYTE       *sendBuffer;
    DWORD       sendBufferSize;
} RD2K_NETWORK, *PRD2K_NETWORK;

PRD2K_NETWORK Network_Create(WORD port);
void Network_Destroy(PRD2K_NETWORK pNet);
int Network_Listen(PRD2K_NETWORK pNet);
SOCKET Network_Accept(PRD2K_NETWORK pNet);
int Network_Connect(PRD2K_NETWORK pNet, const char *host, WORD port);
void Network_Disconnect(PRD2K_NETWORK pNet);
int Network_Send(PRD2K_NETWORK pNet, const BYTE *data, DWORD length);
int Network_RecvExact(PRD2K_NETWORK pNet, BYTE *buffer, DWORD length);
int Network_SendPacket(PRD2K_NETWORK pNet, BYTE msgType, const BYTE *data, DWORD dataLength);
int Network_RecvPacket(PRD2K_NETWORK pNet, RD2K_HEADER *pHeader, BYTE *data, DWORD maxDataLength);
BOOL Network_DataAvailable(PRD2K_NETWORK pNet);

#endif
