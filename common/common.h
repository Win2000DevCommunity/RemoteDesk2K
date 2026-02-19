/*
 * RemoteDesk2K - Remote Desktop Application for Windows 2000
 * Common Header - ID encodes IP address for direct P2P connection
 */

#ifndef _REMOTEDESK2K_COMMON_H_
#define _REMOTEDESK2K_COMMON_H_


// Use DDK w2k headers for strict compatibility
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Always include winsock2.h before windows.h to prevent winsock.h conflicts
#include <winsock2.h> // from 3790.1830/inc/w2k
#include <ws2tcpip.h>
#include <windows.h>   // from 3790.1830/inc/w2k
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto.h"

/* Windows 2000 compatibility - define missing constants */
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL           0x020A
#endif

#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wParam)  ((short)HIWORD(wParam))
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

/* Application Version */
#define RD2K_VERSION_MAJOR      1
#define RD2K_VERSION_MINOR      0
#define RD2K_VERSION_STRING     "1.0.0"
#define RD2K_APP_NAME           "RemoteDesk2K"

/* Network Configuration */
#define RD2K_LISTEN_PORT        5901
#define RD2K_MAX_PACKET_SIZE    (256 * 1024)
#define RD2K_BUFFER_SIZE        (4 * 1024 * 1024)
#define RD2K_FILE_CHUNK_SIZE    (32 * 1024)

/* Connection Modes */
#define RD2K_MODE_DIRECT        0   /* Direct P2P connection (LAN) */
#define RD2K_MODE_RELAY         1   /* Relay server connection (Internet) */

/* Relay Server Configuration (when using relay mode) */
#define RD2K_RELAY_PORT         5900
#define RD2K_RELAY_TIMEOUT      30000   /* 30 seconds connect timeout */

/* Protocol Message Types */
#define MSG_SCREEN_UPDATE       0x01
#define MSG_SCREEN_REQUEST      0x02
#define MSG_MOUSE_EVENT         0x03
#define MSG_KEYBOARD_EVENT      0x04
#define MSG_CLIPBOARD_TEXT      0x05
#define MSG_CLIPBOARD_REQ       0x16
#define MSG_PING                0x06
#define MSG_PONG                0x07
#define MSG_HANDSHAKE           0x08
#define MSG_HANDSHAKE_ACK       0x09
#define MSG_DISCONNECT          0x0A
#define MSG_SCREEN_INFO         0x0B
#define MSG_FULL_SCREEN_REQ     0x0C
#define MSG_FILE_START          0x10
#define MSG_FILE_DATA           0x11
#define MSG_FILE_END            0x12
#define MSG_FILE_CANCEL         0x13
#define MSG_FILE_ACK            0x14  /* Chunk acknowledgment for flow control */
#define MSG_CLIPBOARD_FILES     0x15
#define MSG_FILE_NONE           0x16  /* Response when no files in clipboard */
#define MSG_FILE_REQ            0x1A  /* Request file transfer from remote (for Ctrl+C on remote) */
#define MSG_FOLDER_START        0x17  /* Begin folder transfer */
#define MSG_FOLDER_ENTRY        0x18  /* Folder entry (file or subdir info) */
#define MSG_FOLDER_END          0x19  /* End folder transfer */
#define MSG_AUTH_REQUEST        0x20
#define MSG_AUTH_RESPONSE       0x21

/* Compression Types */
#define COMPRESS_NONE           0x00
#define COMPRESS_RLE            0x01

/* Connection States */
#define STATE_DISCONNECTED      0
#define STATE_LISTENING         1
#define STATE_CONNECTING        2
#define STATE_HANDSHAKE         3
#define STATE_CONNECTED         4

/* Display Modes */
#define DISPLAY_NORMAL          0
#define DISPLAY_STRETCH         1
#define DISPLAY_FULLSCREEN      2

