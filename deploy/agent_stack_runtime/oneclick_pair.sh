#!/usr/bin/env bash
set -euo pipefail

# Helper for first-time pairing (interactive PIN entry on Sunshine is still required).
#
# Usage:
#   ./oneclick_pair.sh <sunshine_ip> [stream_id] [pin] [worker_name]
#   ./oneclick_pair.sh <sunshine_ip> --stream-id N [--pin PIN] [--worker NAME] [--control-port PORT]
#
# Examples:
#   ./oneclick_pair.sh 192.168.11.50
#   ./oneclick_pair.sh 192.168.11.50 1 1234 worker_s1
#   ./oneclick_pair.sh 192.168.11.50 1234          # (pin shorthand, stream_id defaults to 1)

cd "$(dirname "$0")"

positional_args=()
use_local=0
stream_id=""
pin=""
worker=""
control_port=""
explicit_stream_id=0
explicit_worker=0
explicit_pin=0
explicit_control_port=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --local)
      use_local=1
      shift
      ;;
    --stream-id)
      if [[ $# -lt 2 ]]; then
        echo "[pair][ERR] --stream-id 需要一个参数，例如 --stream-id 1" >&2
        exit 2
      fi
      stream_id="${2:-}"
      explicit_stream_id=1
      shift 2
      ;;
    --stream-id=*)
      stream_id="${1#*=}"
      explicit_stream_id=1
      shift
      ;;
    --pin)
      if [[ $# -lt 2 ]]; then
        echo "[pair][ERR] --pin 需要一个 4 位数字，例如 --pin 1234" >&2
        exit 2
      fi
      pin="${2:-}"
      explicit_pin=1
      shift 2
      ;;
    --pin=*)
      pin="${1#*=}"
      explicit_pin=1
      shift
      ;;
    --worker)
      if [[ $# -lt 2 ]]; then
        echo "[pair][ERR] --worker 需要一个名称，例如 --worker worker_s1" >&2
        exit 2
      fi
      worker="${2:-}"
      explicit_worker=1
      shift 2
      ;;
    --worker=*)
      worker="${1#*=}"
      explicit_worker=1
      shift
      ;;
    --control-port)
      if [[ $# -lt 2 ]]; then
        echo "[pair][ERR] --control-port 需要一个端口，例如 --control-port 50100" >&2
        exit 2
      fi
      control_port="${2:-}"
      explicit_control_port=1
      shift 2
      ;;
    --control-port=*)
      control_port="${1#*=}"
      explicit_control_port=1
      shift
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        positional_args+=("$1")
        shift
      done
      ;;
    -h|--help)
      echo "usage: $0 <sunshine_ip> [stream_id] [pin] [worker_name]" >&2
      echo "   or: $0 <sunshine_ip> --stream-id N [--pin PIN] [--worker NAME] [--control-port PORT]" >&2
      exit 0
      ;;
    *)
      positional_args+=("$1")
      shift
      ;;
  esac
done
if [[ "${AGENT_STACK_USE_LOCAL:-0}" == "1" ]]; then
  use_local=1
fi

if [[ -n "$stream_id" ]]; then
  explicit_stream_id=1
fi
if [[ -n "$worker" ]]; then
  explicit_worker=1
fi
if [[ -n "$pin" ]]; then
  explicit_pin=1
fi
if [[ -n "$control_port" ]]; then
  explicit_control_port=1
fi

sunshine_ip="${positional_args[0]:-}"
arg2="${positional_args[1]:-}"
arg3="${positional_args[2]:-}"
arg4="${positional_args[3]:-}"

stream_id="${stream_id:-}"

