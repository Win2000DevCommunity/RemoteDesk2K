@echo off
REM Build RemoteDesk2K Installer with embedded exe
REM Uses Windows 2000 DDK compiler (same approach as build.bat)
REM
REM IMPORTANT: Run build.bat first to create RemoteDesk2K.exe!

setlocal

REM Find DDK/SDK paths
set DDK_ROOT=%~dp0..\..
set DDK_BIN=%DDK_ROOT%\3790.1830\bin\x86
set CRT_INC=%DDK_ROOT%\3790.1830\inc\crt
set W2K_INC=%DDK_ROOT%\3790.1830\inc\w2k
set SDK_INC=%DDK_ROOT%\2000 sdk\Include
set CRT_LIB=%DDK_ROOT%\3790.1830\lib\crt\i386
set W2K_LIB=%DDK_ROOT%\3790.1830\lib\w2k\i386
set SDK_LIB=%DDK_ROOT%\2000 sdk\Lib

REM DDK compiler and linker full paths (MUST use these, not system PATH)
set CL_EXE=%DDK_BIN%\cl.exe
set LINK_EXE=%DDK_BIN%\link.exe
set RC_EXE=%DDK_BIN%\rc.exe

if not exist "%CL_EXE%" (
    echo ERROR: DDK compiler not found at %CL_EXE%
    exit /b 1
)

REM Check if RemoteDesk2K.exe exists
if not exist "..\client\RemoteDesk2K.exe" (
    echo ERROR: RemoteDesk2K.exe not found!
    echo Please run client\build.bat first to create RemoteDesk2K.exe
    exit /b 1
)

echo ============================================================
echo Building RemoteDesk2K Installer (with embedded exe)
echo Using DDK compiler: %CL_EXE%
echo ============================================================
echo.

REM Step 1: Compile resource file (embeds RemoteDesk2K.exe)
echo Compiling resources...
"%RC_EXE%" /I"%SDK_INC%" installer.rc
if errorlevel 1 goto error

REM Step 2: Compile C source with DDK compiler (static CRT for Win2K compatibility)
echo Compiling source...
"%CL_EXE%" /nologo /W3 /O2 /D_WIN32_WINNT=0x0500 /DWINVER=0x0500 ^
    /I"%CRT_INC%" /I"%SDK_INC%" /I"%W2K_INC%" ^
    /c installer.c ..\client\nogs.c
if errorlevel 1 goto error

REM Step 3: Link with DDK linker (static linking, Windows 2000 compatible)
echo Linking...
"%LINK_EXE%" /nologo /subsystem:windows ^
    /LIBPATH:"%SDK_LIB%" /LIBPATH:"%CRT_LIB%" /LIBPATH:"%W2K_LIB%" ^
    installer.obj nogs.obj installer.res ^
    /out:RD2K_Setup.exe ^
    kernel32.lib user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib uuid.lib
if errorlevel 1 goto error

echo.
echo ============================================================
echo Build successful: RD2K_Setup.exe
echo ============================================================
echo.
echo The installer now contains RemoteDesk2K.exe embedded inside!
echo.
echo To customize Server ID:
echo 1. Edit installer.c and set DEFAULT_SERVER_ID
echo 2. Rebuild with build_installer.bat
echo.
echo Distribution: Just give RD2K_Setup.exe to users (single file!)

REM Cleanup
del *.obj 2>nul
del *.res 2>nul
goto end

:error
echo.
echo BUILD FAILED!
del *.obj 2>nul
del *.res 2>nul
exit /b 1

:end
endlocal
