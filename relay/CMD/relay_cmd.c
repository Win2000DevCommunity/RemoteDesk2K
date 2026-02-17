/*
 * RemoteDesk2K Relay Server - Command Line Version
 * Professional console-based relay server for Windows 2000+
 * 
 * (C) 2026 RemoteDesk2K Project
 * 
 * Features:
 * - Pure console application (no GUI dependencies)
 * - Command line arguments for configuration
 * - Color-coded log output
 * - Graceful Ctrl+C shutdown handling
 * - Server ID generation (same as GUI version)
 * - Public IP auto-detection via OpenDNS
 * - Windows 2000 compatible
 * 
 * Usage: relay_cmd.exe [options]
 *   -p, --port PORT     Listen port (default: 5000)
 *   -i, --ip IP         Bind IP address (default: 0.0.0.0)
 *   -h, --help          Show this help message
 *   -v, --version       Show version
 *   -q, --quiet         Suppress banner
 */

#include "relay.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Version info */
#define RELAY_CMD_VERSION   "1.0.0"
#define RELAY_CMD_NAME      "RemoteDesk2K Relay Server"
#define RELAY_CMD_YEAR      "2026"

/* Default configuration */
#define DEFAULT_PORT        5000
#define DEFAULT_IP          "0.0.0.0"

/* Console colors for Windows */
#define CON_RESET           7       /* Default white on black */
#define CON_RED             12      /* Red */
#define CON_GREEN           10      /* Green */
#define CON_YELLOW          14      /* Yellow */
#define CON_CYAN            11      /* Cyan */
#define CON_WHITE           15      /* Bright white */
#define CON_GRAY            8       /* Gray */

/* Global state */
static PRELAY_SERVER g_pServer = NULL;
static BOOL g_bRunning = FALSE;
static HANDLE g_hConsole = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_csConsole;

/* Forward declarations */
static void PrintBanner(void);
static void PrintHelp(const char *progName);
static void PrintVersion(void);
static void SetConsoleColor(WORD color);
static void LogMessage(const char *prefix, WORD prefixColor, const char *message);
static void RelayLogCallback(const char *message);
static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType);
static void ParseLogMessage(const char *message, char *prefix, char *content);
static BOOL GetPublicIPviaOpenDNS(char *publicIP, int bufLen);

/* ========================================================================== 
 * DNS STRUCTURES FOR PUBLIC IP DETECTION
 * ========================================================================== */

#pragma pack(push, 1)
typedef struct _DNS_HEADER {
    USHORT id;
    USHORT flags;
    USHORT qdcount;
    USHORT ancount;
    USHORT nscount;
    USHORT arcount;
} DNS_HEADER;
#pragma pack(pop)

/* Get public IP address using OpenDNS lookup
 * Returns TRUE on success, FALSE on failure
 * publicIP buffer should be at least 16 bytes */
