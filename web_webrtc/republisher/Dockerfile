FROM python:3.11-slim

RUN apt-get update \
  && apt-get install -y --no-install-recommends ffmpeg ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY republish.py /app/republish.py

ENTRYPOINT ["python", "/app/republish.py"]