/* Error Codes */
#define RD2K_SUCCESS            0
#define RD2K_ERR_SOCKET         -1
#define RD2K_ERR_CONNECT        -2
#define RD2K_ERR_SEND           -3
#define RD2K_ERR_RECV           -4
#define RD2K_ERR_TIMEOUT        -9  /* ACK timeout */
#define RD2K_ERR_MEMORY         -5
#define RD2K_ERR_SCREEN         -6
#define RD2K_ERR_PROTOCOL       -7
#define RD2K_ERR_AUTH           -8
#define RD2K_ERR_DISCONNECTED   -10 /* Connection disconnected */
#define RD2K_ERR_PARTNER_LEFT   -11 /* Partner disconnected from relay */
#define RD2K_ERR_SERVER_LOST    -12 /* Relay server connection lost */
#define RD2K_ERR_DUPLICATE_ID   -13 /* Your ID is already connected to server */

/* Reconnection settings */
#define RECONNECT_MAX_ATTEMPTS  5
#define RECONNECT_DELAY_MS      2000  /* 2 seconds between attempts */

#pragma pack(push, 1)

/* Packet Header */
typedef struct _RD2K_HEADER {
    BYTE    msgType;
    BYTE    flags;
    WORD    reserved;
    DWORD   dataLength;
    DWORD   checksum;
} RD2K_HEADER, *PRD2K_HEADER;

/* Handshake Message */
typedef struct _RD2K_HANDSHAKE {
    DWORD   magic;
    DWORD   yourId;
    DWORD   password;
    WORD    screenWidth;
    WORD    screenHeight;
    BYTE    colorDepth;
    BYTE    compression;
    WORD    versionMajor;
    WORD    versionMinor;
} RD2K_HANDSHAKE, *PRD2K_HANDSHAKE;

/* Screen Info */
typedef struct _RD2K_SCREEN_INFO {
    WORD    width;
    WORD    height;
    BYTE    bitsPerPixel;
    BYTE    compression;
    WORD    reserved;
} RD2K_SCREEN_INFO, *PRD2K_SCREEN_INFO;

/* Screen Update Rectangle */
typedef struct _RD2K_RECT {
    WORD    x;
    WORD    y;
    WORD    width;
    WORD    height;
    BYTE    encoding;
    BYTE    reserved;
    DWORD   dataSize;
} RD2K_RECT, *PRD2K_RECT;

/* Mouse Event */
typedef struct _RD2K_MOUSE_EVENT {
    WORD    x;
    WORD    y;
    BYTE    buttons;
    BYTE    flags;
    SHORT   wheelDelta;
} RD2K_MOUSE_EVENT, *PRD2K_MOUSE_EVENT;

/* Keyboard Event */
typedef struct _RD2K_KEY_EVENT {
    WORD    virtualKey;
    WORD    scanCode;
    BYTE    flags;
    BYTE    reserved[3];
} RD2K_KEY_EVENT, *PRD2K_KEY_EVENT;

/* File Transfer Header - supports 64-bit file sizes for up to 100GB files */
typedef struct _RD2K_FILE_HEADER {
    char    fileName[260];
    DWORD   fileSizeHigh;   /* High 32 bits of 64-bit file size */
    DWORD   fileSizeLow;    /* Low 32 bits of 64-bit file size */
    DWORD   fileCount;
    DWORD   totalChunks;    /* Pre-calculated total chunk count */
} RD2K_FILE_HEADER, *PRD2K_FILE_HEADER;

/* File Data Chunk */
typedef struct _RD2K_FILE_CHUNK {
    DWORD   chunkIndex;
    DWORD   chunkSize;
} RD2K_FILE_CHUNK, *PRD2K_FILE_CHUNK;

/* File Chunk Acknowledgment - sent by receiver after N chunks for flow control */
typedef struct _RD2K_FILE_ACK {
    DWORD   chunkIndex;     /* Index of last chunk received */
    DWORD   status;         /* 0 = success, non-zero = error code */
} RD2K_FILE_ACK, *PRD2K_FILE_ACK;

