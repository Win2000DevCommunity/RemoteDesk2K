
// relay_gui.c - Windows 2000 compatible GUI for relay server
#include "relay_gui.h" // central header for GUI, includes common.h
#include "relay.h"     // relay server types and functions
#include "crypto.h"    // XOR encryption for relay messages

/* Custom message for thread-safe console logging */
#define WM_RELAY_LOG (WM_USER + 100)

/* Config file name */
#define RELAY_CONFIG_FILE "relay_config.ini"
#define RELAY_CONFIG_SECTION "RelayServer"

// Global variables
static HWND g_hMainWnd = NULL;  /* Main window handle for logging */
static HWND hTab;
static HWND hEditIP, hEditPort, hBtnStart;
static HWND hEditServerId, hBtnCopyId;  /* Server ID display and copy */
static HWND hEditConsole;
static PRELAY_SERVER g_pRelayServer = NULL;
static BOOL relayRunning = FALSE;
static CRITICAL_SECTION g_csLogQueue;
static BOOL g_bLogCsInitialized = FALSE;
static char g_szConfigPath[MAX_PATH] = {0};  /* Full path to config file */

/* ========================================================================== 
 * CONFIG FILE FUNCTIONS - Save/Load relay settings
 * ========================================================================== */

/* Get config file path (same directory as exe) */
static void GetConfigFilePath(void) {
    char *lastSlash;
    GetModuleFileNameA(NULL, g_szConfigPath, sizeof(g_szConfigPath));
    lastSlash = strrchr(g_szConfigPath, '\\');
    if (lastSlash) {
        lastSlash[1] = '\0';
    }
    strcat(g_szConfigPath, RELAY_CONFIG_FILE);
}

/* Save relay server config to INI file */
static void SaveRelayConfig(const char* ip, const char* port, const char* serverId) {
    if (g_szConfigPath[0] == '\0') GetConfigFilePath();
    
    WritePrivateProfileStringA(RELAY_CONFIG_SECTION, "IP", ip, g_szConfigPath);
    WritePrivateProfileStringA(RELAY_CONFIG_SECTION, "Port", port, g_szConfigPath);
    WritePrivateProfileStringA(RELAY_CONFIG_SECTION, "ServerID", serverId, g_szConfigPath);
}

/* Load relay server config from INI file */
static void LoadRelayConfig(void) {
    char ip[64], port[16], serverId[32];
    
    if (g_szConfigPath[0] == '\0') GetConfigFilePath();
    
    GetPrivateProfileStringA(RELAY_CONFIG_SECTION, "IP", "0.0.0.0", ip, sizeof(ip), g_szConfigPath);
    GetPrivateProfileStringA(RELAY_CONFIG_SECTION, "Port", "5000", port, sizeof(port), g_szConfigPath);
    GetPrivateProfileStringA(RELAY_CONFIG_SECTION, "ServerID", "", serverId, sizeof(serverId), g_szConfigPath);
    
    /* Apply loaded values to controls */
    if (hEditIP) SetWindowTextA(hEditIP, ip);
    if (hEditPort) SetWindowTextA(hEditPort, port);
    if (hEditServerId && serverId[0] != '\0') {
        SetWindowTextA(hEditServerId, serverId);
        EnableWindow(hBtnCopyId, TRUE);
    }
}

// Forward declarations (only those not in headers)
static DWORD WINAPI RelayThreadProc(LPVOID lpParam);
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void OnTabChange(HWND hwnd);
static void StartRelayServer(HWND hwnd);
static void StopRelayServer(HWND hwnd);


// Modular tab creation
static void CreateAllControls(HWND parent, RECT rc) {
    // Parameter tab controls
    CreateWindowExW(0, L"STATIC", L"Relay IP:", WS_CHILD|WS_VISIBLE, 16,40,60,20, parent, NULL, NULL, NULL);
    hEditIP = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.0.0.0", WS_CHILD|WS_VISIBLE, 80,40,100,20, parent, (HMENU)IDC_PARAM_IP, NULL, NULL);
    if (!hEditIP) MessageBoxW(NULL, L"Failed to create hEditIP", L"Debug", MB_OK|MB_ICONERROR);
    CreateWindowExW(0, L"STATIC", L"Port:", WS_CHILD|WS_VISIBLE, 190,40,40,20, parent, NULL, NULL, NULL);
    hEditPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"5000", WS_CHILD|WS_VISIBLE, 230,40,60,20, parent, (HMENU)IDC_PARAM_PORT, NULL, NULL);
    if (!hEditPort) MessageBoxW(NULL, L"Failed to create hEditPort", L"Debug", MB_OK|MB_ICONERROR);
    hBtnStart = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD|WS_VISIBLE, 300,40,60,20, parent, (HMENU)IDC_PARAM_START, NULL, NULL);
    if (!hBtnStart) MessageBoxW(NULL, L"Failed to create hBtnStart", L"Debug", MB_OK|MB_ICONERROR);
    
    // Server ID row (generated when server starts)
    CreateWindowExW(0, L"STATIC", L"Server ID:", WS_CHILD|WS_VISIBLE, 16,70,60,20, parent, NULL, NULL, NULL);
    hEditServerId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"(Start server to generate)", 
                                   WS_CHILD|WS_VISIBLE|ES_READONLY|ES_CENTER, 80,68,180,22, parent, (HMENU)IDC_SERVER_ID, NULL, NULL);
    hBtnCopyId = CreateWindowExW(0, L"BUTTON", L"Copy", WS_CHILD|WS_VISIBLE|WS_DISABLED, 265,68,50,22, parent, (HMENU)IDC_COPY_ID, NULL, NULL);
    
    // Console tab controls
    hEditConsole = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL, 16,100,400,140, parent, (HMENU)IDC_CONSOLE, NULL, NULL);
    if (!hEditConsole) MessageBoxW(NULL, L"Failed to create hEditConsole", L"Debug", MB_OK|MB_ICONERROR);
}

