# ── Build stage ──────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake git curl zip unzip tar \
    g++ pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Pin vcpkg to the same commit used as builtin-baseline in vcpkg.json
# so the package versions are guaranteed to match.
RUN git clone https://github.com/microsoft/vcpkg /vcpkg \
    && git -C /vcpkg checkout ffc071e0c08432c60c9b64f00334c0227667931b \
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
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 zlib1g curl \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --system lob && useradd --system --gid lob --no-create-home lob

COPY --from=builder /build/build/lob_app /usr/local/bin/lob_app
RUN chown lob:lob /usr/local/bin/lob_app

USER lob

EXPOSE 9090

CMD ["lob_app"]
