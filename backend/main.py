import json
import os
import subprocess
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

BASE = Path(__file__).resolve().parent.parent
AGENT = BASE / "agent" / "monitor"
FRONTEND = BASE / "frontend"

app = FastAPI(title="VPS Monitor", version="0.1.0")
app.mount("/static", StaticFiles(directory=FRONTEND), name="static")


def run_command(args):
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=20, check=True
        )
        return result.stdout
    except (subprocess.SubprocessError, OSError) as exc:
        raise HTTPException(status_code=503, detail=f"Command failed: {args[0]}") from exc


def docker_available():
    return subprocess.run(
        ["docker", "info"],
        capture_output=True,
        text=True,
        timeout=10
    ).returncode == 0


@app.get("/")
def dashboard():
    return FileResponse(FRONTEND / "index.html")


@app.get("/api/health")
def health():
    return {"status": "ok", "service": "vps-monitor"}


@app.get("/api/system")
def system():
    try:
        raw = subprocess.run(
            [str(AGENT)], capture_output=True, text=True, timeout=5, check=True
        ).stdout
        return json.loads(raw)
    except (subprocess.SubprocessError, OSError, json.JSONDecodeError) as exc:
        raise HTTPException(status_code=500, detail="C++ agent failed") from exc


@app.get("/api/docker")
def docker():
    if not docker_available():
        raise HTTPException(status_code=503, detail="Docker unavailable or permission denied")

    container_format = (
        '{{json .}}'
    )
    raw_containers = run_command([
        "docker", "ps", "-a", "--size",
        "--format", container_format
    ])
    containers = []
    for line in raw_containers.splitlines():
        if not line.strip():
            continue
        item = json.loads(line)
        labels = {}
        for part in item.get("Labels", "").split(","):
            if "=" in part:
                k, v = part.split("=", 1)
                labels[k] = v
        project = labels.get("com.docker.compose.project", "standalone")
        containers.append({
            "id": item.get("ID"),
            "name": item.get("Names"),
            "image": item.get("Image"),
            "status": item.get("Status"),
            "size": item.get("Size"),
            "project": project,
            "ports": item.get("Ports", ""),
        })

    storage_raw = run_command(["docker", "system", "df", "--format", "{{json .}}"])
    storage = []
    for line in storage_raw.splitlines():
        if line.strip():
            try:
                storage.append(json.loads(line))
            except json.JSONDecodeError:
                storage.append({"raw": line})

    projects = {}
    for c in containers:
        projects.setdefault(c["project"], []).append(c)

    return {
        "containers": containers,
        "projects": [
            {"name": name, "container_count": len(items), "containers": items}
            for name, items in sorted(projects.items())
        ],
        "storage": storage,
    }


@app.get("/api/overview")
def overview():
    sysdata = system()
    docker_data = docker()
    return {
        **sysdata,
        "container_count": len(docker_data["containers"]),
        "project_count": len(docker_data["projects"]),
        "docker_storage": docker_data["storage"],
    }
