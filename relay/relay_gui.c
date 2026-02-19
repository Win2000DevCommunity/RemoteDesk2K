/*
 * relay_gui.c - Beautiful Windows 2000 Classic Style GUI for Relay Server
 * RemoteDesk2K Project (C) 2026
 * 
 * Features:
 * - Classic Windows 2000 look with Info background colors
 * - Professional layout with grouped sections
 * - Status bar showing connection count
 * - Console log with timestamped entries
 */

#include "relay_gui.h"
#include "relay.h"
#include "crypto.h"

/* Custom messages */
#define WM_RELAY_LOG        (WM_USER + 100)
#define WM_UPDATE_STATUS    (WM_USER + 101)

/* Config file */
#define RELAY_CONFIG_FILE       "relay_config.ini"
#define RELAY_CONFIG_SECTION    "RelayServer"

/* Window dimensions */
#define MAIN_WIDTH      520
#define MAIN_HEIGHT     400

/* Colors - Classic Windows 2000 style */
#define COLOR_PANEL_BG      GetSysColor(COLOR_INFOBK)
#define COLOR_HEADER_BG     RGB(100, 149, 237)  /* CornflowerBlue */
#define COLOR_HEADER_TEXT   RGB(255, 255, 255)
#define COLOR_STATUS_OK     RGB(0, 128, 0)
#define COLOR_STATUS_WARN   RGB(200, 100, 0)

/* Control IDs */
#define IDC_HEADER          2000
#define IDC_GROUP_SERVER    2001
#define IDC_GROUP_SERVERID  2002
#define IDC_GROUP_CONSOLE   2003
#define IDC_STATUSBAR       2004
#define IDC_LBL_IP          2005
#define IDC_LBL_PORT        2006
#define IDC_LBL_SERVERID    2007
#define IDC_LBL_STATUS      2008
#define IDC_CONNECTED_COUNT 2009

/* Global variables */
static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hStatusBar = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontLarge = NULL;
static HFONT g_hFontMono = NULL;
static HBRUSH g_hBrushPanel = NULL;
static HBRUSH g_hBrushHeader = NULL;

/* Controls */
static HWND hEditIP = NULL;
static HWND hEditPort = NULL;
static HWND hBtnStart = NULL;
static HWND hEditServerId = NULL;
static HWND hBtnCopyId = NULL;
static HWND hEditConsole = NULL;
static HWND hLblConnected = NULL;

/* Relay server state */
static PRELAY_SERVER g_pRelayServer = NULL;
static BOOL g_bRelayRunning = FALSE;
static char g_szConfigPath[MAX_PATH] = {0};
static HANDLE g_hMutex = NULL;  /* Single instance mutex */

/* Single instance mutex name */
#define RELAY_GUI_MUTEX_NAME    "RemoteDesk2K_RelayGUI_SingleInstance"

/* Forward declarations */
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void CreateMainControls(HWND hwnd);
static void StartRelayServer(HWND hwnd);
static void StopRelayServer(HWND hwnd);
static void UpdateStatusBar(const char *text, BOOL running);
static void AppendConsoleLog(const char *text);

/* ========================================================================== 
 * CONFIG FILE FUNCTIONS
 * ========================================================================== */

static void GetConfigFilePath(void) {
    char *lastSlash;
    GetModuleFileNameA(NULL, g_szConfigPath, sizeof(g_szConfigPath));
    lastSlash = strrchr(g_szConfigPath, '\\');
    if (lastSlash) {
        lastSlash[1] = '\0';
    }
    strcat(g_szConfigPath, RELAY_CONFIG_FILE);
}

static void SaveRelayConfig(const char* ip, const char* port, const char* serverId) {
    if (g_szConfigPath[0] == '\0') GetConfigFilePath();
    WritePrivateProfileStringA(RELAY_CONFIG_SECTION, "IP", ip, g_szConfigPath);
    WritePrivateProfileStringA(RELAY_CONFIG_SECTION, "Port", port, g_szConfigPath);
    WritePrivateProfileStringA(RELAY_CONFIG_SECTION, "ServerID", serverId, g_szConfigPath);
}

