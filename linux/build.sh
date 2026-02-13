#!/bin/bash
# RemoteDesk2K Linux Relay Server - Build Script
# Run this on any Linux system with GCC installed

set -e

echo "=========================================="
echo " RemoteDesk2K Linux Relay Server Builder"
echo "=========================================="
echo ""

# Check for GCC
if ! command -v gcc &> /dev/null; then
    echo "GCC not found. Installing build tools..."
    if command -v apt &> /dev/null; then
        sudo apt update && sudo apt install -y build-essential
    elif command -v yum &> /dev/null; then
        sudo yum groupinstall -y "Development Tools"
    elif command -v dnf &> /dev/null; then
        sudo dnf groupinstall -y "Development Tools"
    else
        echo "ERROR: Cannot install GCC automatically."
        echo "Please install GCC manually and re-run this script."
        exit 1
    fi
fi

echo "Compiling..."

# Compile
gcc -Wall -Wextra -std=c99 -O2 \
    -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
    -DNDEBUG \
    relay_main.c relay.c crypto.c \
    -lpthread \
    -o relay_server

echo ""
echo "=========================================="
echo " Build successful!"
echo "=========================================="
echo ""
echo " Run with: ./relay_server --help"
echo "           ./relay_server -p 5000"
echo ""

# Make executable
chmod +x relay_server

# Show help
./relay_server --help
