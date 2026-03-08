FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    ffmpeg \
    curl \
    libssl3 \
    libexpat1 \
    libavahi-client3 \
    libuuid1 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY ml_worker /app/ml_worker
COPY lib/ /app/lib/

RUN chmod +x /app/ml_worker

ENV LD_LIBRARY_PATH=/app/lib

ENTRYPOINT ["/app/ml_worker"]
