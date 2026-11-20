#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

tools/build-fixtures.sh
if [[ -d generated/android17-init-root ]]; then
    tools/prepare-property-reader.sh
fi
python3 tools/generate-android-aot.py generated/android17-init-root/system/lib64/libcrypto.so \
    generated/AndroidBootAotBlocks.hpp
xcodegen generate --spec project.yml
