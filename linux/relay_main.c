/*
 * relay_main.c - RemoteDesk2K Linux Relay Server CLI
 * 
 * Professional terminal interface with:
 * - Signal handling (SIGINT, SIGTERM)
 * - Runtime statistics display
 * - Color-coded log output
 * - Daemon mode support
 */

#include "common.h"
#include "crypto.h"
#include "relay.h"
#include <sys/file.h>  /* For flock() */

/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

static RELAY_SERVER *g_pServer = NULL;
static volatile int g_bRunning = 1;
static int g_bDaemon = 0;
static int g_bColor = 1;
static FILE *g_logFile = NULL;
static pthread_mutex_t g_printMutex = PTHREAD_MUTEX_INITIALIZER;
static char g_customIp[64] = "";  /* Custom IP for Server ID (for local testing) */
static int g_lockFd = -1;  /* Lock file descriptor for single instance */

/* Lock file path */
#define LOCK_FILE_PATH  "/tmp/rd2k_relay.lock"

/* ANSI color codes */
#define COLOR_RESET     "\033[0m"
#define COLOR_RED       "\033[31m"
#define COLOR_GREEN     "\033[32m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_BLUE      "\033[34m"
#define COLOR_MAGENTA   "\033[35m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_BOLD      "\033[1m"
#define COLOR_DIM       "\033[2m"

/* ============================================================ * SINGLE INSTANCE LOCK
 * ============================================================ */

/* Acquire single instance lock using file locking
 * Returns: 0 on success, -1 if another instance is running */
