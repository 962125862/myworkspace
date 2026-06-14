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
VAAPI_FILTER="${STREAM_TRANSCODE_VAAPI_FILTER-scale_vaapi=format=nv12}"
VAAPI_PROFILE="${STREAM_TRANSCODE_VAAPI_PROFILE:-}"
NVENC_HWACCEL="${STREAM_TRANSCODE_NVENC_HWACCEL:-cuda}"
NVENC_FILTER="${STREAM_TRANSCODE_NVENC_FILTER:-}"
NVENC_PROFILE="${STREAM_TRANSCODE_NVENC_PROFILE:-}"
BATCH_SIZE="${STREAM_TRANSCODE_BATCH_SIZE:-10}"
PARALLEL="${STREAM_TRANSCODE_PARALLEL:-1}"
DELETE_SOURCE="${STREAM_TRANSCODE_DELETE_SOURCE:-0}"
MAX_BATCHES="${STREAM_TRANSCODE_MAX_BATCHES:-0}"
DRY_RUN="${STREAM_TRANSCODE_DRY_RUN:-0}"
LOCK_FILE="${STREAM_TRANSCODE_LOCK_FILE:-/tmp/transcode_stream_records.lock}"
CLAIM_DIR="${STREAM_TRANSCODE_CLAIM_DIR:-}"
CLAIM_MAX_AGE_SEC="${STREAM_TRANSCODE_CLAIM_MAX_AGE_SEC:-1800}"
WORKER_NAME="${STREAM_TRANSCODE_WORKER_NAME:-$(hostname -s 2>/dev/null || hostname)}"

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
  STREAM_TRANSCODE_VAAPI_FILTER=scale_vaapi=format=nv12
  STREAM_TRANSCODE_VAAPI_PROFILE=
  STREAM_TRANSCODE_NVENC_HWACCEL=cuda
  STREAM_TRANSCODE_NVENC_FILTER=
  STREAM_TRANSCODE_NVENC_PROFILE=
  STREAM_TRANSCODE_BATCH_SIZE=10
  STREAM_TRANSCODE_PARALLEL=1
  STREAM_TRANSCODE_DELETE_SOURCE=0
  STREAM_TRANSCODE_MAX_BATCHES=0
  STREAM_TRANSCODE_DRY_RUN=0
  STREAM_TRANSCODE_CLAIM_DIR=
  STREAM_TRANSCODE_CLAIM_MAX_AGE_SEC=1800
  STREAM_TRANSCODE_WORKER_NAME=$(hostname -s)

behavior:
  - scans closed *.h264 / *.hevc files under per-stream directories
  - waits until each stream has BATCH_SIZE untranscoded files
  - concatenates one stream batch in timestamp order and transcodes it to one MP4
  - writes output as *.mp4.tmp, then renames to *.mp4 after ffmpeg succeeds
  - marks every source in a successful batch as *.transcoded
  - runs up to STREAM_TRANSCODE_PARALLEL ffmpeg batches at the same time
  - uses flock so timer/manual runs cannot overlap
  - optionally uses STREAM_TRANSCODE_CLAIM_DIR for cross-host batch claims
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

batch_name() {
    local stream_dir="$1"
    shift
    local files=("$@")
    local first last stream_name first_base last_base

    first="${files[0]}"
    last="${files[$((${#files[@]} - 1))]}"
    stream_name="$(basename "$stream_dir")"
    first_base="$(basename "${first%.*}")"
    last_base="$(basename "${last%.*}")"
    printf '%s_%s__%s' "$stream_name" "$first_base" "$last_base"
}

claim_batch() {
    local stream_dir="$1"
    shift
    local claim name

    if [[ -z "$CLAIM_DIR" || "$DRY_RUN" != "0" ]]; then
        printf '\n'
        return 0
    fi

    name="$(batch_name "$stream_dir" "$@")"
    mkdir -p "$CLAIM_DIR"
    claim="$CLAIM_DIR/$name.claim"

    if mkdir "$claim" 2>/dev/null; then
        {
            printf 'worker=%s\n' "$WORKER_NAME"
            printf 'host=%s\n' "$(hostname -f 2>/dev/null || hostname)"
            printf 'pid=%s\n' "$$"
            printf 'time=%s\n' "$(date -Is)"
        } >"$claim/owner"
        printf '%s\n' "$claim"
        return 0
    fi

    if [[ -d "$claim" ]] && claim_is_stale "$claim"; then
        err "remove stale claim batch=$name owner=$(tr '\n' ' ' <"$claim/owner" 2>/dev/null || true)"
        rm -rf -- "$claim"
        if mkdir "$claim" 2>/dev/null; then
            {
                printf 'worker=%s\n' "$WORKER_NAME"
                printf 'host=%s\n' "$(hostname -f 2>/dev/null || hostname)"
                printf 'pid=%s\n' "$$"
                printf 'time=%s\n' "$(date -Is)"
            } >"$claim/owner"
            printf '%s\n' "$claim"
            return 0
        fi
    fi

    err "skip claimed batch=$name owner=$(tr '\n' ' ' <"$claim/owner" 2>/dev/null || true)"
    return 1
}

