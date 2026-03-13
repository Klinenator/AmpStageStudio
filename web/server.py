#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parent.parent
STATIC_DIR = Path(__file__).resolve().parent / "static"
DEFAULT_CONTROL_FILE = ROOT / "web" / "live_state.cfg"
AMPS_DIR = ROOT / "amps"
PREAMPS_DIR = ROOT / "preamps"
AMP_STAGE_LIVE_BIN = ROOT / "build" / "amp_stage_live"


def parse_cfg(path: Path) -> dict[str, str]:
    if not path.exists():
      return {}
    result: dict[str, str] = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def write_cfg(path: Path, data: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"{key} = {value}" for key, value in data.items() if value != ""]
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text("\n".join(lines) + "\n")
    tmp.replace(path)


def list_amps() -> list[str]:
    return sorted(p.stem for p in AMPS_DIR.glob("*.amp"))


def list_preamps() -> list[str]:
    return sorted(p.stem for p in PREAMPS_DIR.glob("*.preamp"))


def list_power_tubes() -> list[str]:
    return ["6V6", "6L6", "EL34", "EL84"]


def list_audio_devices() -> dict[str, list[str]]:
    if not AMP_STAGE_LIVE_BIN.exists():
        return {"input_devices": [], "output_devices": []}

    try:
        result = subprocess.run(
            [str(AMP_STAGE_LIVE_BIN), "--list-devices"],
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.SubprocessError, OSError):
        return {"input_devices": [], "output_devices": []}

    input_devices: list[str] = []
    output_devices: list[str] = []
    pattern = re.compile(r"^\[(\d+)\]\s+(.*?)\s+in=(\d+)\s+out=(\d+)$")
    for line in result.stdout.splitlines() + result.stderr.splitlines():
        match = pattern.match(line.strip())
        if not match:
            continue
        name = match.group(2)
        input_channels = int(match.group(3))
        output_channels = int(match.group(4))
        if input_channels >= 2:
            input_devices.append(name)
        if output_channels >= 2:
            output_devices.append(name)

    return {
        "input_devices": input_devices,
        "output_devices": output_devices,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "AmpStageStudioWeb/0.1"

    @property
    def control_path(self) -> Path:
        return Path(self.server.control_path)  # type: ignore[attr-defined]

    def _send_json(self, payload: object, status: int = 200) -> None:
        encoded = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.end_headers()
        self.wfile.write(encoded)

    def _send_file(self, path: Path, content_type: str) -> None:
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/state":
            payload = parse_cfg(self.control_path)
            payload["_control_file"] = str(self.control_path)
            self._send_json(payload)
            return
        if parsed.path == "/api/amps":
            self._send_json({"amps": list_amps()})
            return
        if parsed.path == "/api/preamps":
            self._send_json({"preamps": list_preamps()})
            return
        if parsed.path == "/api/power-tubes":
            self._send_json({"power_tubes": list_power_tubes()})
            return
        if parsed.path == "/api/audio-devices":
            self._send_json(list_audio_devices())
            return
        if parsed.path == "/api/health":
            self._send_json({"ok": True})
            return
        if parsed.path == "/" or parsed.path == "/index.html":
            self._send_file(STATIC_DIR / "index.html", "text/html; charset=utf-8")
            return
        if parsed.path == "/app.js":
            self._send_file(STATIC_DIR / "app.js", "application/javascript; charset=utf-8")
            return
        if parsed.path == "/styles.css":
            self._send_file(STATIC_DIR / "styles.css", "text/css; charset=utf-8")
            return

        self.send_error(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/api/state":
            self.send_error(HTTPStatus.NOT_FOUND, "Not found")
            return

        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            body = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            self.send_error(HTTPStatus.BAD_REQUEST, "Invalid JSON")
            return

        current = parse_cfg(self.control_path)
        for key, value in body.items():
            if key.startswith("_"):
                continue
            current[key] = str(value)
        write_cfg(self.control_path, current)
        current["_control_file"] = str(self.control_path)
        self._send_json(current)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--control-file", default=str(DEFAULT_CONTROL_FILE))
    args = parser.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.control_path = os.path.abspath(args.control_file)  # type: ignore[attr-defined]
    print(f"Serving AmpStageStudio UI on http://{args.host}:{args.port}")
    print(f"Control file: {httpd.control_path}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
