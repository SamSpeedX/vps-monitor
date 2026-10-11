
FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libssl-dev \
        libsqlite3-dev \
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
        libsqlite3-0 \
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
