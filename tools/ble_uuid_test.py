#!/usr/bin/env python3
"""Write a team UUID and attack-points count to an SS Forest node over BLE GATT."""

import argparse
import asyncio
import sys
import uuid

from bleak import BleakClient, BleakScanner


DEFAULT_DEVICE_NAME = "SS-FOREST-NODE"
SERVICE_UUID = "01008f7a-8e13-6e9b-8348-6df4029a6c70"
CHAR_UUID = "02008f7a-8e13-6e9b-8348-6df4029a6c70"


async def find_device(args: argparse.Namespace):
    if args.address:
        return args.address

    print(f"Scanning for BLE device named {args.name!r}...")
    devices = await BleakScanner.discover(timeout=args.scan_timeout)
    for device in devices:
        if device.name == args.name:
            print(f"Found {device.name} at {device.address}")
            return device.address

    raise RuntimeError(f"BLE device {args.name!r} not found")


async def main_async(args: argparse.Namespace) -> None:
    node_uuid = args.uuid or str(uuid.uuid4())
    team_uuid = uuid.UUID(node_uuid)
    payload = team_uuid.bytes + args.attack_points.to_bytes(2, byteorder="little")

    address = await find_device(args)
    print(f"Connecting to {address}...")
    async with BleakClient(address, timeout=args.connect_timeout) as client:
        if not client.is_connected:
            raise RuntimeError("BLE connection failed")
        await client.write_gatt_char(args.char_uuid, payload, response=True)
        print(f"Wrote UUID and attack points to node: {team_uuid} points={args.attack_points}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uuid", help="Team UUID to send. Default: generated uuid4")
    parser.add_argument("--attack-points", type=int, required=True, choices=range(0, 65536),
                        metavar="0..65535", help="Unsigned 16-bit attack-points count")
    parser.add_argument("--address", help="BLE MAC/address. If omitted, scan by --name")
    parser.add_argument("--name", default=DEFAULT_DEVICE_NAME, help=f"BLE device name, default {DEFAULT_DEVICE_NAME}")
    parser.add_argument("--char-uuid", default=CHAR_UUID, help=f"Writable characteristic UUID, default {CHAR_UUID}")
    parser.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout seconds")
    parser.add_argument("--connect-timeout", type=float, default=15.0, help="BLE connect timeout seconds")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(main_async(args))
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
