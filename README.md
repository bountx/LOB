# LOB — Multi-Exchange Order Book Server

[![Tests](https://github.com/bountx/LOB/actions/workflows/test.yml/badge.svg)](https://github.com/bountx/LOB/actions/workflows/test.yml)
[![Coverage Status](https://coveralls.io/repos/github/bountx/LOB/badge.svg?branch=main&kill_cache=1)](https://coveralls.io/github/bountx/LOB?branch=main&kill_cachee=2)

Connects to Binance and Kraken, maintains live order books, and fans out normalized updates to subscribers over WebSocket.

## Quickstart

```bash
# edit config.json, then:
docker compose up -d
```

Subscribe:
```python
import websockets, asyncio, json

async def main():
    async with websockets.connect("ws://localhost:8765") as ws:
        await ws.send(json.dumps({
            "op": "subscribe",
            "streams": ["binance.BTC-USDT.book", "kraken.BTC-USDT.book"]
        }))
        async for msg in ws:
            print(json.loads(msg))

asyncio.run(main())
```

## Subscription protocol

Stream format: `{exchange}.{CANONICAL-SYMBOL}.book`

On subscribe, you get a full snapshot:
```json
{"type":"snapshot","exchange":"binance","symbol":"BTC-USDT","ts":1712000000000,
 "bids":[["94500.00000000","1.23000000"]],"asks":[["94501.00000000","0.54000000"]]}
```

Then incremental updates. Quantity `"0"` means the level was removed:
```json
{"type":"update","exchange":"binance","symbol":"BTC-USDT","ts":1712000000123,
 "bids":[["94500.00000000","0"]],"asks":[["94502.00000000","1.80000000"]]}
```

Unsubscribe:
```json
{"op": "unsubscribe", "streams": ["binance.BTC-USDT.book"]}
```

## Configuration

`config.json` in the working directory (or pass path as first argument):

```json
{
  "exchanges": [
    {
      "name": "binance",
      "update_interval_ms": 100,
      "symbols": ["BTC-USDT", "ETH-USDT"]
    },
    {
      "name": "kraken",
      "symbols": ["BTC-USDT", "ETH-USDT"]
    }
  ],
  "snapshot_depth": 1000,
  "primary_symbol": "BTC-USDT"
}
```

| Field | Required | Default | Notes |
|---|---|---|---|
| `exchanges[].name` | Yes | — | `"binance"` or `"kraken"` |
| `exchanges[].symbols` | Yes | — | Canonical `BASE-QUOTE` symbols |
| `exchanges[].update_interval_ms` | Binance only | `100` | `100` or `1000` |
| `snapshot_depth` | No | `1000` | Depth per side. Kraken clamps to 10/25/100/500/1000. |
| `primary_symbol` | Yes | — | Must appear in at least one exchange |

## Exchange specifications

### Binance

- **WebSocket:** `wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms`
- **Snapshot:** REST `GET /api/v3/depth?symbol=BTCUSDT&limit=1000` (50 weight; limit 6000/min)
- **Book model:** Unmanaged. The adapter fetches a REST snapshot then applies a continuous diff stream. The book starts at `snapshot_depth` levels and grows over time (typically 3k-10k+ per side) as the price range widens.
- **Sequence checking:** Yes (`U`/`u` fields). On gap: clears book, re-fetches REST snapshot, replays buffered messages.
- **Rate limits:** 429 waits `Retry-After`; 418 (IP ban) triggers a longer wait. Both handled automatically.
- **Symbol format:** `BTCUSDT` <-> `BTC-USDT` (strips/inserts `-`). Supported quote suffixes: `USDT BUSD USDC USD BTC ETH BNB EUR`.

### Kraken

- **WebSocket:** `wss://ws.kraken.com/v2`
- **Snapshot:** Delivered via WebSocket after subscribe (no REST call).
- **Book model:** Managed. Kraken maintains exactly `depth` levels server-side. When a level is consumed or cancelled, Kraken sends a replacement from deeper in the book (a backfill). Depth stays flat at the subscribed value.
- **Sequence checking:** None — Kraken guarantees ordering on the connection.
- **Supported depths:** 10, 25, 100, 500, 1000 (requested depth is clamped down to nearest).
- **Backfill events:** When a top-of-book level is consumed, the same update message contains the removal and a new level at the outer window boundary. That new level is a real resting order that was already in the full book, just previously outside the visible window.
- **Symbol format:** `BTC/USDT` <-> `BTC-USDT` (swaps `/`/`-`). Kraken's `XBT` ticker maps to canonical `BTC`.

### Binance vs Kraken

| | Binance | Kraken |
|---|---|---|
| Book size over time | Grows (3k-10k+ levels) | Fixed at subscribed depth |
| All updates are new order arrivals? | Yes | No — outer-boundary additions are backfills |
| Resync needed? | Yes, on sequence gap | No |

## Prometheus metrics

Available at `http://your-host:9090/metrics`. All labeled by `exchange` and `symbol`.

| Metric | Type | Description |
|---|---|---|
| `lob_messages_total` | counter | WebSocket messages processed |
| `lob_event_lag_milliseconds` | gauge | Exchange timestamp to local receipt |
| `lob_processing_time_microseconds` | gauge | Last update processing time |
| `lob_max_processing_time_microseconds` | gauge | Peak processing time |
| `lob_orderbook_bids_count` | gauge | Bid levels in book |
| `lob_orderbook_asks_count` | gauge | Ask levels in book |
| `lob_orderbook_best_bid_price` | gauge | Best bid |
| `lob_orderbook_best_ask_price` | gauge | Best ask |
| `lob_orderbook_spread_price` | gauge | Spread (emitted only when book is populated) |

`GET /health` returns `200 OK`.

Grafana dashboard auto-provisioned at `http://your-host:3000` (admin/admin).

## Build locally

Requires [vcpkg](https://github.com/microsoft/vcpkg) at `~/vcpkg`.

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build
./build/lob_app config.json
```

Dependencies (`vcpkg.json`): `ixwebsocket`, `nlohmann-json`, `cpp-httplib`.

## Status

- [x] Binance and Kraken adapters
- [x] Subscriber WebSocket server
- [x] Prometheus metrics + Grafana dashboard
- [x] Docker Compose deployment
- [ ] OFI delta layer — genuine vs backfill event classification per level
- [ ] Data recording — write order book update stream to Parquet for offline use
- [ ] Data replay — re-emit recorded updates for strategy backtesting over a fixed time window
- [ ] Python client library
- [ ] Unified cross-exchange book
- [ ] Additional exchanges