static BOOL GetPublicIPviaOpenDNS(char *publicIP, int bufLen) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in dnsServer;
    BYTE queryBuf[512];
    BYTE responseBuf[512];
    int queryLen = 0;
    int recvLen = 0;
    DNS_HEADER *dnsHeader;
    BYTE *ptr;
    fd_set readfds;
    struct timeval tv;
    USHORT rdlength;
    
    /* DNS server: resolver1.opendns.com = 208.67.222.222 */
    const char *DNS_SERVER_IP = "208.67.222.222";
    const WORD DNS_PORT = 53;
    
    /* Query for: myip.opendns.com (returns your public IP) */
    /* DNS question: 4myip7opendns3com0 (length-prefixed labels) */
    static const BYTE dnsQuestion[] = {
        4, 'm', 'y', 'i', 'p',           /* "myip" */
        7, 'o', 'p', 'e', 'n', 'd', 'n', 's',  /* "opendns" */
        3, 'c', 'o', 'm',                 /* "com" */
        0,                                /* End of name */
        0, 1,                             /* Type: A (IPv4) */
        0, 1                              /* Class: IN (Internet) */
    };
    
    if (!publicIP || bufLen < 16) return FALSE;
    publicIP[0] = '\0';
    
    /* Create UDP socket */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return FALSE;
    }
    
    /* Set timeout */
    tv.tv_sec = 3;  /* 3 second timeout */
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    
    /* Setup DNS server address */
    memset(&dnsServer, 0, sizeof(dnsServer));
    dnsServer.sin_family = AF_INET;
    dnsServer.sin_port = htons(DNS_PORT);
    dnsServer.sin_addr.s_addr = inet_addr(DNS_SERVER_IP);
    
    /* Build DNS query packet */
    memset(queryBuf, 0, sizeof(queryBuf));
    dnsHeader = (DNS_HEADER*)queryBuf;
    dnsHeader->id = htons(0x1234);      /* Transaction ID */
    dnsHeader->flags = htons(0x0100);   /* Standard query, recursion desired */
    dnsHeader->qdcount = htons(1);      /* 1 question */
    dnsHeader->ancount = 0;
    dnsHeader->nscount = 0;
    dnsHeader->arcount = 0;
    
    /* Copy question after header */
    queryLen = sizeof(DNS_HEADER);
    memcpy(queryBuf + queryLen, dnsQuestion, sizeof(dnsQuestion));
    queryLen += sizeof(dnsQuestion);
    
    /* Send DNS query */
    if (sendto(sock, (const char*)queryBuf, queryLen, 0,
               (struct sockaddr*)&dnsServer, sizeof(dnsServer)) == SOCKET_ERROR) {
        closesocket(sock);
        return FALSE;
    }
    
    /* Wait for response with select */
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    
    if (select(0, &readfds, NULL, NULL, &tv) <= 0) {
        closesocket(sock);
        return FALSE;
    }
    
    /* Receive response */
    recvLen = recv(sock, (char*)responseBuf, sizeof(responseBuf), 0);
    closesocket(sock);
    
    if (recvLen <= (int)(sizeof(DNS_HEADER) + sizeof(dnsQuestion))) {
        return FALSE;
    }
    
    /* Parse response */
    dnsHeader = (DNS_HEADER*)responseBuf;
    
    /* Check for successful response */
    if ((ntohs(dnsHeader->flags) & 0x8000) == 0) {
        return FALSE;  /* Not a response */
    }
    if ((ntohs(dnsHeader->flags) & 0x000F) != 0) {
        return FALSE;  /* Error in response */
    }
    if (ntohs(dnsHeader->ancount) == 0) {
        return FALSE;  /* No answers */
    }
    
    /* Skip header and question section to find answer */
    ptr = responseBuf + sizeof(DNS_HEADER);
    
    /* Skip question (same as query) */
    while (*ptr != 0 && ptr < responseBuf + recvLen) {
        ptr += (*ptr) + 1;  /* Skip label */
    }
    ptr += 1 + 4;  /* Skip null terminator, type and class */
    
    /* Now at answer section */
    /* Answer format: name(2 bytes compressed), type(2), class(2), ttl(4), rdlength(2), rdata(4 for A record) */
    
    /* Skip name (could be compressed pointer 0xC0XX or labels) */
    if ((*ptr & 0xC0) == 0xC0) {
        ptr += 2;  /* Compressed pointer */
    } else {
        while (*ptr != 0 && ptr < responseBuf + recvLen) {
            ptr += (*ptr) + 1;
        }
        ptr++;
    }
    
    /* Check we have enough data for answer record */
    if (ptr + 10 > responseBuf + recvLen) {
        return FALSE;
    }
    
    /* Check type is A (0x0001) */
    if (ptr[0] != 0 || ptr[1] != 1) {
        return FALSE;  /* Not type A */
    }
    ptr += 2;  /* Skip type */
    ptr += 2;  /* Skip class */
    ptr += 4;  /* Skip TTL */
    
    /* Get rdlength */
    rdlength = (USHORT)((ptr[0] << 8) | ptr[1]);
    ptr += 2;
    
    if (rdlength != 4 || ptr + 4 > responseBuf + recvLen) {
        return FALSE;  /* Invalid A record */
    }
    
    /* Extract IP address */
    _snprintf(publicIP, bufLen, "%d.%d.%d.%d", ptr[0], ptr[1], ptr[2], ptr[3]);
    
    return TRUE;
}

