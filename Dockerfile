# ── Build stage ──────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    cmake git curl zip unzip tar \
    g++ pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg
RUN git clone https://github.com/microsoft/vcpkg /vcpkg \
    && /vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/vcpkg

WORKDIR /build

# Copy manifest + CMakeLists first so vcpkg deps are cached in a separate layer.
# This layer is only rebuilt when vcpkg.json or CMakeLists.txt changes.
COPY vcpkg.json CMakeLists.txt ./

RUN cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release

# Now copy source and compile only the app binary
COPY src/ ./src/
RUN cmake --build build -j"$(nproc)"

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    ca-certificates libssl3 zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/lob_app /usr/local/bin/lob_app

EXPOSE 9090

CMD ["lob_app"]