static void LoadRelayConfig(void) {
    char ip[64], port[16], serverId[32];
    
    if (g_szConfigPath[0] == '\0') GetConfigFilePath();
    
    GetPrivateProfileStringA(RELAY_CONFIG_SECTION, "IP", "0.0.0.0", ip, sizeof(ip), g_szConfigPath);
    GetPrivateProfileStringA(RELAY_CONFIG_SECTION, "Port", "5000", port, sizeof(port), g_szConfigPath);
    GetPrivateProfileStringA(RELAY_CONFIG_SECTION, "ServerID", "", serverId, sizeof(serverId), g_szConfigPath);
    
    if (hEditIP) SetWindowTextA(hEditIP, ip);
    if (hEditPort) SetWindowTextA(hEditPort, port);
    if (hEditServerId && serverId[0] != '\0') {
        SetWindowTextA(hEditServerId, serverId);
        EnableWindow(hBtnCopyId, TRUE);
    }
}

/* ========================================================================== 
 * CONSOLE LOGGING
 * ========================================================================== */

static void AppendConsoleLog(const char *text) {
    SYSTEMTIME st;
    char timeStamp[32];
    char fullText[512];
    int len;
    
    if (!hEditConsole || !IsWindow(hEditConsole)) return;
    
    /* Add timestamp */
    GetLocalTime(&st);
    _snprintf(timeStamp, sizeof(timeStamp), "[%02d:%02d:%02d] ", 
              st.wHour, st.wMinute, st.wSecond);
    _snprintf(fullText, sizeof(fullText), "%s%s", timeStamp, text);
    
    /* Append to console */
    len = GetWindowTextLengthA(hEditConsole);
    SendMessageA(hEditConsole, EM_SETSEL, len, len);
    SendMessageA(hEditConsole, EM_REPLACESEL, FALSE, (LPARAM)fullText);
    
    /* Auto-scroll to bottom */
    SendMessageA(hEditConsole, EM_SCROLLCARET, 0, 0);
}

/* Relay log callback - posts message to main thread for thread safety */
static void RelayLogCallback(const char* message) {
    char* msgCopy;
    if (!g_hMainWnd || !IsWindow(g_hMainWnd)) return;
    
    msgCopy = (char*)malloc(strlen(message) + 1);
    if (msgCopy) {
        strcpy(msgCopy, message);
        if (!PostMessageA(g_hMainWnd, WM_RELAY_LOG, 0, (LPARAM)msgCopy)) {
            free(msgCopy);
        }
    }
}

/* ========================================================================== 
 * PUBLIC IP DETECTION via OpenDNS (for VPS/Cloud servers)
 * Uses DNS query to myip.opendns.com via resolver1.opendns.com (208.67.222.222)
 * This returns the PUBLIC IP even when server is behind NAT or on VPS
 * ========================================================================== */

/* DNS header structure */
#pragma pack(push, 1)
typedef struct _DNS_HEADER {
    USHORT id;
    USHORT flags;
    USHORT qdcount;  /* Question count */
    USHORT ancount;  /* Answer count */
    USHORT nscount;  /* Authority count */
    USHORT arcount;  /* Additional count */
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
    USHORT rdlength = (ptr[0] << 8) | ptr[1];
    ptr += 2;
    
    if (rdlength != 4 || ptr + 4 > responseBuf + recvLen) {
        return FALSE;  /* Invalid A record */
    }
    
    /* Extract IP address */
    _snprintf(publicIP, bufLen, "%d.%d.%d.%d", ptr[0], ptr[1], ptr[2], ptr[3]);
    
    return TRUE;
}

/* ========================================================================== 
 * STATUS BAR
 * ========================================================================== */