/* Folder Entry - for folder transfer support */
typedef struct _RD2K_FOLDER_ENTRY {
    char    relativePath[MAX_PATH];  /* Path relative to root folder */
    DWORD   attributes;              /* FILE_ATTRIBUTE_* flags */
    DWORD   fileSizeHigh;            /* High 32 bits (0 for directories) */
    DWORD   fileSizeLow;             /* Low 32 bits (0 for directories) */
    FILETIME lastWriteTime;          /* Preserve timestamps */
} RD2K_FOLDER_ENTRY, *PRD2K_FOLDER_ENTRY;

/* Folder Transfer Header */
typedef struct _RD2K_FOLDER_HEADER {
    char    folderName[260];         /* Root folder name */
    DWORD   totalFiles;              /* Total file count */
    DWORD   totalFolders;            /* Total subfolder count */
    DWORD   totalSizeHigh;           /* High 32 bits of total size */
    DWORD   totalSizeLow;            /* Low 32 bits of total size */
} RD2K_FOLDER_HEADER, *PRD2K_FOLDER_HEADER;

/* Clipboard Text Header */
typedef struct _RD2K_CLIPBOARD {
    DWORD   dataLength;
    BYTE    isFile;
    BYTE    reserved[3];
} RD2K_CLIPBOARD, *PRD2K_CLIPBOARD;

#pragma pack(pop)

/* Magic number for protocol */
#define RD2K_MAGIC              0x4B324452

/* Utility macros */
#define SAFE_FREE(p)            if(p) { free(p); (p) = NULL; }
#define SAFE_CLOSE_SOCKET(s)    if((s) != INVALID_SOCKET) { closesocket(s); (s) = INVALID_SOCKET; }
#define SAFE_CLOSE_HANDLE(h)    if((h) && (h) != INVALID_HANDLE_VALUE) { CloseHandle(h); (h) = NULL; }

/* Calculate checksum */
static __inline DWORD CalculateChecksum(const BYTE *data, DWORD length)
{
    DWORD checksum = 0;
    DWORD i;
    for (i = 0; i < length; i++) {
        checksum = ((checksum << 5) + checksum) + data[i];
    }
    return checksum;
}

/*
 * ID System: Direct IP encoding for P2P connection
 * The ID IS the IP address stored as a 32-bit value
 * Display format: XXX XXX XXX XXX (12 digits, 3 per octet)
 * Example: IP 192.168.1.100 displays as "192 168 001 100"
 */

/* Get local IP address (first non-loopback interface) */
static __inline DWORD GetLocalIPAddress(void)
{
    char hostname[256];
    struct hostent *pHost;
    struct in_addr addr;
    
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        pHost = gethostbyname(hostname);
        if (pHost && pHost->h_addr_list[0]) {
            int i;
            /* Find first non-loopback address */
            for (i = 0; pHost->h_addr_list[i]; i++) {
                memcpy(&addr, pHost->h_addr_list[i], sizeof(addr));
                if ((ntohl(addr.s_addr) & 0xFF000000) != 0x7F000000) {
                    return ntohl(addr.s_addr);
                }
            }
            /* Fallback to first address */
            memcpy(&addr, pHost->h_addr_list[0], sizeof(addr));
            return ntohl(addr.s_addr);
        }
    }
    return 0x7F000001; /* 127.0.0.1 */
}

/* Generate ID from local IP - ID is the ENCRYPTED IP address */
static __inline DWORD GenerateUniqueId(void)
{
    DWORD ip = GetLocalIPAddress();
    /* Encrypt IP to create the ID shown to user */
    return Crypto_EncryptIP(ip);
}

