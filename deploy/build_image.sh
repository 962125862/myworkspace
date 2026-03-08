#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$BASE_DIR/.." && pwd)"
IMAGE_DIR="$BASE_DIR/image"

IMAGE_NAME="${ML_IMAGE:-ml-worker:latest}"
BIN="$ROOT_DIR/build/ml_worker"

if [[ ! -f "$BIN" ]]; then
    echo "[ERR] 找不到已编译二进制: $BIN"
    echo "先编译：cmake --build build -j\"$(nproc)\""
    exit 1
fi

rm -f "$IMAGE_DIR/ml_worker"
rm -rf "$IMAGE_DIR/lib"
mkdir -p "$IMAGE_DIR/lib"

cp -av "$BIN" "$IMAGE_DIR/ml_worker"
chmod +x "$IMAGE_DIR/ml_worker"

mapfile -t LOCAL_LIBS < <(
    ldd "$BIN" | awk -v root="$ROOT_DIR/" '
        $3 ~ "^" root { print $3 }
    ' | sort -u
)

if [[ "${#LOCAL_LIBS[@]}" -eq 0 ]]; then
    echo "[WARN] 没找到项目内本地共享库（这不一定是错）"
else
    echo "[INFO] copying local shared libs:"
    for lib in "${LOCAL_LIBS[@]}"; do
        echo "  $lib"
        cp -Lv "$lib" "$IMAGE_DIR/lib/"
    done
fi

echo
echo "[INFO] final image dir:"
ls -l "$IMAGE_DIR"
echo
ls -l "$IMAGE_DIR/lib" || true
echo

docker build -t "$IMAGE_NAME" "$IMAGE_DIR"

echo
echo "[OK] image built: $IMAGE_NAME"
