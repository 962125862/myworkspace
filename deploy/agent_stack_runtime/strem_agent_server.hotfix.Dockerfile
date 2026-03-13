ARG BASE_IMAGE=ghcr.io/962125862/myworkspace/strem-agent-server:latest
FROM ${BASE_IMAGE}

COPY strem_agent_server /app/strem_agent_server

