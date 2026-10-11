# VPS Monitor

A lightweight VPS monitoring dashboard powered by C++ and Docker.

VPS Monitor provides a web dashboard for viewing server system information,
Docker containers, Docker Compose projects, and storage usage.

## Features

- C++ backend using cpp-httplib
- JSON handling with nlohmann/json
- System monitoring agent written in C++
- Docker container and project monitoring
- Lightweight web frontend
- Docker-based deployment

## Requirements

Before installing VPS Monitor, ensure you have:

- A Linux VPS or server.
- Docker Engine installed and running.
- Access to the server's Docker socket.
- Sufficient permissions to read the host system information.

## Quick Installation

### 1. Pull the Docker image

Download the latest published image from Docker Hub:

```bash
docker pull samochu/vps-monitor:latest
```

### 2. Run the container

Run the following command on your VPS:

```bash
docker run -d \
  --name vps-monitor \
  --restart unless-stopped \
  -p 8765:8765 \
  -v /var/run/docker.sock:/var/run/docker.sock:ro \
  -v /:/host:ro \
  -e HOST_ROOT=/host \
  samochu/vps-monitor:latest
```

This configuration:

- Runs VPS Monitor in the background.
- Restarts the container automatically when Docker restarts.
- Exposes the dashboard on the server's loopback interface.
- Provides access to Docker information through the Docker socket.
- Mounts the host filesystem as read-only for system monitoring.

### 3. Open the dashboard

Because the example above binds the dashboard to `127.0.0.1`, it is not directly exposed to the public internet.

From your local computer, create an SSH tunnel:

```bash
ssh -L 8765:127.0.0.1:8765 USER@YOUR_SERVER_IP
```

Replace `USER` with your server username and `YOUR_SERVER_IP` with your VPS IP address.

Then open the following URL in your browser:

```text
http://127.0.0.1:8765
```

Keep the SSH session open while using the dashboard.

## Docker Compose Installation

You can also deploy VPS Monitor using Docker Compose.

Create a `compose.yaml` file:

```yaml
services:
  vps-monitor:
    image: samochu/vps-monitor:latest
    container_name: vps-monitor
    restart: unless-stopped

    ports:
      - "127.0.0.1:8765:8765"

    volumes:
      - /var/run/docker.sock:/var/run/docker.sock:ro
      - /:/host:ro

    environment:
      HOST_ROOT: /host

    mem_limit: 256m
    cpus: 1.0
```

Start the application:

```bash
docker compose up -d
```

Check the container status:

```bash
docker compose ps
```

View application logs:

```bash
docker compose logs -f
```

Stop the application:

```bash
docker compose down
```

Update to the latest published image:

```bash
docker compose pull
docker compose up -d
```

## API Endpoints

The backend provides the following monitoring endpoints:

| Endpoint | Purpose |
|---|---|
| `/api/system` | Retrieve system monitoring information |
| `/api/docker` | Retrieve Docker-related information |

To test the system endpoint on the VPS:

```bash
curl http://127.0.0.1:8765/api/system
```

To test the Docker endpoint:

```bash
curl http://127.0.0.1:8765/api/docker
```

These endpoints return API responses rather than the web dashboard.

## Troubleshooting

### Container is not running

Check the container status:

```bash
docker ps -a --filter name=vps-monitor
```

Inspect the logs:

```bash
docker logs vps-monitor
```

### Docker monitoring is failing

Check that Docker is running on the host:

```bash
docker info
```

Check whether the Docker socket exists:

```bash
ls -l /var/run/docker.sock
```

Remember that mounting the Docker socket does not guarantee that every Docker operation will be permitted or available.

### Dashboard is not accessible

Confirm that the container is running:

```bash
docker ps --filter name=vps-monitor
```

Check the port mapping:

```bash
docker port vps-monitor
```

If using the SSH tunnel, confirm that the tunnel is active and that port `8765` is not already occupied on your local computer.

## Security Notice

**VPS Monitor requires access to sensitive host resources.**

The Docker socket can provide powerful control over the Docker daemon. Mounting it with `:ro` does not make Docker API operations read-only. A process with access to the socket may be able to control containers or gain access to the host.

The host filesystem is also mounted into the container. Although the mount is read-only, it exposes host files for inspection.

For safer deployment:

- Keep the dashboard bound to `127.0.0.1` unless external access is specifically required.
- Use SSH tunneling or a properly secured reverse proxy.
- Add authentication and authorization before exposing the dashboard to other users or the public internet.
- Restrict access to the VPS and Docker daemon.
- Do not deploy this application as a publicly accessible, unauthenticated monitoring service.

## Technology Stack

- **C++17** — system monitoring agent.
- **Python 3.12** — backend runtime.
- **FastAPI** — API framework.
- **Uvicorn** — ASGI server.
- **Docker** — containerized deployment.
- **HTML, CSS, and JavaScript** — web frontend.

## Docker Image

Pull the latest image:

```bash
docker pull samochu/vps-monitor:latest
```

Docker Hub repository:  
https://hub.docker.com/r/samochu/vps-monitor

## License

VPS Monitor is free and open-source software licensed under the GNU General Public License v3.0 (GPL-3.0).

You are welcome to use, study, modify, and contribute to this project in accordance with the license terms.

Contributions are welcome from developers of all experience levels. Please submit issues, bug reports, feature requests, or pull requests through GitHub.

See the [LICENSE](LICENSE) file for the full license terms.

---

## Original Developer

**VPS Monitor** was created by **Sam Software**(SamSpeedX).

If you use, modify, or redistribute this project, please preserve the original copyright notices and license notices in accordance with the GPL-3.0 license.

Original project: [VPS Monitor](https://github.com/SamSpeedX/vps-monitor)

Contributions from developers of all experience levels are welcome. Please submit issues, improvements, bug fixes, and pull requests through GitHub.

**VPS Monitor** — lightweight monitoring for your VPS infrastructure.

Copyright (C) 2026 Sam Software
