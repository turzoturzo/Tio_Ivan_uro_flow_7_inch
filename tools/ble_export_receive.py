#!/usr/bin/env python3
"""
BLE receiver for UroFlow "Export All" mode.

Protocol from firmware (TX notify characteristic):
  0x01 + name_len(2 LE) + file_size(4 LE) + file_name bytes
  0x02 + file chunk bytes
  0x03
  0x04 + file_count(2 LE)
  0x7E + status text
  0x7F + utf-8 error text
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "7E400001-B5A3-F393-E0A9-E50E24DCCA9E"
TX_UUID = "7E400002-B5A3-F393-E0A9-E50E24DCCA9E"
RX_UUID = "7E400003-B5A3-F393-E0A9-E50E24DCCA9E"


@dataclass
class ActiveFile:
    name: str
    expected_size: int
    path: Path
    written: int = 0


class ExportReceiver:
    def __init__(self, out_dir: Path) -> None:
        self.out_dir = out_dir
        self.current: Optional[ActiveFile] = None
        self.handle = None
        self.done = asyncio.Event()
        self.error: Optional[str] = None
        self.completed_files = 0

    def _safe_unique_path(self, name: str) -> Path:
        cleaned = name.lstrip("/").replace("\\", "_")
        candidate = self.out_dir / cleaned
        if not candidate.exists():
            return candidate

        stem = candidate.stem
        suffix = candidate.suffix
        i = 1
        while True:
            alt = candidate.with_name(f"{stem}_{i}{suffix}")
            if not alt.exists():
                return alt
            i += 1

    def on_notify(self, _: int, data: bytearray) -> None:
        if not data:
            return
        pkt_type = data[0]
        payload = bytes(data[1:])

        if pkt_type == 0x01:
            if len(payload) < 6:
                self.error = "bad file header packet"
                self.done.set()
                return
            name_len = struct.unpack_from("<H", payload, 0)[0]
            file_size = struct.unpack_from("<I", payload, 2)[0]
            if len(payload) < 6 + name_len:
                self.error = "truncated file header"
                self.done.set()
                return
            name = payload[6 : 6 + name_len].decode("utf-8", errors="replace")
            path = self._safe_unique_path(name)
            path.parent.mkdir(parents=True, exist_ok=True)
            self.handle = open(path, "wb")
            self.current = ActiveFile(name=name, expected_size=file_size, path=path)
            print(f"START {name} ({file_size} bytes) -> {path}")
            return

        if pkt_type == 0x02:
            if not self.current or not self.handle:
                self.error = "received data chunk with no active file"
                self.done.set()
                return
            self.handle.write(payload)
            self.current.written += len(payload)
            return

        if pkt_type == 0x03:
            if self.handle:
                self.handle.close()
                self.handle = None
            if self.current:
                print(
                    f"END   {self.current.name} "
                    f"({self.current.written}/{self.current.expected_size} bytes)"
                )
                self.completed_files += 1
            self.current = None
            return

        if pkt_type == 0x04:
            reported = 0
            if len(payload) >= 2:
                reported = struct.unpack_from("<H", payload, 0)[0]
            print(f"DONE  exported={reported}, saved={self.completed_files}")
            self.done.set()
            return

        if pkt_type == 0x7F:
            msg = payload.decode("utf-8", errors="replace")
            self.error = f"device error: {msg}"
            print(f"ERROR {msg}")
            self.done.set()
            return

        if pkt_type == 0x7E:
            msg = payload.decode("utf-8", errors="replace")
            print(f"INFO  {msg}")
            return

        print(f"WARN  unknown packet type 0x{pkt_type:02X}, len={len(payload)}")

    def close(self) -> None:
        if self.handle:
            self.handle.close()
            self.handle = None


async def find_device(name: str, timeout: float) -> str:
    print(f"Scanning for BLE device name '{name}' ...")
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        if d.name == name:
            print(f"Found {name}: {d.address}")
            return d.address
    raise RuntimeError(f"Could not find BLE device named '{name}'")


async def run(args: argparse.Namespace) -> int:
    out_dir = Path(args.out).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    address = args.address or await find_device(args.name, args.scan_timeout)

    receiver = ExportReceiver(out_dir=out_dir)
    try:
        async with BleakClient(address, timeout=args.connect_timeout) as client:
            print(f"Connected: {address}")

            await client.start_notify(TX_UUID, receiver.on_notify)
            print("Subscribed to TX notifications.")
            if args.wifi_ssid is not None:
                wifi_cmd = f"WIFI:{args.wifi_ssid}|{args.wifi_pass or ''}".encode("utf-8")
                await client.write_gatt_char(RX_UUID, wifi_cmd, response=True)
                print(f"Sent WiFi credentials for SSID '{args.wifi_ssid}'.")
                await asyncio.sleep(1.0)
            if args.start_cmd:
                await client.write_gatt_char(RX_UUID, b"START", response=True)
                print("Sent START command.")
            else:
                print("Waiting for transfer. Tap ESP32 screen to start export.")

            try:
                await asyncio.wait_for(receiver.done.wait(), timeout=args.transfer_timeout)
            except asyncio.TimeoutError:
                print("Timed out waiting for export completion.")
                return 2
            finally:
                await client.stop_notify(TX_UUID)
    finally:
        receiver.close()

    if receiver.error:
        print(receiver.error)
        return 1

    print(f"Saved files to: {out_dir}")
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Receive all CSV files from UroFlow BLE export mode.")
    p.add_argument("--name", default="Logger", help="BLE peripheral name (default: Logger)")
    p.add_argument("--address", default="", help="BLE address/UUID; skips scanning when provided")
    p.add_argument("--out", default="exports", help="Output directory for received files")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="Seconds to scan for device name")
    p.add_argument("--connect-timeout", type=float, default=15.0, help="BLE connect timeout seconds")
    p.add_argument("--transfer-timeout", type=float, default=300.0, help="Seconds to wait for transfer")
    p.add_argument(
        "--start-cmd",
        action="store_true",
        help="Send START command over RX char (not needed if using tap-to-send)",
    )
    p.add_argument("--wifi-ssid", default=None, help="Provision WiFi SSID over BLE before export")
    p.add_argument("--wifi-pass", default="", help="Provision WiFi password over BLE")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("Cancelled.")
        return 130
    except Exception as exc:
        print(f"Fatal: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
