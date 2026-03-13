#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

mode="${AGENT_STACK_MODE:-build}"   # build | pull
compose_file="docker-compose.yml"
if [[ "$mode" == "pull" ]]; then
  compose_file="docker-compose.pull.yml"
fi

docker compose -f "$compose_file" down

