#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Same sudo/DOCKER_CONFIG handling as oneclick_up.sh
if ! docker ps >/dev/null 2>&1; then
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "[agent_stack_runtime][ERR] docker socket requires root on this machine." >&2
    echo "[agent_stack_runtime][ERR] Please run: sudo ./oneclick_down.sh" >&2
    exit 1
  fi
fi

if ! docker compose version >/dev/null 2>&1; then
  if [[ -n "${SUDO_USER:-}" ]]; then
    uhome="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6 || true)"
    if [[ -z "$uhome" ]]; then
      uhome="/home/$SUDO_USER"
    fi
    export DOCKER_CONFIG="${DOCKER_CONFIG:-$uhome/.docker}"
  fi
fi

docker compose version >/dev/null 2>&1 || { echo "[agent_stack_runtime][ERR] docker compose not available" >&2; exit 1; }
compose_args=(-f docker-compose.yml)
if [[ -f docker-compose.local.yml ]]; then
  compose_args+=(-f docker-compose.local.yml)
elif [[ -f _build_ctx/docker-compose.local.yml ]]; then
  compose_args+=(-f _build_ctx/docker-compose.local.yml)
fi
rm_vol=0
for a in "${@:-}"; do
  case "$a" in
    -v|--volumes)
      rm_vol=1
      ;;
  esac
done

if [[ "${AGENT_STACK_RM_VOLUMES:-0}" == "1" ]]; then
  rm_vol=1
fi

if [[ "$rm_vol" -eq 1 ]]; then
  docker compose "${compose_args[@]}" down -v --remove-orphans
else
  docker compose "${compose_args[@]}" down
fi
