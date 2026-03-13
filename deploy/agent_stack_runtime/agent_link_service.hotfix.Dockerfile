ARG BASE_IMAGE=ghcr.io/962125862/myworkspace/agent-link-service:latest
FROM ${BASE_IMAGE}

# Hotfix layer: replace the service code only. This avoids needing Docker Hub access
# to rebuild from scratch when the base image is already present locally.
COPY agent_link_service.py /app/agent_link_service.py

