# RemoteDesk2K Makefile for Windows 2000 DDK
# Build with: nmake -f Makefile

# DDK/SDK paths
DDK_PATH = C:\Users\win2000\Downloads\WinDDK-1ed3987db6e72d1fb9c6298fddf0c080b8f591e1
W2K_INC = $(DDK_PATH)\3790.1830\inc\w2k
DDK_INC = $(DDK_PATH)\3790.1830\inc\ddk\w2k
CRT_INC = $(DDK_PATH)\3790.1830\inc\crt
SDK_INC = $(DDK_PATH)\2000 sdk\Include
SDK_LIB = $(DDK_PATH)\2000 sdk\Lib
W2K_LIB = $(DDK_PATH)\3790.1830\lib\w2k\i386
CRT_LIB = $(DDK_PATH)\3790.1830\lib\crt\i386

# Compiler and linker
CC = cl
LINK = link

# Compiler flags - use W2K DDK headers first, then CRT, then SDK
# /GS- disables buffer security checks (requires newer CRT)
CFLAGS = /nologo /W3 /O2 /GS- /DWIN32 /D_WINDOWS /DNDEBUG /I"$(W2K_INC)" /I"$(CRT_INC)" /I"$(DDK_INC)" /I"$(SDK_INC)"

# Linker flags - include CRT lib path, add bufferoverflow lib for security stubs
LDFLAGS = /nologo /subsystem:windows /LIBPATH:"$(SDK_LIB)" /LIBPATH:"$(W2K_LIB)" /LIBPATH:"$(CRT_LIB)"

# Libraries - BufferOverflowU.lib provides __security_check_cookie stub
# ole32.lib and oleaut32.lib needed for COM-based Explorer folder detection
LIBS = kernel32.lib user32.lib gdi32.lib ws2_32.lib comctl32.lib advapi32.lib shell32.lib comdlg32.lib ole32.lib oleaut32.lib BufferOverflowU.lib

# Object files
OBJS = remotedesk2k.obj screen.obj network.obj clipboard.obj filetransfer.obj progress.obj input.obj crypto.obj

# Main target
all: RemoteDesk2K.exe

# Executable
RemoteDesk2K.exe: $(OBJS)
	$(LINK) $(LDFLAGS) /out:$@ $(OBJS) $(LIBS)

# Compile rules
remotedesk2k.obj: remotedesk2k.c common.h screen.h network.h clipboard.h filetransfer.h progress.h crypto.h
	$(CC) $(CFLAGS) /c remotedesk2k.c

screen.obj: screen.c screen.h common.h
	$(CC) $(CFLAGS) /c screen.c

network.obj: network.c network.h common.h crypto.h
	$(CC) $(CFLAGS) /c network.c

clipboard.obj: clipboard.c clipboard.h common.h filetransfer.h
	$(CC) $(CFLAGS) /c clipboard.c

filetransfer.obj: filetransfer.c filetransfer.h common.h network.h progress.h
	$(CC) $(CFLAGS) /c filetransfer.c

progress.obj: progress.c progress.h
	$(CC) $(CFLAGS) /c progress.c

input.obj: input.c input.h common.h
	$(CC) $(CFLAGS) /c input.c

crypto.obj: crypto.c crypto.h
	$(CC) $(CFLAGS) /c crypto.c

# Clean
clean:
	-del *.obj
	-del *.exe
	-del *.pdb
	-del *.ilk

# Rebuild
rebuild: clean all

.PHONY: all clean rebuild
