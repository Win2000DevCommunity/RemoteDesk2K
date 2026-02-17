@echo off
REM ============================================================================
REM Build script for relay_cmd.exe - Console Relay Server (Windows 2000+)
REM RemoteDesk2K Project (C) 2026
REM ============================================================================
REM Usage: build.bat [clean]
REM ============================================================================

setlocal

REM Change to script directory
cd /d "%~dp0"

REM Handle clean command
if "%1"=="clean" (
    echo Cleaning build artifacts...
    del /Q *.obj 2>nul
    del /Q *.pdb 2>nul
    del /Q *.ilk 2>nul
    del /Q relay_cmd.exe 2>nul
    echo Done.
    goto :eof
)

REM ============================================================================
REM Path configuration
REM ============================================================================
set DDK_ROOT=%~dp0..\..\..
set COMMON=%~dp0..\..\common
set RELAY=%~dp0..
set CRT_INC=%DDK_ROOT%\3790.1830\inc\crt
set W2K_INC=%DDK_ROOT%\3790.1830\inc\w2k
set SDK_INC=%DDK_ROOT%\2000 sdk\Include
set CRT_LIB=%DDK_ROOT%\3790.1830\lib\crt\i386
set SDK_LIB=%DDK_ROOT%\2000 sdk\Lib

echo ============================================================================
echo Building relay_cmd.exe - Console Relay Server
echo ============================================================================
echo.

REM ============================================================================
REM Compile all source files
REM ============================================================================
echo Compiling source files...

cl.exe /nologo /W3 /Zi /MD /DWIN32 /D_CONSOLE ^
    /I"%COMMON%" /I"%W2K_INC%" /I"%CRT_INC%" /I"%SDK_INC%" ^
    /c relay_cmd.c "%RELAY%\relay.c" "%COMMON%\crypto.c" "%RELAY%\security_cookie_stub.c"
if errorlevel 1 goto :error

echo.

REM ============================================================================
REM Link executable
REM ============================================================================
echo Linking relay_cmd.exe...

link.exe /nologo /subsystem:console ^
    relay_cmd.obj relay.obj crypto.obj security_cookie_stub.obj ^
    /out:relay_cmd.exe ^
    ws2_32.lib user32.lib kernel32.lib advapi32.lib ^
    "%CRT_LIB%\msvcrt.lib" BufferOverflowU.lib
if errorlevel 1 goto :error

echo.
echo ============================================================================
echo Build successful: relay_cmd.exe
echo ============================================================================
echo.

REM Show usage
echo Usage:
echo   relay_cmd.exe              - Start on default port 5000
echo   relay_cmd.exe -p 5900      - Start on port 5900
echo   relay_cmd.exe -h           - Show help
echo.

goto :eof

:error
echo.
echo ============================================================================
echo BUILD FAILED
echo ============================================================================
exit /b 1
