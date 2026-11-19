#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

tools/build-fixtures.sh
if [[ -d generated/android17-init-root ]]; then
    tools/prepare-property-reader.sh
fi
xcodegen generate --spec project.yml