/* ========================================================================== 
 * CONSOLE OUTPUT FUNCTIONS
 * ========================================================================== */

static void SetConsoleColor(WORD color)
{
    if (g_hConsole != INVALID_HANDLE_VALUE) {
        SetConsoleTextAttribute(g_hConsole, color);
    }
}

static void PrintBanner(void)
{
    printf("\n");
    SetConsoleColor(CON_CYAN);
    printf("  =====================================================\n");
    printf("     %s v%s\n", RELAY_CMD_NAME, RELAY_CMD_VERSION);
    printf("     (C) %s RemoteDesk2K Project\n", RELAY_CMD_YEAR);
    printf("  =====================================================\n");
    SetConsoleColor(CON_RESET);
    printf("\n");
}

static void PrintHelp(const char *progName)
{
    printf("\n");
    SetConsoleColor(CON_WHITE);
    printf("Usage: %s [options]\n\n", progName);
    SetConsoleColor(CON_CYAN);
    printf("Options:\n");
    SetConsoleColor(CON_RESET);
    printf("  -p, --port PORT     Listen port (default: %d)\n", DEFAULT_PORT);
    printf("  -i, --ip IP         Bind IP address (default: %s)\n", DEFAULT_IP);
    printf("  -h, --help          Show this help message\n");
    printf("  -v, --version       Show version\n");
    printf("  -q, --quiet         Suppress banner\n");
    printf("\n");
    SetConsoleColor(CON_CYAN);
    printf("Examples:\n");
    SetConsoleColor(CON_RESET);
    printf("  %s                      # Start on default port 5000\n", progName);
    printf("  %s -p 5900              # Start on port 5900\n", progName);
    printf("  %s -i 192.168.1.10      # Bind to specific IP\n", progName);
    printf("  %s -p 5000 -i 0.0.0.0   # Full configuration\n", progName);
    printf("\n");
    SetConsoleColor(CON_YELLOW);
    printf("Press Ctrl+C to stop the server gracefully.\n");
    SetConsoleColor(CON_RESET);
    printf("\n");
}

static void PrintVersion(void)
{
    printf("%s v%s\n", RELAY_CMD_NAME, RELAY_CMD_VERSION);
}

/* Parse log message to extract prefix like [INFO], [ERROR], etc. */
static void ParseLogMessage(const char *message, char *prefix, char *content)
{
    const char *p = message;
    int prefixLen = 0;
    
    /* Initialize outputs */
    prefix[0] = '\0';
    content[0] = '\0';
    
    /* Look for [PREFIX] pattern */
    if (*p == '[') {
        const char *end = strchr(p, ']');
        if (end && (end - p) < 20) {
            prefixLen = (int)(end - p + 1);
            strncpy(prefix, p, prefixLen);
            prefix[prefixLen] = '\0';
            
            /* Skip whitespace after prefix */
            p = end + 1;
            while (*p == ' ') p++;
        }
    }
    
    /* Rest is content */
    strcpy(content, p);
    
    /* Remove trailing \r\n */
    {
        int len = (int)strlen(content);
        while (len > 0 && (content[len-1] == '\r' || content[len-1] == '\n')) {
            content[--len] = '\0';
        }
    }
}