static void UpdateStatusBar(const char *text, BOOL running) {
    char statusText[256];
    sprintf(statusText, " %s  %s", running ? "\x95" : "\x95", text);
    SendMessageA(g_hStatusBar, SB_SETTEXTA, 0, (LPARAM)statusText);
}

/* ========================================================================== 
 * RELAY SERVER CONTROL
 * ========================================================================== */

static void StartRelayServer(HWND hwnd) {
    char ip[64] = {0};
    char portStr[16] = {0};
    char serverId[SERVER_ID_MAX_LEN] = {0};
    WORD port = 0;
    int result = 0;
    
    /* Initialize crypto for XOR encryption */
    Crypto_Init(NULL);
    
    /* Set log callback */
    Relay_SetLogCallback(RelayLogCallback);
    
    /* Get IP and port from edit controls */
    GetWindowTextA(hEditIP, ip, sizeof(ip)-1);
    GetWindowTextA(hEditPort, portStr, sizeof(portStr)-1);
    port = (WORD)atoi(portStr);
    if (port == 0) port = RELAY_DEFAULT_PORT;
    
    /* Create and start relay server */
    g_pRelayServer = Relay_Create(port, ip);
    if (!g_pRelayServer) {
        char errbuf[128];
        int wsaerr = WSAGetLastError();
        _snprintf(errbuf, sizeof(errbuf), "Failed to create server (Error: %d)\r\n", wsaerr);
        AppendConsoleLog(errbuf);
        UpdateStatusBar("Failed to start server", FALSE);
        return;
    }
    
    result = Relay_Start(g_pRelayServer);
    if (result != 0) {
        char errbuf[128];
        int wsaerr = WSAGetLastError();
        _snprintf(errbuf, sizeof(errbuf), "Failed to start server (Error: %d)\r\n", wsaerr);
        AppendConsoleLog(errbuf);
        Relay_Destroy(g_pRelayServer);
        g_pRelayServer = NULL;
        UpdateStatusBar("Failed to start server", FALSE);
        return;
    }
    
    /* Generate Server ID from IP:Port */
    /* IMPORTANT: For VPS/Cloud servers, we need the PUBLIC IP, not the private IP!
     * Use OpenDNS lookup to get the real public IP address */
    if (strcmp(ip, "0.0.0.0") == 0) {
        char publicIP[64] = {0};
        
        AppendConsoleLog("Detecting public IP via OpenDNS...\r\n");
        
        /* Try to get public IP using OpenDNS DNS lookup */
        if (GetPublicIPviaOpenDNS(publicIP, sizeof(publicIP)) && publicIP[0] != '\0') {
            char logbuf[128];
            strncpy(ip, publicIP, sizeof(ip)-1);
            _snprintf(logbuf, sizeof(logbuf), "Public IP detected: %s\r\n", ip);
            AppendConsoleLog(logbuf);
        } else {
            /* Fallback to local hostname resolution (may give private IP on VPS) */
            char hostname[256];
            struct hostent *he;
            AppendConsoleLog("OpenDNS lookup failed, using local IP...\r\n");
            if (gethostname(hostname, sizeof(hostname)) == 0) {
                he = gethostbyname(hostname);
                if (he && he->h_addr_list[0]) {
                    struct in_addr addr;
                    memcpy(&addr, he->h_addr_list[0], sizeof(addr));
                    strncpy(ip, inet_ntoa(addr), sizeof(ip)-1);
                    AppendConsoleLog("WARNING: Using local IP - may not work on VPS!\r\n");
                }
            }
            if (strcmp(ip, "0.0.0.0") == 0) {
                strcpy(ip, "127.0.0.1");
            }
        }
    }
    
    result = Crypto_EncodeServerID(ip, port, serverId);
    if (result == CRYPTO_SUCCESS) {
        SetWindowTextA(hEditServerId, serverId);
        EnableWindow(hBtnCopyId, TRUE);
        AppendConsoleLog("Server ID generated successfully\r\n");
        AppendConsoleLog("Distribute this ID to client users!\r\n");
        SaveRelayConfig(ip, portStr, serverId);
    } else {
        SetWindowTextA(hEditServerId, "(Generation failed)");
        EnableWindow(hBtnCopyId, FALSE);
    }
    
    SetWindowTextA(hBtnStart, "Stop Server");
    EnableWindow(hEditIP, FALSE);
    EnableWindow(hEditPort, FALSE);
    
    g_bRelayRunning = TRUE;
    UpdateStatusBar("Server running - waiting for connections", TRUE);
    AppendConsoleLog("Relay server started successfully\r\n");
}

