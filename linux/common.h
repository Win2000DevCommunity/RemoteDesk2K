/*
 * RemoteDesk2K - Linux Relay Server
 * Platform compatibility header
 */

#ifndef _RD2K_COMMON_H_
#define _RD2K_COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

/* Windows type compatibility */
typedef uint8_t     BYTE;
typedef uint16_t    WORD;
typedef uint32_t    DWORD;
typedef int         BOOL;
typedef void*       HANDLE;

#define TRUE        1
#define FALSE       0
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)
typedef int         SOCKET;

/* Memory operations */
#define ZeroMemory(dest, len)   memset((dest), 0, (len))
#define CopyMemory(dest, src, len) memcpy((dest), (src), (len))

/* Error codes */
#define RD2K_SUCCESS            0
#define RD2K_ERR_SOCKET         (-1)
#define RD2K_ERR_CONNECT        (-2)
#define RD2K_ERR_SEND           (-3)
#define RD2K_ERR_RECV           (-4)
#define RD2K_ERR_MEMORY         (-5)
#define RD2K_ERR_TIMEOUT        (-6)
#define RD2K_ERR_SERVER_LOST    (-7)

/* Relay configuration */
#define RELAY_DEFAULT_PORT          5000
#define RELAY_MAX_CONNECTIONS       1024
#define RELAY_BUFFER_SIZE           (64 * 1024)
#define RELAY_CONNECTION_TIMEOUT    300000

/* Relay message types */
#define RELAY_MSG_REGISTER          0x50
#define RELAY_MSG_CONNECT_REQUEST   0x51
#define RELAY_MSG_CONNECT_RESPONSE  0x52
#define RELAY_MSG_DATA              0x53
#define RELAY_MSG_DISCONNECT        0x54
#define RELAY_MSG_PING              0x55
#define RELAY_MSG_PONG              0x56
#define RELAY_MSG_PARTNER_DISCONNECTED 0x57

/* Relay states */
#define RELAY_STATE_CONNECTED       0
#define RELAY_STATE_REGISTERED      1
#define RELAY_STATE_WAITING         2
#define RELAY_STATE_PAIRED          3
#define RELAY_STATE_DISCONNECTED    4

/* Disconnect reasons */
#define RELAY_DISCONNECT_NORMAL         0
#define RELAY_DISCONNECT_ERROR          1
#define RELAY_DISCONNECT_TIMEOUT        2
#define RELAY_DISCONNECT_PARTNER_LEFT   3
#define RELAY_DISCONNECT_SERVER_STOP    4

/* Packet structures - packed for network transmission */
#pragma pack(push, 1)

typedef struct {
    BYTE    msgType;
    BYTE    flags;
    WORD    reserved;
    DWORD   dataLength;
} RELAY_HEADER;

typedef struct {
    DWORD   clientId;
    DWORD   reserved;
} RELAY_REGISTER_MSG;

typedef struct {
    DWORD   partnerId;
    DWORD   password;
} RELAY_CONNECT_REQUEST;

typedef struct {
    DWORD   status;
    DWORD   reserved;
} RELAY_CONNECT_RESPONSE;

typedef struct {
    DWORD   reason;
    DWORD   partnerId;
} RELAY_PARTNER_DISCONNECTED;

#pragma pack(pop)

/* Time helper */
static inline DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#endif /* _RD2K_COMMON_H_ */
