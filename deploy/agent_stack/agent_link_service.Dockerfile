FROM docker:27-cli

# Minimal runtime: python3 + bash for mlctl.sh
RUN apk add --no-cache python3 bash ca-certificates

WORKDIR /app
COPY python_dir/agent_link_service.py /app/agent_link_service.py
COPY deploy/mlctl.sh /app/mlctl.sh

RUN chmod +x /app/mlctl.sh && mkdir -p /app/workers /app/data

EXPOSE 40120/tcp 40121/tcp 40122/tcp

ENTRYPOINT ["python3", "/app/agent_link_service.py"]
