#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${STREAM_TRANSCODE_SRC:-/home/gejun/hevc_store/raw}"
DST_DIR="${STREAM_TRANSCODE_DST:-/home/gejun/hevc_store/transcoded}"
LOG_DIR="${STREAM_TRANSCODE_LOG_DIR:-/home/gejun/hevc_store/logs}"
REQUIRE_NFS="${STREAM_TRANSCODE_REQUIRE_NFS:-0}"
MIN_AGE_SEC="${STREAM_TRANSCODE_MIN_AGE_SEC:-120}"
INPUT_FPS="${STREAM_TRANSCODE_INPUT_FPS:-30}"
OUTPUT_FPS="${STREAM_TRANSCODE_OUTPUT_FPS:-10}"
BITRATE="${STREAM_TRANSCODE_BITRATE:-1200k}"
ENCODER="${STREAM_TRANSCODE_ENCODER:-hevc_nvenc}"
VAAPI_DEVICE="${STREAM_TRANSCODE_VAAPI_DEVICE:-/dev/dri/renderD128}"
BATCH_SIZE="${STREAM_TRANSCODE_BATCH_SIZE:-10}"
PARALLEL="${STREAM_TRANSCODE_PARALLEL:-1}"
DELETE_SOURCE="${STREAM_TRANSCODE_DELETE_SOURCE:-0}"
MAX_BATCHES="${STREAM_TRANSCODE_MAX_BATCHES:-0}"
DRY_RUN="${STREAM_TRANSCODE_DRY_RUN:-0}"
LOCK_FILE="${STREAM_TRANSCODE_LOCK_FILE:-/tmp/transcode_stream_records.lock}"

usage() {
    cat <<'EOF'
usage:
  ./deploy/transcode_stream_records.sh

env:
  STREAM_TRANSCODE_SRC=/home/gejun/hevc_store/raw
  STREAM_TRANSCODE_DST=/home/gejun/hevc_store/transcoded
  STREAM_TRANSCODE_LOG_DIR=/home/gejun/hevc_store/logs
  STREAM_TRANSCODE_REQUIRE_NFS=0
  STREAM_TRANSCODE_MIN_AGE_SEC=120
  STREAM_TRANSCODE_INPUT_FPS=30
  STREAM_TRANSCODE_OUTPUT_FPS=10
  STREAM_TRANSCODE_BITRATE=1200k
  STREAM_TRANSCODE_ENCODER=hevc_nvenc
  STREAM_TRANSCODE_VAAPI_DEVICE=/dev/dri/renderD128
  STREAM_TRANSCODE_BATCH_SIZE=10
  STREAM_TRANSCODE_PARALLEL=1
  STREAM_TRANSCODE_DELETE_SOURCE=0
  STREAM_TRANSCODE_MAX_BATCHES=0
  STREAM_TRANSCODE_DRY_RUN=0

behavior:
  - scans closed *.h264 / *.hevc files under per-stream directories
  - waits until each stream has BATCH_SIZE untranscoded files
  - concatenates one stream batch in timestamp order and transcodes it to one MP4
  - writes output as *.mp4.tmp, then renames to *.mp4 after ffmpeg succeeds
  - marks every source in a successful batch as *.transcoded
  - runs up to STREAM_TRANSCODE_PARALLEL ffmpeg batches at the same time
  - uses flock so timer/manual runs cannot overlap
EOF
}

err() {
    printf '%s\n' "$*" >&2
}

is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

codec_for_ext() {
    case "$1" in
        hevc) printf 'hevc\n' ;;
        h264) printf 'h264\n' ;;
        *) return 1 ;;
    esac
}

mark_batch_done() {
    local f
    for f in "$@"; do
        touch "$f.transcoded"
        if [[ "$DELETE_SOURCE" != "0" ]]; then
            rm -f -- "$f" "$f.synced"
        fi
    done
}