static int AcquireLock(void)
{
    g_lockFd = open(LOCK_FILE_PATH, O_CREAT | O_RDWR, 0644);
    if (g_lockFd < 0) {
        perror(\"Failed to open lock file\");
        return -1;
    }
    
    /* Try to acquire exclusive lock (non-blocking) */
    if (flock(g_lockFd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr, \"\\n*** ERROR: Another instance of Relay Server is already running! ***\\n\");
            fprintf(stderr, \"Close the other instance first.\\n\\n\");
        } else {
            perror(\"Failed to acquire lock\");
        }
        close(g_lockFd);
        g_lockFd = -1;
        return -1;
    }
    
    /* Write PID to lock file */
    {
        char pid_str[32];
        int len = snprintf(pid_str, sizeof(pid_str), \"%d\\n\", (int)getpid());
        if (ftruncate(g_lockFd, 0) == 0) {
            (void)write(g_lockFd, pid_str, len);
        }
    }
    
    return 0;
}

/* Release single instance lock */
static void ReleaseLock(void)
{
    if (g_lockFd >= 0) {
        flock(g_lockFd, LOCK_UN);
        close(g_lockFd);
        g_lockFd = -1;
        unlink(LOCK_FILE_PATH);
    }
}

/* ============================================================ * LOGGING
 * ============================================================ */

static void GetTimestamp(char *buffer, size_t len)
{
    time_t now;
    struct tm *tm_info;
    
    time(&now);
    tm_info = localtime(&now);
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", tm_info);
}

static void LogCallback(const char *message)
{
    char timestamp[32];
    const char *color = COLOR_RESET;
    
    GetTimestamp(timestamp, sizeof(timestamp));
    
    /* Determine color based on message type */
    if (strstr(message, "[ERROR]")) {
        color = COLOR_RED;
    } else if (strstr(message, "[WARN]")) {
        color = COLOR_YELLOW;
    } else if (strstr(message, "[INFO]")) {
        color = COLOR_GREEN;
    } else if (strstr(message, "[REGISTER]")) {
        color = COLOR_CYAN;
    } else if (strstr(message, "[CONNECT]")) {
        color = COLOR_MAGENTA;
    } else if (strstr(message, "[DISCONNECT]")) {
        color = COLOR_BLUE;
    } else if (strstr(message, "[CLEANUP]")) {
        color = COLOR_DIM;
    } else if (strstr(message, "[PROTECT]")) {
        color = COLOR_YELLOW;
    } else if (strstr(message, "[REJECT]")) {
        color = COLOR_RED;
    }
    
    pthread_mutex_lock(&g_printMutex);
    
    if (g_logFile) {
        /* Plain text for log file */
        fprintf(g_logFile, "[%s] %s\n", timestamp, message);
        fflush(g_logFile);
    }
    
    if (!g_bDaemon) {
        if (g_bColor) {
            fprintf(stdout, "%s[%s]%s %s%s%s", 
                    COLOR_DIM, timestamp, COLOR_RESET,
                    color, message, COLOR_RESET);
        } else {
            fprintf(stdout, "[%s] %s", timestamp, message);
        }
        
        /* Add newline if message doesn't have one */
        if (message[strlen(message)-1] != '\n')
            fprintf(stdout, "\n");
        
        fflush(stdout);
    }
    
    pthread_mutex_unlock(&g_printMutex);
}

/* ============================================================
 * SIGNAL HANDLING
 * ============================================================ */

static void SignalHandler(int sig)
{
    pthread_mutex_lock(&g_printMutex);
    
    if (g_bColor && !g_bDaemon) {
        fprintf(stdout, "\n%s%s>>> Received signal %d (%s), shutting down...%s\n",
                COLOR_BOLD, COLOR_YELLOW, sig,
                sig == SIGINT ? "SIGINT" : sig == SIGTERM ? "SIGTERM" : "UNKNOWN",
                COLOR_RESET);
    } else if (!g_bDaemon) {
        fprintf(stdout, "\n>>> Received signal %d, shutting down...\n", sig);
    }
    
    pthread_mutex_unlock(&g_printMutex);
    
    g_bRunning = 0;
}

static void SetupSignalHandlers(void)
{
    struct sigaction sa;
    
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    /* Ignore SIGPIPE (broken pipe) - handle in send() instead */
    signal(SIGPIPE, SIG_IGN);
}

/* ============================================================
 * BANNER & HELP
 * ============================================================ */

static void PrintBanner(void)
{
    if (g_bDaemon) return;
    
    pthread_mutex_lock(&g_printMutex);
    
    if (g_bColor) {
        fprintf(stdout, "\n");
        fprintf(stdout, "%s%s╔══════════════════════════════════════════════════════════╗%s\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s%s║%s%s        RemoteDesk2K - Linux Relay Server v1.0.0         %s%s║%s\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET, COLOR_BOLD, COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s%s║%s     Compatible with Windows 2000 RemoteDesk2K Clients    %s║%s\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s%s╚══════════════════════════════════════════════════════════╝%s\n", COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "\n");
    } else {
        fprintf(stdout, "\n");
        fprintf(stdout, "============================================================\n");
        fprintf(stdout, "        RemoteDesk2K - Linux Relay Server v1.0.0\n");
        fprintf(stdout, "     Compatible with Windows 2000 RemoteDesk2K Clients\n");
        fprintf(stdout, "============================================================\n");
        fprintf(stdout, "\n");
    }
    
    pthread_mutex_unlock(&g_printMutex);
}

static void PrintHelp(const char *progname)
{
    fprintf(stdout, "Usage: %s [OPTIONS]\n\n", progname);
    fprintf(stdout, "Options:\n");
    fprintf(stdout, "  -p, --port PORT      Listen port (default: 5000)\n");
    fprintf(stdout, "  -b, --bind IP        Bind to specific IP (default: 0.0.0.0)\n");
    fprintf(stdout, "  -i, --server-ip IP   Override IP in Server ID (for local testing)\n");
    fprintf(stdout, "  -d, --daemon         Run as daemon (background)\n");
    fprintf(stdout, "  -l, --log FILE       Log output to file\n");
    fprintf(stdout, "  -n, --no-color       Disable colored output\n");
    fprintf(stdout, "  -h, --help           Show this help message\n");
    fprintf(stdout, "  -v, --version        Show version information\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Examples:\n");
    fprintf(stdout, "  %s                   # Listen on 0.0.0.0:5000\n", progname);
    fprintf(stdout, "  %s -p 5900           # Listen on port 5900\n", progname);
    fprintf(stdout, "  %s -p 80 -i 127.0.0.1  # Local testing (Server ID uses 127.0.0.1)\n", progname);
    fprintf(stdout, "  %s -d -l relay.log   # Run as daemon with logging\n", progname);
    fprintf(stdout, "\n");
    fprintf(stdout, "Signals:\n");
    fprintf(stdout, "  SIGINT (Ctrl+C)      Graceful shutdown\n");
    fprintf(stdout, "  SIGTERM              Graceful shutdown\n");
    fprintf(stdout, "\n");
}

static void PrintVersion(void)
{
    fprintf(stdout, "RemoteDesk2K Linux Relay Server v1.0.0\n");
    fprintf(stdout, "Compatible with Windows RemoteDesk2K clients\n");
    fprintf(stdout, "Built: %s %s\n", __DATE__, __TIME__);
}

/* ============================================================
 * DAEMON MODE
 * ============================================================ */

static int Daemonize(void)
{
    pid_t pid;
    
    /* Fork off parent */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        /* Parent exits */
        printf("Daemon started with PID %d\n", pid);
        exit(0);
    }
    
    /* Child becomes session leader */
    if (setsid() < 0) {
        perror("setsid");
        return -1;
    }
    
    /* Fork again to prevent acquiring terminal */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }
    
    /* Change working directory */
    if (chdir("/") < 0) {
        perror("chdir");
    }
    
    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    /* Redirect to /dev/null */
    open("/dev/null", O_RDONLY);  /* stdin */
    open("/dev/null", O_WRONLY); /* stdout */
    open("/dev/null", O_WRONLY); /* stderr */
    
    return 0;
}

/* ============================================================
 * PUBLIC IP DETECTION VIA OPENDNS
 * ============================================================ */

/* DNS header structure */
#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} DNS_HEADER_T;
#pragma pack(pop)

/* Get public IP address using OpenDNS lookup (myip.opendns.com)
 * Same method as Windows relay server
 * Returns 1 on success, 0 on failure */
static int GetPublicIPviaOpenDNS(char *publicIP, int bufLen)
{
    int sock = -1;
    struct sockaddr_in dnsServer;
    uint8_t queryBuf[512];
    uint8_t responseBuf[512];
    int queryLen = 0;
    ssize_t recvLen = 0;
    DNS_HEADER_T *dnsHeader;
    uint8_t *ptr;
    fd_set readfds;
    struct timeval tv;
    
    /* DNS server: resolver1.opendns.com = 208.67.222.222 */
    const char *DNS_SERVER_IP = "208.67.222.222";
    const uint16_t DNS_PORT = 53;
    
    /* Query for: myip.opendns.com (returns your public IP) */
    static const uint8_t dnsQuestion[] = {
        4, 'm', 'y', 'i', 'p',                /* "myip" */
        7, 'o', 'p', 'e', 'n', 'd', 'n', 's', /* "opendns" */
        3, 'c', 'o', 'm',                     /* "com" */
        0,                                    /* End of name */
        0, 1,                                 /* Type: A (IPv4) */
        0, 1                                  /* Class: IN (Internet) */
    };
    
    if (!publicIP || bufLen < 16) return 0;
    publicIP[0] = '\0';
    
    /* Create UDP socket */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return 0;
    }
    
    /* Set timeout */
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    /* Setup DNS server address */
    memset(&dnsServer, 0, sizeof(dnsServer));
    dnsServer.sin_family = AF_INET;
    dnsServer.sin_port = htons(DNS_PORT);
    inet_pton(AF_INET, DNS_SERVER_IP, &dnsServer.sin_addr);
    
    /* Build DNS query packet */
    memset(queryBuf, 0, sizeof(queryBuf));
    dnsHeader = (DNS_HEADER_T*)queryBuf;
    dnsHeader->id = htons(0x1234);
    dnsHeader->flags = htons(0x0100);  /* Standard query, recursion desired */
    dnsHeader->qdcount = htons(1);
    dnsHeader->ancount = 0;
    dnsHeader->nscount = 0;
    dnsHeader->arcount = 0;
    
    /* Copy question after header */
    queryLen = sizeof(DNS_HEADER_T);
    memcpy(queryBuf + queryLen, dnsQuestion, sizeof(dnsQuestion));
    queryLen += sizeof(dnsQuestion);
    
    /* Send DNS query */
    if (sendto(sock, queryBuf, queryLen, 0,
               (struct sockaddr*)&dnsServer, sizeof(dnsServer)) < 0) {
        close(sock);
        return 0;
    }
    
    /* Wait for response */
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    
    if (select(sock + 1, &readfds, NULL, NULL, &tv) <= 0) {
        close(sock);
        return 0;
    }
    
    /* Receive response */
    recvLen = recv(sock, responseBuf, sizeof(responseBuf), 0);
    close(sock);
    
    if (recvLen <= (ssize_t)(sizeof(DNS_HEADER_T) + sizeof(dnsQuestion))) {
        return 0;
    }
    
    /* Parse response */
    dnsHeader = (DNS_HEADER_T*)responseBuf;
    
    /* Check for valid response */
    if ((ntohs(dnsHeader->flags) & 0x8000) == 0) return 0;
    if ((ntohs(dnsHeader->flags) & 0x000F) != 0) return 0;
    if (ntohs(dnsHeader->ancount) == 0) return 0;
    
    /* Skip header and question to find answer */
    ptr = responseBuf + sizeof(DNS_HEADER_T);
    
    /* Skip question */
    while (*ptr != 0 && ptr < responseBuf + recvLen) {
        ptr += (*ptr) + 1;
    }
    ptr += 1 + 4;  /* null + type + class */
    
    /* Skip answer name (compressed pointer or labels) */
    if ((*ptr & 0xC0) == 0xC0) {
        ptr += 2;
    } else {
        while (*ptr != 0 && ptr < responseBuf + recvLen) {
            ptr += (*ptr) + 1;
        }
        ptr++;
    }
    
    /* Check we have enough data */
    if (ptr + 10 > responseBuf + recvLen) return 0;
    
    /* Check type A */
    if (ptr[0] != 0 || ptr[1] != 1) return 0;
    ptr += 2 + 2 + 4;  /* type + class + ttl */
    
    /* Get rdlength */
    uint16_t rdlength = (ptr[0] << 8) | ptr[1];
    ptr += 2;
    
    if (rdlength != 4 || ptr + 4 > responseBuf + recvLen) return 0;
    
    /* Extract IP address */
    snprintf(publicIP, bufLen, "%d.%d.%d.%d", ptr[0], ptr[1], ptr[2], ptr[3]);
    
    return 1;
}

/* ============================================================
 * STATUS DISPLAY
 * ============================================================ */

static void PrintStatus(WORD port, const char *bindIp)
{
    char serverId[SERVER_ID_MAX_LEN];
    char publicIp[64] = {0};
    char displayIp[64] = {0};
    
    snprintf(displayIp, sizeof(displayIp), "%s", bindIp);
    
    pthread_mutex_lock(&g_printMutex);
    
    if (g_bColor) {
        fprintf(stdout, "%s┌─────────────────────────────────────────────────┐%s\n", COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s│%s  Server Status                                  %s│%s\n", COLOR_CYAN, COLOR_BOLD, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s├─────────────────────────────────────────────────┤%s\n", COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s│%s  Listen IP:   %s%-33s%s│%s\n", COLOR_CYAN, COLOR_RESET, COLOR_GREEN, bindIp, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s│%s  Port:        %s%-33d%s│%s\n", COLOR_CYAN, COLOR_RESET, COLOR_GREEN, port, COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s│%s  Status:      %s%-33s%s│%s\n", COLOR_CYAN, COLOR_RESET, COLOR_GREEN, "RUNNING", COLOR_CYAN, COLOR_RESET);
        fprintf(stdout, "%s└─────────────────────────────────────────────────┘%s\n", COLOR_CYAN, COLOR_RESET);
    } else {
        fprintf(stdout, "Server Status:\n");
        fprintf(stdout, "  Listen IP: %s\n", bindIp);
        fprintf(stdout, "  Port:      %d\n", port);
        fprintf(stdout, "  Status:    RUNNING\n");
    }
    
    /* Check for custom IP override first (for local testing) */
    if (g_customIp[0] != '\0') {
        snprintf(displayIp, sizeof(displayIp), "%s", g_customIp);
        if (g_bColor) {
            fprintf(stdout, "\n%sUsing custom IP for Server ID: %s%s%s\n", COLOR_DIM, COLOR_YELLOW, g_customIp, COLOR_RESET);
        } else {
            fprintf(stdout, "\nUsing custom IP for Server ID: %s\n", g_customIp);
        }
    }
    /* Detect public IP via OpenDNS if bound to 0.0.0.0 and no custom IP */
    else if (strcmp(bindIp, "0.0.0.0") == 0) {
        if (g_bColor) {
            fprintf(stdout, "\n%sDetecting public IP via OpenDNS...%s\n", COLOR_DIM, COLOR_RESET);
        } else {
            fprintf(stdout, "\nDetecting public IP via OpenDNS...\n");
        }
        
        if (GetPublicIPviaOpenDNS(publicIp, sizeof(publicIp)) && publicIp[0] != '\0') {
            snprintf(displayIp, sizeof(displayIp), "%s", publicIp);
            if (g_bColor) {
                fprintf(stdout, "%sPublic IP detected: %s%s%s\n", COLOR_DIM, COLOR_GREEN, publicIp, COLOR_RESET);
            } else {
                fprintf(stdout, "Public IP detected: %s\n", publicIp);
            }
        } else {
            if (g_bColor) {
                fprintf(stdout, "%s%sWARNING: Could not detect public IP%s\n", COLOR_YELLOW, COLOR_BOLD, COLOR_RESET);
            } else {
                fprintf(stdout, "WARNING: Could not detect public IP\n");
            }
        }
    }
    
    /* Generate Server ID */
    if (displayIp[0] != '\0' && strcmp(displayIp, "0.0.0.0") != 0) {
        if (Crypto_EncodeServerID(displayIp, port, serverId) == CRYPTO_SUCCESS) {
            fprintf(stdout, "\n");
            if (g_bColor) {
                fprintf(stdout, "%s╔══════════════════════════════════════════════════════════╗%s\n", COLOR_GREEN, COLOR_RESET);
                fprintf(stdout, "%s║%s  SERVER ID: %s%-44s%s║%s\n", COLOR_GREEN, COLOR_RESET, COLOR_BOLD, serverId, COLOR_GREEN, COLOR_RESET);
                fprintf(stdout, "%s╚══════════════════════════════════════════════════════════╝%s\n", COLOR_GREEN, COLOR_RESET);
                fprintf(stdout, "\n%sServer ID !%s\n", COLOR_DIM, COLOR_RESET);
            } else {
                fprintf(stdout, "============================================================\n");
                fprintf(stdout, "  SERVER ID: %s\n", serverId);
                fprintf(stdout, "============================================================\n");
                fprintf(stdout, "\nGive this Server ID to your clients!\n");
            }
        }
    }
    
    fprintf(stdout, "\n");
    
    if (g_bColor) {
        fprintf(stdout, "%sPress Ctrl+C to stop the server%s\n\n", COLOR_DIM, COLOR_RESET);
    } else {
        fprintf(stdout, "Press Ctrl+C to stop the server\n\n");
    }
    
    pthread_mutex_unlock(&g_printMutex);
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char *argv[])
{
    WORD port = RELAY_DEFAULT_PORT;
    char bindIp[64] = "0.0.0.0";
    const char *logFile = NULL;
    int i;
    
    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = (WORD)atoi(argv[++i]);
                if (port == 0) port = RELAY_DEFAULT_PORT;
            }
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bind") == 0) {
            if (i + 1 < argc) {
                strncpy(bindIp, argv[++i], sizeof(bindIp) - 1);
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            g_bDaemon = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            if (i + 1 < argc) {
                logFile = argv[++i];
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-color") == 0) {
            g_bColor = 0;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--server-ip") == 0) {
            if (i + 1 < argc) {
                strncpy(g_customIp, argv[++i], sizeof(g_customIp) - 1);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintHelp(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            PrintVersion();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            PrintHelp(argv[0]);
            return 1;
        }
    }
    
    /* Acquire single instance lock */
    if (AcquireLock() < 0) {
        return 1;  /* Another instance is running */
    }
    
    /* Open log file if specified */
    if (logFile) {
        g_logFile = fopen(logFile, "a");
        if (!g_logFile) {
            perror("Failed to open log file");
            return 1;
        }
    }
    
    /* Daemonize if requested */
    if (g_bDaemon) {
        if (Daemonize() < 0) {
            fprintf(stderr, "Failed to daemonize\n");
            return 1;
        }
    }
    
    /* Setup signal handlers */
    SetupSignalHandlers();
    
    /* Print banner */
    PrintBanner();
    
    /* Initialize crypto */
    Crypto_Init(NULL);
    
    /* Set log callback */
    Relay_SetLogCallback(LogCallback);
    
    /* Create relay server */
    g_pServer = Relay_Create(port, bindIp);
    if (!g_pServer) {
        LogCallback("[ERROR] Failed to create relay server - check port/IP\n");
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    
    /* Start server */
    if (Relay_Start(g_pServer) != RD2K_SUCCESS) {
        LogCallback("[ERROR] Failed to start relay server\n");
        Relay_Destroy(g_pServer);
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    
    /* Print status */
    PrintStatus(port, bindIp);
    
    LogCallback("[INFO] Relay server started successfully\n");
    
    /* Main loop - wait for shutdown signal */
    while (g_bRunning) {
        usleep(100000);  /* 100ms */
    }
    
    /* Shutdown */
    LogCallback("[INFO] Shutting down relay server...\n");
    
    Relay_Destroy(g_pServer);
    g_pServer = NULL;
    
    Crypto_Cleanup();
    
    LogCallback("[INFO] Relay server stopped\n");
    
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
    
    /* Release single instance lock */
    ReleaseLock();
    
    if (!g_bDaemon && g_bColor) {
        fprintf(stdout, "%s%sGoodbye!%s\n", COLOR_BOLD, COLOR_GREEN, COLOR_RESET);
    }
    
    return 0;
}
