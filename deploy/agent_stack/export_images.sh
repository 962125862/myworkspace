#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

out="${1:-/tmp/agent_stack_images_latest.tar.gz}"

images=(
  "strem-agent-server:latest"
  "agent-link-service:latest"
)

echo "[agent_stack] exporting images to: $out"
docker save "${images[@]}" | gzip -1 >"$out"
ls -lh "$out"

cat <<EOF

Import on another machine:
  gzip -dc "$out" | docker load

Then run:
  cd /path/to/repo/deploy/agent_stack
  ./oneclick_up.sh

EOF