transcode_batch() {
    local stream_dir="$1"
    shift
    local files=("$@")
    local first last ext demuxer stream_name first_base last_base out_dir out tmp log vf
    local -a ffmpeg_args=()

    if (( ${#files[@]} == 0 )); then
        return 0
    fi

    first="${files[0]}"
    last="${files[$((${#files[@]} - 1))]}"
    ext="${first##*.}"
    demuxer="$(codec_for_ext "$ext")" || return 0
    stream_name="$(basename "$stream_dir")"
    first_base="$(basename "${first%.*}")"
    last_base="$(basename "${last%.*}")"

    out_dir="$DST_DIR/$stream_name"
    out="$out_dir/${first_base}__${last_base}.mp4"
    tmp="$out.tmp"
    log="$LOG_DIR/${stream_name}_${first_base}__${last_base}.log"
    vf="fps=${OUTPUT_FPS},format=yuv420p"

    if [[ -s "$out" ]]; then
        mark_batch_done "${files[@]}"
        return 0
    fi

    if [[ "$DRY_RUN" != "0" ]]; then
        printf 'dry-run batch stream=%s count=%s -> %s\n' "$stream_name" "${#files[@]}" "$out"
        printf 'dry-run first=%s last=%s\n' "$first" "$last"
        return 0
    fi

    mkdir -p "$out_dir" "$LOG_DIR"
    rm -f -- "$tmp"
    printf '[%s] transcode batch stream=%s count=%s first=%s last=%s out=%s\n' \
        "$(date -Is)" "$stream_name" "${#files[@]}" "$first" "$last" "$out" | tee -a "$log"

    if [[ "$ENCODER" == *_vaapi ]]; then
        ffmpeg_args=(
            -hide_banner -y
            -hwaccel vaapi
            -hwaccel_device "$VAAPI_DEVICE"
            -hwaccel_output_format vaapi
            -r "$INPUT_FPS" -f "$demuxer" -i pipe:0
            -an
            -vf "scale_vaapi=format=nv12"
            -r "$OUTPUT_FPS"
            -c:v "$ENCODER" -b:v "$BITRATE" -tag:v hvc1 -movflags +faststart
            -f mp4
            "$tmp"
        )
    else
        ffmpeg_args=(
            -hide_banner -y
            -r "$INPUT_FPS" -f "$demuxer" -i pipe:0
            -vf "$vf"
            -an -c:v "$ENCODER" -b:v "$BITRATE" -tag:v hvc1 -movflags +faststart
            -f mp4
            "$tmp"
        )
    fi

    if cat "${files[@]}" | ffmpeg "${ffmpeg_args[@]}" >>"$log" 2>&1; then
        mv -f -- "$tmp" "$out"
        mark_batch_done "${files[@]}"
        printf '[%s] done %s\n' "$(date -Is)" "$out" | tee -a "$log"
    else
        rm -f -- "$tmp"
        err "transcode failed: batch first=$first last=$last (see $log)"
        return 1
    fi
}

wait_for_one_job() {
    local rc

    if (( active_jobs <= 0 )); then
        return 0
    fi

    set +e
    wait -n
    rc=$?
    set -e

    active_jobs=$((active_jobs - 1))
    if (( rc != 0 )); then
        failed_batches=$((failed_batches + 1))
    fi
}

wait_for_available_slot() {
    while (( active_jobs >= PARALLEL )); do
        wait_for_one_job
    done
}

wait_for_all_jobs() {
    while (( active_jobs > 0 )); do
        wait_for_one_job
    done
}

launch_transcode_batch() {
    if (( PARALLEL <= 1 )); then
        transcode_batch "$@" || failed_batches=$((failed_batches + 1))
        return 0
    fi

    wait_for_available_slot
    transcode_batch "$@" &
    active_jobs=$((active_jobs + 1))
}

process_stream_dir() {
    local stream_dir="$1"
    local -a batch=()
    local f ext first_ext

    while IFS= read -r -d '' f; do
        [[ -f "$f.transcoded" ]] && continue
        ext="${f##*.}"
        if [[ ${#batch[@]} -eq 0 ]]; then
            first_ext="$ext"
        elif [[ "$ext" != "$first_ext" ]]; then
            continue
        fi

        batch+=("$f")
        if (( ${#batch[@]} == BATCH_SIZE )); then
            launch_transcode_batch "$stream_dir" "${batch[@]}"
            processed_batches=$((processed_batches + 1))
            if is_uint "$MAX_BATCHES" && (( MAX_BATCHES > 0 && processed_batches >= MAX_BATCHES )); then
                return 0
            fi
            batch=()
        fi
    done < <(
        find "$stream_dir" -maxdepth 1 -type f \( -name '*.h264' -o -name '*.hevc' \) \
            ! -name '*.tmp' \
            -mmin +"$age_mmin" \
            -print0 | sort -z
    )

    if (( ${#batch[@]} > 0 )); then
        printf 'waiting stream=%s have=%s need=%s\n' \
            "$(basename "$stream_dir")" "${#batch[@]}" "$BATCH_SIZE"
    fi
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

command -v ffmpeg >/dev/null 2>&1 || {
    err "missing command: ffmpeg"
    exit 2
}
command -v flock >/dev/null 2>&1 || {
    err "missing command: flock"
    exit 2
}

if ! is_uint "$BATCH_SIZE" || (( BATCH_SIZE < 1 )); then
    err "STREAM_TRANSCODE_BATCH_SIZE must be a positive integer"
    exit 2
fi
if ! is_uint "$PARALLEL" || (( PARALLEL < 1 )); then
    err "STREAM_TRANSCODE_PARALLEL must be a positive integer"
    exit 2
fi

exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    printf 'another transcode run is active, skip\n'
    exit 0
fi

if [[ "$REQUIRE_NFS" != "0" ]] && ! findmnt -rn -T "$SRC_DIR" -t nfs,nfs4 >/dev/null 2>&1; then
    printf 'source is not mounted as NFS, skip: %s\n' "$SRC_DIR"
    exit 0
fi

if [[ ! -d "$SRC_DIR" ]]; then
    err "source directory does not exist: $SRC_DIR"
    exit 1
fi

mkdir -p "$DST_DIR" "$LOG_DIR"

if [[ "$DRY_RUN" == "0" ]]; then
    encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
    if ! grep -qE "[[:space:]]${ENCODER}[[:space:]]" <<<"$encoders"; then
        err "ffmpeg encoder not available: $ENCODER"
        exit 2
    fi
    if [[ "$ENCODER" == *_vaapi && ! -e "$VAAPI_DEVICE" ]]; then
        err "VAAPI device does not exist: $VAAPI_DEVICE"
        exit 2
    fi
fi

age_mmin=$(( (MIN_AGE_SEC + 59) / 60 ))
processed_batches=0
failed_batches=0
active_jobs=0

while IFS= read -r -d '' stream_dir; do
    process_stream_dir "$stream_dir"
    if is_uint "$MAX_BATCHES" && (( MAX_BATCHES > 0 && processed_batches >= MAX_BATCHES )); then
        break
    fi
done < <(
    find "$SRC_DIR" -mindepth 1 -maxdepth 1 -type d -print0 | sort -z
)

wait_for_all_jobs

printf 'processed_batches=%s failed_batches=%s\n' "$processed_batches" "$failed_batches"
