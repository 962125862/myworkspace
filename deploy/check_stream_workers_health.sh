#!/usr/bin/env bash
set -euo pipefail

NAME_REGEX="${ML_HEALTH_NAME_REGEX:-^mlw-worker_[0-9]+$}"
EXPECTED_COUNT="${ML_HEALTH_EXPECTED_COUNT:-20}"
LOG_WINDOW="${ML_HEALTH_LOG_WINDOW:-15s}"
MIN_SAMPLES="${ML_HEALTH_MIN_SAMPLES:-3}"
SHOW_OK_DETAILS="${ML_HEALTH_SHOW_OK_DETAILS:-0}"

usage() {
    cat <<'EOF'
usage:
  ./deploy/check_stream_workers_health.sh

env:
  ML_HEALTH_NAME_REGEX='^mlw-worker_[0-9]+$'
  ML_HEALTH_EXPECTED_COUNT=20
  ML_HEALTH_LOG_WINDOW=15s
  ML_HEALTH_MIN_SAMPLES=3
  ML_HEALTH_SHOW_OK_DETAILS=1

behavior:
  - 直接按 Docker 容器名筛选 worker
  - 默认只看主链路 20 路命名: mlw-worker_<数字>
  - 最近窗口内至少需要 N 条 "[video] fps=... state=CONNECTED" 且 sent_fps>0
  - 全部正常输出 ok
  - 任意一路异常输出 error，并列出异常容器和原因
EOF
}

err() {
    printf '%s\n' "$*" >&2
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        err "missing command: $1"
        exit 2
    }
}

recent_logs() {
    local container="$1"
    docker logs --since "$LOG_WINDOW" "$container" 2>&1 || true
}

failure_reason_from_logs() {
    local logs="$1"
    local pat
    local -a patterns=(
        "host is offline"
        "gs_init failed"
        "host is not paired for this key directory"
        "could not resolve app:"
        "gs_start_app failed"
        "LiStartConnection failed:"
        "control socket open failed"
        "fatal_code="
        "tcp_sender: connect failed"
        "tcp_sender: connect timeout or error"
        "tcp_sender: attempting to reconnect"
    )

    for pat in "${patterns[@]}"; do
        if grep -Fq -- "$pat" <<<"$logs"; then
            printf '%s' "$pat"
            return 0
        fi
    done

    return 1
}

last_relevant_line() {
    local logs="$1"
    awk '
        /\[video\]/ ||
        /host .*offline/ ||
        /gs_init failed/ ||
        /host is not paired for this key directory/ ||
        /could not resolve app:/ ||
        /gs_start_app failed/ ||
        /LiStartConnection failed:/ ||
        /control socket open failed/ ||
        /fatal_code=/ ||
        /tcp_sender: connect failed/ ||
        /tcp_sender: connect timeout or error/ ||
        /tcp_sender: attempting to reconnect/ {
            line=$0
        }
        END {
            if (line != "") {
                print line
            }
        }
    ' <<<"$logs"
}

analyze_video_logs() {
    local logs="$1"
    awk -v min_samples="$MIN_SAMPLES" '
        BEGIN {
            total = 0
            good = 0
        }
        /\[video\]/ {
            total++
            connected = (tolower($0) ~ /state=connected/)
            recv = 0
            sent = 0
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^fps=/) {
                    token = $i
                    sub(/^fps=/, "", token)
                    split(token, parts, "/")
                    recv = parts[1] + 0
                    sent = parts[2] + 0
                }
            }
            if (connected && recv > 0 && sent > 0) {
                good++
            }
        }
        END {
            if (total >= min_samples && good == total) {
                print "OK|" total "|" good
            } else {
                print "BAD|" total "|" good
            }
        }
    ' <<<"$logs"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

require_cmd docker
require_cmd awk
require_cmd grep

if [[ ! "$MIN_SAMPLES" =~ ^[1-9][0-9]*$ ]]; then
    err "invalid ML_HEALTH_MIN_SAMPLES: $MIN_SAMPLES"
    printf 'error\n'
    exit 1
fi

if [[ ! "$EXPECTED_COUNT" =~ ^[0-9]+$ ]]; then
    err "invalid ML_HEALTH_EXPECTED_COUNT: $EXPECTED_COUNT"
    printf 'error\n'
    exit 1
fi

mapfile -t containers < <(docker ps -a --format '{{.Names}}' | grep -E "$NAME_REGEX" | sort || true)

declare -a issues=()
declare -a ok_details=()

found_count="${#containers[@]}"
if (( EXPECTED_COUNT > 0 && found_count != EXPECTED_COUNT )); then
    issues+=("container_count_mismatch: expected=${EXPECTED_COUNT} found=${found_count} regex=${NAME_REGEX}")
fi

if (( found_count == 0 )); then
    printf 'error\n'
    printf '%s\n' "${issues[@]}"
    exit 1
fi

for container in "${containers[@]}"; do
    runtime="$(docker inspect --format '{{.State.Status}}|{{.RestartCount}}|{{.State.ExitCode}}' "$container")"
    IFS='|' read -r state restart_count exit_code <<<"$runtime"

    if [[ "$state" != "running" ]]; then
        issues+=("${container}: state=${state} restart=${restart_count} exit=${exit_code}")
        continue
    fi

    logs="$(recent_logs "$container")"
    analysis="$(analyze_video_logs "$logs")"
    IFS='|' read -r verdict total_samples good_samples <<<"$analysis"

    if [[ "$verdict" == "OK" ]]; then
        if [[ "$SHOW_OK_DETAILS" == "1" ]]; then
            ok_details+=("${container}: ok samples=${good_samples}/${total_samples} restart=${restart_count}")
        fi
        continue
    fi

    reason="$(failure_reason_from_logs "$logs" || true)"
    last_line="$(last_relevant_line "$logs")"

    msg="${container}: state=${state} restart=${restart_count} video_ok=${good_samples}/${total_samples}"
    if [[ -n "$reason" ]]; then
        msg+=" reason=${reason}"
    fi
    if [[ -n "$last_line" ]]; then
        msg+=" last=${last_line}"
    else
        msg+=" last=no_recent_video_stats"
    fi
    issues+=("$msg")
done

if [[ "${#issues[@]}" -eq 0 ]]; then
    printf 'ok\n'
    if [[ "$SHOW_OK_DETAILS" == "1" ]]; then
        printf '%s\n' "${ok_details[@]}"
    fi
    exit 0
fi

printf 'error\n'
printf '%s\n' "${issues[@]}"
exit 1
