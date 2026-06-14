#!/usr/bin/env bash
set -euo pipefail

STORE_PATH="${HEVC_STORE_PRUNE_STORE_PATH:-/home/gejun/hevc_store}"
TRANSCODE_DIR="${HEVC_STORE_PRUNE_TRANSCODE_DIR:-$STORE_PATH/transcoded}"
RAW_DIR="${HEVC_STORE_PRUNE_RAW_DIR:-$STORE_PATH/raw}"
LOG_DIR="${HEVC_STORE_PRUNE_LOG_DIR:-$STORE_PATH/logs}"
CODED_TRIGGER_GB="${HEVC_STORE_PRUNE_CODED_TRIGGER_USED_GB:-600}"
CODED_RETENTION_DAYS="${HEVC_STORE_PRUNE_CODED_RETENTION_DAYS:-3}"
RAW_TRIGGER_GB="${HEVC_STORE_PRUNE_RAW_TRIGGER_USED_GB:-800}"
RAW_TARGET_GB="${HEVC_STORE_PRUNE_RAW_TARGET_USED_GB:-750}"
TMP_MAX_AGE_MIN="${HEVC_STORE_PRUNE_TMP_MAX_AGE_MIN:-360}"
DRY_RUN="${HEVC_STORE_PRUNE_DRY_RUN:-0}"
LOCK_FILE="${HEVC_STORE_PRUNE_LOCK_FILE:-/tmp/hevc_store_prune.lock}"

