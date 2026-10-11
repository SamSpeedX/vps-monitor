FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j2

COPY agent/monitor.cpp ./agent/monitor.cpp

RUN g++ -O2 -std=c++17 \
    agent/monitor.cpp \
    -o agent-monitor


FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libssl3 \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=docker:cli /usr/local/bin/docker /usr/local/bin/docker

WORKDIR /app

COPY frontend/ ./frontend/

COPY --from=builder /src/build/vps-monitor /app/vps-monitor
COPY --from=builder /src/agent-monitor /app/agent/monitor

ENV PORT=8765
ENV FRONTEND_PATH=/app/frontend
ENV AGENT_PATH=/app/agent/monitor

EXPOSE 8765

CMD ["/app/vps-monitor"]


















# FROM python:3.12-slim AS builder

# RUN apt-get update \
#     && apt-get install -y --no-install-recommends g++ \
#     && rm -rf /var/lib/apt/lists/*

# WORKDIR /build
# COPY agent/monitor.cpp ./monitor.cpp

# RUN g++ -O2 -std=c++17 monitor.cpp -o monitor


# FROM python:3.12-slim

# COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /bin/

# # RUN apt-get update \
# #     && apt-get install -y --no-install-recommends docker.io \
# #     && rm -rf /var/lib/apt/lists/*

# COPY --from=docker:cli /usr/local/bin/docker /usr/local/bin/docker

# WORKDIR /app

# COPY backend/ ./backend/
# COPY frontend/ ./frontend/
# COPY --from=builder /build/monitor /app/agent/monitor

# WORKDIR /app/backend

# RUN uv sync --no-dev

# ENV PYTHONUNBUFFERED=1

# EXPOSE 8765

# CMD ["uv", "run", "--no-sync", "uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8765"]
