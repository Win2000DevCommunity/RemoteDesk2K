# RemoteDesk2K Linux Relay Server Makefile
#
# Usage:
#   make          - Build release version
#   make debug    - Build debug version
#   make clean    - Remove all build artifacts
#   make install  - Install to /usr/local/bin

# Compiler and flags
CC = gcc
CFLAGS_COMMON = -Wall -Wextra -Werror -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2 -DNDEBUG
CFLAGS_DEBUG = $(CFLAGS_COMMON) -g -O0 -DDEBUG
LDFLAGS = -lpthread

# Target
TARGET = relay_server
TARGET_DEBUG = relay_server_debug

# Source files
SRCS = relay_main.c relay.c crypto.c
OBJS = $(SRCS:.c=.o)
OBJS_DEBUG = $(SRCS:.c=.debug.o)

# Installation paths
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

# Default target - release build
.PHONY: all
all: release

# Release build
.PHONY: release
release: CFLAGS = $(CFLAGS_RELEASE)
release: $(TARGET)
	@echo ""
	@echo "============================================"
	@echo "  Build complete: $(TARGET)"
	@echo "============================================"
	@echo ""
	@echo "Run with: ./$(TARGET)"
	@echo "  or:     ./$(TARGET) --help"
	@echo ""

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Debug build
.PHONY: debug
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: $(TARGET_DEBUG)
	@echo ""
	@echo "============================================"
	@echo "  Debug build complete: $(TARGET_DEBUG)"
	@echo "============================================"
	@echo ""

$(TARGET_DEBUG): $(OBJS_DEBUG)
	$(CC) $(OBJS_DEBUG) -o $@ $(LDFLAGS)

# Pattern rules
%.o: %.c
	$(CC) $(CFLAGS_RELEASE) -c $< -o $@

%.debug.o: %.c
	$(CC) $(CFLAGS_DEBUG) -c $< -o $@

# Clean
.PHONY: clean
clean:
	rm -f $(TARGET) $(TARGET_DEBUG) *.o *.debug.o
	@echo "Cleaned build artifacts"

# Install (requires root)
.PHONY: install
install: $(TARGET)
	@echo "Installing to $(BINDIR)..."
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/remotedesk2k-relay
	@echo "Installed as: $(BINDIR)/remotedesk2k-relay"

# Uninstall
.PHONY: uninstall
uninstall:
	rm -f $(BINDIR)/remotedesk2k-relay
	@echo "Uninstalled"

# Dependencies
relay_main.o: relay_main.c common.h crypto.h
relay.o: relay.c common.h crypto.h
crypto.o: crypto.c common.h crypto.h

relay_main.debug.o: relay_main.c common.h crypto.h
relay.debug.o: relay.c common.h crypto.h
crypto.debug.o: crypto.c common.h crypto.h

# Help
.PHONY: help
help:
	@echo ""
	@echo "RemoteDesk2K Linux Relay Server - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build release version"
	@echo "  make debug    - Build debug version"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make install  - Install to $(BINDIR)"
	@echo "  make uninstall- Remove from $(BINDIR)"
	@echo "  make help     - Show this help"
	@echo ""
	@echo "Output:"
	@echo "  Release: ./relay_server"
	@echo "  Debug:   ./relay_server_debug"
	@echo ""
