@echo off
REM Build relay.exe using Windows 7 DDK

REM Set environment for Windows 7 DDK
call "C:\Users\win2000\Downloads\WinDDK-1ed3987db6e72d1fb9c6298fddf0c080b8f591e1\7600.16385.1\SetEnv.bat"

REM Change to RemoteDesk2K directory
cd /d "C:\Users\win2000\Downloads\WinDDK-1ed3987db6e72d1fb9c6298fddf0c080b8f591e1\RemoteDesk2K"

REM Compile relay.c with Windows 7 DDK include and lib paths
cl relay.c relay_gui.c relay_gui_main.c /I "..\7600.16385.1\inc" /I "..\7600.16385.1\inc\api" /I "..\7600.16385.1\inc\crt" /link /LIBPATH:"..\7600.16385.1\lib\Crt\i386" /OUT:relay.exe /SUBSYSTEM:WINDOWS

echo Build complete. Check relay.exe for errors.