/* Log with colored prefix */
static void LogMessage(const char *prefix, WORD prefixColor, const char *message)
{
    SYSTEMTIME st;
    
    EnterCriticalSection(&g_csConsole);
    
    GetLocalTime(&st);
    
    /* Timestamp in gray */
    SetConsoleColor(CON_GRAY);
    printf("[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    
    /* Colored prefix */
    if (prefix && prefix[0]) {
        SetConsoleColor(prefixColor);
        printf("%-12s ", prefix);
    }
    
    /* Message in default */
    SetConsoleColor(CON_RESET);
    printf("%s\n", message);
    
    LeaveCriticalSection(&g_csConsole);
}

/* Relay log callback - thread safe console output */
static void RelayLogCallback(const char *message)
{
    char prefix[32];
    char content[512];
    WORD color = CON_RESET;
    
    if (!message || !message[0]) return;
    
    ParseLogMessage(message, prefix, content);
    
    /* Determine color based on prefix */
    if (strstr(prefix, "ERROR") || strstr(prefix, "REJECT")) {
        color = CON_RED;
    } else if (strstr(prefix, "WARN") || strstr(prefix, "CLEANUP")) {
        color = CON_YELLOW;
    } else if (strstr(prefix, "CONNECT") || strstr(prefix, "REGISTER")) {
        color = CON_GREEN;
    } else if (strstr(prefix, "DISCONNECT") || strstr(prefix, "NOTIFY")) {
        color = CON_CYAN;
    } else if (strstr(prefix, "INFO") || strstr(prefix, "PROTECT")) {
        color = CON_WHITE;
    }
    
    LogMessage(prefix, color, content);
}

/* ========================================================================== 
 * SIGNAL HANDLING
 * ========================================================================== */

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            printf("\n");
            LogMessage("[SHUTDOWN]", CON_YELLOW, "Received shutdown signal...");
            g_bRunning = FALSE;
            
            /* Stop the relay server */
            if (g_pServer) {
                Relay_Stop(g_pServer);
            }
            return TRUE;
    }
    return FALSE;
}

/* ========================================================================== 
 * MAIN ENTRY POINT
 * ========================================================================== */

