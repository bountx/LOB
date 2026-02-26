# High-Performance Limit Order Book (LOB) Engine

## Overview
This project is a high-throughput, low-latency Limit Order Book (LOB) built entirely in modern C++20. It connects directly to live cryptocurrency exchange WebSocket firehoses (starting with Binance) to ingest, parse, and maintain a real-time unified order book. 

The primary goal of this project is to explore **Mechanical Sympathy**—optimizing software to utilize underlying hardware efficiently through asynchronous I/O, cache-friendly data structures, and zero-allocation parsing.

## The "Why"
Rather than relying on heavy frameworks, this engine is built from the ground up to handle tens of thousands of messages per second on heavily constrained hardware. The final iteration will analyze the order book for microsecond-level arbitrage opportunities and "whale" trade alerts, broadcasting actionable intelligence via webhooks.

## Tech Stack
* **Language:** C++20
* **Build System & Package Manager:** CMake + vcpkg (Manifest Mode)
* **Networking:** Boost.Asio & Boost.Beast (Asynchronous, event-driven I/O)
* **Data Parsing:** `simdjson` (Zero-copy, SIMD-accelerated JSON parsing)

## Development Roadmap & Milestones

* **Phase 1: The Network Firehose (Current Focus)**
  * [x] Establish CMake and vcpkg build pipeline.
  * [ ] Implement Boost.Asio proactor event loop.
  * [ ] Establish secure TLS WebSocket connection to Binance.
  * [ ] Asynchronously ingest live order stream without blocking.

* **Phase 2: The Parser & The Book**
  * [ ] Integrate `simdjson` for zero-allocation payload extraction.
  * [ ] Design cache-friendly, contiguous memory data structures for the Order Book (avoiding `std::map` overhead).
  * [ ] Implement microsecond-level order insertions, modifications, and deletions.

* **Phase 3: Intelligence & Alerts**
  * [ ] Implement algorithmic checks for cross-exchange arbitrage.
  * [ ] Build a "Whale Tracker" to detect massive limit orders.
  * [ ] Integrate a lightweight HTTP client to push alerts to Discord/Telegram.

* **Phase 4: Profiling & Optimization**
  * [ ] Run the system on isolated hardware.
  * [ ] Profile CPU cache misses (using `perf`/Valgrind) and latency spikes.
  * [ ] Optimize memory alignment and thread pinning.

## Building the Project
This project uses `vcpkg` in manifest mode.
```bash
git clone <your-repo-url>
cd lob_project
export VCPKG_MAX_CONCURRENCY=1 # Optional: For memory-constrained environments
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build
./build/lob_app