/* Validate if decrypted IP is usable (not 0.0.0.0, not 127.x.x.x, etc) */
static __inline BOOL IsValidDecryptedIP(DWORD ip)
{
    BYTE a = (BYTE)((ip >> 24) & 0xFF);
    
    /* Check for invalid IPs */
    if (ip == 0) return FALSE;                    /* 0.0.0.0 */
    if (ip == 0xFFFFFFFF) return FALSE;           /* 255.255.255.255 */
    if (a == 127) return FALSE;                   /* 127.x.x.x (loopback) */
    if (a == 0) return FALSE;                     /* 0.x.x.x */
    if (a >= 224 && a <= 239) return FALSE;       /* Multicast range */
    
    return TRUE;
}

/* Convert encrypted ID back to IP string for connection 
   Returns TRUE if valid, FALSE if invalid ID */
static __inline BOOL IdToIPStringEx(DWORD encryptedId, char *ipStr, BOOL *pValid)
{
    /* Decrypt ID to get original IP */
    DWORD ip = Crypto_DecryptIP(encryptedId);
    BYTE a = (BYTE)((ip >> 24) & 0xFF);
    BYTE b = (BYTE)((ip >> 16) & 0xFF);
    BYTE c = (BYTE)((ip >> 8) & 0xFF);
    BYTE d = (BYTE)(ip & 0xFF);
    
    sprintf(ipStr, "%d.%d.%d.%d", a, b, c, d);
    
    if (pValid) {
        *pValid = IsValidDecryptedIP(ip);
    }
    return IsValidDecryptedIP(ip);
}

/* Convert encrypted ID back to IP string for connection (legacy) */
static __inline void IdToIPString(DWORD encryptedId, char *ipStr)
{
    IdToIPStringEx(encryptedId, ipStr, NULL);
}

/* Generate random password (5 digits) */
static __inline DWORD GeneratePassword(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    srand((unsigned int)(counter.LowPart ^ GetTickCount()));
    return 10000 + (rand() % 90000);
}

/* Format ID for display: XXX XXX XXX XXX (each octet as 3 digits) */
static __inline void FormatId(DWORD id, char *buffer)
{
    BYTE a = (BYTE)((id >> 24) & 0xFF);
    BYTE b = (BYTE)((id >> 16) & 0xFF);
    BYTE c = (BYTE)((id >> 8) & 0xFF);
    BYTE d = (BYTE)(id & 0xFF);
    sprintf(buffer, "%03d %03d %03d %03d", a, b, c, d);
}

/* Parse ID from display string (remove spaces, parse 4 groups of 3 digits) */
static __inline DWORD ParseId(const char *str)
{
    int octets[4] = {0, 0, 0, 0};
    int octetIndex = 0;
    int digitCount = 0;
    
    while (*str && octetIndex < 4) {
        if (*str >= '0' && *str <= '9') {
            octets[octetIndex] = octets[octetIndex] * 10 + (*str - '0');
            digitCount++;
            /* After 3 digits, move to next octet */
            if (digitCount == 3) {
                octetIndex++;
                digitCount = 0;
            }
        } else if (*str == ' ' || *str == '.' || *str == '-') {
            /* Separator - if we have digits, move to next octet */
            if (digitCount > 0) {
                octetIndex++;
                digitCount = 0;
            }
        }
        str++;
    }
    
    /* Handle last octet if not moved */
    if (digitCount > 0 && octetIndex < 4) {
        octetIndex++;
    }
    
    /* Clamp values to 0-255 */
    if (octets[0] > 255) octets[0] = 255;
    if (octets[1] > 255) octets[1] = 255;
    if (octets[2] > 255) octets[2] = 255;
    if (octets[3] > 255) octets[3] = 255;
    
    /* Build 32-bit IP from octets */
    return ((DWORD)octets[0] << 24) | 
           ((DWORD)octets[1] << 16) | 
           ((DWORD)octets[2] << 8) | 
           (DWORD)octets[3];
}

/* Initialize Winsock */
static __inline int InitializeNetwork(void)
{
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

/* Cleanup Winsock */
static __inline void CleanupNetwork(void)
{
    WSACleanup();
}

#endif /* _REMOTEDESK2K_COMMON_H_ */