# Positional fallback with a convenience shorthand:
# - If the 2nd arg is a 4-digit number, treat it as PIN (stream_id defaults to 1).
# - Otherwise treat the 2nd arg as stream_id, and 3rd as optional PIN.
if [[ -z "$stream_id" ]]; then
  if [[ -n "$arg2" && "$arg2" =~ ^[0-9]{4}$ && -z "$arg3" && -z "$pin" ]]; then
    pin="$arg2"
    stream_id="1"
  else
    if [[ -n "$arg2" ]]; then
      stream_id="$arg2"
      explicit_stream_id=1
    fi
    if [[ -n "$arg3" && -z "$pin" ]]; then
      pin="$arg3"
    fi
    if [[ -n "$arg4" && -z "$worker" ]]; then
      worker="$arg4"
      explicit_worker=1
    fi
  fi
fi

stream_id="${stream_id:-1}"
worker="${worker:-worker_s${stream_id}}"

if [[ -z "$sunshine_ip" ]]; then
  echo "usage: $0 <sunshine_ip> [stream_id] [pin] [worker_name]" >&2
  echo "   or: $0 <sunshine_ip> --stream-id N [--pin PIN] [--worker NAME] [--control-port PORT]" >&2
  exit 2
fi

need_docker_root=0
if ! docker ps >/dev/null 2>&1; then
  if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    need_docker_root=1
  fi
fi

dc() {
  if [[ "$need_docker_root" -eq 1 ]]; then
    sudo "$@"
  else
    "$@"
  fi
}

compose_args=(-f docker-compose.yml)
if [[ "$use_local" -eq 1 ]]; then
  if [[ -f docker-compose.local.yml ]]; then
    compose_args+=(-f docker-compose.local.yml)
  elif [[ -f _build_ctx/docker-compose.local.yml ]]; then
    compose_args+=(-f _build_ctx/docker-compose.local.yml)
  fi
fi

if [[ -n "$pin" && ! "$pin" =~ ^[0-9]{4}$ ]]; then
  echo "[pair][ERR] pin 必须是 4 位数字，例如 1234" >&2
  exit 2
fi

if [[ ! "$stream_id" =~ ^[0-9]+$ ]]; then
  echo "[pair][ERR] stream_id 必须是数字，例如 1" >&2
  exit 2
fi

