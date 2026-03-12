#!/usr/bin/env bash
set -euo pipefail

# Toggle Docker daemon proxy settings via a systemd drop-in.
#
# This script is intended to run as root (e.g. from systemd service docker_proxy.service).
# It writes:
#   /etc/systemd/system/docker.service.d/proxy.conf
#
# Config (optional):
#   /etc/default/docker_proxy
#     HTTP_PROXY=http://127.0.0.1:1095
#     HTTPS_PROXY=http://127.0.0.1:1095
#     NO_PROXY=localhost,127.0.0.1,::1,192.168.0.0/16,10.0.0.0/8
#
# NOTE: Enabling/disabling proxy restarts docker.service.

DROPIN_DIR="/etc/systemd/system/docker.service.d"
DROPIN_FILE="$DROPIN_DIR/proxy.conf"
ENV_FILE="/etc/default/docker_proxy"

HTTP_PROXY_DEFAULT="http://127.0.0.1:1095"
HTTPS_PROXY_DEFAULT="http://127.0.0.1:1095"
NO_PROXY_DEFAULT="localhost,127.0.0.1,::1,192.168.0.0/16,10.0.0.0/8"

load_env() {
  if [[ -f "$ENV_FILE" ]]; then
    # shellcheck disable=SC1090
    source "$ENV_FILE"
  fi
  HTTP_PROXY="${HTTP_PROXY:-$HTTP_PROXY_DEFAULT}"
  HTTPS_PROXY="${HTTPS_PROXY:-$HTTPS_PROXY_DEFAULT}"
  NO_PROXY="${NO_PROXY:-$NO_PROXY_DEFAULT}"
}

do_on() {
  load_env
  mkdir -p "$DROPIN_DIR"
  cat >"$DROPIN_FILE" <<EOF
[Service]
Environment="HTTP_PROXY=$HTTP_PROXY"
Environment="HTTPS_PROXY=$HTTPS_PROXY"
Environment="NO_PROXY=$NO_PROXY"
EOF
  systemctl daemon-reload
  systemctl restart docker
}

do_off() {
  rm -f "$DROPIN_FILE"
  # If dir is empty, remove it (best-effort).
  rmdir "$DROPIN_DIR" 2>/dev/null || true
  systemctl daemon-reload
  systemctl restart docker
}

do_status() {
  if [[ -f "$DROPIN_FILE" ]]; then
    echo "ON ($DROPIN_FILE exists)"
    cat "$DROPIN_FILE"
  else
    echo "OFF ($DROPIN_FILE missing)"
  fi
}

cmd="${1:-}"
case "$cmd" in
  on) do_on ;;
  off) do_off ;;
  status) do_status ;;
  *)
    echo "usage: $0 <on|off|status>" >&2
    exit 2
    ;;
esac

