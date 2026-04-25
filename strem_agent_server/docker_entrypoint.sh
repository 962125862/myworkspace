#!/usr/bin/env bash
set -euo pipefail

# strem_agent_server is configured via CLI. This entrypoint maps envs (optionally
# loaded from a mounted env file) to CLI flags to make docker usage simpler.

load_env_file_if_present() {
  local f="$1"
  [[ -n "$f" && -f "$f" ]] || return 0

  # Parse KEY=VALUE (or quoted) without eval.
  # Only set variables that are not already set in the environment so `docker -e`
  # can override the mounted file.
  while IFS= read -r line || [[ -n "$line" ]]; do
    # Strip leading/trailing whitespace.
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" ]] && continue
    [[ "$line" == \#* ]] && continue
    [[ "$line" != *"="* ]] && continue

    local k="${line%%=*}"
    local v="${line#*=}"
    k="${k%"${k##*[![:space:]]}"}"
    k="${k#"${k%%[![:space:]]*}"}"

    # basic key sanity
    [[ "$k" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue

    # Unquote common cases.
    if [[ "$v" =~ ^\".*\"$ ]]; then
      v="${v:1:${#v}-2}"
    elif [[ "$v" =~ ^\'.*\'$ ]]; then
      v="${v:1:${#v}-2}"
    fi

    # Skip if already set.
    if [[ -n "${!k:-}" ]]; then
      continue
    fi
    export "$k=$v"
  done <"$f"
}

ENV_FILE="${STREM_AGENT_SERVER_ENV_FILE:-/config/strem_agent_server.env}"
load_env_file_if_present "$ENV_FILE"

IN_HOST="${IN_HOST:-0.0.0.0}"
IN_PORT="${IN_PORT:-19000}"

VIDEO_BIND="${VIDEO_BIND:-0.0.0.0}"
VIDEO_PORT="${VIDEO_PORT:-31234}"

CTRL_BIND="${CTRL_BIND:-0.0.0.0}"
CTRL_PORT="${CTRL_PORT:-31235}"

WORKER_CTRL_IP="${WORKER_CTRL_IP:-127.0.0.1}"
WORKER_CTRL_PORT="${WORKER_CTRL_PORT:-30001}"

AGENT_TOKEN="${AGENT_TOKEN:-}"

args=(
  --in-host "$IN_HOST" --in-port "$IN_PORT"
  --video-bind "$VIDEO_BIND" --video-port "$VIDEO_PORT"
  --ctrl-bind "$CTRL_BIND" --ctrl-port "$CTRL_PORT"
  --worker-ctrl-ip "$WORKER_CTRL_IP" --worker-ctrl-port "$WORKER_CTRL_PORT"
)

if [[ -n "$AGENT_TOKEN" ]]; then
  args+=(--token "$AGENT_TOKEN")
fi

exec /app/strem_agent_server "${args[@]}" "$@"
