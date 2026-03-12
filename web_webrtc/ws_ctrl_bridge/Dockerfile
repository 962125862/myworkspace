FROM node:20-alpine

WORKDIR /app
COPY package.json package-lock.json* /app/
RUN npm install --omit=dev

COPY server.js /app/server.js
COPY web /app/web

EXPOSE 8080
CMD ["node", "/app/server.js"]

