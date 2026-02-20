#!/bin/bash
#
# CasparCG macOS Release Build Script
#
# Builds, packages, signs, and notarizes CasparCG for distribution.
# Requires .env file with SIGNING_IDENTITY and NOTARIZE_KEYCHAIN_PROFILE.
#
# Usage: ./tools/macos/build-release.sh
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/build.sh" --package --dmg --sign --notarize "$@"
