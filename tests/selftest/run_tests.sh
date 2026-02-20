#!/bin/bash
#
# CasparCG Self-Test Runner
#
# This script automatically:
# 1. Starts CasparCG with the test configuration
# 2. Waits for it to be ready
# 3. Runs the tests
# 4. Shuts down CasparCG
#
# Usage:
#   ./run_tests.sh              # Run all tests
#   ./run_tests.sh --phase 3    # Run specific phase
#   ./run_tests.sh --test color # Run specific test
#   ./run_tests.sh --list       # List available tests
#   ./run_tests.sh --no-server  # Don't start CasparCG (use existing instance)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/shell"
CASPARCG_BINARY="$BUILD_DIR/casparcg"
CASPARCG_PID=""
START_SERVER=1
CASPARCG_PORT=5250

# Parse our own arguments before passing to Python
PYTHON_ARGS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --no-server)
            START_SERVER=0
            shift
            ;;
        --port)
            CASPARCG_PORT="$2"
            PYTHON_ARGS+=("$1" "$2")
            shift 2
            ;;
        *)
            PYTHON_ARGS+=("$1")
            shift
            ;;
    esac
done

cd "$SCRIPT_DIR"

# Check Python
if ! command -v python3 &> /dev/null; then
    echo "Error: python3 is required"
    exit 1
fi

# Check ffprobe (needed for video analysis)
if ! command -v ffprobe &> /dev/null; then
    echo "Warning: ffprobe not found - video analysis will be limited"
fi

# Create output directory
mkdir -p test_output

# Function to test clean shutdown (returns 0 if clean, 1 if had to force kill)
test_clean_shutdown() {
    local pid=$1
    local timeout_seconds=5

    echo ""
    echo "============================================================"
    echo "Testing clean shutdown..."
    echo "============================================================"

    # Send KILL command via AMCP
    echo "Sending KILL command via AMCP..."
    if ! python3 -c "
import socket
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect(('localhost', $CASPARCG_PORT))
    s.sendall(b'KILL\r\n')
    response = s.recv(1024).decode()
    s.close()
    exit(0 if '202' in response else 1)
except Exception as e:
    print(f'Failed to send KILL: {e}')
    exit(1)
" 2>/dev/null; then
        echo "  WARNING: Could not send KILL command, falling back to SIGTERM"
        kill "$pid" 2>/dev/null || true
    fi

    # Wait for clean exit
    echo "Waiting for clean shutdown (max ${timeout_seconds}s)..."
    local elapsed=0
    while [[ $elapsed -lt $timeout_seconds ]]; do
        if ! ps -p "$pid" > /dev/null 2>&1; then
            echo "  OK: CasparCG exited cleanly after ${elapsed}s"
            return 0
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done

    # Process still running - this is the bug we're testing for
    echo "  FAIL: CasparCG did not exit within ${timeout_seconds}s - shutdown is frozen!"
    echo "  Force killing process..."
    kill -9 "$pid" 2>/dev/null || true
    return 1
}

# Function to cleanup on exit (fallback if test_clean_shutdown wasn't called)
cleanup() {
    if [[ -n "$CASPARCG_PID" ]] && ps -p "$CASPARCG_PID" > /dev/null 2>&1; then
        echo ""
        echo "Stopping CasparCG (PID: $CASPARCG_PID)..."
        kill "$CASPARCG_PID" 2>/dev/null || true
        sleep 1
        if ps -p "$CASPARCG_PID" > /dev/null 2>&1; then
            kill -9 "$CASPARCG_PID" 2>/dev/null || true
        fi
    fi
}
trap cleanup EXIT

# Function to wait for CasparCG to be ready
wait_for_server() {
    local max_attempts=30
    local attempt=0
    echo "Waiting for CasparCG to be ready..."
    while [[ $attempt -lt $max_attempts ]]; do
        # Check if we can connect and send/receive an AMCP command
        # Use longer read timeout and read until we get a complete response
        if python3 -c "
import socket
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect(('localhost', $CASPARCG_PORT))
    s.sendall(b'VERSION\r\n')
    # Read response - AMCP responses end with double CRLF
    response = b''
    while True:
        chunk = s.recv(1024)
        if not chunk:
            break
        response += chunk
        if b'\r\n' in response:
            break
    s.close()
    response = response.decode()
    exit(0 if '201' in response else 1)
except Exception as e:
    exit(1)
" 2>/dev/null; then
            echo "CasparCG is ready!"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 0.5
    done
    echo "Error: CasparCG did not respond in time"
    return 1
}

