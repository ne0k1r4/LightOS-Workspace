#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEST="${XDG_BIN_HOME:-$HOME/.local/bin}"
mkdir -p "$DEST"
install -m755 "$ROOT/scripts/lightos-workspace" "$DEST/lightos-workspace"
printf 'Installed LightOS Workspace to %s\n' "$DEST/lightos-workspace"