static void StopRelayServer(HWND hwnd) {
    Relay_SetLogCallback(NULL);
    
    if (g_pRelayServer) {
        Relay_Stop(g_pRelayServer);
        Relay_Destroy(g_pRelayServer);
        g_pRelayServer = NULL;
    }
    
    Crypto_Cleanup();
    
    SetWindowTextA(hEditServerId, "(Start server to generate)");
    EnableWindow(hBtnCopyId, FALSE);
    
    SetWindowTextA(hBtnStart, "Start Server");
    EnableWindow(hEditIP, TRUE);
    EnableWindow(hEditPort, TRUE);
    
    g_bRelayRunning = FALSE;
    UpdateStatusBar("Server stopped", FALSE);
    AppendConsoleLog("Relay server stopped\r\n");
}

/* ========================================================================== 
 * CREATE CONTROLS
 * ========================================================================== */

static void CreateMainControls(HWND hwnd) {
    int y;
    int panelWidth = MAIN_WIDTH - 30;
    int leftMargin = 15;
    
    /* Header - Blue banner */
    CreateWindowExA(0, "STATIC", "  RemoteDesk2K Relay Server",
                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                   0, 0, MAIN_WIDTH, 35, hwnd, (HMENU)IDC_HEADER, g_hInstance, NULL);
    
    y = 50;
    
    /* ---- Server Configuration Group ---- */
    CreateWindowExA(0, "BUTTON", " Server Configuration ",
                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                   leftMargin, y, panelWidth, 75, hwnd, (HMENU)IDC_GROUP_SERVER, g_hInstance, NULL);
    
    y += 22;
    
    /* IP Address */
    CreateWindowExA(0, "STATIC", "Listen IP:",
                   WS_CHILD | WS_VISIBLE | SS_RIGHT,
                   leftMargin + 10, y + 3, 70, 18, hwnd, (HMENU)IDC_LBL_IP, g_hInstance, NULL);
    hEditIP = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0.0.0.0",
                             WS_CHILD | WS_VISIBLE | ES_CENTER,
                             leftMargin + 85, y, 120, 22, hwnd, (HMENU)IDC_PARAM_IP, g_hInstance, NULL);
    
    /* Port */
    CreateWindowExA(0, "STATIC", "Port:",
                   WS_CHILD | WS_VISIBLE | SS_RIGHT,
                   leftMargin + 215, y + 3, 40, 18, hwnd, (HMENU)IDC_LBL_PORT, g_hInstance, NULL);
    hEditPort = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "5000",
                               WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER,
                               leftMargin + 260, y, 60, 22, hwnd, (HMENU)IDC_PARAM_PORT, g_hInstance, NULL);
    
    /* Start/Stop Button */
    hBtnStart = CreateWindowExA(0, "BUTTON", "Start Server",
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               leftMargin + 340, y - 2, 120, 28, hwnd, (HMENU)IDC_PARAM_START, g_hInstance, NULL);
    
    y += 55;
    
    /* ---- Server ID Group ---- */
    CreateWindowExA(0, "BUTTON", " Server ID (share with clients) ",
                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                   leftMargin, y, panelWidth, 55, hwnd, (HMENU)IDC_GROUP_SERVERID, g_hInstance, NULL);
    
    y += 22;
    
    /* Server ID display */
    hEditServerId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "(Start server to generate)",
                                   WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
                                   leftMargin + 15, y, 340, 24, hwnd, (HMENU)IDC_SERVER_ID, g_hInstance, NULL);
    
    /* Copy button */
    hBtnCopyId = CreateWindowExA(0, "BUTTON", "Copy ID",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                leftMargin + 365, y - 1, 90, 26, hwnd, (HMENU)IDC_COPY_ID, g_hInstance, NULL);
    
    y += 55;
    
    /* ---- Console Log Group ---- */
    CreateWindowExA(0, "BUTTON", " Console Log ",
                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                   leftMargin, y, panelWidth, 140, hwnd, (HMENU)IDC_GROUP_CONSOLE, g_hInstance, NULL);
    
    y += 18;
    
    /* Console edit control */
    hEditConsole = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                  WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | 
                                  ES_READONLY | WS_VSCROLL,
                                  leftMargin + 10, y, panelWidth - 20, 110, 
                                  hwnd, (HMENU)IDC_CONSOLE, g_hInstance, NULL);
    
    /* Status bar */
    g_hStatusBar = CreateStatusWindowA(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                      " Ready", hwnd, IDC_STATUSBAR);
    
    /* Set fonts */
    SendMessage(hEditIP, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(hEditPort, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(hBtnStart, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    SendMessage(hEditServerId, WM_SETFONT, (WPARAM)g_hFontLarge, TRUE);
    SendMessage(hBtnCopyId, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    SendMessage(hEditConsole, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
    
    /* Set font for all static controls */
    {
        HWND hChild = GetWindow(hwnd, GW_CHILD);
        while (hChild) {
            char className[32];
            GetClassNameA(hChild, className, sizeof(className));
            if (_stricmp(className, "STATIC") == 0 || _stricmp(className, "BUTTON") == 0) {
                SendMessage(hChild, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            }
            hChild = GetWindow(hChild, GW_HWNDNEXT);
        }
    }
}

/* ========================================================================== 
 * WINDOW PROCEDURE
 * ========================================================================== */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateMainControls(hwnd);
            LoadRelayConfig();
            UpdateStatusBar("Ready - click Start Server to begin", FALSE);
            AppendConsoleLog("RemoteDesk2K Relay Server initialized\r\n");
            AppendConsoleLog("Configure IP/Port and click Start Server\r\n");
            return 0;
        
        case WM_CTLCOLORSTATIC:
        {
            HDC hdcStatic = (HDC)wParam;
            HWND hStatic = (HWND)lParam;
            int ctrlId = GetDlgCtrlID(hStatic);
            
            /* Blue header background */
            if (ctrlId == IDC_HEADER) {
                SetTextColor(hdcStatic, COLOR_HEADER_TEXT);
                SetBkColor(hdcStatic, COLOR_HEADER_BG);
                return (LRESULT)g_hBrushHeader;
            }
            
            /* Panel background for other statics */
            SetBkColor(hdcStatic, COLOR_PANEL_BG);
            return (LRESULT)g_hBrushPanel;
        }
        
        case WM_CTLCOLOREDIT:
        {
            HDC hdcEdit = (HDC)wParam;
            HWND hEdit = (HWND)lParam;
            
            /* Server ID field - light blue when has ID */
            if (hEdit == hEditServerId && g_bRelayRunning) {
                SetBkColor(hdcEdit, RGB(230, 240, 255));
                return (LRESULT)CreateSolidBrush(RGB(230, 240, 255));
            }
            return DefWindowProcA(hwnd, msg, wParam, lParam);
        }
        
        case WM_COMMAND:
        {
            HWND hCtrl = (HWND)lParam;
            
            if (hCtrl == hBtnStart) {
                if (!g_bRelayRunning) {
                    StartRelayServer(hwnd);
                } else {
                    StopRelayServer(hwnd);
                }
            }
            else if (hCtrl == hBtnCopyId) {
                char serverId[64];
                GetWindowTextA(hEditServerId, serverId, sizeof(serverId));
                if (serverId[0] != '\0' && serverId[0] != '(') {
                    if (OpenClipboard(hwnd)) {
                        HGLOBAL hMem;
                        char *pMem;
                        int len = (int)strlen(serverId) + 1;
                        EmptyClipboard();
                        hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                        if (hMem) {
                            pMem = (char*)GlobalLock(hMem);
                            if (pMem) {
                                memcpy(pMem, serverId, len);
                                GlobalUnlock(hMem);
                                SetClipboardData(CF_TEXT, hMem);
                            }
                        }
                        CloseClipboard();
                        AppendConsoleLog("Server ID copied to clipboard\r\n");
                    }
                }
            }
            return 0;
        }
        
        case WM_RELAY_LOG:
        {
            char* logText = (char*)lParam;
            if (logText) {
                AppendConsoleLog(logText);
                free(logText);
            }
            return 0;
        }
        
        case WM_SIZE:
        {
            /* Resize status bar */
            SendMessage(g_hStatusBar, WM_SIZE, 0, 0);
            return 0;
        }
        
        case WM_CLOSE:
            g_hMainWnd = NULL;
            StopRelayServer(hwnd);
            DestroyWindow(hwnd);
            return 0;
        
        case WM_DESTROY:
            g_hMainWnd = NULL;
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ========================================================================== 
 * ENTRY POINT
 * ========================================================================== */

int RelayGui_Run(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXA wc;
    MSG msg;
    WSADATA wsaData;
    INITCOMMONCONTROLSEX icc;
    
    g_hInstance = hInstance;
    
    /* Check for single instance */
    g_hMutex = CreateMutexA(NULL, TRUE, RELAY_GUI_MUTEX_NAME);
    if (g_hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(NULL, "Another instance of Relay Server is already running!\n\nClose the other instance first.", "RemoteDesk2K Relay Server", MB_ICONWARNING | MB_OK);
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }
    
    /* Initialize Winsock */
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        MessageBoxA(NULL, "Failed to initialize network!", "Error", MB_ICONERROR);
        return 1;
    }
    
    /* Initialize common controls */
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    
    /* Create fonts */
    g_hFontNormal = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    g_hFontBold = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    g_hFontLarge = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    g_hFontMono = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    
    /* Create brushes */
    g_hBrushPanel = GetSysColorBrush(COLOR_INFOBK);
    g_hBrushHeader = CreateSolidBrush(COLOR_HEADER_BG);
    
    /* Register window class */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushPanel;
    wc.lpszClassName = "RD2KRelayServerClass";
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    
    if (!RegisterClassExA(&wc)) {
        WSACleanup();
        return 1;
    }
    
    /* Create main window */
    g_hMainWnd = CreateWindowExA(
        0, "RD2KRelayServerClass", "RemoteDesk2K Relay Server",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, MAIN_WIDTH, MAIN_HEIGHT,
        NULL, NULL, hInstance, NULL);
    
    if (!g_hMainWnd) {
        WSACleanup();
        return 1;
    }
    
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);
    
    /* Force initial refresh */
    RedrawWindow(g_hMainWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
    
    /* Message loop */
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    /* Cleanup */
    DeleteObject(g_hFontNormal);
    DeleteObject(g_hFontBold);
    DeleteObject(g_hFontLarge);
    DeleteObject(g_hFontMono);
    DeleteObject(g_hBrushHeader);
    
    WSACleanup();
    
    /* Release single instance mutex */
    if (g_hMutex) {
        CloseHandle(g_hMutex);
        g_hMutex = NULL;
    }
    
    return (int)msg.wParam;
}
