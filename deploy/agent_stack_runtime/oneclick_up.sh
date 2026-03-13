#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "[agent_stack_runtime][ERR] missing command: $1" >&2; exit 1; }
}

need_cmd docker

mkdir -p config

if [[ ! -f config/strem_agent_server.env ]]; then
  cp -v config/strem_agent_server.env.example config/strem_agent_server.env
  echo "[agent_stack_runtime] created config/strem_agent_server.env"
fi
if [[ ! -f config/agent_link_service.env ]]; then
  cp -v config/agent_link_service.env.example config/agent_link_service.env
  echo "[agent_stack_runtime] created config/agent_link_service.env"
fi

allow_defaults=0
for a in "${@:-}"; do
  case "$a" in
    --allow-defaults)
      allow_defaults=1
      ;;
  esac
done

if [[ "$allow_defaults" -ne 1 ]]; then
  if grep -qE '^AGENT_LINK_TOKEN=your-static-token\b' config/agent_link_service.env; then
    echo "[agent_stack_runtime][ERR] please edit config/agent_link_service.env: AGENT_LINK_TOKEN is still 'your-static-token'" >&2
    exit 2
  fi
  if grep -qE '^AGENT_LINK_SK=change-me-sk\b' config/agent_link_service.env; then
    echo "[agent_stack_runtime][ERR] please edit config/agent_link_service.env: AGENT_LINK_SK is still 'change-me-sk'" >&2
    exit 2
  fi
  if grep -qE '^AGENT_TOKEN=your-static-token\b' config/strem_agent_server.env; then
    echo "[agent_stack_runtime][ERR] please edit config/strem_agent_server.env: AGENT_TOKEN is still 'your-static-token'" >&2
    exit 2
  fi

  # token must match between services
  agent_link_token="$(grep -E '^AGENT_LINK_TOKEN=' config/agent_link_service.env | tail -n 1 | cut -d= -f2- || true)"
  agent_token="$(grep -E '^AGENT_TOKEN=' config/strem_agent_server.env | tail -n 1 | cut -d= -f2- || true)"
  if [[ -n "$agent_link_token" && -n "$agent_token" && "$agent_link_token" != "$agent_token" ]]; then
    echo "[agent_stack_runtime][ERR] token mismatch: AGENT_LINK_TOKEN != AGENT_TOKEN" >&2
    echo "[agent_stack_runtime][ERR] config/agent_link_service.env: AGENT_LINK_TOKEN=$agent_link_token" >&2
    echo "[agent_stack_runtime][ERR] config/strem_agent_server.env: AGENT_TOKEN=$agent_token" >&2
    exit 2
  fi
fi

# Ensure ML_IMAGE exists in config (so pair/up can work without extra steps).
if ! grep -qE '^ML_IMAGE=' config/agent_link_service.env; then
  echo 'ML_IMAGE=ghcr.io/962125862/myworkspace/ml-worker:latest' >>config/agent_link_service.env
  echo "[agent_stack_runtime] appended ML_IMAGE to config/agent_link_service.env"
fi

# If the current user cannot access the docker socket, require sudo.
if ! docker ps >/dev/null 2>&1; then
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "[agent_stack_runtime][ERR] docker socket requires root on this machine." >&2
    echo "[agent_stack_runtime][ERR] Please run: sudo ./oneclick_up.sh" >&2
    exit 1
  fi
fi

# When running under sudo, docker plugins are searched under /root/.docker by default.
# If compose isn't found, point DOCKER_CONFIG to the original user's ~/.docker where we installed the plugin.
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

fresh=0
pull=1
for a in "${@:-}"; do
  case "$a" in
    --fresh)
      fresh=1
      ;;
    --no-pull)
      pull=0
      ;;
  esac
done

if [[ "${AGENT_STACK_FRESH:-0}" == "1" ]]; then
  fresh=1
fi
if [[ "${AGENT_STACK_NO_PULL:-0}" == "1" ]]; then
  pull=0
fi

if [[ "$fresh" -eq 1 ]]; then
  echo "[agent_stack_runtime] fresh mode: docker compose down -v"
  docker compose "${compose_args[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
fi

if [[ "$pull" -eq 1 ]]; then
  echo "[agent_stack_runtime] pulling images..."
  docker compose "${compose_args[@]}" pull
else
  echo "[agent_stack_runtime] skip pulling images (--no-pull)"
fi

docker compose "${compose_args[@]}" up -d
docker compose "${compose_args[@]}" ps
