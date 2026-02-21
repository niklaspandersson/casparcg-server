#!/bin/bash
#
# CasparCG macOS Run Script
#
# This script sets up the environment and runs CasparCG on macOS.
# It handles:
#   - NDI runtime library path
#   - Restart on exit code 5 (like the Linux version)
#
# Usage:
#   ./run_macos.sh              # Run CasparCG
#   ./run_macos.sh myconfig     # Run with custom config
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Set library/framework paths for CEF
export DYLD_FRAMEWORK_PATH="$SCRIPT_DIR/../Frameworks:$DYLD_FRAMEWORK_PATH"
export DYLD_LIBRARY_PATH="$SCRIPT_DIR/../Frameworks:$DYLD_LIBRARY_PATH"

# Set NDI runtime directory
# The NDI SDK installs the library to /usr/local/lib on macOS
export NDI_RUNTIME_DIR_V6="/usr/local/lib"

# Restart loop (exit code 5 means restart requested)
RET=5
while [ $RET -eq 5 ]; do
    ./casparcg "$@"
    RET=$?
done
