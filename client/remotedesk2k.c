/*
 * RemoteDesk2K - Unified Remote Desktop Application
 * Similar to UltraViewer/AnyDesk interface
 * 
 * Features:
 * - Combined Server/Client in one window
 * - ID and Password based connection
 * - File transfer via clipboard
 * - Full screen and scaling options
 */

#include "common.h"
#include "screen.h"
#include "network.h"
#include "input.h"
#include "clipboard.h"
#include "filetransfer.h"
#include "progress.h"
#include "relay.h"
#include "crypto.h"
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

/* Client config file */
#define CLIENT_CONFIG_FILE      "client_config.ini"
#define CLIENT_CONFIG_SECTION   "Client"
static char             g_szClientConfigPath[MAX_PATH] = {0};

/* Application */
#define APP_TITLE           "RemoteDesk2K"
#define MAIN_WND_CLASS      "RD2KMainWndClass"
#define VIEWER_WND_CLASS    "RD2KViewerWndClass"

/* Control IDs - Left Panel (Allow Remote Control) */
#define IDC_LEFT_PANEL          100
#define IDC_YOUR_ID_LABEL       101
#define IDC_YOUR_ID             102
#define IDC_YOUR_PWD_LABEL      103
#define IDC_YOUR_PWD            104
#define IDC_COPY_ID_BTN         105
#define IDC_REFRESH_PWD_BTN     106
#define IDC_CUSTOM_PWD_LABEL    107
#define IDC_CUSTOM_PWD          108
#define IDC_SET_PWD_BTN         109
#define IDC_AUTOSTART_CHK       110
#define IDC_PREVENT_SLEEP_CHK   111
#define IDC_LEFT_HEADER         112   /* Allow Remote Control header */
#define IDC_LEFT_INSTR          113   /* Left panel instructions */

/* Control IDs - Right Panel (Control Remote Computer) */
#define IDC_RIGHT_PANEL         200
#define IDC_PARTNER_ID_LABEL    201
#define IDC_PARTNER_ID          202
#define IDC_PARTNER_PWD_LABEL   203
#define IDC_PARTNER_PWD         204
#define IDC_CONNECT_BTN         205
#define IDC_TAB_CTRL            206

/* Control IDs - Server Config Tab (Relay Connect) */
#define IDC_SERVER_ID_LABEL     210   /* "Server ID" label */
#define IDC_SERVER_ID           211   /* Encrypted Server ID field (given by admin) */
#define IDC_RELAY_PARTNER_ID_LABEL  216
#define IDC_RELAY_PARTNER_ID    217
#define IDC_RELAY_PARTNER_PWD_LABEL 218
#define IDC_RELAY_PARTNER_PWD   219
#define IDC_RELAY_CONNECT_SVR_BTN   220   /* Step 1: Connect to server */
#define IDC_RELAY_CONNECT_PARTNER_BTN 221 /* Step 2: Connect to partner */
#define IDC_INSTR_TAB1              222   /* Instruction for Direct Connect tab */
#define IDC_INSTR_TAB2              223   /* Instruction for Server tab */

/* Status and Menu */
#define IDC_STATUSBAR           300
#define IDC_STATUS_ICON         301
#define IDM_FILE_EXIT           400
#define IDM_SETTINGS            401
#define IDM_HELP_ABOUT          402
#define IDM_VIEW_FULLSCREEN     403
#define IDM_VIEW_STRETCH        404
#define IDM_VIEW_ACTUAL         405

/* Viewer Window */
#define IDM_VIEWER_FULLSCREEN   500
#define IDM_VIEWER_STRETCH      501
#define IDM_VIEWER_ACTUAL       502
#define IDM_VIEWER_DISCONNECT   503
#define IDM_VIEWER_SENDFILE     504
#define IDM_VIEWER_CLIPBOARD    505
#define IDM_VIEWER_REFRESH      506
#define IDM_VIEWER_RECEIVEFILE  507  /* Receive files FROM remote */

/* Fullscreen Toolbar Buttons */
#define IDC_TB_DISCONNECT       600
#define IDC_TB_SENDFILE         601
#define IDC_TB_CLIPBOARD        602
#define IDC_TB_FULLSCREEN       603
#define IDC_TB_STRETCH          604
#define IDC_TB_REFRESH          605
#define IDC_TB_RECEIVEFILE      606

/* Timer IDs */
#define TIMER_NETWORK           1
#define TIMER_SCREEN            2
#define TIMER_PING              3
#define TIMER_LISTEN_CHECK      4
#define TIMER_TOOLBAR_HIDE      5
#define TIMER_CLIPBOARD_REQUEST 6
#define TIMER_RELAY_CHECK       7

/* Custom Window Messages for async operations */
#define WM_APP_CONNECT_RESULT   (WM_APP + 1)  /* wParam: result code, lParam: mode (0=direct, 1=relay) */
#define WM_APP_CONNECT_STATUS   (WM_APP + 2)  /* wParam: 0, lParam: pointer to status string */

/* Intervals */
#define NETWORK_INTERVAL        10
#define SCREEN_INTERVAL         100
#define PING_INTERVAL           5000
#define LISTEN_CHECK_INTERVAL   100
#define TOOLBAR_HIDE_DELAY      3000
#define RELAY_CHECK_INTERVAL    30000 /* Send relay keepalive every 30 seconds */

/* Colors */
#define COLOR_PANEL_BG          GetSysColor(COLOR_INFOBK)  /* Windows classic InfoBackground */
#define COLOR_HEADER_BG         RGB(100, 149, 237)  /* CornflowerBlue */
#define COLOR_HEADER_TEXT       RGB(255, 255, 255)
#define COLOR_ID_BG             RGB(255, 255, 255)
#define COLOR_STATUS_OK         RGB(0, 200, 0)
#define COLOR_STATUS_WARN       RGB(255, 165, 0)

/* Window dimensions */
#define MAIN_WIDTH              640
#define MAIN_HEIGHT             420

/* Global Variables */
static HINSTANCE        g_hInstance = NULL;
static HWND             g_hMainWnd = NULL;
static HWND             g_hStatusBar = NULL;
static HWND             g_hViewerWnd = NULL;
static HFONT            g_hFontNormal = NULL;
static HFONT            g_hFontBold = NULL;
static HFONT            g_hFontLarge = NULL;
static HBRUSH           g_hBrushPanel = NULL;
static HBRUSH           g_hBrushHeader = NULL;

/* Left Panel Controls */
static HWND             g_hYourId = NULL;
static HWND             g_hYourPwd = NULL;
static HWND             g_hCopyIdBtn = NULL;
static HWND             g_hRefreshPwdBtn = NULL;
static HWND             g_hCustomPwd = NULL;
static HWND             g_hSetPwdBtn = NULL;

/* Right Panel Controls - Tab 1: Control Remote */
static HWND             g_hPartnerId = NULL;
static HWND             g_hPartnerPwd = NULL;
static HWND             g_hConnectBtn = NULL;

/* Right Panel Controls - Tab Control */
static HWND             g_hTabCtrl = NULL;
static int              g_nCurrentTab = 0;

/* Right Panel Controls - Tab 2: Relay Connect */
static HWND             g_hRelayPartnerId = NULL;   /* Partner ID for relay connection */
static HWND             g_hRelayPartnerPwd = NULL;  /* Password for relay connection */
static HWND             g_hServerId = NULL;          /* Relay address field */
static HWND             g_hRelayConnectSvrBtn = NULL;  /* Step 1: Connect to server */
static HWND             g_hRelayConnectPartnerBtn = NULL; /* Step 2: Connect to partner */
static BOOL             g_bConnectedToRelay = FALSE;  /* TRUE when registered with relay */

/* Relay connection settings (from domain:port, IP:port, or decoded Server ID) */
static char             g_szRelayServerIp[128] = ""; /* Domain/IP for relay server */
static WORD             g_wRelayServerPort = 5000;   /* Port for relay server */
static SOCKET           g_relaySocket = INVALID_SOCKET;
static CRITICAL_SECTION g_csRelay;
static HANDLE           g_hMutex = NULL;  /* Single instance mutex */

/* Single instance mutex name */
#define CLIENT_MUTEX_NAME    "RemoteDesk2K_Client_SingleInstance"

/* Connection State */
static DWORD            g_myId = 0;
static DWORD            g_myPassword = 0;
static char             g_customPassword[32] = {0};
static BOOL             g_useCustomPassword = FALSE;

/* Server State (being controlled) */
static PRD2K_NETWORK    g_pServerNet = NULL;
static PSCREEN_CAPTURE  g_pCapture = NULL;
static BOOL             g_bServerRunning = FALSE;
static BOOL             g_bClientConnected = FALSE;

/* Client State (controlling) */
static PRD2K_NETWORK    g_pClientNet = NULL;
static BOOL             g_bClientConnected2 = FALSE;
static RD2K_SCREEN_INFO g_remoteScreen = {0};
static HDC              g_hdcViewer = NULL;
static HBITMAP          g_hViewerBitmap = NULL;
static HBITMAP          g_hViewerBitmapOld = NULL;
static BYTE            *g_pViewerPixels = NULL;
static BYTE            *g_pDecompressBuffer = NULL;
static DWORD            g_decompressBufferSize = 0;
static int              g_displayMode = DISPLAY_STRETCH;
static BOOL             g_bFullscreen = FALSE;
static RECT             g_rcViewerNormal = {0};
static DWORD            g_dwViewerStyle = 0;

/* Clipboard monitoring */
static HWND             g_hNextClipViewer = NULL;
static BOOL             g_bIgnoreClipboard = FALSE;
static DWORD            g_lastClipboardSeq = 0;

/* Fullscreen toolbar */
static HWND             g_hToolbarWnd = NULL;
static BOOL             g_bToolbarVisible = FALSE;
static int              g_toolbarHeight = 32;

/* Async connect thread data */
typedef struct _CONNECT_THREAD_DATA {
    BOOL    bRelayMode;      /* TRUE = relay, FALSE = direct */
    DWORD   partnerId;
    DWORD   partnerPwd;
    char    host[64];        /* For direct connect */
    int     result;          /* RD2K_SUCCESS or error code */
    char    errorMsg[256];
    /* Filled in on success for creating viewer */
    WORD    remoteWidth;
    WORD    remoteHeight;
    BYTE    remoteBpp;
    BYTE    remoteCompression;
} CONNECT_THREAD_DATA, *PCONNECT_THREAD_DATA;

static HANDLE           g_hConnectThread = NULL;
static BOOL             g_bConnecting = FALSE;  /* TRUE while connection in progress */

/* File transfer state - now handled by filetransfer module */

/* Function prototypes */
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void CreateMainControls(HWND hwnd);
void SwitchTab(int tabIndex);
void ConnectToRelayServer(void);
void ConnectToPartnerViaRelay(void);
void UpdateStatusBar(const char *text, BOOL connected);
void RefreshMainWindow(void);
void StartServer(void);
void StopServer(void);
void ConnectToPartner(void);
void DisconnectFromPartner(void);
void ProcessServerNetwork(void);
void ProcessClientNetwork(void);
void SendScreenUpdate(void);
void HandleScreenUpdate(const BYTE *data, DWORD dataLength);
void HandleMouseEvent(const RD2K_MOUSE_EVENT *pEvent);
void HandleKeyboardEvent(const RD2K_KEY_EVENT *pEvent);
void SendMouseEvent(HWND hwnd, int x, int y, BYTE buttons, BYTE flags, SHORT wheel);
void SendKeyboardEvent(WORD vk, WORD scan, BYTE flags);
void SendClipboardData(PRD2K_NETWORK pNet);
void ReceiveClipboardText(const BYTE *data, DWORD length);
void SendFileToRemote(const char *filePath);
void SendClipboardFiles(PRD2K_NETWORK pNet);
static DWORD WINAPI ConnectThreadProc(LPVOID lpParam);
void HandleConnectResult(int result, BOOL bRelayMode, PCONNECT_THREAD_DATA pData);
void ReceiveFileStart(const BYTE *data, DWORD length);
void ReceiveFileData(const BYTE *data, DWORD length);
void ReceiveFileEnd(void);
BOOL CreateViewerWindow(void);
void DestroyViewerWindow(void);
HMENU CreateViewerMenu(void);
BOOL CreateViewerBitmap(void);
void DestroyViewerBitmap(void);
void ToggleFullscreen(void);
void RefreshPassword(void);
void CopyIdToClipboard(void);
void CreateFullscreenToolbar(void);
void DestroyFullscreenToolbar(void);
void ShowFullscreenToolbar(void);
void HideFullscreenToolbar(void);
void CheckClipboardAndSync(void);

/* Reconnection state */
static BOOL             g_bReconnecting = FALSE;
static int              g_nReconnectAttempt = 0;
static HWND             g_hReconnectDlg = NULL;
static DWORD            g_dwLastPartnerId = 0;
static DWORD            g_dwLastPartnerPwd = 0;

/* Reconnection function prototypes */
void HandleRelayServerLost(void);
void HandlePartnerDisconnect(BOOL isServerSide);
BOOL AttemptRelayReconnection(void);
INT_PTR CALLBACK ReconnectDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void DisconnectFromRelayServer(BOOL silent);

/* ========================================================================== 
 * CLIENT CONFIG FILE FUNCTIONS - Save/Load client settings
 * ========================================================================== */

/* Get config file path (same directory as exe) */
static void GetClientConfigPath(void) {
    char *lastSlash;
    GetModuleFileNameA(NULL, g_szClientConfigPath, sizeof(g_szClientConfigPath));
    lastSlash = strrchr(g_szClientConfigPath, '\\');
    if (lastSlash) {
        lastSlash[1] = '\0';
    }
    strcat(g_szClientConfigPath, CLIENT_CONFIG_FILE);
}

/* Save client config to INI file */
static void SaveClientConfig(void) {
    char serverIdStr[128], partnerIdStr[32];
    
    if (g_szClientConfigPath[0] == '\0') GetClientConfigPath();
    
    /* Get Relay Address */
    if (g_hServerId) {
        GetWindowTextA(g_hServerId, serverIdStr, sizeof(serverIdStr));
        WritePrivateProfileStringA(CLIENT_CONFIG_SECTION, "ServerID", serverIdStr, g_szClientConfigPath);
    }
    
    /* Get Partner ID (relay tab) */
    if (g_hRelayPartnerId) {
        GetWindowTextA(g_hRelayPartnerId, partnerIdStr, sizeof(partnerIdStr));
        WritePrivateProfileStringA(CLIENT_CONFIG_SECTION, "LastPartnerID", partnerIdStr, g_szClientConfigPath);
    }
    
    /* Get Partner ID (direct tab) */
    if (g_hPartnerId) {
        GetWindowTextA(g_hPartnerId, partnerIdStr, sizeof(partnerIdStr));
        WritePrivateProfileStringA(CLIENT_CONFIG_SECTION, "LastDirectPartnerID", partnerIdStr, g_szClientConfigPath);
    }
}

/* Load client config from INI file */
static void LoadClientConfig(void) {
    char serverIdStr[128], partnerIdStr[32], directPartnerIdStr[32];
    
    if (g_szClientConfigPath[0] == '\0') GetClientConfigPath();
    
    GetPrivateProfileStringA(CLIENT_CONFIG_SECTION, "ServerID", "", serverIdStr, sizeof(serverIdStr), g_szClientConfigPath);
    GetPrivateProfileStringA(CLIENT_CONFIG_SECTION, "LastPartnerID", "", partnerIdStr, sizeof(partnerIdStr), g_szClientConfigPath);
    GetPrivateProfileStringA(CLIENT_CONFIG_SECTION, "LastDirectPartnerID", "", directPartnerIdStr, sizeof(directPartnerIdStr), g_szClientConfigPath);
    
    /* Apply loaded values to controls */
    if (g_hServerId && serverIdStr[0] != '\0') {
        SetWindowTextA(g_hServerId, serverIdStr);
    }
    if (g_hRelayPartnerId && partnerIdStr[0] != '\0') {
        SetWindowTextA(g_hRelayPartnerId, partnerIdStr);
    }
    if (g_hPartnerId && directPartnerIdStr[0] != '\0') {
        SetWindowTextA(g_hPartnerId, directPartnerIdStr);
    }
}

