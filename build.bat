@echo off
REM ============================================================
REM RemoteDesk2K Build Script
REM Remote Desktop Application for Windows 2000
REM Uses DDK compiler (Windows 2000 compatible)
REM ============================================================

setlocal

REM Path to Windows 2000 SDK
set SDK_PATH=C:\Users\win2000\Downloads\WinDDK-1ed3987db6e72d1fb9c6298fddf0c080b8f591e1\2000 sdk

REM Path to Windows 2000 DDK (for compiler, CRT headers and libs)
set DDK_PATH=C:\Users\win2000\Downloads\WinDDK-1ed3987db6e72d1fb9c6298fddf0c080b8f591e1\3790.1830

REM DDK compiler and linker paths
set CL_PATH=%DDK_PATH%\bin\x86\cl.exe
set LINK_PATH=%DDK_PATH%\bin\x86\link.exe

REM Check if SDK exists
if not exist "%SDK_PATH%\Include" (
    echo ERROR: Windows 2000 SDK not found at %SDK_PATH%
    goto :error
)

REM Check if DDK compiler exists
if not exist "%CL_PATH%" (
    echo ERROR: DDK compiler not found at %CL_PATH%
    goto :error
)

echo ============================================================
echo Building RemoteDesk2K - Remote Desktop for Windows 2000
echo Using DDK compiler: %CL_PATH%
echo ============================================================
echo.

REM Compile all source files
REM DDK compiler doesn't have /GS flag (older compiler)
REM NOTE: relay_client.c is for CLIENT connecting TO relay server
REM       relay.c is for RELAY SERVER only (built by build_relay.bat)
echo Compiling source files...
"%CL_PATH%" /nologo /O2 /W3 /D_WIN32_WINNT=0x0500 /DWINVER=0x0500 /D_WIN32_IE=0x0500 ^
   /I"%DDK_PATH%\inc\crt" /I"%SDK_PATH%\Include" /I"%DDK_PATH%\inc\w2k" ^
   /c screen.c network.c input.c remotedesk2k.c nogs.c server_config_tab.c clipboard.c filetransfer.c progress.c crypto.c relay_client.c
if errorlevel 1 goto :error

REM Link all objects
echo Linking RemoteDesk2K.exe...
"%LINK_PATH%" /nologo /subsystem:windows ^
     /LIBPATH:"%SDK_PATH%\Lib" /LIBPATH:"%DDK_PATH%\lib\crt\i386" /LIBPATH:"%DDK_PATH%\lib\w2k\i386" ^
     screen.obj network.obj input.obj remotedesk2k.obj nogs.obj server_config_tab.obj clipboard.obj filetransfer.obj progress.obj crypto.obj relay_client.obj ^
     kernel32.lib user32.lib gdi32.lib ws2_32.lib comctl32.lib ^
     comdlg32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib ^
     /out:RemoteDesk2K.exe
if errorlevel 1 goto :error

echo Linking relay.exe (GUI relay server)...