if (( 10#$stream_id < 1 || 10#$stream_id > 20 )); then
  echo "[pair][ERR] stream_id 当前只支持 1..20（strem_agent_server 仍然有这个上限）" >&2
  echo "[pair][ERR] 你可以改成 1..20 内的值，或者先把 strem_agent_server 的 MAX_STREAMS 提升后再用更大 stream_id" >&2
  exit 2
fi

if [[ -n "$control_port" ]]; then
  if [[ ! "$control_port" =~ ^[0-9]+$ ]]; then
    echo "[pair][ERR] control_port 必须是数字，例如 50100" >&2
    exit 2
  fi
  if (( 10#$control_port < 1 || 10#$control_port > 65535 )); then
    echo "[pair][ERR] control_port 必须在 1..65535 之间" >&2
    exit 2
  fi
fi

# Enforce a stable convention:
# - default worker name is derived from stream_id: worker_s<id>
# - if user provides worker_s<id> but not stream_id, derive stream_id from worker name
# - if both are provided and mismatch, fail early to avoid confusing configs
if [[ "$explicit_worker" -eq 0 ]]; then
  worker="worker_s${stream_id}"
else
  if [[ "$explicit_stream_id" -eq 0 && "$worker" =~ ^worker_s([0-9]+)$ ]]; then
    stream_id="${BASH_REMATCH[1]}"
  fi
  if [[ "$explicit_stream_id" -eq 1 && "$worker" =~ ^worker_s([0-9]+)$ ]]; then
    wid="${BASH_REMATCH[1]}"
    if [[ "$wid" != "$stream_id" ]]; then
      echo "[pair][ERR] worker 名和 stream_id 不一致: worker=$worker stream_id=$stream_id" >&2
      echo "[pair][ERR] 约定为 worker_sN <-> stream_id=N，例如:" >&2
      echo "[pair][ERR]   ./oneclick_pair.sh $sunshine_ip $wid ${pin:-'(pin)'} $worker" >&2
      exit 2
    fi
  fi
fi

# Stable convention: stream 1 -> control 50001, stream 2 -> 50002, ...
if [[ -z "$control_port" ]]; then
  control_port=$((50000 + 10#${stream_id}))
fi

echo "[pair] worker=$worker stream_id=$stream_id control_port=$control_port sunshine_ip=$sunshine_ip pin=${pin:-'(auto)'}"

# Ensure ML_IMAGE exists in config and is loaded by the container.
ml_image="$(grep -E '^ML_IMAGE=' config/agent_link_service.env 2>/dev/null | tail -n 1 | cut -d= -f2- || true)"
if [[ -z "$ml_image" ]]; then
  ml_image="ghcr.io/962125862/myworkspace/ml-worker:latest"
  echo "ML_IMAGE=$ml_image" >>config/agent_link_service.env
  echo "[pair] appended ML_IMAGE=$ml_image to config/agent_link_service.env"
fi

# Force-recreate agent_link_service so updated env_file takes effect.
dc docker compose "${compose_args[@]}" up -d --no-deps --force-recreate agent_link_service >/dev/null

echo "[pair] pulling ml_worker image: $ml_image"
dc docker pull "$ml_image" >/dev/null || {
  echo "[pair][ERR] docker pull failed: $ml_image" >&2
  echo "[pair][ERR] 如果是 GHCR 私有镜像，请先执行: docker login ghcr.io" >&2
  exit 3
}

# Ensure worker exists (best-effort). If it already exists, we'll patch it below.
# Args: name host app image worker_bin tcp_host tcp_port stream_id control_bind control_port
dc docker exec -it agent_link_service bash -lc "/app/mlctl.sh add $worker $sunshine_ip Desktop '' '' 127.0.0.1 19000 $stream_id 127.0.0.1 $control_port" || true

# Patch existing worker config:
# - ensure IMAGE is correct
# - keep worker_sN and stream_id aligned
# - if CONTROL_PORT already exists, preserve it unless user explicitly passed --control-port
dc docker exec -it agent_link_service bash -lc "f=/app/workers/$worker.conf; \
  if [[ -f \"\$f\" ]]; then \
    sed -i \"s#^STREAM_ID=.*#STREAM_ID=\\\"$stream_id\\\"#\" \"\$f\" 2>/dev/null || true; \
    sed -i \"s#^TCP_HOST=.*#TCP_HOST=\\\"127.0.0.1\\\"#\" \"\$f\" 2>/dev/null || true; \
    sed -i \"s#^TCP_PORT=.*#TCP_PORT=\\\"19000\\\"#\" \"\$f\" 2>/dev/null || true; \
    sed -i \"s#^CONTROL_BIND=.*#CONTROL_BIND=\\\"127.0.0.1\\\"#\" \"\$f\" 2>/dev/null || true; \
    if [[ \"$explicit_control_port\" -eq 1 ]]; then \
      if grep -q '^CONTROL_PORT=' \"\$f\"; then \
        sed -i \"s#^CONTROL_PORT=.*#CONTROL_PORT=\\\"$control_port\\\"#\" \"\$f\"; \
      else \
        printf '\\nCONTROL_PORT=\\\"%s\\\"\\n' \"$control_port\" >>\"\$f\"; \
      fi; \
    elif ! grep -q '^CONTROL_PORT=' \"\$f\"; then \
      printf '\\nCONTROL_PORT=\\\"%s\\\"\\n' \"$control_port\" >>\"\$f\"; \
    fi; \
    if grep -q '^IMAGE=' \"\$f\"; then \
      sed -i \"s#^IMAGE=\\\".*\\\"#IMAGE=\\\"$ml_image\\\"#\" \"\$f\"; \
    else \
      printf '\\nIMAGE=\\\"%s\\\"\\n' \"$ml_image\" >>\"\$f\"; \
    fi; \
  fi"

if [[ -n "$pin" ]]; then
  dc docker exec -it agent_link_service bash -lc "/app/mlctl.sh pair $worker $pin"
else
  dc docker exec -it agent_link_service bash -lc "/app/mlctl.sh pair $worker"
fi
