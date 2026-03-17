#!/usr/bin/env python3
"""
Record a LOB WebSocket stream to a gzipped NDJSON file.
Each line is one raw JSON frame from the LOB service.

Usage:
    python3 recorder.py binance.BTC-USDT.book data/btc-20260317.ndjson.gz
    python3 recorder.py binance.BTC-USDT.book data/btc.ndjson.gz ws://localhost:8765

Requires:
    pip install websockets
"""
import asyncio
import gzip
import json
import signal
import sys
from pathlib import Path

import websockets


async def record(url: str, stream: str, output: Path) -> int:
    count = 0
    with gzip.open(output, "wt", encoding="utf-8") as f:
        async with websockets.connect(url, max_size=None) as ws:
            await ws.send(json.dumps({"op": "subscribe", "streams": [stream]}))
            print(f"Recording {stream} → {output}", flush=True)
            async for msg in ws:
                f.write(msg + "\n")
                count += 1
                if count % 5000 == 0:
                    f.flush()
                    print(f"  {count} messages", flush=True)
    return count


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: recorder.py <stream> <output.ndjson.gz> [ws://localhost:8765]")
        sys.exit(1)

    stream = sys.argv[1]
    output = Path(sys.argv[2])
    url = sys.argv[3] if len(sys.argv) > 3 else "ws://localhost:8765"

    output.parent.mkdir(parents=True, exist_ok=True)

    loop = asyncio.new_event_loop()
    task = loop.create_task(record(url, stream, output))

    def stop(sig, frame):
        task.cancel()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    try:
        count = loop.run_until_complete(task)
        print(f"Stopped: recorded {count} messages → {output}")
    except asyncio.CancelledError:
        print(f"\nStopped")
    finally:
        loop.close()


if __name__ == "__main__":
    main()
