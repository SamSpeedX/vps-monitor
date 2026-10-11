FROM python:3.12-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY agent/monitor.cpp ./monitor.cpp

RUN g++ -O2 -std=c++17 monitor.cpp -o monitor


FROM python:3.12-slim

COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /bin/

# RUN apt-get update \
#     && apt-get install -y --no-install-recommends docker.io \
#     && rm -rf /var/lib/apt/lists/*

COPY --from=docker:cli /usr/local/bin/docker /usr/local/bin/docker

WORKDIR /app

COPY backend/ ./backend/
COPY frontend/ ./frontend/
COPY --from=builder /build/monitor /app/agent/monitor

WORKDIR /app/backend

RUN uv sync --no-dev

ENV PYTHONUNBUFFERED=1

EXPOSE 8765

CMD ["uv", "run", "--no-sync", "uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8765"]