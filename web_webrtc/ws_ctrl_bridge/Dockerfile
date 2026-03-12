FROM node:20-alpine

WORKDIR /app
COPY package.json package-lock.json* /app/
# Optional proxy support for faster npm install behind local proxy/SS.
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ENV HTTP_PROXY=${HTTP_PROXY}
ENV HTTPS_PROXY=${HTTPS_PROXY}
ENV NO_PROXY=${NO_PROXY}

RUN npm config set fund false \
  && npm config set audit false \
  && npm install --omit=dev

COPY server.js /app/server.js
COPY web /app/web

EXPOSE 8080
CMD ["node", "/app/server.js"]
