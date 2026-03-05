# LOB — Multi-Exchange Order Book Server

[![Tests](https://github.com/bountx/LOB/actions/workflows/test.yml/badge.svg)](https://github.com/bountx/LOB/actions/workflows/test.yml)
[![codecov](https://codecov.io/gh/bountx/LOB/graph/badge.svg?token=DHZNG5VRX6)](https://codecov.io/gh/bountx/LOB)

A self-hosted WebSocket server that connects to multiple crypto exchanges, maintains live limit order books, and re-broadcasts normalized data to any number of subscribers. Run it on a $6 VPS and point your trading scripts, dashboards, or ML pipelines at it.

## The problem it solves

Professional market data (Kaiko, Tardis, CoinAPI) costs hundreds per month. Free exchange APIs have aggressive rate limits — Binance allows one snapshot every ~10 seconds per symbol before you get 429'd, and each connection has stream limits. When you need 50+ symbols across multiple exchanges, you either pay or you build something.

This server handles the exchange complexity once, centrally, and lets any number of clients subscribe to clean normalized feeds — no exchange credentials, no rate limit exposure, no per-client reconnect logic.

## How it works

```
┌─────────────────────────────────────────────────────────┐
│                    Exchange Adapters                     │
│  Binance ──┐                                            │
│  Kraken  ──┼──→ Symbol normalizer → Unified book store  │
│  Coinbase──┘         BTC-USDT           BTC-USDT        │
│                      ETH-USDT           ETH-USDT        │
└─────────────────────────────┬───────────────────────────┘
                              │ fan-out (non-blocking)
              ┌───────────────┴────────────────┐
              ↓                                ↓
        subscriber A                     subscriber B
   (trading script)                  (Grafana/dashboard)
```

Each exchange runs its own adapter: independent WebSocket connection, snapshot/resync logic, and rate limit handling. Updates flow into the unified book store and are fanned out to all active subscribers without blocking ingestion.

## Subscription API

Connect to `ws://your-host:8765` and send:

```json
{"op": "subscribe", "streams": ["binance.BTCUSDT.book", "kraken.XBTUSDT.book"]}
```

Receive on subscribe — a full snapshot:
```json
{
  "type": "snapshot",
  "exchange": "binance",
  "symbol": "BTC-USDT",
  "ts": 1712000000000,
  "bids": [["94500.00", "1.230"], ["94499.50", "0.800"]],
  "asks": [["94501.00", "0.540"], ["94501.50", "2.100"]]
}
```

Then incremental updates:
```json
{
  "type": "update",
  "exchange": "binance",
  "symbol": "BTC-USDT",
  "ts": 1712000000123,
  "bids": [["94500.00", "0.000"]],
  "asks": [["94502.00", "1.800"]]
}
```

Zero quantity means the level was removed. Same format regardless of which exchange the data came from.

Subscribe to the unified stream to get best prices aggregated across all exchanges tracking a symbol:

```json
{"op": "subscribe", "streams": ["unified.BTC-USDT.book"]}
```

## Why C++, why not Cryptofeed

[Cryptofeed](https://github.com/bmoscon/cryptofeed) solves the ingestion problem well in Python. What it doesn't do is serve that data to subscribers. Hooking up 20 client scripts to 20 separate Cryptofeed processes is messy; running a single server that they all connect to is better.

The C++ matters for the serving layer specifically. Fan-out to N subscribers while maintaining ingestion from M exchanges without either side blocking the other is a concurrency problem. Python's GIL and async model make this awkward at scale. Here it's a few std::threads and a lock-free queue, using ~40MB RAM for 5 exchanges and 50 symbols where a Python equivalent would use 400MB+.

## Realistic scale (on a 4GB VPS)

| Exchanges | Symbols each | Msg/s in | Subscribers | RAM |
|-----------|-------------|----------|-------------|-----|
| 1         | 50          | ~500     | 20          | ~60MB |
| 3         | 30 each     | ~900     | 50          | ~120MB |
| 5         | 20 each     | ~1000    | 100         | ~180MB |

Binance rate-limits REST snapshots (depth 1000 = 50 weight, limit 6000/min). Spreading symbols across multiple exchanges distributes this pressure naturally — Binance, Kraken, and Coinbase each handle their own rate limits independently.

## Current status

The Binance ingestion layer and order book core are production-ready. The subscriber server and multi-exchange adapter interface are the next pieces.

```
[DONE]  Binance WebSocket adapter (multi-symbol, snapshot + resync)
[DONE]  Order book core — apply diffs, maintain sorted levels
[DONE]  Prometheus metrics + Grafana dashboard
[DONE]  Docker Compose deployment

[NEXT]  IExchangeAdapter interface — common contract for all exchanges
[NEXT]  Subscriber WebSocket server — fan-out with per-client backpressure
[NEXT]  Symbol normalization — canonical names across exchanges
[ ]     Kraken adapter
[ ]     Coinbase adapter
[ ]     Unified book (aggregated best prices across exchanges)
[ ]     Parquet recording — write book snapshots to disk for offline analysis
[ ]     Python client library — subscribe and get Pandas DataFrames
[ ]     OKX, Bybit adapters
```

## Quickstart

```bash
git clone https://github.com/yourname/lob
cd lob
cp .env.example .env
# Edit .env: set SYMBOLS, add exchange API keys if needed
docker compose up -d
```

Subscribe from Python:
```python
import websockets, asyncio, json

async def main():
    async with websockets.connect("ws://your-vps:8765") as ws:
        await ws.send(json.dumps({
            "op": "subscribe",
            "streams": ["binance.BTCUSDT.book", "kraken.XBTUSDT.book"]
        }))
        async for msg in ws:
            print(json.loads(msg))

asyncio.run(main())
```

## Configuration

| Variable | Default | Description |
|---|---|---|
| `BINANCE_SYMBOLS` | `BTCUSDT,ETHUSDT` | Symbols to track on Binance |
| `KRAKEN_SYMBOLS` | `` | Symbols to track on Kraken (when adapter is ready) |
| `SNAPSHOT_DEPTH` | `1000` | Order book depth per side |
| `SUBSCRIBER_PORT` | `8765` | WebSocket server port for subscribers |
| `METRICS_PORT` | `9090` | Prometheus metrics port |

## Build locally

Requires vcpkg at `~/vcpkg`.

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build
./build/lob_app
```

## Grafana dashboard

Tracks per-exchange, per-symbol: message rate, processing latency, best bid/ask, spread, and book depth. Auto-provisioned on `docker compose up`.

Grafana: `http://your-vps:3000` (admin/admin on first login)
