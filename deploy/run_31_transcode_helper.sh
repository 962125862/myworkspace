#!/usr/bin/env bash
set -euo pipefail

MAX_BATCHES="${1:-0}"

if [[ "${MAX_BATCHES}" == "-h" || "${MAX_BATCHES}" == "--help" ]]; then
    cat <<'EOF'
usage:
  run_31_transcode_helper.sh [max_batches]

examples:
  run_31_transcode_helper.sh       # process until no eligible batch remains
  run_31_transcode_helper.sh 0     # process until no eligible batch remains
  run_31_transcode_helper.sh 1     # process 1 batch
  run_31_transcode_helper.sh 10    # process 10 batches

This is a manual helper for 192.168.11.31. It does not check NVIDIA idle state.
EOF
    exit 0
fi

if [[ ! "${MAX_BATCHES}" =~ ^[0-9]+$ ]]; then
    echo "max_batches must be an unsigned integer" >&2
    exit 2
fi

export STREAM_TRANSCODE_SRC="${STREAM_TRANSCODE_SRC:-/mnt/hevc_store_35/raw}"
export STREAM_TRANSCODE_DST="${STREAM_TRANSCODE_DST:-/mnt/hevc_store_35/transcoded}"
export STREAM_TRANSCODE_LOG_DIR="${STREAM_TRANSCODE_LOG_DIR:-/mnt/hevc_store_35/logs}"
export STREAM_TRANSCODE_REQUIRE_NFS="${STREAM_TRANSCODE_REQUIRE_NFS:-1}"
export STREAM_TRANSCODE_MIN_AGE_SEC="${STREAM_TRANSCODE_MIN_AGE_SEC:-120}"
export STREAM_TRANSCODE_INPUT_FPS="${STREAM_TRANSCODE_INPUT_FPS:-30}"
export STREAM_TRANSCODE_OUTPUT_FPS="${STREAM_TRANSCODE_OUTPUT_FPS:-10}"
export STREAM_TRANSCODE_BITRATE="${STREAM_TRANSCODE_BITRATE:-1200k}"
export STREAM_TRANSCODE_ENCODER="${STREAM_TRANSCODE_ENCODER:-hevc_nvenc}"
export STREAM_TRANSCODE_NVENC_HWACCEL="${STREAM_TRANSCODE_NVENC_HWACCEL:-cuda}"
export STREAM_TRANSCODE_NVENC_FILTER="${STREAM_TRANSCODE_NVENC_FILTER:-}"
export STREAM_TRANSCODE_NVENC_PROFILE="${STREAM_TRANSCODE_NVENC_PROFILE:-rext}"
export STREAM_TRANSCODE_BATCH_SIZE="${STREAM_TRANSCODE_BATCH_SIZE:-5}"
export STREAM_TRANSCODE_PARALLEL="${STREAM_TRANSCODE_PARALLEL:-1}"
export STREAM_TRANSCODE_DELETE_SOURCE="${STREAM_TRANSCODE_DELETE_SOURCE:-1}"
export STREAM_TRANSCODE_MAX_BATCHES="${STREAM_TRANSCODE_MAX_BATCHES:-$MAX_BATCHES}"
export STREAM_TRANSCODE_LOCK_FILE="${STREAM_TRANSCODE_LOCK_FILE:-/tmp/hevc-transcode-helper-31.lock}"
export STREAM_TRANSCODE_CLAIM_DIR="${STREAM_TRANSCODE_CLAIM_DIR:-/mnt/hevc_store_35/.claims}"
export STREAM_TRANSCODE_CLAIM_MAX_AGE_SEC="${STREAM_TRANSCODE_CLAIM_MAX_AGE_SEC:-1800}"
export STREAM_TRANSCODE_WORKER_NAME="${STREAM_TRANSCODE_WORKER_NAME:-31-t10}"

exec "${STREAM_TRANSCODE_BIN:-/home/gejun/bin/transcode_stream_records.sh}"
