#!/usr/bin/env python3
"""Listen to root USB serial and return colors from a UUID-to-color table."""

import argparse
import json
import re
import sys
import time

import serial


UUID_RE = re.compile(r"UUID_REQUEST\s+session=(?P<session>\d+)\s+node=(?P<node>[0-9a-fA-F:]{17})\s+uuid=(?P<uuid>\S*)")


def normalize_color(color: str) -> str:
    if color.startswith("#"):
        color = color[1:]
    if not re.fullmatch(r"[0-9a-fA-F]{6}", color):
        raise ValueError("color must be #RRGGBB or RRGGBB")
    return color.lower()


def parse_uuid_color_pair(pair: str) -> tuple[str, str]:
    if ":" not in pair:
        raise ValueError(f"invalid UUID color pair {pair!r}; expected uuid:color")
    uuid_text, color = pair.split(":", 1)
    uuid_text = uuid_text.strip()
    if not uuid_text:
        raise ValueError("uuid in uuid:color pair cannot be empty")
    return uuid_text, normalize_color(color.strip())


def load_uuid_color_table(args: argparse.Namespace) -> dict[str, str]:
    table: dict[str, str] = {}

    if args.table_file:
        with open(args.table_file, "r", encoding="utf-8") as file:
            raw_table = json.load(file)
        if not isinstance(raw_table, dict):
            raise ValueError("table file must contain a JSON object: {\"uuid\": \"RRGGBB\"}")
        for uuid_text, color in raw_table.items():
            table[str(uuid_text)] = normalize_color(str(color))

    for pair in args.map:
        uuid_text, color = parse_uuid_color_pair(pair)
        table[uuid_text] = color

    return table


def check_health(ser: serial.Serial, timeout: float, print_all: bool) -> None:
    ser.reset_input_buffer()
    ser.write(b"PING\r\n")
    ser.flush()
    deadline = time.monotonic() + timeout
    buffer = ""
    while time.monotonic() < deadline:
        pending = ser.in_waiting
        raw_chunk = ser.read(pending if pending > 0 else 1)
        if not raw_chunk:
            continue
        chunk = raw_chunk.decode("utf-8", errors="replace")
        if print_all:
            sys.stdout.write(chunk)
            sys.stdout.flush()
        buffer += chunk
        if "PONG" in buffer.upper():
            print("Healthcheck OK: PONG")
            return
        if len(buffer) > 256:
            buffer = buffer[-128:]
    raise RuntimeError("healthcheck failed: no PONG received")


def run(args: argparse.Namespace) -> None:
    default_color = normalize_color(args.color)
    uuid_color_table = load_uuid_color_table(args)
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        print(f"Listening on {args.port} at {args.baud} baud")
        if uuid_color_table:
            print(f"Loaded {len(uuid_color_table)} UUID color mapping(s)")
        if args.reset_wait > 0:
            time.sleep(args.reset_wait)
        if args.ping:
            check_health(ser, args.ping_timeout, args.print_all)
        while True:
            raw_line = ser.readline()
            if not raw_line:
                continue

            line = raw_line.decode("utf-8", errors="replace").strip()
            if args.print_all:
                print(line)

            match = UUID_RE.search(line)
            if not match:
                continue

            uuid_text = match.group("uuid")
            color = uuid_color_table.get(uuid_text, default_color)
            command = f"color {color}\n"
            print(f"UUID_REQUEST node={match.group('node')} uuid={uuid_text} -> #{color}")
            ser.write(command.encode("ascii"))
            ser.flush()

            if args.once:
                return


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Root serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--color", default="00ff00", help="Fallback response color #RRGGBB or RRGGBB")
    parser.add_argument("--map", action="append", default=[], help="UUID-to-color entry: uuid:RRGGBB. Can be repeated")
    parser.add_argument("--table-file", help="JSON object with UUID-to-color mappings")
    parser.add_argument("--once", action="store_true", help="Exit after first UUID_REQUEST")
    parser.add_argument("--print-all", action="store_true", help="Print all root serial lines")
    parser.add_argument("--reset-wait", type=float, default=0.0, help="Delay after opening serial port")
    parser.add_argument("--ping", action="store_true", help="Send PING and wait for PONG before listening")
    parser.add_argument("--ping-timeout", type=float, default=2.0, help="Seconds to wait for PONG")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        run(args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
