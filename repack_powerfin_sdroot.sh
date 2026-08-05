#!/usr/bin/env bash
set -euo pipefail

# Compatibility entry point. PowerFin now stores normal and recovery boot
# configurations in one NOR FIT, so a normal-only image must not be produced.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec "${ROOT_DIR}/repack_powerfin_ramboot.sh" "$@"
