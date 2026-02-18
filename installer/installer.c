/*
 * RemoteDesk2K Installer
 * Classic Windows 2000 style GUI installer
 * 
 * Features:
 * - Pre-configured Relay Address (set by admin who builds installer)
 * - Supports domain:port, IP:port, or Server ID formats
 * - Installs to Program Files\RemoteDesk2K
 * - Creates Start Menu shortcut
 * - Creates client_config.ini with relay address
 */

#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

/* ============================================================================
 * CONFIGURATION - Admin sets this before building installer
 * ============================================================================ */
#define DEFAULT_RELAY_ADDR      ""   /* Set relay address: domain:port, IP:port, or Server ID */
#define APP_NAME               "RemoteDesk2K"
#define APP_VERSION            "1.0"
#define INSTALLER_TITLE        "RemoteDesk2K Setup"

/* Resource ID for embedded RemoteDesk2K.exe */
#define IDR_REMOTEDESK2K_EXE    1000

/* Window dimensions */
#define WINDOW_WIDTH    480
#define WINDOW_HEIGHT   360

/* Control IDs */
#define IDC_HEADER_STATIC   100
#define IDC_INFO_STATIC     101
#define IDC_SERVER_LABEL    102
#define IDC_SERVER_ID       103
#define IDC_PATH_LABEL      104
#define IDC_PATH_EDIT       105
#define IDC_BROWSE_BTN      106
#define IDC_PROGRESS        107
#define IDC_STATUS_STATIC   108
#define IDC_INSTALL_BTN     109
#define IDC_CANCEL_BTN      110
#define IDC_DESKTOP_CHK     111
#define IDC_STARTMENU_CHK   112

/* Global variables */
static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static HWND g_hProgress = NULL;
static HWND g_hStatus = NULL;
static HWND g_hServerIdEdit = NULL;
static HWND g_hPathEdit = NULL;
static HWND g_hInstallBtn = NULL;
static HWND g_hDesktopChk = NULL;
static HWND g_hStartMenuChk = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontTitle = NULL;
static HBRUSH g_hBrushHeader = NULL;
static BOOL g_bInstalling = FALSE;

/* Forward declarations */
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL DoInstall(HWND hwnd);
BOOL CreateShortcut(const char* targetPath, const char* shortcutPath, const char* description);
BOOL ExtractEmbeddedExe(const char* destPath);
void UpdateStatus(const char* text);
void SetProgress(int percent);

