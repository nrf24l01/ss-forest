#!/usr/bin/env python3
"""Client for the SS Forest watcher HTTP API."""

import argparse
import json
import sys
from urllib import error, request
from urllib.parse import quote


def call(base_url: str, method: str, path: str, payload: object | None = None) -> object:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = request.Request(base_url.rstrip("/") + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with request.urlopen(req, timeout=5) as response:
            return json.loads(response.read())
    except error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    subparsers = parser.add_subparsers(dest="command", required=True)
    color = subparsers.add_parser("color", help="set one UUID color")
    color.add_argument("uuid")
    color.add_argument("value", help="#RRGGBB or RRGGBB")
    colors = subparsers.add_parser("colors", help="patch multiple UUID colors")
    colors.add_argument("values", help='JSON object, for example {"uuid":"ff0000"}')
    subparsers.add_parser("tree", help="fetch the latest buffered tree")
    args = parser.parse_args()
    try:
        if args.command == "color":
            result = call(args.url, "PUT", "/api/colors/" + quote(args.uuid, safe=""), {"color": args.value})
        elif args.command == "colors":
            result = call(args.url, "PATCH", "/api/colors", json.loads(args.values))
        else:
            result = call(args.url, "GET", "/api/tree")
        print(json.dumps(result, indent=2, sort_keys=True))
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
