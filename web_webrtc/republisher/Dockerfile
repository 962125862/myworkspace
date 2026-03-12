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
  # Debian slim images may use either /etc/apt/sources.list (legacy) or deb822 files (*.sources).\n  # Apply mirror replacement best-effort across all known apt source files.\n  for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do \n+    [ -f \"$f\" ] || continue; \n+    sed -i \\\n+      -e \"s|http://deb.debian.org|http://${APT_MIRROR}|g\" \\\n+      -e \"s|https://deb.debian.org|https://${APT_MIRROR}|g\" \\\n+      -e \"s|http://security.debian.org|http://${APT_MIRROR}|g\" \\\n+      -e \"s|https://security.debian.org|https://${APT_MIRROR}|g\" \\\n+      \"$f\"; \n+  done; \
  if [ -n "${HTTP_PROXY:-}" ]; then \
    printf 'Acquire::http::Proxy "%s";\nAcquire::https::Proxy "%s";\n' "$HTTP_PROXY" "${HTTPS_PROXY:-$HTTP_PROXY}" >/etc/apt/apt.conf.d/01proxy; \
  fi; \
  apt-get update \
  && apt-get install -y --no-install-recommends ffmpeg ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY republish.py /app/republish.py

ENTRYPOINT ["python", "/app/republish.py"]
