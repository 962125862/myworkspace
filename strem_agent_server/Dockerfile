FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential make ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
RUN make -j


FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/strem_agent_server /app/strem_agent_server
COPY docker_entrypoint.sh /app/docker_entrypoint.sh
RUN chmod +x /app/docker_entrypoint.sh

EXPOSE 19000/tcp 31234/tcp 31235/tcp

ENTRYPOINT ["/app/docker_entrypoint.sh"]