int main(int argc, char *argv[])
{
    WORD port = DEFAULT_PORT;
    char ipAddr[64];
    BOOL bQuiet = FALSE;
    WSADATA wsaData;
    int i;
    int result;
    
    /* Initialize */
    strcpy(ipAddr, DEFAULT_IP);
    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    InitializeCriticalSection(&g_csConsole);
    
    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintBanner();
            PrintHelp(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            PrintVersion();
            return 0;
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            bQuiet = TRUE;
        }
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = (WORD)atoi(argv[++i]);
            if (port == 0 || port > 65535) {
                fprintf(stderr, "Error: Invalid port number\n");
                return 1;
            }
        }
        else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--ip") == 0) && i + 1 < argc) {
            strncpy(ipAddr, argv[++i], sizeof(ipAddr) - 1);
            ipAddr[sizeof(ipAddr) - 1] = '\0';
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            fprintf(stderr, "Use -h for help.\n");
            return 1;
        }
    }
    
    /* Show banner unless quiet mode */
    if (!bQuiet) {
        PrintBanner();
    }
    
    /* Initialize Winsock */
    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LogMessage("[ERROR]", CON_RED, "Failed to initialize Winsock");
        return 1;
    }
    
    /* Set up signal handler */
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    
    /* Set up relay logging */
    Relay_SetLogCallback(RelayLogCallback);
    
    /* Create relay server */
    {
        char msg[128];
        _snprintf(msg, sizeof(msg), "Starting server on %s:%d...", ipAddr, port);
        LogMessage("[INFO]", CON_WHITE, msg);
    }
    
    g_pServer = Relay_Create(port, ipAddr);
    if (!g_pServer) {
        LogMessage("[ERROR]", CON_RED, "Failed to create relay server");
        LogMessage("[ERROR]", CON_RED, "Check if port is already in use");
        WSACleanup();
        return 1;
    }
    
    /* Start the server */
    result = Relay_Start(g_pServer);
    if (result != RD2K_SUCCESS) {
        LogMessage("[ERROR]", CON_RED, "Failed to start relay server");
        Relay_Destroy(g_pServer);
        WSACleanup();
        return 1;
    }
    
    g_bRunning = TRUE;
    
    {
        char msg[128];
        _snprintf(msg, sizeof(msg), 
                 "Relay server is running on port %d", port);
        LogMessage("[OK]", CON_GREEN, msg);
        LogMessage("[INFO]", CON_WHITE, "Press Ctrl+C to stop the server");
    }
    
    /* Generate Server ID (same as GUI version) */
    {
        char serverIdIP[64];
        char serverId[SERVER_ID_MAX_LEN];
        
        strncpy(serverIdIP, ipAddr, sizeof(serverIdIP) - 1);
        serverIdIP[sizeof(serverIdIP) - 1] = '\0';
        
        /* If bound to 0.0.0.0, detect public IP for Server ID */
        if (strcmp(serverIdIP, "0.0.0.0") == 0) {
            char publicIP[64];
            
            LogMessage("[INFO]", CON_WHITE, "Detecting public IP via OpenDNS...");
            
            if (GetPublicIPviaOpenDNS(publicIP, sizeof(publicIP)) && publicIP[0] != '\0') {
                char msg[128];
                strncpy(serverIdIP, publicIP, sizeof(serverIdIP) - 1);
                _snprintf(msg, sizeof(msg), "Public IP detected: %s", publicIP);
                LogMessage("[INFO]", CON_WHITE, msg);
            } else {
                /* Fallback to local hostname resolution */
                char hostname[256];
                struct hostent *he;
                LogMessage("[WARN]", CON_YELLOW, "OpenDNS lookup failed, using local IP...");
                if (gethostname(hostname, sizeof(hostname)) == 0) {
                    he = gethostbyname(hostname);
                    if (he && he->h_addr_list[0]) {
                        struct in_addr addr;
                        memcpy(&addr, he->h_addr_list[0], sizeof(addr));
                        strncpy(serverIdIP, inet_ntoa(addr), sizeof(serverIdIP) - 1);
                    }
                }
                if (strcmp(serverIdIP, "0.0.0.0") == 0) {
                    strcpy(serverIdIP, "127.0.0.1");
                }
                LogMessage("[WARN]", CON_YELLOW, "WARNING: Using local IP - may not work on VPS!");
            }
        }
        
        /* Generate Server ID */
        result = Crypto_EncodeServerID(serverIdIP, port, serverId);
        if (result == CRYPTO_SUCCESS) {
            printf("\n");
            SetConsoleColor(CON_CYAN);
            printf("  =====================================================\n");
            printf("                    SERVER ID\n");
            printf("  =====================================================\n");
            SetConsoleColor(CON_WHITE);
            printf("\n                  ");
            SetConsoleColor(CON_GREEN);
            printf("%s\n", serverId);
            SetConsoleColor(CON_WHITE);
            printf("\n  Distribute this ID to client users!\n");
            SetConsoleColor(CON_CYAN);
            printf("  =====================================================\n");
            SetConsoleColor(CON_RESET);
            
            LogMessage("[OK]", CON_GREEN, "Server ID generated successfully");
        } else {
            LogMessage("[WARN]", CON_YELLOW, "Failed to generate Server ID");
        }
    }
    
    printf("\n");
    SetConsoleColor(CON_GREEN);
    printf("  Server Status: RUNNING\n");
    SetConsoleColor(CON_RESET);
    printf("  Listening on:  %s:%d\n", ipAddr, port);
    printf("\n");
    SetConsoleColor(CON_GRAY);
    printf("  --- Activity Log ---\n\n");
    SetConsoleColor(CON_RESET);
    
    /* Main loop - wait for shutdown */
    while (g_bRunning) {
        Sleep(100);
    }
    
    /* Shutdown */
    printf("\n");
    LogMessage("[INFO]", CON_WHITE, "Shutting down relay server...");
    
    if (g_pServer) {
        Relay_Destroy(g_pServer);
        g_pServer = NULL;
    }
    
    WSACleanup();
    DeleteCriticalSection(&g_csConsole);
    
    LogMessage("[OK]", CON_GREEN, "Server stopped gracefully");
    printf("\n");
    
    return 0;
}
