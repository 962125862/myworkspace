#!/usr/bin/env bash
set -u

MOUNTPOINT="${HEVC_STORE_MOUNTPOINT:-/mnt/hevc_store_35}"
TIMEOUT_SEC="${HEVC_STORE_MOUNT_TIMEOUT_SEC:-10}"
TAG="${HEVC_STORE_MOUNT_LOG_TAG:-hevc-store-mount}"

is_nfs_mounted() {
  findmnt -rn -T "$MOUNTPOINT" -t nfs,nfs4 >/dev/null 2>&1
}

if ! is_nfs_mounted; then
  timeout "$TIMEOUT_SEC" mount "$MOUNTPOINT" >/dev/null 2>&1 || true
fi

if is_nfs_mounted; then
  timeout "$TIMEOUT_SEC" mkdir -p "$MOUNTPOINT/raw" >/dev/null 2>&1 || true
  logger -t "$TAG" "mounted: $MOUNTPOINT"
else
  logger -t "$TAG" "not mounted: $MOUNTPOINT"
fi

exit 0