/* Extract embedded RemoteDesk2K.exe from resource to destination path */
BOOL ExtractEmbeddedExe(const char* destPath) {
    HRSRC hRes;
    HGLOBAL hResData;
    LPVOID pData;
    DWORD dwSize;
    HANDLE hFile;
    DWORD dwWritten;
    BOOL success = FALSE;
    
    /* Find the embedded exe resource */
    hRes = FindResourceA(g_hInstance, MAKEINTRESOURCEA(IDR_REMOTEDESK2K_EXE), RT_RCDATA);
    if (!hRes) {
        return FALSE;
    }
    
    /* Load the resource */
    hResData = LoadResource(g_hInstance, hRes);
    if (!hResData) {
        return FALSE;
    }
    
    /* Get pointer to resource data */
    pData = LockResource(hResData);
    if (!pData) {
        return FALSE;
    }
    
    /* Get size of resource */
    dwSize = SizeofResource(g_hInstance, hRes);
    if (dwSize == 0) {
        return FALSE;
    }
    
    /* Create destination file */
    hFile = CreateFileA(destPath, GENERIC_WRITE, 0, NULL, 
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    
    /* Write resource data to file */
    if (WriteFile(hFile, pData, dwSize, &dwWritten, NULL) && dwWritten == dwSize) {
        success = TRUE;
    }
    
    CloseHandle(hFile);
    return success;
}

/* Get default install path */
void GetDefaultInstallPath(char* path, int maxLen) {
    char progFiles[MAX_PATH];
    
    /* Try to get Program Files path */
    if (SHGetFolderPathA(NULL, CSIDL_PROGRAM_FILES, NULL, 0, progFiles) == S_OK) {
        _snprintf(path, maxLen, "%s\\%s", progFiles, APP_NAME);
    } else {
        /* Fallback */
        _snprintf(path, maxLen, "C:\\Program Files\\%s", APP_NAME);
    }
}

/* Create all directories in path */
BOOL CreateDirectoryRecursive(const char* path) {
    char tmp[MAX_PATH];
    char* p = NULL;
    size_t len;
    
    _snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    /* Remove trailing slash */
    if (tmp[len - 1] == '\\') {
        tmp[len - 1] = '\0';
    }
    
    /* Create each directory */
    for (p = tmp + 1; *p; p++) {
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Copy file with progress */
BOOL CopyFileWithProgress(const char* src, const char* dst, HWND hwndProgress) {
    return CopyFileA(src, dst, FALSE);
}

/* Create entries */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSA wc;
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    
    g_hInstance = hInstance;
    
    /* Initialize common controls */
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    
    /* Initialize COM for shell operations */
    CoInitialize(NULL);
    
    /* Create fonts */
    g_hFontNormal = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    
    g_hFontBold = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    
    g_hFontTitle = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    
    /* Create header brush (classic blue) */
    g_hBrushHeader = CreateSolidBrush(RGB(0, 51, 153));
    
    /* Register window class */
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RD2KInstallerWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    
    /* Create main window */
    g_hMainWnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        "RD2KInstallerWnd",
        INSTALLER_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hInstance, NULL);
    
    if (!g_hMainWnd) {
        MessageBoxA(NULL, "Failed to create window!", "Error", MB_ICONERROR);
        return 1;
    }
    
    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);
    
    /* Message loop */
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    /* Cleanup */
    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    if (g_hFontTitle) DeleteObject(g_hFontTitle);
    if (g_hBrushHeader) DeleteObject(g_hBrushHeader);
    
    CoUninitialize();
    
    return (int)msg.wParam;
}

