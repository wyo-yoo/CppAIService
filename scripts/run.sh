#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -f "$project_dir/.env" ]]; then
    set -a
    source "$project_dir/.env"
    set +a
fi
: "${CHAT_MYSQL_PASSWORD:?Configure .env first: python3 scripts/setup_database.py}"
cd "$project_dir/build-chat"
exec ./http_server -p "${1:-8080}"