// Tab switching logic
static void OnTabChange(HWND hwnd) {
    int sel = TabCtrl_GetCurSel(hTab);
    // Parameters tab (always visible on tab 0)
    ShowWindow(hEditIP, sel == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditPort, sel == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnStart, sel == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditServerId, sel == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnCopyId, sel == 0 ? SW_SHOW : SW_HIDE);
    // Console tab
    ShowWindow(hEditConsole, sel == 1 ? SW_SHOW : SW_HIDE);
}


// Minimal relay logic for Start/Stop button (expand as needed)
static void AppendConsoleA(const char* text) {
    if (hEditConsole && IsWindow(hEditConsole)) {
        int len = GetWindowTextLengthA(hEditConsole);
        SendMessageA(hEditConsole, EM_SETSEL, len, len);
        SendMessageA(hEditConsole, EM_REPLACESEL, FALSE, (LPARAM)text);
    }
}

/* Relay log callback - posts message to main thread for thread safety */
static void RelayLogCallback(const char* message) {
    char* msgCopy;
    if (!g_hMainWnd || !IsWindow(g_hMainWnd)) return;
    
    /* Allocate copy of message - will be freed by main thread */
    msgCopy = (char*)malloc(strlen(message) + 1);
    if (msgCopy) {
        strcpy(msgCopy, message);
        /* Post to main thread - if fails, free memory */
        if (!PostMessageA(g_hMainWnd, WM_RELAY_LOG, 0, (LPARAM)msgCopy)) {
            free(msgCopy);
        }
    }
}

static void StartRelayServer(HWND hwnd) {
    char ip[64] = {0};
    char portStr[16] = {0};
    char serverId[SERVER_ID_MAX_LEN] = {0};
    WORD port = 0;
    int result = 0;
    
    /* Initialize crypto for XOR encryption */
    Crypto_Init(NULL);
    
    /* Set log callback to route messages to console */
    Relay_SetLogCallback(RelayLogCallback);
    
    // Get IP and port from edit controls
    GetWindowTextA(hEditIP, ip, sizeof(ip)-1);
    GetWindowTextA(hEditPort, portStr, sizeof(portStr)-1);
    port = (WORD)atoi(portStr);
    if (port == 0) port = RELAY_DEFAULT_PORT;
    
    // Create and start relay server
    g_pRelayServer = Relay_Create(port, ip);
    if (!g_pRelayServer) {
        int wsaerr = WSAGetLastError();
        char errbuf[128];
        _snprintf(errbuf, sizeof(errbuf), "[ERROR] Failed to create relay server. WSAGetLastError=%d\r\n", wsaerr);
        AppendConsoleA(errbuf);
        SetWindowTextW(hBtnStart, L"Start");
        relayRunning = FALSE;
        return;
    }
    result = Relay_Start(g_pRelayServer);
    if (result != 0) {
        int wsaerr = WSAGetLastError();
        char errbuf[128];
        _snprintf(errbuf, sizeof(errbuf), "[ERROR] Failed to start relay server. WSAGetLastError=%d\r\n", wsaerr);
        AppendConsoleA(errbuf);
        Relay_Destroy(g_pRelayServer);
        g_pRelayServer = NULL;
        SetWindowTextW(hBtnStart, L"Start");
        relayRunning = FALSE;
        return;
    }
    
    /* Generate Server ID from IP:Port for clients to use */
    /* If IP is 0.0.0.0, try to get actual LAN IP */
    if (strcmp(ip, "0.0.0.0") == 0) {
        char hostname[256];
        struct hostent *he;
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            he = gethostbyname(hostname);
            if (he && he->h_addr_list[0]) {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[0], sizeof(addr));
                strncpy(ip, inet_ntoa(addr), sizeof(ip)-1);
            }
        }
        /* If still 0.0.0.0, use localhost placeholder */
        if (strcmp(ip, "0.0.0.0") == 0) {
            strcpy(ip, "127.0.0.1");
        }
    }
    
    result = Crypto_EncodeServerID(ip, port, serverId);
    if (result == CRYPTO_SUCCESS) {
        SetWindowTextA(hEditServerId, serverId);
        EnableWindow(hBtnCopyId, TRUE);
        AppendConsoleA("[INFO] Server ID generated: ");
        AppendConsoleA(serverId);
        AppendConsoleA("\r\n[INFO] Distribute this ID to your client users.\r\n");
        /* Save config for next startup */
        SaveRelayConfig(ip, portStr, serverId);
    } else {
        SetWindowTextA(hEditServerId, "(Failed to generate)");
        EnableWindow(hBtnCopyId, FALSE);
    }
    
    SetWindowTextW(hBtnStart, L"Stop");
    AppendConsoleA("[INFO] Relay server started.\r\n");
    relayRunning = TRUE;
}