/* Entry Point */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEX wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    
    g_hInstance = hInstance;
    
    /* Check for single instance */
    g_hMutex = CreateMutexA(NULL, TRUE, CLIENT_MUTEX_NAME);
    if (g_hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(NULL, "RemoteDesk2K is already running!\n\nCheck the taskbar or system tray.", APP_TITLE, MB_ICONWARNING | MB_OK);
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }
    
    /* Initialize Winsock */
    if (InitializeNetwork() != 0) {
        MessageBoxA(NULL, "Failed to initialize network!", APP_TITLE, MB_ICONERROR);
        return 1;
    }
    
    /* Initialize crypto module for secure ID encoding */
    Crypto_Init(NULL);
    
    /* Initialize relay critical section for thread safety */
    InitializeCriticalSection(&g_csRelay);
    
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
    g_hFontLarge = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    
    /* Create brushes - use system color brush for proper matching */
    g_hBrushPanel = GetSysColorBrush(COLOR_INFOBK);
    g_hBrushHeader = CreateSolidBrush(COLOR_HEADER_BG);
    
    /* Generate ID and password */
    g_myId = GenerateUniqueId();
    g_myPassword = GeneratePassword();
    
    /* Register main window class */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushPanel;
    wc.lpszClassName = MAIN_WND_CLASS;
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    
    if (!RegisterClassExA(&wc)) {
        CleanupNetwork();
        return 1;
    }
    
    /* Register viewer window class */
    wc.lpfnWndProc = ViewerWndProc;
    wc.lpszClassName = VIEWER_WND_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExA(&wc);
    
    /* Register toolbar window class for fullscreen mode */
    wc.lpfnWndProc = ToolbarWndProc;
    wc.lpszClassName = "RD2KToolbarClass";
    wc.hbrBackground = CreateSolidBrush(RGB(50, 50, 50));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExA(&wc);
    
    /* Create main window */
    g_hMainWnd = CreateWindowExA(
        0, MAIN_WND_CLASS, APP_TITLE " - Free",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, MAIN_WIDTH, MAIN_HEIGHT,
        NULL, NULL, hInstance, NULL);
    
    if (!g_hMainWnd) {
        CleanupNetwork();
        return 1;
    }
    
    /* Initialize clipboard module */
    Clipboard_Init(g_hMainWnd);
    
    /* Initialize file transfer module */
    FileTransfer_Init(g_hMainWnd, hInstance);
    
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);
    
    /* Force initial refresh to fix display artifacts on startup */
    RefreshMainWindow();
    
    /* Start server automatically */
    StartServer();
    
    /* Message loop */
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    /* Cleanup */
    StopServer();
    DisconnectFromPartner();
    FileTransfer_Cleanup();
    Clipboard_Cleanup();
    Crypto_Cleanup();
    
    /* Cleanup relay resources */
    if (g_relaySocket != INVALID_SOCKET) {
        closesocket(g_relaySocket);
        g_relaySocket = INVALID_SOCKET;
    }
    DeleteCriticalSection(&g_csRelay);
    
    DeleteObject(g_hFontNormal);
    DeleteObject(g_hFontBold);
    DeleteObject(g_hFontLarge);
    /* Note: g_hBrushPanel is a system brush - do not delete */
    DeleteObject(g_hBrushHeader);
    
    CleanupNetwork();
    
    /* Release single instance mutex */
    if (g_hMutex) {
        CloseHandle(g_hMutex);
        g_hMutex = NULL;
    }
    
    return (int)msg.wParam;
}

/* Create main window controls */
void CreateMainControls(HWND hwnd)
{
    char idStr[32], pwdStr[16];
    int panelWidth = 295;
    int leftX = 10, rightX = 320;
    int y;
    HWND hLeftPanel;

    /* Create left panel background */
    hLeftPanel = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        leftX, 10, panelWidth, MAIN_HEIGHT - 40,
        hwnd, (HMENU)IDC_LEFT_PANEL, g_hInstance, NULL);
    
    /* Suppress unused variable warning */
    (void)hLeftPanel;

    /* --- LEFT PANEL: Allow Remote Control --- */
    
    /* Header */
    CreateWindowExA(0, "STATIC", "  Allow Remote Control",
                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                   leftX, 10, panelWidth, 30, hwnd, (HMENU)IDC_LEFT_HEADER, g_hInstance, NULL);
    
    /* Instructions */
    CreateWindowExA(0, "STATIC", 
                   "Please tell your partner the following ID\nand Password to allow remote control",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   leftX + 10, 50, panelWidth - 20, 36, hwnd, (HMENU)IDC_LEFT_INSTR, g_hInstance, NULL);
    
    y = 95;
    
    /* Your ID label */
    CreateWindowExA(0, "STATIC", "Your ID",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   leftX + 10, y, 70, 20, hwnd, (HMENU)IDC_YOUR_ID_LABEL, g_hInstance, NULL);
    
    /* Your ID display */
    FormatId(g_myId, idStr);
    g_hYourId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", idStr,
                               WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
                               leftX + 80, y - 2, 160, 24, hwnd, (HMENU)IDC_YOUR_ID, g_hInstance, NULL);
    
    /* Copy button */
    g_hCopyIdBtn = CreateWindowExA(0, "BUTTON", "Copy",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  leftX + 245, y - 2, 40, 24, hwnd, (HMENU)IDC_COPY_ID_BTN, g_hInstance, NULL);
    
    y += 35;
    
    /* Password label */
    CreateWindowExA(0, "STATIC", "Password",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   leftX + 10, y, 70, 20, hwnd, (HMENU)IDC_YOUR_PWD_LABEL, g_hInstance, NULL);
    
    /* Password display */
    sprintf(pwdStr, "%d", g_myPassword);
    g_hYourPwd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", pwdStr,
                                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
                                leftX + 80, y - 2, 160, 24, hwnd, (HMENU)IDC_YOUR_PWD, g_hInstance, NULL);
    
    /* Refresh button */
    g_hRefreshPwdBtn = CreateWindowExA(0, "BUTTON", "!",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      leftX + 245, y - 2, 24, 24, hwnd, (HMENU)IDC_REFRESH_PWD_BTN, g_hInstance, NULL);
    
    y += 45;
    
    /* Separator line */
    CreateWindowExA(0, "STATIC", "",
                   WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                   leftX + 10, y, panelWidth - 20, 2, hwnd, NULL, g_hInstance, NULL);
    
    y += 15;
    
    /* Custom Password section */
    CreateWindowExA(0, "STATIC", "Custom",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   leftX + 10, y, 80, 20, hwnd, (HMENU)IDC_CUSTOM_PWD_LABEL, g_hInstance, NULL);
    
    y += 22;
    
    g_hCustomPwd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                  WS_CHILD | WS_VISIBLE | ES_PASSWORD,
                                  leftX + 10, y, 160, 24, hwnd, (HMENU)IDC_CUSTOM_PWD, g_hInstance, NULL);
    
    g_hSetPwdBtn = CreateWindowExA(0, "BUTTON", "Set",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  leftX + 175, y, 50, 24, hwnd, (HMENU)IDC_SET_PWD_BTN, g_hInstance, NULL);

    /* --- RIGHT PANEL: Control a Remote Computer --- */
    {
        TCITEMA tabItem;
        int tabY = 45;
        int contentY;
        
        /* Header - use IDC_RIGHT_PANEL for blue coloring */
        CreateWindowExA(0, "STATIC", "  Control a Remote Computer",
                       WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                       rightX, 10, panelWidth, 30, hwnd, (HMENU)IDC_RIGHT_PANEL, g_hInstance, NULL);

        /* Tab Control - placed below header */
        g_hTabCtrl = CreateWindowExA(0, WC_TABCONTROLA, "",
                       WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
                       rightX, tabY, panelWidth, 25, hwnd, (HMENU)IDC_TAB_CTRL, g_hInstance, NULL);
        SendMessage(g_hTabCtrl, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        
        /* Add tabs */
        ZeroMemory(&tabItem, sizeof(tabItem));
        tabItem.mask = TCIF_TEXT;
        tabItem.pszText = "Direct Connect";
        TabCtrl_InsertItem(g_hTabCtrl, 0, &tabItem);
        tabItem.pszText = "Server";
        TabCtrl_InsertItem(g_hTabCtrl, 1, &tabItem);
        
        contentY = tabY + 30;

        /* Instructions for Tab 1: Direct Connect */
        CreateWindowExA(0, "STATIC",
                       "Please enter your partner's ID to remote\ncontrol your partner's computer",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       rightX + 10, contentY, panelWidth - 20, 36, hwnd, (HMENU)IDC_INSTR_TAB1, g_hInstance, NULL);

        /* Instructions for Tab 2: Server (hidden initially) */
        CreateWindowExA(0, "STATIC",
                       "Please enter your partner's ID to remote\ncontrol your partner's computer",
                       WS_CHILD | SS_LEFT,
                       rightX + 10, contentY, panelWidth - 20, 36, hwnd, (HMENU)IDC_INSTR_TAB2, g_hInstance, NULL);

        /* ---- TAB 1: Direct Connect Controls ---- */
        y = contentY + 45;
        
        /* Partner ID label and input */
        CreateWindowExA(0, "STATIC", "Partner ID",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       rightX + 10, y, 70, 20, hwnd, (HMENU)IDC_PARTNER_ID_LABEL, g_hInstance, NULL);
        g_hPartnerId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                      WS_CHILD | WS_VISIBLE | ES_CENTER,
                                      rightX + 80, y - 2, 200, 24, hwnd, (HMENU)IDC_PARTNER_ID, g_hInstance, NULL);
        
        y += 35;
        
        /* Partner Password label and input */
        CreateWindowExA(0, "STATIC", "Password",
                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                       rightX + 10, y, 70, 20, hwnd, (HMENU)IDC_PARTNER_PWD_LABEL, g_hInstance, NULL);
        g_hPartnerPwd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                       WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_CENTER,
                                       rightX + 80, y - 2, 200, 24, hwnd, (HMENU)IDC_PARTNER_PWD, g_hInstance, NULL);
        
        y += 45;
        
        /* Connect button */
        g_hConnectBtn = CreateWindowExA(0, "BUTTON", "Connect Direct",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       rightX + 70, y, 160, 30, hwnd, (HMENU)IDC_CONNECT_BTN, g_hInstance, NULL);

        /* ---- TAB 2: Relay Connect Controls (initially hidden) ---- */
        /* Step 1: Connect to relay server first */
        /* Step 2: Then connect to partner by ID */
        y = contentY + 45;
        
        /* Relay Address (domain:port, IP:port, or Server ID) */
        CreateWindowExA(0, "STATIC", "Relay Addr",
                       WS_CHILD | SS_LEFT,
                       rightX + 10, y, 70, 20, hwnd, (HMENU)IDC_SERVER_ID_LABEL, g_hInstance, NULL);
        g_hServerId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                     WS_CHILD | ES_LEFT,
                                     rightX + 80, y - 2, 200, 24, hwnd, (HMENU)IDC_SERVER_ID, g_hInstance, NULL);
        
        y += 30;
        
        /* Step 1: Connect to Server button */
        g_hRelayConnectSvrBtn = CreateWindowExA(0, "BUTTON", "Connect to Server",
                                               WS_CHILD | BS_PUSHBUTTON,
                                               rightX + 70, y, 160, 26, hwnd, (HMENU)IDC_RELAY_CONNECT_SVR_BTN, g_hInstance, NULL);
        
        y += 35;
        
        /* Partner ID for relay connection */
        CreateWindowExA(0, "STATIC", "Partner ID",
                       WS_CHILD | SS_LEFT,
                       rightX + 10, y, 70, 20, hwnd, (HMENU)IDC_RELAY_PARTNER_ID_LABEL, g_hInstance, NULL);
        g_hRelayPartnerId = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                           WS_CHILD | ES_CENTER,
                                           rightX + 80, y - 2, 200, 24, hwnd, (HMENU)IDC_RELAY_PARTNER_ID, g_hInstance, NULL);
        
        y += 30;
        
        /* Password for relay connection */
        CreateWindowExA(0, "STATIC", "Password",
                       WS_CHILD | SS_LEFT,
                       rightX + 10, y, 70, 20, hwnd, (HMENU)IDC_RELAY_PARTNER_PWD_LABEL, g_hInstance, NULL);
        g_hRelayPartnerPwd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                            WS_CHILD | ES_PASSWORD | ES_CENTER,
                                            rightX + 80, y - 2, 200, 24, hwnd, (HMENU)IDC_RELAY_PARTNER_PWD, g_hInstance, NULL);
        
        y += 35;
        
        /* Step 2: Connect to Partner button (disabled until connected to server) */
        g_hRelayConnectPartnerBtn = CreateWindowExA(0, "BUTTON", "Connect to Partner",
                                                   WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
                                                   rightX + 70, y, 160, 26, hwnd, (HMENU)IDC_RELAY_CONNECT_PARTNER_BTN, g_hInstance, NULL);
        
        /* Set fonts for relay connect controls */
        SendMessage(g_hServerId, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        SendMessage(g_hRelayConnectSvrBtn, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        SendMessage(g_hRelayPartnerId, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        SendMessage(g_hRelayPartnerPwd, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        SendMessage(g_hRelayConnectPartnerBtn, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    }
    
    /* Set fonts for left panel controls */
    SendMessage(g_hYourId, WM_SETFONT, (WPARAM)g_hFontLarge, TRUE);
    SendMessage(g_hYourPwd, WM_SETFONT, (WPARAM)g_hFontLarge, TRUE);
    SendMessage(g_hPartnerId, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hPartnerPwd, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hConnectBtn, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    
    /* Create status bar */
    g_hStatusBar = CreateStatusWindowA(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                      "Ready to connect", hwnd, IDC_STATUSBAR);
}

/* Switch between tabs in the right panel */
void SwitchTab(int tabIndex)
{
    int showTab1, showTab2;
    
    g_nCurrentTab = tabIndex;
    
    if (tabIndex == 0) {
        /* Direct Connect tab */
        showTab1 = SW_SHOW;
        showTab2 = SW_HIDE;
    } else {
        /* Relay tab - for connecting TO a relay server */
        showTab1 = SW_HIDE;
        showTab2 = SW_SHOW;
    }
    
    /* Tab 1: Direct Connect controls */
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_INSTR_TAB1), showTab1);
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_PARTNER_ID_LABEL), showTab1);
    ShowWindow(g_hPartnerId, showTab1);
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_PARTNER_PWD_LABEL), showTab1);
    ShowWindow(g_hPartnerPwd, showTab1);
    ShowWindow(g_hConnectBtn, showTab1);
    
    /* Tab 2: Server Connect controls (using encrypted Server ID) */
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_INSTR_TAB2), showTab2);
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_SERVER_ID_LABEL), showTab2);
    ShowWindow(g_hServerId, showTab2);
    ShowWindow(g_hRelayConnectSvrBtn, showTab2);
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_RELAY_PARTNER_ID_LABEL), showTab2);
    ShowWindow(g_hRelayPartnerId, showTab2);
    ShowWindow(GetDlgItem(g_hMainWnd, IDC_RELAY_PARTNER_PWD_LABEL), showTab2);
    ShowWindow(g_hRelayPartnerPwd, showTab2);
    ShowWindow(g_hRelayConnectPartnerBtn, showTab2);
    
    /* Force refresh after tab switch to fix display artifacts */
    RefreshMainWindow();
}

/* Helper: Parse domain:port or IP:port format */
static BOOL ParseAddressPort(const char *input, char *addressOut, size_t addressSize, WORD *portOut)
{
    const char *colonPos;
    size_t hostLen;
    int port;
    
    colonPos = strrchr(input, ':');  /* Use strrchr for IPv6 compatibility */
    if (!colonPos) return FALSE;
    
    hostLen = colonPos - input;
    if (hostLen == 0 || hostLen >= addressSize) return FALSE;
    
    port = atoi(colonPos + 1);
    if (port <= 0 || port > 65535) return FALSE;
    
    strncpy(addressOut, input, hostLen);
    addressOut[hostLen] = '\0';
    *portOut = (WORD)port;
    return TRUE;
}

