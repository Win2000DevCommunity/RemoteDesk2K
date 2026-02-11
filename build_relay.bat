@echo off
REM Build script for relay.exe (RemoteDesk2K) - Windows 2000 DDK/SDK compatible
REM Usage: build_relay.bat

setlocal
set DDK_ROOT=%~dp0..
set CRT_INC=%DDK_ROOT%\3790.1830\inc\crt
set W2K_INC=%DDK_ROOT%\3790.1830\inc\w2k
set SDK_INC=%DDK_ROOT%\2000 sdk\Include
set CRT_LIB=%DDK_ROOT%\3790.1830\lib\crt\i386
set SDK_LIB=%DDK_ROOT%\2000 sdk\Lib

REM Compile all sources with dynamic CRT (/MD) and strict w2k headers
cl.exe /nologo /W3 /Zi /MD /DWIN32 /D_WINDOWS /D_UNICODE /DUNICODE ^
  /I"%W2K_INC%" /I"%CRT_INC%" /I"%SDK_INC%" ^
  /c relay.c relay_gui.c relay_gui_main.c network.c screen.c security_cookie_stub.c crypto.c || goto :error

REM Link with MSVCRT and BufferOverflowU for security cookie stub
link.exe /nologo /subsystem:windows ^
  relay.obj relay_gui.obj relay_gui_main.obj network.obj screen.obj security_cookie_stub.obj crypto.obj ^
  /out:relay.exe ^
  ws2_32.lib gdi32.lib user32.lib shell32.lib comctl32.lib advapi32.lib ^
  "%CRT_LIB%\msvcrt.lib" BufferOverflowU.lib || goto :error

echo Build succeeded: relay.exe created.
goto :eof

:error
echo Build failed.
exit /b 1