release_claim() {
    local claim="${1:-}"
    if [[ -n "$claim" && -d "$claim" ]]; then
        rm -rf -- "$claim"
    fi
}

owner_value() {
    local claim="$1"
    local key="$2"
    awk -F= -v k="$key" '$1 == k {print substr($0, length(k) + 2); exit}' "$claim/owner" 2>/dev/null || true
}

claim_is_stale() {
    local claim="$1"
    local owner_host owner_pid this_host mtime now age

    owner_host="$(owner_value "$claim" host)"
    owner_pid="$(owner_value "$claim" pid)"
    this_host="$(hostname -f 2>/dev/null || hostname)"

    if [[ -n "$owner_host" && "$owner_host" == "$this_host" && "$owner_pid" =~ ^[0-9]+$ ]]; then
        if ! kill -0 "$owner_pid" 2>/dev/null; then
            return 0
        fi
    fi

    if is_uint "$CLAIM_MAX_AGE_SEC" && (( CLAIM_MAX_AGE_SEC > 0 )); then
        mtime="$(stat -c %Y "$claim" 2>/dev/null || printf '0')"
        now="$(date +%s)"
        if [[ "$mtime" =~ ^[0-9]+$ ]]; then
            age=$((now - mtime))
            if (( age > CLAIM_MAX_AGE_SEC )); then
                return 0
            fi
        fi
    fi

    return 1
}

transcode_batch() {
    local claim_path="$1"
    shift
    local stream_dir="$1"
    shift
    local files=("$@")
    local first last ext demuxer stream_name first_base last_base out_dir out tmp log vf
    local -a ffmpeg_args=()

    if (( ${#files[@]} == 0 )); then
        release_claim "$claim_path"
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
        release_claim "$claim_path"
        return 0
    fi

    if [[ "$DRY_RUN" != "0" ]]; then
        printf 'dry-run batch stream=%s count=%s -> %s\n' "$stream_name" "${#files[@]}" "$out"
        printf 'dry-run first=%s last=%s\n' "$first" "$last"
        release_claim "$claim_path"
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
        )
        if [[ -n "$VAAPI_FILTER" ]]; then
            ffmpeg_args+=(-vf "$VAAPI_FILTER")
        fi
        ffmpeg_args+=(
            -r "$OUTPUT_FPS"
            -c:v "$ENCODER"
        )
        if [[ -n "$VAAPI_PROFILE" ]]; then
            ffmpeg_args+=(-profile:v "$VAAPI_PROFILE")
        fi
        ffmpeg_args+=(
            -b:v "$BITRATE" -tag:v hvc1 -movflags +faststart
            -f mp4
            "$tmp"
        )
    elif [[ "$ENCODER" == *_nvenc ]]; then
        ffmpeg_args=(
            -hide_banner -y
        )
        if [[ "$NVENC_HWACCEL" != "0" && -n "$NVENC_HWACCEL" ]]; then
            ffmpeg_args+=(
                -hwaccel "$NVENC_HWACCEL"
                -hwaccel_output_format "$NVENC_HWACCEL"
            )
        fi
        ffmpeg_args+=(
            -r "$INPUT_FPS" -f "$demuxer" -i pipe:0
            -an
        )
        if [[ -n "$NVENC_FILTER" ]]; then
            ffmpeg_args+=(-vf "$NVENC_FILTER")
        fi
        ffmpeg_args+=(
            -r "$OUTPUT_FPS"
            -c:v "$ENCODER"
        )
        if [[ -n "$NVENC_PROFILE" ]]; then
            ffmpeg_args+=(-profile:v "$NVENC_PROFILE")
        fi
        ffmpeg_args+=(
            -b:v "$BITRATE" -tag:v hvc1 -movflags +faststart
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
        release_claim "$claim_path"
    else
        rm -f -- "$tmp"
        release_claim "$claim_path"
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
    local claim_path

    if (( PARALLEL > 1 )); then
        wait_for_available_slot
    fi

    if ! claim_path="$(claim_batch "$@")"; then
        return 1
    fi

    if (( PARALLEL <= 1 )); then
        transcode_batch "$claim_path" "$@" || failed_batches=$((failed_batches + 1))
        return 0
    fi

    transcode_batch "$claim_path" "$@" &
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
            if launch_transcode_batch "$stream_dir" "${batch[@]}"; then
                processed_batches=$((processed_batches + 1))
                if is_uint "$MAX_BATCHES" && (( MAX_BATCHES > 0 && processed_batches >= MAX_BATCHES )); then
                    return 0
                fi
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
