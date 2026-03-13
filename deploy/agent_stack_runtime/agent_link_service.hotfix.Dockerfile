ARG BASE_IMAGE=ghcr.io/962125862/myworkspace/agent-link-service:latest
FROM ${BASE_IMAGE}

# Hotfix layer: replace the service code only. This avoids needing Docker Hub access
# to rebuild from scratch when the base image is already present locally.
COPY agent_link_service.py /app/agent_link_service.py

# Also hotfix mlctl.sh so env-based defaults (e.g. ML_WORKER_DEFAULT_WIDTH/HEIGHT)
# take effect without requiring per-worker config edits.
COPY mlctl.sh /app/mlctl.sh
