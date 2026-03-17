#!/usr/bin/env python3
"""
Replay a LOB recording as a drop-in WebSocket server.

Point ofi-trader at this instead of the real LOB (change lob_ws_url in config):
    lob_ws_url: ws://localhost:8766

Usage:
    # max speed — replay 24h of data in ~10 seconds
    python3 replayer.py data/btc-20260317.ndjson.gz

    # 10x realtime
    python3 replayer.py data/btc-20260317.ndjson.gz --speed 10

    # custom port
    python3 replayer.py data/btc.ndjson.gz --port 8767 --speed 0

Requires:
    pip install websockets
"""
import argparse
import asyncio
import gzip
import json
from pathlib import Path
from typing import List, Tuple

import websockets


def load_messages(path: Path) -> List[Tuple[float, str]]:
    """Load all recorded messages into memory. Returns list of (ts, raw_json)."""
    messages = []
    open_fn = gzip.open if path.suffix == ".gz" else open
    with open_fn(path, "rt", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                ts = json.loads(line).get("ts", 0.0)
                messages.append((float(ts), line))
            except (json.JSONDecodeError, TypeError):
                continue
    return messages


async def handle_client(
    ws: websockets.WebSocketServerProtocol,
    messages: List[Tuple[float, str]],
    speed: float,
) -> None:
    addr = ws.remote_address
    print(f"Client connected: {addr}", flush=True)

    # Consume the subscribe message — we don't validate it, just start streaming.
    try:
        sub = await ws.recv()
        print(f"  subscribe: {sub[:80]}", flush=True)
    except websockets.exceptions.ConnectionClosed:
        return

    loop = asyncio.get_event_loop()
    start_wall = loop.time()
    first_ts = messages[0][0] if messages else 0.0

    for i, (ts, raw) in enumerate(messages):
        if speed > 0 and i > 0:
            # Sleep just enough so replay wall-clock time matches recording / speed.
            want_elapsed = (ts - first_ts) / speed
            elapsed = loop.time() - start_wall
            if want_elapsed > elapsed:
                await asyncio.sleep(want_elapsed - elapsed)

        try:
            await ws.send(raw)
        except websockets.exceptions.ConnectionClosed:
            print(f"  {addr} disconnected after {i} messages", flush=True)
            return

    print(f"  Replay complete: {len(messages)} messages sent to {addr}", flush=True)


async def serve(messages: List[Tuple[float, str]], port: int, speed: float) -> None:
    speed_label = "max speed" if speed == 0 else f"{speed}x realtime"
    print(f"Replayer ready on ws://localhost:{port} — {speed_label}", flush=True)
    print(f"Set lob_ws_url: ws://localhost:{port} in ofi-trader config\n", flush=True)

    async with websockets.serve(
        lambda ws: handle_client(ws, messages, speed),
        "0.0.0.0",
        port,
        max_size=None,
    ):
        await asyncio.Future()  # run until Ctrl+C


def main() -> None:
    parser = argparse.ArgumentParser(description="Replay a LOB recording as a WebSocket server")
    parser.add_argument("input", help="Recording file (.ndjson or .ndjson.gz)")
    parser.add_argument("--port", type=int, default=8766, help="Listen port (default 8766)")
    parser.add_argument(
        "--speed",
        type=float,
        default=0,
        help="Replay speed multiplier: 0=max, 1=realtime, 10=10x (default 0)",
    )
    args = parser.parse_args()

    path = Path(args.input)
    if not path.exists():
        print(f"Error: file not found: {path}")
        raise SystemExit(1)

    print(f"Loading {path}...", end=" ", flush=True)
    messages = load_messages(path)
    print(f"{len(messages)} messages loaded")

    try:
        asyncio.run(serve(messages, args.port, args.speed))
    except KeyboardInterrupt:
        print("\nStopped")


if __name__ == "__main__":
    main()
