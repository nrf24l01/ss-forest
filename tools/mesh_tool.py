#!/usr/bin/env python3
"""Run the SS Forest serial watcher and HTTP API."""

import argparse
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

import serial

from . import root_color_test


UUID_RE = re.compile(r"UUID_REQUEST\s+session=(?P<session>\d+)\s+node=(?P<node>[0-9a-fA-F:]{17})\s+uuid=(?P<uuid>\S*)")
TREE_HEADER_RE = re.compile(r"^TREE\s+count=(?P<count>\d+)\s+complete=(?P<complete>[01])$")
TREE_NODE_RE = re.compile(
    r"^(?P<index>\d+)\s+(?P<mac>[0-9a-fA-F:]{17})\s+"
    r"parent=(?P<parent>[0-9a-fA-F:]{17})\s+"
    r"parent_known=(?P<known>[01])\s+direct_child=(?P<direct>[01])$"
)


class MeshState:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.colors = root_color_test.load_uuid_color_table(args)
        self.colors_lock = threading.RLock()
        self.serial_lock = threading.Lock()
        self.tree_lock = threading.RLock()
        self.tree: dict = {"header": None, "nodes": [], "updated_at": None, "error": "not received"}
        self.serial: serial.Serial | None = None
        self.stop_event = threading.Event()

    def save_colors(self) -> None:
        if not self.args.table_file:
            return
        path = Path(self.args.table_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        with self.colors_lock:
            path.write_text(json.dumps(self.colors, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def set_color(self, uuid_text: str, color: str) -> str:
        normalized = root_color_test.normalize_color(color)
        with self.colors_lock:
            self.colors[uuid_text] = normalized
            self.save_colors()
        return normalized

    def write_serial(self, command: str) -> None:
        with self.serial_lock:
            if self.serial is None:
                return
            self.serial.write((command + "\r\n").encode("ascii"))
            self.serial.flush()

    def request_tree(self) -> None:
        self.write_serial("TREE")

    def handle_line(self, line: str) -> None:
        uuid_match = UUID_RE.search(line)
        if uuid_match:
            uuid_text = uuid_match.group("uuid")
            with self.colors_lock:
                color = self.colors.get(uuid_text, self.args.color)
            print(f"UUID_REQUEST node={uuid_match.group('node')} uuid={uuid_text} -> #{color}", flush=True)
            self.write_serial(f"color {color}")

        tree_match = TREE_HEADER_RE.fullmatch(line)
        if tree_match:
            with self.tree_lock:
                self.tree = {
                    "header": {
                        "count": int(tree_match.group("count")),
                        "complete": tree_match.group("complete") == "1",
                    },
                    "nodes": [],
                    "updated_at": time.time(),
                    "error": None,
                }
            return

        node_match = TREE_NODE_RE.fullmatch(line)
        if node_match:
            with self.tree_lock:
                if self.tree["header"] is not None:
                    self.tree["nodes"].append({
                        "index": int(node_match.group("index")),
                        "mac": node_match.group("mac").lower(),
                        "parent": node_match.group("parent").lower(),
                        "parent_known": node_match.group("known") == "1",
                        "direct_child": node_match.group("direct") == "1",
                    })

    def run(self) -> None:
        with serial.Serial(self.args.port, self.args.baud, timeout=0.2) as ser:
            self.serial = ser
            print(f"Watching {self.args.port} at {self.args.baud} baud", flush=True)
            next_tree = 0.0
            while not self.stop_event.is_set():
                now = time.monotonic()
                if now >= next_tree:
                    self.request_tree()
                    next_tree = now + self.args.tree_interval
                raw_line = ser.readline()
                if raw_line:
                    line = raw_line.decode("utf-8", errors="replace").strip()
                    if self.args.print_all:
                        print(line, flush=True)
                    self.handle_line(line)


class ApiHandler(BaseHTTPRequestHandler):
    state: MeshState

    def log_message(self, format: str, *args) -> None:
        print(f"API {self.address_string()} {format % args}", flush=True)

    def send_json(self, status: int, payload: object) -> None:
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body_json(self) -> object:
        length = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(length))

    def do_GET(self) -> None:
        path = urlparse(self.path).path.rstrip("/")
        if path == "/api/tree":
            with self.state.tree_lock:
                self.send_json(200, self.state.tree)
        elif path == "/api/colors":
            with self.state.colors_lock:
                self.send_json(200, {"colors": self.state.colors})
        elif path == "/api/health":
            self.send_json(200, {"ok": True})
        else:
            self.send_json(404, {"error": "not found"})

    def do_PUT(self) -> None:
        prefix = "/api/colors/"
        path = urlparse(self.path).path
        if not path.startswith(prefix) or not path[len(prefix):]:
            self.send_json(404, {"error": "use PUT /api/colors/{uuid}"})
            return
        try:
            payload = self.body_json()
            if not isinstance(payload, dict) or "color" not in payload:
                raise ValueError("body must be {\"color\": \"#RRGGBB\"}")
            uuid_text = unquote(path[len(prefix):])
            color = self.state.set_color(uuid_text, str(payload["color"]))
            self.send_json(200, {"uuid": uuid_text, "color": color})
        except (ValueError, json.JSONDecodeError) as exc:
            self.send_json(400, {"error": str(exc)})

    def do_PATCH(self) -> None:
        if urlparse(self.path).path.rstrip("/") != "/api/colors":
            self.send_json(404, {"error": "use PATCH /api/colors"})
            return
        try:
            payload = self.body_json()
            if not isinstance(payload, dict):
                raise ValueError("body must be a JSON object of uuid-to-color mappings")
            updated = {}
            for uuid_text, color in payload.items():
                updated[str(uuid_text)] = self.state.set_color(str(uuid_text), str(color))
            self.send_json(200, {"updated": updated})
        except (ValueError, json.JSONDecodeError) as exc:
            self.send_json(400, {"error": str(exc)})


def run_server(args: argparse.Namespace) -> None:
    state = MeshState(args)
    watcher = threading.Thread(target=state.run, name="mesh-serial", daemon=True)
    watcher.start()
    handler = type("ConfiguredApiHandler", (ApiHandler,), {"state": state})
    server = ThreadingHTTPServer((args.host, args.api_port), handler)
    print(f"API listening on http://{args.host}:{args.api_port}", flush=True)
    try:
        server.serve_forever()
    finally:
        state.stop_event.set()
        server.server_close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    server = subparsers.add_parser("server", help="watch serial, serve colors, and expose the HTTP API")
    server.add_argument("--port", required=True, help="Root serial port")
    server.add_argument("--baud", type=int, default=115200)
    server.add_argument("--host", default="127.0.0.1")
    server.add_argument("--api-port", type=int, default=8080)
    server.add_argument("--color", default="00ff00", help="Fallback response color")
    server.add_argument("--map", action="append", default=[], help="UUID-to-color entry: uuid:RRGGBB")
    server.add_argument("--table-file", required=True, help="JSON file used to persist UUID colors")
    server.add_argument("--tree-interval", type=float, default=5.0)
    server.add_argument("--print-all", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        run_server(args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
