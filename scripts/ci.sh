#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"${ROOT_DIR}/scripts/check.sh"
"${ROOT_DIR}/scripts/security_audit.sh"
SKIP_BUILD=true "${ROOT_DIR}/scripts/e2e.sh"
SKIP_BUILD=true "${ROOT_DIR}/scripts/load_check.sh"

echo "ci ok"