/* Helper: Check if input looks like domain:port or IP:port (contains ':' and valid port) */
static BOOL LooksLikeAddressPort(const char *input)
{
    const char *colonPos = strrchr(input, ':');
    int port;
    if (!colonPos) return FALSE;
    port = atoi(colonPos + 1);
    return (port > 0 && port <= 65535);
}

/* Step 1: Connect to relay server and register */
void ConnectToRelayServer(void)
{
    char serverIdStr[128];  /* Increased for domain:port */
    int result;
    SOCKET relaySocket = INVALID_SOCKET;
    BOOL useDirect;
    
    /* If already connected, disconnect first */
    if (g_bConnectedToRelay) {
        /* Stop relay check timer */
        KillTimer(g_hMainWnd, TIMER_RELAY_CHECK);
        
        EnterCriticalSection(&g_csRelay);
        if (g_relaySocket != INVALID_SOCKET) {
            closesocket(g_relaySocket);
            g_relaySocket = INVALID_SOCKET;
        }
        g_bConnectedToRelay = FALSE;
        LeaveCriticalSection(&g_csRelay);
        
        SetWindowTextA(g_hRelayConnectSvrBtn, "Connect to Server");
        EnableWindow(g_hRelayConnectPartnerBtn, FALSE);
        UpdateStatusBar("Disconnected from relay", FALSE);
        return;
    }
    
    /* Get Server ID or domain:port */
    GetWindowTextA(g_hServerId, serverIdStr, sizeof(serverIdStr));
    
    if (serverIdStr[0] == '\0') {
        MessageBoxA(g_hMainWnd, "Please enter the relay address!\n\n"
                   "Use domain:port (e.g., relay.example.com:5000)\n"
                   "or a Server ID (e.g., XXXX-XXXX-XXXX-X)",
                   APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    /* Try to parse as domain:port or IP:port first */
    useDirect = LooksLikeAddressPort(serverIdStr);
    
    if (useDirect) {
        /* Direct domain:port or IP:port format */
        if (!ParseAddressPort(serverIdStr, g_szRelayServerIp, sizeof(g_szRelayServerIp), &g_wRelayServerPort)) {
            MessageBoxA(g_hMainWnd, "Invalid address format!\n\n"
                       "Use domain:port (e.g., relay.example.com:5000)\n"
                       "or IP:port (e.g., 192.168.1.1:5000)",
                       APP_TITLE, MB_ICONWARNING);
            return;
        }
    } else {
        /* Try as Server ID (legacy format) */
        if (!Crypto_ValidateServerIDFormat(serverIdStr)) {
            MessageBoxA(g_hMainWnd, "Invalid relay address!\n\n"
                       "Use domain:port (e.g., relay.example.com:5000)\n"
                       "or a Server ID (e.g., XXXX-XXXX-XXXX-X)",
                       APP_TITLE, MB_ICONWARNING);
            return;
        }
        
        result = Crypto_DecodeServerID(serverIdStr, g_szRelayServerIp, &g_wRelayServerPort);
        if (result != CRYPTO_SUCCESS) {
            MessageBoxA(g_hMainWnd, "Invalid Server ID!\n\n"
                       "The Server ID could not be decoded.\n"
                       "Please check with your administrator.",
                       APP_TITLE, MB_ICONERROR);
            return;
        }
    }
    
    UpdateStatusBar("Connecting to relay...", FALSE);
    SetWindowTextA(g_hRelayConnectSvrBtn, "Connecting...");
    EnableWindow(g_hRelayConnectSvrBtn, FALSE);
    
    /* Regenerate ID in case network changed since startup */
    {
        DWORD newId = GenerateUniqueId();
        if (newId != g_myId && newId != 0x7F000001) {
            char idStr[32];
            g_myId = newId;
            FormatId(g_myId, idStr);
            SetWindowTextA(g_hYourId, idStr);
        }
    }
    
    EnterCriticalSection(&g_csRelay);
    
    /* Close any existing relay connection */
    if (g_relaySocket != INVALID_SOCKET) {
        closesocket(g_relaySocket);
        g_relaySocket = INVALID_SOCKET;
    }
    
    /* Connect to relay server */
    result = Relay_ConnectToServer(g_szRelayServerIp, g_wRelayServerPort, g_myId, &relaySocket);
    if (result != RD2K_SUCCESS) {
        LeaveCriticalSection(&g_csRelay);
        UpdateStatusBar("Failed to connect to relay", FALSE);
        SetWindowTextA(g_hRelayConnectSvrBtn, "Connect to Server");
        EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        MessageBoxA(g_hMainWnd, 
                   "Failed to connect to relay server!\n\n"
                   "Make sure the relay server is running.",
                   APP_TITLE, MB_ICONERROR);
        return;
    }
    
    /* Register our ID with relay */
    UpdateStatusBar("Registering with relay...", FALSE);
    result = Relay_Register(relaySocket, g_myId);
    if (result != RD2K_SUCCESS) {
        closesocket(relaySocket);
        LeaveCriticalSection(&g_csRelay);
        UpdateStatusBar("Failed to register", FALSE);
        SetWindowTextA(g_hRelayConnectSvrBtn, "Connect to Server");
        EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        
        /* Show specific error message for duplicate ID */
        if (result == RD2K_ERR_DUPLICATE_ID) {
            MessageBoxA(g_hMainWnd, 
                       "Your ID is already connected to this relay server!\n\n"
                       "This can happen if:\n"
                       "- Another instance of RemoteDesk2K is running\n"
                       "- A previous connection didn't close properly\n\n"
                       "Wait a few seconds and try again.",
                       APP_TITLE, MB_ICONWARNING);
        } else {
            MessageBoxA(g_hMainWnd, "Failed to register with relay server!", APP_TITLE, MB_ICONERROR);
        }
        return;
    }
    
    g_relaySocket = relaySocket;
    g_bConnectedToRelay = TRUE;
    LeaveCriticalSection(&g_csRelay);
    
    /* Save Server ID immediately so it's remembered next time */
    SaveClientConfig();
    
    /* Start timer to check relay connection health */
    SetTimer(g_hMainWnd, TIMER_RELAY_CHECK, RELAY_CHECK_INTERVAL, NULL);
    
    /* Update UI - connected to server, now enable partner connection */
    SetWindowTextA(g_hRelayConnectSvrBtn, "Disconnect Server");
    EnableWindow(g_hRelayConnectSvrBtn, TRUE);
    EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
    UpdateStatusBar("Connected to relay - enter partner ID", TRUE);
}

/* Background thread for connection - keeps UI responsive */
static DWORD WINAPI ConnectThreadProc(LPVOID lpParam)
{
    PCONNECT_THREAD_DATA pData = (PCONNECT_THREAD_DATA)lpParam;
    int result;
    RD2K_HANDSHAKE handshake;
    RD2K_HEADER header;
    int screenW, screenH;
    
    if (!pData) return 1;
    
    ScreenCapture_GetDimensions(&screenW, &screenH);
    
    if (pData->bRelayMode) {
        /* ========== RELAY MODE CONNECTION ========== */
        EnterCriticalSection(&g_csRelay);
        
        /* Request connection to partner */
        result = Relay_RequestPartner(g_relaySocket, pData->partnerId, pData->partnerPwd);
        if (result != RD2K_SUCCESS) {
            LeaveCriticalSection(&g_csRelay);
            pData->result = RD2K_ERR_CONNECT;
            lstrcpyA(pData->errorMsg, "Partner not found on relay server!");
            PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)pData->result, (LPARAM)pData);
            return 0;
        }
        
        /* Wait for relay to pair us - this can take up to 30 seconds */
        result = Relay_WaitForConnection(g_relaySocket, 30000);
        if (result != RD2K_SUCCESS) {
            LeaveCriticalSection(&g_csRelay);
            pData->result = RD2K_ERR_TIMEOUT;
            lstrcpyA(pData->errorMsg, "Relay connection timed out!");
            PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)pData->result, (LPARAM)pData);
            return 0;
        }
        
        LeaveCriticalSection(&g_csRelay);
        
        /* Create client network structure for relay mode */
        g_pClientNet = Network_Create(RD2K_LISTEN_PORT);
        if (!g_pClientNet) {
            pData->result = RD2K_ERR_MEMORY;
            lstrcpyA(pData->errorMsg, "Failed to create connection!");
            PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)pData->result, (LPARAM)pData);
            return 0;
        }
        
        /* Set relay mode */
        g_pClientNet->bRelayMode = TRUE;
        g_pClientNet->relaySocket = g_relaySocket;
    }
    else {
        /* ========== DIRECT MODE CONNECTION ========== */
        g_pClientNet = Network_Create(RD2K_LISTEN_PORT);
        if (!g_pClientNet) {
            pData->result = RD2K_ERR_MEMORY;
            lstrcpyA(pData->errorMsg, "Failed to create connection!");
            PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)pData->result, (LPARAM)pData);
            return 0;
        }
        
        /* Direct P2P connection */
        result = Network_Connect(g_pClientNet, pData->host, RD2K_LISTEN_PORT);
        if (result != RD2K_SUCCESS) {
            Network_Destroy(g_pClientNet);
            g_pClientNet = NULL;
            pData->result = RD2K_ERR_CONNECT;
            lstrcpyA(pData->errorMsg, "Failed to connect to partner!");
            PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)pData->result, (LPARAM)pData);
            return 0;
        }
    }
    
    /* Send handshake */
    handshake.magic = RD2K_MAGIC;
    handshake.yourId = g_myId;
    handshake.password = pData->partnerPwd;
    handshake.screenWidth = (WORD)screenW;
    handshake.screenHeight = (WORD)screenH;
    handshake.colorDepth = 24;
    handshake.compression = COMPRESS_RLE;
    handshake.versionMajor = RD2K_VERSION_MAJOR;
    handshake.versionMinor = RD2K_VERSION_MINOR;
    
    Network_SendPacket(g_pClientNet, MSG_HANDSHAKE,
                      (const BYTE*)&handshake, sizeof(handshake));
    
    /* Receive response - blocking */
    result = Network_RecvPacket(g_pClientNet, &header,
                               g_pClientNet->recvBuffer,
                               g_pClientNet->recvBufferSize);
    
    if (result != RD2K_SUCCESS || header.msgType != MSG_HANDSHAKE_ACK) {
        Network_Destroy(g_pClientNet);
        g_pClientNet = NULL;
        pData->result = RD2K_ERR_AUTH;
        lstrcpyA(pData->errorMsg, "Authentication failed!\nWrong password or partner refused.");
        PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)pData->result, (LPARAM)pData);
        return 0;
    }
    
    /* Parse response and store screen info */
    {
        RD2K_HANDSHAKE *pResponse = (RD2K_HANDSHAKE*)g_pClientNet->recvBuffer;
        pData->remoteWidth = pResponse->screenWidth;
        pData->remoteHeight = pResponse->screenHeight;
        pData->remoteBpp = pResponse->colorDepth;
        pData->remoteCompression = pResponse->compression;
    }
    
    /* SUCCESS! */
    pData->result = RD2K_SUCCESS;
    pData->errorMsg[0] = '\0';
    PostMessage(g_hMainWnd, WM_APP_CONNECT_RESULT, (WPARAM)RD2K_SUCCESS, (LPARAM)pData);
    return 0;
}

/* Handle connection result from background thread */
void HandleConnectResult(int result, BOOL bRelayMode, PCONNECT_THREAD_DATA pData)
{
    g_bConnecting = FALSE;
    
    if (g_hConnectThread) {
        CloseHandle(g_hConnectThread);
        g_hConnectThread = NULL;
    }
    
    if (result != RD2K_SUCCESS) {
        /* Connection failed - show error and reset UI */
        if (bRelayMode) {
            SetWindowTextA(g_hRelayConnectPartnerBtn, "Connect to Partner");
            EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
            EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        } else {
            SetWindowTextA(g_hConnectBtn, "Connect to partner");
            EnableWindow(g_hConnectBtn, TRUE);
        }
        UpdateStatusBar("Connection failed", FALSE);
        MessageBoxA(g_hMainWnd, pData->errorMsg, APP_TITLE, MB_ICONERROR);
        free(pData);
        return;
    }
    
    /* Connection succeeded - set up viewer */
    g_remoteScreen.width = pData->remoteWidth;
    g_remoteScreen.height = pData->remoteHeight;
    g_remoteScreen.bitsPerPixel = pData->remoteBpp;
    g_remoteScreen.compression = pData->remoteCompression;
    
    /* Create viewer window */
    if (!CreateViewerWindow()) {
        Network_Destroy(g_pClientNet);
        g_pClientNet = NULL;
        
        if (bRelayMode) {
            SetWindowTextA(g_hRelayConnectPartnerBtn, "Connect to Partner");
            EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
            EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        } else {
            SetWindowTextA(g_hConnectBtn, "Connect to partner");
            EnableWindow(g_hConnectBtn, TRUE);
        }
        UpdateStatusBar("Failed to create viewer", FALSE);
        free(pData);
        return;
    }
    
    g_bClientConnected2 = TRUE;
    g_pClientNet->state = STATE_CONNECTED;
    
    /* Start timers for network processing */
    SetTimer(g_hMainWnd, TIMER_NETWORK, NETWORK_INTERVAL, NULL);
    SetTimer(g_hMainWnd, TIMER_PING, PING_INTERVAL, NULL);
    
    /* Request full screen */
    Network_SendPacket(g_pClientNet, MSG_FULL_SCREEN_REQ, NULL, 0);
    
    if (bRelayMode) {
        SetWindowTextA(g_hRelayConnectPartnerBtn, "Disconnect");
        EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
        EnableWindow(g_hRelayConnectSvrBtn, FALSE);
        UpdateStatusBar("Connected via relay", TRUE);
    } else {
        SetWindowTextA(g_hConnectBtn, "Disconnect");
        EnableWindow(g_hConnectBtn, TRUE);
        UpdateStatusBar("Connected to partner", TRUE);
    }
    
    free(pData);
}

