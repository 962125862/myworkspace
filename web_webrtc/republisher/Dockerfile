FROM python:3.11-slim-bookworm

# Optional proxy support for faster builds behind local proxy/SS.
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY

ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}
ENV NO_PROXY=${NO_PROXY}

# Optional: switch apt sources (can be overridden at build time).
ARG APT_MIRROR=deb.debian.org

RUN set -eux; \
  sed -i "s|http://deb.debian.org/debian|http://${APT_MIRROR}/debian|g" /etc/apt/sources.list; \
  sed -i "s|http://security.debian.org/debian-security|http://${APT_MIRROR}/debian-security|g" /etc/apt/sources.list || true; \
  if [ -n "${HTTP_PROXY:-}" ]; then \
    printf 'Acquire::http::Proxy "%s";\nAcquire::https::Proxy "%s";\n' "$HTTP_PROXY" "${HTTPS_PROXY:-$HTTP_PROXY}" >/etc/apt/apt.conf.d/01proxy; \
  fi; \
  apt-get update \
  && apt-get install -y --no-install-recommends ffmpeg ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY republish.py /app/republish.py

ENTRYPOINT ["python", "/app/republish.py"]