# Start CasparCG if requested
if [[ $START_SERVER -eq 1 ]]; then
    # Check if binary exists
    if [[ ! -x "$CASPARCG_BINARY" ]]; then
        echo "Error: CasparCG binary not found at $CASPARCG_BINARY"
        echo "Please build CasparCG first: ./build_macos.sh"
        exit 1
    fi

    # Select appropriate config for platform
    if [[ "$(uname)" == "Darwin" ]]; then
        CONFIG_FILE="$SCRIPT_DIR/casparcg_test_macos.config"
    else
        CONFIG_FILE="$SCRIPT_DIR/casparcg_test.config"
    fi

    if [[ ! -f "$CONFIG_FILE" ]]; then
        echo "Error: Test config not found at $CONFIG_FILE"
        exit 1
    fi

    # Setup CasparCG directory
    echo "Setting up CasparCG test environment..."
    cp "$CONFIG_FILE" "$BUILD_DIR/casparcg.config"
    mkdir -p "$BUILD_DIR/media" "$BUILD_DIR/log" "$BUILD_DIR/data" "$BUILD_DIR/template"

    # Kill any existing CasparCG processes (by name, catches frozen instances)
    EXISTING_PIDS=$(pgrep -x casparcg 2>/dev/null || true)
    if [[ -n "$EXISTING_PIDS" ]]; then
        echo "Found existing CasparCG process(es): $EXISTING_PIDS"
        echo "Killing existing CasparCG instances..."
        pkill -x casparcg 2>/dev/null || true
        sleep 1
        # Force kill any that didn't stop gracefully
        pkill -9 -x casparcg 2>/dev/null || true
        sleep 0.5
    fi

    # Also check the port in case something else is using it
    if lsof -i ":$CASPARCG_PORT" -t > /dev/null 2>&1; then
        echo "Warning: Port $CASPARCG_PORT still in use, killing process..."
        lsof -i ":$CASPARCG_PORT" -t | xargs kill 2>/dev/null || true
        sleep 1
    fi

    # Set NDI runtime directory (macOS)
    if [[ "$(uname)" == "Darwin" ]]; then
        export NDI_RUNTIME_DIR_V6="/usr/local/lib"
    fi

    # Start CasparCG
    echo "Starting CasparCG..."
    cd "$BUILD_DIR"
    ./casparcg > "$SCRIPT_DIR/test_output/casparcg.log" 2>&1 &
    CASPARCG_PID=$!
    cd "$SCRIPT_DIR"

    echo "CasparCG started (PID: $CASPARCG_PID)"

    # Wait for server to be ready
    if ! wait_for_server; then
        echo "CasparCG log:"
        cat "$SCRIPT_DIR/test_output/casparcg.log"
        exit 1
    fi
fi

# Run tests
echo ""
echo "Running tests..."
python3 test_runner.py "${PYTHON_ARGS[@]}"
TEST_RESULT=$?

# Test clean shutdown if we started the server
SHUTDOWN_RESULT=0
if [[ $START_SERVER -eq 1 ]] && [[ -n "$CASPARCG_PID" ]]; then
    if ! test_clean_shutdown "$CASPARCG_PID"; then
        SHUTDOWN_RESULT=1
        echo ""
        echo "============================================================"
        echo "SHUTDOWN TEST FAILED - CasparCG froze during exit!"
        echo "============================================================"
    fi
    # Clear PID so cleanup trap doesn't try to kill again
    CASPARCG_PID=""
fi

# Report final result
if [[ $TEST_RESULT -ne 0 ]] || [[ $SHUTDOWN_RESULT -ne 0 ]]; then
    exit 1
fi
exit 0