static void StopRelayServer(HWND hwnd) {
    /* Clear log callback */
    Relay_SetLogCallback(NULL);
    
    if (g_pRelayServer) {
        Relay_Stop(g_pRelayServer);
        Relay_Destroy(g_pRelayServer);
        g_pRelayServer = NULL;
    }
    
    /* Cleanup crypto */
    Crypto_Cleanup();
    
    /* Clear Server ID display */
    SetWindowTextA(hEditServerId, "(Start server to generate)");
    EnableWindow(hBtnCopyId, FALSE);
    
    SetWindowTextW(hBtnStart, L"Start");
    AppendConsoleA("[INFO] Relay server stopped.\r\n");
    relayRunning = FALSE;
}

// Entry point for the relay GUI
int RelayGui_Run(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSW wc;
    MSG msg;
    HWND hwnd;
    WSADATA wsaData;
    int wsaInit = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (wsaInit != 0) {
        MessageBoxA(NULL, "WSAStartup failed!", "Error", MB_OK|MB_ICONERROR);
        return 1;
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RelayGuiWnd";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"RelayGuiWnd", L"RemoteDesk2K Relay Server", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 320, NULL, NULL, hInstance, NULL);
    g_hMainWnd = hwnd;  /* Store for thread-safe logging */
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    WSACleanup();
    return 0;
}

// Main window procedure implementation
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES };
        InitCommonControlsEx(&icc);
        RECT rc;
        GetClientRect(hwnd, &rc);
        hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
            0,0,rc.right,rc.bottom, hwnd, (HMENU)IDC_TAB, NULL, NULL);
        TCITEMW tie = { TCIF_TEXT };
        static wchar_t paramText[] = L"Parameters";
        static wchar_t consoleText[] = L"Console";
        tie.pszText = paramText;
        SendMessageW(hTab, TCM_INSERTITEMW, 0, (LPARAM)&tie);
        tie.pszText = consoleText;
        SendMessageW(hTab, TCM_INSERTITEMW, 1, (LPARAM)&tie);
        CreateAllControls(hwnd, rc);
        OnTabChange(hwnd);
        /* Load saved config */
        LoadRelayConfig();
        break; }
    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->idFrom == IDC_TAB && ((LPNMHDR)lParam)->code == TCN_SELCHANGE) {
            OnTabChange(hwnd);
        }
        break;
    case WM_COMMAND:
        if ((HWND)lParam == hBtnStart) {
            if (!relayRunning) {
                StartRelayServer(hwnd);
            } else {
                StopRelayServer(hwnd);
            }
        }
        else if ((HWND)lParam == hBtnCopyId) {
            /* Copy Server ID to clipboard */
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
                    AppendConsoleA("[INFO] Server ID copied to clipboard.\r\n");
                }
            }
        }
        break;
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(hTab, 0,0,rc.right,rc.bottom, TRUE);
        // Move parameter controls
        MoveWindow(hEditIP, 80,40,100,20, TRUE);
        MoveWindow(hEditPort, 230,40,60,20, TRUE);
        MoveWindow(hBtnStart, 300,40,60,20, TRUE);
        // Move Server ID controls
        MoveWindow(hEditServerId, 80,68,180,22, TRUE);
        MoveWindow(hBtnCopyId, 265,68,50,22, TRUE);
        // Move console control
        MoveWindow(hEditConsole, 16,100,400,140, TRUE);
        break; }
    case WM_CLOSE:
        g_hMainWnd = NULL;  /* Prevent more log messages */
        StopRelayServer(hwnd);
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        g_hMainWnd = NULL;
        PostQuitMessage(0);
        break;
    default:
        /* Handle custom log message from worker threads */
        if (msg == WM_RELAY_LOG) {
            char* logText = (char*)lParam;
            if (logText) {
                AppendConsoleA(logText);
                free(logText);  /* Free the allocated copy */
            }
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}



// relay_gui.c - Windows 2000 compatible GUI for relay server
#include "relay_gui.h"



