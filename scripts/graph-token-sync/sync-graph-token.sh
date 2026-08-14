#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_CONFIG_FILE="$SCRIPT_DIR/.env"

if [[ ! -f "$ENV_CONFIG_FILE" ]]; then
    echo "sync-graph-token: missing config file $ENV_CONFIG_FILE (copy .env.example to .env and fill in GOA_IDENTITY)" >&2
    exit 1
fi
# shellcheck source=/dev/null
source "$ENV_CONFIG_FILE"

if [[ -z "${GOA_IDENTITY:-}" ]]; then
    echo "sync-graph-token: GOA_IDENTITY not set in $ENV_CONFIG_FILE" >&2
    exit 1
fi

if [[ -z "${OUTPUT_ENV_FILE:-}" ]]; then
    echo "sync-graph-token: OUTPUT_ENV_FILE not set in $ENV_CONFIG_FILE" >&2
    exit 1
fi

payload="$(secret-tool lookup xdg:schema org.gnome.OnlineAccounts goa-identity "$GOA_IDENTITY")"
if [[ -z "$payload" ]]; then
    echo "sync-graph-token: no secret found for goa-identity $GOA_IDENTITY" >&2
    exit 1
fi

refresh_token="$(GOA_PAYLOAD="$payload" python3 - <<'PYEOF'
import os
import sys
import gi
gi.require_version('GLib', '2.0')
from gi.repository import GLib

payload = os.environ['GOA_PAYLOAD']
try:
    variant = GLib.Variant.parse(None, payload, None, None)
except GLib.Error as exc:
    print(f"sync-graph-token: failed to parse keyring payload: {exc}", file=sys.stderr)
    sys.exit(1)

token = variant.unpack().get('refresh_token', '')
if not token:
    print("sync-graph-token: refresh_token not present in keyring payload", file=sys.stderr)
    sys.exit(1)

sys.stdout.write(token)
PYEOF
)"

umask 077
printf 'GRAPH_REFRESH_TOKEN="%s"\n' "$refresh_token" > "$OUTPUT_ENV_FILE"
chmod 600 "$OUTPUT_ENV_FILE"

export GRAPH_REFRESH_TOKEN="$refresh_token"
systemctl --user import-environment GRAPH_REFRESH_TOKEN