usage() {
    cat <<'EOF'
usage:
  ./deploy/prune_hevc_store_capacity.sh

env:
  HEVC_STORE_PRUNE_STORE_PATH=/home/gejun/hevc_store
  HEVC_STORE_PRUNE_TRANSCODE_DIR=/home/gejun/hevc_store/transcoded
  HEVC_STORE_PRUNE_RAW_DIR=/home/gejun/hevc_store/raw
  HEVC_STORE_PRUNE_LOG_DIR=/home/gejun/hevc_store/logs
  HEVC_STORE_PRUNE_CODED_TRIGGER_USED_GB=600
  HEVC_STORE_PRUNE_CODED_RETENTION_DAYS=3
  HEVC_STORE_PRUNE_RAW_TRIGGER_USED_GB=800
  HEVC_STORE_PRUNE_RAW_TARGET_USED_GB=750
  HEVC_STORE_PRUNE_TMP_MAX_AGE_MIN=360
  HEVC_STORE_PRUNE_DRY_RUN=0

behavior:
  - checks used bytes on the filesystem that contains STORE_PATH
  - when used > CODED_TRIGGER_USED_GB, deletes transcoded *.mp4 older than CODED_RETENTION_DAYS
  - when used > RAW_TRIGGER_USED_GB, deletes transcoded *.mp4 not from today
  - removes matching transcode logs for deleted MP4 files
  - removes stale *.tmp files older than TMP_MAX_AGE_MIN
  - when used still > RAW_TRIGGER_USED_GB, deletes oldest closed raw *.h264/*.hevc until RAW_TARGET_USED_GB
  - never deletes active *.tmp raw files
EOF
}

err() {
    printf '%s\n' "$*" >&2
}

is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

gb_to_bytes() {
    printf '%s\n' "$(( $1 * 1024 * 1024 * 1024 ))"
}

fs_used_bytes() {
    df -PB1 "$STORE_PATH" | awk 'NR == 2 {print $3}'
}

refresh_used_bytes() {
    used_bytes="$(fs_used_bytes)"
}

delete_path() {
    local path="$1"
    local reason="$2"

    if [[ "$DRY_RUN" != "0" ]]; then
        printf 'dry-run delete reason=%s path=%s\n' "$reason" "$path"
    else
        rm -f -- "$path"
        printf 'deleted reason=%s path=%s\n' "$reason" "$path"
    fi
}

delete_matching_log() {
    local mp4="$1"
    local stream base log

    stream="$(basename "$(dirname "$mp4")")"
    base="$(basename "${mp4%.mp4}")"
    log="$LOG_DIR/${stream}_${base}.log"
    [[ -f "$log" ]] || return 0
    delete_path "$log" "matching-log"
}

delete_stale_tmp() {
    [[ -d "$TRANSCODE_DIR" ]] || return 0

    while IFS= read -r -d '' tmp; do
        delete_path "$tmp" "stale-tmp"
        tmp_deleted=$((tmp_deleted + 1))
    done < <(
        find "$TRANSCODE_DIR" -type f -name '*.tmp' -mmin +"$TMP_MAX_AGE_MIN" -print0
    )
}

delete_old_coded() {
    local old_mmin=$((CODED_RETENTION_DAYS * 24 * 60))
    local mtime size path

    [[ -d "$TRANSCODE_DIR" ]] || return 0

    while IFS=$'\t' read -r -d '' mtime size path; do
        [[ -f "$path" ]] || continue
        delete_path "$path" "old-coded"
        delete_matching_log "$path"
        coded_deleted_count=$((coded_deleted_count + 1))
        coded_deleted_bytes=$((coded_deleted_bytes + size))
    done < <(
        find "$TRANSCODE_DIR" -type f -name '*.mp4' -mmin +"$old_mmin" \
            -printf '%T@\t%s\t%p\0' | sort -z -n
    )
}

delete_non_today_coded() {
    local today_start
    local mtime size path

    [[ -d "$TRANSCODE_DIR" ]] || return 0

    today_start="$(date +%F)"
    while IFS=$'\t' read -r -d '' mtime size path; do
        [[ -f "$path" ]] || continue
        delete_path "$path" "non-today-coded"
        delete_matching_log "$path"
        today_coded_deleted_count=$((today_coded_deleted_count + 1))
        today_coded_deleted_bytes=$((today_coded_deleted_bytes + size))
    done < <(
        find "$TRANSCODE_DIR" -type f -name '*.mp4' ! -newermt "$today_start 00:00:00" \
            -printf '%T@\t%s\t%p\0' | sort -z -n
    )
}

delete_raw_until_target() {
    local mtime size path

    [[ -d "$RAW_DIR" ]] || return 0

    while IFS=$'\t' read -r -d '' mtime size path; do
        [[ -f "$path" ]] || continue
        delete_path "$path" "emergency-raw"
        rm -f -- "$path.synced" "$path.transcoded"
        raw_deleted_count=$((raw_deleted_count + 1))
        raw_deleted_bytes=$((raw_deleted_bytes + size))

        if (( used_bytes > size )); then
            used_bytes=$((used_bytes - size))
        else
            used_bytes=0
        fi
        if (( used_bytes <= raw_target_bytes )); then
            return 0
        fi
    done < <(
        find "$RAW_DIR" -type f \( -name '*.h264' -o -name '*.hevc' \) \
            -printf '%T@\t%s\t%p\0' | sort -z -n
    )
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

command -v flock >/dev/null 2>&1 || {
    err "missing command: flock"
    exit 2
}

if [[ ! -d "$STORE_PATH" ]]; then
    err "store path does not exist: $STORE_PATH"
    exit 1
fi
for value in "$CODED_TRIGGER_GB" "$CODED_RETENTION_DAYS" "$RAW_TRIGGER_GB" "$RAW_TARGET_GB" "$TMP_MAX_AGE_MIN"; do
    if ! is_uint "$value"; then
        err "all threshold values must be integers"
        exit 2
    fi
done

coded_trigger_bytes="$(gb_to_bytes "$CODED_TRIGGER_GB")"
raw_trigger_bytes="$(gb_to_bytes "$RAW_TRIGGER_GB")"
raw_target_bytes="$(gb_to_bytes "$RAW_TARGET_GB")"
if (( raw_target_bytes >= raw_trigger_bytes )); then
    raw_target_bytes=$((raw_trigger_bytes - 50 * 1024 * 1024 * 1024))
fi

exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    printf 'another prune run is active, skip\n'
    exit 0
fi

tmp_deleted=0
coded_deleted_count=0
coded_deleted_bytes=0
today_coded_deleted_count=0
today_coded_deleted_bytes=0
raw_deleted_count=0
raw_deleted_bytes=0

delete_stale_tmp
refresh_used_bytes

printf 'capacity check store=%s used=%s coded_trigger=%s raw_trigger=%s raw_target=%s tmp_deleted=%s\n' \
    "$STORE_PATH" "$used_bytes" "$coded_trigger_bytes" "$raw_trigger_bytes" "$raw_target_bytes" "$tmp_deleted"

if (( used_bytes > coded_trigger_bytes )); then
    delete_old_coded
    refresh_used_bytes
fi

if (( used_bytes > raw_trigger_bytes )); then
    delete_non_today_coded
    refresh_used_bytes
fi

if (( used_bytes > raw_trigger_bytes )); then
    delete_raw_until_target
fi

printf 'capacity result used_estimate=%s coded_deleted_count=%s coded_deleted_bytes=%s non_today_coded_deleted_count=%s non_today_coded_deleted_bytes=%s raw_deleted_count=%s raw_deleted_bytes=%s\n' \
    "$used_bytes" "$coded_deleted_count" "$coded_deleted_bytes" "$today_coded_deleted_count" "$today_coded_deleted_bytes" "$raw_deleted_count" "$raw_deleted_bytes"