/* Create controls */
void CreateControls(HWND hwnd) {
    char defaultPath[MAX_PATH];
    RECT rc;
    int y;
    
    GetClientRect(hwnd, &rc);
    
    /* Header panel (blue area at top) - drawn in WM_PAINT */
    
    /* Welcome text - positioned in header area */
    CreateWindowExA(0, "STATIC", "Welcome to RemoteDesk2K Setup",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 15, rc.right - 20, 25, hwnd, (HMENU)IDC_HEADER_STATIC, g_hInstance, NULL);
    
    CreateWindowExA(0, "STATIC", "This wizard will install RemoteDesk2K on your computer.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 42, rc.right - 20, 18, hwnd, (HMENU)IDC_INFO_STATIC, g_hInstance, NULL);
    
    y = 80;
    
    /* Relay Address section */
    CreateWindowExA(0, "STATIC", "Relay Address (e.g. relay.example.com:5000):",
        WS_CHILD | WS_VISIBLE,
        20, y, 350, 18, hwnd, (HMENU)IDC_SERVER_LABEL, g_hInstance, NULL);
    y += 22;
    
    g_hServerIdEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", DEFAULT_RELAY_ADDR,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        20, y, 250, 24, hwnd, (HMENU)IDC_SERVER_ID, g_hInstance, NULL);
    y += 35;
    
    /* Install path section */
    CreateWindowExA(0, "STATIC", "Install location:",
        WS_CHILD | WS_VISIBLE,
        20, y, 200, 18, hwnd, (HMENU)IDC_PATH_LABEL, g_hInstance, NULL);
    y += 22;
    
    GetDefaultInstallPath(defaultPath, sizeof(defaultPath));
    g_hPathEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", defaultPath,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        20, y, 340, 22, hwnd, (HMENU)IDC_PATH_EDIT, g_hInstance, NULL);
    
    CreateWindowExA(0, "BUTTON", "Browse...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        365, y, 75, 22, hwnd, (HMENU)IDC_BROWSE_BTN, g_hInstance, NULL);
    y += 35;
    
    /* Options */
    g_hDesktopChk = CreateWindowExA(0, "BUTTON", "Create Desktop shortcut",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, y, 200, 20, hwnd, (HMENU)IDC_DESKTOP_CHK, g_hInstance, NULL);
    SendMessage(g_hDesktopChk, BM_SETCHECK, BST_CHECKED, 0);
    y += 22;
    
    g_hStartMenuChk = CreateWindowExA(0, "BUTTON", "Create Start Menu shortcut",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, y, 200, 20, hwnd, (HMENU)IDC_STARTMENU_CHK, g_hInstance, NULL);
    SendMessage(g_hStartMenuChk, BM_SETCHECK, BST_CHECKED, 0);
    y += 30;
    
    /* Progress bar */
    g_hProgress = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
        WS_CHILD | PBS_SMOOTH,
        20, y, rc.right - 40, 18, hwnd, (HMENU)IDC_PROGRESS, g_hInstance, NULL);
    y += 22;
    
    /* Status text - use SS_SUNKEN for proper background redraw */
    g_hStatus = CreateWindowExA(WS_EX_STATICEDGE, "STATIC", "Ready to install.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        20, y, rc.right - 40, 20, hwnd, (HMENU)IDC_STATUS_STATIC, g_hInstance, NULL);
    
    /* Buttons at bottom */
    g_hInstallBtn = CreateWindowExA(0, "BUTTON", "Install",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        rc.right - 180, rc.bottom - 40, 75, 26, hwnd, (HMENU)IDC_INSTALL_BTN, g_hInstance, NULL);
    
    CreateWindowExA(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        rc.right - 95, rc.bottom - 40, 75, 26, hwnd, (HMENU)IDC_CANCEL_BTN, g_hInstance, NULL);
    
    /* Set fonts */
    SendMessage(GetDlgItem(hwnd, IDC_HEADER_STATIC), WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);
    SendMessage(GetDlgItem(hwnd, IDC_INFO_STATIC), WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(GetDlgItem(hwnd, IDC_SERVER_LABEL), WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    SendMessage(g_hServerIdEdit, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    SendMessage(GetDlgItem(hwnd, IDC_PATH_LABEL), WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hPathEdit, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(GetDlgItem(hwnd, IDC_BROWSE_BTN), WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hDesktopChk, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hStartMenuChk, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hInstallBtn, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    SendMessage(GetDlgItem(hwnd, IDC_CANCEL_BTN), WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
}

/* Update status text */
void UpdateStatus(const char* text) {
    if (g_hStatus) {
        HDC hdc;
        RECT rc;
        MSG msg;
        
        /* First clear the control by setting empty text */
        SetWindowTextA(g_hStatus, "");
        
        /* Get control rect and repaint with background */
        GetClientRect(g_hStatus, &rc);
        hdc = GetDC(g_hStatus);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        ReleaseDC(g_hStatus, hdc);
        
        /* Now set the new text */
        SetWindowTextA(g_hStatus, text);
        InvalidateRect(g_hStatus, NULL, TRUE);
        UpdateWindow(g_hStatus);
        
        /* Process pending messages */
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

/* Set progress bar */
void SetProgress(int percent) {
    if (g_hProgress) {
        ShowWindow(g_hProgress, SW_SHOW);
        SendMessage(g_hProgress, PBM_SETPOS, percent, 0);
    }
}

/* Browse for folder */
void BrowseForFolder(HWND hwnd) {
    BROWSEINFOA bi;
    char path[MAX_PATH];
    LPITEMIDLIST pidl;
    
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select installation folder:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        if (SHGetPathFromIDListA(pidl, path)) {
            /* Append app name */
            if (path[strlen(path) - 1] != '\\') {
                strcat(path, "\\");
            }
            strcat(path, APP_NAME);
            SetWindowTextA(g_hPathEdit, path);
        }
        CoTaskMemFree(pidl);
    }
}

/* Create shortcut using COM */
BOOL CreateShortcut(const char* targetPath, const char* shortcutPath, const char* description) {
    IShellLinkA* psl = NULL;
    IPersistFile* ppf = NULL;
    WCHAR wsz[MAX_PATH];
    HRESULT hr;
    char workDir[MAX_PATH];
    char* lastSlash;
    
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkA, (void**)&psl);
    if (FAILED(hr)) return FALSE;
    
    /* Set the target path - MUST be absolute path */
    psl->lpVtbl->SetPath(psl, targetPath);
    
    /* Set description tooltip */
    psl->lpVtbl->SetDescription(psl, description);
    
    /* Set working directory (folder where exe is located) */
    strcpy(workDir, targetPath);
    lastSlash = strrchr(workDir, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
    }
    psl->lpVtbl->SetWorkingDirectory(psl, workDir);
    
    /* Set icon location - use the exe's embedded icon (index 0) */
    psl->lpVtbl->SetIconLocation(psl, targetPath, 0);
    
    /* Set window state to normal (not minimized, not maximized) */
    psl->lpVtbl->SetShowCmd(psl, SW_SHOWNORMAL);
    
    /* Set empty arguments (no command line parameters) */
    psl->lpVtbl->SetArguments(psl, "");
    
    /* Save the shortcut file */
    hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void**)&ppf);
    if (SUCCEEDED(hr)) {
        MultiByteToWideChar(CP_ACP, 0, shortcutPath, -1, wsz, MAX_PATH);
        hr = ppf->lpVtbl->Save(ppf, wsz, TRUE);
        ppf->lpVtbl->Release(ppf);
    }
    
    psl->lpVtbl->Release(psl);
    return SUCCEEDED(hr);
}

/* Perform installation */
BOOL DoInstall(HWND hwnd) {
    char installPath[MAX_PATH];
    char dstPath[MAX_PATH];
    char configPath[MAX_PATH];
    char relayAddr[128];
    char desktopPath[MAX_PATH];
    char startMenuPath[MAX_PATH];
    char shortcutPath[MAX_PATH];
    BOOL createDesktop, createStartMenu;
    FILE* fp;
    
    /* Get values from controls */
    GetWindowTextA(g_hPathEdit, installPath, sizeof(installPath));
    GetWindowTextA(g_hServerIdEdit, relayAddr, sizeof(relayAddr));
    createDesktop = (SendMessage(g_hDesktopChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    createStartMenu = (SendMessage(g_hStartMenuChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    /* Validate Relay Address */
    if (relayAddr[0] == '\0') {
        MessageBoxA(hwnd, "Please enter the relay address.\n\n"
                   "Use domain:port (e.g., relay.example.com:5000)\n"
                   "or IP:port (e.g., 192.168.1.1:5000)",
                   "Relay Address Required", MB_ICONWARNING);
        SetFocus(g_hServerIdEdit);
        return FALSE;
    }
    
    g_bInstalling = TRUE;
    EnableWindow(g_hInstallBtn, FALSE);
    
    /* Step 1: Create install directory */
    UpdateStatus("Creating installation directory...");
    SetProgress(10);
    
    if (!CreateDirectoryRecursive(installPath)) {
        MessageBoxA(hwnd, "Failed to create installation directory!\n\n"
                   "Try running installer as Administrator.",
                   "Installation Error", MB_ICONERROR);
        g_bInstalling = FALSE;
        EnableWindow(g_hInstallBtn, TRUE);
        return FALSE;
    }
    
    /* Step 2: Extract embedded RemoteDesk2K.exe */
    UpdateStatus("Extracting RemoteDesk2K.exe...");
    SetProgress(30);
    
    _snprintf(dstPath, sizeof(dstPath), "%s\\RemoteDesk2K.exe", installPath);
    
    if (!ExtractEmbeddedExe(dstPath)) {
        MessageBoxA(hwnd, "Failed to extract RemoteDesk2K.exe!\n\n"
            "The installer may be corrupted.\n"
            "Please download a fresh copy.",
            "Installation Error", MB_ICONERROR);
        g_bInstalling = FALSE;
        EnableWindow(g_hInstallBtn, TRUE);
        return FALSE;
    }
    
    /* Step 3: Create config file with Server ID */
    UpdateStatus("Creating configuration...");
    SetProgress(50);
    
    _snprintf(configPath, sizeof(configPath), "%s\\client_config.ini", installPath);
    fp = fopen(configPath, "w");
    if (fp) {
        fprintf(fp, "[Client]\r\n");
        fprintf(fp, "ServerID=%s\r\n", relayAddr);
        fprintf(fp, "LastPartnerID=\r\n");
        fprintf(fp, "LastDirectPartnerID=\r\n");
        fclose(fp);
    }
    
    /* Step 4: Create Desktop shortcut */
    if (createDesktop) {
        UpdateStatus("Creating Desktop shortcut...");
        SetProgress(70);
        
        if (SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktopPath) == S_OK) {
            _snprintf(shortcutPath, sizeof(shortcutPath), "%s\\RemoteDesk2K.lnk", desktopPath);
            CreateShortcut(dstPath, shortcutPath, "RemoteDesk2K - Remote Desktop");
        }
    }
    
    /* Step 5: Create Start Menu shortcut */
    if (createStartMenu) {
        UpdateStatus("Creating Start Menu shortcut...");
        SetProgress(85);
        
        if (SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, startMenuPath) == S_OK) {
            /* Create program folder */
            _snprintf(shortcutPath, sizeof(shortcutPath), "%s\\RemoteDesk2K", startMenuPath);
            CreateDirectoryA(shortcutPath, NULL);
            
            _snprintf(shortcutPath, sizeof(shortcutPath), "%s\\RemoteDesk2K\\RemoteDesk2K.lnk", startMenuPath);
            CreateShortcut(dstPath, shortcutPath, "RemoteDesk2K - Remote Desktop");
        }
    }
    
    /* Done! */
    SetProgress(100);
    UpdateStatus("Installation completed successfully!");
    
    g_bInstalling = FALSE;
    SetWindowTextA(g_hInstallBtn, "Finish");
    EnableWindow(g_hInstallBtn, TRUE);
    
    MessageBoxA(hwnd, "RemoteDesk2K has been installed successfully!\n\n"
               "You can now launch it from the Desktop or Start Menu.",
               "Installation Complete", MB_ICONINFORMATION);
    
    return TRUE;
}

/* Main window procedure */
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateControls(hwnd);
            return 0;
        
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc, rcHeader;
            
            GetClientRect(hwnd, &rc);
            
            /* Draw header background */
            rcHeader.left = 0;
            rcHeader.top = 0;
            rcHeader.right = rc.right;
            rcHeader.bottom = 70;
            FillRect(hdc, &rcHeader, g_hBrushHeader);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_CTLCOLORSTATIC:
        {
            HDC hdcStatic = (HDC)wParam;
            HWND hwndStatic = (HWND)lParam;
            int id = GetDlgCtrlID(hwndStatic);
            
            /* Header text - white on blue */
            if (id == IDC_HEADER_STATIC || id == IDC_INFO_STATIC) {
                SetTextColor(hdcStatic, RGB(255, 255, 255));
                SetBkMode(hdcStatic, TRANSPARENT);
                return (LRESULT)g_hBrushHeader;
            }
            
            /* Other static controls - default */
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_INSTALL_BTN:
                    if (g_bInstalling) {
                        return 0;
                    }
                    /* Check if already installed (Finish button) */
                    {
                        char btnText[32];
                        GetWindowTextA(g_hInstallBtn, btnText, sizeof(btnText));
                        if (strcmp(btnText, "Finish") == 0) {
                            PostMessage(hwnd, WM_CLOSE, 0, 0);
                            return 0;
                        }
                    }
                    DoInstall(hwnd);
                    return 0;
                
                case IDC_CANCEL_BTN:
                    if (g_bInstalling) {
                        if (MessageBoxA(hwnd, "Installation is in progress.\n\nDo you want to cancel?",
                                       "Cancel Installation", MB_YESNO | MB_ICONQUESTION) == IDNO) {
                            return 0;
                        }
                    }
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                
                case IDC_BROWSE_BTN:
                    BrowseForFolder(hwnd);
                    return 0;
            }
            break;
        
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
