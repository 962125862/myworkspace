#!/usr/bin/env bash
set -uo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKERS_DIR="${ML_WORKERS_DIR:-$BASE_DIR/workers}"
DATA_DIR="${ML_DATA_DIR:-$BASE_DIR/data}"

DEFAULT_IMAGE="${ML_IMAGE:-ml-worker:latest}"
DEFAULT_WORKER_BIN="${ML_BIN:-}"
DOCKER_USER="${ML_DOCKER_USER:-$(id -u):$(id -g)}"

log() {
    printf '[mlctl] %s\n' "$*"
}

err() {
    printf '[mlctl][ERR] %s\n' "$*" >&2
}

pause_wait() {
    read -rp "按回车继续..." _
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        err "缺少命令: $1"
        exit 1
    }
}

worker_config_path() {
    local name="$1"
    printf '%s/%s.conf\n' "$WORKERS_DIR" "$name"
}

worker_exists() {
    local name="$1"
    [[ -f "$(worker_config_path "$name")" ]]
}

list_worker_names() {
    shopt -s nullglob
    local files=("$WORKERS_DIR"/*.conf)
    shopt -u nullglob

    local f
    for f in "${files[@]}"; do
        basename "$f" .conf
    done | sort
}

next_worker_name() {
    local i name
    for i in $(seq 0 99); do
        printf -v name "worker%02d" "$i"
        if ! worker_exists "$name"; then
            printf '%s\n' "$name"
            return 0
        fi
    done
    return 1
}

image_exists() {
    docker image inspect "$1" >/dev/null 2>&1
}

container_exists() {
    docker ps -a --format '{{.Names}}' | grep -qx -- "$1"
}

container_running() {
    docker ps --format '{{.Names}}' | grep -qx -- "$1"
}

container_state() {
    if container_running "$CONTAINER_NAME"; then
        echo "RUNNING"
    elif container_exists "$CONTAINER_NAME"; then
        echo "STOPPED"
    else
        echo "ABSENT"
    fi
}

key_files_state() {
    if [[ -f "$KEY_DIR/client.pem" && -f "$KEY_DIR/key.pem" && -f "$KEY_DIR/uniqueid.dat" ]]; then
        echo "YES"
    else
        echo "NO"
    fi
}

write_worker_config() {
    local name="$1"
    local host="$2"
    local app="$3"
    local image="$4"
    local worker_bin="${5:-}"
    local tcp_host="${6:-127.0.0.1}"
    local tcp_port="${7:-}"
    local stream_id="${8:-}"
    local control_bind="${9:-127.0.0.1}"
    local control_port="${10:-}"
    local width="${11:-1280}"
    local height="${12:-720}"
    local fps="${13:-60}"
    local bitrate="${14:-10000}"
    local packet_size="${15:-1024}"
    local colorspace="${16:-709}"
    local range="${17:-limited}"
    local codec="${18:-h264}"
    local chroma="${19:-420}"
    local bitdepth="${20:-8}"
    local skip_mode_check="${21:-1}"

    mkdir -p "$WORKERS_DIR" "$DATA_DIR/$name/keys"

    # If not specified, auto-assign TCP port / stream_id / control_port.
    # Convention:
    # - worker_sN -> STREAM_ID=N, CONTROL_PORT=50000+N (so stream 1 -> 50001)
    # - other names that end with digits keep the old behavior (idx-based)
    if [[ -z "$tcp_port" ]]; then
        local idx_str=""
        if [[ "$name" =~ ([0-9]+)$ ]]; then
            idx_str="${BASH_REMATCH[1]}"
        fi
        tcp_port=$((9000 + 10#${idx_str:-0}))
    fi

    if [[ -z "$stream_id" ]]; then
        if [[ "$name" =~ ^worker_s([0-9]+)$ ]]; then
            stream_id=$((10#${BASH_REMATCH[1]}))
        else
            local idx_str=""
            if [[ "$name" =~ ([0-9]+)$ ]]; then
                idx_str="${BASH_REMATCH[1]}"
            fi
            stream_id=$((1 + 10#${idx_str:-0}))
        fi
    fi

    if [[ -z "$control_port" ]]; then
        if [[ "$name" =~ ^worker_s[0-9]+$ ]]; then
            # Use STREAM_ID so the mapping stays stable even if naming changes.
            control_port=$((50000 + 10#${stream_id}))
        else
            local idx_str=""
            if [[ "$name" =~ ([0-9]+)$ ]]; then
                idx_str="${BASH_REMATCH[1]}"
            fi
            control_port=$((50001 + 10#${idx_str:-0}))
        fi
    fi

    cat > "$(worker_config_path "$name")" <<EOF
HOST="$host"
APP="$app"
IMAGE="$image"
WORKER_BIN="$worker_bin"
TCP_HOST="$tcp_host"
TCP_PORT="$tcp_port"
STREAM_ID="$stream_id"
CONTROL_BIND="$control_bind"
CONTROL_PORT="$control_port"
WIDTH="$width"
HEIGHT="$height"
FPS="$fps"
BITRATE="$bitrate"
PACKET_SIZE="$packet_size"
COLORSPACE="$colorspace"
RANGE="$range"
CODEC="$codec"
CHROMA="$chroma"
BITDEPTH="$bitdepth"
SKIP_MODE_CHECK="$skip_mode_check"
EOF

    log "已创建 worker 配置: $(worker_config_path "$name")"
    log "data 目录: $DATA_DIR/$name"
    log "推流目标: $tcp_host:$tcp_port (Stream ID: $stream_id)"
}

prompt_with_default() {
    local __var_name="$1"
    local prompt="$2"
    local default_value="$3"
    local input
    read -rp "$prompt [$default_value]: " input
    printf -v "$__var_name" '%s' "${input:-$default_value}"
}

create_worker_interactive() {
    mkdir -p "$WORKERS_DIR" "$DATA_DIR"

    local suggested_name="${1:-}"
    if [[ -z "$suggested_name" ]]; then
        suggested_name="$(next_worker_name)" || {
            err "无法自动分配 worker 名称"
            return 1
        }
    fi

    local name
    read -rp "worker 名称 [$suggested_name]: " name
    name="${name:-$suggested_name}"

    if [[ -z "$name" ]]; then
        err "worker 名称不能为空"
        return 1
    fi

    if worker_exists "$name"; then
        err "worker 已存在: $name"
        return 1
    fi

    local host
    read -rp "Sunshine host IP: " host
    if [[ -z "$host" ]]; then
        err "HOST 不能为空"
        return 1
    fi

    local app
    prompt_with_default app "App" "Desktop"

    local image
    prompt_with_default image "Docker image" "$DEFAULT_IMAGE"

    local worker_bin
    if [[ -n "$DEFAULT_WORKER_BIN" ]]; then
        read -rp "容器内 ml_worker 路径 [$DEFAULT_WORKER_BIN]，回车表示用默认: " worker_bin
        worker_bin="${worker_bin:-$DEFAULT_WORKER_BIN}"
    else
        read -rp "容器内 ml_worker 路径 [留空表示使用镜像 ENTRYPOINT]: " worker_bin
        worker_bin="${worker_bin:-}"
    fi

    # 推流目标配置
    echo
    log "TCP推流目标配置"
    local tcp_host
    prompt_with_default tcp_host "推流目标地址" "127.0.0.1"

    local tcp_port
    prompt_with_default tcp_port "推流目标端口" "9000"

    local stream_id
    prompt_with_default stream_id "Stream ID" "1"

    echo
    log "控制端口配置（用于 REQ_IDR/鼠标键盘注入，ml_worker UDP socket）"
    local control_bind
    prompt_with_default control_bind "CONTROL_BIND" "127.0.0.1"
    local control_port
    prompt_with_default control_port "CONTROL_PORT" "50001"

    echo
    log "视频参数配置"
    echo "  宽高/fps/码率会直接影响 Sunshine 编码负载、带宽和后续解码开销。"
    local width height fps bitrate packet_size
    prompt_with_default width "WIDTH 编码宽度" "1280"
    prompt_with_default height "HEIGHT 编码高度" "720"
    prompt_with_default fps "FPS 帧率" "60"
    prompt_with_default bitrate "BITRATE 码率(kbps)" "10000"
    prompt_with_default packet_size "PACKET_SIZE 单包大小(bytes)" "1024"

    echo
    log "色彩参数配置"
    echo "  COLORSPACE 推荐 709；RANGE 选 full 可保留完整范围，limited 更传统。"
    local colorspace range
    prompt_with_default colorspace "COLORSPACE (709/601)" "709"
    prompt_with_default range "RANGE (full/limited)" "limited"

    echo
    log "编码参数配置"
    echo "  444 场景推荐 hevc + 444 + 8bit；通用兼容场景推荐 h264 + 420 + 8bit。"
    local codec chroma bitdepth skip_mode_check
    prompt_with_default codec "CODEC (h264/hevc)" "h264"
    prompt_with_default chroma "CHROMA (420/444)" "420"
    prompt_with_default bitdepth "BITDEPTH (8/10)" "8"

    echo
    log "兼容性参数"
    echo "  SKIP_MODE_CHECK=1 会跳过 Sunshine supported modes 校验，部分机器必须打开。"
    prompt_with_default skip_mode_check "SKIP_MODE_CHECK (1/0)" "1"

    write_worker_config \
        "$name" "$host" "$app" "$image" "$worker_bin" \
        "$tcp_host" "$tcp_port" "$stream_id" "$control_bind" "$control_port" \
        "$width" "$height" "$fps" "$bitrate" "$packet_size" \
        "$colorspace" "$range" "$codec" "$chroma" "$bitdepth" "$skip_mode_check"

    printf '\n'
    log "创建完成:"
    log "  worker=$name"
    log "  host=$host"
    log "  app=$app"
    log "  image=$image"
    log "  video=${width}x${height}@${fps} bitrate=${bitrate} codec=$codec/$chroma/$bitdepth colorspace=$colorspace range=$range"
    return 0
}

create_worker_noninteractive() {
    local name="$1"
    local host="$2"
    local app="${3:-Desktop}"
    local image="${4:-$DEFAULT_IMAGE}"
    local worker_bin="${5:-$DEFAULT_WORKER_BIN}"
    local tcp_host="${6:-127.0.0.1}"
    local tcp_port="${7:-}"
    local stream_id="${8:-}"
    local control_bind="${9:-127.0.0.1}"
    local control_port="${10:-}"
    local width="${11:-1280}"
    local height="${12:-720}"
    local fps="${13:-60}"
    local bitrate="${14:-10000}"
    local packet_size="${15:-1024}"
    local colorspace="${16:-709}"
    local range="${17:-limited}"
    local codec="${18:-h264}"
    local chroma="${19:-420}"
    local bitdepth="${20:-8}"
    local skip_mode_check="${21:-1}"

    if [[ -z "$name" || -z "$host" ]]; then
        err "create_worker_noninteractive: name/host 不能为空"
        return 1
    fi

    if worker_exists "$name"; then
        err "worker 已存在: $name"
        return 1
    fi

    write_worker_config \
        "$name" "$host" "$app" "$image" "$worker_bin" \
        "$tcp_host" "$tcp_port" "$stream_id" "$control_bind" "$control_port" \
        "$width" "$height" "$fps" "$bitrate" "$packet_size" \
        "$colorspace" "$range" "$codec" "$chroma" "$bitdepth" "$skip_mode_check"
}

ensure_worker_or_create() {
    local name="$1"

    if worker_exists "$name"; then
        return 0
    fi

    err "worker 不存在: $name"
    read -rp "是否现在创建它? [Y/n]: " ans

    if [[ -z "${ans:-}" || "$ans" == "y" || "$ans" == "Y" ]]; then
        create_worker_interactive "$name"
    else
        return 1
    fi
}

load_worker() {
    local name="$1"
    local file
    file="$(worker_config_path "$name")"

    [[ -f "$file" ]] || {
        err "找不到 worker 配置: $file"
        return 1
    }

    unset NAME HOST APP IMAGE WORKER_BIN KEY_DIR CONTROL_BIND CONTROL_PORT
    unset WIDTH HEIGHT FPS BITRATE PACKET_SIZE COLORSPACE RANGE CODEC CHROMA BITDEPTH SKIP_MODE_CHECK
    unset CONTAINER_NAME

    # shellcheck disable=SC1090
    source "$file"

    NAME="${NAME:-$name}"
    [[ -n "${HOST:-}" ]] || {
        err "配置文件缺少 HOST: $file"
        return 1
    }

    APP="${APP:-Desktop}"
    IMAGE="${IMAGE:-$DEFAULT_IMAGE}"
    WORKER_BIN="${WORKER_BIN:-$DEFAULT_WORKER_BIN}"

    local idx_str=""
    local suffix="$NAME"

    if [[ "$NAME" =~ ([0-9]+)$ ]]; then
        idx_str="${BASH_REMATCH[1]}"
        suffix="$idx_str"
    fi

    KEY_DIR="${KEY_DIR:-$DATA_DIR/$NAME/keys}"
    CONTROL_BIND="${CONTROL_BIND:-127.0.0.1}"
    if [[ -z "${CONTROL_PORT:-}" ]]; then
        if [[ -n "$idx_str" ]]; then
            CONTROL_PORT=$((50001 + 10#$idx_str))
        else
            CONTROL_PORT=50001
        fi
    fi

    # TCP推流配置（新增）
    TCP_HOST="${TCP_HOST:-127.0.0.1}"
    if [[ -z "${TCP_PORT:-}" ]]; then
        if [[ -n "$idx_str" ]]; then
            TCP_PORT=$((9000 + 10#$idx_str))
        else
            TCP_PORT=9000
        fi
    fi
    STREAM_ID="${STREAM_ID:-$((1 + 10#${idx_str:-0}))}"

    # Video params can be configured per-worker in the worker config file.
    # For one-click/stack usage, allow container env to provide global defaults
    # (applies only when the worker config doesn't specify a value).
    WIDTH="${WIDTH:-${ML_WORKER_DEFAULT_WIDTH:-1280}}"
    HEIGHT="${HEIGHT:-${ML_WORKER_DEFAULT_HEIGHT:-720}}"
    FPS="${FPS:-${ML_WORKER_DEFAULT_FPS:-60}}"
    BITRATE="${BITRATE:-${ML_WORKER_DEFAULT_BITRATE:-10000}}"
    PACKET_SIZE="${PACKET_SIZE:-${ML_WORKER_DEFAULT_PACKET_SIZE:-1024}}"
    COLORSPACE="${COLORSPACE:-${ML_WORKER_DEFAULT_COLORSPACE:-709}}"
    RANGE="${RANGE:-${ML_WORKER_DEFAULT_RANGE:-limited}}"
    CODEC="${CODEC:-${ML_WORKER_DEFAULT_CODEC:-h264}}"
    CHROMA="${CHROMA:-${ML_WORKER_DEFAULT_CHROMA:-420}}"
    BITDEPTH="${BITDEPTH:-${ML_WORKER_DEFAULT_BITDEPTH:-8}}"
    # Skip Sunshine "supported modes" validation by default. Some Sunshine setups
    # (e.g. certain VMs) don't report modes, but streaming still works.
    SKIP_MODE_CHECK="${SKIP_MODE_CHECK:-${ML_WORKER_DEFAULT_SKIP_MODE_CHECK:-1}}"

    CONTAINER_NAME="${CONTAINER_NAME:-mlw-$NAME}"

    return 0
}

ensure_dirs() {
    mkdir -p "$KEY_DIR"
}

build_cmd_prefix() {
    CMD_PREFIX=()
    if [[ -n "$WORKER_BIN" ]]; then
        CMD_PREFIX+=("$WORKER_BIN")
    fi
}

pair_worker() {
    local name="$1"
    local pin="${2:-}"
    ensure_worker_or_create "$name" || return 1
    load_worker "$name" || return 1
    ensure_dirs
    build_cmd_prefix

    image_exists "$IMAGE" || {
        err "镜像不存在: $IMAGE"
        err "请先构建镜像，例如: ./deploy/build_image.sh"
        return 1
    }

    log "开始配对: $NAME"
    log "host=$HOST"
    log "keys=$KEY_DIR"
    if [[ -n "$pin" ]]; then
        if [[ ! "$pin" =~ ^[0-9]{4}$ ]]; then
            err "pin 必须是 4 位数字，例如 1234"
            return 1
        fi
        log "使用自定义 PIN: $pin（请去 Sunshine 主机输入同样的 PIN）"
    else
        log "看到 PIN 后，请去 Sunshine 主机输入"
    fi

    local cmd=("${CMD_PREFIX[@]}" pair "$HOST" --key-dir /keys)
    if [[ -n "$pin" ]]; then
        cmd+=(--pin "$pin")
    fi

    docker run --rm \
        --user "$DOCKER_USER" \
        --network host \
        -v "$KEY_DIR:/keys" \
        "$IMAGE" \
        "${cmd[@]}"
}

list_apps_worker() {
    local name="$1"
    ensure_worker_or_create "$name" || return 1
    load_worker "$name" || return 1
    ensure_dirs
    build_cmd_prefix

    image_exists "$IMAGE" || {
        err "镜像不存在: $IMAGE"
        return 1
    }

    local cmd=("${CMD_PREFIX[@]}" list "$HOST" --key-dir /keys)

    docker run --rm \
        --user "$DOCKER_USER" \
        --network host \
        -v "$KEY_DIR:/keys" \
        "$IMAGE" \
        "${cmd[@]}"
}

up_worker() {
    local name="$1"
    ensure_worker_or_create "$name" || return 1
    load_worker "$name" || return 1
    ensure_dirs
    build_cmd_prefix

    image_exists "$IMAGE" || {
        err "镜像不存在: $IMAGE"
        return 1
    }

    if container_exists "$CONTAINER_NAME"; then
        log "删除旧容器: $CONTAINER_NAME"
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
    fi

    if [[ "$(key_files_state)" != "YES" ]]; then
        log "警告: key 文件还不完整，建议先 pair"
    fi

    local cmd=(
        "${CMD_PREFIX[@]}"
        stream
        --host "$HOST"
        --app "$APP"
        --key-dir /keys
        --tcp-host "$TCP_HOST"
        --tcp-port "$TCP_PORT"
        --stream-id "$STREAM_ID"
        --control-bind "$CONTROL_BIND"
        --control-port "$CONTROL_PORT"
        --width "$WIDTH"
        --height "$HEIGHT"
        --fps "$FPS"
        --bitrate "$BITRATE"
        --packet-size "$PACKET_SIZE"
        --colorspace "$COLORSPACE"
        --range "$RANGE"
        --codec "$CODEC"
        --chroma "$CHROMA"
        --bitdepth "$BITDEPTH"
    )
    if [[ "${SKIP_MODE_CHECK:-0}" != "0" && "${SKIP_MODE_CHECK,,}" != "false" ]]; then
        cmd+=(--skip-mode-check)
    fi

    docker run -d \
        --user "$DOCKER_USER" \
        --restart unless-stopped \
        --name "$CONTAINER_NAME" \
        --label "ml.worker=$NAME" \
        --network host \
        --ipc host \
        -v "$KEY_DIR:/keys" \
        "$IMAGE" \
        "${cmd[@]}" >/dev/null

    log "stream 已启动: $CONTAINER_NAME"
    log "host=$HOST app=$APP tcp=$TCP_HOST:$TCP_PORT stream_id=$STREAM_ID control=$CONTROL_BIND:$CONTROL_PORT codec=$CODEC chroma=$CHROMA bitdepth=$BITDEPTH"
}

ensure_up_worker() {
    local name="$1"
    ensure_worker_or_create "$name" || return 1
    load_worker "$name" || return 1
    ensure_dirs
    build_cmd_prefix

    image_exists "$IMAGE" || {
        err "镜像不存在: $IMAGE"
        return 1
    }

    if container_running "$CONTAINER_NAME"; then
        log "stream 已在运行: $CONTAINER_NAME"
        return 0
    fi

    if container_exists "$CONTAINER_NAME"; then
        docker start "$CONTAINER_NAME" >/dev/null || {
            err "docker start 失败: $CONTAINER_NAME"
            return 1
        }
        log "stream 已启动(复用容器): $CONTAINER_NAME"
        return 0
    fi

    up_worker "$name"
}

down_worker() {
    local name="$1"
    if ! worker_exists "$name"; then
        err "worker 不存在: $name"
        return 1
    fi

    load_worker "$name" || return 1

    if container_exists "$CONTAINER_NAME"; then
        docker rm -f "$CONTAINER_NAME" >/dev/null
        log "已停止并删除: $CONTAINER_NAME"
    else
        log "容器不存在: $CONTAINER_NAME"
    fi

}

stop_soft_worker() {
    local name="$1"
    if ! worker_exists "$name"; then
        err "worker 不存在: $name"
        return 1
    fi

    load_worker "$name" || return 1

    if container_running "$CONTAINER_NAME"; then
        docker stop -t 2 "$CONTAINER_NAME" >/dev/null || {
            err "docker stop 失败: $CONTAINER_NAME"
            return 1
        }
        log "已停止(保留容器): $CONTAINER_NAME"
    elif container_exists "$CONTAINER_NAME"; then
        log "容器已停止: $CONTAINER_NAME"
    else
        log "容器不存在: $CONTAINER_NAME"
    fi

}

restart_worker() {
    local name="$1"
    down_worker "$name" || return 1
    up_worker "$name"
}

logs_worker() {
    local name="$1"
    if ! worker_exists "$name"; then
        err "worker 不存在: $name"
        return 1
    fi

    load_worker "$name" || return 1

    if container_exists "$CONTAINER_NAME"; then
        docker logs -f --tail 200 "$CONTAINER_NAME"
    else
        err "容器不存在: $CONTAINER_NAME"
        return 1
    fi
}

status_worker() {
    local name="$1"
    if ! worker_exists "$name"; then
        err "worker 不存在: $name"
        return 1
    fi

    load_worker "$name" || return 1

    local cstate
    cstate="$(container_state)"

    local kstate
    kstate="$(key_files_state)"

    printf '%-10s  state=%-8s  host=%-15s  app=%-18s  keys=%-3s  tcp=%s:%s  ctrl=%-15s  video=%s/%s/%s  key_dir=%s\n' \
        "$NAME" "$cstate" "$HOST" "$APP" "$kstate" "$TCP_HOST" "$TCP_PORT" "$CONTROL_BIND:$CONTROL_PORT" "$CODEC" "$CHROMA" "$BITDEPTH" "$KEY_DIR"
}

status_all() {
    local any=0
    local name
    while IFS= read -r name; do
        any=1
        status_worker "$name"
    done < <(list_worker_names)

    if [[ "$any" -eq 0 ]]; then
        log "当前没有任何 worker，先创建一个吧。"
        return 0
    fi
}

pair_up_worker() {
    local name="$1"
    local pin="${2:-}"
    if pair_worker "$name" "$pin"; then
        log "配对成功，开始启动 stream..."
        up_worker "$name"
    else
        err "配对失败，没有启动 stream"
        return 1
    fi
}

delete_worker() {
    local name="$1"

    if ! worker_exists "$name"; then
        err "worker 不存在: $name"
        return 1
    fi

    load_worker "$name" || return 1

    echo
    echo "即将删除 worker: $NAME"
    echo "  config: $(worker_config_path "$NAME")"
    echo "  data  : $DATA_DIR/$NAME"
    echo "  keys  : $KEY_DIR"
    echo "  container: $CONTAINER_NAME"
    echo

    read -rp "是否先停止并删除运行中的容器? [Y/n]: " ans1
    if [[ -z "${ans1:-}" || "$ans1" == "y" || "$ans1" == "Y" ]]; then
        down_worker "$NAME" || true
    fi

    rm -f "$(worker_config_path "$NAME")"
    log "已删除配置文件: $(worker_config_path "$NAME")"

    local default_data_dir="$DATA_DIR/$NAME"
    if [[ "$KEY_DIR" == "$default_data_dir/keys" ]]; then
        read -rp "是否同时删除默认 data 目录 $default_data_dir ? [y/N]: " ans2
        if [[ "${ans2:-}" == "y" || "${ans2:-}" == "Y" ]]; then
            rm -rf "$default_data_dir"
            log "已删除数据目录: $default_data_dir"
        fi
    else
        log "注意: KEY_DIR 使用了自定义路径，未自动删除: $KEY_DIR"
    fi
}

choose_worker() {
    mapfile -t workers < <(list_worker_names)

    if [[ "${#workers[@]}" -eq 0 ]]; then
        err "没有 worker，先创建一个吧"
        return 1
    fi

    {
        echo
        echo "可用 workers:"
        local i
        for i in "${!workers[@]}"; do
            printf '  %d) %s\n' "$((i + 1))" "${workers[$i]}"
        done
        echo "  q) 返回"
    } >&2

    local choice
    read -rp "选择 worker 编号: " choice

    if [[ "$choice" == "q" || "$choice" == "Q" ]]; then
        return 1
    fi

    [[ "$choice" =~ ^[0-9]+$ ]] || {
        err "输入无效"
        return 1
    }

    local idx=$((choice - 1))
    (( idx >= 0 && idx < ${#workers[@]} )) || {
        err "编号越界"
        return 1
    }

    printf '%s\n' "${workers[$idx]}"
}

worker_menu() {
    local name="$1"

    while true; do
        echo
        load_worker "$name" || return 1
        echo "======== $NAME ========"
        echo "host=$HOST"
        echo "app=$APP"
        echo "image=$IMAGE"
        echo "key_dir=$KEY_DIR"
        echo "tcp=$TCP_HOST:$TCP_PORT (stream_id=$STREAM_ID)"
        echo "control=$CONTROL_BIND:$CONTROL_PORT"
        echo "video=$CODEC/$CHROMA/$BITDEPTH colorspace=$COLORSPACE range=$RANGE"
        echo
        echo "1) pair"
        echo "2) pair + up"
        echo "3) list apps"
        echo "4) up/start stream"
        echo "5) down/stop stream"
        echo "6) restart stream"
        echo "7) logs"
        echo "8) status"
        echo "9) 删除这个 worker"
        echo "0) 返回上一级"
        echo

        local choice
        read -rp "选择操作: " choice

        case "$choice" in
            1)
                pair_worker "$name"
                pause_wait
                ;;
            2)
                pair_up_worker "$name"
                pause_wait
                ;;
            3)
                list_apps_worker "$name"
                pause_wait
                ;;
            4)
                up_worker "$name"
                pause_wait
                ;;
            5)
                down_worker "$name"
                pause_wait
                ;;
            6)
                restart_worker "$name"
                pause_wait
                ;;
            7)
                logs_worker "$name"
                ;;
            8)
                status_worker "$name"
                pause_wait
                ;;
            9)
                delete_worker "$name"
                pause_wait
                return 0
                ;;
            0)
                return 0
                ;;
            *)
                err "无效选择"
                ;;
        esac
    done
}

interactive_menu() {
    while true; do
        echo
        echo "========== mlctl =========="
        echo "1) 查看全部状态"
        echo "2) 新建 worker"
        echo "3) 选择一个 worker"
        echo "4) 删除一个 worker"
        echo "5) 退出"
        echo

        local choice
        read -rp "请选择: " choice

        case "$choice" in
            1)
                status_all
                pause_wait
                ;;
            2)
                create_worker_interactive
                pause_wait
                ;;
            3)
                local w
                w="$(choose_worker)" || continue
                worker_menu "$w"
                ;;
            4)
                local d
                d="$(choose_worker)" || continue
                delete_worker "$d"
                pause_wait
                ;;
            5|q|Q)
                exit 0
                ;;
            *)
                err "无效选择"
                ;;
        esac
    done
}

usage() {
    cat <<EOF
用法:
  $0                  # 交互菜单（推荐）
  $0 menu

  $0 add
  $0 add worker00 192.168.11.50 [Desktop]
  $0 add worker00 192.168.11.50 Desktop [image] [worker_bin] [tcp_host] [tcp_port] [stream_id] [control_bind] [control_port]
  $0 add worker00 192.168.11.50 Desktop [image] [worker_bin] [tcp_host] [tcp_port] [stream_id] [control_bind] [control_port] [width] [height] [fps] [bitrate] [packet_size] [colorspace] [range] [codec] [chroma] [bitdepth] [skip_mode_check]

  $0 status
  $0 status worker00

  $0 pair worker00 [pin]
  $0 pair-up worker00 [pin]
  $0 list worker00

  $0 up worker00
  $0 ensure-up worker00
  $0 down worker00
  $0 stop-soft worker00
  $0 restart worker00
  $0 logs worker00
  $0 delete worker00

说明:
- 现在不需要你手动创建 worker00.conf
- 如果 pair/up 时 worker 不存在，会提示你现场创建
- add 可显式新建 worker
- pair      : 启动一次性 pair 容器，看到 PIN 后去 Sunshine 主机输入
-            可选指定 PIN: $0 pair worker00 1234
- pair-up   : pair 成功后自动启动 stream
- up/down   : 启停长期 stream 容器
- ensure-up : 若容器已运行则不做任何事；若容器存在但已停止则 start；否则创建并启动
- stop-soft : 只 stop 容器但不删除（下次 ensure-up 可快速 start）
- logs      : 查看 stream 容器日志
- delete    : 删除 worker 配置，并可选删除数据目录

输出方式:
- 使用 TCP 推流（替代了原来的共享内存）
- 支持推流到本地、局域网其他机器或公网服务器
- 每个 worker 可配置独立的推流目标地址和端口
- 当前主链路推荐对接 stream_server 内置 ZMQ BGR bridge

推流地址配置:
- 交互式创建: 会提示输入推流目标地址、端口和 Stream ID
- 交互式创建: 会提示输入推流地址/端口/Stream ID、控制端口、宽高、FPS、码率、色彩空间、色域范围、编码格式等
- 配置文件: 编辑 deploy/workers/worker00.conf 修改 TCP_HOST/TCP_PORT/STREAM_ID
- 命令行: ./deploy/mlctl.sh add worker00 192.168.11.50 Desktop "" "" 192.168.1.100 9000 1

视频参数配置:
- 配置文件中可设置 WIDTH/HEIGHT/FPS/BITRATE/PACKET_SIZE
- 颜色参数可设置 COLORSPACE/RANGE
- 编码参数可设置 CODEC/CHROMA/BITDEPTH
- 交互创建时会显示默认值与说明:
  - WIDTH=1280, HEIGHT=720, FPS=60
  - BITRATE=10000, PACKET_SIZE=1024
  - COLORSPACE=709, RANGE=limited
  - CODEC=h264, CHROMA=420, BITDEPTH=8
  - SKIP_MODE_CHECK=1
- 也可通过环境变量统一设置默认值:
  - ML_WORKER_DEFAULT_WIDTH
  - ML_WORKER_DEFAULT_HEIGHT
  - ML_WORKER_DEFAULT_FPS
  - ML_WORKER_DEFAULT_BITRATE
  - ML_WORKER_DEFAULT_PACKET_SIZE
  - ML_WORKER_DEFAULT_COLORSPACE
  - ML_WORKER_DEFAULT_RANGE
  - ML_WORKER_DEFAULT_CODEC
  - ML_WORKER_DEFAULT_CHROMA
  - ML_WORKER_DEFAULT_BITDEPTH

默认目录:
- workers 配置目录: $WORKERS_DIR
- data 持久化目录 : $DATA_DIR

环境变量:
- ML_IMAGE        默认镜像名（默认: $DEFAULT_IMAGE）
- ML_BIN          如果镜像没有 ENTRYPOINT，可指定容器内 ml_worker 路径
- ML_WORKERS_DIR  自定义 worker 配置目录
- ML_DATA_DIR     自定义 data 目录
- ML_DOCKER_USER  自定义容器运行用户，默认: $DOCKER_USER
EOF
}

main() {
    require_cmd docker
    mkdir -p "$WORKERS_DIR" "$DATA_DIR"

    local cmd="${1:-menu}"

    case "$cmd" in
        menu)
            interactive_menu
            ;;
        add|new|create)
            if [[ $# -ge 3 ]]; then
                create_worker_noninteractive \
                    "$2" "$3" "${4:-Desktop}" "${5:-$DEFAULT_IMAGE}" "${6:-$DEFAULT_WORKER_BIN}" \
                    "${7:-127.0.0.1}" "${8:-}" "${9:-}" "${10:-127.0.0.1}" "${11:-}" \
                    "${12:-1280}" "${13:-720}" "${14:-60}" "${15:-10000}" "${16:-1024}" \
                    "${17:-709}" "${18:-limited}" "${19:-h264}" "${20:-420}" "${21:-8}" "${22:-1}"
            else
                create_worker_interactive "${2:-}"
            fi
            ;;
        status)
            if [[ $# -ge 2 ]]; then
                status_worker "$2"
            else
                status_all
            fi
            ;;
        pair)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            pair_worker "$2" "${3:-}"
            ;;
        pair-up)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            pair_up_worker "$2" "${3:-}"
            ;;
        list)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            list_apps_worker "$2"
            ;;
        up)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            up_worker "$2"
            ;;
        ensure-up|ensure_up|ensure)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            ensure_up_worker "$2"
            ;;
        down|stop)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            down_worker "$2"
            ;;
        stop-soft|stop_soft)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            stop_soft_worker "$2"
            ;;
        restart)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            restart_worker "$2"
            ;;
        logs)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            logs_worker "$2"
            ;;
        delete|rm-worker|remove)
            [[ $# -ge 2 ]] || { usage; exit 1; }
            delete_worker "$2"
            ;;
        *)
            usage
            exit 1
            ;;
    esac
}

main "$@"
