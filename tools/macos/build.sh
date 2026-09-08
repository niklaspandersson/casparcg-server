#!/bin/bash
# CasparCG macOS Build Script
# This script builds CasparCG for macOS with Vulkan backend

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SRC_DIR="${ROOT_DIR}/src"

# Parse arguments
CLEAN_BUILD=0
VERBOSE=0
ENABLE_HTML=ON
JOBS=$(sysctl -n hw.ncpu)
PACKAGE=0
PACKAGE_ARGS=""

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Build options:"
    echo "  --clean, -c         Clean build from scratch"
    echo "  --verbose, -v       Show verbose output"
    echo "  --jobs, -j N        Number of parallel build jobs (default: CPU count)"
    echo "  --with-html         Enable CEF/HTML module (default)"
    echo "  --no-html           Disable CEF/HTML module"
    echo ""
    echo "Packaging options (--package enables all by default, use --no-* to disable):"
    echo "  --package           Create .app bundle after build"
    echo "  --no-ndi            Exclude NDI library"
    echo "  --no-dmg            Skip DMG creation"
    echo "  --no-sign           Skip code signing"
    echo "  --no-notarize       Skip notarization"
    echo "  --identity \"...\"    Override signing identity from .env"
    echo "  --keychain-profile \"...\" Override keychain profile from .env"
    echo ""
    echo "Signing credentials are read from .env file (SIGNING_IDENTITY, NOTARIZE_KEYCHAIN_PROFILE)."
    echo ""
    echo "Examples:"
    echo "  $0                                    # Build only"
    echo "  $0 --clean                            # Clean build"
    echo "  $0 --package                          # Build + signed, notarized DMG"
    echo "  $0 --package --no-sign                # Build + unsigned app bundle + DMG"
    echo "  $0 --package --no-dmg --no-sign       # Build + unsigned app bundle only"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean|-c)
            CLEAN_BUILD=1
            shift
            ;;
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --with-html)
            ENABLE_HTML=ON
            shift
            ;;
        --no-html)
            ENABLE_HTML=OFF
            shift
            ;;
        --jobs|-j)
            JOBS="$2"
            shift 2
            ;;
        --package)
            PACKAGE=1
            shift
            ;;
        --no-ndi)
            PACKAGE_ARGS="$PACKAGE_ARGS --no-ndi"
            shift
            ;;
        --no-dmg)
            PACKAGE_ARGS="$PACKAGE_ARGS --no-dmg"
            shift
            ;;
        --no-sign)
            PACKAGE_ARGS="$PACKAGE_ARGS --no-sign"
            shift
            ;;
        --no-notarize)
            PACKAGE_ARGS="$PACKAGE_ARGS --no-notarize"
            shift
            ;;
        --identity)
            PACKAGE_ARGS="$PACKAGE_ARGS --identity \"$2\""
            shift 2
            ;;
        --keychain-profile)
            PACKAGE_ARGS="$PACKAGE_ARGS --keychain-profile \"$2\""
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

echo "=== CasparCG macOS Build ==="
echo "Build directory: ${BUILD_DIR}"
echo "Source directory: ${SRC_DIR}"
echo "Parallel jobs: ${JOBS}"
echo ""

# Clean build if requested
if [[ $CLEAN_BUILD -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure with CMake
echo "Configuring with CMake (HTML/CEF: ${ENABLE_HTML})..."
if [[ $VERBOSE -eq 1 ]]; then
    cmake "${SRC_DIR}" -DENABLE_HTML=${ENABLE_HTML}
else
    cmake "${SRC_DIR}" -DENABLE_HTML=${ENABLE_HTML} 2>&1 | grep -E "^--|Found|Error|Warning|===|CEF"
fi

# Build
echo ""
echo "Building with ${JOBS} parallel jobs..."
if [[ $VERBOSE -eq 1 ]]; then
    make -j${JOBS}
else
    make -j${JOBS} 2>&1 | grep -E "^\[|Error|error:|warning:"
fi

# Copy macOS run script to build directory
cp "${SRC_DIR}/shell/run_macos.sh" "${BUILD_DIR}/shell/"
chmod +x "${BUILD_DIR}/shell/run_macos.sh"

echo ""
echo "=== Build Complete ==="
echo "Binary: ${BUILD_DIR}/shell/casparcg"
echo "Run script: ${BUILD_DIR}/shell/run_macos.sh"

# Package if requested
if [[ $PACKAGE -eq 1 ]]; then
    echo ""
    echo "=== Creating App Bundle ==="
    eval "${SCRIPT_DIR}/package.sh $PACKAGE_ARGS"
fi