/* Step 2: Connect to partner through relay */
void ConnectToPartnerViaRelay(void)
{
    char idStr[64], pwdStr[32];
    DWORD partnerId, partnerPwd;
    PCONNECT_THREAD_DATA pData;
    
    if (!g_bConnectedToRelay || g_relaySocket == INVALID_SOCKET) {
        MessageBoxA(g_hMainWnd, "Please connect to relay server first!", APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    if (g_bConnecting) {
        MessageBoxA(g_hMainWnd, "Connection already in progress!", APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    /* Get Partner ID and Password from Relay tab */
    GetWindowTextA(g_hRelayPartnerId, idStr, sizeof(idStr));
    GetWindowTextA(g_hRelayPartnerPwd, pwdStr, sizeof(pwdStr));
    
    partnerId = ParseId(idStr);
    partnerPwd = atoi(pwdStr);
    
    /* Validate Partner ID */
    if (partnerId == 0) {
        MessageBoxA(g_hMainWnd, "Please enter a valid Partner ID!\n\n"
                   "The ID should be in format: XXX XXX XXX XXX",
                   APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    if (partnerId == g_myId) {
        MessageBoxA(g_hMainWnd, "You cannot connect to yourself!\n\n"
                   "Please enter a different Partner ID.",
                   APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    if (partnerPwd == 0) {
        MessageBoxA(g_hMainWnd, "Please enter the password!", APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    /* Create thread data */
    pData = (PCONNECT_THREAD_DATA)calloc(1, sizeof(CONNECT_THREAD_DATA));
    if (!pData) {
        MessageBoxA(g_hMainWnd, "Out of memory!", APP_TITLE, MB_ICONERROR);
        return;
    }
    
    pData->bRelayMode = TRUE;
    pData->partnerId = partnerId;
    pData->partnerPwd = partnerPwd;
    
    /* Update UI to show connecting */
    UpdateStatusBar("Connecting...", FALSE);
    SetWindowTextA(g_hRelayConnectPartnerBtn, "Connecting...");
    EnableWindow(g_hRelayConnectPartnerBtn, FALSE);
    EnableWindow(g_hRelayConnectSvrBtn, FALSE);
    
    /* Save Partner ID immediately so it's remembered */
    SaveClientConfig();
    
    g_bConnecting = TRUE;
    
    /* Start background thread - UI stays responsive */
    g_hConnectThread = CreateThread(NULL, 0, ConnectThreadProc, pData, 0, NULL);
    if (!g_hConnectThread) {
        g_bConnecting = FALSE;
        free(pData);
        SetWindowTextA(g_hRelayConnectPartnerBtn, "Connect to Partner");
        EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
        EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        MessageBoxA(g_hMainWnd, "Failed to start connection thread!", APP_TITLE, MB_ICONERROR);
    }
}

/* Disconnect from relay server cleanly */
void DisconnectFromRelayServer(BOOL silent)
{
    /* Stop relay health check timer */
    KillTimer(g_hMainWnd, TIMER_RELAY_CHECK);
    
    EnterCriticalSection(&g_csRelay);
    
    if (g_relaySocket != INVALID_SOCKET) {
        closesocket(g_relaySocket);
        g_relaySocket = INVALID_SOCKET;
    }
    g_bConnectedToRelay = FALSE;
    
    LeaveCriticalSection(&g_csRelay);
    
    /* Reset UI */
    SetWindowTextA(g_hRelayConnectSvrBtn, "Connect to Server");
    EnableWindow(g_hRelayConnectSvrBtn, TRUE);
    EnableWindow(g_hRelayConnectPartnerBtn, FALSE);
    
    if (!silent) {
        UpdateStatusBar("Disconnected from relay server", FALSE);
    }
}

/* Reconnection dialog procedure */
INT_PTR CALLBACK ReconnectDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_INITDIALOG:
        {
            RECT rcOwner, rcDlg;
            int x, y;
            
            /* Center on owner */
            GetWindowRect(g_hMainWnd, &rcOwner);
            GetWindowRect(hwnd, &rcDlg);
            x = rcOwner.left + ((rcOwner.right - rcOwner.left) - (rcDlg.right - rcDlg.left)) / 2;
            y = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - (rcDlg.bottom - rcDlg.top)) / 2;
            SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
            
            return TRUE;
        }
        
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                g_bReconnecting = FALSE;
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
            
        case WM_CLOSE:
            g_bReconnecting = FALSE;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

/* Attempt to reconnect to relay server */
BOOL AttemptRelayReconnection(void)
{
    int result;
    SOCKET relaySocket = INVALID_SOCKET;
    
    /* Regenerate ID in case network changed */
    {
        DWORD newId = GenerateUniqueId();
        if (newId != g_myId && newId != 0x7F000001) {
            char idStr[32];
            g_myId = newId;
            FormatId(g_myId, idStr);
            SetWindowTextA(g_hYourId, idStr);
        }
    }
    
    /* Try to connect to relay server */
    result = Relay_ConnectToServer(g_szRelayServerIp, g_wRelayServerPort, g_myId, &relaySocket);
    if (result != RD2K_SUCCESS) {
        return FALSE;
    }
    
    /* Register with relay */
    result = Relay_Register(relaySocket, g_myId);
    if (result != RD2K_SUCCESS) {
        closesocket(relaySocket);
        
        /* If duplicate ID error, show message and stop reconnecting */
        if (result == RD2K_ERR_DUPLICATE_ID) {
            MessageBoxA(g_hMainWnd, 
                       "Your ID is already connected to this relay server!\n\n"
                       "Another instance may be running, or the previous "
                       "connection hasn't timed out yet.\n\n"
                       "Please wait and try again.",
                       APP_TITLE, MB_ICONWARNING);
        }
        return FALSE;
    }
    
    EnterCriticalSection(&g_csRelay);
    g_relaySocket = relaySocket;
    g_bConnectedToRelay = TRUE;
    LeaveCriticalSection(&g_csRelay);
    
    return TRUE;
}

/* Handle relay server connection lost */
void HandleRelayServerLost(void)
{
    int attempt;
    char msg[256];
    BOOL reconnected = FALSE;
    
    /* IMPORTANT: Prevent re-entry while already reconnecting */
    if (g_bReconnecting) {
        return;
    }
    
    /* Set flag FIRST to prevent re-entry */
    g_bReconnecting = TRUE;
    
    /* Cancel any pending file transfer */
    FileTransfer_Cancel();
    
    /* Stop all timers IMMEDIATELY */
    KillTimer(g_hMainWnd, TIMER_NETWORK);
    KillTimer(g_hMainWnd, TIMER_PING);
    KillTimer(g_hMainWnd, TIMER_RELAY_CHECK);
    
    /* Mark as disconnected */
    g_bClientConnected2 = FALSE;
    g_bClientConnected = FALSE;
    
    /* Close network structures */
    if (g_pClientNet) {
        Network_Destroy(g_pClientNet);
        g_pClientNet = NULL;
    }
    
    /* Close viewer window */
    DestroyViewerWindow();
    
    /* Close existing relay socket */
    EnterCriticalSection(&g_csRelay);
    if (g_relaySocket != INVALID_SOCKET) {
        closesocket(g_relaySocket);
        g_relaySocket = INVALID_SOCKET;
    }
    g_bConnectedToRelay = FALSE;
    LeaveCriticalSection(&g_csRelay);
    
    /* Start reconnection attempts */
    UpdateStatusBar("Relay server lost - reconnecting...", FALSE);
    
    for (attempt = 1; attempt <= RECONNECT_MAX_ATTEMPTS; attempt++) {
        sprintf(msg, "Reconnecting... Attempt %d of %d", attempt, RECONNECT_MAX_ATTEMPTS);
        UpdateStatusBar(msg, FALSE);
        
        /* Try to reconnect */
        if (AttemptRelayReconnection()) {
            reconnected = TRUE;
            break;
        }
        
        /* Wait before next attempt - but check if app is closing */
        if (attempt < RECONNECT_MAX_ATTEMPTS) {
            Sleep(RECONNECT_DELAY_MS);
        }
    }
    
    g_bReconnecting = FALSE;
    
    if (reconnected) {
        /* Successfully reconnected to relay server */
        UpdateStatusBar("Reconnected to relay server", TRUE);
        SetWindowTextA(g_hRelayConnectSvrBtn, "Disconnect Server");
        EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
        
        /* Restart relay health check timer */
        SetTimer(g_hMainWnd, TIMER_RELAY_CHECK, RELAY_CHECK_INTERVAL, NULL);
        
        /* Show ONE message - only if partner was connected */
        MessageBoxA(g_hMainWnd, 
                   "Reconnected to relay server.\n\n"
                   "Note: Your partner connection was lost.\n"
                   "You need to reconnect to your partner.",
                   APP_TITLE, MB_ICONINFORMATION);
    } else {
        /* Failed to reconnect after all attempts */
        DisconnectFromRelayServer(TRUE);
        UpdateStatusBar("Failed to reconnect to relay", FALSE);
        
        sprintf(msg, "Failed to reconnect to relay server after %d attempts.\n\n"
                    "Please check:\n"
                    "- Relay server is running\n"
                    "- Network connection\n"
                    "- Server IP and port",
                RECONNECT_MAX_ATTEMPTS);
        MessageBoxA(g_hMainWnd, msg, APP_TITLE, MB_ICONERROR);
    }
}

/* Handle partner disconnect from relay */
void HandlePartnerDisconnect(BOOL isServerSide)
{
    /* Cancel any pending file transfer */
    FileTransfer_Cancel();
    
    /* Stop network timers */
    KillTimer(g_hMainWnd, TIMER_NETWORK);
    KillTimer(g_hMainWnd, TIMER_PING);
    if (isServerSide) {
        KillTimer(g_hMainWnd, TIMER_SCREEN);
    }
    
    /* Mark connection state */
    if (isServerSide) {
        g_bClientConnected = FALSE;
        if (g_pServerNet) {
            /* IMPORTANT: Detach relay socket before disconnect to preserve relay connection!
             * The relay socket is shared (g_relaySocket) and should NOT be closed here.
             * Only the partner session is ending, not the relay server connection. */
            if (g_pServerNet->bRelayMode && g_pServerNet->relaySocket == g_relaySocket) {
                g_pServerNet->relaySocket = INVALID_SOCKET;
                g_pServerNet->socket = INVALID_SOCKET;  /* Also shared */
                g_pServerNet->bRelayMode = FALSE;
            }
            Network_Disconnect(g_pServerNet);
            Network_Listen(g_pServerNet);
        }
    } else {
        g_bClientConnected2 = FALSE;
        if (g_pClientNet) {
            /* IMPORTANT: Detach relay socket before destroy to preserve relay connection!
             * The relay socket is shared (g_relaySocket) and should NOT be closed here.
             * Only the partner session is ending, not the relay server connection. */
            if (g_pClientNet->bRelayMode && g_pClientNet->relaySocket == g_relaySocket) {
                g_pClientNet->relaySocket = INVALID_SOCKET;
                g_pClientNet->bRelayMode = FALSE;
            }
            Network_Destroy(g_pClientNet);
            g_pClientNet = NULL;
        }
        DestroyViewerWindow();
    }
    
    /* Update UI - still connected to relay, but partner is gone */
    SetWindowTextA(g_hRelayConnectPartnerBtn, "Connect to Partner");
    EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
    EnableWindow(g_hRelayConnectSvrBtn, TRUE);
    
    UpdateStatusBar("Partner disconnected from relay", TRUE);
    MessageBoxA(g_hMainWnd, 
               "Your partner has disconnected from the relay.\n\n"
               "You are still connected to the relay server.\n"
               "You can connect to a new partner.",
               APP_TITLE, MB_ICONINFORMATION);
}

/* Update status bar */
void UpdateStatusBar(const char *text, BOOL connected)
{
    char statusText[256];
    sprintf(statusText, "%s %s", connected ? "\x95" : "\x95", text);  /* Bullet point */
    SendMessageA(g_hStatusBar, SB_SETTEXTA, 0, (LPARAM)statusText);
    
    /* Force refresh to fix display artifacts on Windows 2000 and 10/11 */
    RefreshMainWindow();
}

/*
 * Refresh main window and all children
 * Fixes display artifacts (blue background covering status bar, etc.)
 * on Windows 2000 and modern Windows versions
 */
void RefreshMainWindow(void)
{
    if (!g_hMainWnd) return;
    
    /* Invalidate entire main window and all children */
    RedrawWindow(g_hMainWnd, NULL, NULL, 
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
}

/* Main window procedure */
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE:
            CreateMainControls(hwnd);
            LoadClientConfig();  /* Load saved Server ID and Partner ID */
            UpdateStatusBar("Ready to connect", TRUE);
            return 0;
        
        /* File transfer progress messages (from worker thread) */
        case WM_FT_PROGRESS:
        {
            /* wParam = bytes transferred, lParam = total bytes */
            Progress_Update((DWORD)wParam);
            return 0;
        }
        
        case WM_FT_COMPLETE:
        {
            /* File transfer completed successfully */
            Progress_Hide();
            
            /* lParam = 1 means file was RECEIVED (not sent) */
            if (lParam == 1) {
                /* File received - just update status bar, don't steal focus */
                UpdateStatusBar("File received successfully", FALSE);
            } else {
                /* File sent - update status bar */
                UpdateStatusBar("File sent successfully", FALSE);
            }
            
            /* Reset transfer state - worker thread is done */
            FileTransfer_Cancel();  /* This resets state and closes thread handle */
            return 0;
        }
        
        case WM_FT_ERROR:
        {
            /* File transfer error - wParam = error code */
            Progress_Hide();
            /* Reset transfer state */
            FileTransfer_Cancel();
            if ((int)wParam != FT_ERR_CANCELLED) {
                MessageBoxA(hwnd, FileTransfer_GetLastError(), APP_TITLE, MB_ICONERROR);
            }
            return 0;
        }
        
        case WM_APP_CONNECT_RESULT:
        {
            /* Connection completed (success or failure) from background thread */
            PCONNECT_THREAD_DATA pData = (PCONNECT_THREAD_DATA)lParam;
            if (pData) {
                HandleConnectResult((int)wParam, pData->bRelayMode, pData);
            }
            return 0;
        }
        
        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            HWND hCtrl = (HWND)lParam;
            int id = GetDlgCtrlID(hCtrl);
            
            /* Header panels - blue bg, white text */
            if (id == IDC_LEFT_PANEL || id == IDC_RIGHT_PANEL) {
                SetTextColor(hdc, COLOR_HEADER_TEXT);
                SetBkColor(hdc, COLOR_HEADER_BG);
                return (LRESULT)g_hBrushHeader;
            }
            
            /* Left panel labels - blue bg, white text */
            if (id == IDC_LEFT_HEADER || id == IDC_LEFT_INSTR ||
                id == IDC_YOUR_ID_LABEL || id == IDC_YOUR_PWD_LABEL ||
                id == IDC_CUSTOM_PWD_LABEL) {
                SetTextColor(hdc, COLOR_HEADER_TEXT);
                SetBkColor(hdc, COLOR_HEADER_BG);
                return (LRESULT)g_hBrushHeader;
            }
            
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_hBrushPanel;
        }
        
        case WM_CTLCOLOREDIT:
        {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, COLOR_ID_BG);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        
        case WM_NOTIFY:
        {
            LPNMHDR pnmh = (LPNMHDR)lParam;
            if (pnmh->idFrom == IDC_TAB_CTRL && pnmh->code == TCN_SELCHANGE) {
                int tabIndex = TabCtrl_GetCurSel(g_hTabCtrl);
                SwitchTab(tabIndex);
            }
            return 0;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_COPY_ID_BTN:
                    CopyIdToClipboard();
                    break;
                
                case IDC_REFRESH_PWD_BTN:
                    RefreshPassword();
                    break;
                
                case IDC_SET_PWD_BTN:
                {
                    char pwd[32];
                    GetWindowTextA(g_hCustomPwd, pwd, sizeof(pwd));
                    if (strlen(pwd) > 0) {
                        strcpy(g_customPassword, pwd);
                        g_useCustomPassword = TRUE;
                        MessageBoxA(hwnd, "Custom password has been set!", APP_TITLE, MB_ICONINFORMATION);
                    }
                    break;
                }
                
                case IDC_CONNECT_BTN:
                    if (g_bClientConnected2) {
                        DisconnectFromPartner();
                    } else {
                        ConnectToPartner();
                    }
                    break;
                
                case IDC_RELAY_CONNECT_SVR_BTN:
                    /* Toggle connection to relay server */
                    ConnectToRelayServer();
                    break;
                
                case IDC_RELAY_CONNECT_PARTNER_BTN:
                    if (g_bClientConnected2) {
                        DisconnectFromPartner();
                        SetWindowTextA(g_hRelayConnectPartnerBtn, "Connect to Partner");
                        EnableWindow(g_hRelayConnectSvrBtn, TRUE);  /* Re-enable server disconnect */
                    } else {
                        ConnectToPartnerViaRelay();
                    }
                    break;
            }
            return 0;
        
        case WM_TIMER:
            switch (wParam) {
                case TIMER_LISTEN_CHECK:
                    if (g_bServerRunning && !g_bClientConnected) {
                        ProcessServerNetwork();
                    }
                    break;
                
                case TIMER_NETWORK:
                    if (g_bClientConnected) {
                        ProcessServerNetwork();
                    }
                    if (g_bClientConnected2) {
                        ProcessClientNetwork();
                    }
                    break;
                
                case TIMER_SCREEN:
                    if (g_bClientConnected) {
                        SendScreenUpdate();
                    }
                    break;
                
                case TIMER_PING:
                    if (g_pClientNet && g_bClientConnected2) {
                        Network_SendPacket(g_pClientNet, MSG_PING, NULL, 0);
                    }
                    /* Check clipboard changes for server side sync */
                    if (g_bClientConnected && g_pServerNet && !g_bIgnoreClipboard) {
                        DWORD seq = GetClipboardSequenceNumber();
                        if (seq != g_lastClipboardSeq) {
                            g_lastClipboardSeq = seq;
                            SendClipboardData(g_pServerNet);
                        }
                    }
                    break;
                
                case TIMER_RELAY_CHECK:
                    /* Check if relay server is still alive (only when not in active session) */
                    if (g_bConnectedToRelay && g_relaySocket != INVALID_SOCKET) {
                        /* Don't check during active partner session - data flow will detect issues */
                        if (!g_bClientConnected2 && !g_bClientConnected) {
                            int result = Relay_CheckConnection(g_relaySocket);
                            if (result != RD2K_SUCCESS) {
                                /* Relay server disconnected */
                                KillTimer(g_hMainWnd, TIMER_RELAY_CHECK);
                                HandleRelayServerLost();
                            }
                        }
                    }
                    break;
            }
            return 0;
        
        case WM_NCLBUTTONDOWN:
        {
            /*
             * Intercept title bar button clicks to prevent the modal tracking loop
             * from blocking our timer-based network/screen processing.
             * Instead of letting DefWindowProc handle these synchronously,
             * we post the corresponding WM_SYSCOMMAND to handle them asynchronously.
             */
            switch (wParam) {
                case HTMINBUTTON:
                    /* Post minimize command - don't let DefWindowProc do modal tracking */
                    PostMessageA(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                    return 0;
                case HTMAXBUTTON:
                    /* Check if maximized to decide restore or maximize */
                    if (IsZoomed(hwnd)) {
                        PostMessageA(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
                    } else {
                        PostMessageA(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                    }
                    return 0;
                case HTCLOSE:
                    PostMessageA(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
                    return 0;
            }
            /* For other non-client areas (title bar drag, etc.), let DefWindowProc handle */
            break;
        }
        
        case WM_CLOSE:
            SaveClientConfig();  /* Save Server ID and Partner ID for next session */
            StopServer();
            DisconnectFromPartner();
            DestroyWindow(hwnd);
            return 0;
        
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Copy ID to clipboard */
void CopyIdToClipboard(void)
{
    char idStr[32];
    HGLOBAL hMem;
    char *pMem;
    
    FormatId(g_myId, idStr);
    
    /* Release modifier keys before clipboard operations */
    Input_ReleaseAllModifiers();
    
    if (OpenClipboard(g_hMainWnd)) {
        EmptyClipboard();
        hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(idStr) + 1);
        if (hMem) {
            pMem = (char*)GlobalLock(hMem);
            strcpy(pMem, idStr);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
        UpdateStatusBar("ID copied to clipboard", TRUE);
    }
}

/* Refresh password */
void RefreshPassword(void)
{
    char pwdStr[16];
    g_myPassword = GeneratePassword();
    sprintf(pwdStr, "%d", g_myPassword);
    SetWindowTextA(g_hYourPwd, pwdStr);
    g_useCustomPassword = FALSE;
}

/* Start server (allow remote control) */
void StartServer(void)
{
    if (g_bServerRunning) return;
    
    g_pServerNet = Network_Create(RD2K_LISTEN_PORT);
    if (!g_pServerNet) {
        UpdateStatusBar("Failed to create server", FALSE);
        return;
    }
    
    if (Network_Listen(g_pServerNet) != RD2K_SUCCESS) {
        Network_Destroy(g_pServerNet);
        g_pServerNet = NULL;
        UpdateStatusBar("Failed to start server", FALSE);
        return;
    }
    
    g_pCapture = ScreenCapture_Create();
    if (!g_pCapture) {
        Network_Destroy(g_pServerNet);
        g_pServerNet = NULL;
        UpdateStatusBar("Failed to init screen capture", FALSE);
        return;
    }
    
    /* Initialize async input processing */
    Input_Initialize();
    
    g_bServerRunning = TRUE;
    SetTimer(g_hMainWnd, TIMER_LISTEN_CHECK, LISTEN_CHECK_INTERVAL, NULL);
    UpdateStatusBar("Ready to connect", TRUE);
}

/* Stop server */
void StopServer(void)
{
    if (!g_bServerRunning) return;
    
    /* Shutdown async input processing */
    Input_Shutdown();
    
    KillTimer(g_hMainWnd, TIMER_LISTEN_CHECK);
    KillTimer(g_hMainWnd, TIMER_SCREEN);
    KillTimer(g_hMainWnd, TIMER_NETWORK);
    
    if (g_pServerNet) {
        Network_Destroy(g_pServerNet);
        g_pServerNet = NULL;
    }
    
    if (g_pCapture) {
        ScreenCapture_Destroy(g_pCapture);
        g_pCapture = NULL;
    }
    
    g_bServerRunning = FALSE;
    g_bClientConnected = FALSE;
}

/* Process server network events */
void ProcessServerNetwork(void)
{
    fd_set readSet;
    struct timeval timeout;
    RD2K_HEADER header;
    int result;
    BYTE headerBuf[sizeof(RD2K_HEADER)];
    int recvLen;
    
    if (!g_pServerNet) return;
    
    /* Check for incoming relay connection (when connected to relay server)
     * IMPORTANT: Don't process relay data while we're connecting as viewer
     * or while we're actively connected as viewer - the relay socket is shared!
     * g_bConnecting: connection in progress (background thread using socket)
     * g_bClientConnected2: actively viewing remote (viewer using socket for screen data) */
    if (!g_bClientConnected && !g_bConnecting && !g_bClientConnected2 && 
        g_bConnectedToRelay && g_relaySocket != INVALID_SOCKET) {
        FD_ZERO(&readSet);
        FD_SET(g_relaySocket, &readSet);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        
        if (select(0, &readSet, NULL, NULL, &timeout) > 0) {
            /* Data available on relay socket - try to receive handshake
             * Network_SendPacket sends header and data as TWO separate relay packets:
             * 1. First packet: RD2K_HEADER (8 bytes)
             * 2. Second packet: actual data (e.g., RD2K_HANDSHAKE, 24 bytes)
             */
            recvLen = Relay_RecvData(g_relaySocket, headerBuf, sizeof(RD2K_HEADER), 500);
            
            if (recvLen == sizeof(RD2K_HEADER)) {
                CopyMemory(&header, headerBuf, sizeof(RD2K_HEADER));
                
                if (header.msgType == MSG_HANDSHAKE && header.dataLength == sizeof(RD2K_HANDSHAKE)) {
                    /* Now receive the handshake data (comes in second relay packet) */
                    recvLen = Relay_RecvData(g_relaySocket, g_pServerNet->recvBuffer, 
                                            header.dataLength, 2000);
                    
                    if (recvLen == (int)header.dataLength) {
                        RD2K_HANDSHAKE *pHandshake = (RD2K_HANDSHAKE*)g_pServerNet->recvBuffer;
                        BOOL authOk = FALSE;
                        
                        /* Check password */
                        if (pHandshake->magic == RD2K_MAGIC) {
                            if (g_useCustomPassword) {
                                authOk = (atoi(g_customPassword) == (int)pHandshake->password);
                            } else {
                                authOk = (pHandshake->password == g_myPassword);
                            }
                        }
                        
                        if (authOk) {
                            /* Set up network structure for relay mode */
                            g_pServerNet->bRelayMode = TRUE;
                            g_pServerNet->relaySocket = g_relaySocket;
                            g_pServerNet->socket = g_relaySocket;
                            
                            /* Send handshake response through relay */
                            {
                                RD2K_HANDSHAKE response;
                                int screenW, screenH;
                                
                                ScreenCapture_GetDimensions(&screenW, &screenH);
                                
                                response.magic = RD2K_MAGIC;
                                response.yourId = g_myId;
                                response.password = 0;
                                response.screenWidth = (WORD)screenW;
                                response.screenHeight = (WORD)screenH;
                                response.colorDepth = 24;
                                response.compression = COMPRESS_RLE;
                                response.versionMajor = RD2K_VERSION_MAJOR;
                                response.versionMinor = RD2K_VERSION_MINOR;
                                
                                Network_SendPacket(g_pServerNet, MSG_HANDSHAKE_ACK,
                                                  (const BYTE*)&response, sizeof(response));
                            }
                            
                            g_bClientConnected = TRUE;
                            g_pServerNet->state = STATE_CONNECTED;
                            
                            SetTimer(g_hMainWnd, TIMER_NETWORK, NETWORK_INTERVAL, NULL);
                            SetTimer(g_hMainWnd, TIMER_SCREEN, SCREEN_INTERVAL, NULL);
                            
                            UpdateStatusBar("Partner connected via relay", TRUE);
                            return;
                        }
                    }
                }
            }
            /* If we didn't get a valid handshake, just return - will try again next timer tick */
        }
    }
    
    /* Check for new direct connections */
    if (!g_bClientConnected && g_pServerNet->listenSocket != INVALID_SOCKET) {
        FD_ZERO(&readSet);
        FD_SET(g_pServerNet->listenSocket, &readSet);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        
        if (select(0, &readSet, NULL, NULL, &timeout) > 0) {
            if (Network_Accept(g_pServerNet) != INVALID_SOCKET) {
                /* Receive handshake */
                result = Network_RecvPacket(g_pServerNet, &header, 
                                           g_pServerNet->recvBuffer, 
                                           g_pServerNet->recvBufferSize);
                
                if (result == RD2K_SUCCESS && header.msgType == MSG_HANDSHAKE) {
                    RD2K_HANDSHAKE *pHandshake = (RD2K_HANDSHAKE*)g_pServerNet->recvBuffer;
                    BOOL authOk = FALSE;
                    
                    /* Check password */
                    if (pHandshake->magic == RD2K_MAGIC) {
                        if (g_useCustomPassword) {
                            authOk = (atoi(g_customPassword) == (int)pHandshake->password);
                        } else {
                            authOk = (pHandshake->password == g_myPassword);
                        }
                    }
                    
                    if (authOk) {
                        /* Send handshake response */
                        RD2K_HANDSHAKE response;
                        int screenW, screenH;
                        
                        ScreenCapture_GetDimensions(&screenW, &screenH);
                        
                        response.magic = RD2K_MAGIC;
                        response.yourId = g_myId;
                        response.password = 0;
                        response.screenWidth = (WORD)screenW;
                        response.screenHeight = (WORD)screenH;
                        response.colorDepth = 24;
                        response.compression = COMPRESS_RLE;
                        response.versionMajor = RD2K_VERSION_MAJOR;
                        response.versionMinor = RD2K_VERSION_MINOR;
                        
                        Network_SendPacket(g_pServerNet, MSG_HANDSHAKE_ACK,
                                          (const BYTE*)&response, sizeof(response));
                        
                        g_bClientConnected = TRUE;
                        g_pServerNet->state = STATE_CONNECTED;
                        
                        SetTimer(g_hMainWnd, TIMER_NETWORK, NETWORK_INTERVAL, NULL);
                        SetTimer(g_hMainWnd, TIMER_SCREEN, SCREEN_INTERVAL, NULL);
                        
                        UpdateStatusBar("Partner connected", TRUE);
                    } else {
                        /* Auth failed */
                        Network_Disconnect(g_pServerNet);
                        g_pServerNet->listenSocket = INVALID_SOCKET;
                        Network_Listen(g_pServerNet);
                    }
                }
            }
        }
        return;
    }
    
    /* Handle connected client */
    if (g_bClientConnected && g_pServerNet->socket != INVALID_SOCKET) {
        while (Network_DataAvailable(g_pServerNet)) {
            result = Network_RecvPacket(g_pServerNet, &header,
                                       g_pServerNet->recvBuffer,
                                       g_pServerNet->recvBufferSize);
            
            if (result != RD2K_SUCCESS) {
                /* Check if this is a relay mode connection */
                if (g_pServerNet && g_pServerNet->bRelayMode) {
                    if (result == RD2K_ERR_SERVER_LOST) {
                        /* Relay server disconnected - attempt reconnection */
                        HandleRelayServerLost();
                        return;
                    } else if (result == RD2K_ERR_PARTNER_LEFT) {
                        /* Partner disconnected from relay */
                        HandlePartnerDisconnect(TRUE);
                        return;
                    }
                }
                /* Direct connection or other error - connection lost */
                g_bClientConnected = FALSE;
                /* Detach relay socket before disconnect to preserve relay connection */
                if (g_pServerNet->bRelayMode && g_pServerNet->relaySocket == g_relaySocket) {
                    g_pServerNet->relaySocket = INVALID_SOCKET;
                    g_pServerNet->socket = INVALID_SOCKET;
                    g_pServerNet->bRelayMode = FALSE;
                }
                Network_Disconnect(g_pServerNet);
                Network_Listen(g_pServerNet);
                KillTimer(g_hMainWnd, TIMER_SCREEN);
                UpdateStatusBar("Partner disconnected", TRUE);
                return;
            }
            
            switch (header.msgType) {
                case MSG_MOUSE_EVENT:
                    HandleMouseEvent((RD2K_MOUSE_EVENT*)g_pServerNet->recvBuffer);
                    break;
                
                case MSG_KEYBOARD_EVENT:
                    HandleKeyboardEvent((RD2K_KEY_EVENT*)g_pServerNet->recvBuffer);
                    break;
                
                case MSG_CLIPBOARD_TEXT:
                    /* Use new clipboard module */
                    Clipboard_ReceiveFromRemote(g_pServerNet->recvBuffer, header.dataLength);
                    break;
                
                case MSG_CLIPBOARD_FILES:
                    /* Use new clipboard module for files */
                    Clipboard_ReceiveFromRemote(g_pServerNet->recvBuffer, header.dataLength);
                    break;
                
                case MSG_FILE_START:
                    ReceiveFileStart(g_pServerNet->recvBuffer, header.dataLength);
                    break;
                
                case MSG_FILE_DATA:
                    ReceiveFileData(g_pServerNet->recvBuffer, header.dataLength);
                    break;
                
                case MSG_FILE_END:
                    ReceiveFileEnd();
                    break;
                
                case MSG_FULL_SCREEN_REQ:
                    /* Clear previous frame to force full update */
                    if (g_pCapture && g_pCapture->pPrevFrame) {
                        ZeroMemory(g_pCapture->pPrevFrame, g_pCapture->pixelDataSize);
                        /* Immediately send full screen update */
                        SendScreenUpdate();
                    }
                    break;
                
                case MSG_CLIPBOARD_REQ:
                    /* Client is requesting server's clipboard (e.g., after Ctrl+C on remote) */
                    Clipboard_SendToRemote(g_pServerNet);
                    break;
                
                case MSG_FILE_REQ:
                    /* Client is requesting actual file transfer from server's clipboard.
                       This is used when user does Ctrl+C on remote and wants files
                       transferred back to the host desktop.
                       
                       IMPORTANT: Use SYNCHRONOUS transfer here!
                       Using async (worker thread) causes socket access from multiple threads
                       which leads to disconnection. The sync version blocks but processes
                       Windows messages to keep UI responsive. */
                    if (!FileTransfer_IsBusy()) {
                        /* Send files from server's clipboard to client - SYNCHRONOUSLY */
                        int ftResult = FileTransfer_SendClipboardFilesSync(g_pServerNet, g_hMainWnd, g_hMainWnd);
                        if (ftResult == FT_ERR_FILE_NOT_FOUND) {
                            /* No files in clipboard - notify client */
                            Network_SendPacket(g_pServerNet, MSG_FILE_NONE, NULL, 0);
                        }
                    }
                    break;
                
                case MSG_PING:
                    Network_SendPacket(g_pServerNet, MSG_PONG, NULL, 0);
                    break;
                
                case MSG_DISCONNECT:
                    g_bClientConnected = FALSE;
                    /* Detach relay socket before disconnect to preserve relay connection */
                    if (g_pServerNet->bRelayMode && g_pServerNet->relaySocket == g_relaySocket) {
                        g_pServerNet->relaySocket = INVALID_SOCKET;
                        g_pServerNet->socket = INVALID_SOCKET;
                        g_pServerNet->bRelayMode = FALSE;
                    }
                    Network_Disconnect(g_pServerNet);
                    Network_Listen(g_pServerNet);
                    KillTimer(g_hMainWnd, TIMER_SCREEN);
                    UpdateStatusBar("Partner disconnected", TRUE);
                    return;
            }
        }
    }
}

/* Send screen update */
void SendScreenUpdate(void)
{
    RECT dirtyRects[2048];  /* Increased for full screen support */
    int numRects, i;
    int bytesPerPixel = 3;
    int stride;
    
    if (!g_pCapture || !g_pServerNet || !g_bClientConnected) return;
    
    if (ScreenCapture_CaptureScreen(g_pCapture) != RD2K_SUCCESS) return;
    
    stride = ((g_pCapture->width * bytesPerPixel + 3) & ~3);
    
    numRects = FindDirtyRects(g_pCapture->pPrevFrame, g_pCapture->pPixelData,
                              g_pCapture->width, g_pCapture->height, bytesPerPixel,
                              dirtyRects, 2048);
    
    for (i = 0; i < numRects; i++) {
        RD2K_RECT rectHeader;
        BYTE *pTempBuffer;
        DWORD rectDataSize, compressedSize;
        int x, y, w, h, j;
        
        x = dirtyRects[i].left;
        y = dirtyRects[i].top;
        w = dirtyRects[i].right - dirtyRects[i].left;
        h = dirtyRects[i].bottom - dirtyRects[i].top;
        
        rectDataSize = w * h * bytesPerPixel;
        pTempBuffer = (BYTE*)malloc(rectDataSize);
        if (!pTempBuffer) continue;
        
        for (j = 0; j < h; j++) {
            memcpy(pTempBuffer + j * w * bytesPerPixel,
                   g_pCapture->pPixelData + (y + j) * stride + x * bytesPerPixel,
                   w * bytesPerPixel);
        }
        
        compressedSize = CompressRLE(pTempBuffer, rectDataSize,
                                     g_pCapture->pCompressBuffer,
                                     g_pCapture->compressBufferSize);
        
        rectHeader.x = (WORD)x;
        rectHeader.y = (WORD)y;
        rectHeader.width = (WORD)w;
        rectHeader.height = (WORD)h;
        rectHeader.encoding = COMPRESS_RLE;
        rectHeader.reserved = 0;
        rectHeader.dataSize = compressedSize;
        
        memcpy(g_pServerNet->sendBuffer, &rectHeader, sizeof(rectHeader));
        memcpy(g_pServerNet->sendBuffer + sizeof(rectHeader),
               g_pCapture->pCompressBuffer, compressedSize);
        
        Network_SendPacket(g_pServerNet, MSG_SCREEN_UPDATE,
                          g_pServerNet->sendBuffer,
                          sizeof(rectHeader) + compressedSize);
        
        free(pTempBuffer);
    }
    
    memcpy(g_pCapture->pPrevFrame, g_pCapture->pPixelData, g_pCapture->pixelDataSize);
}

/* Handle mouse event from client - uses modular input system */
void HandleMouseEvent(const RD2K_MOUSE_EVENT *pEvent)
{
    if (!pEvent) return;
    
    /* Use the new modular input system */
    Input_ProcessMouseEvent(pEvent->x, pEvent->y, pEvent->buttons, 
                           pEvent->flags, pEvent->wheelDelta);
}

/* Handle keyboard event from client - uses modular input system */
void HandleKeyboardEvent(const RD2K_KEY_EVENT *pEvent)
{
    if (!pEvent) return;
    
    /* Use the new modular input system */
    Input_ProcessKeyEvent(pEvent->virtualKey, pEvent->scanCode, pEvent->flags);
}

/* Connect to partner */
void ConnectToPartner(void)
{
    char idStr[64], pwdStr[32], host[64];
    DWORD partnerId, partnerPwd;
    PCONNECT_THREAD_DATA pData;
    
    if (g_bConnecting) {
        MessageBoxA(g_hMainWnd, "Connection already in progress!", APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    GetWindowTextA(g_hPartnerId, idStr, sizeof(idStr));
    GetWindowTextA(g_hPartnerPwd, pwdStr, sizeof(pwdStr));
    
    partnerId = ParseId(idStr);
    partnerPwd = atoi(pwdStr);
    
    /* Check for empty or invalid ID */
    if (partnerId == 0) {
        MessageBoxA(g_hMainWnd, "Please enter a valid Partner ID!\n\n"
                   "The ID should be in format: XXX XXX XXX XXX",
                   APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    /* Check if user is trying to connect to themselves */
    if (partnerId == g_myId) {
        MessageBoxA(g_hMainWnd, "You cannot connect to yourself!\n\n"
                   "Please enter a different Partner ID.",
                   APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    if (partnerPwd == 0) {
        MessageBoxA(g_hMainWnd, "Please enter the password!", APP_TITLE, MB_ICONWARNING);
        return;
    }
    
    /* Convert Partner ID to IP address (decrypts the ID) */
    {
        BOOL bValidIP = FALSE;
        IdToIPStringEx(partnerId, host, &bValidIP);
        
        if (!bValidIP) {
            MessageBoxA(g_hMainWnd, "Invalid Partner ID!\n\n"
                       "The ID you entered is not valid.\n"
                       "Please check and try again.",
                       APP_TITLE, MB_ICONERROR);
            return;
        }
    }
    
    /* Create thread data */
    pData = (PCONNECT_THREAD_DATA)calloc(1, sizeof(CONNECT_THREAD_DATA));
    if (!pData) {
        MessageBoxA(g_hMainWnd, "Out of memory!", APP_TITLE, MB_ICONERROR);
        return;
    }
    
    pData->bRelayMode = FALSE;
    pData->partnerId = partnerId;
    pData->partnerPwd = partnerPwd;
    lstrcpyA(pData->host, host);
    
    /* Update UI to show connecting */
    UpdateStatusBar("Connecting...", FALSE);
    SetWindowTextA(g_hConnectBtn, "Connecting...");
    EnableWindow(g_hConnectBtn, FALSE);
    
    /* Save Partner ID immediately so it's remembered */
    SaveClientConfig();
    
    g_bConnecting = TRUE;
    
    /* Start background thread - UI stays responsive */
    g_hConnectThread = CreateThread(NULL, 0, ConnectThreadProc, pData, 0, NULL);
    if (!g_hConnectThread) {
        g_bConnecting = FALSE;
        free(pData);
        SetWindowTextA(g_hConnectBtn, "Connect to partner");
        EnableWindow(g_hConnectBtn, TRUE);
        MessageBoxA(g_hMainWnd, "Failed to start connection thread!", APP_TITLE, MB_ICONERROR);
    }
}

/* Disconnect from partner */
void DisconnectFromPartner(void)
{
    BOOL wasRelayMode = FALSE;
    
    if (!g_bClientConnected2) return;
    
    KillTimer(g_hMainWnd, TIMER_NETWORK);
    KillTimer(g_hMainWnd, TIMER_PING);
    
    if (g_pClientNet) {
        wasRelayMode = g_pClientNet->bRelayMode;
        Network_SendPacket(g_pClientNet, MSG_DISCONNECT, NULL, 0);
        
        /* IMPORTANT: Detach relay socket before destroy to preserve relay connection!
         * The relay socket is shared (g_relaySocket) and should NOT be closed here.
         * Only the partner session is ending, not the relay server connection. */
        if (g_pClientNet->bRelayMode && g_pClientNet->relaySocket == g_relaySocket) {
            g_pClientNet->relaySocket = INVALID_SOCKET;
            g_pClientNet->bRelayMode = FALSE;
        }
        
        Network_Destroy(g_pClientNet);
        g_pClientNet = NULL;
    }
    
    DestroyViewerWindow();
    
    g_bClientConnected2 = FALSE;
    
    /* Update UI based on connection type */
    if (wasRelayMode && g_bConnectedToRelay) {
        /* Was relay mode - restore relay UI state */
        SetWindowTextA(g_hRelayConnectPartnerBtn, "Connect to Partner");
        EnableWindow(g_hRelayConnectPartnerBtn, TRUE);
        EnableWindow(g_hRelayConnectSvrBtn, TRUE);
        UpdateStatusBar("Disconnected from partner - relay available", TRUE);
    } else {
        SetWindowTextA(g_hConnectBtn, "Connect to partner");
        UpdateStatusBar("Ready to connect", TRUE);
    }
}

/* Process client network events */
void ProcessClientNetwork(void)
{
    RD2K_HEADER header;
    int result;
    
    if (!g_pClientNet || !g_bClientConnected2) return;
    
    while (Network_DataAvailable(g_pClientNet)) {
        result = Network_RecvPacket(g_pClientNet, &header,
                                   g_pClientNet->recvBuffer,
                                   g_pClientNet->recvBufferSize);
        
        if (result != RD2K_SUCCESS) {
            /* Check if this is a relay mode connection */
            if (g_pClientNet && g_pClientNet->bRelayMode) {
                if (result == RD2K_ERR_SERVER_LOST) {
                    /* Relay server disconnected - attempt reconnection */
                    HandleRelayServerLost();
                    return;
                } else if (result == RD2K_ERR_PARTNER_LEFT) {
                    /* Partner disconnected from relay */
                    HandlePartnerDisconnect(FALSE);
                    return;
                }
            }
            /* Direct connection or other error */
            DisconnectFromPartner();
            UpdateStatusBar("Connection to partner lost", TRUE);
            MessageBoxA(g_hMainWnd, "Connection to partner lost!", APP_TITLE, MB_ICONWARNING);
            return;
        }
        
        switch (header.msgType) {
            case MSG_SCREEN_UPDATE:
                HandleScreenUpdate(g_pClientNet->recvBuffer, header.dataLength);
                break;
            
            case MSG_CLIPBOARD_TEXT:
                /* Use new clipboard module */
                Clipboard_ReceiveFromRemote(g_pClientNet->recvBuffer, header.dataLength);
                break;
            
            case MSG_CLIPBOARD_FILES:
                /* Use new clipboard module for files */
                Clipboard_ReceiveFromRemote(g_pClientNet->recvBuffer, header.dataLength);
                break;
            
            case MSG_FILE_START:
                ReceiveFileStart(g_pClientNet->recvBuffer, header.dataLength);
                break;
            
            case MSG_FILE_DATA:
                ReceiveFileData(g_pClientNet->recvBuffer, header.dataLength);
                break;
            
            case MSG_FILE_END:
                ReceiveFileEnd();
                break;
            
            case MSG_FILE_NONE:
                /* Server responded - no files in its clipboard */
                UpdateStatusBar("No files in remote clipboard", FALSE);
                MessageBoxA(g_hViewerWnd ? g_hViewerWnd : g_hMainWnd, 
                           "No files in remote clipboard.\n\n"
                           "To receive files:\n"
                           "1. Select file(s) on the remote computer\n"
                           "2. Press Ctrl+C to copy\n"
                           "3. Click 'Receive' button", 
                           APP_TITLE, MB_ICONINFORMATION);
                break;
            
            case MSG_PONG:
                break;
            
            case MSG_DISCONNECT:
                DisconnectFromPartner();
                return;
        }
    }
}

/* Helper function to create viewer menu (avoids code duplication) */
HMENU CreateViewerMenu(void)
{
    HMENU hMenu = CreateMenu();
    HMENU hViewMenu = CreatePopupMenu();
    HMENU hToolsMenu = CreatePopupMenu();
    
    AppendMenuA(hViewMenu, MF_STRING, IDM_VIEWER_FULLSCREEN, "Full Screen\tF11");
    AppendMenuA(hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hViewMenu, MF_STRING, IDM_VIEWER_ACTUAL, "Actual Size (100%)");
    AppendMenuA(hViewMenu, MF_STRING | (g_displayMode == DISPLAY_STRETCH ? MF_CHECKED : 0), 
                IDM_VIEWER_STRETCH, "Stretch to Fit");
    AppendMenuA(hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hViewMenu, MF_STRING, IDM_VIEWER_REFRESH, "Refresh Screen\tF5");
    
    AppendMenuA(hToolsMenu, MF_STRING, IDM_VIEWER_SENDFILE, "Send File to Remote...");
    AppendMenuA(hToolsMenu, MF_STRING, IDM_VIEWER_RECEIVEFILE, "Receive File from Remote\tCtrl+Shift+V");
    AppendMenuA(hToolsMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hToolsMenu, MF_STRING, IDM_VIEWER_CLIPBOARD, "Sync Clipboard\tCtrl+Shift+C");
    AppendMenuA(hToolsMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hToolsMenu, MF_STRING, IDM_VIEWER_DISCONNECT, "Disconnect");
    
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, "View");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hToolsMenu, "Tools");
    
    return hMenu;
}

/* Create viewer window */
BOOL CreateViewerWindow(void)
{
    char title[128];
    int width, height;
    
    if (g_hViewerWnd) return TRUE;
    
    /* Create bitmap for remote screen */
    if (!CreateViewerBitmap()) return FALSE;
    
    width = g_remoteScreen.width + GetSystemMetrics(SM_CXSIZEFRAME) * 2;
    height = g_remoteScreen.height + GetSystemMetrics(SM_CYSIZEFRAME) * 2 + 
             GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYMENU);
    
    /* Limit to screen size */
    if (width > GetSystemMetrics(SM_CXSCREEN)) width = GetSystemMetrics(SM_CXSCREEN);
    if (height > GetSystemMetrics(SM_CYSCREEN) - 50) height = GetSystemMetrics(SM_CYSCREEN) - 50;
    
    /* Window title - show "Connected" instead of IP for security */
    sprintf(title, "RemoteDesk2K - Connected (%dx%d)", 
            g_remoteScreen.width, g_remoteScreen.height);
    
    g_hViewerWnd = CreateWindowExA(
        WS_EX_ACCEPTFILES, VIEWER_WND_CLASS, title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        NULL, NULL, g_hInstance, NULL);
    
    if (!g_hViewerWnd) {
        DestroyViewerBitmap();
        return FALSE;
    }
    
    /* Capture the current active folder BEFORE showing viewer window.
       This is the folder where files will be saved when received. */
    FileTransfer_CaptureActiveFolder(g_hViewerWnd);
    
    /* Add menu (using helper function) */
    SetMenu(g_hViewerWnd, CreateViewerMenu());
    
    ShowWindow(g_hViewerWnd, SW_SHOW);
    UpdateWindow(g_hViewerWnd);
    SetForegroundWindow(g_hViewerWnd);
    SetFocus(g_hViewerWnd);
    
    return TRUE;
}

/* Destroy viewer window */
void DestroyViewerWindow(void)
{
    if (g_bFullscreen) {
        DestroyFullscreenToolbar();
        g_bFullscreen = FALSE;
    }
    
    if (g_hViewerWnd) {
        KillTimer(g_hViewerWnd, TIMER_TOOLBAR_HIDE);
        DestroyWindow(g_hViewerWnd);
        g_hViewerWnd = NULL;
    }
    
    DestroyViewerBitmap();
}

/* Create viewer bitmap */
BOOL CreateViewerBitmap(void)
{
    BITMAPINFO bmpInfo;
    HDC hdcScreen;
    
    if (g_remoteScreen.width == 0 || g_remoteScreen.height == 0) return FALSE;
    
    hdcScreen = GetDC(NULL);
    g_hdcViewer = CreateCompatibleDC(hdcScreen);
    if (!g_hdcViewer) {
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }
    
    ZeroMemory(&bmpInfo, sizeof(BITMAPINFO));
    bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmpInfo.bmiHeader.biWidth = g_remoteScreen.width;
    bmpInfo.bmiHeader.biHeight = -g_remoteScreen.height;
    bmpInfo.bmiHeader.biPlanes = 1;
    bmpInfo.bmiHeader.biBitCount = 24;
    bmpInfo.bmiHeader.biCompression = BI_RGB;
    
    g_hViewerBitmap = CreateDIBSection(g_hdcViewer, &bmpInfo, DIB_RGB_COLORS,
                                       (void**)&g_pViewerPixels, NULL, 0);
    if (!g_hViewerBitmap) {
        DeleteDC(g_hdcViewer);
        g_hdcViewer = NULL;
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }
    
    g_hViewerBitmapOld = (HBITMAP)SelectObject(g_hdcViewer, g_hViewerBitmap);
    
    /* Clear to black */
    {
        int stride = ((g_remoteScreen.width * 3 + 3) & ~3);
        ZeroMemory(g_pViewerPixels, stride * g_remoteScreen.height);
    }
    
    g_decompressBufferSize = g_remoteScreen.width * g_remoteScreen.height * 4;
    g_pDecompressBuffer = (BYTE*)calloc(1, g_decompressBufferSize);  /* Use calloc to zero memory */
    
    ReleaseDC(NULL, hdcScreen);
    return TRUE;
}

/* Destroy viewer bitmap */
void DestroyViewerBitmap(void)
{
    SAFE_FREE(g_pDecompressBuffer);
    g_decompressBufferSize = 0;
    
    if (g_hdcViewer) {
        if (g_hViewerBitmapOld) {
            SelectObject(g_hdcViewer, g_hViewerBitmapOld);
            g_hViewerBitmapOld = NULL;
        }
        DeleteDC(g_hdcViewer);
        g_hdcViewer = NULL;
    }
    
    if (g_hViewerBitmap) {
        DeleteObject(g_hViewerBitmap);
        g_hViewerBitmap = NULL;
    }
    
    g_pViewerPixels = NULL;
}

/* Handle screen update - Windows 2000 compatible */
void HandleScreenUpdate(const BYTE *data, DWORD dataLength)
{
    RD2K_RECT *pRect;
    BYTE *pSrcPixels;
    int dstStride, x, y, w, h, row;
    DWORD expectedSize;
    
    if (!data || dataLength < sizeof(RD2K_RECT)) return;
    if (!g_pViewerPixels || !g_pDecompressBuffer) return;
    
    pRect = (RD2K_RECT*)data;
    
    /* Extract rectangle info */
    x = pRect->x;
    y = pRect->y;
    w = pRect->width;
    h = pRect->height;
    
    /* Basic validation */
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (x < 0 || y < 0) return;
    if (x >= (int)g_remoteScreen.width || y >= (int)g_remoteScreen.height) return;
    
    /* Clamp to screen bounds */
    if (x + w > (int)g_remoteScreen.width) w = (int)g_remoteScreen.width - x;
    if (y + h > (int)g_remoteScreen.height) h = (int)g_remoteScreen.height - y;
    if (w <= 0 || h <= 0) return;
    
    /* Calculate expected decompressed size */
    expectedSize = (DWORD)(w * h * 3);
    
    /* Decompress if RLE encoded */
    if (pRect->encoding == COMPRESS_RLE) {
        BYTE *pCompressed = (BYTE*)(data + sizeof(RD2K_RECT));
        DWORD decompSize;
        
        if (pRect->dataSize == 0 || pRect->dataSize > dataLength - sizeof(RD2K_RECT)) return;
        
        /* Clear decompress buffer to black before decompression to avoid artifacts
         * from partial decompression or corrupted data */
        ZeroMemory(g_pDecompressBuffer, expectedSize);
        
        decompSize = DecompressRLE(pCompressed, pRect->dataSize,
                                   g_pDecompressBuffer, g_decompressBufferSize);
        
        if (decompSize < expectedSize) return;
        pSrcPixels = g_pDecompressBuffer;
    } else {
        /* Raw data */
        if (dataLength < sizeof(RD2K_RECT) + expectedSize) return;
        pSrcPixels = (BYTE*)(data + sizeof(RD2K_RECT));
    }
    
    /* Calculate destination stride (DWORD aligned) */
    dstStride = ((g_remoteScreen.width * 3 + 3) & ~3);
    
    /* Copy row by row to viewer bitmap */
    for (row = 0; row < h; row++) {
        BYTE *pDst = g_pViewerPixels + ((y + row) * dstStride) + (x * 3);
        BYTE *pSrc = pSrcPixels + (row * w * 3);
        memcpy(pDst, pSrc, w * 3);
    }
    
    /* Request repaint */
    if (g_hViewerWnd && IsWindow(g_hViewerWnd)) {
        InvalidateRect(g_hViewerWnd, NULL, FALSE);
    }
}

/* Toggle fullscreen */
void ToggleFullscreen(void)
{
    if (!g_hViewerWnd) return;
    
    if (!g_bFullscreen) {
        /* Save current window state */
        GetWindowRect(g_hViewerWnd, &g_rcViewerNormal);
        g_dwViewerStyle = GetWindowLong(g_hViewerWnd, GWL_STYLE);
        
        /* Remove borders and maximize */
        SetWindowLong(g_hViewerWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetMenu(g_hViewerWnd, NULL);
        SetWindowPos(g_hViewerWnd, HWND_TOP, 0, 0,
                    GetSystemMetrics(SM_CXSCREEN),
                    GetSystemMetrics(SM_CYSCREEN),
                    SWP_FRAMECHANGED);
        
        /* Create fullscreen toolbar */
        CreateFullscreenToolbar();
        
        g_bFullscreen = TRUE;
    } else {
        /* Destroy fullscreen toolbar */
        DestroyFullscreenToolbar();
        
        /* Restore window */
        SetWindowLong(g_hViewerWnd, GWL_STYLE, g_dwViewerStyle);
        SetMenu(g_hViewerWnd, CreateViewerMenu());
        
        SetWindowPos(g_hViewerWnd, NULL,
                    g_rcViewerNormal.left, g_rcViewerNormal.top,
                    g_rcViewerNormal.right - g_rcViewerNormal.left,
                    g_rcViewerNormal.bottom - g_rcViewerNormal.top,
                    SWP_FRAMECHANGED | SWP_NOZORDER);
        
        g_bFullscreen = FALSE;
    }
}

/* Send mouse event */
void SendMouseEvent(HWND hwnd, int x, int y, BYTE buttons, BYTE flags, SHORT wheel)
{
    RD2K_MOUSE_EVENT event;
    RECT rcClient;
    
    if (!g_pClientNet || !g_bClientConnected2) return;
    
    GetClientRect(hwnd, &rcClient);
    
    if (g_displayMode == DISPLAY_STRETCH && rcClient.right > 0 && rcClient.bottom > 0) {
        x = (x * g_remoteScreen.width) / rcClient.right;
        y = (y * g_remoteScreen.height) / rcClient.bottom;
    }
    
    event.x = (WORD)max(0, min(x, g_remoteScreen.width - 1));
    event.y = (WORD)max(0, min(y, g_remoteScreen.height - 1));
    event.buttons = buttons;
    event.flags = flags;
    event.wheelDelta = wheel;
    
    Network_SendPacket(g_pClientNet, MSG_MOUSE_EVENT, (const BYTE*)&event, sizeof(event));
}

/* Send keyboard event */
void SendKeyboardEvent(WORD vk, WORD scan, BYTE flags)
{
    RD2K_KEY_EVENT event;
    
    if (!g_pClientNet || !g_bClientConnected2) return;
    
    event.virtualKey = vk;
    event.scanCode = scan;
    event.flags = flags;
    event.reserved[0] = event.reserved[1] = event.reserved[2] = 0;
    
    Network_SendPacket(g_pClientNet, MSG_KEYBOARD_EVENT, (const BYTE*)&event, sizeof(event));
}

/* Send clipboard data - handles both text AND files */
void SendClipboardData(PRD2K_NETWORK pNet)
{
    HANDLE hData;
    
    if (!pNet) return;
    
    /* CRITICAL: Release modifier keys before opening clipboard.
       This prevents the "stuck Ctrl key" problem that causes
       Explorer to think Ctrl is held (multi-select mode). */
    Input_ReleaseAllModifiers();
    
    if (!OpenClipboard(NULL)) return;
    
    /* Check for files first (CF_HDROP) */
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        hData = GetClipboardData(CF_HDROP);
        if (hData) {
            UINT fileCount = DragQueryFileA((HDROP)hData, 0xFFFFFFFF, NULL, 0);
            UINT i;
            for (i = 0; i < fileCount; i++) {
                char filePath[MAX_PATH];
                if (DragQueryFileA((HDROP)hData, i, filePath, MAX_PATH) > 0) {
                    /* Check if it's a file (not directory) */
                    DWORD dwAttr = GetFileAttributesA(filePath);
                    if (dwAttr != 0xFFFFFFFF && !(dwAttr & FILE_ATTRIBUTE_DIRECTORY)) {
                        CloseClipboard();
                        SendFileToRemote(filePath);
                        return;
                    }
                }
            }
        }
    }
    
    /* Then check for text */
    if (IsClipboardFormatAvailable(CF_TEXT)) {
        hData = GetClipboardData(CF_TEXT);
        if (hData) {
            char *pText = (char*)GlobalLock(hData);
            if (pText) {
                DWORD len = (DWORD)strlen(pText);
                if (len > 0) {
                    BYTE *buffer = (BYTE*)malloc(sizeof(RD2K_CLIPBOARD) + len);
                    if (buffer) {
                        RD2K_CLIPBOARD *pClip = (RD2K_CLIPBOARD*)buffer;
                        pClip->dataLength = len;
                        pClip->isFile = FALSE;
                        pClip->reserved[0] = pClip->reserved[1] = pClip->reserved[2] = 0;
                        memcpy(buffer + sizeof(RD2K_CLIPBOARD), pText, len);
                        Network_SendPacket(pNet, MSG_CLIPBOARD_TEXT, buffer, sizeof(RD2K_CLIPBOARD) + len);
                        free(buffer);
                    }
                }
                GlobalUnlock(hData);
            }
        }
    }
    
    CloseClipboard();
}

/* Receive clipboard text */
void ReceiveClipboardText(const BYTE *data, DWORD length)
{
    RD2K_CLIPBOARD *pClip;
    char *pText;
    HGLOBAL hMem;
    
    if (!data || length < sizeof(RD2K_CLIPBOARD)) return;
    
    pClip = (RD2K_CLIPBOARD*)data;
    if (pClip->isFile) return;  /* Handle files separately */
    
    pText = (char*)(data + sizeof(RD2K_CLIPBOARD));
    
    /* Ignore clipboard change we're about to make */
    g_bIgnoreClipboard = TRUE;
    
    /* Release modifier keys before clipboard operations */
    Input_ReleaseAllModifiers();
    
    if (OpenClipboard(g_hMainWnd ? g_hMainWnd : g_hViewerWnd)) {
        EmptyClipboard();
        hMem = GlobalAlloc(GMEM_MOVEABLE, pClip->dataLength + 1);
        if (hMem) {
            char *pDst = (char*)GlobalLock(hMem);
            memcpy(pDst, pText, pClip->dataLength);
            pDst[pClip->dataLength] = '\0';
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
        g_lastClipboardSeq = GetClipboardSequenceNumber();
    }
    
    g_bIgnoreClipboard = FALSE;
}

/* Send file to remote - wrapper for file transfer module */
/* IMPORTANT: Always use SYNC method to avoid socket race conditions.
   Both client and server have network processing running on timers,
   so async (worker thread) would cause socket access from multiple threads. */
void SendFileToRemote(const char *filePath)
{
    PRD2K_NETWORK pNet;
    HWND hNotifyWnd;
    int result;
    
    if (!filePath) return;
    
    if (g_bClientConnected2 && g_pClientNet) {
        /* We are the CLIENT (viewer) - sending TO remote server */
        pNet = g_pClientNet;
        hNotifyWnd = g_hViewerWnd ? g_hViewerWnd : g_hMainWnd;
    } else if (g_bClientConnected && g_pServerNet) {
        /* We are the SERVER - sending back TO client */
        pNet = g_pServerNet;
        hNotifyWnd = g_hMainWnd;
    } else {
        return;  /* No connection */
    }
    
    /* Always use SYNC to avoid race conditions with network timer processing */
    result = FileTransfer_SendFileSync(pNet, filePath, hNotifyWnd);
    
    if (result != FT_SUCCESS && result != FT_ERR_CANCELLED) {
        MessageBoxA(g_hMainWnd, FileTransfer_GetLastError(), APP_TITLE, MB_ICONERROR);
    }
}

/* Receive file start - wrapper for file transfer module */
void ReceiveFileStart(const BYTE *data, DWORD length)
{
    char destFolder[MAX_PATH] = {0};
    
    /* CRITICAL: Try to capture the active Explorer folder RIGHT NOW.
       This is the moment when the user initiates a file transfer (paste/send).
       We want to know what folder they're looking at in Explorer at this instant.
       
       Previously, we only captured the folder when the viewer window LOST focus,
       but that misses the case where:
       1. User has Explorer open
       2. User pastes into remote desktop viewer
       3. Remote sends file back
       4. File should go to the Explorer folder user was looking at
       
       By capturing at receive time, we get the most current folder. */
    if (FileTransfer_GetActiveFolder(destFolder, sizeof(destFolder))) {
        /* Successfully got active folder - use it directly */
        int result = FileTransfer_StartReceive(data, length, destFolder);
        if (result != FT_SUCCESS) {
            /* Error already handled in module */
        }
    } else {
        /* Fall back to remembered folder or desktop */
        int result = FileTransfer_StartReceive(data, length, NULL);
        if (result != FT_SUCCESS) {
            /* Error already handled in module */
        }
    }
}

/* Receive file data - wrapper for file transfer module */
void ReceiveFileData(const BYTE *data, DWORD length)
{
    FileTransfer_ReceiveData(data, length);
}

/* Receive file end - wrapper for file transfer module */
void ReceiveFileEnd(void)
{
    FileTransfer_EndReceive();
}

/* Viewer window procedure */
LRESULT CALLBACK ViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_ACTIVATE:
        {
            /* Handle focus changes properly to prevent stuck modifier keys */
            if (LOWORD(wParam) == WA_ACTIVE || LOWORD(wParam) == WA_CLICKACTIVE) {
                /* Viewer window gaining focus */
                /* NOTE: Do NOT capture folder here - the previous window is gone by now.
                   Instead, we capture when we LOSE focus, or when receiving a file. */
                /* Sync our modifier key tracking with actual keyboard state */
                Input_SyncModifierState();
            } else {
                /* WA_INACTIVE - Viewer window losing focus */
                /* This is the IDEAL time to capture the folder - user is switching
                   to Explorer (or another window). If they switch to Explorer and 
                   then paste, we want to know what folder they're looking at. */
                FileTransfer_CaptureActiveFolder(hwnd);
                
                /* Release all modifier keys to prevent stuck keys in other apps */
                Input_ReleaseAllModifiers();
                /* Also send release to remote to prevent stuck keys there */
                if (g_pClientNet && g_bClientConnected2) {
                    SendKeyboardEvent(VK_CONTROL, 0x1D, 0x02); /* Ctrl up */
                    SendKeyboardEvent(VK_SHIFT, 0x2A, 0x02);   /* Shift up */
                    SendKeyboardEvent(VK_MENU, 0x38, 0x02);    /* Alt up */
                }
            }
            return 0;
        }
        
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            if (g_hdcViewer && g_bClientConnected2) {
                RECT rcClient;
                GetClientRect(hwnd, &rcClient);
                
                if (g_displayMode == DISPLAY_STRETCH) {
                    /* Use HALFTONE mode for high-quality smooth scaling
                     * This does proper bilinear interpolation instead of
                     * nearest-neighbor (COLORONCOLOR) which looks pixelated.
                     * SetBrushOrgEx is required after SetStretchBltMode(HALFTONE). */
                    SetStretchBltMode(hdc, HALFTONE);
                    SetBrushOrgEx(hdc, 0, 0, NULL);
                    StretchBlt(hdc, 0, 0, rcClient.right, rcClient.bottom,
                              g_hdcViewer, 0, 0, g_remoteScreen.width, g_remoteScreen.height,
                              SRCCOPY);
                } else {
                    BitBlt(hdc, 0, 0, g_remoteScreen.width, g_remoteScreen.height,
                          g_hdcViewer, 0, 0, SRCCOPY);
                }
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_MOUSEMOVE:
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            BYTE buttons = 0;
            if (wParam & MK_LBUTTON) buttons |= 0x01;
            if (wParam & MK_RBUTTON) buttons |= 0x02;
            if (wParam & MK_MBUTTON) buttons |= 0x04;
            
            /* Show toolbar when mouse is at top of screen in fullscreen mode */
            if (g_bFullscreen && y < 10) {
                ShowFullscreenToolbar();
            } else if (g_bFullscreen && y > g_toolbarHeight + 20 && g_bToolbarVisible) {
                /* Start hide timer when mouse moves away from toolbar area */
                SetTimer(hwnd, TIMER_TOOLBAR_HIDE, TOOLBAR_HIDE_DELAY, NULL);
            }
            
            SendMouseEvent(hwnd, x, y, buttons, 0x01, 0);
            return 0;
        }
        
        case WM_TIMER:
            if (wParam == TIMER_TOOLBAR_HIDE) {
                /* Check if mouse is not over toolbar */
                POINT pt;
                GetCursorPos(&pt);
                if (pt.y > g_toolbarHeight) {
                    HideFullscreenToolbar();
                }
            }
            else if (wParam == TIMER_CLIPBOARD_REQUEST) {
                /* Delayed clipboard request after Ctrl+C on remote.
                   Using timer instead of Sleep() prevents stuck modifier keys. */
                KillTimer(hwnd, TIMER_CLIPBOARD_REQUEST);
                if (g_pClientNet && g_bClientConnected2) {
                    Clipboard_HandleCopy(g_pClientNet);
                }
            }
            return 0;
        
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            SendMouseEvent(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0x01, 0x01 | 0x02, 0);
            return 0;
        
        case WM_LBUTTONUP:
            SendMouseEvent(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0x01, 0x01 | 0x04, 0);
            return 0;
        
        case WM_RBUTTONDOWN:
            SendMouseEvent(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0x02, 0x01 | 0x02, 0);
            return 0;
        
        case WM_RBUTTONUP:
            SendMouseEvent(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0x02, 0x01 | 0x04, 0);
            return 0;
        
        case WM_MBUTTONDOWN:
            SendMouseEvent(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0x04, 0x01 | 0x02, 0);
            return 0;
        
        case WM_MBUTTONUP:
            SendMouseEvent(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0x04, 0x01 | 0x04, 0);
            return 0;
        
        case WM_MOUSEWHEEL:
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ScreenToClient(hwnd, &pt);
            SendMouseEvent(hwnd, pt.x, pt.y, 0, 0x08, (SHORT)HIWORD(wParam));
            return 0;
        }
        
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            WORD vk = (WORD)wParam;
            WORD scan = (WORD)((lParam >> 16) & 0xFF);
            BYTE flags = 0x01;
            if (lParam & 0x01000000) flags |= 0x04;
            
            /* Handle F11 for fullscreen */
            if (vk == VK_F11) {
                ToggleFullscreen();
                return 0;
            }
            
            /* Handle F5 for refresh */
            if (vk == VK_F5) {
                Network_SendPacket(g_pClientNet, MSG_FULL_SCREEN_REQ, NULL, 0);
                return 0;
            }
            
            /* Handle Ctrl+Shift+V - RECEIVE files FROM remote TO local host */
            /* This is the reverse of Ctrl+V (which sends files TO remote) */
            /* User workflow: 1) Ctrl+C on remote, 2) Switch to Host, 3) Open folder, 4) Ctrl+Shift+V */
            if (vk == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
                if (g_pClientNet && g_bClientConnected2) {
                    /* Release modifier keys */
                    SendKeyboardEvent(VK_CONTROL, 0x1D, 0x02); /* Ctrl up */
                    SendKeyboardEvent(VK_SHIFT, 0x2A, 0x02);   /* Shift up */
                    Input_ReleaseAllModifiers();
                    
                    /* Request file transfer FROM remote */
                    /* Server will send its clipboard files to us */
                    if (!FileTransfer_IsBusy()) {
                        Network_SendPacket(g_pClientNet, MSG_FILE_REQ, NULL, 0);
                    }
                    return 0;
                }
            }
            
            /* Handle Ctrl+V - paste to remote */
            /* For FILES: Transfer actual files (not just paths) */
            /* For TEXT: Sync text to remote clipboard, then Ctrl+V */
            if (vk == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (g_pClientNet && g_bClientConnected2) {
                    int clipType;
                    
                    /* CRITICAL: First send Ctrl key-up to remote to prevent stuck Ctrl.
                       Then release local modifiers before clipboard operations. */
                    SendKeyboardEvent(VK_CONTROL, 0x1D, 0x02); /* Ctrl up */
                    
                    clipType = Clipboard_GetLocalType();
                    
                    if (clipType == CLIP_TYPE_FILES) {
                        /* Transfer actual files - don't send Ctrl+V */
                        Clipboard_TransferFiles(g_pClientNet);
                        return 0;  /* Don't send Ctrl+V for file transfer */
                    } else if (clipType == CLIP_TYPE_TEXT) {
                        /* Sync text to remote clipboard */
                        Clipboard_HandlePaste(g_pClientNet);
                        /* Then send Ctrl+V so remote pastes */
                        /* Need to re-press Ctrl, then V, then release both */
                        SendKeyboardEvent(VK_CONTROL, 0x1D, 0x01); /* Ctrl down */
                        SendKeyboardEvent('V', (WORD)scan, 0x01); /* V down */
                        SendKeyboardEvent('V', (WORD)scan, 0x02); /* V up */
                        SendKeyboardEvent(VK_CONTROL, 0x1D, 0x02); /* Ctrl up */
                        return 0;
                    }
                }
                /* No valid clipboard content, just send key normally */
                SendKeyboardEvent(vk, scan, flags);
                return 0;
            }
            
            /* Handle Ctrl+C - copy from remote */
            /* Send Ctrl+C to remote, then use timer to request clipboard later */
            if (vk == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                /* Send complete Ctrl+C sequence to remote */
                SendKeyboardEvent(VK_CONTROL, 0x1D, 0x01); /* Ctrl down */
                SendKeyboardEvent('C', (WORD)scan, 0x01); /* C down */
                SendKeyboardEvent('C', (WORD)scan, 0x02); /* C up */
                SendKeyboardEvent(VK_CONTROL, 0x1D, 0x02); /* Ctrl up */
                
                /* Request clipboard from remote after a short delay using timer.
                   DO NOT use Sleep() here - it blocks the message pump and can
                   cause the Ctrl key to appear stuck. */
                if (g_pClientNet && g_bClientConnected2) {
                    SetTimer(hwnd, TIMER_CLIPBOARD_REQUEST, 150, NULL);
                }
                return 0;
            }
            
            SendKeyboardEvent(vk, scan, flags);
            return 0;
        }
        
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            WORD vk = (WORD)wParam;
            WORD scan = (WORD)((lParam >> 16) & 0xFF);
            BYTE flags = 0x02;
            if (lParam & 0x01000000) flags |= 0x04;
            SendKeyboardEvent(vk, scan, flags);
            return 0;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_VIEWER_FULLSCREEN:
                    ToggleFullscreen();
                    break;
                
                case IDM_VIEWER_ACTUAL:
                    g_displayMode = DISPLAY_NORMAL;
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                
                case IDM_VIEWER_STRETCH:
                    g_displayMode = DISPLAY_STRETCH;
                    InvalidateRect(hwnd, NULL, TRUE);
                    break;
                
                case IDM_VIEWER_REFRESH:
                    if (g_pClientNet) {
                        Network_SendPacket(g_pClientNet, MSG_FULL_SCREEN_REQ, NULL, 0);
                    }
                    break;
                
                case IDM_VIEWER_SENDFILE:
                {
                    OPENFILENAMEA ofn;
                    char fileName[MAX_PATH] = "";
                    
                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = "All Files\0*.*\0";
                    ofn.lpstrFile = fileName;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST;
                    ofn.lpstrTitle = "Select file to send";
                    
                    if (GetOpenFileNameA(&ofn)) {
                        SendFileToRemote(fileName);
                    }
                    break;
                }
                
                case IDM_VIEWER_RECEIVEFILE:
                    /* Receive files FROM remote TO local host */
                    /* Files will go to the active Explorer folder or Desktop */
                    if (g_pClientNet && g_bClientConnected2 && !FileTransfer_IsBusy()) {
                        Input_ReleaseAllModifiers();
                        Network_SendPacket(g_pClientNet, MSG_FILE_REQ, NULL, 0);
                    }
                    break;
                
                case IDM_VIEWER_CLIPBOARD:
                    SendClipboardData(g_pClientNet);
                    break;
                
                case IDM_VIEWER_DISCONNECT:
                    DisconnectFromPartner();
                    break;
            }
            return 0;
        
        case WM_DRAWCLIPBOARD:
            /* Pass to next viewer in chain only - no auto sync */
            if (g_hNextClipViewer) {
                SendMessage(g_hNextClipViewer, msg, wParam, lParam);
            }
            return 0;
        
        case WM_CHANGECBCHAIN:
            /* Another viewer is being removed from chain */
            if ((HWND)wParam == g_hNextClipViewer) {
                g_hNextClipViewer = (HWND)lParam;
            } else if (g_hNextClipViewer) {
                SendMessage(g_hNextClipViewer, msg, wParam, lParam);
            }
            return 0;
        
        case WM_DROPFILES:
        {
            /* Handle drag-drop of files onto viewer window */
            HDROP hDrop = (HDROP)wParam;
            UINT fileCount, i;
            char filePath[MAX_PATH];
            DWORD attrs;
            
            if (!g_pClientNet || !g_bClientConnected2) {
                DragFinish(hDrop);
                return 0;
            }
            
            fileCount = DragQueryFileA(hDrop, 0xFFFFFFFF, NULL, 0);
            
            for (i = 0; i < fileCount && i < 10; i++) {
                if (DragQueryFileA(hDrop, i, filePath, MAX_PATH)) {
                    attrs = GetFileAttributesA(filePath);
                    if (attrs != 0xFFFFFFFF && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        SendFileToRemote(filePath);
                    }
                }
            }
            
            DragFinish(hDrop);
            return 0;
        }
        
        case WM_CLOSE:
            DisconnectFromPartner();
            return 0;
        
        case WM_ERASEBKGND:
            return 1;
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ============================================================
 * FULLSCREEN TOOLBAR FUNCTIONS
 * ============================================================ */

/* Create fullscreen toolbar window */
void CreateFullscreenToolbar(void)
{
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int btnWidth = 90, btnHeight = 24, btnSpacing = 5;
    int totalWidth = (btnWidth + btnSpacing) * 6 - btnSpacing;
    int startX = (screenWidth - totalWidth) / 2;
    int y = 4;
    HWND hBtn;
    
    if (g_hToolbarWnd) return;
    
    /* Create toolbar window - initially hidden */
    g_hToolbarWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "RD2KToolbarClass", NULL,
        WS_POPUP,
        0, -g_toolbarHeight, screenWidth, g_toolbarHeight,
        g_hViewerWnd, NULL, g_hInstance, NULL);
    
    if (!g_hToolbarWnd) return;
    
    /* Create toolbar buttons */
    hBtn = CreateWindowExA(0, "BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_DISCONNECT, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    startX += btnWidth + btnSpacing;
    hBtn = CreateWindowExA(0, "BUTTON", "Send File",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_SENDFILE, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    startX += btnWidth + btnSpacing;
    hBtn = CreateWindowExA(0, "BUTTON", "Receive",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_RECEIVEFILE, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    startX += btnWidth + btnSpacing;
    hBtn = CreateWindowExA(0, "BUTTON", "Clipboard",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_CLIPBOARD, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    startX += btnWidth + btnSpacing;
    hBtn = CreateWindowExA(0, "BUTTON", "Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_REFRESH, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    startX += btnWidth + btnSpacing;
    hBtn = CreateWindowExA(0, "BUTTON", "Stretch",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_STRETCH, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    startX += btnWidth + btnSpacing;
    hBtn = CreateWindowExA(0, "BUTTON", "Exit FS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        startX, y, btnWidth, btnHeight,
        g_hToolbarWnd, (HMENU)IDC_TB_FULLSCREEN, g_hInstance, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    
    g_bToolbarVisible = FALSE;
}

/* Destroy fullscreen toolbar */
void DestroyFullscreenToolbar(void)
{
    if (g_hToolbarWnd) {
        DestroyWindow(g_hToolbarWnd);
        g_hToolbarWnd = NULL;
    }
    g_bToolbarVisible = FALSE;
}

/* Show fullscreen toolbar with animation */
void ShowFullscreenToolbar(void)
{
    if (!g_hToolbarWnd || g_bToolbarVisible) return;
    
    SetWindowPos(g_hToolbarWnd, HWND_TOPMOST, 0, 0,
                 GetSystemMetrics(SM_CXSCREEN), g_toolbarHeight,
                 SWP_SHOWWINDOW);
    g_bToolbarVisible = TRUE;
    
    /* Set timer to auto-hide */
    SetTimer(g_hViewerWnd, TIMER_TOOLBAR_HIDE, TOOLBAR_HIDE_DELAY, NULL);
}

/* Hide fullscreen toolbar */
void HideFullscreenToolbar(void)
{
    if (!g_hToolbarWnd || !g_bToolbarVisible) return;
    
    SetWindowPos(g_hToolbarWnd, NULL, 0, -g_toolbarHeight,
                 GetSystemMetrics(SM_CXSCREEN), g_toolbarHeight,
                 SWP_NOZORDER | SWP_HIDEWINDOW);
    g_bToolbarVisible = FALSE;
    
    KillTimer(g_hViewerWnd, TIMER_TOOLBAR_HIDE);
}

/* Toolbar window procedure */
LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_TB_DISCONNECT:
                    DisconnectFromPartner();
                    break;
                
                case IDC_TB_SENDFILE:
                {
                    OPENFILENAMEA ofn;
                    char fileName[MAX_PATH] = "";
                    
                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = "All Files\0*.*\0";
                    ofn.lpstrFile = fileName;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST;
                    ofn.lpstrTitle = "Select file to send";
                    
                    if (GetOpenFileNameA(&ofn)) {
                        SendFileToRemote(fileName);
                    }
                    break;
                }
                
                case IDC_TB_CLIPBOARD:
                    SendClipboardData(g_pClientNet);
                    break;
                
                case IDC_TB_RECEIVEFILE:
                    /* Receive files FROM remote TO local host */
                    /* Files will go to the active Explorer folder or Desktop */
                    if (g_pClientNet && g_bClientConnected2) {
                        if (FileTransfer_IsBusy()) {
                            MessageBoxA(hwnd, "A file transfer is already in progress.", 
                                       APP_TITLE, MB_ICONINFORMATION);
                        } else {
                            Input_ReleaseAllModifiers();
                            /* Capture destination folder before requesting */
                            FileTransfer_CaptureActiveFolder(g_hViewerWnd);
                            /* Request files from remote's clipboard */
                            Network_SendPacket(g_pClientNet, MSG_FILE_REQ, NULL, 0);
                            UpdateStatusBar("Requesting files from remote...", TRUE);
                        }
                    } else {
                        MessageBoxA(hwnd, "Not connected to remote.", APP_TITLE, MB_ICONWARNING);
                    }
                    break;
                
                case IDC_TB_REFRESH:
                    if (g_pClientNet) {
                        Network_SendPacket(g_pClientNet, MSG_FULL_SCREEN_REQ, NULL, 0);
                    }
                    break;
                
                case IDC_TB_STRETCH:
                    g_displayMode = (g_displayMode == DISPLAY_STRETCH) ? DISPLAY_NORMAL : DISPLAY_STRETCH;
                    if (g_hViewerWnd) {
                        InvalidateRect(g_hViewerWnd, NULL, TRUE);
                    }
                    break;
                
                case IDC_TB_FULLSCREEN:
                    ToggleFullscreen();
                    break;
            }
            return 0;
        
        case WM_MOUSEMOVE:
            /* Reset hide timer when mouse is on toolbar */
            KillTimer(g_hViewerWnd, TIMER_TOOLBAR_HIDE);
            SetTimer(g_hViewerWnd, TIMER_TOOLBAR_HIDE, TOOLBAR_HIDE_DELAY, NULL);
            return 0;
        
        case WM_CTLCOLORBTN:
        {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(50, 50, 50));
            SetTextColor(hdc, RGB(255, 255, 255));
            return (LRESULT)GetStockObject(GRAY_BRUSH);
        }
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Check clipboard and sync automatically */
void CheckClipboardAndSync(void)
{
    PRD2K_NETWORK pNet = g_bClientConnected2 ? g_pClientNet : 
                         (g_bClientConnected ? g_pServerNet : NULL);
    
    if (!pNet) return;
    
    /* Get current clipboard sequence number */
    /* Note: GetClipboardSequenceNumber not available in Win2K, use simple check */
    if (OpenClipboard(NULL)) {
        BOOL hasText = IsClipboardFormatAvailable(CF_TEXT);
        BOOL hasFiles = IsClipboardFormatAvailable(CF_HDROP);
        CloseClipboard();
        
        if (hasText || hasFiles) {
            /* Clipboard has content, could sync here if needed */
        }
    }
}