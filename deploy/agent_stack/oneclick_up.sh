#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "[agent_stack][ERR] missing command: $1" >&2; exit 1; }
}

need_cmd docker

mkdir -p config

if [[ ! -f config/strem_agent_server.env ]]; then
  cp -v config/strem_agent_server.env.example config/strem_agent_server.env
  echo "[agent_stack] created config/strem_agent_server.env (edit token/ports if needed)"
fi
if [[ ! -f config/agent_link_service.env ]]; then
  cp -v config/agent_link_service.env.example config/agent_link_service.env
  echo "[agent_stack] created config/agent_link_service.env (edit token/sk if needed)"
fi

mode="${AGENT_STACK_MODE:-build}"   # build | pull
compose_file="docker-compose.yml"
if [[ "$mode" == "pull" ]]; then
  compose_file="docker-compose.pull.yml"
fi

echo "[agent_stack] mode=$mode compose=$compose_file"

if [[ "$mode" == "pull" ]]; then
  docker compose -f "$compose_file" pull
  docker compose -f "$compose_file" up -d
else
  docker compose -f "$compose_file" up -d --build
fi

docker compose -f "$compose_file" ps

